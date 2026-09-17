// SPDX-License-Identifier: MIT
//
// A64 SIMD&FP register loads and stores: literal, unsigned offset, unscaled,
// pre/post-indexed, register offset, pairs, LD1-4/ST1-4 (multiple and single
// structures) and LD1R-LD4R.
//
// Memory is accessed before any register is written, so a faulting access
// leaves the registers as they were. Loads of B/H/S/D registers clear the
// rest of the V register; LoadMem's FPR form zero-extends to 128 bits.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

#include <algorithm>
#include <array>

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

bool IRBuilder::STNP_LDNP_fpsimd(uint32_t Word) {
  // Index bits 00: the pair at Rn + offset without writeback, i.e. the
  // signed-offset form (the Pi 5 executes it that way).
  return STP_LDP_fpsimd(Word | (1U << 24));
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

// Byte-permutation helper for interleaved structures: Out byte k takes
// Source[Map[k] / 16] byte Map[k] % 16 for up to four 16-byte sources, and
// zero for Map[k] == 0xFF. Two table lookups over source pairs, OR-combined:
// each lookup yields zero for indices outside its pair.
Ref IRBuilder::PermuteBytes(const std::array<Ref, 4>& Sources, uint32_t NumSources, const std::array<uint8_t, 16>& Map) {
  const auto RS = OpSize::i128Bit;
  auto IndexVector = [&](uint32_t FirstSource) -> Ref {
    uint64_t Lo {}, Hi {};
    for (uint32_t k = 0; k < 16; ++k) {
      uint64_t Index = 0xFF;
      const uint32_t Source = Map[k] / 16;
      if (Map[k] != 0xFF && Source >= FirstSource && Source < FirstSource + 2) {
        Index = Map[k] - FirstSource * 16;
      }
      (k < 8 ? Lo : Hi) |= Index << (8 * (k % 8));
    }
    return _VLoadTwoGPRs(Constant(Lo), Constant(Hi));
  };
  Ref Result = _VTBL2(RS, Sources[0], NumSources > 1 ? Sources[1] : Sources[0], IndexVector(0));
  if (NumSources > 2) {
    Ref Upper = _VTBL2(RS, Sources[2], NumSources > 3 ? Sources[3] : Sources[2], IndexVector(2));
    Result = _VOr(RS, RS, Result, Upper);
  }
  return Result;
}

bool IRBuilder::LDx_STx_mult(uint32_t Word) {
  // LD1-4/ST1-4 (multiple structures). LD1/ST1 with 1-4 registers move each
  // register's own byte image (every element is little endian). LD2-4/ST2-4
  // interleave: element i of register s sits at memory element i * n + s.
  const bool Q = Bit(Word, 30);
  const bool PostIndex = Bit(Word, 23);
  const bool IsLoad = Bit(Word, 22);
  const uint32_t Rm = Bits(Word, 20, 16);
  const uint32_t Opcode = Bits(Word, 15, 12);
  const uint32_t Size = Bits(Word, 11, 10);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  uint32_t Registers {};
  uint32_t Interleave = 1;
  switch (Opcode) {
  case 0b0111: Registers = 1; break;
  case 0b1010: Registers = 2; break;
  case 0b0110: Registers = 3; break;
  case 0b0010: Registers = 4; break;
  case 0b1000: Registers = Interleave = 2; break;
  case 0b0100: Registers = Interleave = 3; break;
  case 0b0000: Registers = Interleave = 4; break;
  default: return false;
  }
  if (Interleave > 1 && Size == 3 && !Q) {
    return false;
  }

  const uint64_t RegBytes = Q ? 16 : 8;
  const uint64_t TotalBytes = Registers * RegBytes;
  const auto RS = OpSize::i128Bit;
  Ref Base = LoadXSP(Rn);

  auto WriteBack = [&] {
    if (PostIndex) {
      Ref Increment = Rm == 31 ? Constant(TotalBytes) : LoadX(Rm);
      StoreXSP(Rn, _Add(OpSize::i64Bit, Base, Increment));
    }
  };

  if (Interleave == 1) {
    const auto MemSize = Q ? OpSize::i128Bit : OpSize::i64Bit;
    if (IsLoad) {
      Ref Values[4] {};
      for (uint32_t i = 0; i < Registers; ++i) {
        Ref Address = i ? _Add(OpSize::i64Bit, Base, Constant(i * RegBytes)) : Base;
        Values[i] = _LoadMem(RegClass::FPR, MemSize, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
      }
      WriteBack();
      for (uint32_t i = 0; i < Registers; ++i) {
        StoreV((Rt + i) % 32, Values[i]);
      }
      return true;
    }
    for (uint32_t i = 0; i < Registers; ++i) {
      Ref Address = i ? _Add(OpSize::i64Bit, Base, Constant(i * RegBytes)) : Base;
      _StoreMem(RegClass::FPR, MemSize, LoadV((Rt + i) % 32), Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    }
    WriteBack();
    return true;
  }

  const uint32_t ElementBytes = 1U << Size;
  const uint32_t Chunks = (TotalBytes + 15) / 16;
  const auto ES = IR::SizeToOpSize(ElementBytes);

  if (IsLoad) {
    std::array<Ref, 4> Chunk {};
    for (uint32_t c = 0; c < Chunks; ++c) {
      const uint64_t Bytes = std::min<uint64_t>(16, TotalBytes - 16 * c);
      Ref Address = c ? _Add(OpSize::i64Bit, Base, Constant(16 * c)).Node : Base;
      Chunk[c] = _LoadMem(RegClass::FPR, IR::SizeToOpSize(Bytes), Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    }
    std::array<Ref, 4> Values {};
    for (uint32_t s = 0; s < Interleave; ++s) {
      if (Interleave == 2) {
        // The two-register form is exactly an unzip of the memory image.
        Ref Upper = Q ? Chunk[1] : Chunk[0];
        Values[s] = s == 0 ? _VUnZip(RS, ES, Chunk[0], Upper).Node : _VUnZip2(RS, ES, Chunk[0], Upper).Node;
        continue;
      }
      std::array<uint8_t, 16> Map {};
      for (uint32_t k = 0; k < 16; ++k) {
        Map[k] = k < RegBytes ? ((k / ElementBytes) * Interleave + s) * ElementBytes + k % ElementBytes : 0xFF;
      }
      Values[s] = PermuteBytes(Chunk, Chunks, Map);
    }
    WriteBack();
    for (uint32_t s = 0; s < Interleave; ++s) {
      StoreVQ((Rt + s) % 32, Q, Values[s]);
    }
    return true;
  }

  std::array<Ref, 4> Regs {};
  for (uint32_t s = 0; s < Interleave; ++s) {
    Regs[s] = LoadV((Rt + s) % 32);
  }
  for (uint32_t c = 0; c < Chunks; ++c) {
    const uint64_t Bytes = std::min<uint64_t>(16, TotalBytes - 16 * c);
    Ref Image {};
    if (Interleave == 2) {
      Image = c == 0 ? _VZip(RS, ES, Regs[0], Regs[1]).Node : _VZip2(RS, ES, Regs[0], Regs[1]).Node;
    } else {
      std::array<uint8_t, 16> Map {};
      for (uint32_t b = 0; b < 16; ++b) {
        const uint32_t m = 16 * c + b;
        if (m >= TotalBytes) {
          Map[b] = 0xFF;
          continue;
        }
        const uint32_t Element = m / ElementBytes;
        const uint32_t s = Element % Interleave;
        const uint32_t i = Element / Interleave;
        Map[b] = s * 16 + i * ElementBytes + m % ElementBytes;
      }
      Image = PermuteBytes(Regs, Interleave, Map);
    }
    Ref Address = c ? _Add(OpSize::i64Bit, Base, Constant(16 * c)).Node : Base;
    _StoreMem(RegClass::FPR, IR::SizeToOpSize(Bytes), Image, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
  }
  WriteBack();
  return true;
}

bool IRBuilder::SIMDSingleStructure(uint32_t Word) {
  // LD1-4/ST1-4 (single structure) and LD1R-LD4R: one element of each of
  // 1-4 registers, or a replicated element.
  const bool Q = Bit(Word, 30);
  const bool PostIndex = Bit(Word, 23);
  const bool IsLoad = Bit(Word, 22);
  const bool R = Bit(Word, 21);
  const uint32_t Rm = Bits(Word, 20, 16);
  const uint32_t Opcode = Bits(Word, 15, 13);
  const bool S = Bit(Word, 12);
  const uint32_t Size = Bits(Word, 11, 10);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  const uint32_t Structures = ((Opcode & 1) << 1 | R) + 1;
  uint32_t Scale = Opcode >> 1;
  uint32_t Index = 0;
  bool Replicate = false;
  switch (Scale) {
  case 0: Index = (Q << 3) | (S << 2) | Size; break;
  case 1:
    if (Size & 1) {
      return false;
    }
    Index = (Q << 2) | (S << 1) | (Size >> 1);
    break;
  case 2:
    if (Size & 2) {
      return false;
    }
    if (Size & 1) {
      if (S) {
        return false;
      }
      Index = Q;
      Scale = 3;
    } else {
      Index = (Q << 1) | S;
    }
    break;
  default:
    if (!IsLoad || S) {
      return false;
    }
    Replicate = true;
    Scale = Size;
    break;
  }

  const uint64_t ElementBytes = 1ULL << Scale;
  const auto ES = IR::SizeToOpSize(ElementBytes);
  Ref Base = LoadXSP(Rn);

  std::array<Ref, 4> Loaded {};
  for (uint32_t s = 0; s < Structures; ++s) {
    Ref Address = s ? _Add(OpSize::i64Bit, Base, Constant(s * ElementBytes)).Node : Base;
    if (IsLoad) {
      Loaded[s] = _LoadMem(RegClass::GPR, ES, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    } else {
      Ref Element = _VExtractToGPR(OpSize::i128Bit, ES, LoadV((Rt + s) % 32), Index);
      _StoreMem(RegClass::GPR, ES, Element, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    }
  }
  if (PostIndex) {
    Ref Increment = Rm == 31 ? Constant(Structures * ElementBytes) : LoadX(Rm);
    StoreXSP(Rn, _Add(OpSize::i64Bit, Base, Increment));
  }
  if (IsLoad) {
    for (uint32_t s = 0; s < Structures; ++s) {
      const uint32_t Reg = (Rt + s) % 32;
      if (Replicate) {
        StoreVQ(Reg, Q, _VDupFromGPR(OpSize::i128Bit, ES, Loaded[s]));
      } else {
        StoreV(Reg, _VInsGPR(OpSize::i128Bit, ES, Index, LoadV(Reg), Loaded[s]));
      }
    }
  }
  return true;
}

} // namespace FEXCore::A64
