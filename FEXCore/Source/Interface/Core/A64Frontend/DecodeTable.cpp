// SPDX-License-Identifier: MIT
#include "Interface/Core/A64Frontend/DecodeTable.h"
#include "Interface/Core/A64Frontend/HandlerTable.h"
#include "Interface/Core/A64Frontend/IRBuilder.h"

#include <algorithm>
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

  // HandlerTable sorted by name, as indices into it. The name is the only key,
  // and ties break on the table position so that the first entry for a name
  // wins -- std::sort is constexpr in C++20 and std::stable_sort is not, and
  // the runtime lookup this replaces was a stable sort followed by a
  // lower_bound.
  constexpr std::array<uint16_t, std::size(HandlerTable)> HandlerOrder = [] {
    std::array<uint16_t, std::size(HandlerTable)> Out {};
    for (size_t i = 0; i < Out.size(); ++i) {
      Out[i] = static_cast<uint16_t>(i);
    }
    std::sort(Out.begin(), Out.end(), [](uint16_t A, uint16_t B) {
      if (HandlerTable[A].Name != HandlerTable[B].Name) {
        return HandlerTable[A].Name < HandlerTable[B].Name;
      }
      return A < B;
    });
    return Out;
  }();

  constexpr InstHandler FindHandler(std::string_view Name) {
    const auto It = std::lower_bound(HandlerOrder.begin(), HandlerOrder.end(), Name,
                                     [](uint16_t A, std::string_view N) { return HandlerTable[A].Name < N; });
    if (It != HandlerOrder.end() && HandlerTable[*It].Name == Name) {
      return HandlerTable[*It].Handler;
    }
    return nullptr;
  }

  // In a64.inc order, so RawIndex is the position in the array. Handler is a
  // pointer-to-member on IRBuilder, which the build-time generator cannot form
  // from a name, so the table is resolved here instead -- but in a constant
  // expression, not once per process. It used to be 774 binary searches over
  // 736 cold-cache names in every process that translates anything, measured
  // at ~1.2 M instructions:u and ~0.5 ms per process.
  //
  // Being constant also takes RawTable out of the runtime image: its
  // descriptions and bitstrings were .rodata and its 2322 pointers were
  // dynamic relocations purely so that BuildTable could read them.
  constexpr std::array<InstMatcher, A64_DECODE_ENTRY_COUNT> Matchers = [] {
    std::array<InstMatcher, A64_DECODE_ENTRY_COUNT> Out {};
    for (size_t Index = 0; Index < Out.size(); ++Index) {
      Out[Index] = {RawTable[Index].Name, EntryBits[Index].Mask, EntryBits[Index].Expect, FindHandler(RawTable[Index].Name),
                    static_cast<uint32_t>(Index)};
    }
    return Out;
  }();

  constexpr size_t HandledEntries = [] {
    size_t Count = 0;
    for (const auto& M : Matchers) {
      if (M.Handler) {
        ++Count;
      }
    }
    return Count;
  }();
} // namespace

const InstMatcher* DecodeInstruction(uint32_t Word) {
  const size_t Index = FastLookupIndex(Word);
  const size_t End = A64DecodeBucketStart[Index + 1];
  for (size_t Slot = A64DecodeBucketStart[Index]; Slot != End; ++Slot) {
    if ((Word & ScanTable[Slot].Mask) == ScanTable[Slot].Expect) {
      return &Matchers[A64DecodeSlot[Slot]];
    }
  }
  return nullptr;
}

DecodeTableStats GetDecodeTableStats() {
  return {Matchers.size(), HandledEntries};
}

void ForEachTableEntry(void* Opaque, void (*Visit)(void*, const InstMatcher&)) {
  // a64.inc order. Exists for the NZCV peek's table check (NZCVPeek.cpp),
  // which has to synthesise words for every entry with a handler and compare
  // the peek's classification with what the frontend actually translates the
  // word into; it re-decodes each word to find the entry that really wins, so
  // the order it is handed them in does not matter.
  for (const auto& M : Matchers) {
    Visit(Opaque, M);
  }
}

} // namespace FEXCore::A64
