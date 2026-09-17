// SPDX-License-Identifier: MIT
#include "Interface/Core/A64Frontend/DecodeTable.h"
#include "Interface/Core/A64Frontend/IRBuilder.h"

#include <FEXCore/Utils/LogManager.h>

#include <algorithm>
#include <array>
#include <bit>
#include <string_view>
#include <vector>

namespace FEXCore::A64 {
namespace {
  struct RawEntry {
    const char* Name;
    const char* Bits;
  };

  constexpr RawEntry RawTable[] = {
#define INST(fn, name, bitstring) {#fn, bitstring},
#include "Interface/Core/A64Frontend/a64.inc"
#undef INST
  };

  // dynarmic's fast-lookup index: op1 bits [31:22] and [13:10].
  inline size_t FastLookupIndex(uint32_t Word) {
    return ((Word >> 10) & 0x00F) | ((Word >> 18) & 0xFF0);
  }

  struct Table {
    std::vector<InstMatcher> Matchers;
    std::array<std::vector<const InstMatcher*>, 0x1000> Buckets;
    size_t HandledEntries {};
  };

  Table BuildTable() {
    Table T;
    T.Matchers.reserve(std::size(RawTable));

    for (const auto& Entry : RawTable) {
      std::string_view Bits {Entry.Bits};
      LOGMAN_THROW_A_FMT(Bits.size() == 32, "A64 decode table entry {} is not 32 bits", Entry.Name);

      uint32_t Mask {};
      uint32_t Expect {};
      for (size_t i = 0; i < 32; ++i) {
        const uint32_t Bit = 1U << (31 - i);
        if (Bits[i] == '0') {
          Mask |= Bit;
        } else if (Bits[i] == '1') {
          Mask |= Bit;
          Expect |= Bit;
        }
      }

      auto Handler = IRBuilder::FindHandler(Entry.Name);
      if (Handler) {
        ++T.HandledEntries;
      }
      T.Matchers.push_back({Entry.Name, Mask, Expect, Handler});
    }

    // More fixed bits is more specific, so it wins. dynarmic additionally
    // hoists three SIMD modified-immediate entries; none of them has a
    // translator here, so their relative order does not change any result.
    std::stable_sort(T.Matchers.begin(), T.Matchers.end(), [](const InstMatcher& A, const InstMatcher& B) {
      return std::popcount(A.Mask) > std::popcount(B.Mask);
    });

    for (size_t i = 0; i < T.Buckets.size(); ++i) {
      for (const auto& M : T.Matchers) {
        if ((i & FastLookupIndex(M.Mask)) == FastLookupIndex(M.Expect)) {
          T.Buckets[i].push_back(&M);
        }
      }
    }
    return T;
  }

  const Table& GetTable() {
    static const Table T = BuildTable();
    return T;
  }
} // namespace

const InstMatcher* DecodeInstruction(uint32_t Word) {
  const auto& Bucket = GetTable().Buckets[FastLookupIndex(Word)];
  for (const auto* M : Bucket) {
    if ((Word & M->Mask) == M->Expect) {
      return M;
    }
  }
  return nullptr;
}

DecodeTableStats GetDecodeTableStats() {
  const auto& T = GetTable();
  return {T.Matchers.size(), T.HandledEntries};
}

} // namespace FEXCore::A64
