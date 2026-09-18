// SPDX-License-Identifier: MIT
//
// A64 scalar floating point: moves between general-purpose and FP
// registers, immediates, one- and two-register arithmetic, fused
// multiply-add, compares, conditional compare and select, conversions to and
// from integers and fixed point, and the scalar SIMD integer conversions.
//
// Semantics that differ from the host (DESIGN.md §4.4):
//  * NaN propagation. A64 returns the first signalling NaN operand (quieted),
//    else the first quiet NaN. The VSX arithmetic returns operand 1 whenever
//    it is any NaN, so a quiet NaN in operand 1 with a signalling NaN in
//    operand 2 is fixed up by passing operand 2 in both positions for those
//    lanes (PropagateNaNOperand).
//  * FMIN/FMAX return the propagated NaN for any NaN operand and order -0
//    below +0; FMINNM/FMAXNM first replace a lone quiet NaN by the infinity
//    that loses. Neither matches xsmaxdp/xsmindp, so both are built from
//    ordered compares and selects.
//  * Conversions saturate and convert NaN to 0 (A64FloatToGPR).
//
//  * Half precision has no host arithmetic. Operands are widened to double
//    (exact), the operation runs in double precision, and the result is
//    rounded once to half precision with the FPCR rounding mode (see
//    HalfToDouble/DoubleToHalf for FZ16). Addition, subtraction,
//    multiplication, division and square root of half-precision values are
//    exact or cannot land within 2^-53 of a half-precision rounding boundary,
//    so the double step never changes the rounded result; only the fused
//    multiply-add of widely separated magnitudes can lose a sticky bit.
//    POWERARM-M1-TODO(fpu): FPCR.AHP (alternative half-precision format) is stored but not emulated.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

#include <FEXCore/Core/CoreState.h>

namespace FEXCore::A64 {
using namespace FEXCore::IR;

namespace {
  // FP type field (bits 23:22) -> element size. Returns false for the
  // unallocated type.
  bool FPTypeSize(uint32_t Type, OpSize* Size) {
    switch (Type) {
    case 0b00: *Size = OpSize::i32Bit; return true;
    case 0b01: *Size = OpSize::i64Bit; return true;
    case 0b11: *Size = OpSize::i16Bit; return true;
    default: return false;
    }
  }

  // A64 VFPExpandImm for half, single or double precision.
  uint64_t VFPExpandImm(uint64_t Imm8, OpSize Size) {
    const uint64_t Sign = (Imm8 >> 7) & 1;
    const uint64_t B6 = (Imm8 >> 6) & 1;
    switch (Size) {
    case OpSize::i64Bit: return (Sign << 63) | ((B6 ^ 1) << 62) | ((B6 ? 0xFFULL : 0) << 54) | ((Imm8 & 0x3F) << 48);
    case OpSize::i32Bit: return (Sign << 31) | ((B6 ^ 1) << 30) | ((B6 ? 0x1FULL : 0) << 25) | ((Imm8 & 0x3F) << 19);
    default: return (Sign << 15) | ((B6 ^ 1) << 14) | ((B6 ? 0x3ULL : 0) << 12) | ((Imm8 & 0x3F) << 6);
    }
  }

  uint64_t QuietBit(OpSize ElementSize) {
    return ElementSize == OpSize::i64Bit ? 1ULL << 51 : 1ULL << 22;
  }

  uint64_t Replicate(uint64_t Value, OpSize ElementSize) {
    switch (ElementSize) {
    case OpSize::i64Bit: return Value;
    case OpSize::i32Bit: return (Value << 32) | Value;
    default: return Value * 0x0001000100010001ULL;
    }
  }

  constexpr uint64_t DOUBLE_QUIET_BIT = 1ULL << 51;

  // A64 FPRounding for the FCVT* opcode groups: rmode (bits 20:19) with the
  // A variants (ties away) selected separately.
  constexpr uint8_t ROUND_TIEEVEN = 0;
  constexpr uint8_t ROUND_POSINF = 1;
  constexpr uint8_t ROUND_NEGINF = 2;
  constexpr uint8_t ROUND_ZERO = 3;
  constexpr uint8_t ROUND_TIEAWAY = 4;
} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Ref IRBuilder::FPConstant(uint64_t Bits, OpSize ElementSize) {
  return VectorConstant64(Replicate(Bits, ElementSize));
}

Ref IRBuilder::FZ16Mask() {
  Ref FPCR = _LoadContext(OpSize::i32Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, fpcr));
  return _VDupFromGPR(OpSize::i128Bit, OpSize::i64Bit, _Neg(OpSize::i64Bit, _Bfe(OpSize::i64Bit, 1, 19, FPCR)));
}

Ref IRBuilder::HalfToDouble(Ref V, bool KeepSignalling, bool ApplyFZ16) {
  const auto RS = OpSize::i128Bit;
  // FPUnpack with FPCR.FZ16 set reads a half-precision denormal as a zero of
  // the same sign. FP-to-FP conversions unpack with FZ16 ignored
  // (FPUnpackCV), which the Pi confirms.
  Ref H = V;
  if (ApplyFZ16) {
    Ref Denormal = _VAnd(RS, RS, _VCMPEQZ(RS, OpSize::i16Bit, _VAnd(RS, RS, V, FPConstant(0x7C00, OpSize::i16Bit))), FZ16Mask());
    H = _VBSL(RS, Denormal, _VAnd(RS, RS, V, FPConstant(0x8000, OpSize::i16Bit)), V);
  }
  Ref D = _A64FToF(OpSize::i64Bit, OpSize::i16Bit, H);
  if (!KeepSignalling) {
    return D;
  }
  // The widening quiets a signalling NaN; arithmetic operand precedence
  // needs to see it, so clear the quiet bit again for those.
  Ref Bits16 = _VExtractToGPR(RS, OpSize::i16Bit, H, 0);
  Ref TopMatch = _Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::EQ, _And(OpSize::i64Bit, Bits16, Constant(0x7E00)),
                         Constant(0x7C00), Constant(1), Constant(0));
  Ref Signalling = _Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::NEQ, _And(OpSize::i64Bit, Bits16, Constant(0x1FF)), Constant(0),
                           TopMatch, Constant(0));
  Ref ClearQuiet = _VDupFromGPR(RS, OpSize::i64Bit, _And(OpSize::i64Bit, _Neg(OpSize::i64Bit, Signalling), Constant(DOUBLE_QUIET_BIT)));
  return _VAndn(RS, RS, D, ClearQuiet);
}

Ref IRBuilder::DoubleToHalf(Ref D, bool ApplyFZ16) {
  const auto RS = OpSize::i128Bit;
  // FPRound with FPCR.FZ16 set turns a result below the smallest normal
  // half-precision magnitude (2^-14), before rounding, into a zero of the
  // same sign. FP-to-FP conversions round with FZ16 ignored (FPRoundCV).
  if (!ApplyFZ16) {
    return _A64FToF(OpSize::i16Bit, OpSize::i64Bit, D);
  }
  Ref Tiny = _VAnd(RS, RS, _VFCMPLT(RS, OpSize::i64Bit, _VFAbs(RS, OpSize::i64Bit, D), FPConstant(0x3F10000000000000ULL, OpSize::i64Bit)),
                   FZ16Mask());
  Ref Flushed = _VBSL(RS, Tiny, _VAnd(RS, RS, D, FPConstant(0x8000000000000000ULL, OpSize::i64Bit)), D);
  return _A64FToF(OpSize::i16Bit, OpSize::i64Bit, Flushed);
}

Ref IRBuilder::A64Arith(OpSize ElementSize, Ref A, Ref B, FPBinaryOp Op) {
  // A64FArith's Op field is documented in IR.json as FPBinaryOp's order, and
  // only the four arithmetic kinds are valid there.
  static_assert(static_cast<uint8_t>(FPBinaryOp::Add) == 0);
  static_assert(static_cast<uint8_t>(FPBinaryOp::Sub) == 1);
  static_assert(static_cast<uint8_t>(FPBinaryOp::Mul) == 2);
  static_assert(static_cast<uint8_t>(FPBinaryOp::Div) == 3);
  LOGMAN_THROW_A_FMT(Op <= FPBinaryOp::Div, "A64Arith: not an arithmetic kind");
  return _A64FArith(OpSize::i128Bit, ElementSize, A, B, static_cast<uint8_t>(Op));
}

Ref IRBuilder::PropagateNaNOperand(OpSize ElementSize, Ref A, Ref B) {
  // Lanes where A is a quiet NaN and B a signalling NaN take B for A.
  // POWERARM-M1-TODO(fpu): this adds about a dozen vector instructions to every scalar FP arithmetic op; a fused A64 arithmetic IR op (or a NaN check branching to the fixup) would make the common non-NaN path free.
  const auto RS = OpSize::i128Bit;
  Ref Quiet = FPConstant(QuietBit(ElementSize), ElementSize);
  Ref NaNA = _VFCMPUNO(RS, ElementSize, A, A);
  Ref NaNB = _VFCMPUNO(RS, ElementSize, B, B);
  Ref QuietA = _VAnd(RS, RS, NaNA, _VNot(RS, ElementSize, _VCMPEQZ(RS, ElementSize, _VAnd(RS, RS, A, Quiet))));
  Ref SignallingB = _VAnd(RS, RS, NaNB, _VCMPEQZ(RS, ElementSize, _VAnd(RS, RS, B, Quiet)));
  return _VBSL(RS, _VAnd(RS, RS, QuietA, SignallingB), B, A);
}

Ref IRBuilder::FPMinMax(OpSize ElementSize, Ref A, Ref B, bool IsMax, bool IsNumber) {
  const auto RS = OpSize::i128Bit;
  if (IsNumber) {
    // A lone quiet NaN operand becomes the infinity that loses the compare.
    Ref Quiet = FPConstant(QuietBit(ElementSize), ElementSize);
    Ref QuietNaNA = _VAnd(RS, RS, _VFCMPUNO(RS, ElementSize, A, A), _VNot(RS, ElementSize, _VCMPEQZ(RS, ElementSize, _VAnd(RS, RS, A, Quiet))));
    Ref QuietNaNB = _VAnd(RS, RS, _VFCMPUNO(RS, ElementSize, B, B), _VNot(RS, ElementSize, _VCMPEQZ(RS, ElementSize, _VAnd(RS, RS, B, Quiet))));
    const bool Is64 = ElementSize == OpSize::i64Bit;
    const uint64_t PosInf = Is64 ? 0x7FF0000000000000ULL : 0x7F800000ULL;
    const uint64_t NegInf = Is64 ? 0xFFF0000000000000ULL : 0xFF800000ULL;
    Ref Inf = FPConstant(IsMax ? NegInf : PosInf, ElementSize);
    Ref ReplaceA = _VAndn(RS, RS, QuietNaNA, QuietNaNB);
    Ref ReplaceB = _VAndn(RS, RS, QuietNaNB, QuietNaNA);
    Ref NewA = _VBSL(RS, ReplaceA, Inf, A);
    B = _VBSL(RS, ReplaceB, Inf, B);
    A = NewA;
  }

  Ref Unordered = _VFCMPUNO(RS, ElementSize, A, B);
  Ref NaNResult = _VFAdd(RS, ElementSize, PropagateNaNOperand(ElementSize, A, B), B);
  Ref ALess = _VFCMPLT(RS, ElementSize, A, B);
  Ref BLess = _VFCMPLT(RS, ElementSize, B, A);
  // Equal operands: -0 and +0 are the only distinct bit patterns, and OR
  // (min) or AND (max) of the two picks the right one.
  Ref Result = IsMax ? _VAnd(RS, RS, A, B).Node : _VOr(RS, RS, A, B).Node;
  Result = _VBSL(RS, ALess, IsMax ? B : A, Result);
  Result = _VBSL(RS, BLess, IsMax ? A : B, Result);
  return _VBSL(RS, Unordered, NaNResult, Result);
}

// ---------------------------------------------------------------------------
// Moves and immediates
// ---------------------------------------------------------------------------

bool IRBuilder::FMOV_float_gen(uint32_t Word) {
  const bool Sf = Bit(Word, 31);
  const uint32_t Type = Bits(Word, 23, 22);
  const bool RMode1 = Bit(Word, 19);
  const bool ToFP = Bit(Word, 16);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rd = Bits(Word, 4, 0);

  OpSize Size {};
  uint8_t Index = 0;
  if (!Sf && Type == 0b00 && !RMode1) {
    Size = OpSize::i32Bit;
  } else if (Sf && Type == 0b01 && !RMode1) {
    Size = OpSize::i64Bit;
  } else if (Sf && Type == 0b10 && RMode1) {
    Size = OpSize::i64Bit;
    Index = 1;
  } else if (Type == 0b11 && !RMode1) {
    Size = OpSize::i16Bit;
  } else {
    return false;
  }

  if (ToFP) {
    Ref Src = LoadX(Rn);
    if (Index == 0) {
      StoreV(Rd, _VCastFromGPR(OpSize::i128Bit, Size, Src));
    } else {
      StoreV(Rd, _VInsGPR(OpSize::i128Bit, OpSize::i64Bit, 1, LoadV(Rd), Src));
    }
  } else {
    StoreReg(Rd, Sf, _VExtractToGPR(OpSize::i128Bit, Size, LoadV(Rn), Index));
  }
  return true;
}

bool IRBuilder::FMOV_float(uint32_t Word) {
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size)) {
    return false;
  }
  StoreVSized(Bits(Word, 4, 0), Size, LoadV(Bits(Word, 9, 5)));
  return true;
}

bool IRBuilder::FMOV_float_imm(uint32_t Word) {
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size) || Bits(Word, 9, 5) != 0) {
    return false;
  }
  const uint64_t Bits64 = VFPExpandImm(Bits(Word, 20, 13), Size);
  StoreV(Bits(Word, 4, 0), _VCastFromGPR(OpSize::i128Bit, Size, Constant(Bits64)));
  return true;
}

// ---------------------------------------------------------------------------
// One register
// ---------------------------------------------------------------------------

bool IRBuilder::FPOneRegister(uint32_t Word, FPUnaryOp Op) {
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size)) {
    return false;
  }
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Result {};
  if (Size == OpSize::i16Bit) {
    // FABS and FNEG are sign-bit operations with no unpacking.
    switch (Op) {
    case FPUnaryOp::Abs: Result = _VAnd(RS, RS, V, FPConstant(0x7FFF, Size)); break;
    case FPUnaryOp::Neg: Result = _VXor(RS, RS, V, FPConstant(0x8000, Size)); break;
    case FPUnaryOp::Sqrt: Result = DoubleToHalf(_VFSqrt(RS, OpSize::i64Bit, HalfToDouble(V, false, true)), true); break;
    }
    StoreVSized(Bits(Word, 4, 0), Size, Result);
    return true;
  }
  switch (Op) {
  case FPUnaryOp::Abs: Result = _VFAbs(RS, Size, V); break;
  case FPUnaryOp::Neg: Result = _VFNeg(RS, Size, V); break;
  case FPUnaryOp::Sqrt: Result = _VFSqrt(RS, Size, V); break;
  }
  StoreVSized(Bits(Word, 4, 0), Size, Result);
  return true;
}

bool IRBuilder::FABS_float(uint32_t Word) { return FPOneRegister(Word, FPUnaryOp::Abs); }
bool IRBuilder::FNEG_float(uint32_t Word) { return FPOneRegister(Word, FPUnaryOp::Neg); }
bool IRBuilder::FSQRT_float(uint32_t Word) { return FPOneRegister(Word, FPUnaryOp::Sqrt); }

// Round to integral in the given mode (FPRoundInt). Directed modes and the
// FPCR mode (FRINTX, FRINTI) are Vector_FToI. There is no IR mode for ties
// away (FRINTA) or for ties to even independent of FPCR.RMode (FRINTN), so
// those start from the truncation T and the exact remainder D = X - T:
// |D| > 1/2 rounds away from zero, |D| == 1/2 is the tie. The step is a
// select, never an addition of zero, so the sign of a zero result is T's.
Ref IRBuilder::FPRoundToIntegral(Ref X, OpSize ElementSize, FPRounding Mode) {
  const auto RS = OpSize::i128Bit;
  switch (Mode) {
  case FPRounding::PosInf: return _Vector_FToI(RS, ElementSize, X, RoundMode::PosInfinity);
  case FPRounding::NegInf: return _Vector_FToI(RS, ElementSize, X, RoundMode::NegInfinity);
  case FPRounding::Zero: return _Vector_FToI(RS, ElementSize, X, RoundMode::TowardsZero);
  case FPRounding::Current: return _Vector_FToI(RS, ElementSize, X, RoundMode::Host);
  case FPRounding::TiesAway:
  case FPRounding::TiesEven: break;
  }
  const bool Double = ElementSize == OpSize::i64Bit;
  const uint64_t Half = Double ? 0x3FE0000000000000ULL : 0x3F000000ULL;
  const uint64_t One = Double ? 0x3FF0000000000000ULL : 0x3F800000ULL;
  const uint64_t Sign = Double ? 0x8000000000000000ULL : 0x80000000ULL;
  Ref T = _Vector_FToI(RS, ElementSize, X, RoundMode::TowardsZero);
  Ref AbsD = _VFAbs(RS, ElementSize, _VFSub(RS, ElementSize, X, T));
  // +-1 with the sign of X.
  Ref Step = _VOr(RS, RS, _VAnd(RS, RS, X, FPConstant(Sign, ElementSize)), FPConstant(One, ElementSize));
  Ref Away = _VFAdd(RS, ElementSize, T, Step);
  Ref Above = _VFCMPLT(RS, ElementSize, FPConstant(Half, ElementSize), AbsD);
  Ref Tie = _VFCMPEQ(RS, ElementSize, AbsD, FPConstant(Half, ElementSize));
  Ref TakeAway = Above;
  if (Mode == FPRounding::TiesAway) {
    TakeAway = _VOr(RS, RS, Above, Tie);
  } else {
    // A tie rounds to whichever of T and T +- 1 is even; T is even when
    // trunc(T / 2) * 2 == T. Ties only exist below 2^52, where this is exact.
    Ref HalfT = _VFMul(RS, ElementSize, T, FPConstant(Half, ElementSize));
    Ref TEven = _VFCMPEQ(RS, ElementSize, _VFAdd(RS, ElementSize, _Vector_FToI(RS, ElementSize, HalfT, RoundMode::TowardsZero),
                                                 _Vector_FToI(RS, ElementSize, HalfT, RoundMode::TowardsZero)),
                         T);
    TakeAway = _VOr(RS, RS, Above, _VAnd(RS, RS, Tie, _VNot(RS, ElementSize, TEven)));
  }
  return _VBSL(RS, TakeAway, Away, T);
}

bool IRBuilder::FPRoundInt(uint32_t Word, FPRounding Mode) {
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size)) {
    return false;
  }
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Result {};
  if (Size == OpSize::i16Bit) {
    // An integral half-precision value is exact in double, so round there.
    Result = DoubleToHalf(FPRoundToIntegral(HalfToDouble(V, false, true), OpSize::i64Bit, Mode), true);
  } else {
    Result = FPRoundToIntegral(V, Size, Mode);
  }
  StoreVSized(Bits(Word, 4, 0), Size, Result);
  return true;
}

bool IRBuilder::FRINTN_float(uint32_t Word) { return FPRoundInt(Word, FPRounding::TiesEven); }
bool IRBuilder::FRINTP_float(uint32_t Word) { return FPRoundInt(Word, FPRounding::PosInf); }
bool IRBuilder::FRINTM_float(uint32_t Word) { return FPRoundInt(Word, FPRounding::NegInf); }
bool IRBuilder::FRINTZ_float(uint32_t Word) { return FPRoundInt(Word, FPRounding::Zero); }
bool IRBuilder::FRINTA_float(uint32_t Word) { return FPRoundInt(Word, FPRounding::TiesAway); }
// POWERARM-M1-TODO(fpu): FRINTX should also set FPSR.IXC when the result differs; cumulative FPSR exception flags are not emulated.
bool IRBuilder::FRINTX_float(uint32_t Word) { return FPRoundInt(Word, FPRounding::Current); }
bool IRBuilder::FRINTI_float(uint32_t Word) { return FPRoundInt(Word, FPRounding::Current); }

bool IRBuilder::FCVT_float(uint32_t Word) {
  OpSize SrcSize {}, DstSize {};
  if (!FPTypeSize(Bits(Word, 23, 22), &SrcSize) || !FPTypeSize(Bits(Word, 16, 15), &DstSize) || SrcSize == DstSize) {
    return false;
  }
  Ref V = LoadV(Bits(Word, 9, 5));
  // Half precision goes through double: widening is exact and the narrowing
  // to half rounds once.
  Ref Double {};
  switch (SrcSize) {
  case OpSize::i16Bit: Double = HalfToDouble(V, false, false); break;
  case OpSize::i32Bit: Double = _A64FToF(OpSize::i64Bit, OpSize::i32Bit, V); break;
  default: Double = V; break;
  }
  Ref Result {};
  switch (DstSize) {
  case OpSize::i16Bit: Result = DoubleToHalf(Double, false); break;
  case OpSize::i32Bit: Result = _A64FToF(OpSize::i32Bit, OpSize::i64Bit, Double); break;
  default: Result = Double; break;
  }
  StoreVSized(Bits(Word, 4, 0), DstSize, Result);
  return true;
}

// ---------------------------------------------------------------------------
// Two register
// ---------------------------------------------------------------------------

bool IRBuilder::FPTwoRegister(uint32_t Word, FPBinaryOp Op) {
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size)) {
    return false;
  }
  const auto RS = OpSize::i128Bit;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));
  const bool Half = Size == OpSize::i16Bit;
  if (Half) {
    A = HalfToDouble(A, true, true);
    B = HalfToDouble(B, true, true);
    Size = OpSize::i64Bit;
  }

  Ref Result {};
  switch (Op) {
  case FPBinaryOp::Add:
  case FPBinaryOp::Sub:
  case FPBinaryOp::Mul:
  case FPBinaryOp::Div: Result = A64Arith(Size, A, B, Op); break;
  // FPNeg flips a NaN's sign too, so the negation must follow the arithmetic
  // rather than being folded into a host negating form (checklist B1).
  case FPBinaryOp::NMul: Result = _VFNeg(RS, Size, A64Arith(Size, A, B, FPBinaryOp::Mul)); break;
  case FPBinaryOp::Min: Result = FPMinMax(Size, A, B, false, false); break;
  case FPBinaryOp::Max: Result = FPMinMax(Size, A, B, true, false); break;
  case FPBinaryOp::MinNum: Result = FPMinMax(Size, A, B, false, true); break;
  case FPBinaryOp::MaxNum: Result = FPMinMax(Size, A, B, true, true); break;
  }
  if (Half) {
    StoreVSized(Bits(Word, 4, 0), OpSize::i16Bit, DoubleToHalf(Result, true));
    return true;
  }
  StoreVSized(Bits(Word, 4, 0), Size, Result);
  return true;
}

bool IRBuilder::FADD_float(uint32_t Word) { return FPTwoRegister(Word, FPBinaryOp::Add); }
bool IRBuilder::FSUB_float(uint32_t Word) { return FPTwoRegister(Word, FPBinaryOp::Sub); }
bool IRBuilder::FMUL_float(uint32_t Word) { return FPTwoRegister(Word, FPBinaryOp::Mul); }
bool IRBuilder::FDIV_float(uint32_t Word) { return FPTwoRegister(Word, FPBinaryOp::Div); }
bool IRBuilder::FNMUL_float(uint32_t Word) { return FPTwoRegister(Word, FPBinaryOp::NMul); }
bool IRBuilder::FMIN_float(uint32_t Word) { return FPTwoRegister(Word, FPBinaryOp::Min); }
bool IRBuilder::FMAX_float(uint32_t Word) { return FPTwoRegister(Word, FPBinaryOp::Max); }
bool IRBuilder::FMINNM_float(uint32_t Word) { return FPTwoRegister(Word, FPBinaryOp::MinNum); }
bool IRBuilder::FMAXNM_float(uint32_t Word) { return FPTwoRegister(Word, FPBinaryOp::MaxNum); }

// FPMulAdd(A, N, M) in every Size lane: one fused A + N*M with A64 NaN
// operand precedence (first signalling NaN of A, N, M, else the first quiet
// NaN) and the default NaN for a quiet NaN addend with an Inf*0 product.
// Negations of A or N are applied by the caller before the call.
Ref IRBuilder::FPMulAddLanes(OpSize Size, Ref A, Ref N, Ref M) {
  const auto RS = OpSize::i128Bit;
  Ref Fused = _VFMLA(RS, Size, N, M, A);

  const bool Is64 = Size == OpSize::i64Bit;
  Ref Quiet = FPConstant(QuietBit(Size), Size);
  auto IsNaN = [&](Ref V) -> Ref {
    return _VFCMPUNO(RS, Size, V, V);
  };
  auto QuietBitSet = [&](Ref V) -> Ref {
    return _VNot(RS, Size, _VCMPEQZ(RS, Size, _VAnd(RS, RS, V, Quiet)));
  };
  Ref NaNA = IsNaN(A), NaNN = IsNaN(N), NaNM = IsNaN(M);
  Ref QBitA = QuietBitSet(A), QBitN = QuietBitSet(N), QBitM = QuietBitSet(M);
  Ref NaNResult = M;
  NaNResult = _VBSL(RS, _VAnd(RS, RS, NaNN, QBitN), N, NaNResult);
  NaNResult = _VBSL(RS, _VAnd(RS, RS, NaNA, QBitA), A, NaNResult);
  NaNResult = _VBSL(RS, _VAndn(RS, RS, NaNM, QBitM), M, NaNResult);
  NaNResult = _VBSL(RS, _VAndn(RS, RS, NaNN, QBitN), N, NaNResult);
  NaNResult = _VBSL(RS, _VAndn(RS, RS, NaNA, QBitA), A, NaNResult);
  NaNResult = _VOr(RS, RS, NaNResult, Quiet);
  Ref AnyNaN = _VOr(RS, RS, NaNA, _VOr(RS, RS, NaNN, NaNM));
  Ref Result = _VBSL(RS, AnyNaN, NaNResult, Fused);

  Ref Zero = _VectorImm(RS, OpSize::i8Bit, 0);
  Ref Inf = FPConstant(Is64 ? 0x7FF0000000000000ULL : 0x7F800000ULL, Size);
  Ref InfN = _VFCMPEQ(RS, Size, _VFAbs(RS, Size, N), Inf);
  Ref InfM = _VFCMPEQ(RS, Size, _VFAbs(RS, Size, M), Inf);
  Ref ZeroN = _VFCMPEQ(RS, Size, N, Zero);
  Ref ZeroM = _VFCMPEQ(RS, Size, M, Zero);
  Ref InfTimesZero = _VOr(RS, RS, _VAnd(RS, RS, InfN, ZeroM), _VAnd(RS, RS, ZeroN, InfM));
  Ref DefaultCase = _VAnd(RS, RS, _VAnd(RS, RS, NaNA, QBitA), InfTimesZero);
  Ref DefaultNaN = FPConstant(Is64 ? 0x7FF8000000000000ULL : 0x7FC00000ULL, Size);
  Result = _VBSL(RS, DefaultCase, DefaultNaN, Result);
  return Result;
}

bool IRBuilder::FPThreeRegister(uint32_t Word) {
  // A64 defines the group as one fused FPMulAdd with negated operands:
  // FMADD a+n*m, FMSUB a+(-n)*m, FNMADD (-a)+(-n)*m, FNMSUB (-a)+n*m. The
  // host's negating forms negate after rounding, which differs under the
  // directed rounding modes and in the sign of a NaN result, so the
  // negations go on the operands and the host op is always the plain one.
  //
  // NaNs: A64 picks the first signalling NaN of (addend, n, m), else the
  // first quiet NaN, and a quiet NaN addend with an Inf*0 product gives the
  // default NaN. The host op's precedence differs, so NaN lanes are
  // overridden.
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size)) {
    return false;
  }
  const auto RS = OpSize::i128Bit;
  const bool NegateAddend = Bit(Word, 21);
  const bool NegateOperand = Bit(Word, 21) != Bit(Word, 15);
  Ref N = LoadV(Bits(Word, 9, 5));
  Ref M = LoadV(Bits(Word, 20, 16));
  Ref A = LoadV(Bits(Word, 14, 10));
  const bool Half = Size == OpSize::i16Bit;
  if (Half) {
    N = HalfToDouble(N, true, true);
    M = HalfToDouble(M, true, true);
    A = HalfToDouble(A, true, true);
    Size = OpSize::i64Bit;
  }
  if (NegateOperand) {
    N = _VFNeg(RS, Size, N);
  }
  if (NegateAddend) {
    A = _VFNeg(RS, Size, A);
  }
  Ref Result = FPMulAddLanes(Size, A, N, M);
  if (Half) {
    StoreVSized(Bits(Word, 4, 0), OpSize::i16Bit, DoubleToHalf(Result, true));
    return true;
  }
  StoreVSized(Bits(Word, 4, 0), Size, Result);
  return true;
}

// ---------------------------------------------------------------------------
// Compare and select
// ---------------------------------------------------------------------------

bool IRBuilder::FCMP_float(uint32_t Word) {
  // FCMP and FCMPE set the same flags; they differ only in which NaNs raise
  // Invalid Operation.
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size)) {
    return false;
  }
  const bool WithZero = Bit(Word, 3);
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = WithZero ? _VectorImm(OpSize::i128Bit, OpSize::i8Bit, 0).Node : LoadV(Bits(Word, 20, 16));
  if (Size == OpSize::i16Bit) {
    A = HalfToDouble(A, false, true);
    B = HalfToDouble(B, false, true);
    Size = OpSize::i64Bit;
  }
  _FCmp(Size, A, B);
  return true;
}

bool IRBuilder::FCCMP_float(uint32_t Word) {
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size)) {
    return false;
  }
  const uint32_t Cond = Bits(Word, 15, 12);
  const uint64_t FalseNZCV = Bits(Word, 3, 0);
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));
  if (Size == OpSize::i16Bit) {
    A = HalfToDouble(A, false, true);
    B = HalfToDouble(B, false, true);
    Size = OpSize::i64Bit;
  }
  if (Cond >= 0b1110) {
    _FCmp(Size, A, B);
    return true;
  }
  // The condition reads the incoming NZCV, so it is evaluated before the compare.
  Ref Take = _NZCVSelect(OpSize::i64Bit, MapCondition(Cond), Constant(1), Constant(0));
  _FCmp(Size, A, B);
  Ref Compared = _LoadNZCV();
  _StoreNZCV(_Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::NEQ, Take, Constant(0), Compared, Constant(FalseNZCV << 28)));
  return true;
}

bool IRBuilder::FCSEL_float(uint32_t Word) {
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size)) {
    return false;
  }
  const uint32_t Cond = Bits(Word, 15, 12);
  Ref A = LoadV(Bits(Word, 9, 5));
  if (Cond >= 0b1110) {
    StoreVSized(Bits(Word, 4, 0), Size, A);
    return true;
  }
  Ref B = LoadV(Bits(Word, 20, 16));
  StoreVSized(Bits(Word, 4, 0), Size, _NZCVSelectV(OpSize::i128Bit, MapCondition(Cond), A, B));
  return true;
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

bool IRBuilder::FPConvertToInt(uint32_t Word, uint8_t Rounding, bool Signed) {
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size)) {
    return false;
  }
  const bool Sf = Bit(Word, 31);
  Ref V = LoadV(Bits(Word, 9, 5));
  if (Size == OpSize::i16Bit) {
    V = HalfToDouble(V, false, true);
    Size = OpSize::i64Bit;
  }
  Ref Result = _A64FloatToGPR(SizeFor(Sf), Size, V, Rounding, Signed);
  StoreReg(Bits(Word, 4, 0), Sf, Result);
  return true;
}

bool IRBuilder::FCVTNS_float(uint32_t Word) { return FPConvertToInt(Word, ROUND_TIEEVEN, true); }
bool IRBuilder::FCVTNU_float(uint32_t Word) { return FPConvertToInt(Word, ROUND_TIEEVEN, false); }
bool IRBuilder::FCVTPS_float(uint32_t Word) { return FPConvertToInt(Word, ROUND_POSINF, true); }
bool IRBuilder::FCVTPU_float(uint32_t Word) { return FPConvertToInt(Word, ROUND_POSINF, false); }
bool IRBuilder::FCVTMS_float(uint32_t Word) { return FPConvertToInt(Word, ROUND_NEGINF, true); }
bool IRBuilder::FCVTMU_float(uint32_t Word) { return FPConvertToInt(Word, ROUND_NEGINF, false); }
bool IRBuilder::FCVTZS_float_int(uint32_t Word) { return FPConvertToInt(Word, ROUND_ZERO, true); }
bool IRBuilder::FCVTZU_float_int(uint32_t Word) { return FPConvertToInt(Word, ROUND_ZERO, false); }
bool IRBuilder::FCVTAS_float(uint32_t Word) { return FPConvertToInt(Word, ROUND_TIEAWAY, true); }
bool IRBuilder::FCVTAU_float(uint32_t Word) { return FPConvertToInt(Word, ROUND_TIEAWAY, false); }

bool IRBuilder::FPConvertFromInt(uint32_t Word, bool Signed, bool Fixed) {
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size)) {
    return false;
  }
  const bool Sf = Bit(Word, 31);
  const uint32_t Scale = Bits(Word, 15, 10);
  if (Fixed && !Sf && Scale < 32) {
    return false;
  }
  // Half precision converts through double: every integer that can round to
  // a finite half-precision value is exact in double.
  // POWERARM-M1-TODO(fpu): a fixed-point SCVTF/UCVTF to half precision of an integer above 2^53 rounds twice.
  const bool Half = Size == OpSize::i16Bit;
  const auto ConvSize = Half ? OpSize::i64Bit : Size;
  Ref Result = _A64FloatFromGPR(ConvSize, SizeFor(Sf), LoadX(Bits(Word, 9, 5)), Signed);
  if (Fixed && Scale != 64) {
    // Scaling by a power of two after the single rounding is exact.
    const uint32_t FBits = 64 - Scale;
    const bool Is64 = ConvSize == OpSize::i64Bit;
    const uint64_t InvScale = Is64 ? (static_cast<uint64_t>(1023 - FBits) << 52) : (static_cast<uint64_t>(127 - FBits) << 23);
    Result = _VMov(ConvSize, _VFMul(OpSize::i128Bit, ConvSize, Result, FPConstant(InvScale, ConvSize)));
  }
  if (Half) {
    Result = DoubleToHalf(Result, true);
  }
  StoreV(Bits(Word, 4, 0), Result);
  return true;
}

bool IRBuilder::SCVTF_float_int(uint32_t Word) { return FPConvertFromInt(Word, true, false); }
bool IRBuilder::UCVTF_float_int(uint32_t Word) { return FPConvertFromInt(Word, false, false); }
bool IRBuilder::SCVTF_float_fix(uint32_t Word) { return FPConvertFromInt(Word, true, true); }
bool IRBuilder::UCVTF_float_fix(uint32_t Word) { return FPConvertFromInt(Word, false, true); }

bool IRBuilder::FPConvertToFixed(uint32_t Word, bool Signed) {
  OpSize Size {};
  if (!FPTypeSize(Bits(Word, 23, 22), &Size)) {
    return false;
  }
  const bool Sf = Bit(Word, 31);
  const uint32_t Scale = Bits(Word, 15, 10);
  if (!Sf && Scale < 32) {
    return false;
  }
  Ref V = LoadV(Bits(Word, 9, 5));
  if (Size == OpSize::i16Bit) {
    V = HalfToDouble(V, false, true);
    Size = OpSize::i64Bit;
  }
  if (Scale != 64) {
    // Multiplying by 2^fbits is exact or overflows to an infinity, which
    // saturates the same way the exact product would.
    const uint32_t FBits = 64 - Scale;
    const bool Is64 = Size == OpSize::i64Bit;
    const uint64_t ScaleBits = Is64 ? (static_cast<uint64_t>(1023 + FBits) << 52) : (static_cast<uint64_t>(127 + FBits) << 23);
    V = _VFMul(OpSize::i128Bit, Size, V, FPConstant(ScaleBits, Size));
  }
  StoreReg(Bits(Word, 4, 0), Sf, _A64FloatToGPR(SizeFor(Sf), Size, V, ROUND_ZERO, Signed));
  return true;
}

bool IRBuilder::FCVTZS_float_fix(uint32_t Word) { return FPConvertToFixed(Word, true); }
bool IRBuilder::FCVTZU_float_fix(uint32_t Word) { return FPConvertToFixed(Word, false); }

// Scalar SIMD forms: the integer lives in the same-width vector register.
bool IRBuilder::FPScalarSIMDConvert(uint32_t Word, bool ToInt, bool Signed) {
  const auto Size = Bit(Word, 22) ? OpSize::i64Bit : OpSize::i32Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Result {};
  if (ToInt) {
    Result = _VCastFromGPR(OpSize::i128Bit, Size, _A64FloatToGPR(Size, Size, V, ROUND_ZERO, Signed));
  } else {
    Result = _A64FloatFromGPR(Size, Size, _VExtractToGPR(OpSize::i128Bit, Size, V, 0), Signed);
  }
  StoreV(Bits(Word, 4, 0), Result);
  return true;
}

bool IRBuilder::FCVTZS_int_2(uint32_t Word) { return FPScalarSIMDConvert(Word, true, true); }
bool IRBuilder::FCVTZU_int_2(uint32_t Word) { return FPScalarSIMDConvert(Word, true, false); }
bool IRBuilder::SCVTF_int_2(uint32_t Word) { return FPScalarSIMDConvert(Word, false, true); }
bool IRBuilder::UCVTF_int_2(uint32_t Word) { return FPScalarSIMDConvert(Word, false, false); }

// ---------------------------------------------------------------------------
// Vector FNEG/FABS/FABD, scalar FABD, vector SCVTF/UCVTF
// ---------------------------------------------------------------------------

bool IRBuilder::FPVectorUnary(uint32_t Word, bool IsNeg) {
  const bool Q = Bit(Word, 30);
  const bool Z = Bit(Word, 22);
  if (Z && !Q) {
    return false;
  }
  const auto ES = Z ? OpSize::i64Bit : OpSize::i32Bit;
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  StoreVQ(Bits(Word, 4, 0), Q, IsNeg ? _VFNeg(RS, ES, V).Node : _VFAbs(RS, ES, V).Node);
  return true;
}

bool IRBuilder::FNEG_2(uint32_t Word) { return FPVectorUnary(Word, true); }
bool IRBuilder::FABS_2(uint32_t Word) { return FPVectorUnary(Word, false); }

bool IRBuilder::FPAbsoluteDifference(uint32_t Word, bool Scalar) {
  // FABD = FPAbs(FPSub(a, b)); FPAbs clears the sign of a NaN too.
  const bool Q = Scalar || Bit(Word, 30);
  const bool Z = Bit(Word, 22);
  if (!Scalar && Z && !Q) {
    return false;
  }
  const auto ES = Z ? OpSize::i64Bit : OpSize::i32Bit;
  const auto RS = OpSize::i128Bit;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));
  Ref Result = _VFAbs(RS, ES, A64Arith(ES, A, B, FPBinaryOp::Sub));
  if (Scalar) {
    StoreVSized(Bits(Word, 4, 0), ES, Result);
  } else {
    StoreVQ(Bits(Word, 4, 0), Q, Result);
  }
  return true;
}

bool IRBuilder::FABD_2(uint32_t Word) { return FPAbsoluteDifference(Word, true); }
bool IRBuilder::FABD_4(uint32_t Word) { return FPAbsoluteDifference(Word, false); }

bool IRBuilder::FPVectorIntToFloat(uint32_t Word, bool Signed) {
  // Lane by lane through the scalar conversion, which rounds with FPCR.RMode.
  const bool Q = Bit(Word, 30);
  const bool Z = Bit(Word, 22);
  if (Z && !Q) {
    return false;
  }
  const auto ES = Z ? OpSize::i64Bit : OpSize::i32Bit;
  const auto RS = OpSize::i128Bit;
  const uint8_t Lanes = (Q ? 16 : 8) / IR::OpSizeToSize(ES);
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Result = _VectorImm(RS, OpSize::i8Bit, 0);
  for (uint8_t i = 0; i < Lanes; ++i) {
    Ref Converted = _A64FloatFromGPR(ES, ES, _VExtractToGPR(RS, ES, V, i), Signed);
    Result = _VInsElement(RS, ES, i, 0, Result, Converted);
  }
  StoreV(Bits(Word, 4, 0), Result);
  return true;
}

bool IRBuilder::SCVTF_int_4(uint32_t Word) { return FPVectorIntToFloat(Word, true); }
bool IRBuilder::UCVTF_int_4(uint32_t Word) { return FPVectorIntToFloat(Word, false); }

// ---------------------------------------------------------------------------
// Vector FCVTL/FCVTN
// ---------------------------------------------------------------------------

bool IRBuilder::FCVTL(uint32_t Word) {
  // z=0: half -> single, z=1: single -> double; Q selects the upper half of
  // the source (FCVTL2).
  const bool Q = Bit(Word, 30);
  const bool Z = Bit(Word, 22);
  const auto RS = OpSize::i128Bit;
  const auto SrcES = Z ? OpSize::i32Bit : OpSize::i16Bit;
  const auto DstES = Z ? OpSize::i64Bit : OpSize::i32Bit;
  const uint8_t Count = Z ? 2 : 4;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Result = _VectorImm(RS, OpSize::i8Bit, 0);
  for (uint8_t i = 0; i < Count; ++i) {
    Ref Element = _VDupElement(RS, SrcES, V, (Q ? Count : 0) + i);
    Ref Converted = Z ? _A64FToF(OpSize::i64Bit, OpSize::i32Bit, Element).Node :
                        _A64FToF(OpSize::i32Bit, OpSize::i64Bit, HalfToDouble(Element, false, false)).Node;
    Result = _VInsElement(RS, DstES, i, 0, Result, Converted);
  }
  StoreV(Bits(Word, 4, 0), Result);
  return true;
}

bool IRBuilder::FCVTN(uint32_t Word) {
  // z=0: single -> half, z=1: double -> single; Q writes the upper half and
  // keeps the lower (FCVTN2).
  const bool Q = Bit(Word, 30);
  const bool Z = Bit(Word, 22);
  const auto RS = OpSize::i128Bit;
  const auto SrcES = Z ? OpSize::i64Bit : OpSize::i32Bit;
  const auto DstES = Z ? OpSize::i32Bit : OpSize::i16Bit;
  const uint8_t Count = Z ? 2 : 4;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Narrow = _VectorImm(RS, OpSize::i8Bit, 0);
  for (uint8_t i = 0; i < Count; ++i) {
    Ref Element = _VDupElement(RS, SrcES, V, i);
    Ref Converted = Z ? _A64FToF(OpSize::i32Bit, OpSize::i64Bit, Element).Node :
                        DoubleToHalf(_A64FToF(OpSize::i64Bit, OpSize::i32Bit, Element), false);
    Narrow = _VInsElement(RS, DstES, i, 0, Narrow, Converted);
  }
  StoreNarrow(Bits(Word, 4, 0), Q, Narrow);
  return true;
}

// ---------------------------------------------------------------------------
// FPCR
// ---------------------------------------------------------------------------

void IRBuilder::SyncHostRoundingMode(Ref FPCR) {
  // FPCR.RMode (bits 23:22: nearest, +Inf, -Inf, zero) in SetRoundingMode's
  // encoding (nearest, down, up, truncate): swap the two directed modes.
  // POWERARM-M1-TODO(fpu): FPCR.FZ and FPCR.DN are stored but have no host effect; POWER has no flush-to-zero for VSX and default-NaN mode needs explicit canonicalization.
  Ref RMode = _Bfe(OpSize::i64Bit, 2, 22, FPCR);
  Ref Swapped = _Or(OpSize::i64Bit, _Lshl(OpSize::i64Bit, _And(OpSize::i64Bit, RMode, Constant(1)), Constant(1)),
                    _Lshr(OpSize::i64Bit, RMode, Constant(1)));
  _SetRoundingMode(Swapped, false, Swapped);
}

} // namespace FEXCore::A64
