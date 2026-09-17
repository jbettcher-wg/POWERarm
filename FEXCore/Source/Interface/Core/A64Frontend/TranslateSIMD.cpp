// SPDX-License-Identifier: MIT
//
// A64 Advanced SIMD integer translators: copies between general-purpose and
// vector registers, modified immediates, bitwise and compare operations,
// across-lane reductions, widening and narrowing, shifts by immediate, and
// the permutes.
//
// Every result is computed over the 128-bit register. A 64-bit vector
// (Q == 0) result has its upper half cleared on store (StoreVQ). Operations
// whose output lanes depend on input lanes outside the low 64 bits (the
// permutes, pairwise and across-lane operations) build 128-bit operands from
// the low halves first, because the upper half of a Q == 0 input is not part
// of the operand.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

#include <array>
#include <bit>

namespace FEXCore::A64 {
using namespace FEXCore::IR;

namespace {
  OpSize ElementSizeFor(uint32_t Size) {
    return IR::SizeToOpSize(1U << Size);
  }

  // Lowest set bit of a copy/INS imm5 selects the element size.
  // Returns -1 for imm5 & 0xF == 0 (unallocated).
  int CopyElementSize(uint32_t Imm5) {
    if ((Imm5 & 0xF) == 0) {
      return -1;
    }
    return std::countr_zero(Imm5);
  }

  // A64 AdvSIMDExpandImm, returning the 64-bit lane pattern.
  uint64_t AdvSIMDExpandImm(bool Op, uint32_t CMode, uint64_t Imm8) {
    auto Replicate32 = [](uint64_t V) {
      return (V << 32) | V;
    };
    auto Replicate16 = [](uint64_t V) {
      return V * 0x0001000100010001ULL;
    };
    switch (CMode >> 1) {
    case 0b000: return Replicate32(Imm8);
    case 0b001: return Replicate32(Imm8 << 8);
    case 0b010: return Replicate32(Imm8 << 16);
    case 0b011: return Replicate32(Imm8 << 24);
    case 0b100: return Replicate16(Imm8);
    case 0b101: return Replicate16(Imm8 << 8);
    case 0b110:
      if ((CMode & 1) == 0) {
        return Replicate32((Imm8 << 8) | 0xFF);
      }
      return Replicate32((Imm8 << 16) | 0xFFFF);
    default:
      if ((CMode & 1) == 0) {
        if (!Op) {
          return Imm8 * 0x0101010101010101ULL;
        }
        uint64_t Mask {};
        for (unsigned i = 0; i < 8; ++i) {
          if ((Imm8 >> i) & 1) {
            Mask |= 0xFFULL << (i * 8);
          }
        }
        return Mask;
      }
      // cmode 1111: FMOV. Single precision replicated, or double precision.
      {
        const uint64_t Sign = (Imm8 >> 7) & 1;
        const uint64_t B6 = (Imm8 >> 6) & 1;
        if (!Op) {
          const uint64_t Bits32 = (Sign << 31) | ((B6 ^ 1) << 30) | ((B6 ? 0x1FULL : 0) << 25) | ((Imm8 & 0x3F) << 19);
          return Replicate32(Bits32);
        }
        return (Sign << 63) | ((B6 ^ 1) << 62) | ((B6 ? 0xFFULL : 0) << 54) | ((Imm8 & 0x3F) << 48);
      }
    }
  }
} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Builds a 128-bit value whose two halves are the low halves of Lower and
// Upper: [Upper.low64 : Lower.low64].
static IREmitter::IRPair<IROp_VInsElement> LowHalves(IRBuilder* B, Ref Lower, Ref Upper) {
  return B->_VInsElement(OpSize::i128Bit, OpSize::i64Bit, 1, 0, Lower, Upper);
}

// ---------------------------------------------------------------------------
// Copy
// ---------------------------------------------------------------------------

bool IRBuilder::DUP_gen(uint32_t Word) {
  const bool Q = Bit(Word, 30);
  const int Size = CopyElementSize(Bits(Word, 20, 16));
  if (Size < 0 || (Size == 3 && !Q)) {
    return false;
  }
  Ref Result = _VDupFromGPR(OpSize::i128Bit, ElementSizeFor(Size), LoadX(Bits(Word, 9, 5)));
  StoreVQ(Bits(Word, 4, 0), Q, Result);
  return true;
}

bool IRBuilder::DUP_elt_2(uint32_t Word) {
  const bool Q = Bit(Word, 30);
  const uint32_t Imm5 = Bits(Word, 20, 16);
  const int Size = CopyElementSize(Imm5);
  if (Size < 0 || (Size == 3 && !Q)) {
    return false;
  }
  const uint8_t Index = Imm5 >> (Size + 1);
  Ref Result = _VDupElement(OpSize::i128Bit, ElementSizeFor(Size), LoadV(Bits(Word, 9, 5)), Index);
  StoreVQ(Bits(Word, 4, 0), Q, Result);
  return true;
}

bool IRBuilder::DUP_elt_1(uint32_t Word) {
  // Scalar DUP (MOV Dd, Vn.d[i]): only 64-bit elements are allocated.
  const uint32_t Imm5 = Bits(Word, 20, 16);
  if (CopyElementSize(Imm5) != 3) {
    return false;
  }
  const uint8_t Index = Imm5 >> 4;
  Ref Result = _VDupElement(OpSize::i128Bit, OpSize::i64Bit, LoadV(Bits(Word, 9, 5)), Index);
  StoreVSized(Bits(Word, 4, 0), OpSize::i64Bit, Result);
  return true;
}

bool IRBuilder::UMOV(uint32_t Word) {
  const bool Q = Bit(Word, 30);
  const uint32_t Imm5 = Bits(Word, 20, 16);
  const int Size = CopyElementSize(Imm5);
  if (Size < 0 || (Q != (Size == 3))) {
    return false;
  }
  const uint8_t Index = Imm5 >> (Size + 1);
  Ref Value = _VExtractToGPR(OpSize::i128Bit, ElementSizeFor(Size), LoadV(Bits(Word, 9, 5)), Index);
  StoreReg(Bits(Word, 4, 0), Q, Value);
  return true;
}

bool IRBuilder::SMOV(uint32_t Word) {
  const bool Q = Bit(Word, 30);
  const uint32_t Imm5 = Bits(Word, 20, 16);
  const int Size = CopyElementSize(Imm5);
  if (Size < 0 || Size == 3 || (Size == 2 && !Q)) {
    return false;
  }
  const uint8_t Index = Imm5 >> (Size + 1);
  Ref Value = _VExtractToGPR(OpSize::i128Bit, ElementSizeFor(Size), LoadV(Bits(Word, 9, 5)), Index);
  Value = _Sbfe(OpSize::i64Bit, 8U << Size, 0, Value);
  StoreReg(Bits(Word, 4, 0), Q, Value);
  return true;
}

bool IRBuilder::INS_gen(uint32_t Word) {
  const uint32_t Imm5 = Bits(Word, 20, 16);
  const int Size = CopyElementSize(Imm5);
  if (Size < 0) {
    return false;
  }
  const uint32_t Rd = Bits(Word, 4, 0);
  const uint8_t Index = Imm5 >> (Size + 1);
  StoreV(Rd, _VInsGPR(OpSize::i128Bit, ElementSizeFor(Size), Index, LoadV(Rd), LoadX(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::INS_elt(uint32_t Word) {
  const uint32_t Imm5 = Bits(Word, 20, 16);
  const int Size = CopyElementSize(Imm5);
  if (Size < 0) {
    return false;
  }
  const uint32_t Rd = Bits(Word, 4, 0);
  const uint8_t DestIndex = Imm5 >> (Size + 1);
  const uint8_t SrcIndex = Bits(Word, 14, 11) >> Size;
  StoreV(Rd, _VInsElement(OpSize::i128Bit, ElementSizeFor(Size), DestIndex, SrcIndex, LoadV(Rd), LoadV(Bits(Word, 9, 5))));
  return true;
}

// ---------------------------------------------------------------------------
// Modified immediate
// ---------------------------------------------------------------------------

Ref IRBuilder::VectorConstant64(uint64_t Pattern) {
  if (Pattern == 0) {
    return _VectorImm(OpSize::i128Bit, OpSize::i8Bit, 0);
  }
  if (Pattern == ~0ULL) {
    return _VectorImm(OpSize::i128Bit, OpSize::i8Bit, 0xFF);
  }
  return _VDupFromGPR(OpSize::i128Bit, OpSize::i64Bit, Constant(Pattern));
}

bool IRBuilder::MOVI(uint32_t Word) {
  const bool Q = Bit(Word, 30);
  const bool Op = Bit(Word, 29);
  const uint32_t CMode = Bits(Word, 15, 12);
  const uint64_t Imm8 = (Bits(Word, 18, 16) << 5) | Bits(Word, 9, 5);
  const uint32_t Rd = Bits(Word, 4, 0);

  if (CMode == 0b1111 && Op && !Q) {
    return false;
  }

  const uint64_t Imm = AdvSIMDExpandImm(Op, CMode, Imm8);
  const bool IsOrrBic = CMode < 0b1100 && (CMode & 1);
  const bool IsInverted = Op && CMode < 0b1110;

  if (!IsOrrBic) {
    StoreVQ(Rd, Q, VectorConstant64(IsInverted ? ~Imm : Imm));
    return true;
  }
  Ref Value = LoadV(Rd);
  Ref Result = IsInverted ? _VAndn(OpSize::i128Bit, OpSize::i128Bit, Value, VectorConstant64(Imm)) :
                            _VOr(OpSize::i128Bit, OpSize::i128Bit, Value, VectorConstant64(Imm));
  StoreVQ(Rd, Q, Result);
  return true;
}

bool IRBuilder::FMOV_vec_imm(uint32_t Word) {
  // FMOV_2 shares MOVI's layout with cmode 1111.
  return MOVI(Word);
}

// ---------------------------------------------------------------------------
// Three same: arithmetic, bitwise, compares
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDThreeSame(uint32_t Word, ThreeSameOp Op, bool Scalar) {
  const bool Q = Scalar || Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  const bool NoLongElements = Op == ThreeSameOp::UMax || Op == ThreeSameOp::UMin || Op == ThreeSameOp::SMax || Op == ThreeSameOp::SMin;
  if ((Scalar && Size != 3) || (!Q && Size == 3) || (NoLongElements && Size == 3)) {
    return false;
  }
  const auto ES = ElementSizeFor(Size);
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));

  Ref Result {};
  switch (Op) {
  case ThreeSameOp::Add: Result = _VAdd(RS, ES, A, B); break;
  case ThreeSameOp::Sub: Result = _VSub(RS, ES, A, B); break;
  case ThreeSameOp::CmEq: Result = _VCMPEQ(RS, ES, A, B); break;
  case ThreeSameOp::CmGt: Result = _VCMPGT(RS, ES, A, B); break;
  case ThreeSameOp::CmGe: Result = _VNot(RS, ES, _VCMPGT(RS, ES, B, A)); break;
  // Unsigned compares through the unsigned maximum: A >= B <=> max(A, B) == A.
  case ThreeSameOp::CmHs: Result = _VCMPEQ(RS, ES, _VUMax(RS, ES, A, B), A); break;
  case ThreeSameOp::CmHi: Result = _VNot(RS, ES, _VCMPEQ(RS, ES, _VUMax(RS, ES, A, B), B)); break;
  case ThreeSameOp::CmTst: Result = _VNot(RS, ES, _VCMPEQZ(RS, ES, _VAnd(RS, RS, A, B))); break;
  case ThreeSameOp::UMax: Result = _VUMax(RS, ES, A, B); break;
  case ThreeSameOp::UMin: Result = _VUMin(RS, ES, A, B); break;
  case ThreeSameOp::SMax: Result = _VSMax(RS, ES, A, B); break;
  case ThreeSameOp::SMin: Result = _VSMin(RS, ES, A, B); break;
  }
  StoreVQ(Rd, Q && !Scalar, Result);
  return true;
}

bool IRBuilder::ADD_vector(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::Add, false); }
bool IRBuilder::SUB_2(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::Sub, false); }
bool IRBuilder::CMEQ_reg_2(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmEq, false); }
bool IRBuilder::CMGT_reg_2(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmGt, false); }
bool IRBuilder::CMGE_reg_2(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmGe, false); }
bool IRBuilder::CMHS_2(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmHs, false); }
bool IRBuilder::CMHI_2(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmHi, false); }
bool IRBuilder::CMTST_2(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmTst, false); }
bool IRBuilder::UMAX(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::UMax, false); }
bool IRBuilder::UMIN(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::UMin, false); }
bool IRBuilder::SMAX(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::SMax, false); }
bool IRBuilder::SMIN(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::SMin, false); }
bool IRBuilder::ADD_1(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::Add, true); }
bool IRBuilder::SUB_1(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::Sub, true); }
bool IRBuilder::CMEQ_reg_1(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmEq, true); }
bool IRBuilder::CMGT_reg_1(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmGt, true); }
bool IRBuilder::CMGE_reg_1(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmGe, true); }
bool IRBuilder::CMHS_1(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmHs, true); }
bool IRBuilder::CMHI_1(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmHi, true); }
bool IRBuilder::CMTST_1(uint32_t Word) { return SIMDThreeSame(Word, ThreeSameOp::CmTst, true); }

bool IRBuilder::SIMDLogical(uint32_t Word) {
  // AND/BIC/ORR/ORN (U=0) and EOR/BSL/BIT/BIF (U=1), selected by size.
  const bool Q = Bit(Word, 30);
  const bool U = Bit(Word, 29);
  const uint32_t Size = Bits(Word, 23, 22);
  const uint32_t Rd = Bits(Word, 4, 0);
  const auto RS = OpSize::i128Bit;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));

  Ref Result {};
  if (!U) {
    switch (Size) {
    case 0: Result = _VAnd(RS, RS, A, B); break;
    case 1: Result = _VAndn(RS, RS, A, B); break;
    case 2: Result = _VOr(RS, RS, A, B); break;
    default: Result = _VOrn(RS, RS, A, B); break;
    }
  } else {
    switch (Size) {
    case 0: Result = _VXor(RS, RS, A, B); break;
    case 1: Result = _VBSL(RS, LoadV(Rd), A, B); break; // BSL: Rd selects Rn over Rm.
    case 2: Result = _VBSL(RS, B, A, LoadV(Rd)); break; // BIT: Rm selects Rn over Rd.
    default: Result = _VBSL(RS, B, LoadV(Rd), A); break; // BIF: Rm selects Rd over Rn.
    }
  }
  StoreVQ(Rd, Q, Result);
  return true;
}

// ---------------------------------------------------------------------------
// Pairwise and across lanes
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDPairwise(uint32_t Word, PairwiseOp Op) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3 && (!Q || Op != PairwiseOp::Add)) {
    return false;
  }
  const auto ES = ElementSizeFor(Size);
  const auto RS = OpSize::i128Bit;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));
  // Pairs are (element 2k, element 2k+1) of the concatenation B:A; for a
  // 64-bit vector that concatenation is B.low64:A.low64.
  if (!Q) {
    A = LowHalves(this, A, B);
    B = A;
  }
  Ref Even = _VUnZip(RS, ES, A, B);
  Ref Odd = _VUnZip2(RS, ES, A, B);
  Ref Result {};
  switch (Op) {
  case PairwiseOp::Add: Result = _VAdd(RS, ES, Even, Odd); break;
  case PairwiseOp::UMax: Result = _VUMax(RS, ES, Even, Odd); break;
  case PairwiseOp::UMin: Result = _VUMin(RS, ES, Even, Odd); break;
  case PairwiseOp::SMax: Result = _VSMax(RS, ES, Even, Odd); break;
  case PairwiseOp::SMin: Result = _VSMin(RS, ES, Even, Odd); break;
  }
  StoreVQ(Bits(Word, 4, 0), Q, Result);
  return true;
}

bool IRBuilder::ADDP_vec(uint32_t Word) { return SIMDPairwise(Word, PairwiseOp::Add); }
bool IRBuilder::UMAXP(uint32_t Word) { return SIMDPairwise(Word, PairwiseOp::UMax); }
bool IRBuilder::UMINP(uint32_t Word) { return SIMDPairwise(Word, PairwiseOp::UMin); }
bool IRBuilder::SMAXP(uint32_t Word) { return SIMDPairwise(Word, PairwiseOp::SMax); }
bool IRBuilder::SMINP(uint32_t Word) { return SIMDPairwise(Word, PairwiseOp::SMin); }

bool IRBuilder::ADDV(uint32_t Word) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3 || (Size == 2 && !Q)) {
    return false;
  }
  const auto ES = ElementSizeFor(Size);
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  if (!Q) {
    V = _VMov(OpSize::i64Bit, V);
  }
  Ref Result {};
  if (Size == 2) {
    // VAddV's 32-bit lowering saturates; add the rotated vector lane-wise
    // instead. Element 0 of A + rot(A, 2) is a0+a2 and element 1 is a1+a3.
    Ref Sum = _VAdd(RS, ES, V, _VExtr(RS, OpSize::i8Bit, V, V, 8));
    Result = _VAdd(RS, ES, Sum, _VExtr(RS, OpSize::i8Bit, Sum, Sum, 4));
  } else {
    Result = _VAddV(RS, ES, V);
  }
  StoreVSized(Bits(Word, 4, 0), ES, Result);
  return true;
}

bool IRBuilder::UMAXV(uint32_t Word) { return SIMDAcrossLanesMinMax(Word, true); }
bool IRBuilder::UMINV(uint32_t Word) { return SIMDAcrossLanesMinMax(Word, false); }

bool IRBuilder::SIMDAcrossLanesMinMax(uint32_t Word, bool IsMax) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3 || (Size == 2 && !Q)) {
    return false;
  }
  const auto ES = ElementSizeFor(Size);
  Ref V = LoadV(Bits(Word, 9, 5));
  if (!Q) {
    // Both halves hold the low half, which leaves the result unchanged.
    V = LowHalves(this, V, V);
  }
  Ref Result = IsMax ? _VUMaxV(OpSize::i128Bit, ES, V).Node : _VUMinV(OpSize::i128Bit, ES, V).Node;
  StoreVSized(Bits(Word, 4, 0), ES, Result);
  return true;
}

// ---------------------------------------------------------------------------
// Two-register misc
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDCompareZero(uint32_t Word, CompareZeroOp Op, bool Scalar) {
  const bool Q = Scalar || Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if ((Scalar && Size != 3) || (!Q && Size == 3)) {
    return false;
  }
  const auto ES = ElementSizeFor(Size);
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Result {};
  switch (Op) {
  case CompareZeroOp::Eq: Result = _VCMPEQZ(RS, ES, V); break;
  case CompareZeroOp::Gt: Result = _VCMPGTZ(RS, ES, V); break;
  case CompareZeroOp::Lt: Result = _VCMPLTZ(RS, ES, V); break;
  case CompareZeroOp::Ge: Result = _VNot(RS, ES, _VCMPLTZ(RS, ES, V)); break;
  case CompareZeroOp::Le: Result = _VNot(RS, ES, _VCMPGTZ(RS, ES, V)); break;
  }
  StoreVQ(Bits(Word, 4, 0), Q && !Scalar, Result);
  return true;
}

bool IRBuilder::CMEQ_zero_2(uint32_t Word) { return SIMDCompareZero(Word, CompareZeroOp::Eq, false); }
bool IRBuilder::CMGT_zero_2(uint32_t Word) { return SIMDCompareZero(Word, CompareZeroOp::Gt, false); }
bool IRBuilder::CMGE_zero_2(uint32_t Word) { return SIMDCompareZero(Word, CompareZeroOp::Ge, false); }
bool IRBuilder::CMLE_2(uint32_t Word) { return SIMDCompareZero(Word, CompareZeroOp::Le, false); }
bool IRBuilder::CMLT_2(uint32_t Word) { return SIMDCompareZero(Word, CompareZeroOp::Lt, false); }
bool IRBuilder::CMEQ_zero_1(uint32_t Word) { return SIMDCompareZero(Word, CompareZeroOp::Eq, true); }
bool IRBuilder::CMGT_zero_1(uint32_t Word) { return SIMDCompareZero(Word, CompareZeroOp::Gt, true); }
bool IRBuilder::CMGE_zero_1(uint32_t Word) { return SIMDCompareZero(Word, CompareZeroOp::Ge, true); }
bool IRBuilder::CMLE_1(uint32_t Word) { return SIMDCompareZero(Word, CompareZeroOp::Le, true); }
bool IRBuilder::CMLT_1(uint32_t Word) { return SIMDCompareZero(Word, CompareZeroOp::Lt, true); }

bool IRBuilder::CNT(uint32_t Word) {
  if (Bits(Word, 23, 22) != 0) {
    return false;
  }
  StoreVQ(Bits(Word, 4, 0), Bit(Word, 30), _VPopcount(OpSize::i128Bit, OpSize::i8Bit, LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::NOT(uint32_t Word) {
  StoreVQ(Bits(Word, 4, 0), Bit(Word, 30), _VNot(OpSize::i128Bit, OpSize::i8Bit, LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::NEG_2(uint32_t Word) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (!Q && Size == 3) {
    return false;
  }
  StoreVQ(Bits(Word, 4, 0), Q, _VNeg(OpSize::i128Bit, ElementSizeFor(Size), LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::ABS_2(uint32_t Word) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (!Q && Size == 3) {
    return false;
  }
  StoreVQ(Bits(Word, 4, 0), Q, _VAbs(OpSize::i128Bit, ElementSizeFor(Size), LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::REV64_asimd(uint32_t Word) {
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3) {
    return false;
  }
  StoreVQ(Bits(Word, 4, 0), Bit(Word, 30), _VRev64(OpSize::i128Bit, ElementSizeFor(Size), LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::REV32_asimd(uint32_t Word) {
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size > 1) {
    return false;
  }
  StoreVQ(Bits(Word, 4, 0), Bit(Word, 30), _VRev32(OpSize::i128Bit, ElementSizeFor(Size), LoadV(Bits(Word, 9, 5))));
  return true;
}

// ---------------------------------------------------------------------------
// Widening and narrowing
// ---------------------------------------------------------------------------

// Rd = narrow(Wide) into the low half (Q == 0), or into the upper half with
// the low half of Rd kept (the "2" forms, Q == 1).
void IRBuilder::StoreNarrow(uint32_t Rd, bool Upper, Ref Narrow) {
  if (!Upper) {
    StoreVSized(Rd, OpSize::i64Bit, Narrow);
    return;
  }
  StoreV(Rd, _VInsElement(OpSize::i128Bit, OpSize::i64Bit, 1, 0, LoadV(Rd), Narrow));
}

bool IRBuilder::XTN(uint32_t Word) {
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3) {
    return false;
  }
  Ref Narrow = _VUShrNI(OpSize::i128Bit, ElementSizeFor(Size + 1), LoadV(Bits(Word, 9, 5)), 0);
  StoreNarrow(Bits(Word, 4, 0), Bit(Word, 30), Narrow);
  return true;
}

bool IRBuilder::SIMDThreeDifferent(uint32_t Word, ThreeDifferentOp Op) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3) {
    return false;
  }
  const auto ES = ElementSizeFor(Size);
  const auto WideES = ElementSizeFor(Size + 1);
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));

  auto SExt = [&](Ref V) -> Ref {
    return Q ? _VSXTL2(RS, ES, V).Node : _VSXTL(RS, ES, V).Node;
  };
  auto ZExt = [&](Ref V) -> Ref {
    return Q ? _VUXTL2(RS, ES, V).Node : _VUXTL(RS, ES, V).Node;
  };

  switch (Op) {
  case ThreeDifferentOp::SAddL: StoreV(Rd, _VAdd(RS, WideES, SExt(A), SExt(B))); break;
  case ThreeDifferentOp::UAddL: StoreV(Rd, _VAdd(RS, WideES, ZExt(A), ZExt(B))); break;
  case ThreeDifferentOp::SSubL: StoreV(Rd, _VSub(RS, WideES, SExt(A), SExt(B))); break;
  case ThreeDifferentOp::USubL: StoreV(Rd, _VSub(RS, WideES, ZExt(A), ZExt(B))); break;
  case ThreeDifferentOp::SAddW: StoreV(Rd, _VAdd(RS, WideES, A, SExt(B))); break;
  case ThreeDifferentOp::UAddW: StoreV(Rd, _VAdd(RS, WideES, A, ZExt(B))); break;
  case ThreeDifferentOp::SSubW: StoreV(Rd, _VSub(RS, WideES, A, SExt(B))); break;
  case ThreeDifferentOp::USubW: StoreV(Rd, _VSub(RS, WideES, A, ZExt(B))); break;
  case ThreeDifferentOp::UMull:
  case ThreeDifferentOp::SMull:
  case ThreeDifferentOp::UMlal:
  case ThreeDifferentOp::SMlal:
  case ThreeDifferentOp::UMlsl:
  case ThreeDifferentOp::SMlsl: {
    const bool Signed = Op == ThreeDifferentOp::SMull || Op == ThreeDifferentOp::SMlal || Op == ThreeDifferentOp::SMlsl;
    Ref Product {};
    if (Size == 0) {
      // No 8x8 widening multiply op: widen both, then the modular 16-bit multiply.
      Product = _VMul(RS, WideES, Signed ? SExt(A) : ZExt(A), Signed ? SExt(B) : ZExt(B));
    } else if (Signed) {
      Product = Q ? _VSMull2(RS, ES, A, B).Node : _VSMull(RS, ES, A, B).Node;
    } else {
      Product = Q ? _VUMull2(RS, ES, A, B).Node : _VUMull(RS, ES, A, B).Node;
    }
    if (Op == ThreeDifferentOp::UMull || Op == ThreeDifferentOp::SMull) {
      StoreV(Rd, Product);
    } else if (Op == ThreeDifferentOp::UMlal || Op == ThreeDifferentOp::SMlal) {
      StoreV(Rd, _VAdd(RS, WideES, LoadV(Rd), Product));
    } else {
      StoreV(Rd, _VSub(RS, WideES, LoadV(Rd), Product));
    }
    break;
  }
  case ThreeDifferentOp::AddHN:
    StoreNarrow(Rd, Q, _VUShrNI(RS, WideES, _VAdd(RS, WideES, A, B), 8U << Size));
    break;
  case ThreeDifferentOp::SubHN:
    StoreNarrow(Rd, Q, _VUShrNI(RS, WideES, _VSub(RS, WideES, A, B), 8U << Size));
    break;
  }
  return true;
}

bool IRBuilder::SADDL(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::SAddL); }
bool IRBuilder::UADDL(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::UAddL); }
bool IRBuilder::SSUBL(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::SSubL); }
bool IRBuilder::USUBL(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::USubL); }
bool IRBuilder::SADDW(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::SAddW); }
bool IRBuilder::UADDW(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::UAddW); }
bool IRBuilder::ADDHN(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::AddHN); }
bool IRBuilder::SUBHN(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::SubHN); }
bool IRBuilder::SSUBW(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::SSubW); }
bool IRBuilder::USUBW(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::USubW); }
bool IRBuilder::UMULL_vec(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::UMull); }
bool IRBuilder::SMULL_vec(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::SMull); }
bool IRBuilder::UMLAL_vec(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::UMlal); }
bool IRBuilder::SMLAL_vec(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::SMlal); }
bool IRBuilder::UMLSL_vec(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::UMlsl); }
bool IRBuilder::SMLSL_vec(uint32_t Word) { return SIMDThreeDifferent(Word, ThreeDifferentOp::SMlsl); }

// ---------------------------------------------------------------------------
// Shift by immediate
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDShiftImm(uint32_t Word, ShiftImmOp Op, bool Scalar) {
  const bool Q = Scalar || Bit(Word, 30);
  const uint32_t Immh = Bits(Word, 22, 19);
  const uint32_t ImmhImmb = Bits(Word, 22, 16);
  const uint32_t Rd = Bits(Word, 4, 0);
  if (Immh == 0) {
    return false;
  }
  const uint32_t SizeLog2 = 31 - std::countl_zero(Immh);
  const uint32_t ElementBits = 8U << SizeLog2;
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));

  switch (Op) {
  case ShiftImmOp::SShr:
  case ShiftImmOp::UShr:
  case ShiftImmOp::Shl: {
    if ((Scalar && SizeLog2 != 3) || (!Q && SizeLog2 == 3)) {
      return false;
    }
    const auto ES = ElementSizeFor(SizeLog2);
    Ref Result {};
    if (Op == ShiftImmOp::Shl) {
      Result = _VShlI(RS, ES, V, ImmhImmb - ElementBits);
    } else {
      const uint32_t Shift = 2 * ElementBits - ImmhImmb;
      Result = Op == ShiftImmOp::SShr ? _VSShrI(RS, ES, V, Shift).Node : _VUShrI(RS, ES, V, Shift).Node;
    }
    StoreVQ(Rd, Q && !Scalar, Result);
    return true;
  }
  case ShiftImmOp::Shrn: {
    // Element size here is the narrow size; the source is twice as wide.
    if (SizeLog2 == 3) {
      return false;
    }
    const uint32_t Shift = 2 * ElementBits - ImmhImmb;
    StoreNarrow(Rd, Q, _VUShrNI(RS, ElementSizeFor(SizeLog2 + 1), V, Shift));
    return true;
  }
  case ShiftImmOp::SShll:
  case ShiftImmOp::UShll: {
    if (SizeLog2 == 3) {
      return false;
    }
    const auto ES = ElementSizeFor(SizeLog2);
    const uint32_t Shift = ImmhImmb - ElementBits;
    Ref Wide {};
    if (Op == ShiftImmOp::SShll) {
      Wide = Q ? _VSXTL2(RS, ES, V).Node : _VSXTL(RS, ES, V).Node;
    } else {
      Wide = Q ? _VUXTL2(RS, ES, V).Node : _VUXTL(RS, ES, V).Node;
    }
    if (Shift) {
      Wide = _VShlI(RS, ElementSizeFor(SizeLog2 + 1), Wide, Shift);
    }
    StoreV(Rd, Wide);
    return true;
  }
  }
  return false;
}

bool IRBuilder::SSHR_2(uint32_t Word) { return SIMDShiftImm(Word, ShiftImmOp::SShr, false); }
bool IRBuilder::USHR_2(uint32_t Word) { return SIMDShiftImm(Word, ShiftImmOp::UShr, false); }
bool IRBuilder::SHL_2(uint32_t Word) { return SIMDShiftImm(Word, ShiftImmOp::Shl, false); }
bool IRBuilder::SSHR_1(uint32_t Word) { return SIMDShiftImm(Word, ShiftImmOp::SShr, true); }
bool IRBuilder::USHR_1(uint32_t Word) { return SIMDShiftImm(Word, ShiftImmOp::UShr, true); }
bool IRBuilder::SHL_1(uint32_t Word) { return SIMDShiftImm(Word, ShiftImmOp::Shl, true); }
bool IRBuilder::SHRN(uint32_t Word) { return SIMDShiftImm(Word, ShiftImmOp::Shrn, false); }
bool IRBuilder::SSHLL(uint32_t Word) { return SIMDShiftImm(Word, ShiftImmOp::SShll, false); }
bool IRBuilder::USHLL(uint32_t Word) { return SIMDShiftImm(Word, ShiftImmOp::UShll, false); }

// ---------------------------------------------------------------------------
// Extract and permute
// ---------------------------------------------------------------------------

bool IRBuilder::EXT(uint32_t Word) {
  const bool Q = Bit(Word, 30);
  const uint32_t Index = Bits(Word, 14, 11);
  if (!Q && Index > 7) {
    return false;
  }
  Ref Lo = LoadV(Bits(Word, 9, 5));
  Ref Hi = LoadV(Bits(Word, 20, 16));
  // VExtr's VectorUpper supplies the low result bytes: result = (Hi:Lo) >> 8*Index.
  Ref Result = _VExtr(Q ? OpSize::i128Bit : OpSize::i64Bit, OpSize::i8Bit, Hi, Lo, Index);
  StoreVQ(Bits(Word, 4, 0), Q, Result);
  return true;
}

bool IRBuilder::SIMDPermute(uint32_t Word, PermuteOp Op) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (!Q && Size == 3) {
    return false;
  }
  const auto ES = ElementSizeFor(Size);
  const auto RS = OpSize::i128Bit;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));

  if (Q) {
    Ref Result {};
    switch (Op) {
    case PermuteOp::Uzp1: Result = _VUnZip(RS, ES, A, B); break;
    case PermuteOp::Uzp2: Result = _VUnZip2(RS, ES, A, B); break;
    case PermuteOp::Zip1: Result = _VZip(RS, ES, A, B); break;
    case PermuteOp::Zip2: Result = _VZip2(RS, ES, A, B); break;
    case PermuteOp::Trn1: Result = _VTrn(RS, ES, A, B); break;
    case PermuteOp::Trn2: Result = _VTrn2(RS, ES, A, B); break;
    }
    StoreV(Bits(Word, 4, 0), Result);
    return true;
  }

  // 64-bit vectors. Unzips work on C = B.low64:A.low64 as one 128-bit
  // operand, whose even/odd elements in order are exactly the result. Zips
  // and transpositions of the low halves are the 128-bit forms applied to
  // X = A.low64:A.low64 and Y = B.low64:B.low64, whose results carry the
  // first 64 result bits in the low half and the next 64 in the upper half.
  Ref Result {};
  switch (Op) {
  case PermuteOp::Uzp1:
  case PermuteOp::Uzp2: {
    Ref C = LowHalves(this, A, B);
    Result = Op == PermuteOp::Uzp1 ? _VUnZip(RS, ES, C, C).Node : _VUnZip2(RS, ES, C, C).Node;
    break;
  }
  case PermuteOp::Zip1:
  case PermuteOp::Zip2: {
    Ref X = LowHalves(this, A, A);
    Ref Y = LowHalves(this, B, B);
    Result = _VZip2(RS, ES, X, Y);
    if (Op == PermuteOp::Zip2) {
      Result = _VDupElement(RS, OpSize::i64Bit, Result, 1);
    }
    break;
  }
  case PermuteOp::Trn1:
  case PermuteOp::Trn2: {
    Result = Op == PermuteOp::Trn1 ? _VTrn(RS, ES, A, B).Node : _VTrn2(RS, ES, A, B).Node;
    break;
  }
  }
  StoreVSized(Bits(Word, 4, 0), OpSize::i64Bit, Result);
  return true;
}

bool IRBuilder::UZP1(uint32_t Word) { return SIMDPermute(Word, PermuteOp::Uzp1); }
bool IRBuilder::UZP2(uint32_t Word) { return SIMDPermute(Word, PermuteOp::Uzp2); }
bool IRBuilder::ZIP1(uint32_t Word) { return SIMDPermute(Word, PermuteOp::Zip1); }
bool IRBuilder::ZIP2(uint32_t Word) { return SIMDPermute(Word, PermuteOp::Zip2); }
bool IRBuilder::TRN1(uint32_t Word) { return SIMDPermute(Word, PermuteOp::Trn1); }
bool IRBuilder::TRN2(uint32_t Word) { return SIMDPermute(Word, PermuteOp::Trn2); }

// ---------------------------------------------------------------------------
// Shifts by register, shift-and-insert, shift-and-accumulate
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDShiftRegister(uint32_t Word, bool Signed, bool Scalar) {
  // SSHL/USHL: the count is the signed low byte of each lane of Rm; a
  // non-negative count shifts left (0 when >= W), a negative one shifts right
  // by its magnitude (0 when >= W unsigned, sign fill signed).
  const bool Q = Scalar || Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if ((Scalar && Size != 3) || (!Q && Size == 3)) {
    return false;
  }
  const auto ES = ElementSizeFor(Size);
  const auto RS = OpSize::i128Bit;
  const uint8_t Extend = (8U << Size) - 8;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Count = LoadV(Bits(Word, 20, 16));
  if (Extend) {
    Count = _VSShrI(RS, ES, _VShlI(RS, ES, Count, Extend), Extend);
  }
  Ref Left = _VUShl(RS, ES, V, Count, true);
  Ref Negated = _VNeg(RS, ES, Count);
  Ref Result {};
  if (Signed) {
    Result = _VBSL(RS, _VCMPLTZ(RS, ES, Count), _VSShr(RS, ES, V, Negated, true), Left);
  } else {
    Result = _VOr(RS, RS, Left, _VUShr(RS, ES, V, Negated, true));
  }
  StoreVQ(Bits(Word, 4, 0), Q && !Scalar, Result);
  return true;
}

bool IRBuilder::USHL_2(uint32_t Word) { return SIMDShiftRegister(Word, false, false); }
bool IRBuilder::SSHL_2(uint32_t Word) { return SIMDShiftRegister(Word, true, false); }
bool IRBuilder::USHL_1(uint32_t Word) { return SIMDShiftRegister(Word, false, true); }
bool IRBuilder::SSHL_1(uint32_t Word) { return SIMDShiftRegister(Word, true, true); }

bool IRBuilder::SIMDShiftInsertAccumulate(uint32_t Word, ShiftInsertOp Op) {
  const bool Q = Bit(Word, 30);
  const uint32_t Immh = Bits(Word, 22, 19);
  const uint32_t ImmhImmb = Bits(Word, 22, 16);
  if (Immh == 0) {
    return false;
  }
  const uint32_t SizeLog2 = 31 - std::countl_zero(Immh);
  if (!Q && SizeLog2 == 3) {
    return false;
  }
  const uint32_t ElementBits = 8U << SizeLog2;
  const auto ES = ElementSizeFor(SizeLog2);
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref D = LoadV(Rd);
  Ref Ones = VectorConstant64(~0ULL);
  const uint32_t RightShift = 2 * ElementBits - ImmhImmb; // 1..W
  const uint32_t LeftShift = ImmhImmb - ElementBits;       // 0..W-1

  Ref Result {};
  switch (Op) {
  case ShiftInsertOp::Sri:
    // Keep Rd's top RightShift bits, take the rest from Rn >> RightShift.
    // VUShrI gives 0 for a shift of W, which keeps all of Rd.
    Result = _VOr(RS, RS, _VAndn(RS, RS, D, _VUShrI(RS, ES, Ones, RightShift)), _VUShrI(RS, ES, V, RightShift));
    break;
  case ShiftInsertOp::Sli:
    Result = _VOr(RS, RS, _VAndn(RS, RS, D, _VShlI(RS, ES, Ones, LeftShift)), _VShlI(RS, ES, V, LeftShift));
    break;
  case ShiftInsertOp::Usra: Result = _VAdd(RS, ES, D, _VUShrI(RS, ES, V, RightShift)); break;
  case ShiftInsertOp::Ssra: Result = _VAdd(RS, ES, D, _VSShrI(RS, ES, V, RightShift)); break;
  }
  StoreVQ(Rd, Q, Result);
  return true;
}

bool IRBuilder::SRI_2(uint32_t Word) { return SIMDShiftInsertAccumulate(Word, ShiftInsertOp::Sri); }
bool IRBuilder::SLI_2(uint32_t Word) { return SIMDShiftInsertAccumulate(Word, ShiftInsertOp::Sli); }
bool IRBuilder::USRA_2(uint32_t Word) { return SIMDShiftInsertAccumulate(Word, ShiftInsertOp::Usra); }
bool IRBuilder::SSRA_2(uint32_t Word) { return SIMDShiftInsertAccumulate(Word, ShiftInsertOp::Ssra); }

// ---------------------------------------------------------------------------
// Pairwise long adds, multiplies, misc
// ---------------------------------------------------------------------------

bool IRBuilder::SIMDAddLongPairwise(uint32_t Word, bool Signed, bool Accumulate) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3) {
    return false;
  }
  const auto ES = ElementSizeFor(Size);
  const auto WideES = ElementSizeFor(Size + 1);
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref V = LoadV(Bits(Word, 9, 5));
  // Even and odd lanes of the whole register in the low half; a 64-bit vector
  // only reads the first half of each.
  Ref Even = _VUnZip(RS, ES, V, V);
  Ref Odd = _VUnZip2(RS, ES, V, V);
  Ref Sum = Signed ? _VAdd(RS, WideES, _VSXTL(RS, ES, Even), _VSXTL(RS, ES, Odd)).Node :
                     _VAdd(RS, WideES, _VUXTL(RS, ES, Even), _VUXTL(RS, ES, Odd)).Node;
  if (Accumulate) {
    Sum = _VAdd(RS, WideES, LoadV(Rd), Sum);
  }
  StoreVQ(Rd, Q, Sum);
  return true;
}

bool IRBuilder::UADDLP(uint32_t Word) { return SIMDAddLongPairwise(Word, false, false); }
bool IRBuilder::SADDLP(uint32_t Word) { return SIMDAddLongPairwise(Word, true, false); }
bool IRBuilder::UADALP(uint32_t Word) { return SIMDAddLongPairwise(Word, false, true); }
bool IRBuilder::SADALP(uint32_t Word) { return SIMDAddLongPairwise(Word, true, true); }

bool IRBuilder::SIMDMultiply(uint32_t Word, int Accumulate) {
  // MUL (Accumulate 0), MLA (+1), MLS (-1).
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  if (Size == 3) {
    return false;
  }
  const auto ES = ElementSizeFor(Size);
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref Product = _VMul(RS, ES, LoadV(Bits(Word, 9, 5)), LoadV(Bits(Word, 20, 16)));
  if (Accumulate > 0) {
    Product = _VAdd(RS, ES, LoadV(Rd), Product);
  } else if (Accumulate < 0) {
    Product = _VSub(RS, ES, LoadV(Rd), Product);
  }
  StoreVQ(Rd, Q, Product);
  return true;
}

bool IRBuilder::MUL_vec(uint32_t Word) { return SIMDMultiply(Word, 0); }
bool IRBuilder::MLA_vec(uint32_t Word) { return SIMDMultiply(Word, 1); }
bool IRBuilder::MLS_vec(uint32_t Word) { return SIMDMultiply(Word, -1); }

bool IRBuilder::MUL_elt(uint32_t Word) {
  // MUL by element: Rm lane `index` multiplies every lane of Rn.
  const bool Q = Bit(Word, 30);
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
  const auto ES = ElementSizeFor(Size);
  const auto RS = OpSize::i128Bit;
  Ref Element = _VDupElement(RS, ES, LoadV(Rm), Index);
  StoreVQ(Bits(Word, 4, 0), Q, _VMul(RS, ES, LoadV(Bits(Word, 9, 5)), Element));
  return true;
}

bool IRBuilder::REV16_asimd(uint32_t Word) {
  // Swap the bytes of every pair: interleave odd lanes before even lanes.
  if (Bits(Word, 23, 22) != 0) {
    return false;
  }
  const auto RS = OpSize::i128Bit;
  Ref V = LoadV(Bits(Word, 9, 5));
  Ref Even = _VUnZip(RS, OpSize::i8Bit, V, V);
  Ref Odd = _VUnZip2(RS, OpSize::i8Bit, V, V);
  StoreVQ(Bits(Word, 4, 0), Bit(Word, 30), _VZip(RS, OpSize::i8Bit, Odd, Even));
  return true;
}

bool IRBuilder::NEG_1(uint32_t Word) {
  if (Bits(Word, 23, 22) != 3) {
    return false;
  }
  StoreVSized(Bits(Word, 4, 0), OpSize::i64Bit, _VNeg(OpSize::i128Bit, OpSize::i64Bit, LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::ABS_1(uint32_t Word) {
  if (Bits(Word, 23, 22) != 3) {
    return false;
  }
  StoreVSized(Bits(Word, 4, 0), OpSize::i64Bit, _VAbs(OpSize::i128Bit, OpSize::i64Bit, LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::ADDP_pair(uint32_t Word) {
  // ADDP Dd, Vn.2D: the sum of both lanes.
  if (Bits(Word, 23, 22) != 3) {
    return false;
  }
  StoreVSized(Bits(Word, 4, 0), OpSize::i64Bit, _VAddV(OpSize::i128Bit, OpSize::i64Bit, LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::UQSUB_1(uint32_t Word) {
  // UQSUB on D registers: the difference, or 0 where it would go below zero.
  // POWERARM-M1-TODO(simd): FPSR.QC is not raised by the saturating operations.
  if (Bits(Word, 23, 22) != 3) {
    return false;
  }
  const auto RS = OpSize::i128Bit;
  const auto ES = OpSize::i64Bit;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));
  // B > A unsigned  <=>  max(A, B) != A.
  Ref Underflow = _VNot(RS, ES, _VCMPEQ(RS, ES, _VUMax(RS, ES, A, B), A));
  StoreVSized(Bits(Word, 4, 0), ES, _VAndn(RS, RS, _VSub(RS, ES, A, B), Underflow));
  return true;
}

// ---------------------------------------------------------------------------
// Table lookup
// ---------------------------------------------------------------------------

bool IRBuilder::TableLookup(uint32_t Word, bool IsTBX) {
  // TBL/TBX with 1-4 tables Vn..Vn+len. Indices 0..16n-1 select table bytes;
  // TBL gives zero for larger indices, TBX keeps Rd's byte. VTBL1/VTBL2 give
  // zero for indices past their 16/32 bytes.
  const bool Q = Bit(Word, 30);
  const uint32_t Rm = Bits(Word, 20, 16);
  const uint32_t Tables = Bits(Word, 14, 13) + 1;
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rd = Bits(Word, 4, 0);
  const auto RS = OpSize::i128Bit;

  Ref Indices = LoadV(Rm);
  std::array<Ref, 4> Table {};
  for (uint32_t i = 0; i < Tables; ++i) {
    Table[i] = LoadV((Rn + i) % 32);
  }

  if (IsTBX && Tables == 1) {
    StoreVQ(Rd, Q, _VTBX1(RS, LoadV(Rd), Table[0], Indices));
    return true;
  }

  Ref Result {};
  switch (Tables) {
  case 1: Result = _VTBL1(RS, Table[0], Indices); break;
  case 2: Result = _VTBL2(RS, Table[0], Table[1], Indices); break;
  default: {
    // Indices 32..63 index the second pair after subtracting 32; smaller
    // indices wrap to 224..255 and select nothing there.
    Ref High = _VSub(RS, OpSize::i8Bit, Indices, VectorConstant64(0x2020202020202020ULL));
    Ref Upper = Tables == 3 ? _VTBL1(RS, Table[2], High).Node : _VTBL2(RS, Table[2], Table[3], High).Node;
    Result = _VOr(RS, RS, _VTBL2(RS, Table[0], Table[1], Indices), Upper);
    break;
  }
  }

  if (IsTBX) {
    // In range: min(index, 16n - 1) == index.
    const uint64_t Last = (16ULL * Tables - 1) * 0x0101010101010101ULL;
    Ref InRange = _VCMPEQ(RS, OpSize::i8Bit, _VUMin(RS, OpSize::i8Bit, Indices, VectorConstant64(Last)), Indices);
    Result = _VBSL(RS, InRange, Result, LoadV(Rd));
  }
  StoreVQ(Rd, Q, Result);
  return true;
}

bool IRBuilder::TBL(uint32_t Word) {
  return TableLookup(Word, false);
}
bool IRBuilder::TBX(uint32_t Word) {
  return TableLookup(Word, true);
}

} // namespace FEXCore::A64
