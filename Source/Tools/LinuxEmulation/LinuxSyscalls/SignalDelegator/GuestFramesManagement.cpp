// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|common
desc: AArch64 guest signal frame setup and restore
$end_info$
*/

#include "LinuxSyscalls/SignalDelegator.h"
#include "ArchHelpers/UContext.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/MathUtils.h>

#include <cstring>

namespace FEX::HLE {

// Layout (growing down from NewGuestSP), mirroring arch/arm64/kernel/signal.c:
//   host stack location (FEX-private, read back by RestoreThreadState)
//   frame_record {fp, lr}
//   rt_sigframe {siginfo_t, ucontext_t}
//
// POWERARM-M0-TODO(signals): skeleton only. __reserved[] carries no fpsimd_context/esr_context records yet, SA_RESTORER vs the guest vDSO __kernel_rt_sigreturn is not handled, and sigaltstack/redzone rules have not been checked against the arm64 kernel.
uint64_t SignalDelegator::SetupFrame_Arm64(FEXCore::Core::InternalThreadState* Thread, ArchHelpers::Context::ContextBackup* ContextBackup,
                                           FEXCore::Core::CpuStateFrame* Frame, int Signal, siginfo_t* HostSigInfo, void* ucontext,
                                           GuestSigAction* GuestAction, stack_t* GuestStack, uint64_t NewGuestSP) {
  // A 32-byte block holds the frame record and, above it, the host stack slot,
  // so RestoreThreadState finds the slot at a fixed offset from the guest SP
  // that rt_sigreturn is entered with.
  NewGuestSP = FEXCore::AlignDown(NewGuestSP - 32, 16);
  uint64_t FrameRecordLocation = NewGuestSP;
  uint64_t HostStackLocation = FrameRecordLocation + sizeof(FEXCore::arm64::frame_record);

  NewGuestSP -= sizeof(FEXCore::arm64::rt_sigframe);
  uint64_t SigFrameLocation = NewGuestSP;

  static_assert(sizeof(FEXCore::arm64::rt_sigframe) % 16 == 0);
  static_assert(sizeof(FEXCore::arm64::frame_record) == 16);
  auto* SigFrame = reinterpret_cast<FEXCore::arm64::rt_sigframe*>(SigFrameLocation);
  uint64_t SigInfoLocation = SigFrameLocation + offsetof(FEXCore::arm64::rt_sigframe, info);
  uint64_t UContextLocation = SigFrameLocation + offsetof(FEXCore::arm64::rt_sigframe, uc);

  ContextBackup->FPStateLocation = 0;
  ContextBackup->UContextLocation = UContextLocation;
  ContextBackup->SigInfoLocation = SigInfoLocation;

  *reinterpret_cast<uint64_t*>(HostStackLocation) = reinterpret_cast<uint64_t>(ContextBackup);

  auto& State = Frame->State;
  auto* uc = &SigFrame->uc;
  memset(uc, 0, sizeof(*uc));

  SigFrame->info = *HostSigInfo;
  if (ContextBackup->FaultToTopAndGeneratedException) {
    SigFrame->info.si_code = Frame->SynchronousFaultData.si_code;
    SigFrame->info.si_addr = reinterpret_cast<void*>(ContextBackup->OriginalRIP);
    Signal = Frame->SynchronousFaultData.Signal;
    uc->uc_mcontext.fault_address = ContextBackup->OriginalRIP;
  } else {
    uc->uc_mcontext.fault_address = reinterpret_cast<uint64_t>(HostSigInfo->si_addr);
  }

  memcpy(uc->uc_mcontext.regs, State.x, sizeof(uc->uc_mcontext.regs));
  uc->uc_mcontext.sp = State.sp;
  uc->uc_mcontext.pc = ContextBackup->OriginalRIP;
  uc->uc_mcontext.pstate = State.nzcv;

  uc->uc_stack.ss_sp = GuestStack->ss_sp;
  uc->uc_stack.ss_flags = GuestStack->ss_flags;
  uc->uc_stack.ss_size = GuestStack->ss_size;

  auto* Record = reinterpret_cast<FEXCore::arm64::frame_record*>(FrameRecordLocation);
  Record->fp = State.x[29];
  Record->lr = State.x[30];

  // Handler arguments and return path (AAPCS64: x0-x2, lr).
  State.x[0] = Signal;
  State.x[1] = SigInfoLocation;
  State.x[2] = UContextLocation;
  State.x[29] = FrameRecordLocation;
  State.x[30] = reinterpret_cast<uint64_t>(GuestAction->restorer);

  return NewGuestSP;
}

void SignalDelegator::RestoreFrame_Arm64(FEXCore::Core::InternalThreadState* Thread, ArchHelpers::Context::ContextBackup* Context,
                                         FEXCore::Core::CpuStateFrame* Frame, void* ucontext) {
  auto* uc = reinterpret_cast<FEXCore::arm64::ucontext_t*>(Context->UContextLocation);

  if (Context->OriginalRIP == uc->uc_mcontext.pc && !Context->FaultToTopAndGeneratedException) {
    // The handler did not redirect the guest; the backed-up host context resumes it.
    return;
  }

  Frame->InSyscallInfo = Context->InSyscallInfo;

  // Resume through the dispatcher so the static registers are refilled from the frame.
  ArchHelpers::Context::SetPc(ucontext, Config.AbsoluteLoopTopAddressFillSRA);
  ArchHelpers::Context::SetFillSRASingleInst(ucontext, false);
  ArchHelpers::Context::SetState(ucontext, reinterpret_cast<uint64_t>(Frame));

  auto& State = Frame->State;
  memcpy(State.x, uc->uc_mcontext.regs, sizeof(State.x));
  State.sp = uc->uc_mcontext.sp;
  State.pc = uc->uc_mcontext.pc;
  State.nzcv = static_cast<uint32_t>(uc->uc_mcontext.pstate) & 0xF000'0000U;
  // POWERARM-M0-TODO(signals): restore FPCR/FPSR/V0-V31 from the fpsimd_context record once SetupFrame_Arm64 emits it.
}

} // namespace FEX::HLE
