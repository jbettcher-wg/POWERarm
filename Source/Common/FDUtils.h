// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/fextl/fmt.h>

#include <fcntl.h>
#include <linux/limits.h>
#include <sys/resource.h>
#include <unistd.h>

namespace FEX {

[[nodiscard]]
inline int get_fdpath(int fd, char* SymlinkPath) {
  auto Path = fextl::fmt::format("/proc/self/fd/{}", fd);
  return readlinkat(AT_FDCWD, Path.c_str(), SymlinkPath, PATH_MAX);
}

// Moves a descriptor the emulator keeps for itself (FEXServer socket, log,
// rootfs and /proc directories) to the top of the descriptor table, below the
// RLIMIT_NOFILE soft limit, and closes the original. On Linux the guest's own
// descriptors are numbered from the lowest free slot, so emulator descriptors
// sitting at 3, 4, ... shifted every guest open (a first open returned 5
// instead of 3). Returns the new descriptor, or the original one if it cannot
// be moved (a small limit, or already high).
inline int MoveFDOutOfGuestRange(int FD) {
  if (FD < 0) {
    return FD;
  }
  struct rlimit Limit {};
  if (getrlimit(RLIMIT_NOFILE, &Limit) != 0 || Limit.rlim_cur == RLIM_INFINITY || Limit.rlim_cur < 256) {
    return FD;
  }
  const int Base = static_cast<int>(Limit.rlim_cur) - 64;
  if (FD >= Base) {
    return FD;
  }
  const bool CloExec = (fcntl(FD, F_GETFD) & FD_CLOEXEC) != 0;
  const int NewFD = fcntl(FD, CloExec ? F_DUPFD_CLOEXEC : F_DUPFD, Base);
  if (NewFD < 0) {
    return FD;
  }
  close(FD);
  return NewFD;
}

} // namespace FEX
