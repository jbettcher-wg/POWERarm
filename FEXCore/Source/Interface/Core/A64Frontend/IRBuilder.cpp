// SPDX-License-Identifier: MIT
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Context/Context.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Utils/LogManager.h>

namespace FEXCore::A64 {
using namespace FEXCore::IR;

IRBuilder::IRBuilder(FEXCore::Context::ContextImpl* ctx)
  : IREmitter {ctx->OpDispatcherAllocator, ctx->HostFeatures.SupportsTSOImm9, ctx->HostFeatures.SupportsTSODisp16}
  , CTX {ctx} {}

void IRBuilder::ResetWorkingList() {
  IREmitter::ReownOrClaimBuffer();

  JumpTargets.clear();
  BlockSetPC = false;
  ShouldDump = false;
  CurrentCodeBlock = nullptr;
  CurrentHeader = nullptr;
}

void IRBuilder::BeginFunction(uint64_t PC, const fextl::vector<Decoder::DecodedBlocks>* Blocks, uint32_t NumInstructions) {
  Entry = PC;
  auto IRHeader = _IRHeader(InvalidNode, PC, 0, NumInstructions, 0, 0);

  Ref PrevCodeBlock {};
  for (auto& Target : *Blocks) {
    auto CodeNode = CreateCodeNode(Target.IsEntryPoint, Target.Entry - Entry);
    JumpTargets.try_emplace(Target.Entry, JumpTargetInfo {CodeNode, false, Target.IsEntryPoint});
    if (PrevCodeBlock) {
      LinkCodeBlocks(PrevCodeBlock, CodeNode);
    }
    PrevCodeBlock = CodeNode;
  }

  auto It = JumpTargets.find(PC);
  LOGMAN_THROW_A_FMT(It != JumpTargets.end(), "Couldn't find block generated for 0x{:x}", PC);
  SetCurrentCodeBlock(It->second.BlockEntry);
  IRHeader.first->Blocks = It->second.BlockEntry->Wrapped(DualListData.ListBegin());
  CurrentHeader = IRHeader.first;
}

void IRBuilder::SetNewBlockIfChanged(uint64_t PC) {
  auto It = JumpTargets.find(PC);
  if (It == JumpTargets.end()) {
    return;
  }

  It->second.HaveEmitted = true;

  if (CurrentCodeBlock->Wrapped(DualListData.ListBegin()).ID() == It->second.BlockEntry->Wrapped(DualListData.ListBegin()).ID()) {
    return;
  }

  SetCurrentCodeBlock(It->second.BlockEntry);
}

void IRBuilder::StartNewBlock() {
  BlockSetPC = false;
}

void IRBuilder::Finalize() {
  // Any block that was created but never emitted exits to the dispatcher at its entry.
  for (auto& [PC, Target] : JumpTargets) {
    if (Target.HaveEmitted) {
      continue;
    }
    SetCurrentCodeBlock(Target.BlockEntry);
    ExitFunction(_InlineEntrypointOffset(OpSize::i64Bit, PC - Entry));
  }
}

bool IRBuilder::FinishOp(uint64_t NextPC, bool LastOp) {
  if (LastOp && !BlockSetPC) {
    auto It = JumpTargets.find(NextPC);
    if (It == JumpTargets.end()) {
      ExitFunction(_InlineEntrypointOffset(OpSize::i64Bit, NextPC - Entry));
    } else {
      _Jump(It->second.BlockEntry);
      return true;
    }
  }

  BlockSetPC = false;
  return false;
}

void IRBuilder::RaiseGuestSignal(uint64_t PC, BreakDefinition Reason) {
  _StoreContextGPR(OpSize::i64Bit, GetRelocatedPC(PC), offsetof(FEXCore::Core::CPUState, rip));
  _Break(Reason);
  BlockSetPC = true;
}

bool IRBuilder::TranslateInstruction(const Decoder::DecodedInst& Inst) {
  // POWERARM-M0-TODO(frontend): no A64 translators yet; every instruction raises SIGILL. M1 interpreter / M2 translators replace this.
  UnimplementedInstruction(Inst);
  return true;
}

void IRBuilder::UnimplementedInstruction(const Decoder::DecodedInst& Inst) {
  RaiseGuestSignal(Inst.PC, BreakDefinition {
                              .ErrorRegister = 0,
                              .Signal = FEXCore::Core::FAULT_SIGILL,
                              .TrapNumber = 0,
                              .si_code = 1, ///< ILL_ILLOPC
                            });
}

void IRBuilder::NoExecInstruction(uint64_t PC) {
  RaiseGuestSignal(PC, BreakDefinition {
                         .ErrorRegister = 0,
                         .Signal = FEXCore::Core::FAULT_SIGSEGV,
                         .TrapNumber = 0,
                         .si_code = 2, ///< SEGV_ACCERR
                       });
}

} // namespace FEXCore::A64
