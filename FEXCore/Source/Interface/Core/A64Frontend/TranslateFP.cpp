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
// Half precision (type 11) is not translated yet.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

#include <FEXCore/Core/CoreState.h>

namespace FEXCore::A64 {
using namespace FEXCore::IR;

namespace {
  // FP type field (bits 23:22) -> element size. Returns false for the
  // unallocated type and, for now, half precision.
  bool FPTypeSize(uint32_t Type, OpSize* Size) {
    switch (Type) {
    case 0b00: *Size = OpSize::i32Bit; return true;
    case 0b01: *Size = OpSize::i64Bit; return true;
    // POWERARM-M1-TODO(fpu): half precision (type 11) is not translated; FP16 is to be presented once it is.
    default: return false;
    }
  }

  // A64 VFPExpandImm for single or double precision.
  uint64_t VFPExpandImm(uint64_t Imm8, bool IsDouble) {
    const uint64_t Sign = (Imm8 >> 7) & 1;
    const uint64_t B6 = (Imm8 >> 6) & 1;
    if (IsDouble) {
      return (Sign << 63) | ((B6 ^ 1) << 62) | ((B6 ? 0xFFULL : 0) << 54) | ((Imm8 & 0x3F) << 48);
    }
    return (Sign << 31) | ((B6 ^ 1) << 30) | ((B6 ? 0x1FULL : 0) << 25) | ((Imm8 & 0x3F) << 19);
  }

  uint64_t QuietBit(OpSize ElementSize) {
    return ElementSize == OpSize::i64Bit ? 1ULL << 51 : 1ULL << 22;
  }

  uint64_t Replicate(uint64_t Value, OpSize ElementSize) {
    return ElementSize == OpSize::i64Bit ? Value : (Value << 32) | Value;
  }

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

Ref IRBuilder::PropagateNaNOperand(OpSize ElementSize, Ref A, Ref B) {
  // Lanes where A is a quiet NaN and B a signalling NaN take B for A.
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
  } else {
    // POWERARM-M1-TODO(fpu): FMOV between general-purpose and half-precision registers.
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
  const uint64_t Bits64 = VFPExpandImm(Bits(Word, 20, 13), Size == OpSize::i64Bit);
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

bool IRBuilder::FCVT_float(uint32_t Word) {
  OpSize SrcSize {}, DstSize {};
  if (!FPTypeSize(Bits(Word, 23, 22), &SrcSize) || !FPTypeSize(Bits(Word, 16, 15), &DstSize) || SrcSize == DstSize) {
    return false;
  }
  StoreV(Bits(Word, 4, 0), _VMov(DstSize, _A64FToF(DstSize, SrcSize, LoadV(Bits(Word, 9, 5)))));
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

  Ref Result {};
  switch (Op) {
  case FPBinaryOp::Add: Result = _VFAdd(RS, Size, PropagateNaNOperand(Size, A, B), B); break;
  case FPBinaryOp::Sub: Result = _VFSub(RS, Size, PropagateNaNOperand(Size, A, B), B); break;
  case FPBinaryOp::Mul: Result = _VFMul(RS, Size, PropagateNaNOperand(Size, A, B), B); break;
  case FPBinaryOp::Div: Result = _VFDiv(RS, Size, PropagateNaNOperand(Size, A, B), B); break;
  case FPBinaryOp::NMul: Result = _VFNeg(RS, Size, _VFMul(RS, Size, PropagateNaNOperand(Size, A, B), B)); break;
  case FPBinaryOp::Min: Result = FPMinMax(Size, A, B, false, false); break;
  case FPBinaryOp::Max: Result = FPMinMax(Size, A, B, true, false); break;
  case FPBinaryOp::MinNum: Result = FPMinMax(Size, A, B, false, true); break;
  case FPBinaryOp::MaxNum: Result = FPMinMax(Size, A, B, true, true); break;
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
  if (NegateOperand) {
    N = _VFNeg(RS, Size, N);
  }
  if (NegateAddend) {
    A = _VFNeg(RS, Size, A);
  }
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
  Ref Result = _A64FloatToGPR(SizeFor(Sf), Size, LoadV(Bits(Word, 9, 5)), Rounding, Signed);
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
  Ref Result = _A64FloatFromGPR(Size, SizeFor(Sf), LoadX(Bits(Word, 9, 5)), Signed);
  if (Fixed && Scale != 64) {
    // Scaling by a power of two after the single rounding is exact.
    const uint32_t FBits = 64 - Scale;
    const bool Is64 = Size == OpSize::i64Bit;
    const uint64_t InvScale = Is64 ? (static_cast<uint64_t>(1023 - FBits) << 52) : (static_cast<uint64_t>(127 - FBits) << 23);
    Result = _VMov(Size, _VFMul(OpSize::i128Bit, Size, Result, FPConstant(InvScale, Size)));
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
