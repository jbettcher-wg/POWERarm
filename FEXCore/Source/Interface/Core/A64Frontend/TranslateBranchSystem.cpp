// SPDX-License-Identifier: MIT
//
// A64 branches, exception generation and the EL0-visible system instructions.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/SystemRegisters.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"
#include "Interface/Context/Context.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>

namespace FEXCore::A64 {
using namespace FEXCore::IR;

// ---------------------------------------------------------------------------
// Branches
// ---------------------------------------------------------------------------

bool IRBuilder::B_uncond(uint32_t Word) {
  ExitToPC(CurrentPC + SignExtend(Bits(Word, 25, 0), 26) * 4);
  return true;
}

bool IRBuilder::BL(uint32_t Word) {
  const uint64_t Target = CurrentPC + SignExtend(Bits(Word, 25, 0), 26) * 4;
  StoreX(30, PCValue(CurrentPC + INSTRUCTION_SIZE));
  if (JumpTargets.contains(Target)) {
    // A call to this unit's own entry stays an in-unit jump.
    ExitToPC(Target);
  } else {
    ExitCall(_InlineEntrypointOffset(OpSize::i64Bit, Target - Entry),
             _InlineEntrypointOffset(OpSize::i64Bit, CurrentPC + INSTRUCTION_SIZE - Entry));
    BlockSetPC = true;
  }
  return true;
}

bool IRBuilder::B_cond(uint32_t Word) {
  const uint64_t Target = CurrentPC + SignExtend(Bits(Word, 23, 5), 19) * 4;
  const uint32_t Cond = Bits(Word, 3, 0);
  if (Cond >= 0b1110) {
    ExitToPC(Target);
    return true;
  }

  auto Jump = _CondJump(InvalidNode, InvalidNode, InvalidNode, InvalidNode, MapCondition(Cond), OpSize::iInvalid, true);
  EmitConditionalExit(Jump, Target);
  return true;
}

bool IRBuilder::CompareBranch(uint32_t Word, bool IsNonZero) {
  const bool Is64 = Bit(Word, 31);
  const uint64_t Target = CurrentPC + SignExtend(Bits(Word, 23, 5), 19) * 4;
  Ref Value = LoadX(Bits(Word, 4, 0));
  auto Jump = _CondJump(Value, Constant(0), InvalidNode, InvalidNode, IsNonZero ? CondClass::NEQ : CondClass::EQ, SizeFor(Is64));
  EmitConditionalExit(Jump, Target);
  return true;
}

bool IRBuilder::CBZ(uint32_t Word) {
  return CompareBranch(Word, false);
}
bool IRBuilder::CBNZ(uint32_t Word) {
  return CompareBranch(Word, true);
}

bool IRBuilder::TestBranch(uint32_t Word, bool IsNonZero) {
  const uint32_t BitNumber = (Bits(Word, 31, 31) << 5) | Bits(Word, 23, 19);
  const uint64_t Target = CurrentPC + SignExtend(Bits(Word, 18, 5), 14) * 4;
  Ref Value = LoadX(Bits(Word, 4, 0));
  // The TSTZ/TSTNZ lowering requires the bit position as an inline constant.
  auto Jump = _CondJump(Value, _InlineConstant(BitNumber), InvalidNode, InvalidNode, IsNonZero ? CondClass::TSTNZ : CondClass::TSTZ,
                        OpSize::i64Bit);
  EmitConditionalExit(Jump, Target);
  return true;
}

bool IRBuilder::TBZ(uint32_t Word) {
  return TestBranch(Word, false);
}
bool IRBuilder::TBNZ(uint32_t Word) {
  return TestBranch(Word, true);
}

bool IRBuilder::BranchRegister(uint32_t Word, BranchHint Hint) {
  // Hints drive the backend's call/return pairing: BL/BLR exit with Call and
  // the X30 value, RET with Return. A RET's target is still the register's
  // value, whatever the pairing predicted (see DEF_OP(ExitFunction)).
  Ref Target = LoadX(Bits(Word, 9, 5));
  if (Hint == BranchHint::Call) {
    StoreX(30, PCValue(CurrentPC + INSTRUCTION_SIZE));
    ExitCall(Target, _InlineEntrypointOffset(OpSize::i64Bit, CurrentPC + INSTRUCTION_SIZE - Entry));
  } else {
    ExitFunction(Target, Hint);
  }
  BlockSetPC = true;
  return true;
}

bool IRBuilder::BR(uint32_t Word) {
  return BranchRegister(Word, BranchHint::None);
}
bool IRBuilder::BLR(uint32_t Word) {
  return BranchRegister(Word, BranchHint::Call);
}
bool IRBuilder::RET(uint32_t Word) {
  return BranchRegister(Word, BranchHint::Return);
}

// ---------------------------------------------------------------------------
// Exception generation
// ---------------------------------------------------------------------------

bool IRBuilder::SVC(uint32_t Word) {
  // The W1/W3 seam: syscall number in X8, arguments in X0-X5, result in X0.
  // Linux ignores the SVC immediate.
  //
  // Before the handler runs, State.pc holds the address after the SVC, which
  // is what the arm64 kernel records in ELR_EL1: a clone child resumes there,
  // and execve and signal frames built during the call read it.
  const uint64_t NextPC = CurrentPC + INSTRUCTION_SIZE;
  _StoreContext(OpSize::i64Bit, RegClass::GPR, PCValue(NextPC), offsetof(FEXCore::Core::CPUState, pc));

  // The Syscall lowering spills every static register (and NZCV) into
  // CPUState before calling the handler and stores the result in State.x[0].
  _Syscall(LoadX(8), LoadX(0), LoadX(1), LoadX(2), LoadX(3), LoadX(4), LoadX(5));

  // CPUState is authoritative after the handler: it may have rewritten any
  // register or pc. The lowering's refill skips the static registers held in
  // non-volatile host registers unless a signal intervened, so reload every
  // static register here, then resume at State.pc rather than a constant.
  // (rt_sigreturn does not return here at all.)
  for (uint32_t Reg : FEXCore::Core::StaticGPRGuestReg) {
    StoreXSP(Reg, _LoadContext(OpSize::i64Bit, RegClass::GPR, FEXCore::Core::CPUState::GPROffset(Reg)));
  }
  ExitFunction(_LoadContext(OpSize::i64Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, pc)));
  BlockSetPC = true;
  return true;
}

bool IRBuilder::BRK(uint32_t) {
  RaiseGuestSignal(CurrentPC, BreakDefinition {
                                .ErrorRegister = 0,
                                .Signal = FEXCore::Core::FAULT_SIGTRAP,
                                .TrapNumber = 0,
                                .si_code = 1, ///< TRAP_BRKPT
                              });
  return true;
}

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

bool IRBuilder::HINT(uint32_t) {
  // NOP, YIELD, WFE, WFI, SEV, SEVL, BTI, PACIASP and the rest of the hint
  // space, plus ISB: no guest-visible effect at EL0 here. DSB and DMB used to
  // land here too -- see Barrier() for why that was wrong.
  return true;
}

// DMB and DSB.
//
// These were NOPs until the Claude Code installer wedged: every guest thread
// parked in an untimed futex on a word that no longer changed, because a
// lock-free handoff had lost its wakeup. AArch64 and PPC64 are both weakly
// ordered, but not equally: a guest DMB ISH that the JIT drops leaves the host
// free to reorder exactly the accesses the guest asked it not to, and every
// lock-free wakeup protocol in glibc, JSC and Bun's thread pool is built on
// them. Dropping them is only safe on a host at least as strongly ordered as
// the guest, which POWER9 is not.
//
// CRm bits 1:0 are the access types the barrier orders, and map onto what the
// PPC64 backend emits for each FenceType (MemoryOps.cpp):
//   0b01 (LD): loads before -> loads and stores after.   lwsync (Acquire)
//   0b10 (ST): stores before -> stores after.            lwsync
//   else (SY): everything before -> everything after.    hwsync
// The domain (CRm bits 3:2 -- OSH/NSH/ISH/SY) is not distinguished: every
// domain is at least inner-shareable as far as a guest thread on this host can
// observe, so the widest reading is the correct one.
bool IRBuilder::Barrier(uint32_t Word) {
  switch (Bits(Word, 9, 8)) {
  case 0b01: _Fence(IR::FenceType::Acquire); break;
  case 0b10: _Fence(IR::FenceType::Store); break;
  default: _Fence(IR::FenceType::LoadStore); break;
  }
  return true;
}

bool IRBuilder::CLREX(uint32_t) {
  _StoreContext(OpSize::i8Bit, RegClass::GPR, Constant(0), offsetof(FEXCore::Core::CPUState, excl_valid));
  return true;
}

bool IRBuilder::UnallocatedEncoding(uint32_t) {
  return false;
}

bool IRBuilder::DC_ZVA(uint32_t Word) {
  // DCZID_EL0 reports a 64-byte block (SystemRegisters.h); CacheLineZero
  // zeroes 64 bytes from the address rounded down to 64.
  _CacheLineZero(LoadX(Bits(Word, 4, 0)));
  return true;
}

bool IRBuilder::CacheMaintenanceNop(uint32_t) {
  // DC CVAU and IC IVAU: cache flushes for code the guest wrote. SMC tracking
  // (mtrack) already invalidates translations of written pages.
  return true;
}

namespace {
  constexpr uint32_t SysReg(uint32_t Op0, uint32_t Op1, uint32_t CRn, uint32_t CRm, uint32_t Op2) {
    return (Op0 << 14) | (Op1 << 11) | (CRn << 7) | (CRm << 3) | Op2;
  }

  // op0:op1:CRn:CRm:op2 from an MRS/MSR (register) word.
  constexpr uint32_t SysRegOf(uint32_t Word) {
    return ((2 + Bit(Word, 19)) << 14) | (Bits(Word, 18, 16) << 11) | (Bits(Word, 15, 12) << 7) | (Bits(Word, 11, 8) << 3) |
           Bits(Word, 7, 5);
  }

  constexpr uint32_t REG_NZCV = SysReg(3, 3, 4, 2, 0);
  constexpr uint32_t REG_FPCR = SysReg(3, 3, 4, 4, 0);
  constexpr uint32_t REG_FPSR = SysReg(3, 3, 4, 4, 1);
  constexpr uint32_t REG_TPIDR_EL0 = SysReg(3, 3, 13, 0, 2);
  constexpr uint32_t REG_TPIDRRO_EL0 = SysReg(3, 3, 13, 0, 3);
  constexpr uint32_t REG_CTR_EL0 = SysReg(3, 3, 0, 0, 1);
  constexpr uint32_t REG_DCZID_EL0 = SysReg(3, 3, 0, 0, 7);
  // Generic timer, EL0-readable half. Exactly these two: on the Pi, CNTPCT_EL0,
  // CNTVCTSS_EL0/CNTPCTSS_EL0 (FEAT_ECV, which the A76 lacks and our ID
  // registers agree it lacks) and CNTKCTL_EL1 all SIGILL from EL0, so they must
  // stay absent here to fault the same way.
  constexpr uint32_t REG_CNTFRQ_EL0 = SysReg(3, 3, 14, 0, 0);
  constexpr uint32_t REG_CNTVCT_EL0 = SysReg(3, 3, 14, 0, 2);
} // namespace

bool IRBuilder::MRS(uint32_t Word) {
  const uint32_t Reg = SysRegOf(Word);
  const uint32_t Rt = Bits(Word, 4, 0);

  switch (Reg) {
  case REG_NZCV: StoreX(Rt, _LoadNZCV()); return true;
  case REG_FPCR: StoreW(Rt, _LoadContext(OpSize::i32Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, fpcr))); return true;
  case REG_FPSR: StoreW(Rt, _LoadContext(OpSize::i32Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, fpsr))); return true;
  case REG_TPIDR_EL0: StoreX(Rt, _LoadContext(OpSize::i64Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, tpidr_el0))); return true;
  case REG_TPIDRRO_EL0:
    StoreX(Rt, _LoadContext(OpSize::i64Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, tpidrro_el0)));
    return true;
  case REG_CTR_EL0: StoreX(Rt, Constant(SystemRegisters::CTR_EL0)); return true;
  case REG_DCZID_EL0: StoreX(Rt, Constant(SystemRegisters::DCZID_EL0)); return true;

  // CNTVCT_EL0, the virtual counter. V8 reads it as its high-resolution clock,
  // which is where code-server's Node dies without it. The PPC64 lowering of
  // CycleCounter is `mftb`, the host timebase, which is a free-running upcount
  // at a fixed rate exactly like the ARM virtual counter.
  //
  // SelfSynchronizingLoads=false deliberately: that flag makes the lowering
  // emit `isync` first, which is the RDTSCP ordering guarantee, not this one.
  // Plain CNTVCT_EL0 carries no ordering requirement -- a guest that needs one
  // issues ISB itself -- and paying for a pipeline flush on every clock read in
  // a JIT's hot timing path would be a real cost for nothing. It is also why
  // CNTVCTSS_EL0, the self-synchronising form, staying unimplemented is both
  // faithful to the A76 and the cheap answer.
  case REG_CNTVCT_EL0: StoreX(Rt, _CycleCounter(false)); return true;

  // CNTFRQ_EL0: the rate CNTVCT ticks at, so it must describe `mftb` -- the
  // real host timebase, not the Pi's 54 MHz. CNTFRQ is a board property rather
  // than a CPU feature (real arm64 hardware ranges from 24 MHz to 1 GHz) and
  // every correct guest divides by whatever it reads, so reporting the truth is
  // both honest and free; faking the Pi's value would mean a multiply-shift on
  // every counter read just to keep the pair self-consistent. This is the one
  // place the A64 system-register surface deliberately does NOT model the Pi --
  // see the note in SystemRegisters.h.
  //
  // The frequency is baked in as a constant, so it is captured in cached code.
  // That is sound here because it is a property of the host the cache is keyed
  // on, and architecturally 512 MHz on every POWER8 and later part.
  //
  // A host that cannot report a frequency leaves this unimplemented rather than
  // returning zero: zero is what a guest divides by.
  case REG_CNTFRQ_EL0:
    if (!CTX->CycleCounterFrequency) {
      return false;
    }
    StoreX(Rt, Constant(CTX->CycleCounterFrequency));
    return true;
  default: break;
  }

  // The ID register space Linux emulates for EL0 (arch/arm64/kernel/cpufeature.c,
  // emulate_sys_reg): op0=3, op1=0, CRn=0, CRm=0 or 2..7. Unknown registers in
  // CRm 2..7 read as zero; in CRm 0 only MIDR, MPIDR and REVIDR exist.
  const uint32_t Op0 = Reg >> 14;
  const uint32_t Op1 = (Reg >> 11) & 7;
  const uint32_t CRn = (Reg >> 7) & 15;
  const uint32_t CRm = (Reg >> 3) & 15;
  const uint32_t Op2 = Reg & 7;
  if (Op0 != 3 || Op1 != 0 || CRn != 0 || CRm == 1) {
    return false;
  }

  uint64_t Value {};
  if (!SystemRegisters::ReadIDRegister(CRm, Op2, &Value)) {
    return false;
  }
  StoreX(Rt, Constant(Value));
  return true;
}

bool IRBuilder::MSR_reg(uint32_t Word) {
  const uint32_t Reg = SysRegOf(Word);
  const uint32_t Rt = Bits(Word, 4, 0);

  switch (Reg) {
  case REG_NZCV:
    // StoreNZCV reads bits 31:28; the rest of the word is RES0.
    _StoreNZCV(LoadX(Rt));
    return true;
  case REG_FPCR: {
    Ref FPCR = _And(OpSize::i64Bit, LoadX(Rt), Constant(SystemRegisters::FPCR_WRITABLE_MASK));
    _StoreContext(OpSize::i32Bit, RegClass::GPR, FPCR, offsetof(FEXCore::Core::CPUState, fpcr));
    SyncHostRoundingMode(FPCR);
    return true;
  }
  case REG_FPSR:
    // POWERARM-M1-TODO(fpu): FPSR is stored only. The cumulative exception bits (IOC, DZC, OFC, UFC, IXC, IDC) are not raised by FP operations and QC is not raised by saturating operations; mapping FPSCR's sticky bits (VX, ZX, OX, UX, XX) needs the JIT's own FP use kept out of FPSCR.
    _StoreContext(OpSize::i32Bit, RegClass::GPR, _And(OpSize::i64Bit, LoadX(Rt), Constant(SystemRegisters::FPSR_WRITABLE_MASK)),
                  offsetof(FEXCore::Core::CPUState, fpsr));
    return true;
  case REG_TPIDR_EL0: _StoreContext(OpSize::i64Bit, RegClass::GPR, LoadX(Rt), offsetof(FEXCore::Core::CPUState, tpidr_el0)); return true;
  default:
    // TPIDRRO_EL0 and every ID register are read-only at EL0.
    return false;
  }
}

} // namespace FEXCore::A64
