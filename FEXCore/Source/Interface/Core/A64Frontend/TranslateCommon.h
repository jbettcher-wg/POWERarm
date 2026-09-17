// SPDX-License-Identifier: MIT
//
// Field extraction and immediate decoding shared by the A64 translators.
#pragma once

#include "Interface/IR/IR.h"

#include <cstdint>

namespace FEXCore::A64 {

// Bits [Hi:Lo] of Word, zero-extended.
constexpr uint64_t Bits(uint32_t Word, unsigned Hi, unsigned Lo) {
  return (Word >> Lo) & ((1ULL << (Hi - Lo + 1)) - 1);
}

constexpr bool Bit(uint32_t Word, unsigned Index) {
  return (Word >> Index) & 1;
}

// Sign-extends the low Width bits of Value.
constexpr int64_t SignExtend(uint64_t Value, unsigned Width) {
  const unsigned Shift = 64 - Width;
  return static_cast<int64_t>(Value << Shift) >> Shift;
}

// Arm pseudocode DecodeBitMasks. Returns false for the reserved encodings
// (the caller raises SIGILL). WMask/TMask may be null when unused.
inline bool DecodeBitMasks(bool ImmN, uint32_t ImmS, uint32_t ImmR, bool Immediate, unsigned DataSize, uint64_t* WMask, uint64_t* TMask) {
  const uint32_t Combined = (static_cast<uint32_t>(ImmN) << 6) | (~ImmS & 0x3F);
  if (Combined == 0) {
    return false;
  }
  const unsigned Len = 31 - __builtin_clz(Combined);
  if (Len < 1) {
    return false;
  }
  const uint32_t Levels = (1U << Len) - 1;
  if (Immediate && (ImmS & Levels) == Levels) {
    return false;
  }

  const uint32_t S = ImmS & Levels;
  const uint32_t R = ImmR & Levels;
  const uint32_t Diff = (S - R) & 0x3F;
  const unsigned ESize = 1U << Len;
  const uint32_t D = Diff & Levels;

  auto Ones = [](unsigned N) -> uint64_t {
    return N >= 64 ? ~0ULL : (1ULL << N) - 1;
  };
  auto RotateRight = [&](uint64_t Value, unsigned Amount) -> uint64_t {
    const uint64_t Mask = Ones(ESize);
    Value &= Mask;
    Amount %= ESize;
    if (Amount == 0) {
      return Value;
    }
    return ((Value >> Amount) | (Value << (ESize - Amount))) & Mask;
  };
  auto Replicate = [&](uint64_t Element) -> uint64_t {
    uint64_t Result = 0;
    for (unsigned i = 0; i < DataSize; i += ESize) {
      Result |= Element << i;
    }
    return Result & Ones(DataSize);
  };

  if (WMask) {
    *WMask = Replicate(RotateRight(Ones(S + 1), R));
  }
  if (TMask) {
    *TMask = Replicate(Ones(D + 1));
  }
  return true;
}

// A64 condition code (0-13) -> IR NZCV condition. AL/NV are handled by callers.
inline FEXCore::IR::CondClass MapCondition(uint32_t Cond) {
  using FEXCore::IR::CondClass;
  switch (Cond) {
  case 0x0: return CondClass::EQ;
  case 0x1: return CondClass::NEQ;
  case 0x2: return CondClass::UGE; // CS: C set
  case 0x3: return CondClass::ULT; // CC: C clear
  case 0x4: return CondClass::MI;
  case 0x5: return CondClass::PL;
  case 0x6: return CondClass::VS;
  case 0x7: return CondClass::VC;
  case 0x8: return CondClass::UGT; // HI: C set and Z clear
  case 0x9: return CondClass::ULE; // LS: C clear or Z set
  case 0xA: return CondClass::SGE;
  case 0xB: return CondClass::SLT;
  case 0xC: return CondClass::SGT;
  case 0xD: return CondClass::SLE;
  default: return CondClass::AL;
  }
}

} // namespace FEXCore::A64
