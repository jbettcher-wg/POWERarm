// SPDX-License-Identifier: MIT
//
// A64 Advanced SIMD half-precision arithmetic (FEAT_FP16, HWCAP asimdhp) on
// 4H/8H vectors and H scalars: three-same arithmetic, min/max, FABD, FMULX,
// the fused multiply-accumulates, FRECPS/FRSQRTS, compares, pairwise and
// across-lane forms, by-element multiplies, rounding, square root,
// conversions to and from 16-bit integers and fixed point, and FMOV
// (vector, immediate). FNEG/FABS are TranslateSIMDFloat.cpp's sign-bit
// operations and FRECPE/FRSQRTE/FRECPX are in TranslateSIMDEstimate.cpp.
//
// The host has no half-precision arithmetic. Each lane runs through the
// scalar half-precision method of TranslateFP.cpp: widened to double
// exactly (HalfToDouble, with FPCR.FZ16 on the operands), the operation in
// double with the A64 NaN rules of the single/double helpers, and one
// rounding to half precision (DoubleToHalf, FZ16 on the result). A double
// result of +, -, *, /, sqrt or a fused multiply-add of half-precision
// operands is exact or never lands within 2^-53 of a half-precision rounding
// boundary it is not on, so the one rounding at the end is the operation's.
// Min/max, compares and conversions do not round in double at all.
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

  uint32_t HalfLaneCount(uint32_t Word, bool Scalar) {
    return Scalar ? 1 : (Bit(Word, 30) ? 8 : 4);
  }
} // namespace

// Every half-precision lane of the operands, one at a time: Body gets the
// lane's operands widened to double in element 0 and returns the lane's
// 16-bit result in element 0.
template<typename F>
Ref IRBuilder::HalfLanes(uint32_t Lanes, std::array<Ref, 3> Operands, bool KeepSignalling, F&& Body) {
  const auto RS = OpSize::i128Bit;
  Ref Result = _VectorImm(RS, OpSize::i8Bit, 0);
  for (uint32_t i = 0; i < Lanes; ++i) {
    std::array<Ref, 3> D {};
    for (size_t k = 0; k < Operands.size(); ++k) {
      if (Operands[k]) {
        Ref Lane = i == 0 ? Operands[k] : _VDupElement(RS, OpSize::i16Bit, Operands[k], i).Node;
        D[k] = HalfToDouble(Lane, KeepSignalling, true);
      }
    }
    Result = _VInsElement(RS, OpSize::i16Bit, i, 0, Result, Body(D));
  }
  return Result;
}

void IRBuilder::StoreHalfLanes(uint32_t Word, bool Scalar, Ref Result) {
  const uint32_t Rd = Bits(Word, 4, 0);
  if (Scalar) {
    StoreVSized(Rd, OpSize::i16Bit, Result);
  } else {
    StoreVQ(Rd, Bit(Word, 30), Result);
  }
}

// One binary operation on doubles widened from half precision (signalling
// NaNs kept signalling), rounded to half precision.
Ref IRBuilder::HalfBinaryLane(HalfOp Op, Ref A, Ref B) {
  const auto RS = OpSize::i128Bit;
  const auto D = OpSize::i64Bit;
  switch (Op) {
  case HalfOp::Add: return DoubleToHalf(A64Arith(D, A, B, FPBinaryOp::Add), true);
  case HalfOp::Sub: return DoubleToHalf(A64Arith(D, A, B, FPBinaryOp::Sub), true);
  case HalfOp::Mul: return DoubleToHalf(A64Arith(D, A, B, FPBinaryOp::Mul), true);
  case HalfOp::Div: return DoubleToHalf(A64Arith(D, A, B, FPBinaryOp::Div), true);
  case HalfOp::Min: return DoubleToHalf(FPMinMax(D, A, B, false, false), true);
  case HalfOp::Max: return DoubleToHalf(FPMinMax(D, A, B, true, false), true);
  case HalfOp::MinNum: return DoubleToHalf(FPMinMax(D, A, B, false, true), true);
  case HalfOp::MaxNum: return DoubleToHalf(FPMinMax(D, A, B, true, true), true);
  case HalfOp::MulX: return DoubleToHalf(FPMulXLanes(D, A, B), true);
  // FPAbs after the rounding: rounding the magnitude would round a negative
  // difference the other way under RP and RM.
  case HalfOp::Abd: return _VAnd(RS, RS, DoubleToHalf(A64Arith(D, A, B, FPBinaryOp::Sub), true), FPConstant(0x7FFF, OpSize::i16Bit));
  case HalfOp::RecipStep: return DoubleToHalf(FPStepFusedLanes(D, A, B, false), true);
  case HalfOp::RSqrtStep: return DoubleToHalf(FPStepFusedLanes(D, A, B, true), true);
  }
  return A;
}

// ---------------------------------------------------------------------------
// Three same, by element, fused multiply-accumulate
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDHalfThreeSame(uint32_t Word, HalfOp Op, bool Scalar) {
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));
  StoreHalfLanes(Word, Scalar, HalfLanes(HalfLaneCount(Word, Scalar), {A, B, nullptr}, true, [&](const std::array<Ref, 3>& D) {
    return HalfBinaryLane(Op, D[0], D[1]);
  }));
  return true;
}

// The by-element operand of the half-precision forms: Rm is V0-V15 and the
// index is H:L:M. Every lane holds the element.
Ref IRBuilder::HalfElementOperand(uint32_t Word) {
  const uint32_t Index = (Bit(Word, 11) << 2) | (Bit(Word, 21) << 1) | Bit(Word, 20);
  return _VDupElement(OpSize::i128Bit, OpSize::i16Bit, LoadV(Bits(Word, 19, 16)), Index);
}

bool IRBuilder::SIMDHalfMulElement(uint32_t Word, HalfOp Op, bool Scalar) {
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = HalfElementOperand(Word);
  StoreHalfLanes(Word, Scalar, HalfLanes(HalfLaneCount(Word, Scalar), {A, B, nullptr}, true, [&](const std::array<Ref, 3>& D) {
    return HalfBinaryLane(Op, D[0], D[1]);
  }));
  return true;
}

// FMLA/FMLS: FPMulAdd(d, n, m) with n negated for FMLS (a NaN's sign too).
bool IRBuilder::SIMDHalfMulAccumulate(uint32_t Word, bool Subtract, bool ByElement, bool Scalar) {
  Ref Acc = LoadV(Bits(Word, 4, 0));
  Ref N = LoadV(Bits(Word, 9, 5));
  Ref M = ByElement ? HalfElementOperand(Word) : LoadV(Bits(Word, 20, 16));
  StoreHalfLanes(Word, Scalar, HalfLanes(HalfLaneCount(Word, Scalar), {Acc, N, M}, true, [&](const std::array<Ref, 3>& D) {
    Ref NN = Subtract ? _VFNeg(OpSize::i128Bit, OpSize::i64Bit, D[1]).Node : D[1];
    return DoubleToHalf(FPMulAddLanes(OpSize::i64Bit, D[0], NN, D[2]), true);
  }));
  return true;
}

bool IRBuilder::FADD_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::Add, false); }
bool IRBuilder::FSUB_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::Sub, false); }
bool IRBuilder::FMUL_vec_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::Mul, false); }
bool IRBuilder::FDIV_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::Div, false); }
bool IRBuilder::FMIN_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::Min, false); }
bool IRBuilder::FMAX_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::Max, false); }
bool IRBuilder::FMINNM_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::MinNum, false); }
bool IRBuilder::FMAXNM_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::MaxNum, false); }
bool IRBuilder::FMULX_vec_3(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::MulX, false); }
bool IRBuilder::FMULX_vec_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::MulX, true); }
bool IRBuilder::FABD_3(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::Abd, false); }
bool IRBuilder::FABD_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::Abd, true); }
bool IRBuilder::FRECPS_3(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::RecipStep, false); }
bool IRBuilder::FRECPS_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::RecipStep, true); }
bool IRBuilder::FRSQRTS_3(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::RSqrtStep, false); }
bool IRBuilder::FRSQRTS_1(uint32_t Word) { return SIMDHalfThreeSame(Word, HalfOp::RSqrtStep, true); }
bool IRBuilder::FMLA_vec_1(uint32_t Word) { return SIMDHalfMulAccumulate(Word, false, false, false); }
bool IRBuilder::FMLS_vec_1(uint32_t Word) { return SIMDHalfMulAccumulate(Word, true, false, false); }
bool IRBuilder::FMLA_elt_3(uint32_t Word) { return SIMDHalfMulAccumulate(Word, false, true, false); }
bool IRBuilder::FMLS_elt_3(uint32_t Word) { return SIMDHalfMulAccumulate(Word, true, true, false); }
bool IRBuilder::FMLA_elt_1(uint32_t Word) { return SIMDHalfMulAccumulate(Word, false, true, true); }
bool IRBuilder::FMLS_elt_1(uint32_t Word) { return SIMDHalfMulAccumulate(Word, true, true, true); }
bool IRBuilder::FMUL_elt_3(uint32_t Word) { return SIMDHalfMulElement(Word, HalfOp::Mul, false); }
bool IRBuilder::FMUL_elt_1(uint32_t Word) { return SIMDHalfMulElement(Word, HalfOp::Mul, true); }
bool IRBuilder::FMULX_elt_3(uint32_t Word) { return SIMDHalfMulElement(Word, HalfOp::MulX, false); }
bool IRBuilder::FMULX_elt_1(uint32_t Word) { return SIMDHalfMulElement(Word, HalfOp::MulX, true); }

// ---------------------------------------------------------------------------
// Pairwise and across lanes
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDHalfPairwise(uint32_t Word, HalfOp Op, bool Scalar) {
  const auto RS = OpSize::i128Bit;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref Even {};
  Ref Odd {};
  if (Scalar) {
    // FADDP/FMAXP/... Hd, Vn.2H: the two lowest elements.
    Even = A;
    Odd = _VDupElement(RS, OpSize::i16Bit, A, 1);
  } else {
    // Pairs of the concatenation B:A, as in SIMDFloatPairwise.
    Ref B = LoadV(Bits(Word, 20, 16));
    if (!Bit(Word, 30)) {
      A = _VInsElement(RS, OpSize::i64Bit, 1, 0, A, B);
      B = A;
    }
    Even = _VUnZip(RS, OpSize::i16Bit, A, B);
    Odd = _VUnZip2(RS, OpSize::i16Bit, A, B);
  }
  StoreHalfLanes(Word, Scalar, HalfLanes(HalfLaneCount(Word, Scalar), {Even, Odd, nullptr}, true, [&](const std::array<Ref, 3>& D) {
    return HalfBinaryLane(Op, D[0], D[1]);
  }));
  return true;
}

// FMAXV/FMINV/FMAXNMV/FMINNMV Hd, Vn.4H/8H. Reduce() halves the vector and
// combines the halves, lower first: op(op(e0, e1), op(e2, e3)) for 4H and
// op of the two 4H reductions for 8H. Min/max never round, so the whole
// reduction runs in double and is rounded (exactly) once.
bool IRBuilder::SIMDHalfAcrossLanes(uint32_t Word, HalfOp Op) {
  const auto RS = OpSize::i128Bit;
  const uint32_t Lanes = Bit(Word, 30) ? 8 : 4;
  const bool IsMax = Op == HalfOp::Max || Op == HalfOp::MaxNum;
  const bool IsNumber = Op == HalfOp::MinNum || Op == HalfOp::MaxNum;
  Ref V = LoadV(Bits(Word, 9, 5));
  std::array<Ref, 8> D {};
  for (uint32_t i = 0; i < Lanes; ++i) {
    D[i] = HalfToDouble(i == 0 ? V : _VDupElement(RS, OpSize::i16Bit, V, i).Node, true, true);
  }
  for (uint32_t Width = Lanes; Width > 1; Width /= 2) {
    for (uint32_t i = 0; i < Width / 2; ++i) {
      D[i] = FPMinMax(OpSize::i64Bit, D[2 * i], D[2 * i + 1], IsMax, IsNumber);
    }
  }
  StoreVSized(Bits(Word, 4, 0), OpSize::i16Bit, DoubleToHalf(D[0], true));
  return true;
}

bool IRBuilder::FADDP_vec_1(uint32_t Word) { return SIMDHalfPairwise(Word, HalfOp::Add, false); }
bool IRBuilder::FMAXP_vec_1(uint32_t Word) { return SIMDHalfPairwise(Word, HalfOp::Max, false); }
bool IRBuilder::FMINP_vec_1(uint32_t Word) { return SIMDHalfPairwise(Word, HalfOp::Min, false); }
bool IRBuilder::FMAXNMP_vec_1(uint32_t Word) { return SIMDHalfPairwise(Word, HalfOp::MaxNum, false); }
bool IRBuilder::FMINNMP_vec_1(uint32_t Word) { return SIMDHalfPairwise(Word, HalfOp::MinNum, false); }
bool IRBuilder::FADDP_pair_1(uint32_t Word) { return SIMDHalfPairwise(Word, HalfOp::Add, true); }
bool IRBuilder::FMAXP_pair_1(uint32_t Word) { return SIMDHalfPairwise(Word, HalfOp::Max, true); }
bool IRBuilder::FMINP_pair_1(uint32_t Word) { return SIMDHalfPairwise(Word, HalfOp::Min, true); }
bool IRBuilder::FMAXNMP_pair_1(uint32_t Word) { return SIMDHalfPairwise(Word, HalfOp::MaxNum, true); }
bool IRBuilder::FMINNMP_pair_1(uint32_t Word) { return SIMDHalfPairwise(Word, HalfOp::MinNum, true); }
bool IRBuilder::FMAXV_1(uint32_t Word) { return SIMDHalfAcrossLanes(Word, HalfOp::Max); }
bool IRBuilder::FMINV_1(uint32_t Word) { return SIMDHalfAcrossLanes(Word, HalfOp::Min); }
bool IRBuilder::FMAXNMV_1(uint32_t Word) { return SIMDHalfAcrossLanes(Word, HalfOp::MaxNum); }
bool IRBuilder::FMINNMV_1(uint32_t Word) { return SIMDHalfAcrossLanes(Word, HalfOp::MinNum); }

// ---------------------------------------------------------------------------
// Compares
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDHalfCompare(uint32_t Word, FloatCompareKind Kind, bool Scalar) {
  // All compares are false for a NaN operand. The double compare's 64-bit
  // mask covers the lane's 16 bits.
  const auto RS = OpSize::i128Bit;
  const auto ES = OpSize::i64Bit;
  const bool TwoOperands = Kind == FloatCompareKind::Eq || Kind == FloatCompareKind::Ge || Kind == FloatCompareKind::Gt ||
                           Kind == FloatCompareKind::AbsGe || Kind == FloatCompareKind::AbsGt;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = TwoOperands ? LoadV(Bits(Word, 20, 16)) : nullptr;
  StoreHalfLanes(Word, Scalar, HalfLanes(HalfLaneCount(Word, Scalar), {A, B, nullptr}, false, [&](const std::array<Ref, 3>& D) -> Ref {
    Ref Zero = _VectorImm(RS, OpSize::i8Bit, 0);
    switch (Kind) {
    case FloatCompareKind::Eq: return _VFCMPEQ(RS, ES, D[0], D[1]);
    case FloatCompareKind::Ge: return _VFCMPLE(RS, ES, D[1], D[0]);
    case FloatCompareKind::Gt: return _VFCMPLT(RS, ES, D[1], D[0]);
    case FloatCompareKind::AbsGe: return _VFCMPLE(RS, ES, _VFAbs(RS, ES, D[1]), _VFAbs(RS, ES, D[0]));
    case FloatCompareKind::AbsGt: return _VFCMPLT(RS, ES, _VFAbs(RS, ES, D[1]), _VFAbs(RS, ES, D[0]));
    case FloatCompareKind::EqZero: return _VFCMPEQ(RS, ES, D[0], Zero);
    case FloatCompareKind::GeZero: return _VFCMPLE(RS, ES, Zero, D[0]);
    case FloatCompareKind::GtZero: return _VFCMPLT(RS, ES, Zero, D[0]);
    case FloatCompareKind::LeZero: return _VFCMPLE(RS, ES, D[0], Zero);
    case FloatCompareKind::LtZero: return _VFCMPLT(RS, ES, D[0], Zero);
    }
    return Zero;
  }));
  return true;
}

bool IRBuilder::FCMEQ_reg_3(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::Eq, false); }
bool IRBuilder::FCMGE_reg_3(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::Ge, false); }
bool IRBuilder::FCMGT_reg_3(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::Gt, false); }
bool IRBuilder::FACGE_3(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::AbsGe, false); }
bool IRBuilder::FACGT_3(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::AbsGt, false); }
bool IRBuilder::FCMEQ_zero_3(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::EqZero, false); }
bool IRBuilder::FCMGE_zero_3(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::GeZero, false); }
bool IRBuilder::FCMGT_zero_3(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::GtZero, false); }
bool IRBuilder::FCMLE_3(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::LeZero, false); }
bool IRBuilder::FCMLT_3(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::LtZero, false); }
bool IRBuilder::FCMEQ_reg_1(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::Eq, true); }
bool IRBuilder::FCMGE_reg_1(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::Ge, true); }
bool IRBuilder::FCMGT_reg_1(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::Gt, true); }
bool IRBuilder::FACGE_1(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::AbsGe, true); }
bool IRBuilder::FACGT_1(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::AbsGt, true); }
bool IRBuilder::FCMEQ_zero_1(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::EqZero, true); }
bool IRBuilder::FCMGE_zero_1(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::GeZero, true); }
bool IRBuilder::FCMGT_zero_1(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::GtZero, true); }
bool IRBuilder::FCMLE_1(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::LeZero, true); }
bool IRBuilder::FCMLT_1(uint32_t Word) { return SIMDHalfCompare(Word, FloatCompareKind::LtZero, true); }

// ---------------------------------------------------------------------------
// Rounding, square root
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDHalfRound(uint32_t Word, FPRounding Mode) {
  Ref V = LoadV(Bits(Word, 9, 5));
  StoreHalfLanes(Word, false, HalfLanes(HalfLaneCount(Word, false), {V, nullptr, nullptr}, false, [&](const std::array<Ref, 3>& D) {
    return DoubleToHalf(FPRoundToIntegral(D[0], OpSize::i64Bit, Mode), true);
  }));
  return true;
}

bool IRBuilder::FRINTN_1(uint32_t Word) { return SIMDHalfRound(Word, FPRounding::TiesEven); }
bool IRBuilder::FRINTP_1(uint32_t Word) { return SIMDHalfRound(Word, FPRounding::PosInf); }
bool IRBuilder::FRINTM_1(uint32_t Word) { return SIMDHalfRound(Word, FPRounding::NegInf); }
bool IRBuilder::FRINTZ_1(uint32_t Word) { return SIMDHalfRound(Word, FPRounding::Zero); }
bool IRBuilder::FRINTA_1(uint32_t Word) { return SIMDHalfRound(Word, FPRounding::TiesAway); }
// POWERARM-M1-TODO(fpu): FRINTX sets FPSR.IXC when inexact; cumulative FPSR exception flags are not emulated.
bool IRBuilder::FRINTX_1(uint32_t Word) { return SIMDHalfRound(Word, FPRounding::Current); }
bool IRBuilder::FRINTI_1(uint32_t Word) { return SIMDHalfRound(Word, FPRounding::Current); }

bool IRBuilder::FSQRT_1(uint32_t Word) {
  Ref V = LoadV(Bits(Word, 9, 5));
  StoreHalfLanes(Word, false, HalfLanes(HalfLaneCount(Word, false), {V, nullptr, nullptr}, false, [&](const std::array<Ref, 3>& D) {
    return DoubleToHalf(_VFSqrt(OpSize::i128Bit, OpSize::i64Bit, D[0]), true);
  }));
  return true;
}

// ---------------------------------------------------------------------------
// Conversions to and from 16-bit integers and fixed point
// ---------------------------------------------------------------------------

// Half precision -> 16-bit integer lanes, saturating: the conversion to a
// 32-bit integer is exact in range (|half| <= 65504) and then clamped. A
// fixed-point conversion first scales by 2^FBits, exactly, in double.
bool IRBuilder::SIMDHalfToInt(uint32_t Word, uint8_t Rounding, bool Signed, bool Scalar, uint32_t FBits) {
  const auto RS = OpSize::i128Bit;
  const uint32_t Lanes = HalfLaneCount(Word, Scalar);
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Result = _VectorImm(RS, OpSize::i8Bit, 0);
  for (uint32_t i = 0; i < Lanes; ++i) {
    Ref D = HalfToDouble(i == 0 ? V : _VDupElement(RS, OpSize::i16Bit, V, i).Node, false, true);
    if (FBits) {
      D = _VFMul(RS, OpSize::i64Bit, D, FPConstant(static_cast<uint64_t>(1023 + FBits) << 52, OpSize::i64Bit));
    }
    Ref G = _A64FloatToGPR(OpSize::i32Bit, OpSize::i64Bit, D, Rounding, Signed);
    if (Signed) {
      G = _Select(OpSize::i64Bit, OpSize::i32Bit, CondClass::SGT, G, Constant(0x7FFF), Constant(0x7FFF), G);
      G = _Select(OpSize::i64Bit, OpSize::i32Bit, CondClass::SLT, G, Constant(-0x8000), Constant(-0x8000), G);
    } else {
      G = _Select(OpSize::i64Bit, OpSize::i32Bit, CondClass::UGT, G, Constant(0xFFFF), Constant(0xFFFF), G);
    }
    Result = _VInsGPR(RS, OpSize::i16Bit, i, Result, G);
  }
  StoreHalfLanes(Word, Scalar, Result);
  return true;
}

// 16-bit integer lanes -> half precision, rounded once (FZ16 on the result).
// A fixed-point conversion scales by 2^-FBits, exactly, in double first.
bool IRBuilder::SIMDIntToHalf(uint32_t Word, bool Signed, bool Scalar, uint32_t FBits) {
  const auto RS = OpSize::i128Bit;
  const uint32_t Lanes = HalfLaneCount(Word, Scalar);
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Result = _VectorImm(RS, OpSize::i8Bit, 0);
  for (uint32_t i = 0; i < Lanes; ++i) {
    Ref G = _VExtractToGPR(RS, OpSize::i16Bit, V, i);
    G = Signed ? _Sbfe(OpSize::i64Bit, 16, 0, G).Node : _Bfe(OpSize::i64Bit, 16, 0, G).Node;
    Ref D = _A64FloatFromGPR(OpSize::i64Bit, OpSize::i64Bit, G, true);
    if (FBits) {
      D = _VFMul(RS, OpSize::i64Bit, D, FPConstant(static_cast<uint64_t>(1023 - FBits) << 52, OpSize::i64Bit));
    }
    Result = _VInsElement(RS, OpSize::i16Bit, i, 0, Result, DoubleToHalf(D, true));
  }
  StoreHalfLanes(Word, Scalar, Result);
  return true;
}

// SCVTF/UCVTF/FCVTZS/FCVTZU (vector and scalar, fixed point) with immh 001x:
// fbits = 32 - immh:immb.
bool IRBuilder::SIMDHalfFixedConvert(uint32_t Word, bool ToFloat, bool Signed, bool Scalar) {
  const uint32_t FBits = 32 - Bits(Word, 22, 16);
  return ToFloat ? SIMDIntToHalf(Word, Signed, Scalar, FBits) : SIMDHalfToInt(Word, ROUND_ZERO, Signed, Scalar, FBits);
}

bool IRBuilder::FCVTNS_3(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_TIEEVEN, true, false, 0); }
bool IRBuilder::FCVTNU_3(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_TIEEVEN, false, false, 0); }
bool IRBuilder::FCVTPS_3(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_POSINF, true, false, 0); }
bool IRBuilder::FCVTPU_3(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_POSINF, false, false, 0); }
bool IRBuilder::FCVTMS_3(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_NEGINF, true, false, 0); }
bool IRBuilder::FCVTMU_3(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_NEGINF, false, false, 0); }
bool IRBuilder::FCVTZS_int_3(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_ZERO, true, false, 0); }
bool IRBuilder::FCVTZU_int_3(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_ZERO, false, false, 0); }
bool IRBuilder::FCVTAS_3(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_TIEAWAY, true, false, 0); }
bool IRBuilder::FCVTAU_3(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_TIEAWAY, false, false, 0); }
bool IRBuilder::FCVTNS_1(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_TIEEVEN, true, true, 0); }
bool IRBuilder::FCVTNU_1(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_TIEEVEN, false, true, 0); }
bool IRBuilder::FCVTPS_1(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_POSINF, true, true, 0); }
bool IRBuilder::FCVTPU_1(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_POSINF, false, true, 0); }
bool IRBuilder::FCVTMS_1(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_NEGINF, true, true, 0); }
bool IRBuilder::FCVTMU_1(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_NEGINF, false, true, 0); }
bool IRBuilder::FCVTZS_int_1(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_ZERO, true, true, 0); }
bool IRBuilder::FCVTZU_int_1(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_ZERO, false, true, 0); }
bool IRBuilder::FCVTAS_1(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_TIEAWAY, true, true, 0); }
bool IRBuilder::FCVTAU_1(uint32_t Word) { return SIMDHalfToInt(Word, ROUND_TIEAWAY, false, true, 0); }
bool IRBuilder::SCVTF_int_3(uint32_t Word) { return SIMDIntToHalf(Word, true, false, 0); }
bool IRBuilder::UCVTF_int_3(uint32_t Word) { return SIMDIntToHalf(Word, false, false, 0); }
bool IRBuilder::SCVTF_int_1(uint32_t Word) { return SIMDIntToHalf(Word, true, true, 0); }
bool IRBuilder::UCVTF_int_1(uint32_t Word) { return SIMDIntToHalf(Word, false, true, 0); }

// ---------------------------------------------------------------------------
// FMOV (vector, immediate), half precision
// ---------------------------------------------------------------------------

bool IRBuilder::FMOV_3(uint32_t Word) {
  // VFPExpandImm for 16 bits: imm8 = a:b:c:d:e:f:g:h.
  const uint64_t Imm8 = (Bits(Word, 18, 16) << 5) | Bits(Word, 9, 5);
  const uint64_t B6 = (Imm8 >> 6) & 1;
  const uint64_t Half = (((Imm8 >> 7) & 1) << 15) | ((B6 ^ 1) << 14) | ((B6 ? 0x3ULL : 0) << 12) | ((Imm8 & 0x3F) << 6);
  StoreVQ(Bits(Word, 4, 0), Bit(Word, 30), FPConstant(Half, OpSize::i16Bit));
  return true;
}

} // namespace FEXCore::A64
