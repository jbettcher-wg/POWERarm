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

#include <cstring>

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

  // The register-offset forms' `option` field, restricted to the four values
  // the encoding allows there (bit 1 set): 010 UXTW, 011 LSL/UXTX, 110 SXTW,
  // 111 SXTX. UXTX and SXTX both mean "the whole 64-bit register", which is
  // MemOffsetType::SXTX -- no extension at all.
  IR::MemOffsetType ExtendOffsetType(uint32_t Option) {
    switch (Option) {
    case 0b010: return IR::MemOffsetType::UXTW;
    case 0b110: return IR::MemOffsetType::SXTW;
    default: return IR::MemOffsetType::SXTX;
    }
  }
} // namespace

void IRBuilder::LoadStoreSingle(bool IsLoad, OpSize Size, bool SignExtend, bool Is64Dest, uint32_t Rt, Ref Address, Ref Offset,
                                IR::MemOffsetType OffsetType, uint8_t OffsetScale) {
  if (!Offset) {
    Offset = Invalid();
  }
  if (!IsLoad) {
    _StoreMem(RegClass::GPR, Size, LoadX(Rt), Address, Offset, OpSize::i8Bit, OffsetType, OffsetScale);
    return;
  }

  Ref Value = _LoadMem(RegClass::GPR, Size, Address, Offset, OpSize::i8Bit, OffsetType, OffsetScale);
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

  // The extend, the scale and the add all belong to the memory instruction:
  // POWER's X-form takes base + index directly, and MakeAddrForm folds the
  // UXTW/SXTW extend and the scale into one instruction (rldic, or extswsli on
  // ISA 3.0). Handing the backend a pre-computed address instead cost an
  // explicit add and wasted the X-form's index operand on r0.
  const uint32_t Rm = Bits(Word, 20, 16);
  Ref Base = LoadXSP(Bits(Word, 9, 5));
  const auto MemSize = IR::SizeToOpSize(1U << Size);
  const bool IsLoad = Decode.Op == MemOp::Load;
  if (Rm == 31) {
    // Every extend of XZR is zero, so this is the bare base address.
    LoadStoreSingle(IsLoad, MemSize, Decode.SignExtend, Decode.Is64Dest, Bits(Word, 4, 0), Base);
    return true;
  }
  LoadStoreSingle(IsLoad, MemSize, Decode.SignExtend, Decode.Is64Dest, Bits(Word, 4, 0), Base, LoadX(Rm),
                  ExtendOffsetType(Option), Scaled ? static_cast<uint8_t>(1U << Size) : 1);
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

// ---------------------------------------------------------------------------
// A memory access fused with the byte reverse next to it
// ---------------------------------------------------------------------------
//
// POWER has load-and-byte-reverse and store-and-byte-reverse at halfword,
// word and doubleword (lhbrx/lwbrx/ldbrx, sthbrx/stwbrx/stdbrx, all X-form).
// AArch64 has no byte-swapping memory form at all, so every big-endian field
// a guest reads or writes is an access plus a REV -- which is why the pair is
// adjacent in the WebRTC, ffmpeg, PNG and ICU parsers, in V8's and JSC's
// serialisers, and in every NSS/resolv wire format in libc.
//
// Unfused, the REV alone is 5 host instructions for `rev16` (the SWAR mask
// chain in REV16_int), 3 for a 32-bit `rev`, and for a 64-bit `rev` an
// `addi; stdbrx; ld` through the red zone -- a store-to-load forward on the
// dependent chain. Fused, the whole pair is the one memory instruction (plus
// an addi when a displacement has to be folded in, since the byte-reversed
// forms are X-form only: base + index, no displacement).
//
// WHAT THE TWO DIRECTIONS ELIDE, AND WHAT THEY DO NOT.
//
// Load then REV elides the load's write of the UNREVERSED value, which the
// match proves dead: the REV both reads and writes the very register the load
// wrote, and the two are adjacent in one decoded block. The fused instruction
// faults where the load faults, at the same PC, with Rt unchanged, so a
// resumed synchronous fault sees exactly the unfused state. An asynchronous
// signal taken between the two in unfused code would see Rt unreversed and
// the fused code cannot produce that state -- the same acceptance the
// RBIT+CLZ, vector-scan and exclusive-loop fusions already made.
//
// REV then store elides NOTHING: a guest register is architectural state and
// nothing here proves the reversed value dead, so the REV still runs. What
// the fusion removes is the store's DEPENDENCE on it -- st{h,w,d}brx takes
// the unreversed register, so the store issues without waiting for the byte
// swap (at 64 bits, without waiting for a store-forward stall). The byte
// order in memory is identical, because that is what the instruction is for.
// A byte-reversed store that faults on an mtrack-protected code page is not a
// form SyscallsSMCTracking's DecodePPCStore emulates; it takes the legacy
// invalidate-unprotect-retry path, which is correct and already what every
// vector and store-conditional form there does.
namespace {
  // One side of the pair, decoded from the guest word. Only the plain
  // base+displacement and base+index(64-bit, unscaled) shapes are here:
  // LoadMemRev/StoreMemRev take an address register and nothing else, so an
  // address needing a UXTW/SXTW extend or a scaled index would cost more IR
  // than the REV it saves. The acquire/release forms (LDAPUR/STLUR, LDAR,
  // LDXR) are excluded by name -- their ordering is not this fusion's
  // business.
  struct RevAccess {
    bool Valid {false};
    bool IsLoad {false};
    uint32_t Size {0}; // log2 of the access width: 1, 2 or 3
    uint32_t Rt {0};
    uint32_t Rn {0};
    int64_t Disp {0};
    bool WriteBack {false};
    bool PostIndex {false};
    uint32_t Rm {31}; // 31: no index register (every extend of XZR is zero)
  };

  RevAccess DecodeRevAccess(const Decoder::DecodedInst& Inst) {
    RevAccess A {};
    if (!Inst.Matcher) {
      return A;
    }
    const uint32_t Word = Inst.Word;
    // Every GPR load/store form below is op0=xx1110xx with bit 26 clear
    // ("zz111000..." and "zz111001..."). Checking that first keeps this out
    // of the string comparisons for all but a few percent of instructions:
    // the matcher runs once per decoded instruction at compile time.
    if (Bits(Word, 29, 27) != 0b111 || Bit(Word, 26)) {
      return A;
    }
    const uint32_t Size = Bits(Word, 31, 30);
    const auto Decode = DecodeSizeOpc(Size, Bits(Word, 23, 22));
    // Byte accesses have no byte order, and a sign-extending load does not
    // compose with a byte reverse into one instruction.
    if (Size == 0 || Decode.SignExtend || (Decode.Op != MemOp::Load && Decode.Op != MemOp::Store)) {
      return A;
    }
    A.IsLoad = Decode.Op == MemOp::Load;
    A.Size = Size;
    A.Rt = Bits(Word, 4, 0);
    A.Rn = Bits(Word, 9, 5);

    const char* Name = Inst.Matcher->Name;
    if (strcmp(Name, "STRx_LDRx_imm_2") == 0) {
      A.Disp = static_cast<int64_t>(Bits(Word, 21, 10)) << Size;
      A.Valid = true;
    } else if (strcmp(Name, "STURx_LDURx") == 0) {
      A.Disp = FEXCore::A64::SignExtend(Bits(Word, 20, 12), 9);
      A.Valid = true;
    } else if (strcmp(Name, "STRx_LDRx_imm_1") == 0) {
      // The indexed forms: bits 11:10 are 01 (post) or 11 (pre).
      A.Disp = FEXCore::A64::SignExtend(Bits(Word, 20, 12), 9);
      A.WriteBack = true;
      A.PostIndex = Bits(Word, 11, 10) == 0b01;
      A.Valid = true;
    } else if (strcmp(Name, "LDRx_reg") == 0 || strcmp(Name, "STRx_reg") == 0) {
      // option=011 is LSL/UXTX: the index is the whole 64-bit register. Only
      // the unscaled form, so the address is one add.
      if (Bits(Word, 15, 13) == 0b011 && !Bit(Word, 12)) {
        A.Rm = Bits(Word, 20, 16);
        A.Valid = true;
      }
    }
    return A;
  }

  // Is this instruction the byte reverse that composes with a Size-byte
  // access into a single byte-reversed access?
  bool RevOfWidth(const Decoder::DecodedInst& Rev, uint32_t Size) {
    if (!Rev.Matcher) {
      return false;
    }
    const char* Name = Rev.Matcher->Name;
    if (strcmp(Name, "REV16_int") == 0) {
      // `rev16` swaps the bytes of every halfword, so over a 16-bit field it
      // is exactly bswap16 -- at both operand widths, because the bytes above
      // halfword 0 are zero in a zero-extended halfword load and are not
      // stored by a 16-bit store.
      return Size == 1;
    }
    if (strcmp(Name, "REV") == 0) {
      // sf must equal opc<0> or this is not REV at all (REV() refuses it);
      // then a 32-bit REV composes with a word and a 64-bit REV with a
      // doubleword. A 64-bit REV over a zero-extended word is a different
      // function (the swapped word lands in the upper half), and so is a
      // 32-bit REV over a zero-extended halfword.
      const bool Is64 = Bit(Rev.Word, 31);
      if (Is64 != Bit(Rev.Word, 10)) {
        return false;
      }
      return Size == (Is64 ? 3U : 2U);
    }
    return false;
  }
} // namespace

size_t IRBuilder::TryFuseMemRev(const Decoder::DecodedBlocks& Block, size_t Index) {
  if (Index + 2 > Block.NumInstructions) {
    return 0;
  }
  const auto& First = Block.DecodedInstructions[Index];
  const auto& Second = Block.DecodedInstructions[Index + 1];

  // --- load then REV -------------------------------------------------------
  const RevAccess Load = DecodeRevAccess(First);
  if (Load.Valid && Load.IsLoad && Load.Rt != 31 && RevOfWidth(Second, Load.Size) && Bits(Second.Word, 9, 5) == Load.Rt &&
      Bits(Second.Word, 4, 0) == Load.Rt) {
    // Rt is both operands of the REV, which is what makes the load's own
    // write dead. Rt == 31 is the discarding form and not worth a case.
    const auto MemSize = IR::SizeToOpSize(1U << Load.Size);
    // LoadGPRSlot's value cache is keyed on CurrentPC, which the caller has
    // not advanced yet (TranslateInstruction is not running).
    CurrentPC = First.PC;
    Ref Base = LoadXSP(Load.Rn);
    Ref Address = Base;
    Ref Offsetted = Base;
    if (Load.WriteBack) {
      Offsetted = Load.Disp ? _Add(OpSize::i64Bit, Base, Constant(Load.Disp)) : Base;
      Address = Load.PostIndex ? Base : Offsetted;
    } else if (Load.Rm != 31) {
      Address = _Add(OpSize::i64Bit, Base, LoadX(Load.Rm));
    } else if (Load.Disp) {
      Address = _Add(OpSize::i64Bit, Base, Constant(Load.Disp));
    }
    // TSO=0: a plain AArch64 LDR carries no ordering, exactly as the
    // unfused _LoadMem here does not.
    Ref Value = _LoadMemRev(MemSize, Address, 0);
    if (Load.WriteBack) {
      // The memory access precedes the writeback, so a faulting load leaves
      // Rn as it was; and with Rt == Rn the load's value wins, which is the
      // order LoadStoreImm9 uses.
      StoreXSP(Load.Rn, Offsetted);
    }
    // l{h,w,d}brx zero-extend to 64 bits, which is exactly what a write of
    // the W register means, so the value is stored as it is rather than run
    // through StoreW's extra 32-bit mask.
    StoreX(Load.Rt, Value);
    // Core.cpp emitted the marker for the load; the REV needs its own so a
    // signal resuming at either PC still finds an instruction boundary.
    _GuestOpcode(Second.PC - Entry);
    return 2;
  }

  // --- REV then store ------------------------------------------------------
  const RevAccess Store = DecodeRevAccess(Second);
  if (Store.Valid && !Store.IsLoad && Store.Rt != 31 && RevOfWidth(First, Store.Size) && Bits(First.Word, 4, 0) == Store.Rt) {
    CurrentPC = First.PC;
    // The REV's SOURCE, read before the REV writes anything: `rev x0,x0` is
    // the common shape, and it is the unreversed value the store needs.
    Ref Src = LoadX(Bits(First.Word, 9, 5));
    // The byte reverse itself runs exactly as its own handler emits it.
    const bool RevHandled = strcmp(First.Matcher->Name, "REV16_int") == 0 ? REV16_int(First.Word) : REV(First.Word);
    if (!RevHandled) {
      // Unreachable: RevOfWidth already accepted the encoding. Bail rather
      // than leave a half-translated pair behind.
      return 0;
    }
    // From here the guest PC is the store's, so a faulting store reports it
    // -- with Rd already reversed, which is the architectural state at that
    // point.
    _GuestOpcode(Second.PC - Entry);
    CurrentPC = Second.PC;
    const auto MemSize = IR::SizeToOpSize(1U << Store.Size);
    // The base is read AFTER the REV: in `rev x0,x1 ; str x0,[x0]` the
    // address is the REVERSED x0, which is what the guest does.
    Ref Base = LoadXSP(Store.Rn);
    Ref Address = Base;
    Ref Offsetted = Base;
    if (Store.WriteBack) {
      Offsetted = Store.Disp ? _Add(OpSize::i64Bit, Base, Constant(Store.Disp)) : Base;
      Address = Store.PostIndex ? Base : Offsetted;
    } else if (Store.Rm != 31) {
      Address = _Add(OpSize::i64Bit, Base, LoadX(Store.Rm));
    } else if (Store.Disp) {
      Address = _Add(OpSize::i64Bit, Base, Constant(Store.Disp));
    }
    _StoreMemRev(MemSize, Address, Src, 0);
    if (Store.WriteBack) {
      StoreXSP(Store.Rn, Offsetted);
    }
    return 2;
  }

  return 0;
}

} // namespace FEXCore::A64
