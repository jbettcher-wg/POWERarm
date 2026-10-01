// SPDX-License-Identifier: MIT
#include "Interface/Core/A64Frontend/DecodeTable.h"
#include "Interface/Core/A64Frontend/IRBuilder.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace FEXCore::A64 {
namespace {
  struct RawEntry {
    const char* Name;
    const char* Description;
    const char* Bits;
  };

  constexpr RawEntry RawTable[] = {
#define INST(fn, name, bitstring) {#fn, name, bitstring},
#include "Interface/Core/A64Frontend/a64.inc"
#undef INST
  };

  // The buckets, generated from a64.inc by
  // FEXCore/Scripts/a64_decode_table_generator.py: A64DecodeSlot holds a64.inc
  // entry indices in scan order and A64DecodeBucketStart says where each
  // bucket begins. Parsing 774 bitstrings, sorting them by specificity,
  // hoisting the SIMD modified-immediate entries and filling the buckets is
  // the same work with the same answer in every process, so it happens once at
  // build time and the result is .rodata every guest on the box shares.
#include <FEXCore/A64Frontend/a64-decode-table.inc>

  static_assert(std::size(RawTable) == A64_DECODE_ENTRY_COUNT, "a64-decode-table.inc was generated from a different a64.inc");

  // dynarmic's fast-lookup index: op1 bits [31:22] and [13:10]. The arithmetic
  // used to drop bits 31 (sf) and 30 (the opc bit that separates ADD from SUB,
  // MOVN from MOVZ and ADR from ADRP) despite the comment, which left each of
  // those pairs sharing a bucket.
  inline size_t FastLookupIndex(uint32_t Word) {
    return ((Word >> 10) & 0x00F) | ((Word >> 18) & 0x3FF0);
  }

  struct MaskExpect {
    uint32_t Mask {};
    uint32_t Expect {};
  };

  // '0' and '1' are fixed bits; every other character is an operand field or a
  // don't-care.
  constexpr MaskExpect ParseBits(std::string_view Bits) {
    MaskExpect Out {};
    for (size_t i = 0; i < Bits.size(); ++i) {
      const uint32_t Bit = 1U << (31 - i);
      if (Bits[i] == '0') {
        Out.Mask |= Bit;
      } else if (Bits[i] == '1') {
        Out.Mask |= Bit;
        Out.Expect |= Bit;
      }
    }
    return Out;
  }

  constexpr bool EveryEntryIs32Bits() {
    for (const auto& Entry : RawTable) {
      if (std::string_view {Entry.Bits}.size() != 32) {
        return false;
      }
    }
    return true;
  }
  static_assert(EveryEntryIs32Bits(), "An a64.inc decode table entry is not 32 bits");

  constexpr std::array<MaskExpect, A64_DECODE_ENTRY_COUNT> EntryBits = [] {
    std::array<MaskExpect, A64_DECODE_ENTRY_COUNT> Out {};
    for (size_t i = 0; i < Out.size(); ++i) {
      Out[i] = ParseBits(RawTable[i].Bits);
    }
    return Out;
  }();

  // Mask and Expect are copied out of the matchers so a scan step loads one
  // eight-byte slot and nothing else. They are a pair on their own rather than
  // a triple with A64DecodeSlot's entry index because the stride has to be a
  // power of two: at twelve bytes clang turns the bucket bounds into a
  // magic-number division by 12, which is ~10 instructions on every decode and
  // costs more than the shorter scan saves.
  struct ScanSlot {
    uint32_t Mask;
    uint32_t Expect;
  };

  constexpr std::array<ScanSlot, A64_DECODE_SLOT_COUNT> ScanTable = [] {
    std::array<ScanSlot, A64_DECODE_SLOT_COUNT> Out {};
    for (size_t i = 0; i < Out.size(); ++i) {
      const uint16_t Entry = A64DecodeSlot[i];
      Out[i] = {EntryBits[Entry].Mask, EntryBits[Entry].Expect};
    }
    return Out;
  }();

  struct Table {
    // In a64.inc order, so RawIndex is the position in the array. The one part
    // that cannot be constant: Handler is a pointer-to-member on IRBuilder,
    // which only IRBuilder can resolve from the a64.inc name.
    std::array<InstMatcher, A64_DECODE_ENTRY_COUNT> Matchers {};
    size_t HandledEntries {};
  };

  Table BuildTable() {
    Table T;
    for (size_t Index = 0; Index < T.Matchers.size(); ++Index) {
      auto Handler = IRBuilder::FindHandler(RawTable[Index].Name);
      if (Handler) {
        ++T.HandledEntries;
      }
      T.Matchers[Index] = {RawTable[Index].Name, EntryBits[Index].Mask, EntryBits[Index].Expect, Handler, static_cast<uint32_t>(Index)};
    }
    return T;
  }

  const Table& GetTable() {
    static const Table T = BuildTable();
    return T;
  }
} // namespace

const InstMatcher* DecodeInstruction(uint32_t Word) {
  const size_t Index = FastLookupIndex(Word);
  const size_t End = A64DecodeBucketStart[Index + 1];
  for (size_t Slot = A64DecodeBucketStart[Index]; Slot != End; ++Slot) {
    if ((Word & ScanTable[Slot].Mask) == ScanTable[Slot].Expect) {
      return &GetTable().Matchers[A64DecodeSlot[Slot]];
    }
  }
  return nullptr;
}

DecodeTableStats GetDecodeTableStats() {
  const auto& T = GetTable();
  return {T.Matchers.size(), T.HandledEntries};
}

void ForEachTableEntry(void* Opaque, void (*Visit)(void*, const InstMatcher&)) {
  // a64.inc order. Exists for the NZCV peek's table check (NZCVPeek.cpp),
  // which has to synthesise words for every entry with a handler and compare
  // the peek's classification with what the frontend actually translates the
  // word into; it re-decodes each word to find the entry that really wins, so
  // the order it is handed them in does not matter.
  for (const auto& M : GetTable().Matchers) {
    Visit(Opaque, M);
  }
}

} // namespace FEXCore::A64
