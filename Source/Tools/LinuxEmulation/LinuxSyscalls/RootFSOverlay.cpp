// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|common
desc: Per-user writable layer over the read-only base rootfs
$end_info$
*/

#include "LinuxSyscalls/RootFSOverlay.h"
#include "LinuxSyscalls/Syscalls.h"

#include "Common/FDUtils.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/set.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace FEX::HLE {
namespace {
  using namespace std::string_view_literals;

  // Everything the guest's package manager installs into or keeps state in.
  constexpr std::string_view OwnedPrefixes[] = {
    "/usr"sv, "/etc"sv, "/opt"sv, "/var/lib/pacman"sv, "/var/cache/pacman"sv, "/var/log/pacman.log"sv,
  };
  // Package-manager state: overlay, then base, never the host. A guest pacman
  // that fell through to /var/lib/pacman would read (and try to write) the
  // host's own package database.
  constexpr std::string_view NoFallthroughPrefixes[] = {
    "/var/lib/pacman"sv, "/var/cache/pacman"sv, "/etc/pacman.conf"sv, "/etc/pacman.d"sv, "/var/log/pacman.log"sv,
  };
  // Directories that only lead to an owned prefix. They are looked up in the
  // overlay so their owned children can be, but are never layered themselves.
  constexpr std::string_view OwnedAncestors[] = {
    "/var"sv,
    "/var/lib"sv,
    "/var/cache"sv,
    "/var/log"sv,
  };
  // First components that are, or lead to, an owned prefix.
  constexpr std::string_view OwnedRoots[] = {"usr"sv, "etc"sv, "opt"sv, "var"sv};

  constexpr std::string_view WhiteoutPrefix = ".wh."sv;
  constexpr std::string_view OpaqueMarker = ".wh..wh..opq"sv;
  constexpr int MaxSymlinks = 40;

  bool HasPrefix(std::string_view Path, std::string_view Prefix) {
    return Path.size() >= Prefix.size() && Path.compare(0, Prefix.size(), Prefix) == 0 &&
           (Path.size() == Prefix.size() || Path[Prefix.size()] == '/');
  }

  bool IsOwned(std::string_view Path) {
    return std::any_of(std::begin(OwnedPrefixes), std::end(OwnedPrefixes), [&](auto P) { return HasPrefix(Path, P); });
  }

  bool IsNoFallthrough(std::string_view Path) {
    return std::any_of(std::begin(NoFallthroughPrefixes), std::end(NoFallthroughPrefixes), [&](auto P) { return HasPrefix(Path, P); });
  }

  bool IsOwnedAncestor(std::string_view Path) {
    return std::any_of(std::begin(OwnedAncestors), std::end(OwnedAncestors), [&](auto P) { return Path == P; });
  }

  fextl::string DirName(const fextl::string& Path) {
    auto Slash = Path.rfind('/');
    if (Slash == fextl::string::npos || Slash == 0) {
      return "/";
    }
    return Path.substr(0, Slash);
  }

  std::string_view BaseName(const fextl::string& Path) {
    auto Slash = Path.rfind('/');
    return Slash == fextl::string::npos ? std::string_view {Path} : std::string_view {Path}.substr(Slash + 1);
  }

  fextl::string Join(const fextl::string& Dir, std::string_view Name) {
    fextl::string Out = Dir == "/" ? fextl::string {} : Dir;
    Out += '/';
    Out += Name;
    return Out;
  }

  fextl::string WhiteoutOf(const fextl::string& Path) {
    fextl::string Name {WhiteoutPrefix};
    Name += BaseName(Path);
    return Join(DirName(Path), Name);
  }

  fextl::string TrimSlashes(fextl::string Path) {
    while (Path.size() > 1 && Path.back() == '/') {
      Path.pop_back();
    }
    return Path;
  }

  bool IsDirectory(const fextl::string& Path) {
    struct stat St {};
    return !Path.empty() && ::stat(Path.c_str(), &St) == 0 && S_ISDIR(St.st_mode);
  }

  // The existing FileManager helpers, restated: open() is lenient about flag
  // combinations that openat2() rejects.
  uint64_t SanitizeFlags(uint64_t Flags) {
    if (Flags & O_PATH) {
      Flags &= (O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    }
    return Flags;
  }

  // Opens Rel scoped to the directory Root (RESOLVE_IN_ROOT), falling back to
  // openat() where openat2() refuses what open() accepts.
  int OpenIn(int Root, const char* Rel, int Flags, uint32_t Mode) {
    const uint64_t How = SanitizeFlags(static_cast<uint64_t>(Flags));
    const bool Creates = (How & O_CREAT) || (How & O_TMPFILE) == O_TMPFILE;
    open_how H = {
      .flags = How,
      .mode = Creates ? (Mode & 07777) : 0,
      .resolve = RESOLVE_IN_ROOT,
    };
    int FD = ::syscall(SYSCALL_DEF(openat2), Root, Rel, &H, sizeof(H));
    if (FD == -1 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS)) {
      FD = ::syscall(SYSCALL_DEF(openat), Root, Rel, static_cast<int>(How), Mode);
    }
    return FD;
  }

  // Reads every entry of an open directory descriptor.
  //
  // The buffers here and in CopyData are on the heap on purpose. These run in
  // syscall context, and while CopyData kept a 64 KiB buffer on the stack a
  // guest mv of a base file intermittently died of a corrupted guest stack
  // ("stack smashing detected").
  template<typename Fn>
  bool ForEachEntry(int DirFD, Fn&& Callback) {
    fextl::vector<uint64_t> Storage(16 * 1024 / sizeof(uint64_t));
    char* Buf = reinterpret_cast<char*>(Storage.data());
    for (;;) {
      long N = ::syscall(SYSCALL_DEF(getdents64), DirFD, Buf, Storage.size() * sizeof(uint64_t));
      if (N < 0) {
        return false;
      }
      if (N == 0) {
        return true;
      }
      for (long Off = 0; Off < N;) {
        struct Dirent {
          uint64_t d_ino;
          int64_t d_off;
          uint16_t d_reclen;
          uint8_t d_type;
          char d_name[];
        };
        auto* D = reinterpret_cast<Dirent*>(Buf + Off);
        Callback(D->d_ino, D->d_type, std::string_view {D->d_name});
        Off += D->d_reclen;
      }
    }
  }

  // Copies an open file's bytes to another.
  int CopyData(int Src, int Dst) {
    for (;;) {
      ssize_t N = ::copy_file_range(Src, nullptr, Dst, nullptr, 1 << 30, 0);
      if (N == 0) {
        return 0;
      }
      if (N > 0) {
        continue;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno != EXDEV && errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) {
        return errno;
      }
      break;
    }
    fextl::vector<char> Storage(64 * 1024);
    char* Buf = Storage.data();
    for (;;) {
      ssize_t N = ::read(Src, Buf, Storage.size());
      if (N == 0) {
        return 0;
      }
      if (N < 0) {
        if (errno == EINTR) {
          continue;
        }
        return errno;
      }
      for (ssize_t Off = 0; Off < N;) {
        ssize_t W = ::write(Dst, Buf + Off, N - Off);
        if (W < 0) {
          if (errno == EINTR) {
            continue;
          }
          return errno;
        }
        Off += W;
      }
    }
  }
} // namespace

RootFSOverlay::~RootFSOverlay() {
  if (UpperFD != -1) {
    ::close(UpperFD);
  }
}

fextl::string RootFSOverlay::ConfiguredPath(const fextl::string& RootFS) {
  FEX_CONFIG_OPT(Setting, ROOTFSOVERLAY);
  const fextl::string& Value = Setting();
  if (Value == "0" || Value == "off" || Value == "none") {
    return {};
  }

  const fextl::string BasePath = TrimSlashes(RootFS);
  fextl::string Path;
  if (!Value.empty()) {
    if (Value[0] != '/' || !IsDirectory(Value)) {
      LogMan::Msg::EFmt("RootFSOverlay '{}' is not an absolute path to a directory; the overlay is disabled", Value);
      return {};
    }
    Path = TrimSlashes(Value);
  } else {
    if (BasePath.empty() || BasePath == "/") {
      return {};
    }
    Path = BasePath + "-overlay";
    if (!IsDirectory(Path)) {
      return {};
    }
  }

  // The overlay exists to keep writes out of the base. One that is the base,
  // or lives inside it, would defeat that.
  if (!BasePath.empty() && (Path == BasePath || HasPrefix(Path, BasePath))) {
    LogMan::Msg::EFmt("RootFSOverlay '{}' is inside the rootfs '{}'; the overlay is disabled", Path, BasePath);
    return {};
  }
  return Path;
}

fextl::string RootFSOverlay::LoaderPath(const fextl::string& RootFS, const fextl::string& GuestPath) {
  if (GuestPath.empty() || GuestPath[0] != '/' || RootFS.empty()) {
    return {};
  }
  const auto Path = ConfiguredPath(RootFS);
  if (Path.empty()) {
    return {};
  }

  int Base = ::open(RootFS.c_str(), O_DIRECTORY | O_PATH | O_CLOEXEC);
  if (Base == -1) {
    return {};
  }
  RootFSOverlay Overlay;
  Overlay.Init(RootFS, Base);
  fextl::string Result;
  if (Overlay.Active()) {
    if (auto N = Overlay.Lookup(AT_FDCWD, GuestPath.c_str(), true); N && !N->Error) {
      if (N->Where == Layer::Upper) {
        Result = Overlay.Upper + N->Path;
      } else if (N->Where == Layer::Lower) {
        Result = Overlay.Base + N->Path;
      }
    }
  }
  ::close(Base);
  return Result;
}

void RootFSOverlay::Init(const fextl::string& RootFS, int BaseDirFD) {
  if (BaseDirFD < 0) {
    return;
  }
  const auto Path = ConfiguredPath(RootFS);
  if (Path.empty()) {
    return;
  }
  int FD = ::open(Path.c_str(), O_DIRECTORY | O_PATH | O_CLOEXEC);
  if (FD == -1) {
    return;
  }
  FD = FEX::MoveFDOutOfGuestRange(FD);
  struct stat St {};
  if (FD == -1 || ::fstat(FD, &St) != 0) {
    if (FD != -1) {
      ::close(FD);
    }
    return;
  }

  Base = TrimSlashes(RootFS);
  Upper = Path;
  BaseFD = BaseDirFD;
  UpperFD = FD;
  UpperDev = St.st_dev;

  for (auto Root : OwnedRoots) {
    Candidates.emplace_back(Root);
  }
  // Root-level symlinks into an owned tree (Arch's bin, sbin and lib -> usr/...).
  int Dir = ::openat(BaseFD, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (Dir != -1) {
    ForEachEntry(Dir, [&](uint64_t, uint8_t Type, std::string_view Name) {
      if (Type != DT_LNK) {
        return;
      }
      char Target[PATH_MAX];
      fextl::string N {Name};
      ssize_t Len = ::readlinkat(BaseFD, N.c_str(), Target, sizeof(Target) - 1);
      if (Len <= 0) {
        return;
      }
      std::string_view T {Target, static_cast<size_t>(Len)};
      while (!T.empty() && (T.front() == '/' || T.starts_with("./"))) {
        T.remove_prefix(T.front() == '/' ? 1 : 2);
      }
      auto First = T.substr(0, T.find('/'));
      if (std::find(std::begin(OwnedRoots), std::end(OwnedRoots), First) != std::end(OwnedRoots)) {
        Candidates.emplace_back(N);
      }
    });
    ::close(Dir);
  }
  LogMan::Msg::DFmt("RootFS overlay: {} over {}", Upper, Base);
}

uint64_t RootFSOverlay::Fail(int Error) const {
  errno = Error;
  return static_cast<uint64_t>(-1);
}

int RootFSOverlay::LayerFD(Layer L) const {
  return L == Layer::Upper ? UpperFD : BaseFD;
}

const char* RootFSOverlay::Rel(const fextl::string& Path) {
  return Path.size() <= 1 ? "." : Path.c_str() + 1;
}

fextl::string RootFSOverlay::HostPathOf(const Node& N) const {
  switch (N.Where) {
  case Layer::Upper: return Upper + N.Path;
  case Layer::Lower: return Base + N.Path;
  case Layer::Host: return N.Path;
  case Layer::Hidden: break;
  }
  // Nothing is ever created at this name: the prefix is reserved.
  return Upper + "/.wh..hidden" + N.Path;
}

fextl::string RootFSOverlay::StripUpperPrefix(std::string_view HostPath) const {
  if (!Active() || !HasPrefix(HostPath, Upper)) {
    return {};
  }
  fextl::string Guest {HostPath.substr(Upper.size())};
  return Guest.empty() ? fextl::string {"/"} : Guest;
}

bool RootFSOverlay::IsCandidate(std::string_view Path) const {
  size_t I = 0;
  while (I < Path.size() && Path[I] == '/') {
    ++I;
  }
  auto End = Path.find('/', I);
  auto First = Path.substr(I, End == std::string_view::npos ? std::string_view::npos : End - I);
  return std::any_of(Candidates.begin(), Candidates.end(), [&](const fextl::string& C) { return First == C; });
}

std::optional<fextl::string> RootFSOverlay::GuestPathOf(int DirFD, const char* Path) const {
  if (!Path || !Path[0]) {
    return std::nullopt;
  }
  if (Path[0] == '/') {
    if (!IsCandidate(Path)) {
      return std::nullopt;
    }
    return fextl::string {Path};
  }

  // Relative: find where the directory is in the guest's namespace. A
  // directory inside the overlay or the base maps back to its guest path; any
  // other host directory is already a guest path.
  char Buf[PATH_MAX];
  ssize_t Len = DirFD == AT_FDCWD ? ::readlink("/proc/self/cwd", Buf, sizeof(Buf)) : FEX::get_fdpath(DirFD, Buf);
  if (Len <= 0 || Len >= static_cast<ssize_t>(sizeof(Buf)) || Buf[0] != '/') {
    return std::nullopt;
  }
  std::string_view Dir {Buf, static_cast<size_t>(Len)};
  fextl::string Guest;
  if (HasPrefix(Dir, Upper)) {
    Guest = Dir.substr(Upper.size());
  } else if (HasPrefix(Dir, Base)) {
    Guest = Dir.substr(Base.size());
  } else {
    Guest = Dir;
  }
  if (Guest.empty() || Guest.back() != '/') {
    Guest += '/';
  }
  Guest += Path;
  if (!IsCandidate(Guest)) {
    return std::nullopt;
  }
  return Guest;
}

bool RootFSOverlay::Opaque(const fextl::string& Dir, DirState& S) const {
  if (S.OpaqueKnown) {
    return S.Opaque;
  }
  S.OpaqueKnown = true;
  S.Opaque = false;
  if (!S.Upper || !IsOwned(Dir)) {
    return false;
  }

  // The directory's inode and mtime were recorded when the walk entered it.
  // Creating or removing the marker changes the mtime, so a cached answer
  // cannot outlive it.
  {
    std::lock_guard lk(OpaqueMutex);
    auto It = OpaqueCache.find(S.Ino);
    if (It != OpaqueCache.end() && It->second.Sec == S.MtimeSec && It->second.Nsec == S.MtimeNsec) {
      S.Opaque = It->second.Opaque;
      return S.Opaque;
    }
  }
  struct stat M {};
  S.Opaque = ::fstatat(UpperFD, Rel(Join(Dir, OpaqueMarker)), &M, AT_SYMLINK_NOFOLLOW) == 0;
  {
    std::lock_guard lk(OpaqueMutex);
    if (OpaqueCache.size() > 16384) {
      OpaqueCache.clear();
    }
    OpaqueCache[S.Ino] = {S.MtimeSec, S.MtimeNsec, S.Opaque};
  }
  return S.Opaque;
}

RootFSOverlay::Node RootFSOverlay::Resolve(std::string_view In, bool FollowLast) const {
  // Walks the merged namespace one component at a time, the way the kernel
  // walks a path, so that a symlink in either layer (the base's /lib ->
  // usr/lib, say) leads into the other layer correctly. Each owned component
  // is looked up in the overlay, then its whiteout, then the base; the first
  // component found in neither stops the walk and the host resolves the rest.
  Node N;
  fextl::string Work {In};
  size_t Pos = 0;
  N.TrailingSlash = Work.size() > 1 && Work.back() == '/';
  if (N.TrailingSlash) {
    FollowLast = true;
  }

  struct Frame {
    size_t Len;
    DirState S;
    Layer Where;
    struct stat St;
  };
  fextl::vector<Frame> Frames;
  {
    DirState Root {};
    Root.Upper = true;
    Root.OpaqueKnown = true;
    Root.Lower = true;
    Root.Fallthrough = true;
    Frames.push_back({0, Root, Layer::Lower, {}});
  }
  fextl::string Cur;
  int Links = 0;

  for (;;) {
    while (Pos < Work.size() && Work[Pos] == '/') {
      ++Pos;
    }
    if (Pos >= Work.size()) {
      // Ended on a directory already entered: "/usr/", "/usr/.", "/usr/lib/..".
      auto& F = Frames.back();
      N.Path = Cur.empty() ? fextl::string {"/"} : Cur;
      N.Where = F.Where;
      N.St = F.St;
      N.Self = F.S;
      if (Frames.size() >= 2) {
        auto& P = Frames[Frames.size() - 2];
        Opaque(DirName(N.Path), P.S);
        N.Parent = P.S;
        N.HaveParent = true;
      }
      break;
    }
    size_t End = Work.find('/', Pos);
    if (End == fextl::string::npos) {
      End = Work.size();
    }
    const fextl::string C = Work.substr(Pos, End - Pos);
    Pos = End;
    size_t Next = Pos;
    while (Next < Work.size() && Work[Next] == '/') {
      ++Next;
    }
    const bool Last = Next >= Work.size();

    if (C == ".") {
      continue;
    }
    if (C == "..") {
      if (Frames.size() > 1) {
        Frames.pop_back();
        Cur.resize(Frames.back().Len);
      }
      continue;
    }
    if (C.size() > NAME_MAX) {
      N.Error = ENAMETOOLONG;
      N.Path = Cur + "/" + C;
      break;
    }

    DirState& S = Frames.back().S;
    const fextl::string Cand = Cur + "/" + C;
    const bool Owned = IsOwned(Cand);
    // /var, /var/lib, ...: only looked up in the overlay to reach owned
    // children; the directory itself is always the base's (or the host's).
    const bool Ancestor = !Owned && IsOwnedAncestor(Cand);
    struct stat St {};
    struct stat UpperAncestor {};
    bool HaveUpperAncestor = false;
    Layer Hit = Layer::Host;
    bool Found = false;
    int Err = 0;

    if (Owned && std::string_view {C}.starts_with(WhiteoutPrefix)) {
      // Reserved for whiteouts and copy-up temporaries.
      Hit = Layer::Hidden;
      Found = true;
    } else if (Owned && S.Upper) {
      if (::fstatat(UpperFD, Rel(Cand), &St, AT_SYMLINK_NOFOLLOW) == 0) {
        Hit = Layer::Upper;
        Found = true;
      } else if (errno != ENOENT && errno != ENOTDIR) {
        Err = errno;
      } else {
        struct stat W {};
        if (::fstatat(UpperFD, Rel(WhiteoutOf(Cand)), &W, AT_SYMLINK_NOFOLLOW) == 0) {
          Hit = Layer::Hidden;
          Found = true;
        }
      }
    } else if (Ancestor && S.Upper) {
      HaveUpperAncestor = ::fstatat(UpperFD, Rel(Cand), &UpperAncestor, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(UpperAncestor.st_mode);
    }

    bool ParentOpaque = false;
    if (!Found && !Err) {
      ParentOpaque = Opaque(Cur.empty() ? fextl::string {"/"} : Cur, S);
      if (S.Lower && !ParentOpaque) {
        if (::fstatat(BaseFD, Rel(Cand), &St, AT_SYMLINK_NOFOLLOW) == 0) {
          Hit = Layer::Lower;
          Found = true;
        } else if (errno != ENOENT && errno != ENOTDIR) {
          Err = errno;
        }
      }
    }
    if (!Found && !Err && HaveUpperAncestor) {
      // An ancestor directory only the overlay has.
      Hit = Layer::Upper;
      St = UpperAncestor;
      Found = true;
    }
    if (!Found && !Err) {
      Hit = (!S.Fallthrough || ParentOpaque || (Owned && IsNoFallthrough(Cand))) ? Layer::Hidden : Layer::Host;
    }
    if (Err) {
      N.Error = Err;
      N.Path = Cand;
      break;
    }

    if (Hit == Layer::Hidden || Hit == Layer::Host) {
      // Nothing in either layer. The host resolves whatever is left.
      N.Path = Cand;
      if (!Last) {
        N.Path += '/';
        N.Path += std::string_view {Work}.substr(Next);
        N.StoppedEarly = true;
      }
      N.Where = Hit;
      Opaque(Cur.empty() ? fextl::string {"/"} : Cur, S);
      N.Parent = S;
      N.HaveParent = true;
      break;
    }

    if (S_ISLNK(St.st_mode) && (!Last || FollowLast)) {
      if (++Links > MaxSymlinks) {
        N.Error = ELOOP;
        N.Path = Cand;
        break;
      }
      char Target[PATH_MAX];
      ssize_t Len = ::readlinkat(LayerFD(Hit), Rel(Cand), Target, sizeof(Target) - 1);
      if (Len <= 0) {
        N.Error = Len == 0 ? ENOENT : errno;
        N.Path = Cand;
        break;
      }
      if (Hit == Layer::Upper) {
        N.TouchedUpper = true;
      }
      fextl::string NewWork {Target, static_cast<size_t>(Len)};
      if (!Last) {
        NewWork += '/';
        NewWork += std::string_view {Work}.substr(Next);
      } else if (N.TrailingSlash) {
        NewWork += '/';
      }
      Work = std::move(NewWork);
      Pos = 0;
      if (Target[0] == '/') {
        Frames.resize(1);
        Cur.clear();
      }
      continue;
    }

    if (!Last && !S_ISDIR(St.st_mode)) {
      N.Error = ENOTDIR;
      N.Path = Cand;
      break;
    }

    DirState Child {};
    if (S_ISDIR(St.st_mode)) {
      const bool ParentHidesLower = S.Upper && Opaque(Cur.empty() ? fextl::string {"/"} : Cur, S);
      Child.Upper = Hit == Layer::Upper || HaveUpperAncestor;
      Child.OpaqueKnown = !Child.Upper || !Owned;
      Child.Lower = Hit == Layer::Lower || (S.Lower && !ParentHidesLower);
      Child.Fallthrough = S.Fallthrough && !ParentHidesLower && !(Owned && IsNoFallthrough(Cand));
      const struct stat& U = Hit == Layer::Upper ? St : UpperAncestor;
      Child.Ino = U.st_ino;
      Child.MtimeSec = U.st_mtim.tv_sec;
      Child.MtimeNsec = U.st_mtim.tv_nsec;
    }

    if (Last) {
      N.Path = Cand;
      N.Where = Hit;
      N.St = St;
      N.Self = Child;
      Opaque(Cur.empty() ? fextl::string {"/"} : Cur, S);
      N.Parent = S;
      N.HaveParent = true;
      break;
    }
    Cur = Cand;
    Frames.push_back({Cur.size(), Child, Hit, St});
  }

  if (N.Path.empty()) {
    N.Path = "/";
  }
  if (!N.Error && N.TrailingSlash && (N.Where == Layer::Upper || N.Where == Layer::Lower) && !S_ISDIR(N.St.st_mode)) {
    N.Error = ENOTDIR;
  }
  N.Owned = IsOwned(N.Path);
  return N;
}

std::optional<RootFSOverlay::Node> RootFSOverlay::Lookup(int DirFD, const char* Path, bool FollowLast) const {
  if (!Active()) {
    return std::nullopt;
  }
  auto Guest = GuestPathOf(DirFD, Path);
  if (!Guest) {
    return std::nullopt;
  }
  Node N = Resolve(*Guest, FollowLast);
  if (N.Path == "/" || (!N.Owned && !N.TouchedUpper)) {
    // Not under a guest-owned prefix and nothing in the overlay was involved:
    // the ordinary rootfs/host rules apply, unchanged.
    return std::nullopt;
  }
  return N;
}

fextl::string RootFSOverlay::TempName(const fextl::string& Dir) {
  const auto Seq = TempCounter.fetch_add(1, std::memory_order_relaxed);
  char Name[96];
  snprintf(Name, sizeof(Name), ".wh..cu.%d.%ld.%u", ::getpid(), ::syscall(SYS_gettid), Seq);
  return Join(Dir, Name);
}

int RootFSOverlay::EnsureUpperDir(const fextl::string& Dir) {
  if (Dir.empty() || Dir == "/") {
    return 0;
  }
  Node N = Resolve(Dir, true);
  if (N.Error) {
    return N.Error;
  }
  switch (N.Where) {
  case Layer::Hidden: return ENOENT;
  case Layer::Upper: return S_ISDIR(N.St.st_mode) ? 0 : ENOTDIR;
  case Layer::Lower:
    if (!S_ISDIR(N.St.st_mode)) {
      return ENOTDIR;
    }
    break;
  case Layer::Host: {
    struct stat H {};
    if (::stat(N.Path.c_str(), &H) != 0) {
      return errno;
    }
    if (!S_ISDIR(H.st_mode)) {
      return ENOTDIR;
    }
    break;
  }
  }

  // Create each missing directory of the chain, taking mode and owner from
  // the copy the guest sees (the base's, else the host's).
  fextl::string P;
  size_t Pos = 1;
  while (Pos <= N.Path.size()) {
    size_t End = N.Path.find('/', Pos);
    if (End == fextl::string::npos) {
      End = N.Path.size();
    }
    if (End > Pos) {
      P = N.Path.substr(0, End);
      struct stat U {};
      if (::fstatat(UpperFD, Rel(P), &U, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISDIR(U.st_mode)) {
          return ENOTDIR;
        }
      } else {
        struct stat L {};
        bool Have = ::fstatat(BaseFD, Rel(P), &L, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(L.st_mode);
        if (!Have) {
          Have = ::stat(P.c_str(), &L) == 0 && S_ISDIR(L.st_mode);
        }
        const mode_t Mode = Have ? (L.st_mode & 07777) : 0755;
        if (::mkdirat(UpperFD, Rel(P), 0700) != 0 && errno != EEXIST) {
          return errno;
        }
        if (Have) {
          [[maybe_unused]] int Ignored = ::fchownat(UpperFD, Rel(P), L.st_uid, L.st_gid, AT_SYMLINK_NOFOLLOW);
        }
        ::fchmodat(UpperFD, Rel(P), Mode, 0);
      }
    }
    Pos = End + 1;
  }
  return 0;
}

int RootFSOverlay::MakeWhiteout(const fextl::string& Path) {
  if (int Err = EnsureUpperDir(DirName(Path))) {
    return Err;
  }
  int FD = ::openat(UpperFD, Rel(WhiteoutOf(Path)), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (FD == -1) {
    return errno;
  }
  ::close(FD);
  return 0;
}

bool RootFSOverlay::RemoveWhiteout(const fextl::string& Path) {
  return ::unlinkat(UpperFD, Rel(WhiteoutOf(Path)), 0) == 0;
}

int RootFSOverlay::MakeOpaque(const fextl::string& Dir) {
  int FD = ::openat(UpperFD, Rel(Join(Dir, OpaqueMarker)), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (FD == -1) {
    return errno;
  }
  ::close(FD);
  return 0;
}

bool RootFSOverlay::LowerVisible(const fextl::string& Path, const DirState& Parent) const {
  if (Parent.Upper && Parent.Opaque) {
    return false;
  }
  struct stat St {};
  if (Parent.Lower && ::fstatat(BaseFD, Rel(Path), &St, AT_SYMLINK_NOFOLLOW) == 0) {
    return true;
  }
  return Parent.Fallthrough && !IsNoFallthrough(Path) && ::lstat(Path.c_str(), &St) == 0;
}

int RootFSOverlay::CopyObject(const Node& Src, const fextl::string& Dest, bool WithData) {
  struct stat St = Src.St;
  if (Src.Where == Layer::Host && ::lstat(Src.Path.c_str(), &St) != 0) {
    return errno;
  }
  const fextl::string Tmp = TempName(DirName(Dest));
  int Err = 0;

  if (S_ISREG(St.st_mode)) {
    int In = Src.Where == Layer::Host ? ::open(Src.Path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC) :
                                        OpenIn(LayerFD(Src.Where), Rel(Src.Path), O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0);
    if (In == -1) {
      return errno;
    }
    int Out = ::openat(UpperFD, Rel(Tmp), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (Out == -1) {
      Err = errno;
      ::close(In);
      return Err;
    }
    if (WithData) {
      Err = CopyData(In, Out);
    }
    if (!Err) {
      [[maybe_unused]] int Ignored = ::fchown(Out, St.st_uid, St.st_gid);
      ::fchmod(Out, St.st_mode & 07777);
      const struct timespec Times[2] = {St.st_atim, St.st_mtim};
      ::futimens(Out, Times);
    }
    ::close(Out);
    ::close(In);
  } else if (S_ISLNK(St.st_mode)) {
    char Target[PATH_MAX];
    ssize_t Len = Src.Where == Layer::Host ? ::readlink(Src.Path.c_str(), Target, sizeof(Target) - 1) :
                                             ::readlinkat(LayerFD(Src.Where), Rel(Src.Path), Target, sizeof(Target) - 1);
    if (Len < 0) {
      return errno;
    }
    Target[Len] = 0;
    if (::symlinkat(Target, UpperFD, Rel(Tmp)) != 0) {
      return errno;
    }
    [[maybe_unused]] int Ignored = ::fchownat(UpperFD, Rel(Tmp), St.st_uid, St.st_gid, AT_SYMLINK_NOFOLLOW);
    const struct timespec Times[2] = {St.st_atim, St.st_mtim};
    ::utimensat(UpperFD, Rel(Tmp), Times, AT_SYMLINK_NOFOLLOW);
  } else if (S_ISFIFO(St.st_mode)) {
    if (::mknodat(UpperFD, Rel(Tmp), S_IFIFO | (St.st_mode & 07777), 0) != 0) {
      return errno;
    }
  } else {
    // Devices and sockets are not copied up.
    return EPERM;
  }

  if (!Err && ::renameat(UpperFD, Rel(Tmp), UpperFD, Rel(Dest)) != 0) {
    Err = errno;
  }
  if (Err) {
    ::unlinkat(UpperFD, Rel(Tmp), 0);
  }
  return Err;
}

int RootFSOverlay::CopyUp(const Node& N, bool WithData) {
  if (N.Where == Layer::Upper) {
    return 0;
  }
  if (N.Where == Layer::Hidden) {
    return ENOENT;
  }
  struct stat St = N.St;
  if (N.Where == Layer::Host && ::lstat(N.Path.c_str(), &St) != 0) {
    return errno;
  }
  if (S_ISDIR(St.st_mode)) {
    return EnsureUpperDir(N.Path);
  }
  if (int Err = EnsureUpperDir(DirName(N.Path))) {
    return Err;
  }
  return CopyObject(N, N.Path, WithData);
}

bool RootFSOverlay::BuildListing(const fextl::string& GuestDir, fextl::vector<DirEntry>& Out) const {
  Node N = Resolve(GuestDir, true);
  if (N.Error || N.Where != Layer::Upper || !S_ISDIR(N.St.st_mode)) {
    return false;
  }
  DirState Self = N.Self;
  const bool LowerShown = Self.Lower && !Opaque(N.Path, Self);

  fextl::set<fextl::string> Seen;
  fextl::set<fextl::string> Whiteouts;
  int Dir = ::openat(UpperFD, Rel(N.Path), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (Dir == -1) {
    return false;
  }
  ForEachEntry(Dir, [&](uint64_t Ino, uint8_t Type, std::string_view Name) {
    if (Name.starts_with(WhiteoutPrefix)) {
      if (Name != OpaqueMarker) {
        Whiteouts.emplace(Name.substr(WhiteoutPrefix.size()));
      }
      return;
    }
    Seen.emplace(Name);
    Out.push_back({Ino, Type, fextl::string {Name}});
  });
  ::close(Dir);

  if (LowerShown) {
    Dir = ::openat(BaseFD, Rel(N.Path), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (Dir != -1) {
      ForEachEntry(Dir, [&](uint64_t Ino, uint8_t Type, std::string_view Name) {
        if (Name == "." || Name == ".." || Name.starts_with(WhiteoutPrefix)) {
          return;
        }
        fextl::string S {Name};
        if (Seen.contains(S) || Whiteouts.contains(S)) {
          return;
        }
        Out.push_back({Ino, Type, std::move(S)});
      });
      ::close(Dir);
    }
  }
  return true;
}

int RootFSOverlay::CheckEmptyDir(const Node& N) {
  if (N.Where == Layer::Host || N.Where == Layer::Lower) {
    // A single layer: no whiteouts can apply.
    int Dir = N.Where == Layer::Host ? ::open(N.Path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) :
                                       ::openat(BaseFD, Rel(N.Path), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (Dir == -1) {
      return errno;
    }
    bool Empty = true;
    ForEachEntry(Dir, [&](uint64_t, uint8_t, std::string_view Name) {
      if (Name != "." && Name != "..") {
        Empty = false;
      }
    });
    ::close(Dir);
    return Empty ? 0 : ENOTEMPTY;
  }
  fextl::vector<DirEntry> Entries;
  if (!BuildListing(N.Path, Entries)) {
    return EIO;
  }
  for (const auto& E : Entries) {
    if (E.Name != "." && E.Name != "..") {
      return ENOTEMPTY;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Syscalls
// ---------------------------------------------------------------------------

std::optional<uint64_t> RootFSOverlay::Openat(int DirFD, const char* Path, int Flags, uint32_t Mode) {
  const bool Tmpfile = (Flags & O_TMPFILE) == O_TMPFILE;
  const bool Creat = (Flags & O_CREAT) && !Tmpfile;
  const bool Excl = Creat && (Flags & O_EXCL);
  const bool PathOnly = Flags & O_PATH;
  const bool Writes = !PathOnly && ((Flags & O_ACCMODE) != O_RDONLY || (Flags & O_TRUNC) || Creat || Tmpfile);
  // O_CREAT|O_EXCL never follows the final symlink; a dangling one is EEXIST.
  const bool Follow = !(Flags & O_NOFOLLOW) && !Excl;

  auto N = Lookup(DirFD, Path, Follow);
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }

  if (!N->Owned) {
    // Reached only through an overlay symlink into unowned territory: open the
    // object where it was found, with the caller's flags.
    if (N->Where == Layer::Hidden) {
      return Fail(ENOENT);
    }
    int FD = N->Where == Layer::Host ? ::open(N->Path.c_str(), Flags, Mode) : OpenIn(LayerFD(N->Where), Rel(N->Path), Flags, Mode);
    return FD == -1 ? Fail(errno) : static_cast<uint64_t>(FD);
  }

  auto Create = [&]() -> uint64_t {
    if (N->StoppedEarly && N->Where == Layer::Hidden) {
      return Fail(ENOENT);
    }
    if (int Err = EnsureUpperDir(DirName(N->Path))) {
      return Fail(Err);
    }
    int FD = OpenIn(UpperFD, Rel(N->Path), Flags, Mode);
    if (FD == -1) {
      return Fail(errno);
    }
    RemoveWhiteout(N->Path);
    return FD;
  };

  switch (N->Where) {
  case Layer::Upper: {
    if (Tmpfile && !S_ISDIR(N->St.st_mode)) {
      return Fail(ENOTDIR);
    }
    int FD = OpenIn(UpperFD, Rel(N->Path), Flags, Mode);
    return FD == -1 ? Fail(errno) : static_cast<uint64_t>(FD);
  }
  case Layer::Hidden: return Creat ? Create() : Fail(ENOENT);
  case Layer::Lower:
  case Layer::Host: {
    struct stat St = N->St;
    if (N->Where == Layer::Host) {
      const int R = Follow ? ::stat(N->Path.c_str(), &St) : ::lstat(N->Path.c_str(), &St);
      if (R != 0) {
        if (errno == ENOENT && Creat) {
          return Create();
        }
        return Fail(errno);
      }
    }
    if (!Writes) {
      int FD = N->Where == Layer::Host ? ::open(N->Path.c_str(), Flags, Mode) : OpenIn(BaseFD, Rel(N->Path), Flags, Mode);
      return FD == -1 ? Fail(errno) : static_cast<uint64_t>(FD);
    }
    if (Excl) {
      return Fail(EEXIST);
    }
    if (S_ISDIR(St.st_mode)) {
      if (!Tmpfile) {
        return Fail(EISDIR);
      }
      if (int Err = EnsureUpperDir(N->Path)) {
        return Fail(Err);
      }
      int FD = OpenIn(UpperFD, Rel(N->Path), Flags, Mode);
      return FD == -1 ? Fail(errno) : static_cast<uint64_t>(FD);
    }
    if (Tmpfile) {
      return Fail(ENOTDIR);
    }
    if (S_ISLNK(St.st_mode)) {
      // O_NOFOLLOW on a symlink.
      return Fail(ELOOP);
    }
    if (int Err = CopyUp(*N, !(Flags & O_TRUNC))) {
      return Fail(Err);
    }
    int FD = OpenIn(UpperFD, Rel(N->Path), Flags & ~(O_CREAT | O_EXCL), Mode);
    return FD == -1 ? Fail(errno) : static_cast<uint64_t>(FD);
  }
  }
  return std::nullopt;
}

std::optional<uint64_t> RootFSOverlay::Fstatat(int DirFD, const char* Path, struct stat* Buf, int Flags) {
  auto N = Lookup(DirFD, Path, !(Flags & AT_SYMLINK_NOFOLLOW));
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  switch (N->Where) {
  case Layer::Upper:
  case Layer::Lower: memcpy(Buf, &N->St, sizeof(*Buf)); return 0;
  case Layer::Host: {
    uint64_t R = ::fstatat(AT_FDCWD, N->Path.c_str(), Buf, Flags & ~AT_EMPTY_PATH);
    return R;
  }
  case Layer::Hidden: break;
  }
  return Fail(ENOENT);
}

std::optional<uint64_t> RootFSOverlay::Statx(int DirFD, const char* Path, int Flags, uint32_t Mask, struct statx* Buf) {
  auto N = Lookup(DirFD, Path, !(Flags & AT_SYMLINK_NOFOLLOW));
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  switch (N->Where) {
  case Layer::Upper:
  case Layer::Lower:
    return ::syscall(SYSCALL_DEF(statx), LayerFD(N->Where), Rel(N->Path), (Flags & ~AT_EMPTY_PATH) | AT_SYMLINK_NOFOLLOW, Mask, Buf);
  case Layer::Host: return ::syscall(SYSCALL_DEF(statx), AT_FDCWD, N->Path.c_str(), Flags & ~AT_EMPTY_PATH, Mask, Buf);
  case Layer::Hidden: break;
  }
  return Fail(ENOENT);
}

std::optional<uint64_t> RootFSOverlay::Faccessat(int DirFD, const char* Path, int Mode, int Flags) {
  auto N = Lookup(DirFD, Path, !(Flags & AT_SYMLINK_NOFOLLOW));
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  switch (N->Where) {
  case Layer::Upper:
  case Layer::Lower:
    return ::syscall(SYSCALL_DEF(faccessat2), LayerFD(N->Where), Rel(N->Path), Mode, (Flags & AT_EACCESS) | AT_SYMLINK_NOFOLLOW);
  case Layer::Host: return ::syscall(SYSCALL_DEF(faccessat2), AT_FDCWD, N->Path.c_str(), Mode, Flags & ~AT_EMPTY_PATH);
  case Layer::Hidden: break;
  }
  return Fail(ENOENT);
}

std::optional<uint64_t> RootFSOverlay::Readlinkat(int DirFD, const char* Path, char* Buf, size_t Size) {
  auto N = Lookup(DirFD, Path, false);
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  switch (N->Where) {
  case Layer::Upper:
  case Layer::Lower: {
    if (!S_ISLNK(N->St.st_mode)) {
      return Fail(EINVAL);
    }
    ssize_t R = ::readlinkat(LayerFD(N->Where), Rel(N->Path), Buf, Size);
    return R < 0 ? Fail(errno) : static_cast<uint64_t>(R);
  }
  case Layer::Host: {
    ssize_t R = ::readlink(N->Path.c_str(), Buf, Size);
    return R < 0 ? Fail(errno) : static_cast<uint64_t>(R);
  }
  case Layer::Hidden: break;
  }
  return Fail(ENOENT);
}

std::optional<uint64_t> RootFSOverlay::Fchmodat(int DirFD, const char* Path, mode_t Mode, int Flags) {
  const bool Follow = !(Flags & AT_SYMLINK_NOFOLLOW);
  auto N = Lookup(DirFD, Path, Follow);
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  if (N->Where == Layer::Hidden) {
    return Fail(ENOENT);
  }
  if (!N->Owned) {
    if (N->Where == Layer::Host) {
      return ::syscall(SYSCALL_DEF(fchmodat), AT_FDCWD, N->Path.c_str(), Mode);
    }
    return ::syscall(SYSCALL_DEF(fchmodat), LayerFD(N->Where), Rel(N->Path), Mode);
  }
  if (N->Where != Layer::Host && S_ISLNK(N->St.st_mode)) {
    // Linux has no mode on symlinks.
    return Fail(EOPNOTSUPP);
  }
  if (int Err = CopyUp(*N, true)) {
    return Fail(Err);
  }
  return ::syscall(SYSCALL_DEF(fchmodat), UpperFD, Rel(N->Path), Mode);
}

std::optional<uint64_t> RootFSOverlay::Fchownat(int DirFD, const char* Path, uid_t Owner, gid_t Group, int Flags) {
  auto N = Lookup(DirFD, Path, !(Flags & AT_SYMLINK_NOFOLLOW));
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  if (N->Where == Layer::Hidden) {
    return Fail(ENOENT);
  }
  if (!N->Owned) {
    if (N->Where == Layer::Host) {
      return ::syscall(SYSCALL_DEF(fchownat), AT_FDCWD, N->Path.c_str(), Owner, Group, Flags & ~AT_EMPTY_PATH);
    }
    return ::syscall(SYSCALL_DEF(fchownat), LayerFD(N->Where), Rel(N->Path), Owner, Group, AT_SYMLINK_NOFOLLOW);
  }
  if (int Err = CopyUp(*N, true)) {
    return Fail(Err);
  }
  return ::syscall(SYSCALL_DEF(fchownat), UpperFD, Rel(N->Path), Owner, Group, AT_SYMLINK_NOFOLLOW);
}

std::optional<uint64_t> RootFSOverlay::Utimensat(int DirFD, const char* Path, const struct timespec* Times, int Flags) {
  auto N = Lookup(DirFD, Path, !(Flags & AT_SYMLINK_NOFOLLOW));
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  if (N->Where == Layer::Hidden) {
    return Fail(ENOENT);
  }
  if (!N->Owned) {
    if (N->Where == Layer::Host) {
      return ::syscall(SYSCALL_DEF(utimensat), AT_FDCWD, N->Path.c_str(), Times, Flags & ~AT_EMPTY_PATH);
    }
    return ::syscall(SYSCALL_DEF(utimensat), LayerFD(N->Where), Rel(N->Path), Times, AT_SYMLINK_NOFOLLOW);
  }
  if (int Err = CopyUp(*N, true)) {
    return Fail(Err);
  }
  return ::syscall(SYSCALL_DEF(utimensat), UpperFD, Rel(N->Path), Times, AT_SYMLINK_NOFOLLOW);
}

std::optional<uint64_t> RootFSOverlay::Truncate(const char* Path, off_t Length) {
  auto N = Lookup(AT_FDCWD, Path, true);
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  if (N->Where == Layer::Hidden) {
    return Fail(ENOENT);
  }
  if (!N->Owned) {
    return ::truncate(HostPathOf(*N).c_str(), Length);
  }
  if (N->Where != Layer::Host && S_ISDIR(N->St.st_mode)) {
    return Fail(EISDIR);
  }
  if (int Err = CopyUp(*N, Length != 0)) {
    return Fail(Err);
  }
  int FD = OpenIn(UpperFD, Rel(N->Path), O_WRONLY | O_CLOEXEC, 0);
  if (FD == -1) {
    return Fail(errno);
  }
  uint64_t R = ::ftruncate(FD, Length);
  int Saved = errno;
  ::close(FD);
  errno = Saved;
  return R;
}

std::optional<uint64_t> RootFSOverlay::Unlinkat(int DirFD, const char* Path, int Flags) {
  auto N = Lookup(DirFD, Path, false);
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  const bool RemoveDir = Flags & AT_REMOVEDIR;
  if (N->Where == Layer::Hidden) {
    return Fail(ENOENT);
  }
  if (!N->Owned) {
    if (N->Where == Layer::Host) {
      return ::syscall(SYSCALL_DEF(unlinkat), AT_FDCWD, N->Path.c_str(), Flags);
    }
    return ::syscall(SYSCALL_DEF(unlinkat), LayerFD(N->Where), Rel(N->Path), Flags);
  }
  if (RemoveDir && (N->Path == "/" || BaseName(N->Path) == "." || BaseName(N->Path) == "..")) {
    return Fail(EINVAL);
  }

  struct stat St = N->St;
  if (N->Where == Layer::Host && ::lstat(N->Path.c_str(), &St) != 0) {
    return Fail(errno);
  }
  if (RemoveDir && !S_ISDIR(St.st_mode)) {
    return Fail(ENOTDIR);
  }
  if (!RemoveDir && S_ISDIR(St.st_mode)) {
    return Fail(EISDIR);
  }
  if (RemoveDir) {
    if (int Err = CheckEmptyDir(*N)) {
      return Fail(Err);
    }
  }

  if (N->Where == Layer::Upper) {
    if (RemoveDir) {
      // Only whiteouts and the opaque marker are left in it.
      int Dir = ::openat(UpperFD, Rel(N->Path), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (Dir != -1) {
        fextl::vector<fextl::string> Markers;
        ForEachEntry(Dir, [&](uint64_t, uint8_t, std::string_view Name) {
          if (Name.starts_with(WhiteoutPrefix)) {
            Markers.emplace_back(Name);
          }
        });
        for (const auto& M : Markers) {
          ::unlinkat(Dir, M.c_str(), 0);
        }
        ::close(Dir);
      }
    }
    if (::unlinkat(UpperFD, Rel(N->Path), Flags & AT_REMOVEDIR) != 0) {
      return Fail(errno);
    }
    if (N->HaveParent && LowerVisible(N->Path, N->Parent)) {
      if (int Err = MakeWhiteout(N->Path)) {
        return Fail(Err);
      }
    }
    return 0;
  }

  // In the base or on the host: hide it.
  if (int Err = MakeWhiteout(N->Path)) {
    return Fail(Err);
  }
  return 0;
}

std::optional<uint64_t> RootFSOverlay::Mkdirat(int DirFD, const char* Path, mode_t Mode) {
  auto N = Lookup(DirFD, Path, false);
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  if (!N->Owned) {
    if (N->Where != Layer::Host) {
      return Fail(N->Where == Layer::Hidden ? ENOENT : EEXIST);
    }
    return ::syscall(SYSCALL_DEF(mkdirat), AT_FDCWD, N->Path.c_str(), Mode);
  }
  if (N->Where == Layer::Upper || N->Where == Layer::Lower) {
    return Fail(EEXIST);
  }
  struct stat St {};
  if (N->Where == Layer::Host && ::lstat(N->Path.c_str(), &St) == 0) {
    return Fail(EEXIST);
  }
  if (N->Where == Layer::Hidden && N->StoppedEarly) {
    return Fail(ENOENT);
  }
  if (int Err = EnsureUpperDir(DirName(N->Path))) {
    return Fail(Err);
  }
  if (::mkdirat(UpperFD, Rel(N->Path), Mode) != 0) {
    return Fail(errno);
  }
  // Recreating a deleted directory must not bring its old contents back.
  struct stat W {};
  if (::fstatat(UpperFD, Rel(WhiteoutOf(N->Path)), &W, AT_SYMLINK_NOFOLLOW) == 0) {
    MakeOpaque(N->Path);
    RemoveWhiteout(N->Path);
  }
  return 0;
}

std::optional<uint64_t> RootFSOverlay::Mknodat(int DirFD, const char* Path, mode_t Mode, dev_t Dev) {
  auto N = Lookup(DirFD, Path, false);
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  if (!N->Owned) {
    if (N->Where != Layer::Host) {
      return Fail(N->Where == Layer::Hidden ? ENOENT : EEXIST);
    }
    return ::syscall(SYSCALL_DEF(mknodat), AT_FDCWD, N->Path.c_str(), Mode, Dev);
  }
  struct stat St {};
  if (N->Where == Layer::Upper || N->Where == Layer::Lower || (N->Where == Layer::Host && ::lstat(N->Path.c_str(), &St) == 0)) {
    return Fail(EEXIST);
  }
  if (N->Where == Layer::Hidden && N->StoppedEarly) {
    return Fail(ENOENT);
  }
  if (int Err = EnsureUpperDir(DirName(N->Path))) {
    return Fail(Err);
  }
  if (::mknodat(UpperFD, Rel(N->Path), Mode, Dev) != 0) {
    return Fail(errno);
  }
  RemoveWhiteout(N->Path);
  return 0;
}

std::optional<uint64_t> RootFSOverlay::Symlinkat(const char* Target, int DirFD, const char* Path) {
  auto N = Lookup(DirFD, Path, false);
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  if (!N->Owned) {
    if (N->Where != Layer::Host) {
      return Fail(N->Where == Layer::Hidden ? ENOENT : EEXIST);
    }
    return ::syscall(SYSCALL_DEF(symlinkat), Target, AT_FDCWD, N->Path.c_str());
  }
  struct stat St {};
  if (N->Where == Layer::Upper || N->Where == Layer::Lower || (N->Where == Layer::Host && ::lstat(N->Path.c_str(), &St) == 0)) {
    return Fail(EEXIST);
  }
  if (N->Where == Layer::Hidden && N->StoppedEarly) {
    return Fail(ENOENT);
  }
  if (int Err = EnsureUpperDir(DirName(N->Path))) {
    return Fail(Err);
  }
  if (::symlinkat(Target, UpperFD, Rel(N->Path)) != 0) {
    return Fail(errno);
  }
  RemoveWhiteout(N->Path);
  return 0;
}

std::optional<uint64_t> RootFSOverlay::Linkat(int OldDirFD, const char* OldPath, int NewDirFD, const char* NewPath, int Flags) {
  const bool EmptyOld = (Flags & AT_EMPTY_PATH) && OldPath && OldPath[0] == 0;
  std::optional<Node> Old;
  if (!EmptyOld) {
    Old = Lookup(OldDirFD, OldPath, Flags & AT_SYMLINK_FOLLOW);
  }
  auto New = Lookup(NewDirFD, NewPath, false);
  if (!Old && !New) {
    return std::nullopt;
  }

  // Where the source is.
  int SrcFD = OldDirFD;
  fextl::string SrcPath = OldPath ? OldPath : "";
  int SrcFlags = Flags;
  if (Old) {
    if (Old->Error) {
      return Fail(Old->Error);
    }
    if (Old->Where == Layer::Hidden) {
      return Fail(ENOENT);
    }
    if (Old->Owned && Old->Where != Layer::Upper) {
      struct stat St = Old->St;
      if (Old->Where == Layer::Host && ::lstat(Old->Path.c_str(), &St) != 0) {
        return Fail(errno);
      }
      if (S_ISDIR(St.st_mode)) {
        return Fail(EPERM);
      }
      // Link to the overlay copy, so both names share it.
      if (int Err = CopyUp(*Old, true)) {
        return Fail(Err);
      }
      Old->Where = Layer::Upper;
    }
    SrcFD = Old->Where == Layer::Host ? AT_FDCWD : LayerFD(Old->Where);
    SrcPath = Old->Where == Layer::Host ? Old->Path : fextl::string {Rel(Old->Path)};
    SrcFlags = 0;
  } else if (!EmptyOld && OldPath && OldPath[0] == '/' && ::faccessat(AT_FDCWD, OldPath, F_OK, AT_SYMLINK_NOFOLLOW) != 0 &&
             ::faccessat(BaseFD, OldPath + 1, F_OK, AT_SYMLINK_NOFOLLOW) == 0) {
    // FileManager's per-leg rule: host if it exists there, else the rootfs.
    SrcFD = BaseFD;
    SrcPath = OldPath + 1;
  }

  // Where the new name goes.
  int DstFD = NewDirFD;
  fextl::string DstPath = NewPath ? NewPath : "";
  if (New) {
    if (New->Error) {
      return Fail(New->Error);
    }
    if (!New->Owned) {
      if (New->Where != Layer::Host) {
        return Fail(New->Where == Layer::Hidden ? ENOENT : EEXIST);
      }
      DstFD = AT_FDCWD;
      DstPath = New->Path;
    } else {
      struct stat St {};
      if (New->Where == Layer::Upper || New->Where == Layer::Lower || (New->Where == Layer::Host && ::lstat(New->Path.c_str(), &St) == 0)) {
        return Fail(EEXIST);
      }
      if (New->Where == Layer::Hidden && New->StoppedEarly) {
        return Fail(ENOENT);
      }
      if (int Err = EnsureUpperDir(DirName(New->Path))) {
        return Fail(Err);
      }
      DstFD = UpperFD;
      DstPath = Rel(New->Path);
    }
  }

  uint64_t Result = ::syscall(SYSCALL_DEF(linkat), SrcFD, SrcPath.c_str(), DstFD, DstPath.c_str(), SrcFlags);
  if (Result == static_cast<uint64_t>(-1) && errno == EXDEV) {
    // Across filesystems: copy, as FileManager::Linkat does.
    int In = -1;
    if (EmptyOld) {
      char Proc[64];
      snprintf(Proc, sizeof(Proc), "/proc/self/fd/%d", OldDirFD);
      In = ::open(Proc, O_RDONLY | O_CLOEXEC);
    } else {
      In = ::openat(SrcFD, SrcPath.c_str(), O_RDONLY | O_CLOEXEC | ((SrcFlags & AT_SYMLINK_FOLLOW) ? 0 : O_NOFOLLOW));
    }
    if (In == -1) {
      return Fail(errno == ELOOP ? EXDEV : errno);
    }
    struct stat St {};
    ::fstat(In, &St);
    int Out = ::openat(DstFD, DstPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, St.st_mode & 07777);
    if (Out == -1) {
      int Saved = errno;
      ::close(In);
      return Fail(Saved);
    }
    int Err = CopyData(In, Out);
    ::close(Out);
    ::close(In);
    if (Err) {
      return Fail(Err);
    }
    Result = 0;
  }
  if (Result == 0 && New && New->Owned) {
    RemoveWhiteout(New->Path);
  }
  return Result;
}

std::optional<uint64_t> RootFSOverlay::Renameat2(int OldDirFD, const char* OldPath, int NewDirFD, const char* NewPath, unsigned Flags) {
  auto Old = Lookup(OldDirFD, OldPath, false);
  auto New = Lookup(NewDirFD, NewPath, false);
  if (!Old && !New) {
    return std::nullopt;
  }
  if (Flags & RENAME_WHITEOUT) {
    return Fail(EINVAL);
  }
  if (Old && Old->Error) {
    return Fail(Old->Error);
  }
  if (New && New->Error) {
    return Fail(New->Error);
  }

  // The source.
  struct stat OldSt {};
  int SrcFD = OldDirFD;
  fextl::string SrcPath = OldPath;
  if (Old) {
    if (Old->Where == Layer::Hidden) {
      return Fail(ENOENT);
    }
    OldSt = Old->St;
    if (Old->Where == Layer::Host && ::lstat(Old->Path.c_str(), &OldSt) != 0) {
      return Fail(errno);
    }
    SrcFD = Old->Where == Layer::Host ? AT_FDCWD : LayerFD(Old->Where);
    SrcPath = Old->Where == Layer::Host ? Old->Path : fextl::string {Rel(Old->Path)};
  } else {
    if (OldPath[0] == '/' && ::faccessat(AT_FDCWD, OldPath, F_OK, AT_SYMLINK_NOFOLLOW) != 0 &&
        ::faccessat(BaseFD, OldPath + 1, F_OK, AT_SYMLINK_NOFOLLOW) == 0) {
      SrcFD = BaseFD;
      SrcPath = OldPath + 1;
    }
    if (::fstatat(SrcFD, SrcPath.c_str(), &OldSt, AT_SYMLINK_NOFOLLOW) != 0) {
      return Fail(errno);
    }
  }
  const bool OldOwned = Old && Old->Owned;
  const bool OldIsDir = S_ISDIR(OldSt.st_mode);

  if (Flags & RENAME_EXCHANGE) {
    // Only an exchange of two overlay entries keeps both layers consistent.
    if (!(Old && New && OldOwned && New->Owned && Old->Where == Layer::Upper && New->Where == Layer::Upper && !OldIsDir &&
          !S_ISDIR(New->St.st_mode))) {
      return Fail(EXDEV);
    }
    return ::syscall(SYSCALL_DEF(renameat2), UpperFD, Rel(Old->Path), UpperFD, Rel(New->Path), Flags);
  }

  if (OldOwned && OldIsDir) {
    // A directory with anything below it in the base or on the host cannot be
    // moved without moving those too; like overlayfs without redirect_dir,
    // report EXDEV and let the caller copy.
    if (Old->Where != Layer::Upper || (Old->HaveParent && LowerVisible(Old->Path, Old->Parent))) {
      return Fail(EXDEV);
    }
  }

  // The destination.
  int DstFD = NewDirFD;
  fextl::string DstPath = NewPath;
  bool DstWhiteout = false;
  if (New) {
    if (!New->Owned) {
      if (New->Where == Layer::Hidden) {
        return Fail(ENOENT);
      }
      DstFD = New->Where == Layer::Host ? AT_FDCWD : LayerFD(New->Where);
      DstPath = New->Where == Layer::Host ? New->Path : fextl::string {Rel(New->Path)};
    } else {
      if (New->Where == Layer::Hidden && New->StoppedEarly) {
        return Fail(ENOENT);
      }
      struct stat NewSt = New->St;
      bool NewExists = New->Where == Layer::Upper || New->Where == Layer::Lower;
      if (New->Where == Layer::Host) {
        NewExists = ::lstat(New->Path.c_str(), &NewSt) == 0;
      }
      if (NewExists) {
        if (Flags & RENAME_NOREPLACE) {
          return Fail(EEXIST);
        }
        if (OldIsDir && !S_ISDIR(NewSt.st_mode)) {
          return Fail(ENOTDIR);
        }
        if (!OldIsDir && S_ISDIR(NewSt.st_mode)) {
          return Fail(EISDIR);
        }
        if (S_ISDIR(NewSt.st_mode)) {
          if (int Err = CheckEmptyDir(*New)) {
            return Fail(Err);
          }
          if (New->Where != Layer::Upper || (New->HaveParent && LowerVisible(New->Path, New->Parent))) {
            return Fail(EXDEV);
          }
        }
      }
      if (int Err = EnsureUpperDir(DirName(New->Path))) {
        return Fail(Err);
      }
      struct stat W {};
      DstWhiteout = ::fstatat(UpperFD, Rel(WhiteoutOf(New->Path)), &W, AT_SYMLINK_NOFOLLOW) == 0;
      DstFD = UpperFD;
      DstPath = Rel(New->Path);
    }
  } else if (NewPath[0] == '/') {
    // FileManager's per-leg rule for a new name: the host, unless its parent is only in the rootfs.
    fextl::string Parent = DirName(fextl::string {NewPath});
    struct stat P {};
    if (::stat(Parent.c_str(), &P) != 0 && ::fstatat(BaseFD, Rel(Parent), &P, 0) == 0) {
      DstFD = BaseFD;
      DstPath = NewPath + 1;
    }
  }

  uint64_t Result;
  if (!OldOwned || Old->Where == Layer::Upper) {
    Result = ::syscall(SYSCALL_DEF(renameat2), SrcFD, SrcPath.c_str(), DstFD, DstPath.c_str(), Flags);
    if (Result == static_cast<uint64_t>(-1) && errno == EXDEV && !OldIsDir && !(Flags & RENAME_NOREPLACE)) {
      // Across filesystems: copy and remove, as FileManager::Renameat2 does.
      int Err = 0;
      if (S_ISLNK(OldSt.st_mode)) {
        char Target[PATH_MAX];
        ssize_t Len = ::readlinkat(SrcFD, SrcPath.c_str(), Target, sizeof(Target) - 1);
        if (Len < 0) {
          return Fail(errno);
        }
        Target[Len] = 0;
        ::unlinkat(DstFD, DstPath.c_str(), 0);
        Err = ::symlinkat(Target, DstFD, DstPath.c_str()) == 0 ? 0 : errno;
      } else {
        int In = ::openat(SrcFD, SrcPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (In == -1) {
          return Fail(errno);
        }
        int Out = ::openat(DstFD, DstPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, OldSt.st_mode & 07777);
        if (Out == -1) {
          Err = errno;
        } else {
          Err = CopyData(In, Out);
          ::fchmod(Out, OldSt.st_mode & 07777);
          ::close(Out);
        }
        ::close(In);
      }
      if (Err) {
        return Fail(Err);
      }
      ::unlinkat(SrcFD, SrcPath.c_str(), 0);
      Result = 0;
    }
  } else {
    // The source is in the base or on the host: copy it to the destination.
    if (OldIsDir) {
      return Fail(EXDEV);
    }
    if (DstFD == UpperFD) {
      if (int Err = CopyObject(*Old, New->Path, true)) {
        return Fail(Err);
      }
    } else {
      Node Src = *Old;
      int Err = 0;
      if (S_ISLNK(OldSt.st_mode)) {
        char Target[PATH_MAX];
        ssize_t Len = ::readlinkat(SrcFD, SrcPath.c_str(), Target, sizeof(Target) - 1);
        if (Len < 0) {
          return Fail(errno);
        }
        Target[Len] = 0;
        ::unlinkat(DstFD, DstPath.c_str(), 0);
        Err = ::symlinkat(Target, DstFD, DstPath.c_str()) == 0 ? 0 : errno;
      } else {
        int In = Src.Where == Layer::Host ? ::open(Src.Path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW) :
                                            OpenIn(BaseFD, Rel(Src.Path), O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
        if (In == -1) {
          return Fail(errno);
        }
        int Out = ::openat(DstFD, DstPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, OldSt.st_mode & 07777);
        if (Out == -1) {
          Err = errno;
        } else {
          Err = CopyData(In, Out);
          ::close(Out);
        }
        ::close(In);
      }
      if (Err) {
        return Fail(Err);
      }
    }
    Result = 0;
  }
  if (Result != 0) {
    return Result;
  }

  if (OldOwned) {
    // The old name is gone from the guest's view. Hide any copy below it.
    if (Old->Where != Layer::Upper || (Old->HaveParent && LowerVisible(Old->Path, Old->Parent))) {
      MakeWhiteout(Old->Path);
    }
  }
  if (New && New->Owned && DstWhiteout) {
    if (OldIsDir) {
      MakeOpaque(New->Path);
    }
    RemoveWhiteout(New->Path);
  }
  return 0;
}

std::optional<uint64_t> RootFSOverlay::Chdir(const char* Path) {
  auto N = Lookup(AT_FDCWD, Path, true);
  if (!N) {
    return std::nullopt;
  }
  // Host first, as FileManager::Chdir: a directory that exists on the host is
  // entered there, so relative host executables keep working.
  uint64_t Result = ::chdir(Path);
  if (Result != static_cast<uint64_t>(-1) || errno != ENOENT) {
    return Result;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  if (N->Where == Layer::Upper || N->Where == Layer::Lower) {
    return ::chdir(HostPathOf(*N).c_str());
  }
  return Fail(ENOENT);
}

std::optional<uint64_t> RootFSOverlay::Statfs(const char* Path, struct statfs* Buf) {
  auto N = Lookup(AT_FDCWD, Path, true);
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  if (N->Where == Layer::Hidden) {
    return Fail(ENOENT);
  }
  return ::statfs(HostPathOf(*N).c_str(), Buf);
}

std::optional<uint64_t> RootFSOverlay::Xattr(XattrOp Op, int DirFD, const char* Path, bool Follow, const XattrArgs& A) {
  auto N = Lookup(DirFD, Path, Follow);
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  if (N->Where == Layer::Hidden) {
    return Fail(ENOENT);
  }
  const bool Writes = Op == XattrOp::Set || Op == XattrOp::Remove;
  if (Writes && N->Owned) {
    if (int Err = CopyUp(*N, true)) {
      return Fail(Err);
    }
    N->Where = Layer::Upper;
  }
  // The resolved path has no symlinks left to follow unless the host resolves
  // a remainder, where the caller's choice applies.
  const bool UseFollow = N->Where == Layer::Host ? Follow : false;
  const fextl::string Host = HostPathOf(*N);
  const char* P = Host.c_str();
  ssize_t R = -1;
  switch (Op) {
  case XattrOp::Get: R = UseFollow ? ::getxattr(P, A.Name, A.Value, A.Size) : ::lgetxattr(P, A.Name, A.Value, A.Size); break;
  case XattrOp::List:
    R = UseFollow ? ::listxattr(P, static_cast<char*>(A.Value), A.Size) : ::llistxattr(P, static_cast<char*>(A.Value), A.Size);
    break;
  case XattrOp::Set:
    R = UseFollow ? ::setxattr(P, A.Name, A.Value, A.Size, A.SetFlags) : ::lsetxattr(P, A.Name, A.Value, A.Size, A.SetFlags);
    break;
  case XattrOp::Remove: R = UseFollow ? ::removexattr(P, A.Name) : ::lremovexattr(P, A.Name); break;
  }
  return R < 0 ? Fail(errno) : static_cast<uint64_t>(R);
}

std::optional<fextl::string> RootFSOverlay::EmulatedPath(const char* Path, bool Follow) const {
  auto N = Lookup(AT_FDCWD, Path, Follow);
  if (!N) {
    return std::nullopt;
  }
  if (N->Error) {
    N->Where = Layer::Hidden;
  }
  return HostPathOf(*N);
}

bool RootFSOverlay::IsHidden(const char* Path) const {
  auto N = Lookup(AT_FDCWD, Path, true);
  return N && N->Where == Layer::Hidden;
}

std::optional<uint64_t> RootFSOverlay::Getdents64(int FD, void* GuestDirp, uint32_t Count) {
  struct stat St {};
  if (::fstat(FD, &St) != 0 || !S_ISDIR(St.st_mode) || St.st_dev != UpperDev) {
    return std::nullopt;
  }
  char Buf[PATH_MAX];
  ssize_t Len = FEX::get_fdpath(FD, Buf);
  if (Len <= 0 || Len >= static_cast<ssize_t>(sizeof(Buf))) {
    return std::nullopt;
  }
  const fextl::string Guest = StripUpperPrefix(std::string_view {Buf, static_cast<size_t>(Len)});
  if (Guest.empty() || !IsOwned(Guest)) {
    return std::nullopt;
  }

  // The descriptor's own file position is the cursor: an index into the
  // merged listing. It is per open file description, as the kernel's is, and
  // rewinddir()/seekdir() move it with lseek().
  off_t Pos = ::lseek(FD, 0, SEEK_CUR);
  if (Pos < 0) {
    return std::nullopt;
  }
  const auto Key = std::make_pair(static_cast<uint64_t>(St.st_dev), static_cast<uint64_t>(St.st_ino));

  fextl::vector<char> Out;
  size_t Index = static_cast<size_t>(Pos);
  {
    std::lock_guard lk(ListingMutex);
    auto It = Listings.find(Key);
    if (Pos == 0 || It == Listings.end()) {
      DirSnapshot Snap;
      if (!BuildListing(Guest, Snap.Entries)) {
        return std::nullopt;
      }
      if (Listings.size() > 64) {
        Listings.clear();
      }
      It = Listings.insert_or_assign(Key, std::move(Snap)).first;
    }
    const auto& Entries = It->second.Entries;
    constexpr size_t Header = 19; // d_ino, d_off, d_reclen, d_type
    while (Index < Entries.size()) {
      const auto& E = Entries[Index];
      const size_t RecLen = (Header + E.Name.size() + 1 + 7) & ~size_t {7};
      if (Out.size() + RecLen > Count) {
        break;
      }
      const size_t At = Out.size();
      Out.resize(At + RecLen, 0);
      const uint64_t Ino = E.Ino;
      const int64_t Off = static_cast<int64_t>(Index + 1);
      const uint16_t Rec = static_cast<uint16_t>(RecLen);
      memcpy(&Out[At], &Ino, 8);
      memcpy(&Out[At + 8], &Off, 8);
      memcpy(&Out[At + 16], &Rec, 2);
      Out[At + 18] = static_cast<char>(E.Type);
      memcpy(&Out[At + Header], E.Name.data(), E.Name.size());
      ++Index;
    }
    if (Out.empty() && Index < Entries.size()) {
      return Fail(EINVAL);
    }
  }
  if (!Out.empty() && FaultSafeUserMemAccess::CopyToUser(GuestDirp, Out.data(), Out.size()) != 0) {
    return Fail(EFAULT);
  }
  ::lseek(FD, static_cast<off_t>(Index), SEEK_SET);
  return Out.size();
}

std::optional<uint64_t> RootFSOverlay::SocketPath(bool Bind, int FD, const void* GuestAddr, uint32_t Len) {
  // sockaddr_un is laid out the same for the arm64 guest and the host.
  constexpr size_t PathOffset = offsetof(sockaddr_un, sun_path);
  if (!GuestAddr || Len <= PathOffset || Len > sizeof(sockaddr_un)) {
    return std::nullopt;
  }
  sockaddr_un Addr {};
  if (FaultSafeUserMemAccess::CopyFromUser(&Addr, GuestAddr, Len) != 0) {
    return std::nullopt;
  }
  if (Addr.sun_family != AF_UNIX || Addr.sun_path[0] == 0) {
    // Abstract or not a unix socket.
    return std::nullopt;
  }
  const fextl::string Path {Addr.sun_path, strnlen(Addr.sun_path, Len - PathOffset)};
  // bind() creates the final component and never follows it.
  auto N = Lookup(AT_FDCWD, Path.c_str(), !Bind);
  if (!N || !N->Owned) {
    return std::nullopt;
  }
  if (N->Error) {
    return Fail(N->Error);
  }
  int Dir = UpperFD;
  if (Bind) {
    struct stat St {};
    if (N->Where == Layer::Upper || N->Where == Layer::Lower || (N->Where == Layer::Host && ::lstat(N->Path.c_str(), &St) == 0)) {
      return Fail(EADDRINUSE);
    }
    if (N->Where == Layer::Hidden && N->StoppedEarly) {
      return Fail(ENOENT);
    }
    if (int Err = EnsureUpperDir(DirName(N->Path))) {
      return Fail(Err);
    }
  } else {
    if (N->Where == Layer::Hidden) {
      return Fail(ENOENT);
    }
    if (N->Where == Layer::Host) {
      // A host socket, reached as before.
      return std::nullopt;
    }
    Dir = LayerFD(N->Where);
  }

  fextl::string Host = (Dir == UpperFD ? Upper : Base) + N->Path;
  if (Host.size() >= sizeof(Addr.sun_path)) {
    // Too long for sun_path: reach the directory through its descriptor.
    Host = fextl::fmt::format("/proc/self/fd/{}/{}", Dir, Rel(N->Path));
    if (Host.size() >= sizeof(Addr.sun_path)) {
      return Fail(ENAMETOOLONG);
    }
  }
  sockaddr_un Out {};
  Out.sun_family = AF_UNIX;
  memcpy(Out.sun_path, Host.data(), Host.size());
  const socklen_t OutLen = PathOffset + Host.size() + 1;
  long R = ::syscall(Bind ? SYSCALL_DEF(bind) : SYSCALL_DEF(connect), FD, &Out, OutLen);
  if (R != 0) {
    return Fail(errno);
  }
  if (Bind) {
    RemoveWhiteout(N->Path);
  }
  return 0;
}

std::optional<uint64_t> RootFSOverlay::Getcwd(char* GuestBuf, size_t Size) {
  char Buf[PATH_MAX];
  long R = ::syscall(SYSCALL_DEF(getcwd), Buf, sizeof(Buf));
  if (R <= 0) {
    return std::nullopt;
  }
  fextl::string Guest = StripUpperPrefix(std::string_view {Buf});
  if (Guest.empty() && HasPrefix(Buf, Base)) {
    // A guest-owned directory entered from the base (chdir falls back to it
    // when the host has no such directory) reads as its guest path too.
    fextl::string InBase {std::string_view {Buf}.substr(Base.size())};
    if (IsOwned(InBase)) {
      Guest = std::move(InBase);
    }
  }
  if (Guest.empty()) {
    return std::nullopt;
  }
  if (Guest.size() + 1 > Size) {
    return Fail(ERANGE);
  }
  if (FaultSafeUserMemAccess::CopyToUser(GuestBuf, Guest.c_str(), Guest.size() + 1) != 0) {
    return Fail(EFAULT);
  }
  return Guest.size() + 1;
}

} // namespace FEX::HLE
