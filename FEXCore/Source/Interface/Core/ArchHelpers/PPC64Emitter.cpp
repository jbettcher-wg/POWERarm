// SPDX-License-Identifier: MIT
// PPC64LE emitter helper implementation.
#include "Interface/Core/ArchHelpers/PPC64Emitter.h"
#include "Interface/Context/Context.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Utils/LogManager.h>

#include <array>
#include <cstddef>

namespace FEXCore::CPU {

PPC64EmitterBase::PPC64EmitterBase(FEXCore::Context::ContextImpl* ctx,
                                   void* EmitPtr, size_t Size)
  : EmitterCTX(ctx) {
  if (EmitPtr) {
    SetBuffer(static_cast<uint8_t*>(EmitPtr), Size);
  }
}

void PPC64EmitterBase::LoadConstant(GPR rt, uint64_t Constant) {
  LoadImm64(rt, Constant);
}

void PPC64EmitterBase::LoadConstantFixed(GPR rt, uint64_t Constant) {
  LoadImm64Fixed(rt, Constant);
}

// Align to 16-byte boundary with NOPs
void PPC64EmitterBase::Align16B() {
  while ((GetOffset() & 0xF) != 0) {
    nop();
  }
}

// Spill static (SRA) registers from host regs → CpuStateFrame
void PPC64EmitterBase::SpillStaticRegs(GPR tmp) {
  // SRA[i] holds the host-side register dedicated to static slot i. The RA pass
  // emits StoreRegister/LoadRegister with PhysicalRegister.Reg = slot index,
  // and slot i holds guest register FEXCore::Core::StaticGPRGuestReg[i], whose
  // context home is CPUState::GPROffset().
  const auto& SRA = std::span<const GPR>(a64::SRA);
  for (size_t i = 0; i < SRA.size(); ++i) {
    int32_t off = static_cast<int32_t>(offsetof(FEXCore::Core::CpuStateFrame, State) +
                                       FEXCore::Core::CPUState::GPROffset(FEXCore::Core::StaticGPRGuestReg[i]));
    if (off >= -32768 && off <= 32764 && (off & 3) == 0) {
      std(SRA[i], off, STATE);
    } else {
      LoadImm32(tmp, static_cast<uint32_t>(off));
      stdx(SRA[i], STATE, tmp);
    }
  }

  // Spill SRA FPRs (V0-V15) to State.v
  const auto& SRAFPR = std::span<const VR>(a64::SRAFPR);
  for (size_t i = 0; i < SRAFPR.size(); ++i) {
    int32_t v_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, State.v[i][0]));
    LoadImm32(tmp, static_cast<uint32_t>(v_off));
    stvx(SRAFPR[i], STATE, tmp);
  }

  // Spill pinned VSX low bank (V16-V31 in vs16-vs31) to State.v[16..31]
  for (size_t i = 16; i < 32; ++i) {
    int32_t v_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, State.v[i][0]));
    LoadImm32(tmp, static_cast<uint32_t>(v_off));
    if (EmitterCTX->HostFeatures.SupportsISA30) {
      stxvx(PPC64Emitter::vsx(i), STATE, tmp);
    } else {
      xxpermdi(VTMP3_VSX, PPC64Emitter::vsx(i), PPC64Emitter::vsx(i), 2);
      stxvd2x(VTMP3_VSX, STATE, tmp);
    }
  }

  // Spill CALLRET_SP to State.callret_sp
  int32_t callret_sp_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, State.callret_sp));
  std(a64::CALLRET_SP, static_cast<int16_t>(callret_sp_off), STATE);

  // Save NZCV across the dispatcher / C++ slow paths. Pack CR0 + XER into the
  // ARM-style 32-bit NZCV layout (N=LSB31, Z=30, C=29, V=28) and store at
  // State.nzcv. Mirrors DEF_OP(LoadNZCV) bit shuffles.
  // ARM64 doesn't need this because PSTATE.NZCV is a hardware register that
  // survives across the dispatcher's C++ calls; PPC's CR0 + XER do not.
  //
  // Every ppc64le caller passes tmp = TMP1. tmp aliases scratch we used in the
  // SRA loop above, so we need two non-aliased scratches (TMP2/TMP3) for the
  // CR/XER pack. TMP2 (=r4) is saved through FPR f0 via mtfprd/mffprd:
  //   * ELFv2 nominally reserves a 288B red zone below r1, but a
  //     previous `std TMP2, -8(r1)` save faulted whenever r1 sat within
  //     8 bytes of a stack-mapping boundary.
  //   * f0 is volatile (caller-saved) per ELFv2 FP register conventions, and
  //     not in any SRA/RA pool — it is used only as an op-local scratch
  //     by a few VectorOps emitters and is guaranteed dead between IR ops.
  //   * mtfprd/mffprd are POWER8 ISA 2.07 instructions; available on host.
  mtfprd(FPR{0}, TMP2);                         // save TMP2 in f0 (no memory)
  // mfocrf 0x80 (single-field, uncracked): the rlwinm extracts below read
  // only CR0.LT (PPC bit 0) and CR0.EQ (PPC bit 2), both inside the defined
  // field-0 nibble; the masks discard the pre-3.0C-undefined remainder.
  mfocrf(TMP2, 0x80);                           // TMP2 = CR0 (LT@LSB31, EQ@LSB29)
  rlwinm(TMP3, TMP2, 0, 0, 0);                 // TMP3 = N @ LSB31
  rlwinm(TMP2, TMP2, 1, 1, 1);                 // TMP2 = Z (CR0.EQ@LSB29 → LSB30)
  or_(TMP3, TMP3, TMP2);                        // TMP3 = N | Z
  mfspr(TMP2, 1);                               // TMP2 = XER (CA@LSB29, OV@LSB30)
  rlwinm(tmp, TMP2, 0, 2, 2);                  // tmp  = C @ LSB29 (XER.CA)
  or_(TMP3, TMP3, tmp);
  rlwinm(TMP2, TMP2, 30, 3, 3);                 // TMP2 = V (XER.OV@LSB30 → LSB28)
  or_(TMP3, TMP3, TMP2);
  int32_t nzcv_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, State.nzcv));
  stw(TMP3, static_cast<int16_t>(nzcv_off), STATE);
  mffprd(TMP2, FPR{0});                         // restore TMP2 from f0
}

PPC64EmitterBase::SpillStubOffsets PPC64EmitterBase::EmitSpillStubs(size_t LinkStubAddrOffset) {
  // The exact two stubs the JIT used to emit per compile unit (JIT.cpp
  // "Shared miss-leg spill stubs"): byte-identical bodies, now one copy each
  // for the whole context. See the declaration for the position-independence
  // and register contracts.
  SpillStubOffsets Offsets;
  Offsets.Exit = GetOffset();
  SpillStaticRegs(TMP1);
  const int32_t exit_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, Pointers.ExitFunctionLinker));
  ld(TMP1, static_cast<int16_t>(exit_off), STATE);
  mtctr(TMP1);
  bctr();

  Align16B();
  Offsets.Link = GetOffset();
  SpillStaticRegs(TMP1);
  ld(TMP1, static_cast<int16_t>(LinkStubAddrOffset), TMP2);
  mtctr(TMP1);
  bctr();
  return Offsets;
}

// Fill static registers from CpuStateFrame → host regs
void PPC64EmitterBase::FillStaticRegs(FillMode Mode) {
  // SRA[i] ↔ guest register StaticGPRGuestReg[i]; see SpillStaticRegs.
  const auto& SRA = std::span<const GPR>(a64::SRA);

  const bool WantVolatile    = Mode != FillMode::NonVolatileGPRsOnly;
  const bool WantNonVolatile = Mode != FillMode::SkipNonVolatileGPRs;

  for (size_t i = 0; i < SRA.size(); ++i) {
    const bool NonVolatile = SRA[i].idx >= RegVolatility::kFirstNonVolatileGPR;
    if (NonVolatile ? !WantNonVolatile : !WantVolatile) {
      continue;
    }
    int32_t off = static_cast<int32_t>(offsetof(FEXCore::Core::CpuStateFrame, State) +
                                       FEXCore::Core::CPUState::GPROffset(FEXCore::Core::StaticGPRGuestReg[i]));
    if (off >= -32768 && off <= 32764 && (off & 3) == 0) {
      ld(SRA[i], off, STATE);
    } else {
      LoadImm32(TMP1, static_cast<uint32_t>(off));
      ldx(SRA[i], STATE, TMP1);
    }
  }

  if (WantNonVolatile) {
    int32_t callret_sp_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, State.callret_sp));
    ld(a64::CALLRET_SP, static_cast<int16_t>(callret_sp_off), STATE);
  }

  if (Mode == FillMode::NonVolatileGPRsOnly) {
    // Everything below belongs to the complementary half. Returning here is
    // what makes NonVolatileGPRsOnly + SkipNonVolatileGPRs == All.
    return;
  }

  // Fill SRA FPRs (V0-V15)
  const auto& SRAFPR = std::span<const VR>(a64::SRAFPR);
  for (size_t i = 0; i < SRAFPR.size(); ++i) {
    int32_t v_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, State.v[i][0]));
    LoadImm32(TMP1, static_cast<uint32_t>(v_off));
    lvx(SRAFPR[i], STATE, TMP1);
  }

  // Fill pinned VSX low bank (V16-V31 in vs16-vs31) from State.v[16..31]
  for (size_t i = 16; i < 32; ++i) {
    int32_t v_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, State.v[i][0]));
    LoadImm32(TMP1, static_cast<uint32_t>(v_off));
    if (EmitterCTX->HostFeatures.SupportsISA30) {
      lxvx(PPC64Emitter::vsx(i), STATE, TMP1);
    } else {
      lxvd2x(PPC64Emitter::vsx(i), STATE, TMP1);
      xxpermdi(PPC64Emitter::vsx(i), PPC64Emitter::vsx(i), PPC64Emitter::vsx(i), 2);
    }
  }

  // Restore NZCV across the dispatcher / C++ slow paths. Inverse of the
  // SpillStaticRegs save: load packed NZCV from State.nzcv
  // and unpack into CR0.LT/EQ + XER.CA/OV. Mirrors DEF_OP(StoreNZCV).
  int32_t nzcv_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, State.nzcv));
  lwz(TMP2, static_cast<int16_t>(nzcv_off), STATE);
  // Build CR0 input in TMP1: CR0.LT @ LSB31 ← packed N @ LSB31 (no shift),
  // CR0.EQ @ LSB29 ← packed Z @ LSB30 (rotl 31).
  rlwinm(TMP1, TMP2, 0,  0, 0);              // N → LSB31 (CR0.LT)
  rlwinm(TMP3, TMP2, 31, 2, 2);              // Z → LSB29 (CR0.EQ)
  or_(TMP1, TMP1, TMP3);
  mtocrf(0x80, TMP1);                         // CR0 ← bits 31..28 of TMP1 (single-field form)
  // XER: both bits fully written, so generate them arithmetically (addic for
  // CA, sldi-62 + addo for OV — PPC64Emitter.h helper block). The from-bit
  // helpers read no zero register, which matters here: r0 is NOT the JIT zero
  // in this routine (see the r0 NOTE below).
  rlwinm(TMP1, TMP2, 3, 31, 31);              // C (LSB29 = PPC 2) → 0/1 at LSB 0
  SetCAFromBit(TMP1, TMP3);
  rlwinm(TMP1, TMP2, 4, 31, 31);              // V (LSB28 = PPC 3) → 0/1 at LSB 0
  SetOVFromBit(TMP1, TMP3);

  // NOTE: this routine deliberately does NOT touch r0. ExitFunctionLinker
  // smuggles the resolved host-code pointer through r0 across FillStaticRegs
  // (see PPC64Dispatcher.cpp). Direct callers that need the JIT's r0=0
  // zero-index invariant must set it themselves; FillForABICall and the
  // Syscall return path both do.
}

// PPC64LE ELFv2 callee-saved registers: r14-r31, f14-f31, v20-v31, LR, CR2/3/4.
// Per ELFv2 §2.2.1.1, LR / CR / TOC save slots live in the *caller's* frame at fixed
// offsets from the caller's SP:
//   [old_SP + 8]  CR save (4 bytes)
//   [old_SP + 16] LR save (8 bytes)
//   [old_SP + 24] TOC save (8 bytes; only used by the callee around indirect calls)
// After `stdu r1, -576, r1` these become [r1+584], [r1+592], [r1+600] respectively.
//
// Local-frame layout (grows down from old_r1, 16-byte aligned, 576 bytes total).
// Offsets shown are from old_r1; after stdu r1, -576, r1 the same offsets
// expressed from the NEW r1 add 576 (so the back-chain at [old_r1-576] is
// [r1+0] post-stdu).
//
// This frame MUST reserve the ELFv2 96-byte linkage + parameter-save block at
// the bottom, because the dispatcher issues several ELFv2 C-ABI bctrls out of
// it (SleepThread, CompileSingleStep, ExitFunctionLink, ThreadPauseHandler,
// and the C++ signal-restart trampoline).  Any of those callees will write
// LR into [caller_r1+16] and CR into [caller_r1+8]; the dispatcher itself
// writes TOC into [caller_r1+24] at :391/:465 around every bctrl.
// PPC64Dispatcher.cpp:391 in particular runs on every L1 miss.
//
// The earlier 512-byte frame put VMX v20 at [new_r1+16], directly on top of
// the callee's LR save slot.  On return from any bctrl, PopCalleeSavedRegisters'
// `lvx v20, [r1+16]` reloaded {orig_v20_lo, callee_LR_or_TOC} into v20, silently
// corrupting a live host non-volatile vector register that C++ callers of the
// dispatcher (typically libstdc++/libc paths during thread bring-up) can rely on.
//
// New layout:
//   [new_r1 +   0]:            back chain (= old_r1)
//   [new_r1 +   8]: (CR)       our callee's CR save slot per ELFv2
//   [new_r1 +  16]: (LR)       our callee's LR save slot per ELFv2
//   [new_r1 +  24]: (TOC)      our callee's TOC save slot (dispatcher writes r2 here)
//   [new_r1 +  32.. 95]:       parameter save area (8 doublewords, for our callees)
//   [old_r1 - 480..-288]:      VMX v20..v31  (12 × 16 = 192 bytes)  — [new_r1 +  96..287]
//   [old_r1 - 288..-144]:      FPR f14..f31  (18 ×  8 = 144 bytes)  — [new_r1 + 288..431]
//   [old_r1 - 144..   0]:      GPR r14..r31  (18 ×  8 = 144 bytes)  — [new_r1 + 432..575]
//
// The FRAME_GPR_SAVE / FRAME_FPR_SAVE / FRAME_VMX_SAVE constants give the FIRST
// (lowest-numbered-reg) slot's offset FROM OLD_R1: -144 / -288 / -480.
static constexpr int32_t FRAME_GPR_SAVE   = -(8 * 18);                  // -144
static constexpr int32_t FRAME_FPR_SAVE   = FRAME_GPR_SAVE - 8 * 18;    // -288
static constexpr int32_t FRAME_VMX_SAVE   = FRAME_FPR_SAVE - 16 * 12;   // -480
static constexpr int32_t FRAME_TOTAL      = -576;
// ABI save slots in the caller's linkage area, expressed as offsets from r1
// after the stdu has decremented r1 by FRAME_TOTAL (576 bytes).
static constexpr int16_t LR_SAVE_OFFSET   = -FRAME_TOTAL + 16;  //  592
static constexpr int16_t CR_SAVE_OFFSET   = -FRAME_TOTAL + 8;   //  584

void PPC64EmitterBase::PushCalleeSavedRegisters() {
  // Use r0 for LR save: TMP1=r3 is the first C-ABI argument and must not be clobbered.
  mflr(r(0));

  // Save CR (only CR2/3/4 are non-volatile per ABI; we save the whole CR for simplicity).
  // Must be done before stdu so we can use the caller's CR save slot.
  mfcr(TMP2);

  // Allocate stack frame (back-chain at offset 0).
  stdu(r1, FRAME_TOTAL, r1);

  // Save LR and CR into the caller's linkage area (now at r1 + 528 / r1 + 520).
  std(r(0), LR_SAVE_OFFSET, r1);
  stw(TMP2, CR_SAVE_OFFSET, r1);

  for (int i = 14; i <= 31; ++i) {
    int32_t off = FRAME_GPR_SAVE + (i - 14) * 8;
    std(r(i), static_cast<int16_t>(off - FRAME_TOTAL), r1);
  }
  for (int i = 14; i <= 31; ++i) {
    int32_t off = FRAME_FPR_SAVE + (i - 14) * 8;
    stfd(f(i), static_cast<int16_t>(off - FRAME_TOTAL), r1);
  }
  for (int i = 20; i <= 31; ++i) {
    int32_t off = FRAME_VMX_SAVE + (i - 20) * 16;
    LoadImm32(TMP2, static_cast<uint32_t>(off - FRAME_TOTAL));
    stvx(VR{static_cast<uint32_t>(i)}, r1, TMP2);
  }
}

void PPC64EmitterBase::PopCalleeSavedRegisters() {
  for (int i = 20; i <= 31; ++i) {
    int32_t off = FRAME_VMX_SAVE + (i - 20) * 16;
    LoadImm32(TMP2, static_cast<uint32_t>(off - FRAME_TOTAL));
    lvx(VR{static_cast<uint32_t>(i)}, r1, TMP2);
  }
  for (int i = 14; i <= 31; ++i) {
    int32_t off = FRAME_FPR_SAVE + (i - 14) * 8;
    lfd(f(i), static_cast<int16_t>(off - FRAME_TOTAL), r1);
  }
  for (int i = 14; i <= 31; ++i) {
    int32_t off = FRAME_GPR_SAVE + (i - 14) * 8;
    ld(r(i), static_cast<int16_t>(off - FRAME_TOTAL), r1);
  }

  // Restore CR (caller's linkage area) and LR.
  lwz(TMP2, CR_SAVE_OFFSET, r1);
  ld(r(0), LR_SAVE_OFFSET, r1);
  // Per ELFv2 ABI only CR2/3/4 are callee-saved (FXM=0x38).  Restoring all
  // 8 fields would overwrite CR0 (the JIT block may have packed post-spill
  // NZCV before exiting) and CR1/5/6/7 (volatile per ABI but cleaner to
  // leave alone).  Only-CR2/3/4 is faster and ABI-correct.
  mtcrf(0x38, TMP2);
  mtlr(r(0));

  // Deallocate frame.
  addi(r1, r1, static_cast<int16_t>(-FRAME_TOTAL));
}

// Push dynamic (non-SRA) registers before an ABI call
//
// PPC64LE ELFv2 requires the *caller* to leave a 32-byte frame header at the
// top of the new stack frame for the *callee*'s use: back-chain (+0), CR (+8),
// LR (+16), TOC (+24). A typical C++ callee's prologue is
// `mflr r0; std r0, 16(r1)` — that store lands in *its caller's* r1+16.
// If we spilled a register at offset 16 of our dynamic-spill area, the
// callee's LR-save would silently corrupt the spill and PopDynamicRegs would
// then read back the bogus LR (≈ a JIT-trampoline code address). The result
// was an obscure SEGV the next time the JIT used the affected SRA register
// as a memory base — first reproduced by the precision_test_* x87 cases that
// exercise multiple FLDCW + FABI-helper sequences in one block.
// Spill GPRs at [r1+32..] and FPRs at the next 16-byte boundary above.
size_t PPC64EmitterBase::PushDynamicRegs(GPR tmp) {
  const auto RAFPR = std::span<const VR>(a64::RAFPR);

  const size_t FPRStart = a64::kDynFPRStart;
  const size_t SaveSize = a64::kDynRegSaveSize;

  // stdu writes the back-chain at [new_r1+0] = old r1, so traceback walkers
  // can step past this frame.  SaveSize fits the signed-16-bit displacement.
  stdu(r1, -static_cast<int16_t>(SaveSize), r1);

  // Save ONLY what an ELFv2 callee may clobber. RA is entirely callee-saved
  // in both modes (x64: r24-r26/r30/r31; x32: r16-r26/r30/r31 — the
  // static_asserts by the pool definitions in PPC64Emitter.h pin this), and
  // of RAFPR only v0-v19 are volatile, so v20+ survive any C call. That
  // includes host calls that RE-ENTER the JIT via a guest callback: the
  // dispatcher's C entry runs PushCalleeSavedRegisters, which saves
  // r14-r31/f14-f31/v20-v31 before any JIT code runs. Frame layout is
  // deliberately UNCHANGED — the skipped registers keep their (now unwritten)
  // slots, so kDynGPRStart/kDynFPRStart arithmetic and the DEF_OP(Thunk)
  // linkage-area assumption stay valid, and stack cost is the only thing not
  // reclaimed (240 bytes of dead frame, irrelevant).
  //
  // Previously this saved all of RA + all of RAFPR: ~50 wasted instructions
  // per SpillForABICall/FillForABICall pair on every host C call (thunks,
  // CPUID, atomic/crypto helpers, x87 fallbacks). DEF_OP(Syscall) already
  // relied on exactly this reasoning for the GPR half (BranchOps.cpp).
  for (size_t i = 0; i < RAFPR.size(); ++i) {
    if (RAFPR[i].idx >= 20) {
      continue;  // v20-v31: ELFv2 callee-saved
    }
    if (!(DynVRSpillMask & (1u << i))) {
      continue;  // not live across this call (see DynVRSpillMask contract)
    }
    int32_t off = static_cast<int32_t>(FPRStart + i * 16);
    LoadImm32(tmp, static_cast<uint32_t>(off));
    stvx(RAFPR[i], r1, tmp);
  }
  return SaveSize;
}

void PPC64EmitterBase::PopDynamicRegs() {
  const auto RAFPR = std::span<const VR>(a64::RAFPR);

  const size_t FPRStart = a64::kDynFPRStart;
  const size_t SaveSize = a64::kDynRegSaveSize;

  // Mirror of PushDynamicRegs: only the ELFv2-volatile subset was saved, so
  // only that subset is reloaded. Everything else was preserved by the callee.
  for (size_t i = 0; i < RAFPR.size(); ++i) {
    if (RAFPR[i].idx >= 20) {
      continue;  // v20-v31: ELFv2 callee-saved, never spilled
    }
    if (!(DynVRSpillMask & (1u << i))) {
      continue;  // not spilled by PushDynamicRegs (mask unchanged in between)
    }
    int32_t off = static_cast<int32_t>(FPRStart + i * 16);
    LoadImm32(TMP1, static_cast<uint32_t>(off));
    lvx(RAFPR[i], r1, TMP1);
  }

  addi(r1, r1, static_cast<int16_t>(SaveSize));
}

// FABI-callsite caller-save of the live volatile dynamic VRs. The FABI bridge
// stubs (PPC64Dispatcher::GenerateABICall) are shared across every callsite,
// so they cannot know which dynamic VRs are live — their Push/PopDynamicRegs
// run under mask 0 and the two callsite emitters bracket the bctrl with this
// pair instead, storing into the callsite's own scratch frame. Slots are
// packed by SAVED register (k counts set mask bits, not pool indices) so the
// save/restore loops must stay in lockstep — both iterate the same mask.
void PPC64EmitterBase::SaveDynVRsToFrame(int32_t BaseOffset) {
  const auto RAFPR = std::span<const VR>(a64::RAFPR);

  int32_t off = BaseOffset;
  for (size_t i = 0; i < RAFPR.size(); ++i) {
    if (RAFPR[i].idx >= 20 || !(DynVRSpillMask & (1u << i))) {
      continue;
    }
    LoadImm32(TMP3, static_cast<uint32_t>(off));
    stvx(RAFPR[i], r1, TMP3);
    off += 16;
  }
}

void PPC64EmitterBase::RestoreDynVRsFromFrame(int32_t BaseOffset) {
  const auto RAFPR = std::span<const VR>(a64::RAFPR);

  int32_t off = BaseOffset;
  for (size_t i = 0; i < RAFPR.size(); ++i) {
    if (RAFPR[i].idx >= 20 || !(DynVRSpillMask & (1u << i))) {
      continue;
    }
    LoadImm32(TMP3, static_cast<uint32_t>(off));
    lvx(RAFPR[i], r1, TMP3);
    off += 16;
  }
}

// Unaligned 128-bit load/store.
//
// Historical shape: bounce through STATE+JITScratch — ld/ld + std/std + lvx
// (7 instructions) — because lvx/stvx silently mask EA to a 16-byte boundary.
// Both replacement paths below were verified to produce the byte-identical
// register image (mem[EA+i] → BE byte element 15-i, i.e. guest XMM byte i in
// element 15-i):
//   * lxvx/stxvx (ISA 3.0): LE semantics place mem[EA+i] into element 15-i
//     directly — 1 instruction, any alignment, no scratch, no memory bounce.
//     Emit-time gated on HostFeatures.SupportsISA30 (POWER8 would SIGILL).
//     Their DQ-form siblings lxv/stxv are the same operation with the
//     displacement in the instruction, and are selected by PrepareV128Addr
//     when the displacement is a multiple of 16 — see below.
//   * lxvd2x/stxvd2x (ISA 2.06, POWER8-legal): produce/consume the
//     DOUBLEWORD-SWAPPED image (dword[0] = LE int mem[EA..7], dword[1] =
//     LE int mem[EA+8..15]); xxpermdi DM=2 (T.dw0 = A.dw1, T.dw1 = B.dw0)
//     swaps the halves — 2 instructions.
// Register-contract change vs the bounce: these paths clobber NO TMP GPRs
// and NO VMX registers. The store's pre-3.0 path swaps through VTMP3_VSX
// (vs12, the RA-invisible low bank) — it used to burn VTMP2/VTMP1.
// Fault behaviour: si_addr/DAR precision for lxvx/stxvx across protected page
// boundaries was measured good on the target (POWER9 DD2.2, Radix) in all
// first/second/both-page patterns — see docs/POWER9_PORT_PLAN.md §2.1.
// NOTE: `ea` must not be r0 — RA=0 in the encoding reads literal zero (the
// same constraint the old D-form ld/std bounce had).
void PPC64EmitterBase::LoadUnalignedV128(VSXR dst, GPR ea) {
  LoadUnalignedV128(dst, V128AddrForm {ea, GPRegs::r0, 0});
}

void PPC64EmitterBase::StoreUnalignedV128(VSXR src, GPR ea) {
  StoreUnalignedV128(src, V128AddrForm {ea, GPRegs::r0, 0});
}

// Address-form selection for the two above. Every form they emit is X-form
// (RA+RB), so an index needs no `add`; ISA 3.0's lxv/stxv additionally hold a
// multiple-of-16 displacement in the instruction, so it needs no `addi`
// either. See the V128AddrForm comment in the header for why the single-`ea`
// signature was costing an instruction.
PPC64EmitterBase::V128AddrForm PPC64EmitterBase::PrepareV128Addr(GPR base, GPR index, bool has_index, int64_t disp) {
  LOGMAN_THROW_A_FMT(base != GPRegs::r0, "PPC64 V128 base register must not be r0; RA=0 encodes literal zero");
  if (has_index) {
    // The X-form absorbs it on every ISA level. Nothing to emit.
    return {base, index, 0};
  }
  if (disp == 0) {
    return {base, GPRegs::r0, 0};
  }
  // DQ holds EA bits 4:15, so the displacement must be a multiple of 16 and
  // fit a signed 16-bit field. AArch64's `ldr q`/`str q` unsigned-offset form
  // scales imm12 by 16 and `ldp q`/`stp q` scales imm7 by 16, so the common
  // guest shapes land here; LDUR's unscaled simm9 does not and falls through.
  if (EmitterCTX->HostFeatures.SupportsISA30 && (disp & 0xF) == 0 && disp >= -32768 && disp <= 32752) {
    return {base, GPRegs::r0, static_cast<int16_t>(disp)};
  }
  if (disp >= -32768 && disp <= 32767) {
    addi(TMP3, base, static_cast<int16_t>(disp));
    return {TMP3, GPRegs::r0, 0};
  }
  // Wider than any displacement field: materialize it and let the X-form's RB
  // carry it. Still cheaper than materialize-then-add.
  LoadConstant(TMP3, static_cast<uint64_t>(disp));
  return {base, TMP3, 0};
}

void PPC64EmitterBase::LoadUnalignedV128(VSXR dst, const V128AddrForm& A) {
  if (EmitterCTX->HostFeatures.SupportsISA30) {
    if (A.Disp != 0) {
      lxv(dst, A.Disp, A.RA);           // DQ-form: EA = GPR[RA] + Disp
      return;
    }
    lxvx(dst, A.RA, A.RB);              // EA = GPR[RA] + GPR[RB], r0 ≡ 0
    return;
  }
  // Pre-3.0 VSX fallback (POWER7/POWER8): swapped-image load + fixup.
  // PrepareV128Addr never hands this arm a displacement.
  LOGMAN_THROW_A_FMT(A.Disp == 0, "pre-ISA-3.0 has no DQ-form vector load");
  lxvd2x(dst, A.RA, A.RB);
  xxpermdi(dst, dst, dst, 2);           // swap dword[0] <-> dword[1]
}

void PPC64EmitterBase::StoreUnalignedV128(VSXR src, const V128AddrForm& A) {
  if (EmitterCTX->HostFeatures.SupportsISA30) {
    if (A.Disp != 0) {
      stxv(src, A.Disp, A.RA);
      return;
    }
    stxvx(src, A.RA, A.RB);
    return;
  }
  // Pre-3.0 VSX fallback: stxvd2x writes the doubleword-swapped image, so
  // swap into a scratch first (src must be preserved for the caller). The
  // scratch is VTMP3_VSX from the RA-invisible low bank — both consumers
  // (xxpermdi, stxvd2x) are VSX-form so they can encode it, and using it
  // instead of VTMP1/VTMP2 means this path clobbers NO VMX register at all.
  LOGMAN_THROW_A_FMT(A.Disp == 0, "pre-ISA-3.0 has no DQ-form vector store");
  xxpermdi(VTMP3_VSX, src, src, 2);
  stxvd2x(VTMP3_VSX, A.RA, A.RB);
}

// Sub-128-bit FPR memory ops (guest `ldr`/`str` b/h/s/d, and x86
// vmovd/vmovq/vmov{ss,sd}) write/read only `size` bytes, and the load form
// zero-extends the upper bits. Naively using stvx/lvx writes/reads 16 bytes
// and corrupts adjacent stack/structure slots (root cause of hello_static
// SEGV at __tls_init_tp), so every arm below is size-exact.
//
// Both helpers take VSXR, not VR: every arm is a VSX-form instruction, which
// is what lets a low-bank (vs16-vs31) register reach them at all, and what
// makes them re-exportable into PPC64VSXView. VR converts to VSXR, so the
// VMX-side callers are unaffected.
//
// The historical shape for the sizes with no scalar VSX instruction was a
// bounce through STATE+JITScratch (16 bytes, alignas(16) in CpuStateFrame --
// ELFv2 nominally has a 288B red zone below r1, but an r1-relative slot
// faulted on tight clone-allocated stacks where the mapping ended before the
// ABI red zone did). The store side no longer has any such size, and the load
// side only f80. CRITICAL for the one that remains: the bounce clobbers
// TMP1/TMP2/TMP3, so capture `ea` into TMP4 first if it aliases.
void PPC64EmitterBase::StoreFPRSized(VSXR src, GPR ea, uint32_t size) {
  if (size == 16) {
    StoreUnalignedV128(src, ea);
    return;
  }

  // Every remaining size is a staging permute plus one scalar VSX store, and
  // the permute is shared because all of them read out of dword[0].
  //
  // Register image convention (see the LoadFPRSized block comment): the guest
  // value lives in the LOW bits of dword[1] — BE bytes (16-size)..15 — as an
  // LE integer. The scalar stores read dword[0] instead (stxsdx the whole
  // doubleword, stxsiwx word element 1 = its low word, stxsibx/stxsihx its low
  // byte/halfword), so one xxpermdi with DM=2 moves dword[1] into dword[0]:
  //   T.dw0 = A.dw1 = the guest value, T.dw1 = B.dw0 = don't-care.
  // That is the exact inverse of the load path's xxpermdi(dst,dst,dst,2), and
  // for the narrow sizes the guest's low bytes land at the bottom of dw0 —
  // the end those stores read — because they were the low bytes of the LE
  // integer in dword[1]. Nothing outside the `size` bytes is written, which is
  // the whole point of not using stvx here (hello_static __tls_init_tp SEGV).
  //
  // src must be preserved for the caller, so permute into VTMP3_VSX (the
  // RA-invisible low-bank scratch) — same choice, and therefore the same
  // clobbers-no-VMX-register contract, as the pre-3.0 StoreUnalignedV128
  // path. Every consumer here is VSX-form, which is what makes the low bank
  // encodable at all.
  // stxsdx is ISA 2.06 and stxsiwx ISA 2.07, so both are POWER8-legal with
  // no feature gate — matching the ungated lxsdx/lxsiwzx on the load side.
  // `ea` sits in RA (where RA=0 would mean literal zero), so it must not be
  // r0; that is already the documented precondition for these helpers.
  //
  // Anything else is a bug upstream, not a size to emit for: the old code
  // reached a `default: break` here after spilling to JITScratch and stored
  // NOTHING, silently. f80 stores do not exist (x86's FSTP tword is the only
  // producer and there is no x86 frontend in this tree); if one appears, it
  // needs a lowering, not a no-op.
  // ERROR_AND_DIE_FMT, not LOGMAN_MSG_A_FMT: the A-family macros compile to
  // nothing unless ASSERTIONS_ENABLED, which only a DEBUG build sets
  // (CMakeLists.txt:199) -- so in the Release binary that actually ships, an
  // assert-and-return here IS the silent no-op this guard exists to replace.
  if (size != 1 && size != 2 && size != 4 && size != 8) {
    ERROR_AND_DIE_FMT("StoreFPRSized: no scalar store for size {}", size);
  }
  xxpermdi(VTMP3_VSX, src, src, 2);
  if (size == 8) {
    stxsdx(VTMP3_VSX, ea, GPRegs::r0);
    return;
  }
  if (size == 4) {
    stxsiwx(VTMP3_VSX, ea, GPRegs::r0);
    return;
  }
  // Sizes 1 and 2. On ISA 3.0 stxsibx/stxsihx store bits 56:63 and 48:63 of
  // dword[0] -- guest bytes 0 and 0:1 after the permute -- so this is the same
  // two instructions the 4/8 arms cost. What it replaces is a five-instruction
  // bounce through STATE+JITScratch (addi, li, stvx, lbz, stbx) plus the
  // store-to-load-forwarding stall of reading back what stvx just wrote, and
  // it replaced it on ISA 3.0 TOO, because the emitter had lxsibzx/lxsihzx but
  // not their store twins until they were added. `str b`/`str h` were 7 host
  // instructions on POWER9 for that reason alone.
  //
  // Pre-3.0 has no byte/halfword scalar VSX store, so move dword[0] into a GPR
  // and use the ordinary stb/sth: three instructions, still no memory bounce
  // and still nothing written outside the `size` bytes. mfvsrd is ISA 2.07, as
  // is the stxsiwx above, so this arm needs no further gate.
  if (EmitterCTX->HostFeatures.SupportsISA30) {
    if (size == 1) {
      stxsibx(VTMP3_VSX, ea, GPRegs::r0);
    } else {
      stxsihx(VTMP3_VSX, ea, GPRegs::r0);
    }
    return;
  }
  // The GPR is written before `ea` is read, so it must not BE `ea`.
  // [Load|Store]MemPair hands this helper TMP1 as the address (see
  // ComputeOffsetAddrInto) and MaterializeAddr hands it TMP3, so the aliasing
  // case is reachable rather than hypothetical.
  const GPR Scratch = (ea == TMP1) ? TMP2 : TMP1;
  mfvsrd(Scratch, VTMP3_VSX);
  if (size == 1) {
    stbx(Scratch, ea, GPRegs::r0);
  } else {
    sthx(Scratch, ea, GPRegs::r0);
  }
}

void PPC64EmitterBase::LoadFPRSized(VSXR dst, GPR ea, uint32_t size) {
  if (size == 16) {
    LoadUnalignedV128(dst, ea);
    return;
  }

  // Scalar VSX fast paths. Target image (matches the historical zero-fill
  // bounce): guest value in BE byte elements (16-size)..15 as an LE integer
  // — i.e. the value in the LOW bits of dword[1] — with every other byte 0.
  // All four lxs* loads deposit the (zero-extended) value into dword[0], so
  // one xxpermdi moves it to dword[1] with a zero in dword[0]:
  //   * ISA 3.0 (gated):    lxs*  +  xxpermdi(dst,dst,dst,2).  v3.0 defines
  //     dword[1] <- 0 for these loads, so the DM=2 swap (T.dw0 = A.dw1 = 0,
  //     T.dw1 = B.dw0 = value) finishes the job in 2 instructions.
  //   * pre-3.0 (sizes 4/8): lxsiwzx/lxsdx leave dword[1] UNDEFINED on
  //     2.06/2.07 hardware, so merge against the pinned zero with DM=0
  //     (T.dw0 = VZERO_VSX.dw0 = 0, T.dw1 = B.dw0 = value) — never reading
  //     the undefined half.  2 instructions.
  //   * pre-3.0 (sizes 1/2): no scalar VSX load exists, so the zero-extended
  //     byte/halfword comes through a GPR: lbzx/lhzx + mtvsrd into dword[0] +
  //     the same DM=0 merge against the pinned zero.  3 instructions, against
  //     the 7 of the JITScratch bounce this replaces (addi, std, std, lbzx,
  //     stb, li, lvx) and with no store-to-load-forwarding stall.  `ldr b`/
  //     `ldr h` cost 9 host instructions on a POWER8 for that reason.
  // The 3.0 and 4/8 paths clobber no TMP GPR and no vector temp; the pre-3.0
  // 1/2 path writes one TMP (`ea` is read first, in the same instruction).
  const bool ISA30 = EmitterCTX->HostFeatures.SupportsISA30;
  if (ISA30 && (size == 1 || size == 2 || size == 4 || size == 8)) {
    switch (size) {
    case 1: lxsibzx(dst, ea, GPRegs::r0); break;
    case 2: lxsihzx(dst, ea, GPRegs::r0); break;
    case 4: lxsiwzx(dst, ea, GPRegs::r0); break;
    case 8: lxsdx  (dst, ea, GPRegs::r0); break;
    }
    xxpermdi(dst, dst, dst, 2);
    return;
  }
  if (!ISA30 && (size == 1 || size == 2 || size == 4 || size == 8)) {
    switch (size) {
    case 1:
      // lbzx/lhzx zero-extend to the whole 64-bit GPR and mtvsrd copies all
      // of it into dword[0], so the upper bytes of the result are zero for
      // the same reason the wider arms' are. RT may be RA here (dst and ea
      // are different register files, and a load reads its address operands
      // before writing RT), so no aliasing guard is needed.
      lbzx(TMP1, ea, GPRegs::r0);
      mtvsrd(dst, TMP1);
      break;
    case 2:
      lhzx(TMP1, ea, GPRegs::r0);
      mtvsrd(dst, TMP1);
      break;
    case 4: lxsiwzx(dst, ea, GPRegs::r0); break;
    case 8: lxsdx(dst, ea, GPRegs::r0); break;
    }
    // DM=0: T.dw0 = VZERO_VSX.dw0 = 0, T.dw1 = B.dw0 = value — the undefined
    // dword[1] of the scalar load (and the undefined dword[1] of mtvsrd) is
    // never read. The pinned zero replaces a per-load xxlxor + vector temp
    // (one per guest movss/movsd in DSP-heavy blocks); the dispatcher
    // maintains vs14 == 0 (see VZERO_VSX). T == B is fine: xxpermdi reads
    // both source doublewords before writing either.
    xxpermdi(dst, VZERO_VSX, dst, 0);
    return;
  }

  // f80 only, and so x86 only: the one size with no scalar VSX load at any
  // level. No AArch64 instruction produces it, and there is no x86 frontend in
  // this tree, so nothing below is reachable today -- it is kept because it is
  // the IR contract for OpSize::f80Bit and a silent mis-load is worse than a
  // cold path.
  // ERROR_AND_DIE_FMT for the same reason as StoreFPRSized's guard: an
  // assert-and-return is compiled out of the Release binary, and here that
  // leaves `dst` UNWRITTEN -- a load that silently yields whatever was in the
  // register, which is worse than the no-op on the store side.
  if (size != 10) {
    ERROR_AND_DIE_FMT("LoadFPRSized: no scalar load for size {}", size);
  }
  GPR EaSafe = ea;
  if (ea == TMP1 || ea == TMP2 || ea == TMP3) {
    mr(TMP4, ea);
    EaSafe = TMP4;
  }
  // Zero the 16-byte JITScratch slot, then write `size` bytes from *ea into
  // its low end so the upper bits are zero (matches x86 vmovd/vmovq
  // zero-extend semantics).  See StoreFPRSized for why we bounce through
  // STATE+JITScratch instead of r1-redzone (ELFv2 has no red zone; r1-relative
  // scratch faulted at stack-mapping boundaries).
  constexpr int32_t kScratchOff = offsetof(FEXCore::Core::CpuStateFrame, JITScratch);
  addi(TMP3, STATE, static_cast<int16_t>(kScratchOff));
  std(GPRegs::r0, 0, TMP3);
  std(GPRegs::r0, 8, TMP3);
  // 80-bit float (FLD tword [mem]): read 8+2 bytes from EA into scratch's
  // low 10 bytes, leaving the upper 6 zeroed by the std r0 prelude above.
  ldx(TMP1, EaSafe, GPRegs::r0);
  std(TMP1, 0, TMP3);
  li(TMP2, 8);
  lhzx(TMP1, EaSafe, TMP2);
  sth(TMP1, 8, TMP3);
  // The reload is the full-width VSX one, not `li` + `lvx`. JITScratch is
  // alignas(16), so lvx's EA masking never bit, but the two forms produce the
  // byte-identical register image for an aligned EA (the LoadUnalignedV128
  // block comment above derives that) and this one needs no zero register --
  // one instruction on ISA 3.0 against two, and the same two on POWER8.  It is
  // also the only spelling a VSX-form helper may use at all: `lvx` takes a VR,
  // which a low-bank destination cannot be.
  LoadUnalignedV128(dst, V128AddrForm {TMP3, GPRegs::r0, 0});
}

void PPC64EmitterBase::SpillForABICall(GPR tmp, bool FPRs) {
  SpillStaticRegs(tmp);
  PushDynamicRegs(tmp);
  // NB. Do NOT set InSyscallInfo here — arming belongs at the CROSSING SITE,
  // strictly after this helper returns (DEF_OP(Syscall), DEF_OP(Thunk), the
  // FABI bridge stubs), never inside it. Two reasons, one historical and
  // still load-bearing:
  //  1. The marker's low bits claim "these SRA GPRs are already spilled";
  //     raising it before SpillStaticRegs completes would let a signal
  //     handler skip spilling registers whose frame copies are stale.
  //  2. SpillForABICall serves every host-C-call site in the backend, and
  //     some callees RE-ENTER the guest via CallbackPtr. Arming used to be
  //     lethal there: callback guest code ran with SpillSRA
  //     IgnoreMask=0xFFFF, so any signal arriving in the callback skipped
  //     all 16 x86-64 SRA GPR spills; State.rip then got a ContextBackup*
  //     interpreted as guest-stack and the guest resumed with a garbage RIP
  //     — the 2026-07-31 Factorio crash (SIGTRACE showed isi=0xffff on
  //     every DELIVER; PC=0xaacb584c2489d700). The dispatcher's CallbackPtr
  //     entry now zeroes InSyscallInfo before any callback guest code runs
  //     (PPC64Dispatcher.cpp), which is what makes arming across
  //     guest-re-entering callees sound at all — but the discipline stands:
  //     only a crossing that pairs the arm with a sentinel-checked refill
  //     and a disarm may raise the marker.
}

void PPC64EmitterBase::FillForABICall(bool FPRs) {
  PopDynamicRegs();
  FillStaticRegs();
  // Paired clear removed with the set above. r0 is volatile per the ELFv2
  // ABI; the JIT relies on r0=0 as the zero-index for `ldx`/`stdx`-style
  // instructions, so restore that invariant explicitly. Not a store to
  // InSyscallInfo any more.
  li(GPRegs::r0, 0);
}

void PPC64EmitterBase::ArmInSyscallSentinel(uint64_t Sentinel) {
  // Contract in PPC64Emitter.h (kInSyscallSentinel + the declaration): emit
  // only after the crossing's SpillForABICall, pair with
  // FillForABICallChecked. 0x100FFFF fits LoadImm32's lis+ori.
  LoadConstant(TMP1, Sentinel);
  const int32_t isi_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, InSyscallInfo));
  std(TMP1, static_cast<int16_t>(isi_off), STATE);
}

void PPC64EmitterBase::FillForABICallChecked() {
  // 64-bit guests only — FillStaticRegs asserts this for the partial modes
  // (the 32-bit lwz zero-extension invariant forbids skipping GPR fills).
  PopDynamicRegs();

  // Mirror of DEF_OP(Syscall)'s fill elision: SRA slots 6..15 live in
  // r14..r23, which ELFv2 preserves across the bctrl, so when the sentinel
  // survived — no signal delivery truncated it, no callback cleared it —
  // those host registers still hold the exact values SpillStaticRegs
  // published and the ten `ld`s are pure overhead. When any of those events
  // republished the frame, the branch takes the complementary
  // NonVolatileGPRsOnly fill first, making the pair a full refill.
  const int32_t isi_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, InSyscallInfo));
  PPC64Emitter::Label SentinelIntact{};
  ld(TMP1, static_cast<int16_t>(isi_off), STATE);
  // TMP1 = InSyscallInfo >> 16, recording into CR0: EQ iff nothing above
  // bit 15 survived, i.e. iff the frame was republished behind us. CR0 is
  // dead here (host-call return); FillStaticRegs rebuilds it from the
  // frame's packed NZCV below.
  rldicl_(TMP1, TMP1, 48, 16);
  // BO=4 (branch if false), BI=2 (CR0.EQ) — i.e. bne cr0.
  bc({4, 2}, &SentinelIntact);
  FillStaticRegs(FillMode::NonVolatileGPRsOnly);
  Bind(&SentinelIntact);
  FillStaticRegs(FillMode::SkipNonVolatileGPRs);

  // Same tail as FillForABICall: restore the r0=0 zero-index invariant.
  li(GPRegs::r0, 0);

  // Disarm. From here on any signal treats this code as normal JIT and the
  // full SRA spill path is correct again. Skipping the disarm would leave
  // the mask claiming "spilled" over live SRA state — the Factorio failure
  // class described at SpillForABICall.
  li(TMP1, 0);
  std(TMP1, static_cast<int16_t>(isi_off), STATE);
}

} // namespace FEXCore::CPU
