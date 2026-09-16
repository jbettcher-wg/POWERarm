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

  // POWERARM-M0-TODO(frontend): single-instruction blocks only; multiblock discovery (direct B/B.cond/CBZ/TBZ targets) and a pooled decode buffer come with the M2 translator.
  std::array<DecodedInst, 1> DecodedBuffer {};

  DecodedBlockInformation BlockInfo;
  fextl::set<uint64_t>* ExternalBranches {nullptr};
};
} // namespace FEXCore::A64
