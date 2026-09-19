// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|common
desc: AArch64 guest signal frame setup and restore
$end_info$
*/

#include "LinuxSyscalls/SignalDelegator.h"
#include "ArchHelpers/UContext.h"
#include "LinuxSyscalls/Arm64/GeneratedABI.h"
#include "LinuxSyscalls/ThreadManager.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/MathUtils.h>

#include <cstring>

namespace FEX::HLE {

namespace {
  // FPCR and FPSR bits EL0 can set, as MSR writes them (A64Frontend
  // SystemRegisters.h, measured on the Pi 5).
  constexpr uint32_t FPCR_WRITABLE_MASK = 0x07C80000;
  constexpr uint32_t FPSR_WRITABLE_MASK = 0xF800009F;
  constexpr uint32_t FPCR_RMODE_SHIFT = 22;
  constexpr uint32_t FPCR_RMODE_MASK = 3U << FPCR_RMODE_SHIFT;
  constexpr uint64_t PSTATE_NZCV_MASK = 0xF000'0000U;

  // Whether the handler left anything but the PC different from the state the
  // frame was built from. Delivered is that state (the ContextBackup's copy).
  bool FrameEdited(const FEXCore::arm64::sigcontext& mc, const FEXCore::arm64::fpsimd_context* FPSIMD,
                   const FEXCore::Core::CPUState& Delivered) {
    if (memcmp(mc.regs, Delivered.x, sizeof(Delivered.x)) != 0 || mc.sp != Delivered.sp || ((mc.pstate ^ Delivered.nzcv) & PSTATE_NZCV_MASK) != 0) {
      return true;
    }
    return FPSIMD && (FPSIMD->fpcr != Delivered.fpcr || FPSIMD->fpsr != Delivered.fpsr ||
                      memcmp(FPSIMD->vregs, Delivered.v, sizeof(Delivered.v)) != 0);
  }

  // Everything rt_sigreturn restores from the frame: X0-X30, SP, PC, NZCV,
  // and V0-V31, FPSR and FPCR when the frame has its fpsimd_context record
  // (without one the kernel rejects the frame; here the FP state stays as it
  // was delivered).
  void LoadFrame(const FEXCore::arm64::sigcontext& mc, const FEXCore::arm64::fpsimd_context* FPSIMD, FEXCore::Core::CPUState* State) {
    memcpy(State->x, mc.regs, sizeof(State->x));
    State->sp = mc.sp;
    State->pc = mc.pc;
    State->nzcv = static_cast<uint32_t>(mc.pstate & PSTATE_NZCV_MASK);
    if (FPSIMD) {
      static_assert(sizeof(FPSIMD->vregs) == sizeof(State->v));
      memcpy(State->v, FPSIMD->vregs, sizeof(State->v));
      State->fpcr = FPSIMD->fpcr & FPCR_WRITABLE_MASK;
      State->fpsr = FPSIMD->fpsr & FPSR_WRITABLE_MASK;
    }
  }

  // The host rounding mode follows FPCR.RMode (MSR FPCR does the same through
  // SyncHostRoundingMode). Whatever resumes -- the backed-up host context or
  // the dispatcher -- runs with the FPSCR of the host frame, so it is set there.
  void SetHostRoundingMode([[maybe_unused]] void* ucontext, [[maybe_unused]] uint32_t FPCR) {
#ifdef ARCHITECTURE_ppc64le
    // RMode nearest, +Inf, -Inf, zero -> FPSCR[RN] 0, 2, 3, 1.
    static constexpr uint8_t RN[4] = {0, 2, 3, 1};
    ArchHelpers::Context::SetFPSCRRoundingMode(ucontext, RN[(FPCR & FPCR_RMODE_MASK) >> FPCR_RMODE_SHIFT]);
#endif
  }
} // namespace

// Layout (growing down from NewGuestSP), mirroring arch/arm64/kernel/signal.c:
//   handler level serial (FEX-private, checked by RestoreThreadState)
//   host stack location (FEX-private, read back by RestoreThreadState)
//   frame_record {fp, lr}
//   rt_sigframe {siginfo_t, ucontext_t}
//
// POWERARM-M0-TODO(signals): __reserved[] carries no esr_context record yet, and sigaltstack/redzone rules have not been checked against the arm64 kernel.
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

  // __reserved[]: the fpsimd_context record, which the kernel always writes
  // first, then the terminating null record (zero already). rt_sigreturn reads
  // V0-V31, FPSR and FPCR back from it.
  auto* FPSIMD = reinterpret_cast<FEXCore::arm64::fpsimd_context*>(uc->uc_mcontext.__reserved);
  FPSIMD->head.magic = FEXCore::arm64::FPSIMD_MAGIC;
  FPSIMD->head.size = sizeof(*FPSIMD);
  FPSIMD->fpsr = State.fpsr;
  FPSIMD->fpcr = State.fpcr;
  static_assert(sizeof(FPSIMD->vregs) == sizeof(State.v));
  memcpy(FPSIMD->vregs, State.v, sizeof(FPSIMD->vregs));
  ContextBackup->FPStateLocation = reinterpret_cast<uint64_t>(FPSIMD);

  // The mask rt_sigreturn restores: the one in effect before this delivery.
  uc->uc_sigmask = FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread)->SignalInfo.CurrentSignalMask.Val;

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
  // Like arch/arm64/kernel/signal.c setup_return(): SA_RESTORER wins,
  // otherwise the handler returns to the vDSO sigreturn trampoline.
  if (GuestAction->sa_flags & FEX::HLE::Arm64::ABI::GUEST_SA_RESTORER) {
    State.x[30] = reinterpret_cast<uint64_t>(GuestAction->restorer);
  } else {
    State.x[30] = reinterpret_cast<uint64_t>(VDSOPointers.VDSO_kernel_rt_sigreturn);
  }

  return NewGuestSP;
}

// rt_sigreturn restores everything the handler left in the signal frame
// (LoadFrame), whether or not it moved the PC. RestoreThreadState has already
// put back the interrupted host context and the guest state as delivered; what
// happens next depends on what the handler changed and where the signal
// arrived.
//
//  - Nothing: the backed-up host context resumes exactly where it was. This is
//    the common case (profilers, GC suspend signals, most async handlers), and
//    it costs one comparison of the frame with the backup.
//
//  - Registers but not the PC, and the signal arrived outside JIT code: in a
//    guest syscall, a thunk's host call, or one of the dispatcher's calls into
//    C++. That host code reloads the guest state from CPUState before it runs
//    guest code again (DEF_OP(Syscall) and the thunk crossings refill every
//    static register once a delivery has cleared their sentinel; the
//    dispatcher refills after its calls), so the frame goes into CPUState and
//    the host context resumes, which also lets the syscall complete. The
//    syscall writes its result to X0 on the way out, so an edited X0 goes to
//    HandleSyscall to return instead: on arm64 Linux the result is already in
//    X0 when the frame is built, and a handler's value replaces it.
//
//  - Registers but not the PC, and the signal arrived in JIT code (a fault, or
//    a signal drained at an interrupt-page poke): the guest state lives in
//    host registers as the translation left it -- the static registers, NZCV
//    in CR0/XER, values the block derived from them -- and cannot be patched
//    there. Resume through the dispatcher at the frame's PC instead, like a
//    new PC: it refills every static register and NZCV from CPUState and
//    enters a translation that starts at that instruction. That is exact
//    because a faulting A64 translation has written no register yet (memory
//    is accessed first, TranslateLoadStore.cpp), and a drained signal is
//    reported at the instruction after the drain point, so the frame's PC is
//    where hardware would resume.
//
//  - A new PC, or a synthesized fault (UDF, BRK and the like, raised from a
//    dispatcher stub that would only fault again): the same dispatcher resume.
//
// Returns true when the guest resumes through the dispatcher.
bool SignalDelegator::RestoreFrame_Arm64(FEXCore::Core::InternalThreadState* Thread, ArchHelpers::Context::ContextBackup* Context,
                                         FEXCore::Core::CpuStateFrame* Frame, void* ucontext, bool InGuestSyscall) {
  const auto* uc = reinterpret_cast<const FEXCore::arm64::ucontext_t*>(Context->UContextLocation);
  const auto& mc = uc->uc_mcontext;
  const auto* FPSIMD = FEXCore::arm64::FindFPSIMDContext(mc);
  auto& State = Frame->State;
  const bool SamePC = mc.pc == Context->OriginalRIP && !Context->FaultToTopAndGeneratedException;

  if (SamePC && !FrameEdited(mc, FPSIMD, Context->GuestState)) {
    return false;
  }

  LoadFrame(mc, FPSIMD, &State);
  if ((State.fpcr ^ Context->GuestState.fpcr) & FPCR_RMODE_MASK) {
    SetHostRoundingMode(ucontext, State.fpcr);
  }

  if (SamePC && !(Context->Flags & ArchHelpers::Context::ContextFlags::CONTEXT_FLAG_INJIT)) {
    if (InGuestSyscall && State.x[0] != Context->GuestState.x[0]) {
      auto& SignalInfo = FEX::HLE::ThreadManager::GetStateObjectFromFEXCoreThread(Thread)->SignalInfo;
      SignalInfo.HasSyscallResultOverride = true;
      SignalInfo.SyscallResultOverride = State.x[0];
    }
    return false;
  }

  Frame->InSyscallInfo = Context->InSyscallInfo;

  // Resume through the dispatcher so the static registers are refilled from the frame.
  ArchHelpers::Context::SetPc(ucontext, Config.AbsoluteLoopTopAddressFillSRA);
  ArchHelpers::Context::SetFillSRASingleInst(ucontext, false);
  ArchHelpers::Context::SetState(ucontext, reinterpret_cast<uint64_t>(Frame));
  return true;
}

void SignalDelegator::LoadFrameForDispatcher_Arm64(FEXCore::Core::CpuStateFrame* Frame, const FEXCore::arm64::ucontext_t* uc, void* ucontext) {
  LoadFrame(uc->uc_mcontext, FEXCore::arm64::FindFPSIMDContext(uc->uc_mcontext), &Frame->State);
  SetHostRoundingMode(ucontext, Frame->State.fpcr);
}

} // namespace FEX::HLE
