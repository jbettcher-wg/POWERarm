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
    const char* Description;
    const char* Bits;
  };

  constexpr RawEntry RawTable[] = {
#define INST(fn, name, bitstring) {#fn, name, bitstring},
#include "Interface/Core/A64Frontend/a64.inc"
#undef INST
  };

  // dynarmic's fast-lookup index: op1 bits [31:22] and [13:10].
  inline size_t FastLookupIndex(uint32_t Word) {
    return ((Word >> 10) & 0x00F) | ((Word >> 18) & 0xFF0);
  }

  struct Table {
    std::vector<InstMatcher> Matchers;
    // Buckets in CSR form: bucket i is BucketEntries[BucketStart[i] ..
    // BucketStart[i + 1]), in matcher priority order.
    std::array<uint32_t, 0x1001> BucketStart {};
    // Mask and Expect are copied next to the matcher pointer so the bucket
    // scan does not load each candidate matcher.
    struct BucketEntry {
      uint32_t Mask;
      uint32_t Expect;
      const InstMatcher* Matcher;
    };
    std::vector<BucketEntry> BucketEntries;
    size_t HandledEntries {};
  };

  // Calls Visit(i) for every 12-bit fast-lookup index i with
  // (i & FastLookupIndex(Mask)) == FastLookupIndex(Expect), i.e. every bucket
  // the matcher can be reached from. Enumerating the free index bits visits
  // exactly those buckets; testing all 4096 per matcher cost most of the table
  // build, which runs at every process start.
  template<typename F>
  void ForEachBucket(const InstMatcher& M, F&& Visit) {
    const uint32_t Fixed = FastLookupIndex(M.Mask);
    const uint32_t Want = FastLookupIndex(M.Expect);
    const uint32_t Free = ~Fixed & 0xFFF;
    uint32_t Sub = Free;
    while (true) {
      Visit(Want | Sub);
      if (Sub == 0) {
        break;
      }
      Sub = (Sub - 1) & Free;
    }
  }

  Table BuildTable() {
    Table T;
    T.Matchers.reserve(std::size(RawTable));

    for (size_t Index = 0; Index < std::size(RawTable); ++Index) {
      const auto& Entry = RawTable[Index];
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
      T.Matchers.push_back({Entry.Name, Mask, Expect, Handler, static_cast<uint32_t>(Index)});
    }

    // More fixed bits is more specific, so it wins.
    std::stable_sort(T.Matchers.begin(), T.Matchers.end(), [](const InstMatcher& A, const InstMatcher& B) {
      return std::popcount(A.Mask) > std::popcount(B.Mask);
    });

    // dynarmic's exceptions, by description: the SIMD modified-immediate
    // entries come before everything else. Without this MOVI/MVNI/BIC
    // (vector) words decode as the shift-by-immediate entries whose
    // immh=0000 space they occupy.
    auto ComesFirst = [](const InstMatcher& M) {
      const std::string_view D {RawTable[M.RawIndex].Description};
      return D == "MOVI, MVNI, ORR, BIC (vector, immediate)" || D == "FMOV (vector, immediate)" || D == "Unallocated SIMD modified immediate";
    };
    std::stable_partition(T.Matchers.begin(), T.Matchers.end(), ComesFirst);

    // Count, then fill in matcher order so each bucket keeps priority order.
    for (const auto& M : T.Matchers) {
      ForEachBucket(M, [&](uint32_t i) { ++T.BucketStart[i + 1]; });
    }
    for (size_t i = 1; i < T.BucketStart.size(); ++i) {
      T.BucketStart[i] += T.BucketStart[i - 1];
    }
    T.BucketEntries.resize(T.BucketStart.back());
    std::array<uint32_t, 0x1000> Cursor {};
    std::copy_n(T.BucketStart.begin(), Cursor.size(), Cursor.begin());
    for (const auto& M : T.Matchers) {
      ForEachBucket(M, [&](uint32_t i) { T.BucketEntries[Cursor[i]++] = {M.Mask, M.Expect, &M}; });
    }
    return T;
  }

  const Table& GetTable() {
    static const Table T = BuildTable();
    return T;
  }
} // namespace

const InstMatcher* DecodeInstruction(uint32_t Word) {
  const auto& T = GetTable();
  const size_t Index = FastLookupIndex(Word);
  const auto* const End = T.BucketEntries.data() + T.BucketStart[Index + 1];
  for (auto* It = T.BucketEntries.data() + T.BucketStart[Index]; It != End; ++It) {
    if ((Word & It->Mask) == It->Expect) {
      return It->Matcher;
    }
  }
  return nullptr;
}

DecodeTableStats GetDecodeTableStats() {
  const auto& T = GetTable();
  return {T.Matchers.size(), T.HandledEntries};
}

} // namespace FEXCore::A64
