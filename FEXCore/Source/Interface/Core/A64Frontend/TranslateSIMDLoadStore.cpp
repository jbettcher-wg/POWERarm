// SPDX-License-Identifier: MIT
//
// A64 SIMD&FP register loads and stores: literal, unsigned offset, unscaled,
// pre/post-indexed, register offset, pairs, and LD1/ST1 with one to four
// registers.
//
// Memory is accessed before any register is written, so a faulting access
// leaves the registers as they were. Loads of B/H/S/D registers clear the
// rest of the V register; LoadMem's FPR form zero-extends to 128 bits.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

namespace FEXCore::A64 {
using namespace FEXCore::IR;

namespace {
  // size:opc of the single-register SIMD&FP groups. opc<1> selects the
  // 128-bit register, which only exists with size == 00. Returns the transfer
  // size in bytes as log2, or -1 if unallocated.
  int FPSIMDScale(uint32_t Size, bool Opc1) {
    if (Opc1) {
      return Size == 0 ? 4 : -1;
    }
    return static_cast<int>(Size);
  }
} // namespace

void IRBuilder::LoadStoreV(bool IsLoad, OpSize Size, uint32_t Rt, Ref Address) {
  if (IsLoad) {
    StoreV(Rt, _LoadMem(RegClass::FPR, Size, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1));
  } else {
    _StoreMem(RegClass::FPR, Size, LoadV(Rt), Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
  }
}

bool IRBuilder::LDR_lit_fpsimd(uint32_t Word) {
  // opc 00: S, 01: D, 10: Q, 11: unallocated.
  const uint32_t Opc = Bits(Word, 31, 30);
  if (Opc == 0b11) {
    return false;
  }
  Ref Address = PCValue(CurrentPC + SignExtend(Bits(Word, 23, 5), 19) * 4);
  LoadStoreV(true, IR::SizeToOpSize(4U << Opc), Bits(Word, 4, 0), Address);
  return true;
}

bool IRBuilder::STR_LDR_imm_fpsimd_2(uint32_t Word) {
  // Unsigned scaled 12-bit offset.
  const int Scale = FPSIMDScale(Bits(Word, 31, 30), Bit(Word, 23));
  if (Scale < 0) {
    return false;
  }
  const uint64_t Offset = Bits(Word, 21, 10) << Scale;
  Ref Base = LoadXSP(Bits(Word, 9, 5));
  Ref Address = Offset ? _Add(OpSize::i64Bit, Base, Constant(Offset)) : Base;
  LoadStoreV(Bit(Word, 22), IR::SizeToOpSize(1U << Scale), Bits(Word, 4, 0), Address);
  return true;
}

bool IRBuilder::STUR_LDUR_fpsimd(uint32_t Word) {
  const int Scale = FPSIMDScale(Bits(Word, 31, 30), Bit(Word, 23));
  if (Scale < 0) {
    return false;
  }
  const int64_t Offset = SignExtend(Bits(Word, 20, 12), 9);
  Ref Base = LoadXSP(Bits(Word, 9, 5));
  Ref Address = Offset ? _Add(OpSize::i64Bit, Base, Constant(Offset)) : Base;
  LoadStoreV(Bit(Word, 22), IR::SizeToOpSize(1U << Scale), Bits(Word, 4, 0), Address);
  return true;
}

bool IRBuilder::STR_LDR_imm_fpsimd_1(uint32_t Word) {
  // Pre-indexed (bit 11 set) or post-indexed, with writeback.
  const int Scale = FPSIMDScale(Bits(Word, 31, 30), Bit(Word, 23));
  if (Scale < 0) {
    return false;
  }
  const bool IsLoad = Bit(Word, 22);
  const bool PostIndex = !Bit(Word, 11);
  const int64_t Offset = SignExtend(Bits(Word, 20, 12), 9);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);
  const auto Size = IR::SizeToOpSize(1U << Scale);

  Ref Base = LoadXSP(Rn);
  Ref Offsetted = Offset ? _Add(OpSize::i64Bit, Base, Constant(Offset)) : Base;
  Ref Address = PostIndex ? Base : Offsetted;
  if (IsLoad) {
    Ref Value = _LoadMem(RegClass::FPR, Size, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    StoreXSP(Rn, Offsetted);
    StoreV(Rt, Value);
  } else {
    _StoreMem(RegClass::FPR, Size, LoadV(Rt), Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    StoreXSP(Rn, Offsetted);
  }
  return true;
}

bool IRBuilder::STR_LDR_reg_fpsimd(uint32_t Word) {
  const int Scale = FPSIMDScale(Bits(Word, 31, 30), Bit(Word, 23));
  const uint32_t Option = Bits(Word, 15, 13);
  if (Scale < 0 || (Option & 0b010) == 0) {
    return false;
  }
  Ref Base = LoadXSP(Bits(Word, 9, 5));
  Ref Offset = ExtendReg(LoadX(Bits(Word, 20, 16)), Option, Bit(Word, 12) ? Scale : 0);
  LoadStoreV(Bit(Word, 22), IR::SizeToOpSize(1U << Scale), Bits(Word, 4, 0), _Add(OpSize::i64Bit, Base, Offset));
  return true;
}

bool IRBuilder::STP_LDP_fpsimd(uint32_t Word) {
  const uint32_t Opc = Bits(Word, 31, 30);
  const bool PreOrOffset = Bit(Word, 24);
  const bool WriteBack = Bit(Word, 23);
  const bool IsLoad = Bit(Word, 22);
  const uint32_t Rt2 = Bits(Word, 14, 10);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  // opc 00: S, 01: D, 10: Q, 11: unallocated. p=0, w=0 is the no-allocate pair (its own entry).
  if (Opc == 0b11 || (!PreOrOffset && !WriteBack)) {
    return false;
  }

  const int64_t ElementSize = 4LL << Opc;
  const auto Size = IR::SizeToOpSize(ElementSize);
  const int64_t Offset = SignExtend(Bits(Word, 21, 15), 7) * ElementSize;

  Ref Base = LoadXSP(Rn);
  Ref Offsetted = _Add(OpSize::i64Bit, Base, Constant(Offset));
  Ref Address = PreOrOffset ? Offsetted : Base;
  Ref Address2 = _Add(OpSize::i64Bit, Address, Constant(ElementSize));

  if (!IsLoad) {
    _StoreMem(RegClass::FPR, Size, LoadV(Rt), Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    _StoreMem(RegClass::FPR, Size, LoadV(Rt2), Address2, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    if (WriteBack) {
      StoreXSP(Rn, Offsetted);
    }
    return true;
  }

  Ref Value1 = _LoadMem(RegClass::FPR, Size, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
  Ref Value2 = _LoadMem(RegClass::FPR, Size, Address2, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
  if (WriteBack) {
    StoreXSP(Rn, Offsetted);
  }
  StoreV(Rt, Value1);
  StoreV(Rt2, Value2);
  return true;
}

bool IRBuilder::LDx_STx_mult(uint32_t Word) {
  // LD1/ST1 with 1-4 registers. Element size does not change the memory
  // image: every element is little endian, so the structure is the vector's
  // own byte image.
  const bool Q = Bit(Word, 30);
  const bool PostIndex = Bit(Word, 23);
  const bool IsLoad = Bit(Word, 22);
  const uint32_t Rm = Bits(Word, 20, 16);
  const uint32_t Opcode = Bits(Word, 15, 12);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  uint32_t Registers {};
  switch (Opcode) {
  case 0b0111: Registers = 1; break;
  case 0b1010: Registers = 2; break;
  case 0b0110: Registers = 3; break;
  case 0b0010: Registers = 4; break;
  case 0b1000: return LD2_ST2_mult(Word);
  default:
    // POWERARM-M2-TODO(simd): LD3/LD4 and ST3/ST4 (three- and four-way interleaved structures) are not translated.
    return false;
  }

  const uint64_t RegBytes = Q ? 16 : 8;
  const auto Size = Q ? OpSize::i128Bit : OpSize::i64Bit;

  Ref Base = LoadXSP(Rn);
  if (IsLoad) {
    Ref Values[4] {};
    for (uint32_t i = 0; i < Registers; ++i) {
      Ref Address = i ? _Add(OpSize::i64Bit, Base, Constant(i * RegBytes)) : Base;
      Values[i] = _LoadMem(RegClass::FPR, Size, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    }
    if (PostIndex) {
      Ref Increment = Rm == 31 ? Constant(Registers * RegBytes) : LoadX(Rm);
      StoreXSP(Rn, _Add(OpSize::i64Bit, Base, Increment));
    }
    for (uint32_t i = 0; i < Registers; ++i) {
      StoreV((Rt + i) % 32, Values[i]);
    }
    return true;
  }

  for (uint32_t i = 0; i < Registers; ++i) {
    Ref Address = i ? _Add(OpSize::i64Bit, Base, Constant(i * RegBytes)) : Base;
    _StoreMem(RegClass::FPR, Size, LoadV((Rt + i) % 32), Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
  }
  if (PostIndex) {
    Ref Increment = Rm == 31 ? Constant(Registers * RegBytes) : LoadX(Rm);
    StoreXSP(Rn, _Add(OpSize::i64Bit, Base, Increment));
  }
  return true;
}

bool IRBuilder::LD2_ST2_mult(uint32_t Word) {
  // Two interleaved structures: memory holds A0 B0 A1 B1 ... and the
  // registers hold A and B. With the memory read as vectors M0 (low) and M1,
  // A and B are the even and odd elements of M1:M0 (UZP1/UZP2 of M0, M1), and
  // the store is the inverse (ZIP1/ZIP2). The 64-bit forms use one 128-bit
  // image, the same construction as SIMDPermute's 64-bit unzip and zip
  // (NEON-LANDINGS 3.13: vpku*um for the load, vmrgl/h* for the store).
  const bool Q = Bit(Word, 30);
  const bool PostIndex = Bit(Word, 23);
  const bool IsLoad = Bit(Word, 22);
  const uint32_t Rm = Bits(Word, 20, 16);
  const uint32_t Size = Bits(Word, 11, 10);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);
  const uint32_t Rt2 = (Rt + 1) % 32;
  if (!Q && Size == 3) {
    return false;
  }
  const auto ES = static_cast<OpSize>(1U << Size);
  const auto RS = OpSize::i128Bit;
  const uint64_t Bytes = Q ? 32 : 16;

  Ref Base = LoadXSP(Rn);
  auto WriteBack = [&]() {
    if (PostIndex) {
      StoreXSP(Rn, _Add(OpSize::i64Bit, Base, Rm == 31 ? Constant(Bytes) : LoadX(Rm)));
    }
  };
  if (IsLoad) {
    Ref A {}, B {};
    if (Q) {
      Ref M0 = _LoadMem(RegClass::FPR, RS, Base, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
      Ref M1 = _LoadMem(RegClass::FPR, RS, _Add(OpSize::i64Bit, Base, Constant(16)), Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
      A = _VUnZip(RS, ES, M0, M1);
      B = _VUnZip2(RS, ES, M0, M1);
    } else {
      Ref C = _LoadMem(RegClass::FPR, RS, Base, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
      A = _VUnZip(RS, ES, C, C);
      B = _VUnZip2(RS, ES, C, C);
    }
    WriteBack();
    StoreVQ(Rt, Q, A);
    StoreVQ(Rt2, Q, B);
    return true;
  }

  Ref A = LoadV(Rt);
  Ref B = LoadV(Rt2);
  if (Q) {
    Ref M0 = _VZip(RS, ES, A, B);
    Ref M1 = _VZip2(RS, ES, A, B);
    _StoreMem(RegClass::FPR, RS, M0, Base, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    _StoreMem(RegClass::FPR, RS, M1, _Add(OpSize::i64Bit, Base, Constant(16)), Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
  } else {
    Ref X = _VInsElement(RS, OpSize::i64Bit, 1, 0, A, A);
    Ref Y = _VInsElement(RS, OpSize::i64Bit, 1, 0, B, B);
    _StoreMem(RegClass::FPR, RS, _VZip2(RS, ES, X, Y), Base, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
  }
  WriteBack();
  return true;
}

bool IRBuilder::LDx_STx_sngl(uint32_t Word) {
  // ARM ARM "AdvSIMD load/store single structure": one lane of each of
  // selem registers, at element index `index`. Only selem == 1 (LD1/ST1) is
  // translated.
  const bool Q = Bit(Word, 30);
  const bool PostIndex = Bit(Word, 23);
  const bool IsLoad = Bit(Word, 22);
  const bool R = Bit(Word, 21);
  const uint32_t Rm = Bits(Word, 20, 16);
  const uint32_t Opcode = Bits(Word, 15, 13);
  const bool S = Bit(Word, 12);
  const uint32_t SizeBits = Bits(Word, 11, 10);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  uint32_t Scale = Opcode >> 1;
  const uint32_t Selem = (((Opcode & 1) << 1) | R) + 1;
  uint32_t Index {};
  switch (Scale) {
  case 0: Index = (Q << 3) | (S << 2) | SizeBits; break;
  case 1:
    if (SizeBits & 1) {
      return false;
    }
    Index = (Q << 2) | (S << 1) | (SizeBits >> 1);
    break;
  case 2:
    if (SizeBits & 2) {
      return false;
    }
    if (!(SizeBits & 1)) {
      Index = (Q << 1) | S;
    } else {
      if (S) {
        return false;
      }
      Index = Q;
      Scale = 3;
    }
    break;
  default: return false;
  }
  // POWERARM-M2-TODO(simd): LD2/LD3/LD4 and ST2/ST3/ST4 single structures (selem > 1) are not translated.
  if (Selem != 1) {
    return false;
  }

  const auto ElementSize = static_cast<OpSize>(1U << Scale);
  Ref Base = LoadXSP(Rn);
  if (IsLoad) {
    Ref Value = _LoadMem(RegClass::GPR, ElementSize, Base, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    if (PostIndex) {
      StoreXSP(Rn, _Add(OpSize::i64Bit, Base, Rm == 31 ? Constant(Selem << Scale) : LoadX(Rm)));
    }
    StoreV(Rt, _VInsGPR(OpSize::i128Bit, ElementSize, Index, LoadV(Rt), Value));
    return true;
  }
  _StoreMem(RegClass::GPR, ElementSize, _VExtractToGPR(OpSize::i128Bit, ElementSize, LoadV(Rt), Index), Base, Invalid(), OpSize::i8Bit,
            MemOffsetType::SXTX, 1);
  if (PostIndex) {
    StoreXSP(Rn, _Add(OpSize::i64Bit, Base, Rm == 31 ? Constant(Selem << Scale) : LoadX(Rm)));
  }
  return true;
}

} // namespace FEXCore::A64
