// SPDX-License-Identifier: MIT
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Context/Context.h"
#include "Interface/IR/RegisterAllocationData.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>

#include <array>
#include <cstdlib>

namespace FEXCore::A64 {
using namespace FEXCore::IR;

namespace {
  // Guest register (0-30 = Xn, 31 = SP) -> static register slot, or -1 when the
  // register lives only in the context. Derived from StaticGPRGuestReg so the
  // two cannot disagree.
  constexpr std::array<int8_t, 32> GuestRegToSlot = [] {
    std::array<int8_t, 32> Slots {};
    Slots.fill(-1);
    for (size_t i = 0; i < FEXCore::Core::StaticGPRGuestReg.size(); ++i) {
      Slots[FEXCore::Core::StaticGPRGuestReg[i]] = static_cast<int8_t>(i);
    }
    return Slots;
  }();
} // namespace

IRBuilder::IRBuilder(FEXCore::Context::ContextImpl* ctx)
  : IREmitter {ctx->OpDispatcherAllocator, ctx->HostFeatures.SupportsTSOImm9, ctx->HostFeatures.SupportsTSODisp16}
  , CTX {ctx} {}

void IRBuilder::ResetWorkingList() {
  IREmitter::ReownOrClaimBuffer();

  JumpTargets.clear();
  GPRCache = {};
  CacheBlock = nullptr;
  BlockSetPC = false;
  ShouldDump = false;
  CurrentCodeBlock = nullptr;
  CurrentHeader = nullptr;
}

void IRBuilder::BeginFunction(uint64_t PC, const fextl::vector<Decoder::DecodedBlocks>* Blocks, uint32_t NumInstructions) {
  Entry = PC;
  ISBExitTargets.clear();
  auto IRHeader = _IRHeader(InvalidNode, PC, 0, NumInstructions, 0, 0);

  Ref PrevCodeBlock {};
  for (auto& Target : *Blocks) {
    auto CodeNode = CreateCodeNode(Target.IsEntryPoint, Target.Entry - Entry);
    // A block the decoder found exactly one in-unit predecessor for, and that
    // is not the unit's entry point, may continue that predecessor's register
    // region (warm G6). The entry point is excluded because the dispatcher and
    // block linking enter it from outside the unit, where nothing is cached.
    const bool SolePred = Target.SolePredEntry != 0 && !Target.IsEntryPoint;
    JumpTargets.try_emplace(Target.Entry, JumpTargetInfo {CodeNode, false, Target.IsEntryPoint, SolePred, nullptr});
    if (PrevCodeBlock) {
      LinkCodeBlocks(PrevCodeBlock, CodeNode);
    }
    PrevCodeBlock = CodeNode;
  }

  auto It = JumpTargets.find(PC);
  if (It == JumpTargets.end()) {
    auto CodeNode = CreateCodeNode(true, 0);
    auto [InsertedIt, _] = JumpTargets.try_emplace(PC, JumpTargetInfo {CodeNode, false, true, false, nullptr});
    It = InsertedIt;
  }
  LOGMAN_THROW_A_FMT(It != JumpTargets.end(), "Couldn't find block generated for 0x{:x}", PC);
  SetCurrentCodeBlock(It->second.BlockEntry);
  IRHeader.first->Blocks = It->second.BlockEntry->Wrapped(DualListData.ListBegin());
  CurrentHeader = IRHeader.first;
}

// Warm G6: does the GPR value cache survive the edge into the block at PC?
//
// Only when the decoder's census says this block has exactly one in-unit
// predecessor, that predecessor is the block the cache currently describes
// (so every cached value was computed by code that must have run), and at
// least one entry is still inside the reuse window. The entry block, the
// blocks the frontend itself creates (conditional-exit legs, the full-SMC
// validate/continue pair) and any block reachable more than one way keep the
// cold cache they have today.
bool IRBuilder::CacheSurvivesEdge(const JumpTargetInfo& Target, uint64_t PC) const {
  // Field kill switch, in the shape the other codegen levers use
  // (FEX_FALLTHROUGH, FEX_ZEXTOPT, FEX_NO_ABI_LIVEMASK): a wrong carry shows
  // up as a wrong value, not a crash, so it gets a lever that needs no
  // rebuild. With it set the frontend never marks RegionPred, so the
  // allocator sees the block-local IR it saw before. The name reaches the
  // process as POWERARM_NOREGIONCACHE (Source/POWERarm/EnvPrefix.cpp), and
  // like the other getenv levers it is not hashed into the code-cache
  // ConfigId, so flip it with a private cache when comparing.
  static const bool Disabled = getenv("FEX_NOREGIONCACHE") != nullptr;
  if (Disabled) {
    return false;
  }
  if (!Target.SolePred || !Target.PredBlock || Target.PredBlock != CacheBlock) {
    return false;
  }
  for (const auto& Entry : GPRCache) {
    if (Entry.Value && PC >= Entry.PC && PC - Entry.PC <= GPR_CACHE_WINDOW) {
      return true;
    }
  }
  return false;
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

  const bool Carry = CacheSurvivesEdge(It->second, PC);
  if (Carry) {
    // Tell the register allocator to treat predecessor and successor as one
    // allocation region, so the values the cache still holds stay in their
    // host registers across the edge instead of being freed at the block head.
    // RegionPred is the predecessor's block ID biased by one (0 = none).
    auto* PredOp = It->second.PredBlock->Op(DualListData.DataBegin())->CW<FEXCore::IR::IROp_CodeBlock>();
    auto* BlockOp = It->second.BlockEntry->Op(DualListData.DataBegin())->CW<FEXCore::IR::IROp_CodeBlock>();
    BlockOp->RegionPred = PredOp->ID + 1;
  }

  SetCurrentCodeBlock(It->second.BlockEntry);

  if (Carry) {
    // The cache moves with the cursor; every StoreContext stays where it was,
    // so CPUState is still exact at every guest instruction boundary.
    CacheBlock = CurrentCodeBlock;
  }
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
  if (BlockSetPC) {
    // The instruction already left the block.
    BlockSetPC = false;
    return true;
  }

  if (LastOp) {
    ExitToPC(NextPC);
    return true;
  }

  return false;
}

void IRBuilder::ExitToPC(uint64_t Target) {
  auto It = JumpTargets.find(Target);
  if (It != JumpTargets.end()) {
    It->second.PredBlock = GetCurrentBlock();
    _Jump(It->second.BlockEntry);
  } else {
    ExitFunction(_InlineEntrypointOffset(OpSize::i64Bit, Target - Entry));
  }
  BlockSetPC = true;
}

void IRBuilder::EmitConditionalExit(IRPair<IROp_CondJump> Jump, uint64_t Target) {
  // A successor that is a block of this compile unit is the CondJump's target
  // itself. Routing it through a new block holding only a Jump cost the host
  // code a `b` hop per edge (up to three taken branches per guest B.cond).
  // Only successors outside the unit get a block, which holds their exit.
  Ref LastBlock = GetCurrentBlock();
  // The block that holds the CondJump is the predecessor of both in-unit
  // successors; LastBlock is reassigned below, so capture it first.
  const Ref CondBlock = LastBlock;
  if (auto It = JumpTargets.find(Target); It != JumpTargets.end()) {
    It->second.PredBlock = CondBlock;
    SetTrueJumpTarget(Jump, It->second.BlockEntry);
  } else {
    auto TakenBlock = CreateNewCodeBlockAfter(LastBlock);
    SetTrueJumpTarget(Jump, TakenBlock);
    SetCurrentCodeBlock(TakenBlock);
    ExitToPC(Target);
    LastBlock = TakenBlock;
  }

  const uint64_t NextPC = CurrentPC + INSTRUCTION_SIZE;
  if (auto It = JumpTargets.find(NextPC); It != JumpTargets.end()) {
    It->second.PredBlock = CondBlock;
    SetFalseJumpTarget(Jump, It->second.BlockEntry);
  } else {
    auto NotTakenBlock = CreateNewCodeBlockAfter(LastBlock);
    SetFalseJumpTarget(Jump, NotTakenBlock);
    SetCurrentCodeBlock(NotTakenBlock);
    ExitToPC(NextPC);
  }
  BlockSetPC = true;
}

void IRBuilder::RaiseGuestSignal(uint64_t PC, BreakDefinition Reason) {
  _StoreContext(OpSize::i64Bit, RegClass::GPR, PCValue(PC), offsetof(FEXCore::Core::CPUState, pc));
  _Break(Reason);
  BlockSetPC = true;
}

bool IRBuilder::TranslateInstruction(const Decoder::DecodedInst& Inst) {
  CurrentPC = Inst.PC;

  const auto* Matcher = Inst.Matcher;
  if (!Matcher || !Matcher->Handler || !(this->*(Matcher->Handler))(Inst.Word)) {
    // Handlers reject before emitting anything, so the signal is the whole
    // translation of this instruction.
    UnimplementedInstruction(Inst);
  }
  return true;
}

void IRBuilder::UnimplementedInstruction(const Decoder::DecodedInst& Inst) {
  // A guest that installs a SIGILL handler (cc1 does) never reaches the
  // emulator's own "unimplemented A64 instruction" line, so name the word
  // here for POWERARM_SILENTLOG=0 runs.
  LogMan::Msg::IFmt("Unimplemented A64 instruction 0x{:08x} at pc 0x{:x}", Inst.Word, Inst.PC);
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

void IRBuilder::UnalignedPCInstruction(uint64_t PC) {
  RaiseGuestSignal(PC, BreakDefinition {
                         .ErrorRegister = 0,
                         .Signal = FEXCore::Core::FAULT_SIGBUS,
                         .TrapNumber = 0,
                         .si_code = 1, ///< BUS_ADRALN
                       });
}

// ---------------------------------------------------------------------------
// Guest register access
// ---------------------------------------------------------------------------

// Context-backed registers (the ones with no static slot) keep a short-range
// cache of their last known value: a load within GPR_CACHE_WINDOW guest
// instructions of a store or load of the same register, in the same IR block,
// reuses that SSA value instead of reading the slot again. That removes the
// std-then-ld of one slot the pipeline research measured (PIPE Rule 1(a)) and
// repeated loads of a register a few instructions apart.
//
// Every store still happens, in place, so CPUState is exact at every guest
// instruction boundary, as before. The cache is only a statement about what
// the slot holds, and it is dropped when that could change behind the
// frontend's back: at a code block change (the syscall and exception paths
// all end the block), at a new compile, and for a stored value that is not
// 64 bits wide, whose upper half is not the stored one.
//
// Warm G6 (2026-09-22): the block change no longer drops it unconditionally.
// Across an intra-unit edge whose successor has exactly one in-unit
// predecessor -- the predecessor the cache belongs to -- the values stay live
// and the allocator is told to keep the two blocks in one allocation region
// (SetNewBlockIfChanged, IROp_CodeBlock::RegionPred,
// ConstrainedRAPass::Run). Nothing else changes: every StoreContext is still
// emitted in place, so a fault or signal anywhere in either block sees exact
// architectural state, and the exit, link and RIP-window paths read CPUState
// as before. The window is short so that the reused value does not
// have to stay live across much code; with five dynamic host registers a long
// lifetime is spilled to the stack instead, which costs more than the load it
// replaced. Measured (A64Bench warm ms, CPU 104; window in instructions):
//   window     crc32  sha256    vm  sort
//   off         1450    1428  2620  1049
//   1            598    1295  2339   993
//   3            403     990  1901   973
//   8            402     960  1742   970
//   32           401     952  1905   901
// gcc -O2 lvm.c moved by under 1% at every window.
Ref IRBuilder::LoadGPRSlot(uint32_t Index) {
  const int Slot = GuestRegToSlot[Index];
  if (Slot >= 0) {
    return _LoadRegister(Slot, RegClass::GPR, OpSize::i64Bit);
  }
  SyncGPRCache();
  auto& Cached = GPRCache[Index];
  if (Cached.Value && CurrentPC >= Cached.PC && CurrentPC - Cached.PC <= GPR_CACHE_WINDOW) {
    Cached.PC = CurrentPC;
    return Cached.Value;
  }
  Ref Value = _LoadContext(OpSize::i64Bit, RegClass::GPR, FEXCore::Core::CPUState::GPROffset(Index));
  Cached = {.Value = Value, .PC = CurrentPC};
  return Value;
}

void IRBuilder::StoreGPRSlot(uint32_t Index, Ref Value) {
  const int Slot = GuestRegToSlot[Index];
  if (Slot >= 0) {
    // StoreRegister carries its static slot as the node's fixed physical register.
    Ref Store = _StoreRegister(Value, OpSize::i64Bit);
    Store->Reg = PhysicalRegister(RegClass::GPRFixed, Slot).Raw;
  } else {
    _StoreContext(OpSize::i64Bit, RegClass::GPR, Value, FEXCore::Core::CPUState::GPROffset(Index));
    SyncGPRCache();
    if (GetOpSize(Value) == OpSize::i64Bit) {
      GPRCache[Index] = {.Value = Value, .PC = CurrentPC};
    } else {
      GPRCache[Index] = {};
    }
  }
}

Ref IRBuilder::LoadX(uint32_t Reg) {
  if (Reg == 31) {
    return Constant(0);
  }
  return LoadGPRSlot(Reg);
}

Ref IRBuilder::LoadXSP(uint32_t Reg) {
  return LoadGPRSlot(Reg);
}

void IRBuilder::StoreX(uint32_t Reg, Ref Value) {
  if (Reg == 31) {
    return;
  }
  StoreGPRSlot(Reg, Value);
}

void IRBuilder::StoreXSP(uint32_t Reg, Ref Value) {
  StoreGPRSlot(Reg, Value);
}

void IRBuilder::StoreW(uint32_t Reg, Ref Value) {
  if (Reg == 31) {
    return;
  }
  StoreGPRSlot(Reg, ZeroExtend32(Value));
}

void IRBuilder::StoreWSP(uint32_t Reg, Ref Value) {
  StoreGPRSlot(Reg, ZeroExtend32(Value));
}

Ref IRBuilder::LoadV(uint32_t Reg) {
  if (Reg < FEXCore::Core::NumStaticVectorRegs) {
    return _LoadRegister(Reg, RegClass::FPR, OpSize::i128Bit);
  }
  return _LoadContext(OpSize::i128Bit, RegClass::FPR, FEXCore::Core::CPUState::VectorOffset(Reg));
}

void IRBuilder::StoreV(uint32_t Reg, Ref Value) {
  if (Reg < FEXCore::Core::NumStaticVectorRegs) {
    Ref Store = _StoreRegister(Value, OpSize::i128Bit);
    Store->Reg = PhysicalRegister(RegClass::FPRFixed, Reg).Raw;
  } else {
    _StoreContext(OpSize::i128Bit, RegClass::FPR, Value, FEXCore::Core::CPUState::VectorOffset(Reg));
  }
}

// ---------------------------------------------------------------------------
// Shared operand forms
// ---------------------------------------------------------------------------

Ref IRBuilder::ShiftReg(Ref Value, uint32_t ShiftType, uint32_t Amount, bool Is64) {
  if (Amount == 0) {
    return Value;
  }
  const auto Size = SizeFor(Is64);
  switch (ShiftType) {
  case 0: return _Lshl(Size, Value, Constant(Amount));
  case 1: return _Lshr(Size, Value, Constant(Amount));
  case 2: return _Ashr(Size, Value, Constant(Amount));
  default: return _Ror(Size, Value, Constant(Amount));
  }
}

Ref IRBuilder::ExtendReg(Ref Value, uint32_t Option, uint32_t Shift) {
  Ref Extended {};
  switch (Option) {
  case 0b000: Extended = _Bfe(OpSize::i64Bit, 8, 0, Value); break;   // UXTB
  case 0b001: Extended = _Bfe(OpSize::i64Bit, 16, 0, Value); break;  // UXTH
  case 0b010: Extended = _Bfe(OpSize::i64Bit, 32, 0, Value); break;  // UXTW
  case 0b011: Extended = Value; break;                               // UXTX
  case 0b100: Extended = _Sbfe(OpSize::i64Bit, 8, 0, Value); break;  // SXTB
  case 0b101: Extended = _Sbfe(OpSize::i64Bit, 16, 0, Value); break; // SXTH
  case 0b110: Extended = _Sbfe(OpSize::i64Bit, 32, 0, Value); break; // SXTW
  default: Extended = Value; break;                                  // SXTX
  }
  if (Shift == 0) {
    return Extended;
  }
  return _Lshl(OpSize::i64Bit, Extended, Constant(Shift));
}

} // namespace FEXCore::A64
