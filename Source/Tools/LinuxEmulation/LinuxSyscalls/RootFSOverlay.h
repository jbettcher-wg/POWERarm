// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|common
desc: Per-user writable layer over the read-only base rootfs
$end_info$
*/

// The two-tier guest rootfs (docs/powerarm/DESIGN.md section 6.2a).
//
// A read-only base rootfs (RootFSFD in FileManager) sits under a per-user
// writable directory, by default "<rootfs>-overlay". Both are plain host
// directories and the layering is path redirection inside the emulator, so
// nothing here needs root or a kernel mount.
//
// Only the guest-owned prefixes are layered: /usr, /etc, /opt, /var/lib/pacman
// and /var/cache/pacman. Under them a lookup tries the overlay, then the base,
// then (except for package-manager state, and in a sealed process) the host
// path, and every mutation lands in the overlay: a base or host file is copied
// up before it is modified, and a deleted one is hidden with a whiteout.
// Everything else is left to FileManager's existing rootfs/host rules. The
// on-disk format is the OCI layer one: ".wh.<name>" hides <name>,
// ".wh..wh..opq" hides everything below a directory. Neither is ever visible
// to the guest.
//
// A sealed process (the guest's package manager, by default) sees only the
// overlay and the base under the guest-owned prefixes: a host file that no
// layer has does not exist for it, so installing a package that ships the
// same path as the host is not a file conflict.
//
// Every entry point returns std::nullopt when the path is not layered, which
// includes every call when no overlay directory exists, and the caller then
// runs its original code unchanged.

#pragma once

#include <FEXCore/fextl/map.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>

struct statx;

namespace FEX::HLE {

class RootFSOverlay final {
public:
  RootFSOverlay() = default;
  RootFSOverlay(const RootFSOverlay&) = delete;
  RootFSOverlay& operator=(const RootFSOverlay&) = delete;
  ~RootFSOverlay();

  // The overlay directory configured for a rootfs, or empty when there is none.
  // RootFSOverlay (POWERARM_ROOTFSOVERLAY): empty = "<RootFS>-overlay" when that
  // directory exists; "0", "off" or "none" = disabled; otherwise an absolute
  // path to an existing directory.
  static fextl::string ConfiguredPath(const fextl::string& RootFS);

  // Whether the process running ProgramName (the guest program's file name)
  // is sealed. RootFSOverlaySeal (POWERARM_ROOTFSOVERLAYSEAL): "auto" (the
  // default) seals the guest's package manager, pacman, and nothing else;
  // "on"/"1" seals every process; "off"/"0" none.
  static bool ConfiguredSeal(std::string_view ProgramName);

  // For the ELF loader, before any FileManager exists: the host path the guest
  // path GuestPath resolves to through the overlay and the base, or empty when
  // no overlay is configured or the path is not found in either layer.
  static fextl::string LoaderPath(const fextl::string& RootFS, const fextl::string& GuestPath);

  // BaseFD is FileManager's rootfs directory descriptor; it stays owned by the
  // caller. ProgramName decides the seal (ConfiguredSeal).
  void Init(const fextl::string& RootFS, int BaseFD, std::string_view ProgramName = {});

  bool Active() const {
    return UpperFD != -1;
  }
  // Host paths never show through the guest-owned prefixes.
  bool Sealed() const {
    return Seal;
  }
  const fextl::string& UpperPath() const {
    return Upper;
  }
  int UpperDirFD() const {
    return UpperFD;
  }

  // Path syscalls. Each returns nullopt when the call is not layered (the
  // caller then runs its normal code), else the syscall result: the value, or
  // uint64_t(-1) with errno set.
  std::optional<uint64_t> Openat(int DirFD, const char* Path, int Flags, uint32_t Mode);
  std::optional<uint64_t> Fstatat(int DirFD, const char* Path, struct stat* Buf, int Flags);
  std::optional<uint64_t> Statx(int DirFD, const char* Path, int Flags, uint32_t Mask, struct statx* Buf);
  std::optional<uint64_t> Faccessat(int DirFD, const char* Path, int Mode, int Flags);
  std::optional<uint64_t> Readlinkat(int DirFD, const char* Path, char* Buf, size_t Size);
  std::optional<uint64_t> Fchmodat(int DirFD, const char* Path, mode_t Mode, int Flags);
  std::optional<uint64_t> Fchownat(int DirFD, const char* Path, uid_t Owner, gid_t Group, int Flags);
  std::optional<uint64_t> Utimensat(int DirFD, const char* Path, const struct timespec* Times, int Flags);
  std::optional<uint64_t> Truncate(const char* Path, off_t Length);
  std::optional<uint64_t> Unlinkat(int DirFD, const char* Path, int Flags);
  std::optional<uint64_t> Mkdirat(int DirFD, const char* Path, mode_t Mode);
  std::optional<uint64_t> Mknodat(int DirFD, const char* Path, mode_t Mode, dev_t Dev);
  std::optional<uint64_t> Symlinkat(const char* Target, int DirFD, const char* Path);
  std::optional<uint64_t> Linkat(int OldDirFD, const char* OldPath, int NewDirFD, const char* NewPath, int Flags);
  std::optional<uint64_t> Renameat2(int OldDirFD, const char* OldPath, int NewDirFD, const char* NewPath, unsigned Flags);
  std::optional<uint64_t> Chdir(const char* Path);
  std::optional<uint64_t> Statfs(const char* Path, struct statfs* Buf);

  // Extended attributes. Reads operate on the visible copy, writes copy up.
  enum class XattrOp { Get, List, Set, Remove };
  struct XattrArgs {
    const char* Name;
    void* Value;
    size_t Size;
    int SetFlags;
  };
  std::optional<uint64_t> Xattr(XattrOp Op, int DirFD, const char* Path, bool Follow, const XattrArgs& Args);

  // A change through a descriptor (fchmod, fchown, futimens, fsetxattr,
  // fremovexattr, or the *at forms with AT_EMPTY_PATH) whose object is a base
  // or host file under a guest-owned prefix: the object is copied up and the
  // change lands on the overlay copy, so neither the base nor the host is ever
  // modified. The descriptor keeps pointing at the original object. nullopt
  // when the descriptor is not such an object (the caller changes it as
  // before). FD may be AT_FDCWD.
  enum class DescriptorOp { Chmod, Chown, Utimens, SetXattr, RemoveXattr };
  struct DescriptorArgs {
    mode_t Mode;
    uid_t Owner;
    gid_t Group;
    const struct timespec* Times;
    const char* Name;
    const void* Value;
    size_t Size;
    int SetFlags;
  };
  std::optional<uint64_t> DescriptorChange(int FD, DescriptorOp Op, const DescriptorArgs& Args);

  // execve and friends: the host path to run for an absolute guest path, or
  // nullopt when not layered. A path that is hidden (whited out, or package
  // state absent from both layers) comes back as a reserved overlay path that
  // never exists, so callers fall through as they do for a base miss.
  std::optional<fextl::string> EmulatedPath(const char* Path, bool Follow) const;
  // True when the guest must see ENOENT for this absolute path.
  bool IsHidden(const char* Path) const;

  // getdents64 on a directory descriptor that points into the overlay: the
  // merged overlay + base listing with whiteouts applied. nullopt for any
  // other descriptor.
  std::optional<uint64_t> Getdents64(int FD, void* GuestDirp, uint32_t Count);

  // getcwd: the guest path of a working directory inside the overlay.
  std::optional<uint64_t> Getcwd(char* GuestBuf, size_t Size);

  // bind/connect on an AF_UNIX pathname socket under a guest-owned prefix:
  // the socket file is created in (or found through) the layers. nullopt for
  // any other address.
  std::optional<uint64_t> SocketPath(bool Bind, int FD, const void* GuestAddr, uint32_t Len);

  // Guest path for a host path inside the overlay tree ("/usr/bin/x"), or empty.
  fextl::string StripUpperPrefix(std::string_view HostPath) const;

private:
  enum class Layer : uint8_t {
    Upper,  // exists in the overlay
    Lower,  // exists in the base
    Host,   // in neither; the host path is what the guest sees (may not exist)
    Hidden, // whited out, below an opaque directory, or package state: ENOENT
  };

  struct DirState {
    bool Upper {};       // a real directory in the overlay
    bool OpaqueKnown {}; // Opaque has been looked up
    bool Opaque {};      // ".wh..wh..opq": base and host hidden below it
    bool Lower {};       // the base's directory is visible here
    bool Fallthrough {}; // children missing from both layers fall through to the host
    // Identity of the overlay directory, for the opaque-marker cache.
    uint64_t Ino {};
    int64_t MtimeSec {};
    int64_t MtimeNsec {};
  };

  struct Node {
    fextl::string Path; // canonical guest path; for Host the unresolved remainder is appended
    Layer Where {Layer::Hidden};
    int Error {};          // resolution failure (ELOOP, ENOTDIR, EACCES, ENAMETOOLONG)
    struct stat St {};     // Upper or Lower: lstat of the resolved object
    bool Owned {};         // Path lies under a guest-owned prefix
    bool TouchedUpper {};  // resolution followed a symlink out of the overlay
    bool TrailingSlash {}; // the path named a directory ("x/")
    bool StoppedEarly {};  // a directory component was missing from both layers
    DirState Parent {};    // the parent directory, when the leaf was looked up in it
    bool HaveParent {};
    DirState Self {}; // the object itself, when it is a directory
  };

  // Absolute guest path of (DirFD, Path), or nullopt when the call cannot be
  // layered at all (empty path, unreadable descriptor, not a candidate prefix).
  std::optional<fextl::string> GuestPathOf(int DirFD, const char* Path) const;
  // True when the first component can reach a guest-owned prefix.
  bool IsCandidate(std::string_view AbsPath) const;
  // True when a name under the guest-owned path Path missing from both layers
  // is absent, rather than the host's: package-manager state always, and
  // everything in a sealed process.
  bool NoFallthrough(std::string_view Path) const;
  Node Resolve(std::string_view AbsPath, bool FollowLast) const;
  // GuestPathOf + Resolve; nullopt when the result is not under a guest-owned prefix.
  std::optional<Node> Lookup(int DirFD, const char* Path, bool FollowLast) const;

  bool Opaque(const fextl::string& Dir, DirState& S) const;

  int LayerFD(Layer L) const;
  // Relative path for LayerFD(), "." for the root.
  static const char* Rel(const fextl::string& Path);
  fextl::string HostPathOf(const Node& N) const;

  // Mutation helpers; each returns 0 or an errno value.
  int EnsureUpperDir(const fextl::string& Dir);
  int CopyUp(const Node& N, bool CopyData);
  int MakeWhiteout(const fextl::string& Path);
  bool RemoveWhiteout(const fextl::string& Path);
  int MakeOpaque(const fextl::string& Dir);
  // Whether anything below the overlay (base or host) shows at Path once the
  // overlay entry is gone, given the parent's state.
  bool LowerVisible(const fextl::string& Path, const DirState& Parent) const;
  // For rmdir: whether the merged directory holds anything but whiteouts.
  int CheckEmptyDir(const Node& N);
  fextl::string TempName(const fextl::string& Dir);

  // A copy of the node's content at the overlay path of Dest (file, symlink, or
  // empty directory), keeping mode, owner and times.
  int CopyObject(const Node& Src, const fextl::string& Dest, bool CopyData);

  uint64_t Fail(int Error) const;

  // Merged directory listings, keyed by the overlay directory's inode.
  struct DirEntry {
    uint64_t Ino;
    uint8_t Type;
    fextl::string Name;
  };
  struct DirSnapshot {
    fextl::vector<DirEntry> Entries;
  };
  bool BuildListing(const fextl::string& GuestDir, fextl::vector<DirEntry>& Out) const;
  std::mutex ListingMutex;
  fextl::map<std::pair<uint64_t, uint64_t>, DirSnapshot> Listings;

  // Opaque-marker lookups, keyed by overlay directory inode and valid while
  // its mtime is unchanged (adding or removing the marker changes it).
  struct OpaqueEntry {
    int64_t Sec;
    int64_t Nsec;
    bool Opaque;
  };
  mutable std::mutex OpaqueMutex;
  mutable fextl::map<uint64_t, OpaqueEntry> OpaqueCache;

  fextl::string Base;  // base rootfs path, no trailing '/'
  fextl::string Upper; // overlay path, no trailing '/'
  int BaseFD {-1};
  int UpperFD {-1};
  dev_t UpperDev {};
  bool Seal {};
  // First components that can lead into a guest-owned prefix: usr, etc, opt,
  // var, plus the base's root-level symlinks into them (bin, sbin, lib).
  fextl::vector<fextl::string> Candidates;
  mutable std::atomic<uint32_t> TempCounter {};
};

} // namespace FEX::HLE
