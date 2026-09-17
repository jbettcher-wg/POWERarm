// SPDX-License-Identifier: MIT
//
// A64 guest IR builder.
//
// Replaces the x86 OpDispatchBuilder as the IREmitter subclass a thread's
// compiler drives. It owns the per-compilation block bookkeeping (one IR code
// block per decoded guest block, entry/exit plumbing) and, from M2 on, the
// per-instruction A64 translators.
//
// M0: there are no translators. Every instruction becomes a guest SIGILL
// (ILL_ILLOPC) at its PC, delivered through the backend's Break path.
#pragma once

#include "Interface/Core/A64Frontend/Decoder.h"
#include "Interface/IR/IR.h"
#include "Interface/IR/IREmitter.h"

#include <FEXCore/fextl/map.h>
#include <FEXCore/fextl/vector.h>

#include <cstdint>

namespace FEXCore::Context {
class ContextImpl;
}

namespace FEXCore::A64 {

class IRBuilder final : public FEXCore::IR::IREmitter {
public:
  using Ref = FEXCore::IR::Ref;

  explicit IRBuilder(FEXCore::Context::ContextImpl* ctx);

  // Should only be called at the start of IR emission.
  void ResetWorkingList();

  void BeginFunction(uint64_t PC, const fextl::vector<Decoder::DecodedBlocks>* Blocks, uint32_t NumInstructions);
  void Finalize();

  void SetNewBlockIfChanged(uint64_t PC);
  void StartNewBlock();
  // A new IR block that continues the current guest block (SMC full-validation split).
  void StartContinuationBlock() {}

  // Translates one decoded instruction. Returns false if the instruction could
  // not be handled at all (the caller abandons the compile).
  bool TranslateInstruction(const Decoder::DecodedInst& Inst);

  // Guest SIGSEGV (SEGV_ACCERR) at PC: the instruction word could not be read.
  void NoExecInstruction(uint64_t PC);

  // Ends the current guest block at NextPC if no instruction already set the PC.
  // Returns true if the block was closed with a jump to an already-known block.
  bool FinishOp(uint64_t NextPC, bool LastOp);

  IRPair<FEXCore::IR::IROp_ExitFunction> ExitFunction(Ref NewPC, FEXCore::IR::BranchHint Hint = FEXCore::IR::BranchHint::None) {
    return _ExitFunction(FEXCore::IR::OpSize::i64Bit, NewPC, Hint, InvalidNode, InvalidNode);
  }

  IRPair<FEXCore::IR::IROp_CondJump> CondJump(Ref Cmp, FEXCore::IR::CondClass Cond = FEXCore::IR::CondClass::NEQ) {
    return _CondJump(Cmp, Cond);
  }

  void SetDumpIR(bool DumpIR) {
    ShouldDump = DumpIR;
  }
  bool ShouldDumpIR() const {
    return ShouldDump;
  }

  bool NeedsBlockEnder() const {
    return false;
  }

private:
  // Stores PC to the guest context and raises the given synchronous guest signal.
  void RaiseGuestSignal(uint64_t PC, FEXCore::IR::BreakDefinition Reason);

  void UnimplementedInstruction(const Decoder::DecodedInst& Inst);

  Ref GetRelocatedPC(uint64_t PC) {
    return _EntrypointOffset(FEXCore::IR::OpSize::i64Bit, PC - Entry);
  }

  struct JumpTargetInfo {
    Ref BlockEntry;
    bool HaveEmitted;
    bool IsEntryPoint;
  };

  [[maybe_unused]] FEXCore::Context::ContextImpl* CTX;
  fextl::map<uint64_t, JumpTargetInfo> JumpTargets;
  FEXCore::IR::IROp_IRHeader* CurrentHeader {};
  bool BlockSetPC {};
  bool ShouldDump {};
};

} // namespace FEXCore::A64
