// SPDX-License-Identifier: MIT
//
// A64 Advanced SIMD floating point on single and double precision lanes:
// three-same arithmetic, FMULX, by-element multiplies, fused multiply-accumulate,
// minimum/maximum, pairwise forms, compares (register and zero, absolute),
// rounding, square root, conversions to and from integer and fixed point,
// and the half-precision sign-bit operations.
//
// Every lane follows the scalar semantics in TranslateFP.cpp: NaN operand
// precedence through PropagateNaNOperand, FMIN/FMAX through FPMinMax, the
// fused group through FPMulAddLanes, conversions through A64FloatToGPR /
// A64FloatFromGPR lane by lane. The operations run on all lanes of the
// 128-bit register; a Q == 0 result has its upper half cleared on store and
// a scalar result keeps only its element.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

namespace FEXCore::A64 {
using namespace FEXCore::IR;

namespace {
  constexpr uint8_t ROUND_TIEEVEN = 0;
  constexpr uint8_t ROUND_POSINF = 1;
  constexpr uint8_t ROUND_NEGINF = 2;
  constexpr uint8_t ROUND_ZERO = 3;
  constexpr uint8_t ROUND_TIEAWAY = 4;

  // Element size from the z bit (22). A 64-bit vector of doubles is unallocated.
  bool FloatLaneSize(uint32_t Word, bool Scalar, OpSize* ES) {
    const bool Z = Bit(Word, 22);
    if (!Scalar && Z && !Bit(Word, 30)) {
      return false;
    }
    *ES = Z ? OpSize::i64Bit : OpSize::i32Bit;
    return true;
  }
} // namespace

void IRBuilder::StoreFloatLanes(uint32_t Word, bool Scalar, OpSize ES, Ref Result) {
  const uint32_t Rd = Bits(Word, 4, 0);
  if (Scalar) {
    StoreVSized(Rd, ES, Result);
  } else {
    StoreVQ(Rd, Bit(Word, 30), Result);
  }
}

// ---------------------------------------------------------------------------
// Three same
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDFloatThreeSame(uint32_t Word, FPBinaryOp Op, bool Scalar) {
  OpSize ES {};
  if (!FloatLaneSize(Word, Scalar, &ES)) {
    return false;
  }
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));
  StoreFloatLanes(Word, Scalar, ES, FPBinaryLanes(Op, ES, A, B));
  return true;
}

Ref IRBuilder::FPBinaryLanes(FPBinaryOp Op, OpSize ES, Ref A, Ref B) {
  const auto RS = OpSize::i128Bit;
  switch (Op) {
  case FPBinaryOp::Add:
  case FPBinaryOp::Sub:
  case FPBinaryOp::Mul:
  case FPBinaryOp::Div: return A64Arith(ES, A, B, Op);
  // FPNeg flips a NaN's sign too, so the negation stays outside the op.
  case FPBinaryOp::NMul: return _VFNeg(RS, ES, A64Arith(ES, A, B, FPBinaryOp::Mul));
  case FPBinaryOp::Min: return FPMinMax(ES, A, B, false, false);
  case FPBinaryOp::Max: return FPMinMax(ES, A, B, true, false);
  case FPBinaryOp::MinNum: return FPMinMax(ES, A, B, false, true);
  case FPBinaryOp::MaxNum: return FPMinMax(ES, A, B, true, true);
  }
  return A;
}

bool IRBuilder::FADD_2(uint32_t Word) { return SIMDFloatThreeSame(Word, FPBinaryOp::Add, false); }
bool IRBuilder::FSUB_2(uint32_t Word) { return SIMDFloatThreeSame(Word, FPBinaryOp::Sub, false); }
bool IRBuilder::FMUL_vec_2(uint32_t Word) { return SIMDFloatThreeSame(Word, FPBinaryOp::Mul, false); }
bool IRBuilder::FDIV_2(uint32_t Word) { return SIMDFloatThreeSame(Word, FPBinaryOp::Div, false); }
bool IRBuilder::FMIN_2(uint32_t Word) { return SIMDFloatThreeSame(Word, FPBinaryOp::Min, false); }
bool IRBuilder::FMAX_2(uint32_t Word) { return SIMDFloatThreeSame(Word, FPBinaryOp::Max, false); }
bool IRBuilder::FMINNM_2(uint32_t Word) { return SIMDFloatThreeSame(Word, FPBinaryOp::MinNum, false); }
bool IRBuilder::FMAXNM_2(uint32_t Word) { return SIMDFloatThreeSame(Word, FPBinaryOp::MaxNum, false); }

// ---------------------------------------------------------------------------
// By element: FMUL, FMLA, FMLS
// ---------------------------------------------------------------------------

// Decodes the element operand of the floating-point by-element group:
// single precision indexes with H:L and double with H (L must be 0); Rm is M:Rm.
bool IRBuilder::FloatElementOperand(uint32_t Word, OpSize ES, Ref* Element) {
  const uint32_t H = Bit(Word, 11), L = Bit(Word, 21), M = Bit(Word, 20);
  const uint32_t Rm = (M << 4) | Bits(Word, 19, 16);
  uint32_t Index {};
  if (ES == OpSize::i64Bit) {
    if (L) {
      return false;
    }
    Index = H;
  } else {
    Index = (H << 1) | L;
  }
  *Element = _VDupElement(OpSize::i128Bit, ES, LoadV(Rm), Index);
  return true;
}

bool IRBuilder::SIMDFloatMulElement(uint32_t Word, int Accumulate, bool Scalar) {
  // Accumulate 0: FMUL, +1: FMLA, -1: FMLS.
  OpSize ES {};
  Ref Element {};
  if (!FloatLaneSize(Word, Scalar, &ES) || !FloatElementOperand(Word, ES, &Element)) {
    return false;
  }
  Ref N = LoadV(Bits(Word, 9, 5));
  Ref Result {};
  if (Accumulate == 0) {
    Result = FPBinaryLanes(FPBinaryOp::Mul, ES, N, Element);
  } else {
    if (Accumulate < 0) {
      N = _VFNeg(OpSize::i128Bit, ES, N);
    }
    Result = FPMulAddLanes(ES, LoadV(Bits(Word, 4, 0)), N, Element);
  }
  StoreFloatLanes(Word, Scalar, ES, Result);
  return true;
}

bool IRBuilder::FMUL_elt_4(uint32_t Word) { return SIMDFloatMulElement(Word, 0, false); }
bool IRBuilder::FMUL_elt_2(uint32_t Word) { return SIMDFloatMulElement(Word, 0, true); }
bool IRBuilder::FMLA_elt_4(uint32_t Word) { return SIMDFloatMulElement(Word, 1, false); }
bool IRBuilder::FMLA_elt_2(uint32_t Word) { return SIMDFloatMulElement(Word, 1, true); }
bool IRBuilder::FMLS_elt_4(uint32_t Word) { return SIMDFloatMulElement(Word, -1, false); }
bool IRBuilder::FMLS_elt_2(uint32_t Word) { return SIMDFloatMulElement(Word, -1, true); }

// FMULX (vector, scalar, by element): FPMulX is FPMul except that an
// infinity times a zero gives 2.0 with the sign of the product.
Ref IRBuilder::FPMulXLanes(OpSize ES, Ref A, Ref B) {
  const auto RS = OpSize::i128Bit;
  const bool Is64 = ES == OpSize::i64Bit;
  Ref Inf = FPConstant(Is64 ? 0x7FF0000000000000ULL : 0x7F800000ULL, ES);
  Ref Zero = _VectorImm(RS, OpSize::i8Bit, 0);
  Ref InfTimesZero = _VOr(RS, RS, _VAnd(RS, RS, _VFCMPEQ(RS, ES, _VFAbs(RS, ES, A), Inf), _VFCMPEQ(RS, ES, B, Zero)),
                          _VAnd(RS, RS, _VFCMPEQ(RS, ES, A, Zero), _VFCMPEQ(RS, ES, _VFAbs(RS, ES, B), Inf)));
  Ref Sign = _VAnd(RS, RS, _VXor(RS, RS, A, B), FPConstant(Is64 ? 0x8000000000000000ULL : 0x80000000ULL, ES));
  Ref Two = _VOr(RS, RS, Sign, FPConstant(Is64 ? 0x4000000000000000ULL : 0x40000000ULL, ES));
  return _VBSL(RS, InfTimesZero, Two, A64Arith(ES, A, B, FPBinaryOp::Mul));
}

bool IRBuilder::SIMDFloatMulX(uint32_t Word, bool Scalar, bool ByElement) {
  OpSize ES {};
  if (!FloatLaneSize(Word, Scalar, &ES)) {
    return false;
  }
  Ref B {};
  if (ByElement) {
    if (!FloatElementOperand(Word, ES, &B)) {
      return false;
    }
  } else {
    B = LoadV(Bits(Word, 20, 16));
  }
  StoreFloatLanes(Word, Scalar, ES, FPMulXLanes(ES, LoadV(Bits(Word, 9, 5)), B));
  return true;
}

bool IRBuilder::FMULX_vec_2(uint32_t Word) { return SIMDFloatMulX(Word, true, false); }
bool IRBuilder::FMULX_vec_4(uint32_t Word) { return SIMDFloatMulX(Word, false, false); }
bool IRBuilder::FMULX_elt_2(uint32_t Word) { return SIMDFloatMulX(Word, true, true); }
bool IRBuilder::FMULX_elt_4(uint32_t Word) { return SIMDFloatMulX(Word, false, true); }

bool IRBuilder::SIMDFloatMulAccumulate(uint32_t Word, bool Subtract) {
  OpSize ES {};
  if (!FloatLaneSize(Word, false, &ES)) {
    return false;
  }
  Ref N = LoadV(Bits(Word, 9, 5));
  if (Subtract) {
    N = _VFNeg(OpSize::i128Bit, ES, N);
  }
  StoreFloatLanes(Word, false, ES, FPMulAddLanes(ES, LoadV(Bits(Word, 4, 0)), N, LoadV(Bits(Word, 20, 16))));
  return true;
}

bool IRBuilder::FMLA_vec_2(uint32_t Word) { return SIMDFloatMulAccumulate(Word, false); }
bool IRBuilder::FMLS_vec_2(uint32_t Word) { return SIMDFloatMulAccumulate(Word, true); }

// ---------------------------------------------------------------------------
// Pairwise
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDFloatPairwise(uint32_t Word, FPBinaryOp Op, bool Scalar) {
  OpSize ES {};
  if (!FloatLaneSize(Word, Scalar, &ES)) {
    return false;
  }
  const auto RS = OpSize::i128Bit;
  Ref A = LoadV(Bits(Word, 9, 5));
  if (Scalar) {
    // FADDP/FMAXP/... Sd/Dd, Vn.2<T>: the two lowest elements.
    StoreFloatLanes(Word, true, ES, FPBinaryLanes(Op, ES, A, _VDupElement(RS, ES, A, 1)));
    return true;
  }
  Ref B = LoadV(Bits(Word, 20, 16));
  // Pairs of the concatenation B:A, as in SIMDPairwise.
  if (!Bit(Word, 30)) {
    A = _VInsElement(RS, OpSize::i64Bit, 1, 0, A, B);
    B = A;
  }
  Ref Even = _VUnZip(RS, ES, A, B);
  Ref Odd = _VUnZip2(RS, ES, A, B);
  StoreFloatLanes(Word, false, ES, FPBinaryLanes(Op, ES, Even, Odd));
  return true;
}

// FMAXV/FMINV/FMAXNMV/FMINNMV Sd, Vn.4S. Only the 4S form is allocated here (the
// half-precision forms are the _1 encodings). The architecture's Reduce()
// halves the vector and combines the halves, so the result is
// op(op(e0, e1), op(e2, e3)) with the lower element as the first operand at
// each step, which decides which NaN propagates when both are NaNs.
bool IRBuilder::SIMDFloatAcrossLanes(uint32_t Word, FPBinaryOp Op) {
  if (!Bit(Word, 30) || Bit(Word, 22)) {
    return false;
  }
  const auto RS = OpSize::i128Bit;
  const auto ES = OpSize::i32Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  // Lanes 0 and 1 of Pairs hold op(e0, e1) and op(e2, e3).
  Ref Pairs = FPBinaryLanes(Op, ES, _VUnZip(RS, ES, V, V), _VUnZip2(RS, ES, V, V));
  StoreVSized(Bits(Word, 4, 0), ES, FPBinaryLanes(Op, ES, Pairs, _VDupElement(RS, ES, Pairs, 1)));
  return true;
}

bool IRBuilder::FMAXV_2(uint32_t Word) { return SIMDFloatAcrossLanes(Word, FPBinaryOp::Max); }
bool IRBuilder::FMINV_2(uint32_t Word) { return SIMDFloatAcrossLanes(Word, FPBinaryOp::Min); }
bool IRBuilder::FMAXNMV_2(uint32_t Word) { return SIMDFloatAcrossLanes(Word, FPBinaryOp::MaxNum); }
bool IRBuilder::FMINNMV_2(uint32_t Word) { return SIMDFloatAcrossLanes(Word, FPBinaryOp::MinNum); }

bool IRBuilder::FADDP_vec_2(uint32_t Word) { return SIMDFloatPairwise(Word, FPBinaryOp::Add, false); }
bool IRBuilder::FMAXP_vec_2(uint32_t Word) { return SIMDFloatPairwise(Word, FPBinaryOp::Max, false); }
bool IRBuilder::FMINP_vec_2(uint32_t Word) { return SIMDFloatPairwise(Word, FPBinaryOp::Min, false); }
bool IRBuilder::FMAXNMP_vec_2(uint32_t Word) { return SIMDFloatPairwise(Word, FPBinaryOp::MaxNum, false); }
bool IRBuilder::FMINNMP_vec_2(uint32_t Word) { return SIMDFloatPairwise(Word, FPBinaryOp::MinNum, false); }
bool IRBuilder::FADDP_pair_2(uint32_t Word) { return SIMDFloatPairwise(Word, FPBinaryOp::Add, true); }
bool IRBuilder::FMAXP_pair_2(uint32_t Word) { return SIMDFloatPairwise(Word, FPBinaryOp::Max, true); }
bool IRBuilder::FMINP_pair_2(uint32_t Word) { return SIMDFloatPairwise(Word, FPBinaryOp::Min, true); }
bool IRBuilder::FMAXNMP_pair_2(uint32_t Word) { return SIMDFloatPairwise(Word, FPBinaryOp::MaxNum, true); }
bool IRBuilder::FMINNMP_pair_2(uint32_t Word) { return SIMDFloatPairwise(Word, FPBinaryOp::MinNum, true); }

// ---------------------------------------------------------------------------
// Compares
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDFloatCompare(uint32_t Word, FloatCompareKind Kind, bool Scalar) {
  // All compares are false for a NaN operand; the result lane is all ones or zero.
  OpSize ES {};
  if (!FloatLaneSize(Word, Scalar, &ES)) {
    return false;
  }
  const auto RS = OpSize::i128Bit;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref Zero = _VectorImm(RS, OpSize::i8Bit, 0);
  Ref Result {};
  switch (Kind) {
  case FloatCompareKind::Eq: Result = _VFCMPEQ(RS, ES, A, LoadV(Bits(Word, 20, 16))); break;
  case FloatCompareKind::Ge: Result = _VFCMPLE(RS, ES, LoadV(Bits(Word, 20, 16)), A); break;
  case FloatCompareKind::Gt: Result = _VFCMPLT(RS, ES, LoadV(Bits(Word, 20, 16)), A); break;
  case FloatCompareKind::AbsGe: Result = _VFCMPLE(RS, ES, _VFAbs(RS, ES, LoadV(Bits(Word, 20, 16))), _VFAbs(RS, ES, A)); break;
  case FloatCompareKind::AbsGt: Result = _VFCMPLT(RS, ES, _VFAbs(RS, ES, LoadV(Bits(Word, 20, 16))), _VFAbs(RS, ES, A)); break;
  case FloatCompareKind::EqZero: Result = _VFCMPEQ(RS, ES, A, Zero); break;
  case FloatCompareKind::GeZero: Result = _VFCMPLE(RS, ES, Zero, A); break;
  case FloatCompareKind::GtZero: Result = _VFCMPLT(RS, ES, Zero, A); break;
  case FloatCompareKind::LeZero: Result = _VFCMPLE(RS, ES, A, Zero); break;
  case FloatCompareKind::LtZero: Result = _VFCMPLT(RS, ES, A, Zero); break;
  }
  StoreFloatLanes(Word, Scalar, ES, Result);
  return true;
}

bool IRBuilder::FCMEQ_reg_4(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::Eq, false); }
bool IRBuilder::FCMGE_reg_4(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::Ge, false); }
bool IRBuilder::FCMGT_reg_4(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::Gt, false); }
bool IRBuilder::FACGE_4(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::AbsGe, false); }
bool IRBuilder::FACGT_4(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::AbsGt, false); }
bool IRBuilder::FCMEQ_zero_4(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::EqZero, false); }
bool IRBuilder::FCMGE_zero_4(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::GeZero, false); }
bool IRBuilder::FCMGT_zero_4(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::GtZero, false); }
bool IRBuilder::FCMLE_4(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::LeZero, false); }
bool IRBuilder::FCMLT_4(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::LtZero, false); }
bool IRBuilder::FCMEQ_reg_2(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::Eq, true); }
bool IRBuilder::FCMGE_reg_2(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::Ge, true); }
bool IRBuilder::FCMGT_reg_2(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::Gt, true); }
bool IRBuilder::FACGE_2(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::AbsGe, true); }
bool IRBuilder::FACGT_2(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::AbsGt, true); }
bool IRBuilder::FCMEQ_zero_2(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::EqZero, true); }
bool IRBuilder::FCMGE_zero_2(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::GeZero, true); }
bool IRBuilder::FCMGT_zero_2(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::GtZero, true); }
bool IRBuilder::FCMLE_2(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::LeZero, true); }
bool IRBuilder::FCMLT_2(uint32_t Word) { return SIMDFloatCompare(Word, FloatCompareKind::LtZero, true); }

// ---------------------------------------------------------------------------
// One register: rounding, square root, half-precision sign operations
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDFloatRound(uint32_t Word, FPRounding Mode) {
  OpSize ES {};
  if (!FloatLaneSize(Word, false, &ES)) {
    return false;
  }
  StoreFloatLanes(Word, false, ES, FPRoundToIntegral(LoadV(Bits(Word, 9, 5)), ES, Mode));
  return true;
}

bool IRBuilder::FRINTN_2(uint32_t Word) { return SIMDFloatRound(Word, FPRounding::TiesEven); }
bool IRBuilder::FRINTP_2(uint32_t Word) { return SIMDFloatRound(Word, FPRounding::PosInf); }
bool IRBuilder::FRINTM_2(uint32_t Word) { return SIMDFloatRound(Word, FPRounding::NegInf); }
bool IRBuilder::FRINTZ_2(uint32_t Word) { return SIMDFloatRound(Word, FPRounding::Zero); }
bool IRBuilder::FRINTA_2(uint32_t Word) { return SIMDFloatRound(Word, FPRounding::TiesAway); }
// POWERARM-M1-TODO(fpu): FRINTX sets FPSR.IXC when inexact; cumulative FPSR exception flags are not emulated.
bool IRBuilder::FRINTX_2(uint32_t Word) { return SIMDFloatRound(Word, FPRounding::Current); }
bool IRBuilder::FRINTI_2(uint32_t Word) { return SIMDFloatRound(Word, FPRounding::Current); }

bool IRBuilder::FSQRT_2(uint32_t Word) {
  OpSize ES {};
  if (!FloatLaneSize(Word, false, &ES)) {
    return false;
  }
  StoreFloatLanes(Word, false, ES, _VFSqrt(OpSize::i128Bit, ES, LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::SIMDHalfSign(uint32_t Word, bool IsNeg) {
  // FNEG/FABS on half-precision lanes touch only the sign bits.
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Result = IsNeg ? _VXor(RS, RS, V, FPConstant(0x8000, OpSize::i16Bit)).Node : _VAnd(RS, RS, V, FPConstant(0x7FFF, OpSize::i16Bit)).Node;
  StoreVQ(Bits(Word, 4, 0), Bit(Word, 30), Result);
  return true;
}

bool IRBuilder::FNEG_1(uint32_t Word) { return SIMDHalfSign(Word, true); }
bool IRBuilder::FABS_1(uint32_t Word) { return SIMDHalfSign(Word, false); }

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDFloatToInt(uint32_t Word, uint8_t Rounding, bool Signed, bool Scalar) {
  OpSize ES {};
  if (!FloatLaneSize(Word, Scalar, &ES)) {
    return false;
  }
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  const uint8_t Lanes = Scalar ? 1 : (Bit(Word, 30) ? 16 : 8) / IR::OpSizeToSize(ES);
  Ref Result = _VectorImm(RS, OpSize::i8Bit, 0);
  for (uint8_t i = 0; i < Lanes; ++i) {
    Ref Lane = i == 0 ? V : _VDupElement(RS, ES, V, i).Node;
    Result = _VInsGPR(RS, ES, i, Result, _A64FloatToGPR(ES, ES, Lane, Rounding, Signed));
  }
  StoreV(Bits(Word, 4, 0), Result);
  return true;
}

bool IRBuilder::FCVTNS_4(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_TIEEVEN, true, false); }
bool IRBuilder::FCVTNU_4(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_TIEEVEN, false, false); }
bool IRBuilder::FCVTPS_4(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_POSINF, true, false); }
bool IRBuilder::FCVTPU_4(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_POSINF, false, false); }
bool IRBuilder::FCVTMS_4(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_NEGINF, true, false); }
bool IRBuilder::FCVTMU_4(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_NEGINF, false, false); }
bool IRBuilder::FCVTZS_int_4(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_ZERO, true, false); }
bool IRBuilder::FCVTZU_int_4(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_ZERO, false, false); }
bool IRBuilder::FCVTAS_4(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_TIEAWAY, true, false); }
bool IRBuilder::FCVTAU_4(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_TIEAWAY, false, false); }
bool IRBuilder::FCVTNS_2(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_TIEEVEN, true, true); }
bool IRBuilder::FCVTNU_2(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_TIEEVEN, false, true); }
bool IRBuilder::FCVTPS_2(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_POSINF, true, true); }
bool IRBuilder::FCVTPU_2(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_POSINF, false, true); }
bool IRBuilder::FCVTMS_2(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_NEGINF, true, true); }
bool IRBuilder::FCVTMU_2(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_NEGINF, false, true); }
bool IRBuilder::FCVTAS_2(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_TIEAWAY, true, true); }
bool IRBuilder::FCVTAU_2(uint32_t Word) { return SIMDFloatToInt(Word, ROUND_TIEAWAY, false, true); }

bool IRBuilder::SIMDFixedConvert(uint32_t Word, bool ToFloat, bool Signed, bool Scalar) {
  // SCVTF/UCVTF/FCVTZS/FCVTZU with fbits = 2 * esize - immh:immb. immh 001x
  // is half precision (TranslateSIMDHalf.cpp), 01xx single, 1xxx double;
  // immh 0001 is unallocated.
  const uint32_t Immh = Bits(Word, 22, 19);
  const bool Q = Scalar || Bit(Word, 30);
  if ((Immh & 0b1110) == 0b0010) {
    return SIMDHalfFixedConvert(Word, ToFloat, Signed, Scalar);
  }
  if ((Immh & 0b1100) == 0 || (!Q && (Immh & 0b1000))) {
    return false;
  }
  const bool Is64 = (Immh & 0b1000) != 0;
  const auto ES = Is64 ? OpSize::i64Bit : OpSize::i32Bit;
  const uint32_t FBits = (Is64 ? 128 : 64) - Bits(Word, 22, 16);
  const auto RS = OpSize::i128Bit;
  const uint8_t Lanes = Scalar ? 1 : (Bit(Word, 30) ? 16 : 8) / IR::OpSizeToSize(ES);
  Ref V = LoadV(Bits(Word, 9, 5));

  Ref Result = _VectorImm(RS, OpSize::i8Bit, 0);
  if (ToFloat) {
    for (uint8_t i = 0; i < Lanes; ++i) {
      Result = _VInsElement(RS, ES, i, 0, Result, _A64FloatFromGPR(ES, ES, _VExtractToGPR(RS, ES, V, i), Signed));
    }
    // One rounding in the conversion; the power-of-two scale is exact
    // (magnitudes stay at or above 2^-64).
    const uint64_t InvScale = Is64 ? (static_cast<uint64_t>(1023 - FBits) << 52) : (static_cast<uint64_t>(127 - FBits) << 23);
    Result = _VFMul(RS, ES, Result, FPConstant(InvScale, ES));
  } else {
    // Multiplying by 2^fbits is exact or overflows to an infinity, which
    // saturates like the exact product.
    const uint64_t ScaleBits = Is64 ? (static_cast<uint64_t>(1023 + FBits) << 52) : (static_cast<uint64_t>(127 + FBits) << 23);
    Ref Scaled = _VFMul(RS, ES, V, FPConstant(ScaleBits, ES));
    for (uint8_t i = 0; i < Lanes; ++i) {
      Ref Lane = i == 0 ? Scaled : _VDupElement(RS, ES, Scaled, i).Node;
      Result = _VInsGPR(RS, ES, i, Result, _A64FloatToGPR(ES, ES, Lane, ROUND_ZERO, Signed));
    }
  }
  if (Scalar) {
    StoreVSized(Bits(Word, 4, 0), ES, Result);
  } else {
    StoreVQ(Bits(Word, 4, 0), Bit(Word, 30), Result);
  }
  return true;
}

bool IRBuilder::SCVTF_fix_2(uint32_t Word) { return SIMDFixedConvert(Word, true, true, false); }
bool IRBuilder::UCVTF_fix_2(uint32_t Word) { return SIMDFixedConvert(Word, true, false, false); }
bool IRBuilder::FCVTZS_fix_2(uint32_t Word) { return SIMDFixedConvert(Word, false, true, false); }
bool IRBuilder::FCVTZU_fix_2(uint32_t Word) { return SIMDFixedConvert(Word, false, false, false); }
bool IRBuilder::SCVTF_fix_1(uint32_t Word) { return SIMDFixedConvert(Word, true, true, true); }
bool IRBuilder::UCVTF_fix_1(uint32_t Word) { return SIMDFixedConvert(Word, true, false, true); }
bool IRBuilder::FCVTZS_fix_1(uint32_t Word) { return SIMDFixedConvert(Word, false, true, true); }
bool IRBuilder::FCVTZU_fix_1(uint32_t Word) { return SIMDFixedConvert(Word, false, false, true); }

} // namespace FEXCore::A64
