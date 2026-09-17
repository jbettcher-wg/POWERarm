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
// instructions, which are gated on SupportsISA30 with a POWER8 path, and
// touches only TMP1-TMP4, VTMP1, VTMP2, f0 and CR1. CR0 and XER, which hold the guest NZCV,
// are untouched.
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

} // namespace FEXCore::CPU
