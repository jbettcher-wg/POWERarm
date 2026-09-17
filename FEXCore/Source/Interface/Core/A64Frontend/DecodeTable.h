// SPDX-License-Identifier: MIT
//
// A64 instruction decode table.
//
// The encodings come verbatim from dynarmic's A64 decoder table (a64.inc in
// this directory, 0BSD, see THIRD_PARTY.md). Each INST(name, description,
// bitstring) entry becomes a mask/expect matcher; '0'/'1' are fixed bits and
// every other character is an operand field or a don't-care. Matchers are
// ordered the way dynarmic orders them (more fixed bits first, stable, then
// the SIMD modified-immediate entries hoisted to the front) and
// bucketed by dynarmic's fast-lookup index.
//
// A matcher's handler is the IRBuilder member function registered under the
// same name (IRBuilder::HandlerTable). Entries with no registered handler, and
// words that match no entry at all, are unimplemented and raise SIGILL.
#pragma once

#include <cstddef>
#include <cstdint>

namespace FEXCore::A64 {
class IRBuilder;

using InstHandler = bool (IRBuilder::*)(uint32_t Word);

struct InstMatcher final {
  const char* Name;
  uint32_t Mask;
  uint32_t Expect;
  InstHandler Handler;
  uint32_t RawIndex; ///< Position in a64.inc.
};

// Returns the first matcher for Word, or nullptr if no table entry matches.
const InstMatcher* DecodeInstruction(uint32_t Word);

struct DecodeTableStats final {
  size_t Entries;        ///< Active INST entries in a64.inc.
  size_t HandledEntries; ///< Entries with a registered translator.
};
DecodeTableStats GetDecodeTableStats();

} // namespace FEXCore::A64
