// SPDX-License-Identifier: MIT
//
// A64 Advanced SIMD integer families built on saturation, rounding and
// halving: saturating add/subtract/absolute/negate, saturating and rounding
// narrows (SQXTN, SQSHRN, RSHRN, ...), rounding and saturating shifts by
// immediate, halving adds, absolute differences, rounding high narrows, the
// doubling multiplies (SQDMULH/SQRDMULH), by-element multiplies,
// across-lane long adds and signed min/max, CLZ/CLS and the dot products.
//
// Saturating operations set the cumulative FPSR.QC (bit 27) when any lane
// they write saturated, computed from the lanes that differ between the
// saturated and the wrapping result.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

#include <FEXCore/Core/CoreState.h>

#include <bit>

namespace FEXCore::A64 {
using namespace FEXCore::IR;

namespace {
  OpSize LaneSize(uint32_t SizeLog2) {
    return IR::SizeToOpSize(1U << SizeLog2);
  }

  // Value replicated into every ES lane of a 64-bit half.
  uint64_t ReplicateLane(uint64_t Value, OpSize ES) {
    switch (ES) {
    case OpSize::i8Bit: return (Value & 0xFF) * 0x0101010101010101ULL;
    case OpSize::i16Bit: return (Value & 0xFFFF) * 0x0001000100010001ULL;
    case OpSize::i32Bit: return ((Value & 0xFFFFFFFFULL) << 32) | (Value & 0xFFFFFFFFULL);
    default: return Value;
    }
  }

  uint64_t LaneMask(OpSize ES) {
    const unsigned BitsN = IR::OpSizeAsBits(ES);
    return BitsN >= 64 ? ~0ULL : (1ULL << BitsN) - 1;
  }
} // namespace

Ref IRBuilder::LaneConstant(uint64_t Value, OpSize ES) {
  return VectorConstant64(ReplicateLane(Value, ES));
}

void IRBuilder::SetQCIfAny(Ref Mask) {
  Ref Any = _VAnyNonZero(OpSize::i128Bit, Mask);
  const auto Offset = offsetof(FEXCore::Core::CPUState, fpsr);
  Ref FPSR = _LoadContext(OpSize::i32Bit, RegClass::GPR, Offset);
  _StoreContext(OpSize::i32Bit, RegClass::GPR, _Or(OpSize::i64Bit, FPSR, _Lshl(OpSize::i64Bit, Any, Constant(27))), Offset);
}

// The written lanes of a saturation mask: all of it for Q, the low half for a
// 64-bit vector, the element for a scalar.
Ref IRBuilder::UsedLanes(Ref Mask, bool Scalar, bool Q, OpSize ES) {
  if (Scalar) {
    return _VMov(ES, Mask);
  }
  return Q ? Mask : _VMov(OpSize::i64Bit, Mask).Node;
}

void IRBuilder::StoreIntLanes(uint32_t Word, bool Scalar, OpSize ES, Ref Result) {
  const uint32_t Rd = Bits(Word, 4, 0);
  if (Scalar) {
    StoreVSized(Rd, ES, Result);
  } else {
    StoreVQ(Rd, Bit(Word, 30), Result);
  }
}

// ---------------------------------------------------------------------------
// Saturating add and subtract, absolute value and negation
// ---------------------------------------------------------------------------

Ref IRBuilder::SaturatingAddSub(OpSize ES, Ref A, Ref B, bool Sub, bool Signed, Ref* Saturated) {
  const auto RS = OpSize::i128Bit;
  Ref Wrap = Sub ? _VSub(RS, ES, A, B).Node : _VAdd(RS, ES, A, B).Node;
  if (ES != OpSize::i64Bit) {
    Ref Sat {};
    if (Signed) {
      Sat = Sub ? _VSQSub(RS, ES, A, B).Node : _VSQAdd(RS, ES, A, B).Node;
    } else {
      Sat = Sub ? _VUQSub(RS, ES, A, B).Node : _VUQAdd(RS, ES, A, B).Node;
    }
    *Saturated = _VXor(RS, RS, Sat, Wrap);
    return Sat;
  }
  // 64-bit lanes (NEON-LANDINGS §3.4).
  const auto ES64 = OpSize::i64Bit;
  if (!Signed) {
    Ref Ovf {};
    if (Sub) {
      // B > A unsigned: max(A, B) != A.
      Ovf = _VNot(RS, ES64, _VCMPEQ(RS, ES64, _VUMax(RS, ES64, A, B), A));
      *Saturated = Ovf;
      return _VAndn(RS, RS, Wrap, Ovf);
    }
    // A > sum unsigned: max(A, sum) != sum.
    Ovf = _VNot(RS, ES64, _VCMPEQ(RS, ES64, _VUMax(RS, ES64, A, Wrap), Wrap));
    *Saturated = Ovf;
    return _VOr(RS, RS, Wrap, Ovf);
  }
  // Signed overflow: the operands (add: same sign, sub: different sign)
  // disagree with the result's sign.
  Ref SignMix = Sub ? _VAnd(RS, RS, _VXor(RS, RS, A, B), _VXor(RS, RS, A, Wrap)).Node :
                      _VAndn(RS, RS, _VXor(RS, RS, A, Wrap), _VXor(RS, RS, A, B)).Node;
  Ref Ovf = _VCMPLTZ(RS, ES64, SignMix);
  // A's sign selects the bound: A >= 0 -> INT64_MAX, A < 0 -> INT64_MIN.
  Ref Bound = _VXor(RS, RS, _VSShrI(RS, ES64, A, 63), LaneConstant(0x7FFFFFFFFFFFFFFFULL, ES64));
  *Saturated = Ovf;
  return _VBSL(RS, Ovf, Bound, Wrap);
}

bool IRBuilder::SIMDSaturatingAddSub(uint32_t Word, bool Sub, bool Signed, bool Scalar) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (!Scalar && Size == 3 && !Q) {
    return false;
  }
  const auto ES = LaneSize(Size);
  Ref Sat {};
  Ref Result = SaturatingAddSub(ES, LoadV(Bits(Word, 9, 5)), LoadV(Bits(Word, 20, 16)), Sub, Signed, &Sat);
  SetQCIfAny(UsedLanes(Sat, Scalar, Q, ES));
  StoreIntLanes(Word, Scalar, ES, Result);
  return true;
}

bool IRBuilder::SQADD_2(uint32_t Word) { return SIMDSaturatingAddSub(Word, false, true, false); }
bool IRBuilder::SQSUB_2(uint32_t Word) { return SIMDSaturatingAddSub(Word, true, true, false); }
bool IRBuilder::UQADD_2(uint32_t Word) { return SIMDSaturatingAddSub(Word, false, false, false); }
bool IRBuilder::UQSUB_2(uint32_t Word) { return SIMDSaturatingAddSub(Word, true, false, false); }
bool IRBuilder::SQADD_1(uint32_t Word) { return SIMDSaturatingAddSub(Word, false, true, true); }
bool IRBuilder::SQSUB_1(uint32_t Word) { return SIMDSaturatingAddSub(Word, true, true, true); }
bool IRBuilder::UQADD_1(uint32_t Word) { return SIMDSaturatingAddSub(Word, false, false, true); }
bool IRBuilder::UQSUB_1(uint32_t Word) { return SIMDSaturatingAddSub(Word, true, false, true); }

bool IRBuilder::SIMDSaturatingAbsNeg(uint32_t Word, bool IsNeg, bool Scalar) {
  // abs(MIN) and -MIN wrap to MIN; those lanes become MAX (MIN ^ ~0).
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (!Scalar && Size == 3 && !Q) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Min = LaneConstant(1ULL << (IR::OpSizeAsBits(ES) - 1), ES);
  Ref IsMin = _VCMPEQ(RS, ES, V, Min);
  Ref Wrap = IsNeg ? _VNeg(RS, ES, V).Node : _VAbs(RS, ES, V).Node;
  SetQCIfAny(UsedLanes(IsMin, Scalar, Q, ES));
  StoreIntLanes(Word, Scalar, ES, _VXor(RS, RS, Wrap, IsMin));
  return true;
}

bool IRBuilder::SQABS_2(uint32_t Word) { return SIMDSaturatingAbsNeg(Word, false, false); }
bool IRBuilder::SQNEG_2(uint32_t Word) { return SIMDSaturatingAbsNeg(Word, true, false); }
bool IRBuilder::SQABS_1(uint32_t Word) { return SIMDSaturatingAbsNeg(Word, false, true); }
bool IRBuilder::SQNEG_1(uint32_t Word) { return SIMDSaturatingAbsNeg(Word, true, true); }

// SUQADD: Rd (signed) + Rn (unsigned) with signed saturation. USQADD: Rd
// (unsigned) + Rn (signed) with unsigned saturation. Scalar and vector.
bool IRBuilder::SIMDSaturatingAccumulate(uint32_t Word, bool SignedAcc, bool Scalar) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (!Scalar && Size == 3 && !Q) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref D = LoadV(Rd);
  Ref N = LoadV(Bits(Word, 9, 5));
  // SUQADD: the addend's top bit; USQADD: a negative addend.
  Ref TopSet = _VCMPLTZ(RS, ES, N);
  Ref Unused {};
  Ref Result {};
  if (SignedAcc) {
    // An addend below 2^(W-1) is an ordinary signed saturating add. A
    // larger one makes the sum non-negative: (d + 2^(W-1)) + (n - 2^(W-1))
    // in unsigned saturating arithmetic, capped at the signed maximum.
    Ref Flip = LaneConstant(1ULL << (IR::OpSizeAsBits(ES) - 1), ES);
    Ref Small = SaturatingAddSub(ES, D, N, false, true, &Unused);
    Ref Offset = SaturatingAddSub(ES, _VXor(RS, RS, D, Flip), _VXor(RS, RS, N, Flip), false, false, &Unused);
    Ref Large = _VUMin(RS, ES, Offset, LaneConstant(LaneMask(ES) >> 1, ES));
    Result = _VBSL(RS, TopSet, Large, Small);
  } else {
    // A negative addend subtracts its magnitude, saturating at 0 (-MIN
    // wraps to 2^(W-1), which is its magnitude as an unsigned value).
    Ref Add = SaturatingAddSub(ES, D, N, false, false, &Unused);
    Ref Sub = SaturatingAddSub(ES, D, _VNeg(RS, ES, N), true, false, &Unused);
    Result = _VBSL(RS, TopSet, Sub, Add);
  }
  // A saturated lane never equals the wrapped sum.
  SetQCIfAny(UsedLanes(_VXor(RS, RS, Result, _VAdd(RS, ES, D, N)), Scalar, Q, ES));
  StoreIntLanes(Word, Scalar, ES, Result);
  return true;
}

bool IRBuilder::SUQADD_1(uint32_t Word) { return SIMDSaturatingAccumulate(Word, true, true); }
bool IRBuilder::SUQADD_2(uint32_t Word) { return SIMDSaturatingAccumulate(Word, true, false); }
bool IRBuilder::USQADD_1(uint32_t Word) { return SIMDSaturatingAccumulate(Word, false, true); }
bool IRBuilder::USQADD_2(uint32_t Word) { return SIMDSaturatingAccumulate(Word, false, false); }

// ---------------------------------------------------------------------------
// Saturating narrows
// ---------------------------------------------------------------------------

Ref IRBuilder::SaturateNarrow(Ref V, OpSize WideES, NarrowKind Kind, Ref* Saturated) {
  const auto RS = OpSize::i128Bit;
  const unsigned NarrowBits = IR::OpSizeAsBits(WideES) / 2;
  const uint64_t UMax = (1ULL << NarrowBits) - 1;
  const uint64_t SMax = UMax >> 1;
  const uint64_t SMin = ~SMax & LaneMask(WideES);
  Ref Clamped {};
  switch (Kind) {
  case NarrowKind::Truncate: *Saturated = nullptr; return _VUShrNI(RS, WideES, V, 0);
  case NarrowKind::SignedToSigned:
    Clamped = _VSMax(RS, WideES, _VSMin(RS, WideES, V, LaneConstant(SMax, WideES)), LaneConstant(SMin, WideES));
    break;
  case NarrowKind::UnsignedToUnsigned: Clamped = _VUMin(RS, WideES, V, LaneConstant(UMax, WideES)); break;
  case NarrowKind::SignedToUnsigned:
    Clamped = _VUMin(RS, WideES, _VSMax(RS, WideES, V, _VectorImm(RS, OpSize::i8Bit, 0)), LaneConstant(UMax, WideES));
    break;
  }
  *Saturated = _VXor(RS, RS, Clamped, V);
  return _VUShrNI(RS, WideES, Clamped, 0);
}

bool IRBuilder::SIMDSaturatingExtractNarrow(uint32_t Word, NarrowKind Kind, bool Scalar) {
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3) {
    return false;
  }
  const auto NarrowES = LaneSize(Size);
  const auto WideES = LaneSize(Size + 1);
  Ref Sat {};
  Ref Narrow = SaturateNarrow(LoadV(Bits(Word, 9, 5)), WideES, Kind, &Sat);
  // The source is the whole wide register (a scalar: its element).
  SetQCIfAny(Scalar ? _VMov(WideES, Sat).Node : Sat);
  if (Scalar) {
    StoreVSized(Bits(Word, 4, 0), NarrowES, Narrow);
  } else {
    StoreNarrow(Bits(Word, 4, 0), Bit(Word, 30), Narrow);
  }
  return true;
}

bool IRBuilder::SQXTN_2(uint32_t Word) { return SIMDSaturatingExtractNarrow(Word, NarrowKind::SignedToSigned, false); }
bool IRBuilder::UQXTN_2(uint32_t Word) { return SIMDSaturatingExtractNarrow(Word, NarrowKind::UnsignedToUnsigned, false); }
bool IRBuilder::SQXTUN_2(uint32_t Word) { return SIMDSaturatingExtractNarrow(Word, NarrowKind::SignedToUnsigned, false); }
bool IRBuilder::SQXTN_1(uint32_t Word) { return SIMDSaturatingExtractNarrow(Word, NarrowKind::SignedToSigned, true); }
bool IRBuilder::UQXTN_1(uint32_t Word) { return SIMDSaturatingExtractNarrow(Word, NarrowKind::UnsignedToUnsigned, true); }
bool IRBuilder::SQXTUN_1(uint32_t Word) { return SIMDSaturatingExtractNarrow(Word, NarrowKind::SignedToUnsigned, true); }

// ---------------------------------------------------------------------------
// Shifts by immediate: rounding, saturating, narrowing
// ---------------------------------------------------------------------------

// (V >> Shift) rounded to nearest with ties up, for 1 <= Shift <= lane width:
// (V >> Shift) + ((V >> (Shift - 1)) & 1), which cannot overflow the lane.
Ref IRBuilder::RoundingShiftRight(Ref V, OpSize ES, uint32_t Shift, bool Signed) {
  const auto RS = OpSize::i128Bit;
  Ref Shifted = Signed ? _VSShrI(RS, ES, V, Shift).Node : _VUShrI(RS, ES, V, Shift).Node;
  Ref Below = Shift == 1 ? V : _VUShrI(RS, ES, V, Shift - 1).Node;
  return _VAdd(RS, ES, Shifted, _VAnd(RS, RS, Below, LaneConstant(1, ES)));
}

bool IRBuilder::SIMDShiftRightNarrow(uint32_t Word, NarrowKind Kind, bool Rounding, bool SignedShift, bool Scalar) {
  const uint32_t Immh = Bits(Word, 22, 19);
  if (Immh == 0 || (Immh & 0b1000)) {
    return false;
  }
  const uint32_t SizeLog2 = 31 - std::countl_zero(Immh);
  const uint32_t NarrowBits = 8U << SizeLog2;
  const uint32_t Shift = 2 * NarrowBits - Bits(Word, 22, 16);
  const auto NarrowES = LaneSize(SizeLog2);
  const auto WideES = LaneSize(SizeLog2 + 1);
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Shifted {};
  if (Rounding) {
    Shifted = RoundingShiftRight(V, WideES, Shift, SignedShift);
  } else {
    Shifted = SignedShift ? _VSShrI(RS, WideES, V, Shift).Node : _VUShrI(RS, WideES, V, Shift).Node;
  }
  Ref Sat {};
  Ref Narrow = SaturateNarrow(Shifted, WideES, Kind, &Sat);
  if (Sat) {
    SetQCIfAny(Scalar ? _VMov(WideES, Sat).Node : Sat);
  }
  if (Scalar) {
    StoreVSized(Bits(Word, 4, 0), NarrowES, Narrow);
  } else {
    StoreNarrow(Bits(Word, 4, 0), Bit(Word, 30), Narrow);
  }
  return true;
}

bool IRBuilder::RSHRN(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::Truncate, true, false, false); }
bool IRBuilder::SQSHRN_2(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::SignedToSigned, false, true, false); }
bool IRBuilder::SQRSHRN_2(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::SignedToSigned, true, true, false); }
bool IRBuilder::UQSHRN_2(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::UnsignedToUnsigned, false, false, false); }
bool IRBuilder::UQRSHRN_2(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::UnsignedToUnsigned, true, false, false); }
bool IRBuilder::SQSHRUN_2(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::SignedToUnsigned, false, true, false); }
bool IRBuilder::SQRSHRUN_2(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::SignedToUnsigned, true, true, false); }
bool IRBuilder::SQSHRN_1(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::SignedToSigned, false, true, true); }
bool IRBuilder::UQSHRN_1(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::UnsignedToUnsigned, false, false, true); }
bool IRBuilder::SQSHRUN_1(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::SignedToUnsigned, false, true, true); }
bool IRBuilder::SQRSHRN_1(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::SignedToSigned, true, true, true); }
bool IRBuilder::UQRSHRN_1(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::UnsignedToUnsigned, true, false, true); }
bool IRBuilder::SQRSHRUN_1(uint32_t Word) { return SIMDShiftRightNarrow(Word, NarrowKind::SignedToUnsigned, true, true, true); }

bool IRBuilder::SIMDRoundingShiftRight(uint32_t Word, bool Signed, bool Accumulate, bool Scalar) {
  const bool Q = Scalar || Bit(Word, 30);
  const uint32_t Immh = Bits(Word, 22, 19);
  if (Immh == 0) {
    return false;
  }
  const uint32_t SizeLog2 = 31 - std::countl_zero(Immh);
  if ((Scalar && SizeLog2 != 3) || (!Q && SizeLog2 == 3)) {
    return false;
  }
  const uint32_t ElementBits = 8U << SizeLog2;
  const auto ES = LaneSize(SizeLog2);
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref Result = RoundingShiftRight(LoadV(Bits(Word, 9, 5)), ES, 2 * ElementBits - Bits(Word, 22, 16), Signed);
  if (Accumulate) {
    Result = _VAdd(OpSize::i128Bit, ES, LoadV(Rd), Result);
  }
  StoreIntLanes(Word, Scalar, ES, Result);
  return true;
}

bool IRBuilder::SRSHR_2(uint32_t Word) { return SIMDRoundingShiftRight(Word, true, false, false); }
bool IRBuilder::URSHR_2(uint32_t Word) { return SIMDRoundingShiftRight(Word, false, false, false); }
bool IRBuilder::SRSRA_2(uint32_t Word) { return SIMDRoundingShiftRight(Word, true, true, false); }
bool IRBuilder::URSRA_2(uint32_t Word) { return SIMDRoundingShiftRight(Word, false, true, false); }
bool IRBuilder::SRSHR_1(uint32_t Word) { return SIMDRoundingShiftRight(Word, true, false, true); }
bool IRBuilder::URSHR_1(uint32_t Word) { return SIMDRoundingShiftRight(Word, false, false, true); }
bool IRBuilder::SRSRA_1(uint32_t Word) { return SIMDRoundingShiftRight(Word, true, true, true); }
bool IRBuilder::URSRA_1(uint32_t Word) { return SIMDRoundingShiftRight(Word, false, true, true); }

bool IRBuilder::SIMDSaturatingShiftLeft(uint32_t Word, ShiftLeftKind Kind, bool Scalar) {
  const bool Q = Bit(Word, 30);
  const uint32_t Immh = Bits(Word, 22, 19);
  if (Immh == 0) {
    return false;
  }
  const uint32_t SizeLog2 = 31 - std::countl_zero(Immh);
  if (!Scalar && !Q && SizeLog2 == 3) {
    return false;
  }
  const uint32_t ElementBits = 8U << SizeLog2;
  const uint32_t Shift = Bits(Word, 22, 16) - ElementBits;
  const auto ES = LaneSize(SizeLog2);
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  // A shift of 0 passes the value through (and still clamps negative lanes for SQSHLU).
  auto Shl = [&](Ref X) -> Ref {
    return Shift ? _VShlI(RS, ES, X, Shift).Node : X;
  };
  auto Unchanged = [&](Ref Shifted, Ref Original, bool SignedBack) -> Ref {
    if (!Shift) {
      return _VCMPEQ(RS, ES, Original, Original);
    }
    Ref Back = SignedBack ? _VSShrI(RS, ES, Shifted, Shift).Node : _VUShrI(RS, ES, Shifted, Shift).Node;
    return _VCMPEQ(RS, ES, Back, Original);
  };
  Ref Result {};
  Ref Sat {};
  switch (Kind) {
  case ShiftLeftKind::Unsigned: {
    Ref Shifted = Shl(V);
    Sat = _VNot(RS, ES, Unchanged(Shifted, V, false));
    Result = _VOr(RS, RS, Shifted, Sat);
    break;
  }
  case ShiftLeftKind::Signed: {
    Ref Shifted = Shl(V);
    Sat = _VNot(RS, ES, Unchanged(Shifted, V, true));
    Ref Bound = _VXor(RS, RS, _VSShrI(RS, ES, V, ElementBits - 1), LaneConstant(LaneMask(ES) >> 1, ES));
    Result = _VBSL(RS, Sat, Bound, Shifted);
    break;
  }
  case ShiftLeftKind::SignedToUnsigned: {
    // Negative lanes give 0; the rest shift with unsigned saturation.
    Ref Negative = _VCMPLTZ(RS, ES, V);
    Ref Positive = _VAndn(RS, RS, V, Negative);
    Ref Shifted = Shl(Positive);
    Ref Ovf = _VNot(RS, ES, Unchanged(Shifted, Positive, false));
    Result = _VOr(RS, RS, Shifted, Ovf);
    Sat = _VOr(RS, RS, Ovf, Negative);
    break;
  }
  }
  SetQCIfAny(UsedLanes(Sat, Scalar, Q, ES));
  StoreIntLanes(Word, Scalar, ES, Result);
  return true;
}

bool IRBuilder::SQSHL_imm_2(uint32_t Word) { return SIMDSaturatingShiftLeft(Word, ShiftLeftKind::Signed, false); }
bool IRBuilder::UQSHL_imm_2(uint32_t Word) { return SIMDSaturatingShiftLeft(Word, ShiftLeftKind::Unsigned, false); }
bool IRBuilder::SQSHLU_2(uint32_t Word) { return SIMDSaturatingShiftLeft(Word, ShiftLeftKind::SignedToUnsigned, false); }
bool IRBuilder::SQSHL_imm_1(uint32_t Word) { return SIMDSaturatingShiftLeft(Word, ShiftLeftKind::Signed, true); }
bool IRBuilder::UQSHL_imm_1(uint32_t Word) { return SIMDSaturatingShiftLeft(Word, ShiftLeftKind::Unsigned, true); }
bool IRBuilder::SQSHLU_1(uint32_t Word) { return SIMDSaturatingShiftLeft(Word, ShiftLeftKind::SignedToUnsigned, true); }

// SQSHL/UQSHL/SRSHL/URSHL/SQRSHL/UQRSHL (register), vector and scalar. The
// count is the signed low byte of each lane of Rm. A non-negative count
// shifts left, saturating for the Q forms (FPSR.QC) and keeping the low bits
// for SRSHL/URSHL; a negative count shifts right by its magnitude, rounded
// to nearest with ties up for the R forms. A right shift never saturates.
bool IRBuilder::SIMDShiftByRegister(uint32_t Word, bool Signed, bool Rounding, bool Saturating, bool Scalar) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Scalar ? (!Saturating && Size != 3) : (Size == 3 && !Q)) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto RS = OpSize::i128Bit;
  const unsigned W = 8U << Size;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Count = LoadV(Bits(Word, 20, 16));
  if (W > 8) {
    Count = _VSShrI(RS, ES, _VShlI(RS, ES, Count, W - 8), W - 8);
  }
  Ref Negative = _VCMPLTZ(RS, ES, Count);
  auto ShiftRight = [&](Ref X, Ref Amount) -> Ref {
    // Amounts of the lane width or more leave 0, or the sign when signed.
    return Signed ? _VSShr(RS, ES, X, Amount, true).Node : _VUShr(RS, ES, X, Amount, true).Node;
  };
  // Right by -count (1..128). Rounding adds bit (-count - 1) of V, the sign
  // beyond the lane: (V >> s) + ((V >> (s - 1)) & 1) cannot overflow.
  Ref Right = ShiftRight(V, _VNeg(RS, ES, Count));
  if (Rounding) {
    Right = _VAdd(RS, ES, Right, _VAnd(RS, RS, ShiftRight(V, _VNot(RS, ES, Count)), LaneConstant(1, ES)));
  }
  // Left by count, 0 at the lane width or more.
  Ref Left = _VUShl(RS, ES, V, Count, true);
  if (Saturating) {
    // Saturated where shifting back does not recover V.
    Ref Sat = _VAndn(RS, RS, _VNot(RS, ES, _VCMPEQ(RS, ES, ShiftRight(Left, Count), V)), Negative);
    if (Signed) {
      Ref Bound = _VXor(RS, RS, _VSShrI(RS, ES, V, W - 1), LaneConstant(LaneMask(ES) >> 1, ES));
      Left = _VBSL(RS, Sat, Bound, Left);
    } else {
      Left = _VOr(RS, RS, Left, Sat);
    }
    SetQCIfAny(UsedLanes(Sat, Scalar, Q, ES));
  }
  StoreIntLanes(Word, Scalar, ES, _VBSL(RS, Negative, Right, Left));
  return true;
}

bool IRBuilder::SQSHL_reg_2(uint32_t Word) { return SIMDShiftByRegister(Word, true, false, true, false); }
bool IRBuilder::UQSHL_reg_2(uint32_t Word) { return SIMDShiftByRegister(Word, false, false, true, false); }
bool IRBuilder::SRSHL_2(uint32_t Word) { return SIMDShiftByRegister(Word, true, true, false, false); }
bool IRBuilder::URSHL_2(uint32_t Word) { return SIMDShiftByRegister(Word, false, true, false, false); }
bool IRBuilder::SQRSHL_2(uint32_t Word) { return SIMDShiftByRegister(Word, true, true, true, false); }
bool IRBuilder::UQRSHL_2(uint32_t Word) { return SIMDShiftByRegister(Word, false, true, true, false); }
bool IRBuilder::SQSHL_reg_1(uint32_t Word) { return SIMDShiftByRegister(Word, true, false, true, true); }
bool IRBuilder::UQSHL_reg_1(uint32_t Word) { return SIMDShiftByRegister(Word, false, false, true, true); }
bool IRBuilder::SRSHL_1(uint32_t Word) { return SIMDShiftByRegister(Word, true, true, false, true); }
bool IRBuilder::URSHL_1(uint32_t Word) { return SIMDShiftByRegister(Word, false, true, false, true); }
bool IRBuilder::SQRSHL_1(uint32_t Word) { return SIMDShiftByRegister(Word, true, true, true, true); }
bool IRBuilder::UQRSHL_1(uint32_t Word) { return SIMDShiftByRegister(Word, false, true, true, true); }

// ---------------------------------------------------------------------------
// Halving add/subtract, absolute difference, rounding high narrow
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDHalving(uint32_t Word, HalvingOp Op) {
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto RS = OpSize::i128Bit;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));
  // The signed forms are the unsigned ones with the sign bits flipped
  // (x ^ MIN maps signed order onto unsigned order and keeps differences).
  Ref Flip = LaneConstant(1ULL << (IR::OpSizeAsBits(ES) - 1), ES);
  Ref Result {};
  switch (Op) {
  case HalvingOp::UHAdd:
  case HalvingOp::SHAdd: {
    // (a & b) + ((a ^ b) >> 1)
    Ref Half = Op == HalvingOp::SHAdd ? _VSShrI(RS, ES, _VXor(RS, RS, A, B), 1).Node : _VUShrI(RS, ES, _VXor(RS, RS, A, B), 1).Node;
    Result = _VAdd(RS, ES, _VAnd(RS, RS, A, B), Half);
    break;
  }
  case HalvingOp::URHAdd: Result = _VURAvg(RS, ES, A, B); break;
  case HalvingOp::SRHAdd: Result = _VXor(RS, RS, _VURAvg(RS, ES, _VXor(RS, RS, A, Flip), _VXor(RS, RS, B, Flip)), Flip); break;
  case HalvingOp::UHSub:
  case HalvingOp::SHSub: {
    // ((a ^ b) >> 1) - (~a & b), on the flipped operands for the signed form.
    if (Op == HalvingOp::SHSub) {
      A = _VXor(RS, RS, A, Flip);
      B = _VXor(RS, RS, B, Flip);
    }
    Result = _VSub(RS, ES, _VUShrI(RS, ES, _VXor(RS, RS, A, B), 1), _VAndn(RS, RS, B, A));
    break;
  }
  }
  StoreVQ(Bits(Word, 4, 0), Bit(Word, 30), Result);
  return true;
}

bool IRBuilder::UHADD(uint32_t Word) { return SIMDHalving(Word, HalvingOp::UHAdd); }
bool IRBuilder::SHADD(uint32_t Word) { return SIMDHalving(Word, HalvingOp::SHAdd); }
bool IRBuilder::URHADD(uint32_t Word) { return SIMDHalving(Word, HalvingOp::URHAdd); }
bool IRBuilder::SRHADD(uint32_t Word) { return SIMDHalving(Word, HalvingOp::SRHAdd); }
bool IRBuilder::UHSUB(uint32_t Word) { return SIMDHalving(Word, HalvingOp::UHSub); }
bool IRBuilder::SHSUB(uint32_t Word) { return SIMDHalving(Word, HalvingOp::SHSub); }

Ref IRBuilder::AbsoluteDifference(OpSize ES, Ref A, Ref B, bool Signed) {
  // max - min; the signed difference wraps into the unsigned value exactly.
  const auto RS = OpSize::i128Bit;
  if (Signed) {
    return _VSub(RS, ES, _VSMax(RS, ES, A, B), _VSMin(RS, ES, A, B));
  }
  return _VSub(RS, ES, _VUMax(RS, ES, A, B), _VUMin(RS, ES, A, B));
}

bool IRBuilder::SIMDAbsoluteDifference(uint32_t Word, bool Signed, bool Accumulate, bool Long) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref Diff = AbsoluteDifference(ES, LoadV(Bits(Word, 9, 5)), LoadV(Bits(Word, 20, 16)), Signed);
  if (!Long) {
    if (Accumulate) {
      Diff = _VAdd(RS, ES, LoadV(Rd), Diff);
    }
    StoreVQ(Rd, Q, Diff);
    return true;
  }
  // The absolute difference fits the narrow lane unsigned: zero-extend it.
  const auto WideES = LaneSize(Size + 1);
  Ref Wide = Q ? _VUXTL2(RS, ES, Diff).Node : _VUXTL(RS, ES, Diff).Node;
  if (Accumulate) {
    Wide = _VAdd(RS, WideES, LoadV(Rd), Wide);
  }
  StoreV(Rd, Wide);
  return true;
}

bool IRBuilder::UABD(uint32_t Word) { return SIMDAbsoluteDifference(Word, false, false, false); }
bool IRBuilder::SABD(uint32_t Word) { return SIMDAbsoluteDifference(Word, true, false, false); }
bool IRBuilder::UABA(uint32_t Word) { return SIMDAbsoluteDifference(Word, false, true, false); }
bool IRBuilder::SABA(uint32_t Word) { return SIMDAbsoluteDifference(Word, true, true, false); }
bool IRBuilder::UABDL(uint32_t Word) { return SIMDAbsoluteDifference(Word, false, false, true); }
bool IRBuilder::SABDL(uint32_t Word) { return SIMDAbsoluteDifference(Word, true, false, true); }
bool IRBuilder::UABAL(uint32_t Word) { return SIMDAbsoluteDifference(Word, false, true, true); }
bool IRBuilder::SABAL(uint32_t Word) { return SIMDAbsoluteDifference(Word, true, true, true); }

bool IRBuilder::SIMDRoundingHighNarrow(uint32_t Word, bool Sub) {
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3) {
    return false;
  }
  const auto WideES = LaneSize(Size + 1);
  const auto RS = OpSize::i128Bit;
  const uint32_t NarrowBits = 8U << Size;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));
  Ref Value = Sub ? _VSub(RS, WideES, A, B).Node : _VAdd(RS, WideES, A, B).Node;
  Value = _VAdd(RS, WideES, Value, LaneConstant(1ULL << (NarrowBits - 1), WideES));
  StoreNarrow(Bits(Word, 4, 0), Bit(Word, 30), _VUShrNI(RS, WideES, Value, NarrowBits));
  return true;
}

bool IRBuilder::RADDHN(uint32_t Word) { return SIMDRoundingHighNarrow(Word, false); }
bool IRBuilder::RSUBHN(uint32_t Word) { return SIMDRoundingHighNarrow(Word, true); }

// ---------------------------------------------------------------------------
// Doubling multiplies and by-element multiplies
// ---------------------------------------------------------------------------

// The Rm operand of the integer by-element group: size 1 indexes with H:L:M
// and a 4-bit Rm, size 2 with H:L and M:Rm. Every lane of *Element holds it.
bool IRBuilder::IntElementOperand(uint32_t Word, Ref* Element) {
  const uint32_t Size = Bits(Word, 23, 22);
  const uint32_t H = Bit(Word, 11), L = Bit(Word, 21), M = Bit(Word, 20);
  uint32_t Index {}, Rm {};
  if (Size == 1) {
    Index = (H << 2) | (L << 1) | M;
    Rm = Bits(Word, 19, 16);
  } else if (Size == 2) {
    Index = (H << 1) | L;
    Rm = (M << 4) | Bits(Word, 19, 16);
  } else {
    return false;
  }
  *Element = _VDupElement(OpSize::i128Bit, LaneSize(Size), LoadV(Rm), Index);
  return true;
}

// SQDMULH: sat((2ab) >> W); SQRDMULH: sat((2ab + 2^(W-1)) >> W), W = 16 or 32,
// in every lane. Only a == b == MIN saturates, and it is the only case whose
// wrapped result is MIN, so MIN lanes flip to MAX.
Ref IRBuilder::DoublingMultiplyHigh(OpSize ES, Ref A, Ref B, bool Rounding, Ref* Saturated) {
  const auto RS = OpSize::i128Bit;
  const auto WideES = ES == OpSize::i16Bit ? OpSize::i32Bit : OpSize::i64Bit;
  const unsigned W = IR::OpSizeAsBits(ES);
  auto HighHalf = [&](Ref Product) -> Ref {
    if (Rounding) {
      Product = _VAdd(RS, WideES, Product, LaneConstant(1ULL << (W - 2), WideES));
    }
    return _VUShrNI(RS, WideES, _VSShrI(RS, WideES, Product, W - 1), 0);
  };
  Ref Low = HighHalf(_VSMull(RS, ES, A, B));
  Ref High = HighHalf(_VSMull2(RS, ES, A, B));
  Ref Wrapped = _VInsElement(RS, OpSize::i64Bit, 1, 0, Low, High);
  Ref IsMin = _VCMPEQ(RS, ES, Wrapped, LaneConstant(1ULL << (W - 1), ES));
  *Saturated = IsMin;
  return _VXor(RS, RS, Wrapped, IsMin);
}

bool IRBuilder::SIMDDoublingMultiplyHigh(uint32_t Word, bool Rounding, bool Scalar, bool ByElement) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size != 1 && Size != 2) {
    return false;
  }
  const auto ES = LaneSize(Size);
  Ref B {};
  if (ByElement) {
    if (!IntElementOperand(Word, &B)) {
      return false;
    }
  } else {
    B = LoadV(Bits(Word, 20, 16));
  }
  Ref Sat {};
  Ref Result = DoublingMultiplyHigh(ES, LoadV(Bits(Word, 9, 5)), B, Rounding, &Sat);
  SetQCIfAny(UsedLanes(Sat, Scalar, Q, ES));
  StoreIntLanes(Word, Scalar, ES, Result);
  return true;
}

bool IRBuilder::SQDMULH_vec_2(uint32_t Word) { return SIMDDoublingMultiplyHigh(Word, false, false, false); }
bool IRBuilder::SQRDMULH_vec_2(uint32_t Word) { return SIMDDoublingMultiplyHigh(Word, true, false, false); }
bool IRBuilder::SQDMULH_vec_1(uint32_t Word) { return SIMDDoublingMultiplyHigh(Word, false, true, false); }
bool IRBuilder::SQRDMULH_vec_1(uint32_t Word) { return SIMDDoublingMultiplyHigh(Word, true, true, false); }
bool IRBuilder::SQDMULH_elt_2(uint32_t Word) { return SIMDDoublingMultiplyHigh(Word, false, false, true); }
bool IRBuilder::SQRDMULH_elt_2(uint32_t Word) { return SIMDDoublingMultiplyHigh(Word, true, false, true); }
bool IRBuilder::SQDMULH_elt_1(uint32_t Word) { return SIMDDoublingMultiplyHigh(Word, false, true, true); }
bool IRBuilder::SQRDMULH_elt_1(uint32_t Word) { return SIMDDoublingMultiplyHigh(Word, true, true, true); }

// SQDMULL/SQDMLAL/SQDMLSL (vector, scalar, by element; the "2" forms take
// the upper half of the sources): 2 * n * m saturated to the double-width
// lane (only MIN * MIN saturates), then for SQDMLAL/SQDMLSL added to or
// subtracted from Rd with saturation again. Either saturation sets FPSR.QC.
bool IRBuilder::SIMDDoublingMultiplyLong(uint32_t Word, int Accumulate, bool Scalar, bool ByElement) {
  const bool Upper = !Scalar && Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size != 1 && Size != 2) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto WideES = LaneSize(Size + 1);
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref B {};
  if (ByElement) {
    if (!IntElementOperand(Word, &B)) {
      return false;
    }
  } else {
    B = LoadV(Bits(Word, 20, 16));
  }
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref Product = Upper ? _VSMull2(RS, ES, A, B).Node : _VSMull(RS, ES, A, B).Node;
  Ref Sat {};
  Ref Result = SaturatingAddSub(WideES, Product, Product, false, true, &Sat);
  if (Accumulate != 0) {
    Ref AccSat {};
    Result = SaturatingAddSub(WideES, LoadV(Rd), Result, Accumulate < 0, true, &AccSat);
    Sat = _VOr(RS, RS, Sat, AccSat);
  }
  SetQCIfAny(Scalar ? _VMov(WideES, Sat).Node : Sat);
  if (Scalar) {
    StoreVSized(Rd, WideES, Result);
  } else {
    StoreV(Rd, Result);
  }
  return true;
}

bool IRBuilder::SQDMULL_vec_1(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, 0, true, false); }
bool IRBuilder::SQDMULL_vec_2(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, 0, false, false); }
bool IRBuilder::SQDMLAL_vec_1(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, 1, true, false); }
bool IRBuilder::SQDMLAL_vec_2(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, 1, false, false); }
bool IRBuilder::SQDMLSL_vec_1(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, -1, true, false); }
bool IRBuilder::SQDMLSL_vec_2(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, -1, false, false); }
bool IRBuilder::SQDMULL_elt_1(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, 0, true, true); }
bool IRBuilder::SQDMULL_elt_2(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, 0, false, true); }
bool IRBuilder::SQDMLAL_elt_1(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, 1, true, true); }
bool IRBuilder::SQDMLAL_elt_2(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, 1, false, true); }
bool IRBuilder::SQDMLSL_elt_1(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, -1, true, true); }
bool IRBuilder::SQDMLSL_elt_2(uint32_t Word) { return SIMDDoublingMultiplyLong(Word, -1, false, true); }

bool IRBuilder::SIMDMultiplyElement(uint32_t Word, int Accumulate) {
  // MLA/MLS by element (Accumulate +1/-1).
  const uint32_t Size = Bits(Word, 23, 22);
  Ref Element {};
  if (!IntElementOperand(Word, &Element)) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref Product = _VMul(RS, ES, LoadV(Bits(Word, 9, 5)), Element);
  Ref Result = Accumulate > 0 ? _VAdd(RS, ES, LoadV(Rd), Product).Node : _VSub(RS, ES, LoadV(Rd), Product).Node;
  StoreVQ(Rd, Bit(Word, 30), Result);
  return true;
}

bool IRBuilder::MLA_elt(uint32_t Word) { return SIMDMultiplyElement(Word, 1); }
bool IRBuilder::MLS_elt(uint32_t Word) { return SIMDMultiplyElement(Word, -1); }

bool IRBuilder::SIMDMultiplyLongElement(uint32_t Word, bool Signed, int Accumulate) {
  // [SU]MULL/[SU]MLAL/[SU]MLSL by element; Q selects the upper half of Rn (the "2" forms).
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  Ref Element {};
  if (!IntElementOperand(Word, &Element)) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto WideES = LaneSize(Size + 1);
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref Product {};
  if (Signed) {
    Product = Q ? _VSMull2(RS, ES, A, Element).Node : _VSMull(RS, ES, A, Element).Node;
  } else {
    Product = Q ? _VUMull2(RS, ES, A, Element).Node : _VUMull(RS, ES, A, Element).Node;
  }
  if (Accumulate > 0) {
    Product = _VAdd(RS, WideES, LoadV(Rd), Product);
  } else if (Accumulate < 0) {
    Product = _VSub(RS, WideES, LoadV(Rd), Product);
  }
  StoreV(Rd, Product);
  return true;
}

bool IRBuilder::SMULL_elt(uint32_t Word) { return SIMDMultiplyLongElement(Word, true, 0); }
bool IRBuilder::UMULL_elt(uint32_t Word) { return SIMDMultiplyLongElement(Word, false, 0); }
bool IRBuilder::SMLAL_elt(uint32_t Word) { return SIMDMultiplyLongElement(Word, true, 1); }
bool IRBuilder::UMLAL_elt(uint32_t Word) { return SIMDMultiplyLongElement(Word, false, 1); }
bool IRBuilder::SMLSL_elt(uint32_t Word) { return SIMDMultiplyLongElement(Word, true, -1); }
bool IRBuilder::UMLSL_elt(uint32_t Word) { return SIMDMultiplyLongElement(Word, false, -1); }

// ---------------------------------------------------------------------------
// Across lanes, counts, dot product
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDAddLongAcrossLanes(uint32_t Word, bool Signed) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3 || (Size == 2 && !Q)) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto WideES = LaneSize(Size + 1);
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  auto Widen = [&](bool Upper) -> Ref {
    if (Signed) {
      return Upper ? _VSXTL2(RS, ES, V).Node : _VSXTL(RS, ES, V).Node;
    }
    return Upper ? _VUXTL2(RS, ES, V).Node : _VUXTL(RS, ES, V).Node;
  };
  auto SumLanes = [&](Ref W) -> Ref { return _VAddV(RS, WideES, W); };
  Ref Result = SumLanes(Widen(false));
  if (Q) {
    Result = _VAdd(RS, WideES, Result, SumLanes(Widen(true)));
  }
  StoreVSized(Bits(Word, 4, 0), WideES, Result);
  return true;
}

bool IRBuilder::UADDLV(uint32_t Word) { return SIMDAddLongAcrossLanes(Word, false); }
bool IRBuilder::SADDLV(uint32_t Word) { return SIMDAddLongAcrossLanes(Word, true); }

bool IRBuilder::SIMDSignedAcrossLanesMinMax(uint32_t Word, bool IsMax) {
  // Signed order is unsigned order with the sign bits flipped.
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3 || (Size == 2 && !Q)) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto RS = OpSize::i128Bit;
  Ref Flip = LaneConstant(1ULL << (IR::OpSizeAsBits(ES) - 1), ES);
  Ref V = _VXor(RS, RS, LoadV(Bits(Word, 9, 5)), Flip);
  if (!Q) {
    V = _VInsElement(RS, OpSize::i64Bit, 1, 0, V, V);
  }
  Ref Result = IsMax ? _VUMaxV(RS, ES, V).Node : _VUMinV(RS, ES, V).Node;
  StoreVSized(Bits(Word, 4, 0), ES, _VXor(RS, RS, Result, Flip));
  return true;
}

bool IRBuilder::SMAXV(uint32_t Word) { return SIMDSignedAcrossLanesMinMax(Word, true); }
bool IRBuilder::SMINV(uint32_t Word) { return SIMDSignedAcrossLanesMinMax(Word, false); }

bool IRBuilder::SIMDCountLeading(uint32_t Word, bool Sign) {
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Result {};
  if (Sign) {
    // CLS(x) = CLZ(x ^ (x >> 1 arithmetic)) - 1
    Result = _VSub(RS, ES, _VCLZ(RS, ES, _VXor(RS, RS, V, _VSShrI(RS, ES, V, 1))), LaneConstant(1, ES));
  } else {
    Result = _VCLZ(RS, ES, V);
  }
  StoreVQ(Bits(Word, 4, 0), Bit(Word, 30), Result);
  return true;
}

bool IRBuilder::CLZ_asimd(uint32_t Word) { return SIMDCountLeading(Word, false); }
bool IRBuilder::CLS_asimd(uint32_t Word) { return SIMDCountLeading(Word, true); }

bool IRBuilder::SHLL(uint32_t Word) {
  // SHLL/SHLL2: widen (the extension bits are shifted out) and shift left by the element width.
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3) {
    return false;
  }
  const auto ES = LaneSize(Size);
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Wide = Bit(Word, 30) ? _VUXTL2(RS, ES, V).Node : _VUXTL(RS, ES, V).Node;
  StoreV(Bits(Word, 4, 0), _VShlI(RS, LaneSize(Size + 1), Wide, 8U << Size));
  return true;
}

// UDOT/SDOT (vector and by element): each 32-bit lane of Rd accumulates the
// four products of the corresponding bytes, modulo 2^32. VUDot (vmsumubm)
// multiplies unsigned bytes. The signed form offsets both operands into the
// unsigned range, a' = a ^ 0x80 = a + 128, and removes the offset terms:
// sum(a * b) = sum(a' * b') - 128 * (sum a' + sum b') + 4 * 128 * 128.
bool IRBuilder::SIMDDotProduct(uint32_t Word, bool Signed, bool ByElement) {
  if (Bits(Word, 23, 22) != 2) {
    return false;
  }
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B {};
  if (ByElement) {
    // The element is a group of four bytes: Rm<index> as a 32-bit lane.
    if (!IntElementOperand(Word, &B)) {
      return false;
    }
  } else {
    B = LoadV(Bits(Word, 20, 16));
  }
  Ref Acc = LoadV(Rd);
  Ref Result {};
  if (!Signed) {
    Result = _VUDot(RS, A, B, Acc);
  } else {
    Ref Flip = LaneConstant(0x80, OpSize::i8Bit);
    Ref Ones = LaneConstant(1, OpSize::i8Bit);
    Ref UA = _VXor(RS, RS, A, Flip);
    Ref UB = _VXor(RS, RS, B, Flip);
    Ref Sums = _VUDot(RS, UB, Ones, _VUDot(RS, UA, Ones, _VectorImm(RS, OpSize::i8Bit, 0)));
    Ref Products = _VUDot(RS, UA, UB, _VAdd(RS, OpSize::i32Bit, Acc, LaneConstant(4 * 128 * 128, OpSize::i32Bit)));
    Result = _VSub(RS, OpSize::i32Bit, Products, _VShlI(RS, OpSize::i32Bit, Sums, 7));
  }
  StoreVQ(Rd, Bit(Word, 30), Result);
  return true;
}

bool IRBuilder::UDOT_vec(uint32_t Word) { return SIMDDotProduct(Word, false, false); }
bool IRBuilder::SDOT_vec(uint32_t Word) { return SIMDDotProduct(Word, true, false); }
bool IRBuilder::UDOT_elt(uint32_t Word) { return SIMDDotProduct(Word, false, true); }
bool IRBuilder::SDOT_elt(uint32_t Word) { return SIMDDotProduct(Word, true, true); }

} // namespace FEXCore::A64
