// SPDX-License-Identifier: MIT
//
// A64 general-purpose register loads and stores: literal, unsigned offset,
// unscaled, pre/post-indexed, register offset with extend, pairs, and the
// unprivileged forms (identical to unscaled at EL0).
//
// For Rt == Rn with writeback the architecture leaves the result UNKNOWN; a
// store writes the value of Rt before the writeback and a load's value wins
// over the writeback. Memory is accessed before any register is written, so a
// faulting access leaves the registers as they were.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

namespace FEXCore::A64 {
using namespace FEXCore::IR;

namespace {
  enum class MemOp { Store, Load, Prefetch, Unallocated };

  struct OpcDecode {
    MemOp Op;
    bool SignExtend;
    bool Is64Dest;
  };

  // Arm pseudocode for size:opc in the single-register load/store groups.
  OpcDecode DecodeSizeOpc(uint32_t Size, uint32_t Opc) {
    if ((Opc & 2) == 0) {
      return {(Opc & 1) ? MemOp::Load : MemOp::Store, false, Size == 3};
    }
    if (Size == 3) {
      return {(Opc & 1) ? MemOp::Unallocated : MemOp::Prefetch, false, true};
    }
    if (Size == 2 && (Opc & 1)) {
      return {MemOp::Unallocated, false, false};
    }
    return {MemOp::Load, true, (Opc & 1) == 0};
  }
} // namespace

void IRBuilder::LoadStoreSingle(bool IsLoad, OpSize Size, bool SignExtend, bool Is64Dest, uint32_t Rt, Ref Address, Ref Offset) {
  if (!Offset) {
    Offset = Invalid();
  }
  if (!IsLoad) {
    _StoreMem(RegClass::GPR, Size, LoadX(Rt), Address, Offset, OpSize::i8Bit, MemOffsetType::SXTX, 1);
    return;
  }

  Ref Value = _LoadMem(RegClass::GPR, Size, Address, Offset, OpSize::i8Bit, MemOffsetType::SXTX, 1);
  if (SignExtend && Size != OpSize::i64Bit) {
    Value = _Sbfe(OpSize::i64Bit, IR::OpSizeAsBits(Size), 0, Value);
  }
  // Narrow loads are zero-extended by LoadMem; sign-extended W loads are
  // truncated by StoreW.
  StoreReg(Rt, Is64Dest, Value);
}

// ---------------------------------------------------------------------------
// Literal
// ---------------------------------------------------------------------------

bool IRBuilder::LDR_lit_gen(uint32_t Word) {
  const bool Is64 = Bit(Word, 30);
  Ref Address = PCValue(CurrentPC + SignExtend(Bits(Word, 23, 5), 19) * 4);
  LoadStoreSingle(true, Is64 ? OpSize::i64Bit : OpSize::i32Bit, false, Is64, Bits(Word, 4, 0), Address);
  return true;
}

bool IRBuilder::LDRSW_lit(uint32_t Word) {
  Ref Address = PCValue(CurrentPC + SignExtend(Bits(Word, 23, 5), 19) * 4);
  LoadStoreSingle(true, OpSize::i32Bit, true, true, Bits(Word, 4, 0), Address);
  return true;
}

bool IRBuilder::PRFM_lit(uint32_t) {
  return true;
}

bool IRBuilder::PRFM_imm(uint32_t) {
  return true;
}

// ---------------------------------------------------------------------------
// Immediate offsets
// ---------------------------------------------------------------------------

bool IRBuilder::STLURx_LDAPURx(uint32_t Word) {
  // LRCPC unscaled loads and stores: the STURx/LDURx fields with bit 24 set,
  // except that size 11 with opc 1x is unallocated rather than a prefetch.
  if (Bits(Word, 31, 30) == 3 && Bit(Word, 23)) {
    return false;
  }
  return LoadStoreImm9(Word & ~(1U << 24));
}

bool IRBuilder::LoadStoreImm9(uint32_t Word) {
  // STURx/LDURx (bits 11:10 = 00), post-index (01), unprivileged (10), pre-index (11).
  const uint32_t Size = Bits(Word, 31, 30);
  const auto Decode = DecodeSizeOpc(Size, Bits(Word, 23, 22));
  const uint32_t Mode = Bits(Word, 11, 10);
  const int64_t Offset = SignExtend(Bits(Word, 20, 12), 9);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  if (Decode.Op == MemOp::Unallocated) {
    return false;
  }
  if (Decode.Op == MemOp::Prefetch) {
    // PRFM (unscaled) is its own table entry; the indexed and unprivileged
    // forms of size=11 opc=10 are unallocated.
    return Mode == 0b00;
  }

  const bool WriteBack = Mode == 0b01 || Mode == 0b11;
  const bool PostIndex = Mode == 0b01;
  const auto MemSize = IR::SizeToOpSize(1U << Size);

  Ref Base = LoadXSP(Rn);
  Ref Offsetted = (Offset && WriteBack) ? _Add(OpSize::i64Bit, Base, Constant(Offset)) : Base;
  Ref Address = PostIndex ? Base : Offsetted;

  if (Decode.Op == MemOp::Store) {
    // The store precedes the writeback so a faulting store leaves Rn intact.
    if (!WriteBack) {
      LoadStoreSingle(false, MemSize, false, false, Rt, Base, Offset ? _InlineConstant(Offset) : nullptr);
      return true;
    }
    _StoreMem(RegClass::GPR, MemSize, LoadX(Rt), Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    if (WriteBack) {
      StoreXSP(Rn, Offsetted);
    }
    return true;
  }

  if (WriteBack) {
    // Load first so a faulting load leaves Rn intact, then write both back.
    Ref Value = _LoadMem(RegClass::GPR, MemSize, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    if (Decode.SignExtend && MemSize != OpSize::i64Bit) {
      Value = _Sbfe(OpSize::i64Bit, IR::OpSizeAsBits(MemSize), 0, Value);
    }
    StoreXSP(Rn, Offsetted);
    StoreReg(Rt, Decode.Is64Dest, Value);
    return true;
  }

  // No writeback: the offset rides on the load as a displacement instead of an
  // Add into a scratch register.
  LoadStoreSingle(true, MemSize, Decode.SignExtend, Decode.Is64Dest, Rt, Base, Offset ? _InlineConstant(Offset) : nullptr);
  return true;
}

bool IRBuilder::STRx_LDRx_imm_2(uint32_t Word) {
  // Unsigned scaled 12-bit offset, no writeback.
  const uint32_t Size = Bits(Word, 31, 30);
  const auto Decode = DecodeSizeOpc(Size, Bits(Word, 23, 22));
  if (Decode.Op == MemOp::Unallocated) {
    return false;
  }
  if (Decode.Op == MemOp::Prefetch) {
    return true;
  }

  const uint64_t Offset = Bits(Word, 21, 10) << Size;
  Ref Base = LoadXSP(Bits(Word, 9, 5));
  LoadStoreSingle(Decode.Op == MemOp::Load, IR::SizeToOpSize(1U << Size), Decode.SignExtend, Decode.Is64Dest, Bits(Word, 4, 0), Base,
                  Offset ? _InlineConstant(Offset) : nullptr);
  return true;
}

// ---------------------------------------------------------------------------
// Register offset
// ---------------------------------------------------------------------------

bool IRBuilder::LoadStoreRegOffset(uint32_t Word) {
  const uint32_t Size = Bits(Word, 31, 30);
  const auto Decode = DecodeSizeOpc(Size, Bits(Word, 23, 22));
  const uint32_t Option = Bits(Word, 15, 13);
  const bool Scaled = Bit(Word, 12);

  if ((Option & 0b010) == 0 || Decode.Op == MemOp::Unallocated) {
    return false;
  }
  if (Decode.Op == MemOp::Prefetch) {
    return true;
  }

  Ref Base = LoadXSP(Bits(Word, 9, 5));
  Ref Offset = ExtendReg(LoadX(Bits(Word, 20, 16)), Option, Scaled ? Size : 0);
  Ref Address = _Add(OpSize::i64Bit, Base, Offset);
  LoadStoreSingle(Decode.Op == MemOp::Load, IR::SizeToOpSize(1U << Size), Decode.SignExtend, Decode.Is64Dest, Bits(Word, 4, 0), Address);
  return true;
}

// ---------------------------------------------------------------------------
// Pairs
// ---------------------------------------------------------------------------

bool IRBuilder::STNP_LDNP_gen(uint32_t Word) {
  // Index bits 00: the signed-offset pair without writeback (see STNP_LDNP_fpsimd).
  return STP_LDP_gen(Word | (1U << 24));
}

bool IRBuilder::STP_LDP_gen(uint32_t Word) {
  const uint32_t Opc = Bits(Word, 31, 30);
  const bool PreOrOffset = Bit(Word, 24);
  const bool WriteBack = Bit(Word, 23);
  const bool IsLoad = Bit(Word, 22);
  const uint32_t Rt2 = Bits(Word, 14, 10);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  // opc=00: 32-bit, opc=01: LDPSW (load only), opc=10: 64-bit, opc=11: unallocated.
  if (Opc == 0b11 || (Opc == 0b01 && !IsLoad) || (!PreOrOffset && !WriteBack)) {
    return false;
  }

  const bool Is64 = Opc == 0b10;
  const bool SignExtend = Opc == 0b01;
  const auto MemSize = Is64 ? OpSize::i64Bit : OpSize::i32Bit;
  const int64_t ElementSize = Is64 ? 8 : 4;
  const int64_t Offset = FEXCore::A64::SignExtend(Bits(Word, 21, 15), 7) * ElementSize;
  const bool PostIndex = !PreOrOffset;

  // Both elements are addressed as a base plus a displacement on the memory op:
  // Rn for the offset and post-index forms, the written-back Rn+offset for
  // pre-index.
  Ref Base = LoadXSP(Rn);
  Ref Offsetted = WriteBack ? _Add(OpSize::i64Bit, Base, Constant(Offset)) : Base;
  const bool PreIndex = WriteBack && !PostIndex;
  Ref Address = PreIndex ? Offsetted : Base;
  const int64_t Disp1 = (WriteBack || Offset == 0) ? 0 : Offset;
  const int64_t Disp2 = Disp1 + ElementSize;
  Ref Offset1 = Disp1 ? _InlineConstant(Disp1) : Invalid();
  Ref Offset2 = _InlineConstant(Disp2);

  if (!IsLoad) {
    Ref Value1 = LoadX(Rt);
    Ref Value2 = LoadX(Rt2);
    _StoreMem(RegClass::GPR, MemSize, Value1, Address, Offset1, OpSize::i8Bit, MemOffsetType::SXTX, 1);
    _StoreMem(RegClass::GPR, MemSize, Value2, Address, Offset2, OpSize::i8Bit, MemOffsetType::SXTX, 1);
    if (WriteBack) {
      StoreXSP(Rn, Offsetted);
    }
    return true;
  }

  Ref Value1 = _LoadMem(RegClass::GPR, MemSize, Address, Offset1, OpSize::i8Bit, MemOffsetType::SXTX, 1);
  Ref Value2 = _LoadMem(RegClass::GPR, MemSize, Address, Offset2, OpSize::i8Bit, MemOffsetType::SXTX, 1);
  if (SignExtend) {
    Value1 = _Sbfe(OpSize::i64Bit, 32, 0, Value1);
    Value2 = _Sbfe(OpSize::i64Bit, 32, 0, Value2);
  }
  if (WriteBack) {
    StoreXSP(Rn, Offsetted);
  }
  const bool Is64Dest = Is64 || SignExtend;
  StoreReg(Rt, Is64Dest, Value1);
  StoreReg(Rt2, Is64Dest, Value2);
  return true;
}

} // namespace FEXCore::A64
