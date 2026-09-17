// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|syscalls-arm64
desc: stat, getdents64, fcntl and terminal ioctls for the arm64 guest
$end_info$
*/

#include "LinuxSyscalls/FileManagement.h"
#include "LinuxSyscalls/Syscalls.h"
#include "LinuxSyscalls/Arm64/ABITranslation.h"
#include "LinuxSyscalls/Arm64/Syscalls.h"

#include <FEXCore/Utils/LogManager.h>

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace FEX::HLE::Arm64 {
using namespace FEX::HLE::Arm64::ABI;

namespace {
  // glibc's struct stat on ppc64le is the kernel's (Arm64/Tests checks it), so
  // the file manager's fstatat result can be read as the generated HostStat.
  static_assert(sizeof(struct stat) == sizeof(HostStat));

  bool CopyToGuest(void* Dest, const void* Src, size_t Size) {
    return FaultSafeUserMemAccess::CopyToUser(Dest, Src, Size) == 0;
  }

  bool CopyFromGuest(void* Dest, const void* Src, size_t Size) {
    return FaultSafeUserMemAccess::CopyFromUser(Dest, Src, Size) == 0;
  }

  uint64_t StatResultToGuest(uint64_t Result, const struct stat& Host, void* GuestBuf) {
    if (Result == static_cast<uint64_t>(-1)) {
      return -errno;
    }
    GuestStat Guest {};
    HostStat Kernel {};
    memcpy(&Kernel, &Host, sizeof(Kernel));
    if (!ConvertStat(Kernel, &Guest)) {
      return -EOVERFLOW;
    }
    if (!CopyToGuest(GuestBuf, &Guest, sizeof(Guest))) {
      return -EFAULT;
    }
    return 0;
  }

  struct linux_dirent_64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
  };

  uint64_t TerminalIoctl(int fd, const IoctlEntry& Entry, uint64_t arg) {
    switch (Entry.Arg) {
    case IoctlArg::Plain: {
      uint64_t Result = ::syscall(SYSCALL_DEF(ioctl), fd, Entry.Host, arg);
      SYSCALL_ERRNO();
    }
    case IoctlArg::OpenFlags: {
      uint64_t Result = ::syscall(SYSCALL_DEF(ioctl), fd, Entry.Host, static_cast<uint64_t>(OpenFlagsToHost(arg)));
      SYSCALL_ERRNO();
    }
    case IoctlArg::TermiosGet:
    case IoctlArg::Termios2Get: {
      HostTermios Host {};
      uint64_t Result = ::syscall(SYSCALL_DEF(ioctl), fd, HOST_TCGETS, &Host);
      if (Result == static_cast<uint64_t>(-1)) {
        return -errno;
      }
      if (Entry.Arg == IoctlArg::TermiosGet) {
        GuestTermios Guest {};
        TermiosToGuest(Host, &Guest);
        return CopyToGuest(reinterpret_cast<void*>(arg), &Guest, sizeof(Guest)) ? 0 : -EFAULT;
      }
      GuestTermios2 Guest {};
      Termios2ToGuest(Host, &Guest);
      return CopyToGuest(reinterpret_cast<void*>(arg), &Guest, sizeof(Guest)) ? 0 : -EFAULT;
    }
    case IoctlArg::TermiosSet:
    case IoctlArg::Termios2Set: {
      // Start from the tty's current host termios, so the powerpc-only parts
      // (the speeds for TCSETS, unnamed c_cc slots) keep their values, just as
      // an asm-generic kernel keeps them.
      HostTermios Host {};
      uint64_t Result = ::syscall(SYSCALL_DEF(ioctl), fd, HOST_TCGETS, &Host);
      if (Result == static_cast<uint64_t>(-1)) {
        return -errno;
      }
      uint64_t HostRequest = Entry.Host;
      if (Entry.Arg == IoctlArg::TermiosSet) {
        GuestTermios Guest {};
        if (!CopyFromGuest(&Guest, reinterpret_cast<void*>(arg), sizeof(Guest))) {
          return -EFAULT;
        }
        TermiosToHost(Guest, &Host);
      } else {
        GuestTermios2 Guest {};
        if (!CopyFromGuest(&Guest, reinterpret_cast<void*>(arg), sizeof(Guest))) {
          return -EFAULT;
        }
        Termios2ToHost(Guest, &Host);
        HostRequest = Entry.Guest == GUEST_TCSETS2 ? HOST_TCSETS : Entry.Guest == GUEST_TCSETSW2 ? HOST_TCSETSW : HOST_TCSETSF;
      }
      Result = ::syscall(SYSCALL_DEF(ioctl), fd, HostRequest, &Host);
      SYSCALL_ERRNO();
    }
    case IoctlArg::Unsupported:
      // POWERARM-M1-TODO(syscalls): struct termio (TCGETA/TCSETA*), serial_struct, serial_rs485, serial_iso7816, termiox and the locked-termios requests are not translated; they answer ENOTTY.
      LogMan::Msg::DFmt("ioctl: {} is not translated for the arm64 guest", Entry.Name);
      return -ENOTTY;
    }
    return -ENOTTY;
  }
} // namespace

void RegisterFD(FEX::HLE::SyscallHandler* Handler) {
  REGISTER_SYSCALL_IMPL(
    newfstatat, [](FEXCore::Core::CpuStateFrame* Frame, int dirfd, const char* pathname, void* buf, int flag) -> uint64_t {
      GuestPath Guest_pathname(pathname, true);
      if (Guest_pathname.error()) {
        return Guest_pathname.error();
      }
      struct stat Host {};
      uint64_t Result = FEX::HLE::_SyscallHandler->FM.NewFSStatAt(dirfd, Guest_pathname.c_str(), &Host, flag);
      return StatResultToGuest(Result, Host, buf);
    });

  REGISTER_SYSCALL_IMPL(fstat, [](FEXCore::Core::CpuStateFrame* Frame, int fd, void* buf) -> uint64_t {
    struct stat Host {};
    uint64_t Result = ::fstat(fd, &Host);
    return StatResultToGuest(Result, Host, buf);
  });

  REGISTER_SYSCALL_IMPL(getdents64, [](FEXCore::Core::CpuStateFrame* Frame, int fd, void* dirp, uint32_t count) -> uint64_t {
    // linux_dirent64 is the same on both; only the RootFS entries are hidden.
    uint64_t Result = ::syscall(SYSCALL_DEF(getdents64), static_cast<uint64_t>(fd), dirp, static_cast<uint64_t>(count));
    if (Result != static_cast<uint64_t>(-1)) {
      for (size_t i = 0; i < Result;) {
        auto* Incoming = reinterpret_cast<linux_dirent_64*>(reinterpret_cast<uint64_t>(dirp) + i);
        if (FEX::HLE::_SyscallHandler->FM.IsHiddenDentry(fd, Incoming->d_ino, Incoming->d_name)) {
          const uint16_t RecLen = Incoming->d_reclen;
          memmove(Incoming, reinterpret_cast<void*>(reinterpret_cast<uint64_t>(Incoming) + RecLen), Result - i - RecLen);
          Result -= RecLen;
          continue;
        }
        i += Incoming->d_reclen;
      }
    }
    SYSCALL_ERRNO();
  });

  REGISTER_SYSCALL_IMPL(fcntl, [](FEXCore::Core::CpuStateFrame* Frame, int fd, int cmd, uint64_t arg) -> uint64_t {
    // F_* commands, lock types, struct flock and struct f_owner_ex are identical
    // on both (checked by the generator); only the file status flags differ.
    uint64_t Result {};
    switch (cmd) {
    case F_GETFL:
      Result = ::syscall(SYSCALL_DEF(fcntl), fd, cmd, arg);
      if (Result != static_cast<uint64_t>(-1)) {
        return OpenFlagsToGuest(Result);
      }
      break;
    case F_SETFL: Result = ::syscall(SYSCALL_DEF(fcntl), fd, cmd, static_cast<uint64_t>(OpenFlagsToHost(arg))); break;
    default: Result = ::syscall(SYSCALL_DEF(fcntl), fd, cmd, arg); break;
    }
    SYSCALL_ERRNO();
  });

  REGISTER_SYSCALL_IMPL(ioctl, [](FEXCore::Core::CpuStateFrame* Frame, int fd, uint64_t request, uint64_t arg) -> uint64_t {
    // The kernel takes the request as an unsigned int.
    const uint32_t GuestRequest = static_cast<uint32_t>(request);
    if (const auto* Entry = FindTerminalIoctl(GuestRequest)) {
      return TerminalIoctl(fd, *Entry, arg);
    }

    // POWERARM-M1-TODO(syscalls): ioctls beyond the terminal set only get their _IOC direction/size bits re-encoded; request-specific argument structs (DRM, input, sound, sockios, ...) are not translated.
    uint32_t HostRequest {};
    if (!IoctlRequestToHost(GuestRequest, &HostRequest)) {
      return -ENOTTY;
    }
    uint64_t Result = ::syscall(SYSCALL_DEF(ioctl), fd, static_cast<uint64_t>(HostRequest), arg);
    SYSCALL_ERRNO();
  });
}
} // namespace FEX::HLE::Arm64
