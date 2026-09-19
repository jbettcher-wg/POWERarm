// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|syscalls-shared
$end_info$
*/

#include "LinuxSyscalls/Syscalls.h"
#include "LinuxSyscalls/Arm64/Syscalls.h"

#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXHeaderUtils/Syscalls.h>

#include <cstring>
#include <linux/kcmp.h>
#include <linux/seccomp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <syslog.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/klog.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <unistd.h>

#include <git_version.h>

namespace FEX::HLE {
using cap_user_header_t = void*;
using cap_user_data_t = void*;

// RLIMIT_AS of the calling process goes through
// SyscallHandler::GuestAddressSpaceLimit, which holds it until the next
// execve; every other resource, and every other process, is the host's. Like
// the kernel, a new limit is copied in first and the old one copied out after
// it is set (so a bad old pointer is EFAULT with the new limit in place).
static uint64_t AddressSpaceLimit(const struct rlimit* GuestNew, struct rlimit* GuestOld) {
  struct rlimit New {};
  struct rlimit Old {};
  if (GuestNew && FaultSafeUserMemAccess::CopyFromUser(&New, GuestNew, sizeof(New)) != 0) {
    return -EFAULT;
  }
  const int Result = FEX::HLE::_SyscallHandler->GuestAddressSpaceLimit(GuestNew ? &New : nullptr, GuestOld ? &Old : nullptr);
  if (Result != 0) {
    return Result;
  }
  if (GuestOld && FaultSafeUserMemAccess::CopyToUser(GuestOld, &Old, sizeof(Old)) != 0) {
    return -EFAULT;
  }
  return 0;
}

// prlimit64's pid names this process when it is 0, the process id, or the id
// of any of its threads (rlimits belong to the thread group).
static bool IsThisProcess(pid_t Pid) {
  if (Pid == 0 || Pid == ::getpid() || Pid == static_cast<pid_t>(FHU::Syscalls::gettid())) {
    return true;
  }
  if (Pid < 0) {
    return false;
  }
  char TaskPath[64];
  snprintf(TaskPath, sizeof(TaskPath), "/proc/self/task/%d", Pid);
  return ::access(TaskPath, F_OK) == 0;
}

void RegisterInfo(FEX::HLE::SyscallHandler* Handler) {
  using namespace FEXCore::IR;

  REGISTER_SYSCALL_IMPL(uname, [](FEXCore::Core::CpuStateFrame* Frame, struct utsname* GuestBuf) -> uint64_t {
    auto Thread = FEX::HLE::ThreadManager::GetStateObjectFromCPUState(Frame);

    // Verify writability before doing any strcpy/memcpy into the guest buffer.
    // A bad guest pointer would otherwise cause a host SEGV in the first write
    // (the kernel returns -EFAULT in that case, which is what we want to mimic).
    // Filled in host memory and copied out once: a bad pointer is EFAULT.
    struct utsname LocalBuf {};
    auto* buf = &LocalBuf;

    struct utsname Local {};
    if (::uname(&Local) == 0) {
      memcpy(buf->nodename, Local.nodename, sizeof(Local.nodename));
      static_assert(sizeof(Local.nodename) <= sizeof(buf->nodename));
      memcpy(buf->domainname, Local.domainname, sizeof(Local.domainname));
      static_assert(sizeof(Local.domainname) <= sizeof(buf->domainname));
    } else {
      strcpy(buf->nodename, "POWERarm");
      LogMan::Msg::EFmt("Couldn't determine host nodename. Defaulting to '{}'", buf->nodename);
    }
    strcpy(buf->sysname, "Linux");
    uint32_t GuestVersion = FEX::HLE::_SyscallHandler->GetGuestKernelVersion();
    if (Thread->persona & UNAME26) {
      // Kernel version converts from 6.x.y to 2.6.60+x.
      GuestVersion = FEX::HLE::SyscallHandler::KernelVersion(2, 6, 60 + FEX::HLE::SyscallHandler::KernelMinor(GuestVersion));
    }
    snprintf(buf->release, sizeof(buf->release), "%d.%d.%d", FEX::HLE::SyscallHandler::KernelMajor(GuestVersion),
             FEX::HLE::SyscallHandler::KernelMinor(GuestVersion), FEX::HLE::SyscallHandler::KernelPatch(GuestVersion));

    const char version[] = "#" GIT_DESCRIBE_STRING " SMP " __DATE__ " " __TIME__;
    strcpy(buf->version, version);
    static_assert(sizeof(version) <= sizeof(buf->version), "uname version define became too large!");
    // An arm64 kernel reports "armv8l" under PER_LINUX32 only when the CPU
    // runs AArch32 at EL0; the presented CPU does not, and personality()
    // refuses PER_LINUX32, so this is always "aarch64".
    strcpy(buf->machine, "aarch64");
    if (FaultSafeUserMemAccess::CopyToUser(GuestBuf, buf, sizeof(*buf)) != 0) {
      return -EFAULT;
    }
    return 0;
  });

  REGISTER_SYSCALL_IMPL(personality, [](FEXCore::Core::CpuStateFrame* Frame, uint32_t persona) -> uint64_t {
    auto Thread = FEX::HLE::ThreadManager::GetStateObjectFromCPUState(Frame);

    if (persona == ~0U) {
      // Special case, only queries the persona.
      return Thread->persona;
    }

    // arm64_personality: PER_LINUX32 needs AArch32 at EL0, which the presented
    // CPU does not have.
    if ((persona & PER_MASK) == PER_LINUX32) {
      return -EINVAL;
    }

    (void)::syscall(SYSCALL_DEF(personality), persona);

    // Return the old persona while setting the new one.
    auto OldPersona = Thread->persona;
    Thread->persona = persona;
    return OldPersona;
  });

  REGISTER_SYSCALL_IMPL(seccomp, [](FEXCore::Core::CpuStateFrame* Frame, unsigned int operation, unsigned int flags, void* args) -> uint64_t {
    return FEX::HLE::_SyscallHandler->SeccompEmulator.Handle(Frame, operation, flags, args);
  });
  REGISTER_SYSCALL_IMPL(
    ptrace, [](FEXCore::Core::CpuStateFrame* Frame, int /*enum __ptrace_request*/ request, pid_t pid, void* addr, void* data) -> uint64_t {
      uint64_t Result {};

      switch (request) {
      case PTRACE_PEEKTEXT:
      case PTRACE_PEEKDATA:
      case PTRACE_POKETEXT:
      case PTRACE_POKEDATA:
      case PTRACE_ATTACH:
      case PTRACE_DETACH:
        // Passthrough these requests. Allows Wine to run the Ubisoft launcher.
        Result = ::syscall(SYSCALL_DEF(ptrace), request, pid, addr, data);
        // POKETEXT / POKEDATA writes guest bytes with no fault path FEX can
        // observe — the kernel services them via FOLL_FORCE, which writes
        // through PROT_READ so mtrack cannot see the store even in principle.
        // If the tracee is in this same address space (our own tgid), a
        // block compiled from the poked word continues executing the pre-poke
        // translation. Invalidate the word (host quadword: 8 bytes on 64-bit,
        // 4 bytes on 32-bit) after a successful POKE. Cross-process POKE is
        // unclosable: the tracee's translations live in the tracee's FEX,
        // which our FEX has no handle to.
        if (Result != static_cast<uint64_t>(-1) && (request == PTRACE_POKETEXT || request == PTRACE_POKEDATA)) {
          // Same-tgid check: /proc/self/task/<pid> exists iff pid is one of
          // our own threads (same address space). Avoids syscall overhead of
          // reading /proc/<pid>/status Tgid.
          char task_path[64];
          snprintf(task_path, sizeof(task_path), "/proc/self/task/%d", pid);
          if (access(task_path, F_OK) == 0 || pid == 0) {
            const size_t Word = sizeof(long);
            FEX::HLE::_SyscallHandler->InvalidateCodeRangeIfNecessary(Frame->Thread, reinterpret_cast<uint64_t>(addr), Word);
          }
        }
        SYSCALL_ERRNO();
      default: break;
      }
      // We don't support this
      return -EPERM;
    });

  REGISTER_SYSCALL_IMPL(getrlimit, [](FEXCore::Core::CpuStateFrame* Frame, uint32_t resource, struct rlimit* rlim) -> uint64_t {
    if (resource == RLIMIT_AS) {
      return rlim ? AddressSpaceLimit(nullptr, rlim) : -EFAULT;
    }
    uint64_t Result = ::syscall(SYSCALL_DEF(getrlimit), resource, rlim);
    SYSCALL_ERRNO();
  });
  REGISTER_SYSCALL_IMPL(setrlimit, [](FEXCore::Core::CpuStateFrame* Frame, uint32_t resource, const struct rlimit* rlim) -> uint64_t {
    if (resource == RLIMIT_AS) {
      return rlim ? AddressSpaceLimit(rlim, nullptr) : -EFAULT;
    }
    uint64_t Result = ::syscall(SYSCALL_DEF(setrlimit), resource, rlim);
    SYSCALL_ERRNO();
  });
  REGISTER_SYSCALL_IMPL(prlimit_64, [](FEXCore::Core::CpuStateFrame* Frame, pid_t pid, uint32_t resource, const struct rlimit* new_limit,
                                       struct rlimit* old_limit) -> uint64_t {
    if (resource == RLIMIT_AS && IsThisProcess(pid)) {
      return AddressSpaceLimit(new_limit, old_limit);
    }
    uint64_t Result = ::syscall(SYSCALL_DEF(prlimit_64), pid, resource, new_limit, old_limit);
    SYSCALL_ERRNO();
  });
}
} // namespace FEX::HLE
