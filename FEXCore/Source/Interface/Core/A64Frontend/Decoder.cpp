// SPDX-License-Identifier: MIT
#include "Interface/Core/A64Frontend/Decoder.h"
#include "Interface/Context/Context.h"
#include "Interface/Core/A64Frontend/DecodeTable.h"

#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Profiler.h>
#include <FEXCore/Utils/TypeDefines.h>

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

  DecodedBlocks Block {
    .Entry = PC,
    .Size = 0,
    .NumInstructions = 0,
    .DecodedInstructions = DecodedBuffer.data(),
    .BlockStatus = DecodedBlockStatus::SUCCESS,
    .IsEntryPoint = true,
  };

  if (!CheckRangeExecutable(PC, INSTRUCTION_SIZE)) {
    // Emitted as a guest SIGSEGV at PC by the IR builder, exactly like the x86 decoder's NOEXEC_INST.
    DecodedBuffer[0] = {.PC = PC, .Word = 0};
    Block.BlockStatus = DecodedBlockStatus::NOEXEC_INST;
    Block.NumInstructions = 1;
  } else {
    uint64_t InstPC = PC;
    while (Block.NumInstructions < Cap) {
      if (Block.NumInstructions != 0 && !CheckRangeExecutable(InstPC, INSTRUCTION_SIZE)) {
        // The next word is not executable; it becomes the entry of its own block.
        break;
      }

      auto& Inst = DecodedBuffer[Block.NumInstructions];
      Inst.PC = InstPC;
      std::memcpy(&Inst.Word, reinterpret_cast<const void*>(InstPC), sizeof(Inst.Word));
      ++Block.NumInstructions;
      Block.Size += INSTRUCTION_SIZE;

      BlockInfo.CodePages.insert(InstPC & FEXCore::Utils::FEX_GUEST_PAGE_MASK);
      InstPC += INSTRUCTION_SIZE;

      const auto* Matcher = DecodeInstruction(Inst.Word);
      if (!Matcher || !Matcher->Handler || EndsBlock(Inst.Word)) {
        break;
      }
    }
    DecodedMaxAddress = InstPC;
  }

  BlockInfo.Blocks.push_back(Block);
  BlockInfo.TotalInstructionCount = Block.NumInstructions;
}

} // namespace FEXCore::A64
