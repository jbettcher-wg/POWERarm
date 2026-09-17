// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|syscalls-arm64
desc: Signal disposition syscalls for the arm64 guest
$end_info$
*/

#include "LinuxSyscalls/SignalDelegator.h"
#include "LinuxSyscalls/Syscalls.h"
#include "LinuxSyscalls/Types.h"
#include "LinuxSyscalls/Arm64/ABITranslation.h"
#include "LinuxSyscalls/Arm64/Syscalls.h"

#include <signal.h>

namespace FEX::HLE::Arm64 {
using namespace FEX::HLE::Arm64::ABI;

namespace {
  // struct sigaction and sigset_t are identical on arm64 and powerpc (checked
  // by the generator), and so are every SA_* flag and signal number, so the
  // delegator's GuestSigAction is the arm64 layout as is.
  static_assert(sizeof(FEX::HLE::GuestSigAction) == LAYOUT_SIGACTION_SIZE);
  static_assert(sizeof(stack_t) == LAYOUT_STACK_T_SIZE);

  // kernel/signal.c do_sigaction keeps only the uapi SA_* flags (UAPI_SA_FLAGS,
  // with arm64's __ARCH_UAPI_SA_FLAGS = SA_RESTORER).
  const uint64_t GuestUapiSAFlags = GUEST_SA_NOCLDSTOP | GUEST_SA_NOCLDWAIT | GUEST_SA_SIGINFO | GUEST_SA_ONSTACK | GUEST_SA_RESTART |
                                    GUEST_SA_NODEFER | GUEST_SA_RESETHAND | GUEST_SA_EXPOSE_TAGBITS | GUEST_SA_RESTORER;
  const uint64_t GuestNSig = GUEST_U_NSIG;
} // namespace

void RegisterSignals(FEX::HLE::SyscallHandler* Handler) {
  REGISTER_SYSCALL_IMPL(
    rt_sigaction, [](FEXCore::Core::CpuStateFrame* Frame, int signum, const GuestSigAction* act, GuestSigAction* oldact, size_t sigsetsize) -> uint64_t {
      if (sigsetsize != LAYOUT_SIGSET_T_SIZE) {
        return -EINVAL;
      }
      // Same order as the kernel: copy in, then validate the signal.
      GuestSigAction New {};
      if (act && FaultSafeUserMemAccess::CopyFromUser(&New, act, sizeof(New)) != 0) {
        return -EFAULT;
      }
      if (signum < 1 || static_cast<uint64_t>(signum) > GuestNSig) {
        return -EINVAL;
      }
      if (act) {
        if (signum == SIGKILL || signum == SIGSTOP) {
          return -EINVAL;
        }
        New.sa_flags &= GuestUapiSAFlags;
        // The kernel never lets SIGKILL/SIGSTOP into a handler's mask.
        New.sa_mask.Val &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
      }
      GuestSigAction Old {};
      const uint64_t Result =
        FEX::HLE::_SyscallHandler->GetSignalDelegator()->RegisterGuestSignalHandler(signum, act ? &New : nullptr, oldact ? &Old : nullptr);
      if (Result != 0) {
        return Result;
      }
      if (oldact) {
        Old.sa_flags &= GuestUapiSAFlags;
        if (FaultSafeUserMemAccess::CopyToUser(oldact, &Old, sizeof(Old)) != 0) {
          return -EFAULT;
        }
      }
      return 0;
    });

  REGISTER_SYSCALL_IMPL(sigaltstack, [](FEXCore::Core::CpuStateFrame* Frame, const stack_t* ss, stack_t* old_ss) -> uint64_t {
    stack_t New {};
    if (ss && FaultSafeUserMemAccess::CopyFromUser(&New, ss, sizeof(New)) != 0) {
      return -EFAULT;
    }
    stack_t Old {};
    const uint64_t Result = FEX::HLE::_SyscallHandler->GetSignalDelegator()->RegisterGuestSigAltStack(
      FEX::HLE::ThreadManager::GetStateObjectFromCPUState(Frame), ss ? &New : nullptr, old_ss ? &Old : nullptr);
    if (Result != 0) {
      return Result;
    }
    if (old_ss && FaultSafeUserMemAccess::CopyToUser(old_ss, &Old, sizeof(Old)) != 0) {
      return -EFAULT;
    }
    return 0;
  });

  REGISTER_SYSCALL_IMPL(
    rt_sigtimedwait,
    [](FEXCore::Core::CpuStateFrame* Frame, uint64_t* set, siginfo_t* info, const struct timespec* timeout, size_t sigsetsize) -> uint64_t {
      return FEX::HLE::_SyscallHandler->GetSignalDelegator()->GuestSigTimedWait(set, info, timeout, sigsetsize);
    });
}
} // namespace FEX::HLE::Arm64
