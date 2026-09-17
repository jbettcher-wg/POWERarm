// SPDX-License-Identifier: MIT
//
// A64 (AArch64 guest) block decoder.
//
// Replaces the x86 Frontend::Decoder. The contract with Core.cpp is the one the
// x86 decoder had: DecodeInstructionsAtEntry fills a DecodedBlockInformation
// (blocks, entry points, code pages, decoded address range) that GenerateIR
// walks, and that CompileBlock later uses to register the pages for SMC
// tracking.
//
// A64 is fixed-width, so there is no length decode: an instruction is the
// 32-bit little-endian word at its PC.
//
// Block formation: a compile unit is a small region of guest blocks. Each block
// is decoded linearly and ends after the first instruction that leaves it
// (B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, BR/BLR/RET and the other branch-register
// encodings), after an exception-generating instruction (SVC, BRK, HLT, ...),
// after an instruction the translator does not know (it raises SIGILL), at the
// instruction cap, or before an instruction whose word is not in executable
// memory (that PC gets its own block and SIGSEGV). The targets of B, B.cond,
// CBZ/CBNZ and TBZ/TBNZ that lie near the branch become further blocks of the
// same unit (see DecodeInstructionsAtEntry for the limits), so the branches
// between them, loops included, stay inside the unit. POWERARM_MULTIBLOCK=0 or
// POWERARM_MAXINST=1 gives one block per compile.
#pragma once

#include "Interface/IR/IR.h"

#include <FEXCore/fextl/set.h>
#include <FEXCore/fextl/vector.h>

#include <array>
#include <cstdint>

namespace FEXCore::Context {
class ContextImpl;
}
namespace FEXCore::Core {
struct InternalThreadState;
}

namespace FEXCore::A64 {
constexpr uint64_t INSTRUCTION_SIZE = 4;
// Upper bound on instructions in one block, below the MaxInst config.
constexpr uint64_t DEFAULT_MAX_INSTRUCTIONS = 1024;

class Decoder final {
public:
  enum class DecodedBlockStatus {
    SUCCESS,
    // The instruction word could not be read because the page is not executable.
    NOEXEC_INST,
  };

  struct DecodedInst final {
    uint64_t PC {};
    uint32_t Word {};
  };

  struct DecodedBlocks final {
    uint64_t Entry {};
    uint64_t Size {};
    uint64_t NumInstructions {};
    DecodedInst* DecodedInstructions {};
    DecodedBlockStatus BlockStatus {};
    bool IsEntryPoint {};
    bool ForceFullSMCDetection {};
  };

  struct DecodedBlockInformation final {
    uint64_t TotalInstructionCount {};
    fextl::vector<DecodedBlocks> Blocks;
    fextl::set<uint64_t> EntryPoints;
    fextl::set<uint64_t> CodePages; // Start addresses of all pages touching the block
  };

  explicit Decoder(FEXCore::Core::InternalThreadState* Thread);

  bool CheckIfCacheable(FEXCore::Core::InternalThreadState& Thread, uint64_t PC, uint64_t MaxInst);
  void DecodeInstructionsAtEntry(FEXCore::Core::InternalThreadState* Thread, uint64_t PC, uint64_t MaxInst);

  const DecodedBlockInformation* GetDecodedBlockInfo() const {
    return &BlockInfo;
  }

  uint64_t DecodedMinAddress {};
  uint64_t DecodedMaxAddress {~0ULL};

  void SetExternalBranches(fextl::set<uint64_t>* v) {
    ExternalBranches = v;
  }

  void DelayedDisownBuffer() {}

  void ResetExecutableRangeCache() {
    ExecutableRangeBase = ExecutableRangeEnd = 0;
  }

  bool IsCheapTierBlock() const {
    return false;
  }

private:
  bool CheckRangeExecutable(uint64_t Address, uint64_t Size);

  FEXCore::Core::InternalThreadState* Thread;
  FEXCore::Context::ContextImpl* CTX;

  uint64_t ExecutableRangeBase {};
  uint64_t ExecutableRangeEnd {};

  fextl::vector<DecodedInst> DecodedBuffer;

  // Region discovery scratch (DecodeInstructionsAtEntry), kept to reuse storage.
  fextl::vector<uint32_t> SlotStamp;
  fextl::vector<uint32_t> SlotWord;
  uint32_t Generation {};
  fextl::vector<uint64_t> Leaders;
  fextl::vector<uint64_t> Worklist;

  DecodedBlockInformation BlockInfo;
  fextl::set<uint64_t>* ExternalBranches {nullptr};
};
} // namespace FEXCore::A64
