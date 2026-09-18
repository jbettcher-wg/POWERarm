// SPDX-License-Identifier: MIT
//
// PPC64LE lowerings of the A64-specific scalar FP conversion ops
// (IR.json: A64FloatToGPR, A64FloatFromGPR, A64FToF).
//
// The x86-shaped conversions (Float_ToGPR_ZS, Float_FromGPR_S, Float_FToF)
// return INT_MIN for NaN and positive overflow, have no unsigned forms and
// convert i64 -> f32 by rounding twice. These implement the A64 rules:
// saturation to the destination range, NaN -> 0, a single rounding with the
// FPSCR rounding mode (kept equal to FPCR.RMode), and quieting of a
// signalling NaN on FCVT.
//
// Vector register image (see CoreState.h and PPC64Emitter.cpp): element 0 is
// VSX doubleword 1; a single-precision element 0 is the low word of it. The
// VSX scalar instructions work on doubleword 0, so operands are positioned
// there first. Measured on POWER9 with a probe of each instruction:
// xscvdpspn, xscvdpsp and xscvdpsxws/xscvdpuxws write their
// 32-bit result into both words of doubleword 0; xscvspdpn reads word 0;
// the unsigned converts give 0 for NaN and negative inputs; xsrdpi rounds
// ties away from zero.
//
// Everything here is ISA 2.07 (POWER8) except the half-precision
// instructions, which are gated on SupportsISA30 with a POWER8 path. The
// conversions touch only TMP1-TMP4, VTMP1, VTMP2, f0 and CR1; A64FArith at the
// bottom of this file additionally writes CR6 and, on its cold path only,
// vs3-vs8 and LR (saved and restored). CR0 and XER, which hold the guest NZCV,
// are untouched throughout.
#include "Interface/Context/Context.h"
#include "Interface/Core/JIT/PPC64LE/JITClass.h"

namespace FEXCore::CPU {

using namespace PPC64Emitter::FPRegs;

namespace {
  // Encodings the shared emitter does not provide, checked against the
  // POWER9 assembler: xscvdpuxds vs34,vs35 = f0401d23, xscvdpuxws = f0401923,
  // xsrdpi = f0401927, mfvsrwz r7,v2 = 7c4700e7, mtfsb0 30 = ffc0008c.
  constexpr uint32_t XO_XSCVDPUXDS = 328;
  constexpr uint32_t XO_XSCVDPUXWS = 72;
  constexpr uint32_t XO_XSRDPI = 73;
  // ISA 3.0: xscvhpdp vs34,vs35 = f0501d6f and xscvdphp = f0511d6f (XO 347,
  // told apart by 16/17 in the RA field).
  constexpr uint32_t XO_XSCVHPDP = 347;
  constexpr uint32_t XO_XSCVDPHP = 347;

  void EmitXX2WithRA(PPC64JITCore* J, VR T, VR B, uint32_t XO, uint32_t RA) {
    J->Emit32((60u << 26) | (T.idx << 21) | (RA << 16) | (B.idx << 11) | ((XO & 0x1FFu) << 2) | (1u << 1) | 1u);
  }

  void mfvsrwz(PPC64JITCore* J, GPR Rt, VR Vrs) {
    J->Emit32((31u << 26) | (Vrs.idx << 21) | (Rt.idx << 16) | (115u << 1) | 1u);
  }
  void mtfsb0(PPC64JITCore* J, uint32_t Bit) {
    J->Emit32((63u << 26) | (Bit << 21) | (70u << 1));
  }

  // FPSCR bits 62:63 (32-bit numbering 30:31) are RN.
  constexpr uint32_t FPSCR_RN_HI = 30;
  constexpr uint32_t FPSCR_RN_LO = 31;
} // namespace

// Positions element 0 of Vec in doubleword 0 of Dst as a double.
static void PositionElement0AsDouble(PPC64JITCore* J, VR Dst, VR Vec, IR::OpSize ElementSize) {
  if (ElementSize == IR::OpSize::i32Bit) {
    J->xxsldwi(Dst, Vec, Vec, 3);   // word 0 <- element 0
    J->xscvspdpn(Dst, Dst);         // bit-preserving for NaN
  } else {
    J->xxpermdi(Dst, Vec, Vec, 0b11); // doubleword 0 <- doubleword 1
  }
}

// Dst = [0 : Value] with Value a 64-bit pattern in doubleword 0 of Src, or a
// 32-bit pattern in TMP1 when FromGPR32 is set.
static void PlaceElement0(PPC64JITCore* J, VR Dst, VR Src) {
  J->vspltisw(VTMP2, 0);
  J->xxpermdi(Dst, VTMP2, Src, 0); // dw0 <- 0, dw1 <- Src.dw0
}

static void PlaceSingleFromDoubleword0(PPC64JITCore* J, VR Dst, VR Src) {
  // The single's bits are in the low word of doubleword 0; the high word is
  // not guaranteed to be zero.
  mfvsrwz(J, TMP1, Src);
  J->mtvsrd(VTMP1, TMP1);
  PlaceElement0(J, Dst, VTMP1);
}

DEF_OP(A64FloatToGPR) {
  const auto Op = IROp->C<IR::IROp_A64FloatToGPR>();
  const auto Dst = GetReg(Node);
  const auto Src = GetVReg(Op->Scalar);
  const bool Is64 = IROp->Size == IR::OpSize::i64Bit;

  PositionElement0AsDouble(this, VTMP1, Src, Op->SrcElementSize);

  switch (Op->Rounding) {
  case 0: // Ties to even: round with RN forced to nearest, then restore RN.
    mffs(f(0));
    mtfsb0(this, FPSCR_RN_HI);
    mtfsb0(this, FPSCR_RN_LO);
    xsrdpic(VTMP1, VTMP1);
    mtfsf(0x01, f(0)); // field 7: XE, NI and RN
    break;
  case 1: xsrdpip(VTMP1, VTMP1); break;
  case 2: xsrdpim(VTMP1, VTMP1); break;
  case 3: break; // The converts below truncate.
  default: EmitXX2(VTMP1.idx, VTMP1.idx, XO_XSRDPI); break;
  }

  if (!Op->Signed) {
    EmitXX2(VTMP2.idx, VTMP1.idx, Is64 ? XO_XSCVDPUXDS : XO_XSCVDPUXWS);
    if (Is64) {
      mfvsrd(Dst, VTMP2);
    } else {
      mfvsrwz(this, Dst, VTMP2);
    }
    return;
  }

  // Signed converts saturate but give INT_MIN for NaN; A64 wants 0.
  xscmpudp(1, VTMP1, VTMP1);
  if (Is64) {
    xscvdpsxds(VTMP2, VTMP1);
    mfvsrd(Dst, VTMP2);
  } else {
    xscvdpsxws(VTMP2, VTMP1);
    mfvsrwz(this, Dst, VTMP2);
  }
  PPC64Emitter::Label Ordered;
  bc(PPC64Emitter::Cond {4, 7}, &Ordered); // CR1.SO clear: not NaN
  li(Dst, 0);
  Bind(&Ordered);
}

DEF_OP(A64FloatFromGPR) {
  const auto Op = IROp->C<IR::IROp_A64FloatFromGPR>();
  const auto Dst = GetVReg(Node);
  GPR Src = GetReg(Op->Src);

  if (Op->SrcSize == IR::OpSize::i32Bit) {
    if (Op->Signed) {
      extsw(TMP1, Src);
    } else {
      clrldi(TMP1, Src, 32);
    }
    Src = TMP1;
  }
  mtvsrd(VTMP1, Src);

  if (IROp->Size == IR::OpSize::i64Bit) {
    Op->Signed ? xscvsxddp(VTMP1, VTMP1) : xscvuxddp(VTMP1, VTMP1);
    PlaceElement0(this, Dst, VTMP1);
    return;
  }
  // One rounding, straight to single precision (double format), then the
  // bit-preserving format change.
  Op->Signed ? xscvsxdsp(VTMP1, VTMP1) : xscvuxdsp(VTMP1, VTMP1);
  xscvdpspn(VTMP1, VTMP1);
  PlaceSingleFromDoubleword0(this, Dst, VTMP1);
}

// Half precision <-> double, element 0.
//
// ISA 3.0 has xscvhpdp/xscvdphp. Measured on the POWER9: xscvhpdp reads the
// low 16 bits of doubleword 0 and quiets a signalling NaN (payload kept);
// xscvdphp rounds with RN, overflows to Inf or the largest finite value as
// RN directs, and keeps the top 9 payload bits of a NaN with the quiet bit
// set. Those are the A64 FCVT rules. The POWER8 paths compute the same
// results with GPR code that never writes XER (XER.CA holds the guest C
// flag): no subfic/sradi/addic, compares only into CR1.
static void EmitHalfToDouble(PPC64JITCore* J, VR Dst, VR Src, bool ISA30) {
  J->xxpermdi(VTMP1, Src, Src, 0b11);
  if (ISA30) {
    EmitXX2WithRA(J, VTMP1, VTMP1, XO_XSCVHPDP, 16);
    PlaceElement0(J, Dst, VTMP1);
    return;
  }

  PPC64Emitter::Label Done, NotSpecial, Normal;
  J->mfvsrd(TMP1, VTMP1);
  J->rldicl(TMP4, TMP1, 0, 54);  // f = h & 0x3ff
  J->rldicl(TMP3, TMP1, 54, 59); // e = (h >> 10) & 0x1f
  J->rldicl(TMP2, TMP1, 49, 63); // s = (h >> 15) & 1
  J->sldi(TMP2, TMP2, 63);
  J->cmpldi(cr(1), TMP3, 0x1F);
  J->bc(PPC64Emitter::Cond {4, 6}, &NotSpecial);
  // Infinity or NaN: frac << 42, quiet bit forced for a NaN.
  J->sldi(TMP1, TMP4, 42);
  J->or_(TMP2, TMP2, TMP1);
  J->li(TMP3, 0x7FF);
  J->sldi(TMP3, TMP3, 52);
  J->or_(TMP2, TMP2, TMP3);
  J->cmpldi(cr(1), TMP4, 0);
  J->bc(PPC64Emitter::Cond {12, 6}, &Done);
  J->li(TMP3, 1);
  J->sldi(TMP3, TMP3, 51);
  J->or_(TMP2, TMP2, TMP3);
  J->b(&Done);

  J->Bind(&NotSpecial);
  J->cmpldi(cr(1), TMP3, 0);
  J->bc(PPC64Emitter::Cond {4, 6}, &Normal);
  J->cmpldi(cr(1), TMP4, 0);
  J->bc(PPC64Emitter::Cond {12, 6}, &Done); // signed zero
  // Denormal f * 2^-24: normalize on the highest set bit p (0..9).
  J->cntlzd(TMP3, TMP4);
  J->addi(TMP3, TMP3, -63);
  J->neg(TMP3, TMP3);           // p
  J->li(TMP1, 52);
  J->subf(TMP1, TMP3, TMP1);    // 52 - p
  J->sld(TMP1, TMP4, TMP1);
  J->clrldi(TMP1, TMP1, 12);    // drop the leading one
  J->or_(TMP2, TMP2, TMP1);
  J->addi(TMP3, TMP3, 999);     // p - 24 + 1023
  J->sldi(TMP3, TMP3, 52);
  J->or_(TMP2, TMP2, TMP3);
  J->b(&Done);

  J->Bind(&Normal);
  J->addi(TMP3, TMP3, 1008);    // e - 15 + 1023
  J->sldi(TMP3, TMP3, 52);
  J->or_(TMP2, TMP2, TMP3);
  J->sldi(TMP1, TMP4, 42);
  J->or_(TMP2, TMP2, TMP1);

  J->Bind(&Done);
  J->mtvsrd(VTMP1, TMP2);
  PlaceElement0(J, Dst, VTMP1);
}

static void EmitDoubleToHalf(PPC64JITCore* J, VR Dst, VR Src, bool ISA30) {
  J->xxpermdi(VTMP1, Src, Src, 0b11);
  if (ISA30) {
    EmitXX2WithRA(J, VTMP1, VTMP1, XO_XSCVDPHP, 17);
    J->mfvsrd(TMP1, VTMP1);
    J->clrldi(TMP1, TMP1, 48);
    J->mtvsrd(VTMP1, TMP1);
    PlaceElement0(J, Dst, VTMP1);
    return;
  }

  PPC64Emitter::Label Done, Finite, InfResult, MaxResult, NotRN2, InRange, KOk, Positive, NoCarry, NoCarryOverflow, Subnormal;
  J->mfvsrd(TMP1, VTMP1);
  J->rldicl(TMP2, TMP1, 1, 63);
  J->sldi(TMP2, TMP2, 15);        // half sign
  J->rldicl(TMP3, TMP1, 12, 53);  // biased exponent
  J->cmpldi(cr(1), TMP3, 0x7FF);
  J->bc(PPC64Emitter::Cond {4, 6}, &Finite);
  J->clrldi(TMP4, TMP1, 12);      // fraction
  J->cmpldi(cr(1), TMP4, 0);
  J->bc(PPC64Emitter::Cond {12, 6}, &InfResult);
  J->srdi(TMP4, TMP4, 42);
  J->ori(TMP4, TMP4, 0x7E00);     // all-ones exponent, quiet bit, top payload
  J->or_(TMP2, TMP2, TMP4);
  J->b(&Done);

  J->Bind(&Finite);
  J->addi(TMP3, TMP3, -1023);     // unbiased exponent e
  J->cmpdi(cr(1), TMP3, 15);
  J->bc(PPC64Emitter::Cond {4, 5}, &InRange); // e <= 15
  // |x| >= 2^16: Inf for nearest, toward +Inf when positive, toward -Inf
  // when negative; the largest finite value otherwise.
  J->mffs(f(0));
  J->mffprd(TMP4, f(0));
  J->rldicl(TMP4, TMP4, 0, 62);   // RN
  J->cmpldi(cr(1), TMP4, 0);
  J->bc(PPC64Emitter::Cond {12, 6}, &InfResult);
  J->cmpldi(cr(1), TMP4, 1);
  J->bc(PPC64Emitter::Cond {12, 6}, &MaxResult);
  J->cmpldi(cr(1), TMP4, 2);
  J->bc(PPC64Emitter::Cond {4, 6}, &NotRN2);
  J->cmpldi(cr(1), TMP2, 0);
  J->bc(PPC64Emitter::Cond {12, 6}, &InfResult);
  J->b(&MaxResult);
  J->Bind(&NotRN2);
  J->cmpldi(cr(1), TMP2, 0);
  J->bc(PPC64Emitter::Cond {4, 6}, &InfResult);
  J->Bind(&MaxResult);
  J->ori(TMP2, TMP2, 0x7BFF);
  J->b(&Done);
  J->Bind(&InfResult);
  J->ori(TMP2, TMP2, 0x7C00);
  J->b(&Done);

  J->Bind(&InRange);
  // k = max(e, -14). M = x * 2^(10 - k), rounded to an integer with RN, is
  // the half significand (subnormal when |M| < 2^10).
  J->cmpdi(cr(1), TMP3, -14);
  J->bc(PPC64Emitter::Cond {4, 4}, &KOk); // e >= -14
  J->li(TMP3, -14);
  J->Bind(&KOk);
  J->li(TMP4, 1033);
  J->subf(TMP4, TMP3, TMP4);      // 1023 + 10 - k
  J->sldi(TMP4, TMP4, 52);
  J->mtvsrd(VTMP2, TMP4);
  J->xsmuldp(VTMP1, VTMP1, VTMP2); // exact
  J->xsrdpic(VTMP1, VTMP1);
  J->xscvdpsxds(VTMP1, VTMP1);
  J->mfvsrd(TMP1, VTMP1);
  J->cmpdi(cr(1), TMP1, 0);
  J->bc(PPC64Emitter::Cond {4, 4}, &Positive);
  J->neg(TMP1, TMP1);
  J->Bind(&Positive);
  J->cmpldi(cr(1), TMP1, 2048);
  J->bc(PPC64Emitter::Cond {12, 4}, &NoCarry); // |M| < 2^11
  J->srdi(TMP1, TMP1, 1);
  J->addi(TMP3, TMP3, 1);
  J->Bind(&NoCarry);
  J->cmpdi(cr(1), TMP3, 15);
  J->bc(PPC64Emitter::Cond {4, 5}, &NoCarryOverflow);
  J->ori(TMP2, TMP2, 0x7C00);     // rounding carried past the largest finite value
  J->b(&Done);
  J->Bind(&NoCarryOverflow);
  J->cmpldi(cr(1), TMP1, 1024);
  J->bc(PPC64Emitter::Cond {12, 4}, &Subnormal);
  J->addi(TMP3, TMP3, 15);
  J->sldi(TMP3, TMP3, 10);
  J->or_(TMP2, TMP2, TMP3);
  J->clrldi(TMP1, TMP1, 54);
  J->Bind(&Subnormal);
  J->or_(TMP2, TMP2, TMP1);

  J->Bind(&Done);
  J->mtvsrd(VTMP1, TMP2);
  PlaceElement0(J, Dst, VTMP1);
}

DEF_OP(A64FToF) {
  const auto Op = IROp->C<IR::IROp_A64FToF>();
  const auto Dst = GetVReg(Node);
  const auto Src = GetVReg(Op->Scalar);
  const auto DstES = IROp->Size;
  const auto SrcES = Op->SrcElementSize;

  if (SrcES == IR::OpSize::i64Bit && DstES == IR::OpSize::i32Bit) {
    xxpermdi(VTMP1, Src, Src, 0b11);
    xscvdpsp(VTMP1, VTMP1); // rounds with RN, quiets a signalling NaN
    PlaceSingleFromDoubleword0(this, Dst, VTMP1);
    return;
  }

  if (SrcES == IR::OpSize::i32Bit && DstES == IR::OpSize::i64Bit) {
    xxsldwi(VTMP1, Src, Src, 3);
    xscvspdpn(VTMP1, VTMP1);
    // Quiet a signalling NaN: every NaN gets the quiet bit.
    xscmpudp(1, VTMP1, VTMP1);
    PPC64Emitter::Label Ordered;
    bc(PPC64Emitter::Cond {4, 7}, &Ordered);
    LoadConstant(TMP1, 1ULL << 51);
    mtvsrd(VTMP2, TMP1);
    vor(VTMP1, VTMP1, VTMP2);
    Bind(&Ordered);
    PlaceElement0(this, Dst, VTMP1);
    return;
  }

  if (SrcES == IR::OpSize::i16Bit && DstES == IR::OpSize::i64Bit) {
    EmitHalfToDouble(this, Dst, Src, EmitterCTX->HostFeatures.SupportsISA30);
    return;
  }
  if (SrcES == IR::OpSize::i64Bit && DstES == IR::OpSize::i16Bit) {
    EmitDoubleToHalf(this, Dst, Src, EmitterCTX->HostFeatures.SupportsISA30);
    return;
  }

  Op_Unhandled(IROp, Node);
}

// ===========================================================================
// A64FArith — FADD/FSUB/FMUL/FDIV with A64 NaN precedence (F1)
// ===========================================================================
//
// The frontend used to run PropagateNaNOperand (8 IR ops plus a constant) in
// front of every VFAdd/VFSub/VFMul/VFDiv. The research measured that
// branch-free fix at 11 dependent instructions / 18.35 cycles against 6.35 for
// the bare add, i.e. "more than the arithmetic itself, three times over"
// (docs/powerarm/research/scalar-fp/SCALAR-FP-LOWERING.md §3.2, §13).
//
// It exists to cover ONE divergent shape. VSX arithmetic returns operand 1 if
// it is any NaN, else operand 2, quieted; ARM returns the first SIGNALLING NaN,
// else the first quiet one. Those agree except when operand 1 is a quiet NaN
// and operand 2 a signalling NaN — 72 of 2 756 672 corpus pairs, and nothing
// else in any rounding mode (§3.1 [MEASURED]).
//
// So: run the plain instruction, ask the result whether ANY lane is a NaN, and
// branch out of line if so.
//
//     [xxlor VTMP2, Ds, Ds]         ; only when Dst aliases a source
//     xv{add,sub,mul,div}{dp,sp} Dst, A, B
//     xvcmpeq{dp,sp}. VTMP1, Dst, Dst
//     bc 4, 24, .cold               ; CR6[0] clear <=> some lane is a NaN
//   .join:
//
// Two instructions on the ordered path, three when the destination aliases a
// source. The post-check is COMPLETE for the binary ops: a NaN result implies
// a NaN operand or an invalid operation, and invalid operations (Inf-Inf,
// 0*Inf, 0/0, Inf/Inf) give the default NaN on both architectures (§3.1), so
// re-running the op in the stub always reproduces the fast path's answer where
// the fast path was already right. The compare reads the RESULT, so it does not
// lengthen the dependent chain the next op sees, and the `bc` is predicted not
// taken. It also fires when a *garbage* lane of the result is a NaN, which is
// correct but slow (§3.2 note); for a scalar op the upper lane is zeroed by the
// StoreVSized VMov afterwards, so the value written is unaffected.
//
// REGISTER AND CR DISCIPLINE (COLD-BLOCK-DESIGN.md §5):
//   Site:  writes Dst, VTMP1 (the compare's discarded lane mask), VTMP2 (the
//          alias stash) and CR6. No GPR, no CR0/CR1, no XER, no LR, no memory.
//          The stash and the discard target are deliberately DIFFERENT
//          registers — AtomicOps.cpp:552-558 records what happens when a stash
//          register is also the op's scratch.
//   CR6:   safe. Its three users (CondJump's vector-compare leg,
//          DEF_OP(MemCpy)'s alignment-loop predicate, DEF_OP(VAnyNonZero)) are
//          each confined to one IR op, so CR6 never crosses an op boundary.
//          Do not widen CR6 use beyond this op.
//   Stub:  adds TMP1 (constant materialisation in the body), TMP4 (the LR
//          park), vs3-vs8 and LR — SAVED AND RESTORED, not left clobbered.
//   LR:    LR is NOT scratch between ops. The rule is stated in tree at
//          BranchOps.cpp:23 and :1035 ("Every mflr(r0) in the backend today is
//          paired with a restore ... the failure mode is silent guest memory
//          corruption"), and all eight mid-op LR clobbers honour it. The park
//          goes through TMP4, never r0: routing LR through r0 would break the
//          global r0 == 0 invariant that JIT blocks rely on for
//          `ldx/stdx rX, rBase, r0`, which is the bug ALUOps.cpp:3768 records.
//          The bodies clobber at most TMP1, so TMP4 is safe by construction —
//          A BODY THAT GROWS A TMP4 USE SILENTLY DESTROYS THE RETURN ADDRESS.
//          If one ever needs more GPRs, switch it to the full
//          VectorOps.cpp:4934-4950 frame ceremony including the trailing
//          `li r0, 0`, rather than reaching for TMP3/TMP4.
//
// Everything is ISA 2.06 (VSX arithmetic, xxsel, the record-form compares) or
// 2.07 (mtfprd), so POWERARM_HOSTFEATURES=disableisa30 runs identical code.
//
// Signal note: on the taken path Dst briefly holds the host's NaN choice
// before the stub overwrites it with ARM's. If Dst is an SRA register and an
// asynchronous signal lands in that window, a guest handler would observe the
// other NaN's payload. Same shape as DEF_OP(A64FloatToGPR)'s `mfvsrwz Dst`
// followed by a conditional `li Dst, 0`, and as DEF_OP(VFMLA)'s addend copy
// into Dst; it differs only in the NaN payload, never in the value's class.

namespace {
  // The cold path's fixed low-bank registers. vs3-vs8 are RA-invisible: the
  // backend names only f0-f2 as FPRs, vs12 is VTMP3_VSX, vs14 VZERO_VSX, vs15
  // was the AES-mask trap and vs16-vs31 are the AVX-high bank when it is on
  // (PPC64Emitter.h). They are transient within a stub — no host call can occur
  // between the `bl` and the stub's final `b` — so nothing is pinned. The SHA-1
  // and SHA-256 round emitters (VectorOps.cpp EmitSha1Rounds4 /
  // EmitSha256Rounds4) already take vs2-vs9 as op-local scratch the same way.
  constexpr auto P0 = PPC64Emitter::VSXR {3}; // A, and A' on return
  constexpr auto P1 = PPC64Emitter::VSXR {4}; // B
  constexpr auto P2 = PPC64Emitter::VSXR {5};
  constexpr auto P3 = PPC64Emitter::VSXR {6};
  constexpr auto P4 = PPC64Emitter::VSXR {7}; // the quiet-bit mask
  constexpr auto P5 = PPC64Emitter::VSXR {8};

  // CR6[0] (CR bit 24) is set iff EVERY lane compared equal, so BO=4 ("branch
  // if the bit is clear") on it is "some lane is a NaN".
  constexpr PPC64Emitter::Cond CondSomeLaneNaN {4, 24};
} // namespace

DEF_OP(A64FArith) {
  const auto Op = IROp->C<IR::IROp_A64FArith>();
  const auto ElemSz = IROp->ElementSize;
  if (ElemSz != IR::OpSize::i32Bit && ElemSz != IR::OpSize::i64Bit) {
    Op_Unhandled(IROp, Node);
    return;
  }
  const bool Is64 = ElemSz == IR::OpSize::i64Bit;
  const auto Dst = GetVReg(Node);
  const auto V1 = GetVReg(Op->Vector1);
  const auto V2 = GetVReg(Op->Vector2);

  // The RA prefers the SRA register the result is next stored to
  // (RegisterAllocationPass.cpp:626-633), so accumulator shapes like
  // `fadd d0, d0, d1` routinely tie Dst to a source. The arithmetic then
  // destroys an operand the cold stub still needs, so stash it first. At most
  // one stash is ever needed: when Dst aliases BOTH sources the two sources
  // are the same register, and the one stash serves for both.
  PPC64Emitter::VR A = V1, B = V2;
  if (Dst == V1 || Dst == V2) {
    const auto Aliased = (Dst == V1) ? V1 : V2;
    xxlor(VTMP2, Aliased, Aliased);
    if (Dst == V1) {
      A = VTMP2;
    }
    if (Dst == V2) {
      B = VTMP2;
    }
  }

  switch (Op->Op) {
  case 0: Is64 ? xvadddp(Dst, V1, V2) : xvaddsp(Dst, V1, V2); break;
  case 1: Is64 ? xvsubdp(Dst, V1, V2) : xvsubsp(Dst, V1, V2); break;
  case 2: Is64 ? xvmuldp(Dst, V1, V2) : xvmulsp(Dst, V1, V2); break;
  case 3: Is64 ? xvdivdp(Dst, V1, V2) : xvdivsp(Dst, V1, V2); break;
  default: Op_Unhandled(IROp, Node); return;
  }

  Is64 ? xvcmpeqdp_(VTMP1, Dst, Dst) : xvcmpeqsp_(VTMP1, Dst, Dst);

  if (!FPColdEnabled()) {
    // Positive control: the check is emitted, the branch is not. The goldens'
    // (qNaN, sNaN) rows must then FAIL, which is how we know the cold path is
    // reached at all.
    return;
  }

  FPNaNFixBodyUsed[Is64] = true;
  auto& Stub = FPColdStubs.emplace_back();
  Stub.SiteOffset = GetOffset();
  Stub.Dst = Dst;
  Stub.A = A;
  Stub.B = B;
  Stub.Op = Op->Op;
  Stub.Is64 = Is64;
  bc(CondSomeLaneNaN, &Stub.Entry);
  Bind(&Stub.Join);
}

// NaNFix_{dp,sp} — the host transcription of IRBuilder::PropagateNaNOperand
// (A64Frontend/TranslateFP.cpp:135-145), which the Pi goldens already verify,
// so the cold path's specification is the existing frontend code.
//
//   in:  P0 = A, P1 = B
//   out: P0 = A' — B in lanes where A is a quiet NaN and B a signalling NaN,
//        A everywhere else
//   clobbers: P2, P3, P4, P5, TMP1. NO CR field, no XER, no VMX register, no
//             memory, and no GPR besides TMP1 — TMP4 carries the caller's LR.
//
// Re-running the site's own op on (A', B) is what makes the swap sufficient
// for sub and div too: a NaN operand makes the result a NaN whatever the
// order, only the choice of NaN changes (FP research §3.4 [MEASURED: fsub and
// fdiv PASS]).
//
// The quiet-bit test is `xxland t, X, Q` then `xvcmpeq{dp,sp} m, t, Q`: X & Q
// is either 0 or Q, Q is a denormal bit pattern, and VSX has no DAZ (FP
// research §6.3, "denormals are free on POWER9"), so comparing it with itself
// is exact and no VMX-form op is needed on the low bank. The sticky VXSNAN
// this raises in FPSCR is unobservable — FPSR cumulative flags are not
// emulated (TranslateFP.cpp:335).
void PPC64JITCore::EmitFPNaNFixBody(bool Is64) {
  Bind(&FPNaNFixBody[Is64]);

  // The quiet bit, replicated into every lane. mtfprd writes doubleword 0 and
  // leaves doubleword 1 undefined, so splat dw0 across both with xxpermdi
  // DM=0b00. The sp pattern is pre-replicated into the 64-bit immediate.
  LoadConstant(TMP1, Is64 ? 0x0008000000000000ULL : 0x0040000000400000ULL);
  mtfprd(f(P4.idx), TMP1);
  xxpermdi(P4, P4, P4, 0b00);

  // A lane is unordered with itself iff it is a NaN.
  if (Is64) {
    xvcmpeqdp(P2, P0, P0); // OrdA
    xvcmpeqdp(P3, P1, P1); // OrdB
  } else {
    xvcmpeqsp(P2, P0, P0);
    xvcmpeqsp(P3, P1, P1);
  }

  xxland(P5, P0, P4);
  Is64 ? xvcmpeqdp(P5, P5, P4) : xvcmpeqsp(P5, P5, P4); // A's quiet bit set
  xxlandc(P2, P5, P2);                                  // QuietA   = QSetA & ~OrdA

  xxland(P5, P1, P4);
  Is64 ? xvcmpeqdp(P5, P5, P4) : xvcmpeqsp(P5, P5, P4); // B's quiet bit set
  xxlnor(P3, P3, P5);                                   // SigB     = ~(OrdB | QSetB)

  xxland(P2, P2, P3);                                   // swap where both hold
  xxsel(P0, P0, P1, P2);                                // A' = mask ? B : A
  blr();
}

// One per-site stub: copy the (possibly stashed) operands to the body's fixed
// registers, call the body, re-run the site's own arithmetic on the fixed
// operands, and branch back. Join is already bound, so the `b` is backward and
// its 24-bit displacement is never a concern.
//
// The cold path takes two taken branches plus the call/return pair; its cost is
// irrelevant, an ordered-data workload never enters it (FP research §11).
void PPC64JITCore::EmitFPColdStubs(bool BranchOver) {
  if (FPColdStubs.empty()) {
    return;
  }

  // `bl` sets the emitter's r0-dirty flag (primary 18 with LK=1 is a call form,
  // and ELFv2 r0 is volatile). Here it is a false positive: the callee is a
  // JIT-emitted leaf that never touches r0. Restoring the flag keeps the P5.0.2
  // exit re-zero elision exact for units that only ever "clobber" r0 this way.
  const bool WasR0Dirty = R0Dirty();

  PPC64Emitter::Label Over {};
  if (BranchOver) {
    b(&Over);
  }

  for (auto& S : FPColdStubs) {
    if (GetOffset() - S.SiteOffset > 32764) {
      ERROR_AND_DIE_FMT("PPC64 JIT: A64FArith cold branch at +{:#x} cannot reach its stub at +{:#x}", S.SiteOffset, GetOffset());
    }
    Bind(&S.Entry);
    xxlor(P0, AsVSX(S.A), AsVSX(S.A));
    xxlor(P1, AsVSX(S.B), AsVSX(S.B));
    mflr(TMP4);
    bl(&FPNaNFixBody[S.Is64]);
    mtlr(TMP4);
    const auto D = AsVSX(S.Dst);
    switch (S.Op) {
    case 0: S.Is64 ? xvadddp(D, P0, P1) : xvaddsp(D, P0, P1); break;
    case 1: S.Is64 ? xvsubdp(D, P0, P1) : xvsubsp(D, P0, P1); break;
    case 2: S.Is64 ? xvmuldp(D, P0, P1) : xvmulsp(D, P0, P1); break;
    case 3: S.Is64 ? xvdivdp(D, P0, P1) : xvdivsp(D, P0, P1); break;
    default: ERROR_AND_DIE_FMT("PPC64 JIT: A64FArith cold stub with kind {}", S.Op);
    }
    b(&S.Join);
  }
  FPColdStubs.clear();

  // Bodies last, and only once per unit per width: a later flush's `bl` to an
  // already-bound label is a backward branch with 24 bits of reach.
  for (int W = 0; W < 2; ++W) {
    if (FPNaNFixBodyUsed[W] && !FPNaNFixBody[W].bound) {
      EmitFPNaNFixBody(W != 0);
    }
  }

  if (BranchOver) {
    Bind(&Over);
  }
  if (!WasR0Dirty) {
    ResetR0Dirty();
  }
}

} // namespace FEXCore::CPU
