// SPDX-License-Identifier: MIT
// Cheap "is this actually a guest rootfs?" check.
//
// It exists because the alternative is silence: FileManager's constructor used to set
// RootFSFD = AT_FDCWD whenever the configured tree could not be opened, and from then on
// every guest absolute path resolved against the *host* filesystem.  A wrong or missing
// rootfs then looks like an emulator bug in whatever fails first.  Reading the ELF machine
// of the tree's own /bin/sh costs three syscalls and names the real problem instead.
#include "RootFSCheck.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/vector.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace FEX::RootFSCheck {
namespace {
  // First hit wins, so the cheapest and most universal paths come first.
  constexpr std::array<std::string_view, 8> ProbePaths {
    "/usr/bin/env", "/bin/sh", "/usr/bin/sh", "/bin/bash", "/usr/bin/bash", "/usr/bin/ls", "/bin/busybox", "/usr/bin/busybox",
  };

  // A symlink chain longer than this is a loop as far as we care.
  constexpr int MaxSymlinkHops = 64;

  fextl::string ReadLinkAt(const fextl::string& Path) {
    // Loop rather than trust one lstat()'d size: st_size can be stale on some filesystems.
    for (size_t Size = 256; Size <= PATH_MAX; Size *= 2) {
      fextl::string Buffer;
      Buffer.resize(Size);
      const auto Read = ::readlink(Path.c_str(), Buffer.data(), Size);
      if (Read == -1) {
        return {};
      }
      if (static_cast<size_t>(Read) < Size) {
        Buffer.resize(Read);
        return Buffer;
      }
    }
    return {};
  }

  bool IsSymlink(const fextl::string& Path) {
    struct stat Buffer {};
    return ::lstat(Path.c_str(), &Buffer) == 0 && S_ISLNK(Buffer.st_mode);
  }

  // Splits on '/', dropping empty components and ".".
  void SplitInto(std::string_view Path, fextl::vector<fextl::string>& Out, size_t At) {
    fextl::vector<fextl::string> Parts;
    size_t Pos = 0;
    while (Pos < Path.size()) {
      const auto Next = Path.find('/', Pos);
      const auto Component = Path.substr(Pos, Next == std::string_view::npos ? std::string_view::npos : Next - Pos);
      if (!Component.empty() && Component != ".") {
        Parts.emplace_back(Component);
      }
      if (Next == std::string_view::npos) {
        break;
      }
      Pos = Next + 1;
    }
    Out.insert(Out.begin() + At, Parts.begin(), Parts.end());
  }

  fextl::string TrimTrailingSlashes(const fextl::string& Path) {
    fextl::string Result = Path;
    while (Result.size() > 1 && Result.back() == '/') {
      Result.pop_back();
    }
    return Result;
  }

  // 0 when the file is not an ELF we can read a machine out of.
  uint16_t ELFMachineOf(const fextl::string& Path) {
    const int FD = ::open(Path.c_str(), O_RDONLY | O_CLOEXEC);
    if (FD == -1) {
      return EM_NONE_;
    }
    // e_machine sits at offset 18 in both ELF32 and ELF64.
    uint8_t Header[20];
    const auto Read = ::pread(FD, Header, sizeof(Header), 0);
    ::close(FD);
    if (Read != static_cast<ssize_t>(sizeof(Header))) {
      return EM_NONE_;
    }
    if (Header[0] != 0x7F || Header[1] != 'E' || Header[2] != 'L' || Header[3] != 'F') {
      return EM_NONE_;
    }
    // EI_DATA: 1 is little-endian, 2 is big-endian.  A ppc64 (BE) tree must be named
    // correctly too, so honour it rather than assuming the host's order.
    if (Header[5] == 2) {
      return static_cast<uint16_t>((Header[18] << 8) | Header[19]);
    }
    return static_cast<uint16_t>(Header[18] | (Header[19] << 8));
  }
} // namespace

bool IsFatal(enum Verdict V) {
  switch (V) {
  case Verdict::MISSING:
  case Verdict::EMPTY:
  case Verdict::WRONG_MACHINE:
  case Verdict::NOT_A_DIRECTORY: return true;
  case Verdict::OK:
  case Verdict::UNVERIFIED: return false;
  }
  return false;
}

const char* MachineName(uint16_t Machine) {
  switch (Machine) {
  case EM_AARCH64_: return "AArch64";
  case EM_X86_64_: return "x86-64";
  case EM_386_: return "i386";
  case EM_ARM_: return "32-bit Arm";
  case EM_PPC64_: return "ppc64";
  case EM_RISCV_: return "RISC-V";
  default: return nullptr;
  }
}

fextl::string ResolveInsideRootFS(const fextl::string& Base, const fextl::string& Relative) {
  const fextl::string BasePath = TrimTrailingSlashes(Base);
  fextl::string Result = BasePath;

  fextl::vector<fextl::string> Pending;
  SplitInto(Relative, Pending, 0);

  int Hops = 0;
  while (!Pending.empty()) {
    if (++Hops > MaxSymlinkHops) {
      return {};
    }

    const fextl::string Component = Pending.front();
    Pending.erase(Pending.begin());
    if (Component == "..") {
      // ".." at the guest's root stays at the root, as the kernel does; anything
      // that would climb out of the tree is refused instead.
      if (Result == BasePath) {
        continue;
      }
      const auto Slash = Result.rfind('/');
      if (Slash == fextl::string::npos || Slash < BasePath.size()) {
        return {};
      }
      Result.resize(Slash);
      continue;
    }

    Result += "/";
    Result += Component;
    if (!IsSymlink(Result)) {
      continue;
    }

    const auto Target = ReadLinkAt(Result);
    if (Target.empty()) {
      return {};
    }

    if (Target.front() == '/') {
      // The whole point: an absolute target is relative to the *rootfs*, not the host.
      Result = BasePath;
    } else {
      const auto Slash = Result.rfind('/');
      Result.resize(Slash == fextl::string::npos ? 0 : Slash);
    }
    SplitInto(Target, Pending, 0);
  }

  // Never report a path that climbed back out of the tree.
  if (Result.size() < BasePath.size() || Result.compare(0, BasePath.size(), BasePath) != 0) {
    return {};
  }
  return Result;
}

uint16_t DetectMachine(const fextl::string& RootFS, fextl::string* ProbeOut) {
  for (const auto& Relative : ProbePaths) {
    const auto Resolved = ResolveInsideRootFS(RootFS, fextl::string {Relative});
    if (Resolved.empty()) {
      continue;
    }
    struct stat Buffer {};
    if (::stat(Resolved.c_str(), &Buffer) != 0 || !S_ISREG(Buffer.st_mode)) {
      continue;
    }
    const auto Machine = ELFMachineOf(Resolved);
    if (Machine != EM_NONE_) {
      if (ProbeOut) {
        *ProbeOut = Resolved;
      }
      return Machine;
    }
  }
  return EM_NONE_;
}

Result Check(const fextl::string& RootFS) {
  Result Res {};
  if (RootFS.empty()) {
    Res.Verdict = Verdict::MISSING;
    Res.Reason = "no RootFS is configured";
    return Res;
  }

  struct stat Buffer {};
  if (::stat(RootFS.c_str(), &Buffer) != 0) {
    Res.Verdict = Verdict::MISSING;
    Res.Reason = fextl::fmt::format("'{}' does not exist ({})", RootFS, std::strerror(errno));
    return Res;
  }

  if (!S_ISDIR(Buffer.st_mode)) {
    Res.Verdict = Verdict::NOT_A_DIRECTORY;
    Res.Reason = fextl::fmt::format("'{}' is not a directory (an unmounted squashfs or erofs image?)", RootFS);
    return Res;
  }

  {
    DIR* Dir = ::opendir(RootFS.c_str());
    if (!Dir) {
      Res.Verdict = Verdict::MISSING;
      Res.Reason = fextl::fmt::format("'{}' cannot be read ({})", RootFS, std::strerror(errno));
      return Res;
    }
    bool HasEntry = false;
    while (const auto* Entry = ::readdir(Dir)) {
      if (std::strcmp(Entry->d_name, ".") == 0 || std::strcmp(Entry->d_name, "..") == 0) {
        continue;
      }
      HasEntry = true;
      break;
    }
    ::closedir(Dir);
    if (!HasEntry) {
      Res.Verdict = Verdict::EMPTY;
      Res.Reason = fextl::fmt::format("'{}' is an empty directory", RootFS);
      return Res;
    }
  }

  Res.Machine = DetectMachine(RootFS, &Res.Probe);
  if (Res.Machine == EM_NONE_) {
    Res.Verdict = Verdict::UNVERIFIED;
    Res.Reason = fextl::fmt::format("'{}' has no readable /bin/sh or /usr/bin/env, so its guest "
                                    "architecture could not be checked",
                                    RootFS);
    return Res;
  }

  if (Res.Machine != EM_AARCH64_) {
    const auto* Name = MachineName(Res.Machine);
    const fextl::string Described = Name ? fextl::string {Name} : fextl::fmt::format("e_machine {}", Res.Machine);
    Res.Verdict = Verdict::WRONG_MACHINE;
    Res.Reason = fextl::fmt::format("'{}' is not an AArch64 tree: {} is {}. POWERarm runs AArch64 guests", RootFS, Res.Probe, Described);
    return Res;
  }

  Res.Verdict = Verdict::OK;
  return Res;
}
} // namespace FEX::RootFSCheck
