// SPDX-License-Identifier: MIT
#include "Interface/Core/A64Frontend/Decoder.h"
#include "Interface/Context/Context.h"

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

void Decoder::DecodeInstructionsAtEntry(FEXCore::Core::InternalThreadState*, uint64_t PC, uint64_t) {
  FEXCORE_PROFILE_SCOPED("DecodeInstructions");

  BlockInfo.TotalInstructionCount = 0;
  BlockInfo.Blocks.clear();
  BlockInfo.EntryPoints = {PC};
  BlockInfo.CodePages = {PC & FEXCore::Utils::FEX_GUEST_PAGE_MASK};

  DecodedMinAddress = PC;
  DecodedMaxAddress = PC;

  auto& Inst = DecodedBuffer[0];
  Inst.PC = PC;
  Inst.Word = 0;

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
    Block.BlockStatus = DecodedBlockStatus::NOEXEC_INST;
    Block.NumInstructions = 1;
  } else {
    std::memcpy(&Inst.Word, reinterpret_cast<const void*>(PC), sizeof(Inst.Word));
    Block.NumInstructions = 1;
    Block.Size = INSTRUCTION_SIZE;
    DecodedMaxAddress = PC + INSTRUCTION_SIZE;

    const uint64_t LastPage = (PC + INSTRUCTION_SIZE - 1) & FEXCore::Utils::FEX_GUEST_PAGE_MASK;
    BlockInfo.CodePages.insert(LastPage);
  }

  BlockInfo.Blocks.push_back(Block);
  BlockInfo.TotalInstructionCount = Block.NumInstructions;
}

} // namespace FEXCore::A64
