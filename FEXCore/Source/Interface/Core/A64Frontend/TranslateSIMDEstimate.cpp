// SPDX-License-Identifier: MIT
//
// A64 reciprocal and reciprocal square root estimates and their
// Newton-Raphson steps, on single and double precision lanes (vector and
// scalar SIMD forms): FRECPE, FRSQRTE, FRECPX, FRECPS, FRSQRTS, and the
// unsigned fixed-point estimates URECPE and URSQRTE.
//
// The estimates are the architecture's 8-bit tables (RecipEstimate,
// RecipSqrtEstimate), which no host instruction reproduces. Each table entry
// is a short integer computation on a 9- or 10-bit input, done here exactly
// in floating point on every lane at once:
//  * RecipEstimate(a): b = floor(2^19 / (2a + 1)), r = (b + 1) >> 1. The
//    quotient's fractional part is at least 1/1023, far above the error of
//    one rounded division in any rounding mode, so floor() is exact.
//  * RecipSqrtEstimate(a): with a' the table's rounded input, r = c >> 1
//    where c = ceil(2^14 / sqrt(a')), the least c with a' * c^2 >= 2^28.
//    2^14 / sqrt(a') can lie within 1e-6 of an integer, closer than a
//    single-precision estimate resolves, so c is found from that estimate
//    and settled by the sign of the exact a' * c^2 - 2^28 (one fused
//    multiply-add: rounding keeps the sign).
// The table input a' is built directly as a floating-point value from the
// operand's fraction bits, and the output r (256..511) is read back from the
// fraction bits of its floating-point value, which hold r - 256 at the top.
//
// Special cases follow FPRecipEstimate/FPRSqrtEstimate: NaNs are quieted,
// zeros give infinities, infinities give zeros, a negative operand of
// FRSQRTE gives the default NaN, and an FRECPE operand too small for a
// finite reciprocal overflows to infinity or the largest finite value by
// FPCR.RMode and its sign. As elsewhere FPCR.FZ and FPCR.DN have no effect
// (TranslateFP.cpp POWERARM-M1-TODO(fpu)).
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

#include <FEXCore/Core/CoreState.h>

namespace FEXCore::A64 {
using namespace FEXCore::IR;

namespace {
  // The IEEE layout of a single or double precision lane.
  struct FloatLayout {
    unsigned Bits;
    unsigned FracBits;
    uint64_t Sign;
    uint64_t ExpField;
    uint64_t FracMask;
    uint64_t Quiet;
    uint64_t Inf;
    uint64_t MaxNormal;
    uint64_t DefaultNaN;
    // The biased exponent field of 2^8 and 2^9.
    uint64_t Exp256;
    uint64_t Exp512;
  };

  FloatLayout LayoutFor(OpSize ES) {
    if (ES == OpSize::i64Bit) {
      return {64,
              52,
              1ULL << 63,
              0x7FFULL << 52,
              (1ULL << 52) - 1,
              1ULL << 51,
              0x7FF0000000000000ULL,
              0x7FEFFFFFFFFFFFFFULL,
              0x7FF8000000000000ULL,
              (1023ULL + 8) << 52,
              (1023ULL + 9) << 52};
    }
    return {32, 23, 1ULL << 31, 0xFFULL << 23, (1ULL << 23) - 1, 1ULL << 22, 0x7F800000ULL,
            0x7F7FFFFFULL, 0x7FC00000ULL, (127ULL + 8) << 23, (127ULL + 9) << 23};
  }

  // A floating-point constant of the lane's precision.
  uint64_t FloatBits(double Value, OpSize ES) {
    if (ES == OpSize::i64Bit) {
      uint64_t Out;
      __builtin_memcpy(&Out, &Value, sizeof(Out));
      return Out;
    }
    const float F = static_cast<float>(Value);
    uint32_t Out;
    __builtin_memcpy(&Out, &F, sizeof(Out));
    return Out;
  }

  // Element size from the z bit (22); a 64-bit vector of doubles is unallocated.
  bool EstimateLaneSize(uint32_t Word, bool Scalar, OpSize* ES) {
    const bool Z = Bit(Word, 22);
    if (!Scalar && Z && !Bit(Word, 30)) {
      return false;
    }
    *ES = Z ? OpSize::i64Bit : OpSize::i32Bit;
    return true;
  }
} // namespace

// ---------------------------------------------------------------------------
// The estimate tables
// ---------------------------------------------------------------------------

// RecipEstimate: A holds the table input 2a + 1 (513..1023) as a float of
// the lane's precision. Returns r (256..511) as a float of the same precision.
Ref IRBuilder::RecipEstimateTable(OpSize ES, Ref A) {
  const auto RS = OpSize::i128Bit;
  Ref Quotient = _VFDiv(RS, ES, FPConstant(FloatBits(524288.0, ES), ES), A);
  Ref B = _Vector_FToI(RS, ES, Quotient, RoundMode::NegInfinity);
  Ref Sum = _VFAdd(RS, ES, B, FPConstant(FloatBits(1.0, ES), ES));
  return _Vector_FToI(RS, ES, _VFMul(RS, ES, Sum, FPConstant(FloatBits(0.5, ES), ES)), RoundMode::NegInfinity);
}

// RecipSqrtEstimate: A holds the rounded table input a' (257..1022) as a
// float of the lane's precision. Returns r (256..511) as a float of the
// same precision.
Ref IRBuilder::RecipSqrtEstimateTable(OpSize ES, Ref A) {
  const auto RS = OpSize::i128Bit;
  Ref One = FPConstant(FloatBits(1.0, ES), ES);
  Ref Estimate = _VFDiv(RS, ES, FPConstant(FloatBits(16384.0, ES), ES), _VFSqrt(RS, ES, A));
  // The estimate is within a few units in the last place of 2^14 / sqrt(a'),
  // so c is one of C0 - 1, C0, C0 + 1.
  Ref C0 = _Vector_FToI(RS, ES, Estimate, RoundMode::PosInfinity);
  Ref CM = _VFSub(RS, ES, C0, One);
  Ref Minus2p28 = FPConstant(FloatBits(-268435456.0, ES), ES);
  Ref Zero = _VectorImm(RS, OpSize::i8Bit, 0);
  // c^2 is exact (c <= 1025); a' * c^2 - 2^28 is rounded once and never zero.
  Ref CMEnough = _VFCMPLE(RS, ES, Zero, _VFMLA(RS, ES, A, _VFMul(RS, ES, CM, CM), Minus2p28));
  Ref C0Enough = _VFCMPLE(RS, ES, Zero, _VFMLA(RS, ES, A, _VFMul(RS, ES, C0, C0), Minus2p28));
  Ref C = _VBSL(RS, CMEnough, CM, _VBSL(RS, C0Enough, C0, _VFAdd(RS, ES, C0, One)));
  return _Vector_FToI(RS, ES, _VFMul(RS, ES, C, FPConstant(FloatBits(0.5, ES), ES)), RoundMode::NegInfinity);
}

// ---------------------------------------------------------------------------
// FRECPE, FRSQRTE, FRECPX
// ---------------------------------------------------------------------------

// All-ones lanes where an FRECPE overflow of an operand with the lane's sign
// rounds to infinity: always under RN, for positive operands under RP, for
// negative ones under RM, never under RZ.
Ref IRBuilder::OverflowToInfinityMask(OpSize ES, Ref X) {
  const auto RS = OpSize::i128Bit;
  Ref FPCR = _LoadContext(OpSize::i32Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, fpcr));
  // RMode is FPCR<23:22>: 00 RN, 01 RP, 10 RM, 11 RZ.
  Ref PosInf = _Sub(OpSize::i64Bit, _Bfe(OpSize::i64Bit, 1, 23, FPCR), Constant(1));
  Ref NegInf = _Sub(OpSize::i64Bit, _Bfe(OpSize::i64Bit, 1, 22, FPCR), Constant(1));
  return _VBSL(RS, _VCMPLTZ(RS, ES, X), _VDupFromGPR(RS, ES, NegInf), _VDupFromGPR(RS, ES, PosInf));
}

Ref IRBuilder::FPRecipEstimateLanes(OpSize ES, Ref X) {
  const auto RS = OpSize::i128Bit;
  const auto L = LayoutFor(ES);
  const unsigned F = L.FracBits;
  Ref Sign = _VAnd(RS, RS, X, LaneConstant(L.Sign, ES));
  Ref Frac = _VAnd(RS, RS, X, LaneConstant(L.FracMask, ES));
  Ref ExpBits = _VAnd(RS, RS, X, LaneConstant(L.ExpField, ES));
  Ref Exp = _VUShrI(RS, ES, ExpBits, F);
  Ref IsDenormal = _VCMPEQZ(RS, ES, ExpBits);

  // A denormal operand is scaled by one or two places: frac<top> set gives
  // exponent 0 and one place, clear gives exponent -1 and two places (only
  // those with one of the top two fraction bits set reach the table).
  Ref TopSet = _VNot(RS, ES, _VCMPEQZ(RS, ES, _VAnd(RS, RS, X, LaneConstant(1ULL << (F - 1), ES))));
  Ref DenormalFrac = _VBSL(RS, TopSet, _VShlI(RS, ES, Frac, 1), _VShlI(RS, ES, Frac, 2));
  Ref TableFrac = _VBSL(RS, IsDenormal, DenormalFrac, Frac);
  Exp = _VAdd(RS, ES, Exp, _VAndn(RS, RS, IsDenormal, TopSet));

  // Table input 2 * (256 + frac<top 8>) + 1 = 2^9 * (1 + (frac<top 8> : 1) / 2^9).
  const uint64_t Top8 = 0xFFULL << (F - 8);
  Ref A = _VOr(RS, RS, _VAnd(RS, RS, TableFrac, LaneConstant(Top8, ES)), LaneConstant(L.Exp512 | (1ULL << (F - 9)), ES));
  Ref Estimate = _VAnd(RS, RS, RecipEstimateTable(ES, A), LaneConstant(Top8, ES));

  // Result exponent K - exp (K = 253 / 2045), in -1..K+1. Exponents 0 and -1
  // are denormal results: the significand 1.estimate shifted right once more.
  const uint64_t K = F == 52 ? 2045 : 253;
  Ref Normal = _VOr(RS, RS, _VShlI(RS, ES, _VSub(RS, ES, LaneConstant(K, ES), Exp), F), Estimate);
  Ref Significand = _VOr(RS, RS, Estimate, LaneConstant(1ULL << F, ES));
  Ref Magnitude = _VBSL(RS, _VCMPEQ(RS, ES, Exp, LaneConstant(K, ES)), _VUShrI(RS, ES, Significand, 1), Normal);
  Magnitude = _VBSL(RS, _VCMPEQ(RS, ES, Exp, LaneConstant(K + 1, ES)), _VUShrI(RS, ES, Significand, 2), Magnitude);
  Ref Result = _VOr(RS, RS, Sign, Magnitude);

  // |x| < 2^-128 (2^-1024): the reciprocal overflows.
  Ref IsZero = _VCMPEQZ(RS, ES, _VShlI(RS, ES, X, 1));
  Ref Tiny = _VAndn(RS, RS, _VAnd(RS, RS, IsDenormal, _VCMPEQZ(RS, ES, _VUShrI(RS, ES, Frac, F - 2))), IsZero);
  Ref Overflow = _VOr(RS, RS, Sign, _VSub(RS, ES, LaneConstant(L.MaxNormal, ES), OverflowToInfinityMask(ES, X)));
  Result = _VBSL(RS, Tiny, Overflow, Result);
  Result = _VBSL(RS, IsZero, _VOr(RS, RS, Sign, LaneConstant(L.Inf, ES)), Result);
  Ref InfOrNaN = _VCMPEQ(RS, ES, ExpBits, LaneConstant(L.ExpField, ES));
  Ref FracZero = _VCMPEQZ(RS, ES, Frac);
  Result = _VBSL(RS, _VAnd(RS, RS, InfOrNaN, FracZero), Sign, Result);
  return _VBSL(RS, _VAndn(RS, RS, InfOrNaN, FracZero), _VOr(RS, RS, X, LaneConstant(L.Quiet, ES)), Result);
}

Ref IRBuilder::FPRSqrtEstimateLanes(OpSize ES, Ref X) {
  const auto RS = OpSize::i128Bit;
  const auto L = LayoutFor(ES);
  const unsigned F = L.FracBits;
  const unsigned ExpWidthPlusSign = L.Bits - F;
  Ref Frac = _VAnd(RS, RS, X, LaneConstant(L.FracMask, ES));
  Ref ExpBits = _VAnd(RS, RS, X, LaneConstant(L.ExpField, ES));
  Ref IsDenormal = _VCMPEQZ(RS, ES, ExpBits);

  // A denormal operand is normalised: exponent -lz, the fraction shifted
  // past its leading one (lz leading zeros in the F-bit fraction).
  Ref Clz = _VCLZ(RS, ES, Frac);
  Ref Shift = _VSub(RS, ES, Clz, LaneConstant(ExpWidthPlusSign - 1, ES));
  Ref DenormalFrac = _VAnd(RS, RS, _VUShl(RS, ES, Frac, Shift, true), LaneConstant(L.FracMask, ES));
  Ref TableFrac = _VBSL(RS, IsDenormal, DenormalFrac, Frac);
  Ref Exp = _VBSL(RS, IsDenormal, _VSub(RS, ES, LaneConstant(ExpWidthPlusSign, ES), Clz), _VUShrI(RS, ES, ExpBits, F));

  // Table input: an odd biased exponent (an even power of two) scales into
  // [0.25, 0.5) with frac<top 7>, a' = 2 * (128 + frac<top 7>) + 1; an even
  // one into [0.5, 1) with frac<top 8>, a' = 2 * ((256 + frac<top 8>) | 1).
  // Both as floats.
  const uint64_t Top7 = 0x7FULL << (F - 7);
  const uint64_t Top8 = 0xFFULL << (F - 8);
  const uint64_t LowOne = 1ULL << (F - 8);
  Ref Quarter = _VOr(RS, RS, _VAnd(RS, RS, TableFrac, LaneConstant(Top7, ES)), LaneConstant(L.Exp256 | LowOne, ES));
  Ref Half = _VOr(RS, RS, _VAnd(RS, RS, TableFrac, LaneConstant(Top8, ES)), LaneConstant(L.Exp512 | LowOne, ES));
  Ref IsOdd = _VSShrI(RS, ES, _VShlI(RS, ES, Exp, L.Bits - 1), L.Bits - 1);
  Ref A = _VBSL(RS, IsOdd, Quarter, Half);
  Ref Estimate = _VAnd(RS, RS, RecipSqrtEstimateTable(ES, A), LaneConstant(Top8, ES));

  // Result exponent (C - exp) / 2, C = 380 / 3068, always a normal exponent.
  const uint64_t C = F == 52 ? 3068 : 380;
  Ref ResultExp = _VSShrI(RS, ES, _VSub(RS, ES, LaneConstant(C, ES), Exp), 1);
  Ref Result = _VOr(RS, RS, _VShlI(RS, ES, ResultExp, F), Estimate);

  Ref Sign = _VAnd(RS, RS, X, LaneConstant(L.Sign, ES));
  Ref IsZero = _VCMPEQZ(RS, ES, _VShlI(RS, ES, X, 1));
  Ref InfOrNaN = _VCMPEQ(RS, ES, ExpBits, LaneConstant(L.ExpField, ES));
  Ref FracZero = _VCMPEQZ(RS, ES, Frac);
  Ref IsNaN = _VAndn(RS, RS, InfOrNaN, FracZero);
  // +Inf gives +0; a negative operand (not a zero) the default NaN.
  Result = _VAndn(RS, RS, Result, _VAnd(RS, RS, InfOrNaN, FracZero));
  Result = _VBSL(RS, _VCMPLTZ(RS, ES, X), LaneConstant(L.DefaultNaN, ES), Result);
  Result = _VBSL(RS, IsZero, _VOr(RS, RS, Sign, LaneConstant(L.Inf, ES)), Result);
  return _VBSL(RS, IsNaN, _VOr(RS, RS, X, LaneConstant(L.Quiet, ES)), Result);
}

bool IRBuilder::SIMDFloatEstimate(uint32_t Word, bool Sqrt, bool Scalar) {
  OpSize ES {};
  if (!EstimateLaneSize(Word, Scalar, &ES)) {
    return false;
  }
  Ref X = LoadV(Bits(Word, 9, 5));
  StoreFloatLanes(Word, Scalar, ES, Sqrt ? FPRSqrtEstimateLanes(ES, X) : FPRecipEstimateLanes(ES, X));
  return true;
}

bool IRBuilder::FRECPE_2(uint32_t Word) { return SIMDFloatEstimate(Word, false, true); }
bool IRBuilder::FRECPE_4(uint32_t Word) { return SIMDFloatEstimate(Word, false, false); }
bool IRBuilder::FRSQRTE_2(uint32_t Word) { return SIMDFloatEstimate(Word, true, true); }
bool IRBuilder::FRSQRTE_4(uint32_t Word) { return SIMDFloatEstimate(Word, true, false); }

// FRECPX: the sign, the exponent field inverted (a zero or denormal operand
// gets the largest finite exponent) and a zero fraction; NaNs are quieted.
bool IRBuilder::FRECPX_2(uint32_t Word) {
  const auto ES = Bit(Word, 22) ? OpSize::i64Bit : OpSize::i32Bit;
  const auto RS = OpSize::i128Bit;
  const auto L = LayoutFor(ES);
  Ref X = LoadV(Bits(Word, 9, 5));
  Ref ExpBits = _VAnd(RS, RS, X, LaneConstant(L.ExpField, ES));
  Ref Inverted = _VXor(RS, RS, ExpBits, LaneConstant(L.ExpField, ES));
  Ref MaxExp = LaneConstant(L.ExpField - (1ULL << L.FracBits), ES);
  Ref Result = _VOr(RS, RS, _VAnd(RS, RS, X, LaneConstant(L.Sign, ES)), _VBSL(RS, _VCMPEQZ(RS, ES, ExpBits), MaxExp, Inverted));
  Ref IsNaN = _VAndn(RS, RS, _VCMPEQ(RS, ES, ExpBits, LaneConstant(L.ExpField, ES)),
                     _VCMPEQZ(RS, ES, _VAnd(RS, RS, X, LaneConstant(L.FracMask, ES))));
  StoreVSized(Bits(Word, 4, 0), ES, _VBSL(RS, IsNaN, _VOr(RS, RS, X, LaneConstant(L.Quiet, ES)), Result));
  return true;
}

// ---------------------------------------------------------------------------
// URECPE, URSQRTE
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDUnsignedEstimate(uint32_t Word, bool Sqrt) {
  // 32-bit lanes only; the operand is a fixed-point value in [0, 1) whose
  // top nine bits index the table, and the estimate fills the top nine bits
  // of the result. Operands below 0.5 (0.25 for URSQRTE) give all ones.
  if (Bit(Word, 22)) {
    return false;
  }
  const auto ES = OpSize::i32Bit;
  const auto RS = OpSize::i128Bit;
  const auto L = LayoutFor(ES);
  Ref V = LoadV(Bits(Word, 9, 5));
  // Element<30:23> at the top of a single-precision fraction.
  Ref Top8 = _VAnd(RS, RS, _VUShrI(RS, ES, V, 8), LaneConstant(0x7F8000, ES));
  Ref Estimate {};
  Ref Small {};
  if (!Sqrt) {
    Estimate = RecipEstimateTable(ES, _VOr(RS, RS, Top8, LaneConstant(L.Exp512 | 0x4000, ES)));
    Small = _VNot(RS, ES, _VCMPLTZ(RS, ES, V));
  } else {
    // Element<31> clear: a' = 2 * element<31:23> + 1 from element<29:23>;
    // set: a' = 2 * (element<31:23> | 1).
    Ref Top7 = _VAnd(RS, RS, _VUShrI(RS, ES, V, 7), LaneConstant(0x7F0000, ES));
    Ref Even = _VOr(RS, RS, Top7, LaneConstant(L.Exp256 | 0x8000, ES));
    Ref Odd = _VOr(RS, RS, Top8, LaneConstant(L.Exp512 | 0x8000, ES));
    Estimate = RecipSqrtEstimateTable(ES, _VBSL(RS, _VCMPLTZ(RS, ES, V), Odd, Even));
    Small = _VCMPEQZ(RS, ES, _VUShrI(RS, ES, V, 30));
  }
  // r (256..511) as the top nine bits: 1 : (r - 256) << 23.
  Ref Result = _VOr(RS, RS, _VShlI(RS, ES, _VAnd(RS, RS, Estimate, LaneConstant(0x7F8000, ES)), 8), LaneConstant(L.Sign, ES));
  StoreVQ(Bits(Word, 4, 0), Bit(Word, 30), _VOr(RS, RS, Result, Small));
  return true;
}

bool IRBuilder::URECPE(uint32_t Word) { return SIMDUnsignedEstimate(Word, false); }
bool IRBuilder::URSQRTE(uint32_t Word) { return SIMDUnsignedEstimate(Word, true); }

// ---------------------------------------------------------------------------
// FRECPS, FRSQRTS
// ---------------------------------------------------------------------------

// FPRecipStepFused (2 - op1 * op2) and FPRSqrtStepFused ((3 - op1 * op2) / 2),
// each with one rounding. op1 is negated first, NaN or not, so a NaN op1
// propagates with its sign flipped. inf * 0 gives 2.0 (1.5).
Ref IRBuilder::FPStepFusedLanes(OpSize ES, Ref Op1, Ref Op2, bool Sqrt) {
  const auto RS = OpSize::i128Bit;
  Ref N = _VFNeg(RS, ES, Op1);
  Ref M = Op2;
  Ref AbsN = _VFAbs(RS, ES, N);
  Ref AbsM = _VFAbs(RS, ES, M);
  Ref Result {};
  if (!Sqrt) {
    Result = _VFMLA(RS, ES, N, M, FPConstant(FloatBits(2.0, ES), ES));
  } else {
    // (3 + n * m) / 2. The fused 3 + n * m can overflow where the halved
    // value does not, so when the larger operand is at least 1 the halving is
    // moved onto it (exact there) and 1.5 + (big / 2) * small is rounded
    // once. Otherwise |n * m| < 1, 3 + n * m lies in (2, 4), and halving the
    // rounded sum is exact.
    Ref Half = FPConstant(FloatBits(0.5, ES), ES);
    Ref NBigger = _VFCMPLE(RS, ES, AbsM, AbsN);
    Ref Big = _VBSL(RS, NBigger, N, M);
    Ref Small = _VBSL(RS, NBigger, M, N);
    Ref Moved = _VFMLA(RS, ES, _VFMul(RS, ES, Big, Half), Small, FPConstant(FloatBits(1.5, ES), ES));
    Ref Halved = _VFMul(RS, ES, _VFMLA(RS, ES, N, M, FPConstant(FloatBits(3.0, ES), ES)), Half);
    Ref UseMoved = _VFCMPLE(RS, ES, FPConstant(FloatBits(1.0, ES), ES), _VBSL(RS, NBigger, AbsN, AbsM));
    Result = _VBSL(RS, UseMoved, Moved, Halved);
  }
  Ref Inf = FPConstant(FloatBits(__builtin_inf(), ES), ES);
  Ref Zero = _VectorImm(RS, OpSize::i8Bit, 0);
  Ref InfTimesZero = _VOr(RS, RS, _VAnd(RS, RS, _VFCMPEQ(RS, ES, AbsN, Inf), _VFCMPEQ(RS, ES, M, Zero)),
                          _VAnd(RS, RS, _VFCMPEQ(RS, ES, N, Zero), _VFCMPEQ(RS, ES, AbsM, Inf)));
  Result = _VBSL(RS, InfTimesZero, FPConstant(FloatBits(Sqrt ? 1.5 : 2.0, ES), ES), Result);
  Ref NaNResult = _VFAdd(RS, ES, PropagateNaNOperand(ES, N, M), M);
  return _VBSL(RS, _VFCMPUNO(RS, ES, N, M), NaNResult, Result);
}

bool IRBuilder::SIMDFloatStep(uint32_t Word, bool Sqrt, bool Scalar) {
  OpSize ES {};
  if (!EstimateLaneSize(Word, Scalar, &ES)) {
    return false;
  }
  Ref Result = FPStepFusedLanes(ES, LoadV(Bits(Word, 9, 5)), LoadV(Bits(Word, 20, 16)), Sqrt);
  StoreFloatLanes(Word, Scalar, ES, Result);
  return true;
}

bool IRBuilder::FRECPS_2(uint32_t Word) { return SIMDFloatStep(Word, false, true); }
bool IRBuilder::FRECPS_4(uint32_t Word) { return SIMDFloatStep(Word, false, false); }
bool IRBuilder::FRSQRTS_2(uint32_t Word) { return SIMDFloatStep(Word, true, true); }
bool IRBuilder::FRSQRTS_4(uint32_t Word) { return SIMDFloatStep(Word, true, false); }

} // namespace FEXCore::A64
