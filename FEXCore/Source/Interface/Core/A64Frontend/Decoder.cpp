// SPDX-License-Identifier: MIT
#include "Interface/Core/A64Frontend/Decoder.h"
#include "Interface/Context/Context.h"
#include "Interface/Core/A64Frontend/DecodeTable.h"

#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Profiler.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <algorithm>
#include <cstring>

namespace FEXCore::A64 {

Decoder::Decoder(FEXCore::Core::InternalThreadState* Thread)
  : Thread {Thread}
  , CTX {static_cast<FEXCore::Context::ContextImpl*>(Thread->CTX)} {}

bool Decoder::CheckRangeExecutable(uint64_t Address, uint64_t Size) {
  if (!CTX->SyscallHandler) {
    // Compile-only tools (no syscall layer) treat all memory as executable.
    return true;
  }

  while (Address < ExecutableRangeBase || Address + Size > ExecutableRangeEnd) {
    auto RangeInfo = CTX->SyscallHandler->QueryGuestExecutableRange(Thread, Address);
    ExecutableRangeBase = RangeInfo.Base;
    ExecutableRangeEnd = RangeInfo.Base + RangeInfo.Size;

    if (RangeInfo.Size == 0) {
      return false;
    }

    uint64_t RangeRemainingSize = ExecutableRangeEnd - Address;
    if (Size > RangeRemainingSize) {
      Size -= RangeRemainingSize;
      Address += RangeRemainingSize;
    }
  }

  return true;
}

bool Decoder::CheckIfCacheable(FEXCore::Core::InternalThreadState& Thread, uint64_t PC, uint64_t MaxInst) {
  DecodeInstructionsAtEntry(&Thread, PC, MaxInst);
  return true;
}

// True if Word ends a block: it transfers control, or it is an
// exception-generating instruction. See the Decoder.h block comment.
static bool EndsBlock(uint32_t Word) {
  // Unconditional branch (immediate): B, BL.
  if ((Word & 0x7C000000) == 0x14000000) {
    return true;
  }
  // Compare and branch (CBZ/CBNZ) and test and branch (TBZ/TBNZ).
  if ((Word & 0x7C000000) == 0x34000000) {
    return true;
  }
  // Conditional branch (B.cond, BC.cond).
  if ((Word & 0xFE000000) == 0x54000000) {
    return true;
  }
  // Exception generation (SVC, HVC, SMC, BRK, HLT, DCPSn).
  if ((Word & 0xFF000000) == 0xD4000000) {
    return true;
  }
  // Unconditional branch (register): BR, BLR, RET, ERET, DRPS and the PAC forms.
  if ((Word & 0xFE000000) == 0xD6000000) {
    return true;
  }
  return false;
}

// Direct successors of a block-ending instruction that the decoder may follow
// when it grows a compile unit. Calls (BL) are not followed (their continuation
// is reached through a return), and neither are register branches or
// exception-generating instructions.
struct DirectSuccessors {
  uint64_t Target[2];
  uint32_t Count;
};
static DirectSuccessors GetDirectSuccessors(uint32_t Word, uint64_t PC) {
  DirectSuccessors S {{}, 0};
  auto SignExtendImm = [](uint64_t Value, unsigned Width) {
    return static_cast<int64_t>(Value << (64 - Width)) >> (64 - Width);
  };
  if ((Word & 0xFC000000) == 0x14000000) { // B
    S.Target[S.Count++] = PC + SignExtendImm(Word & 0x03FFFFFF, 26) * 4;
  } else if ((Word & 0x7E000000) == 0x34000000 || (Word & 0x7E000000) == 0x36000000) {
    // CBZ/CBNZ (imm19) and TBZ/TBNZ (imm14).
    const bool IsTest = (Word & 0x7E000000) == 0x36000000;
    S.Target[S.Count++] = PC + (IsTest ? SignExtendImm((Word >> 5) & 0x3FFF, 14) : SignExtendImm((Word >> 5) & 0x7FFFF, 19)) * 4;
    S.Target[S.Count++] = PC + INSTRUCTION_SIZE;
  } else if ((Word & 0xFE000000) == 0x54000000) { // B.cond, BC.cond
    S.Target[S.Count++] = PC + SignExtendImm((Word >> 5) & 0x7FFFF, 19) * 4;
    if ((Word & 0xF) < 0xE) {
      S.Target[S.Count++] = PC + INSTRUCTION_SIZE;
    }
  }
  return S;
}

void Decoder::DecodeInstructionsAtEntry(FEXCore::Core::InternalThreadState*, uint64_t PC, uint64_t MaxInst) {
  FEXCORE_PROFILE_SCOPED("DecodeInstructions");

  BlockInfo.TotalInstructionCount = 0;
  BlockInfo.Blocks.clear();
  BlockInfo.EntryPoints = {PC};
  BlockInfo.CodePages = {PC & FEXCore::Utils::FEX_GUEST_PAGE_MASK};

  DecodedMinAddress = PC;
  DecodedMaxAddress = PC;

  uint64_t Cap = MaxInst ? MaxInst : static_cast<uint64_t>(CTX->Config.MaxInstPerBlock());
  if (Cap == 0 || Cap > DEFAULT_MAX_INSTRUCTIONS) {
    Cap = DEFAULT_MAX_INSTRUCTIONS;
  }
  if (DecodedBuffer.size() < Cap) {
    DecodedBuffer.resize(Cap);
  }

  if (!CheckRangeExecutable(PC, INSTRUCTION_SIZE)) {
    // Emitted as a guest SIGSEGV at PC by the IR builder, exactly like the x86 decoder's NOEXEC_INST.
    DecodedBuffer[0] = {.PC = PC, .Word = 0, .Matcher = DecodeInstruction(0)};
    BlockInfo.Blocks.push_back(DecodedBlocks {
      .Entry = PC,
      .Size = 0,
      .NumInstructions = 1,
      .DecodedInstructions = DecodedBuffer.data(),
      .BlockStatus = DecodedBlockStatus::NOEXEC_INST,
      .IsEntryPoint = true,
    });
    BlockInfo.TotalInstructionCount = 1;
    return;
  }

  // Region discovery. Decode linearly from the entry to the first block-ending
  // instruction; when that instruction is a direct branch with targets inside
  // the region window, decode those as well. The entry and every followed
  // target are block leaders. A leader inside an already decoded run splits
  // it in the layout pass below.
  //
  // The window is small on purpose: every instruction in a region is translated
  // whether it runs or not, and a block that is also entered from outside the
  // region is translated again under its own entry. Measured on the M2 zlib and
  // Lua builds (cc1 lvm.c / gcc -c empty.c / zlib / Lua, seconds):
  //   single block             12.2 / 0.40 / 134.6 / 118.7
  //   16 KiB, 64 leaders       10.1 / 0.55 /     - /     -
  //   256 bytes, 16 leaders     9.2 / 0.42 / 124.3 / 107.5
  //   128 bytes, 8 leaders      9.4 / 0.40 / 123.3 / 106.7
  //   64 bytes, 8 leaders       9.4 / 0.39 / 121.8 / 106.4
  // The window bounds the distance from the branch to its target.
  constexpr uint64_t RegionWindow = 128;
  constexpr size_t MaxLeaders = 8;
  const bool FollowBranches = CTX->Config.Multiblock() && Cap > 1 && MaxLeaders > 1;
  // Slot range: a region may reach RegionWindow below the entry, and a linear
  // run from the entry is never cut short by the window (it is bounded by Cap).
  const uint64_t WindowLow = PC >= RegionWindow ? PC - RegionWindow : 0;
  const uint64_t WindowHigh = PC + Cap * INSTRUCTION_SIZE + RegionWindow;

  // Per-slot state for the window, indexed by (PC - WindowLow) / 4. A slot is
  // valid when its stamp equals the current generation, so nothing needs
  // clearing between compiles.
  const size_t NumSlots = (WindowHigh - WindowLow) / INSTRUCTION_SIZE + 1;
  if (SlotStamp.size() < NumSlots) {
    SlotStamp.assign(NumSlots, 0);
    SlotWord.resize(NumSlots);
    SlotMatcher.resize(NumSlots);
    Generation = 0;
  }
  if (++Generation == 0) {
    std::fill(SlotStamp.begin(), SlotStamp.end(), 0);
    Generation = 1;
  }
  const uint32_t DecodedBit = 1u << 31;
  const uint32_t LeaderBit = 1u << 30;
  // Set on a decoded slot whose instruction ends its run (no handler, or
  // EndsBlock), so the layout pass needs no second decode.
  const uint32_t StopBit = 1u << 29;
  const uint32_t GenMask = StopBit - 1;
  const uint32_t Gen = Generation & GenMask;
  auto SlotOf = [&](uint64_t Addr) -> size_t {
    return (Addr - WindowLow) / INSTRUCTION_SIZE;
  };
  auto InWindow = [&](uint64_t Addr) {
    return Addr >= WindowLow && Addr <= WindowHigh && (Addr & (INSTRUCTION_SIZE - 1)) == 0;
  };
  auto Flags = [&](uint64_t Addr) -> uint32_t {
    const uint32_t S = SlotStamp[SlotOf(Addr)];
    return (S & GenMask) == Gen ? (S & ~GenMask) : 0;
  };
  auto SetFlag = [&](uint64_t Addr, uint32_t Flag) {
    auto& S = SlotStamp[SlotOf(Addr)];
    S = ((S & GenMask) == Gen ? S : Gen) | Flag;
  };

  Leaders.clear();
  Worklist.clear();
  Leaders.push_back(PC);
  SetFlag(PC, LeaderBit);
  Worklist.push_back(PC);
  uint64_t Decoded = 0;

  while (!Worklist.empty() && Decoded < Cap) {
    uint64_t InstPC = Worklist.back();
    Worklist.pop_back();

    while (Decoded < Cap) {
      // A run that leaves the slot range, or reaches decoded code, stops here:
      // the layout pass exits to (or falls into) whatever follows.
      if (!InWindow(InstPC) || (Flags(InstPC) & DecodedBit)) {
        break;
      }
      if (InstPC != PC && !CheckRangeExecutable(InstPC, INSTRUCTION_SIZE)) {
        break;
      }
      uint32_t Word;
      std::memcpy(&Word, reinterpret_cast<const void*>(InstPC), sizeof(Word));
      SlotWord[SlotOf(InstPC)] = Word;
      SetFlag(InstPC, DecodedBit);
      ++Decoded;

      const auto* Matcher = DecodeInstruction(Word);
      if (Word == THUNK_MARKER_WORD && !CheckRangeExecutable(InstPC + INSTRUCTION_SIZE, THUNK_HASH_SIZE)) {
        // A marker whose hash cannot be read is not a thunk: plain HLT, SIGILL.
        Matcher = nullptr;
      }
      SlotMatcher[SlotOf(InstPC)] = Matcher;
      if (!Matcher || !Matcher->Handler) {
        SetFlag(InstPC, StopBit);
        break;
      }
      if (EndsBlock(Word)) {
        SetFlag(InstPC, StopBit);
        if (FollowBranches) {
          const auto Succ = GetDirectSuccessors(Word, InstPC);
          for (uint32_t i = 0; i < Succ.Count; ++i) {
            const uint64_t T = Succ.Target[i];
            const uint64_t Distance = T > InstPC ? T - InstPC : InstPC - T;
            if (Distance > RegionWindow || !InWindow(T) || (Flags(T) & LeaderBit) || Leaders.size() >= MaxLeaders) {
              continue;
            }
            Leaders.push_back(T);
            SetFlag(T, LeaderBit);
            Worklist.push_back(T);
          }
        }
        break;
      }
      InstPC += INSTRUCTION_SIZE;
    }
  }

  // Layout: the entry block first (the IR header points at it), then the other
  // leaders in address order, so that a block's fallthrough successor is usually
  // the next block emitted. A leader whose first word was never decoded (cap
  // reached, or not executable) gets no block, and branches to it exit.
  if (DecodedBuffer.size() < Decoded) {
    DecodedBuffer.resize(Decoded);
  }
  std::sort(Leaders.begin() + 1, Leaders.end());
  size_t Used = 0;
  uint64_t LastPage = PC & FEXCore::Utils::FEX_GUEST_PAGE_MASK;
  for (uint64_t Leader : Leaders) {
    if (!(Flags(Leader) & DecodedBit)) {
      continue;
    }
    DecodedBlocks Block {
      .Entry = Leader,
      .Size = 0,
      .NumInstructions = 0,
      .DecodedInstructions = DecodedBuffer.data() + Used,
      .BlockStatus = DecodedBlockStatus::SUCCESS,
      .IsEntryPoint = Leader == PC,
    };
    uint64_t InstPC = Leader;
    uint64_t DecodedEnd = Leader;
    while (InWindow(InstPC)) {
      const uint32_t F = Flags(InstPC);
      if (!(F & DecodedBit) || (InstPC != Leader && (F & LeaderBit))) {
        break;
      }
      const size_t Slot = SlotOf(InstPC);
      DecodedBuffer[Used++] = {.PC = InstPC, .Word = SlotWord[Slot], .Matcher = SlotMatcher[Slot]};
      ++Block.NumInstructions;
      Block.Size += INSTRUCTION_SIZE;
      // CodePages is a set: insert only on a page change (the set already
      // holds the entry page and every page an earlier block touched).
      const uint64_t Page = InstPC & FEXCore::Utils::FEX_GUEST_PAGE_MASK;
      if (Page != LastPage) {
        BlockInfo.CodePages.insert(Page);
        LastPage = Page;
      }
      InstPC += INSTRUCTION_SIZE;
      DecodedEnd = InstPC;
      if (SlotWord[Slot] == THUNK_MARKER_WORD && SlotMatcher[Slot]) {
        // The translation embeds the hash that follows the marker, so SMC
        // tracking must cover those bytes too. The marker ends the block.
        DecodedEnd = InstPC + THUNK_HASH_SIZE;
        const uint64_t HashPage = (DecodedEnd - 1) & FEXCore::Utils::FEX_GUEST_PAGE_MASK;
        if (HashPage != LastPage) {
          BlockInfo.CodePages.insert(HashPage);
          LastPage = HashPage;
        }
      }
      if (F & StopBit) {
        break;
      }
    }
    DecodedMinAddress = std::min(DecodedMinAddress, Leader);
    DecodedMaxAddress = std::max(DecodedMaxAddress, DecodedEnd);
    BlockInfo.TotalInstructionCount += Block.NumInstructions;
    BlockInfo.Blocks.push_back(Block);
  }
}

} // namespace FEXCore::A64
