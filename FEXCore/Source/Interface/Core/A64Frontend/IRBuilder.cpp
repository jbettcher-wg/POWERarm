// SPDX-License-Identifier: MIT
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Context/Context.h"
#include "Interface/IR/RegisterAllocationData.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Utils/LogManager.h>

#include <array>

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

// clang-format off
const IRBuilder::HandlerEntry IRBuilder::HandlerTable[] = {
  // PC-relative addressing.
  {"ADR", &IRBuilder::ADR}, {"ADRP", &IRBuilder::ADRP},
  // Add/sub (immediate).
  {"ADD_imm", &IRBuilder::ADD_imm}, {"ADDS_imm", &IRBuilder::ADDS_imm},
  {"SUB_imm", &IRBuilder::SUB_imm}, {"SUBS_imm", &IRBuilder::SUBS_imm},
  // Logical (immediate).
  {"AND_imm", &IRBuilder::AND_imm}, {"ORR_imm", &IRBuilder::ORR_imm},
  {"EOR_imm", &IRBuilder::EOR_imm}, {"ANDS_imm", &IRBuilder::ANDS_imm},
  // Move wide.
  {"MOVN", &IRBuilder::MOVN}, {"MOVZ", &IRBuilder::MOVZ}, {"MOVK", &IRBuilder::MOVK},
  // Bitfield, including the aliases the table lists separately.
  {"SBFM", &IRBuilder::SBFM}, {"BFM", &IRBuilder::BFM}, {"UBFM", &IRBuilder::UBFM},
  {"ASR_1", &IRBuilder::SBFM}, {"ASR_2", &IRBuilder::SBFM},
  {"SXTB_1", &IRBuilder::SBFM}, {"SXTB_2", &IRBuilder::SBFM},
  {"SXTH_1", &IRBuilder::SBFM}, {"SXTH_2", &IRBuilder::SBFM}, {"SXTW", &IRBuilder::SBFM},
  {"EXTR", &IRBuilder::EXTR},
  // Branches and exceptions.
  {"B_cond", &IRBuilder::B_cond}, {"B_uncond", &IRBuilder::B_uncond}, {"BL", &IRBuilder::BL},
  {"CBZ", &IRBuilder::CBZ}, {"CBNZ", &IRBuilder::CBNZ}, {"TBZ", &IRBuilder::TBZ}, {"TBNZ", &IRBuilder::TBNZ},
  {"BR", &IRBuilder::BR}, {"BLR", &IRBuilder::BLR}, {"RET", &IRBuilder::RET},
  {"SVC", &IRBuilder::SVC}, {"BRK", &IRBuilder::BRK},
  // System. Every hint (NOP, YIELD, WFE, WFI, SEV, SEVL, BTI, PAC*SP, ...) is a NOP.
  {"HINT", &IRBuilder::HINT}, {"NOP", &IRBuilder::HINT}, {"YIELD", &IRBuilder::HINT},
  {"WFE", &IRBuilder::HINT}, {"WFI", &IRBuilder::HINT}, {"SEV", &IRBuilder::HINT}, {"SEVL", &IRBuilder::HINT},
  {"DSB", &IRBuilder::HINT}, {"DMB", &IRBuilder::HINT}, {"ISB", &IRBuilder::HINT},
  {"CLREX", &IRBuilder::CLREX},
  {"MRS", &IRBuilder::MRS}, {"MSR_reg", &IRBuilder::MSR_reg},
  {"DC_ZVA", &IRBuilder::DC_ZVA},
  {"DC_CVAU", &IRBuilder::CacheMaintenanceNop}, {"IC_IVAU", &IRBuilder::CacheMaintenanceNop},
  {"UnallocatedEncoding", &IRBuilder::UnallocatedEncoding},
  // Data processing (register): 2 source, 1 source.
  {"UDIV", &IRBuilder::UDIV}, {"SDIV", &IRBuilder::SDIV},
  {"LSLV", &IRBuilder::LSLV}, {"LSRV", &IRBuilder::LSRV}, {"ASRV", &IRBuilder::ASRV}, {"RORV", &IRBuilder::RORV},
  {"RBIT_int", &IRBuilder::RBIT_int}, {"REV16_int", &IRBuilder::REV16_int}, {"REV", &IRBuilder::REV},
  {"REV32_int", &IRBuilder::REV32_int}, {"CLZ_int", &IRBuilder::CLZ_int}, {"CLS_int", &IRBuilder::CLS_int},
  // Logical and add/sub (shifted and extended register), with carry.
  {"AND_shift", &IRBuilder::LogicalShifted}, {"BIC_shift", &IRBuilder::LogicalShifted},
  {"ORR_shift", &IRBuilder::LogicalShifted}, {"ORN_shift", &IRBuilder::LogicalShifted},
  {"EOR_shift", &IRBuilder::LogicalShifted}, {"EON", &IRBuilder::LogicalShifted},
  {"ANDS_shift", &IRBuilder::LogicalShifted}, {"BICS", &IRBuilder::LogicalShifted},
  {"ADD_shift", &IRBuilder::AddSubShifted}, {"ADDS_shift", &IRBuilder::AddSubShifted},
  {"SUB_shift", &IRBuilder::AddSubShifted}, {"SUBS_shift", &IRBuilder::AddSubShifted},
  {"ADD_ext", &IRBuilder::AddSubExtended}, {"ADDS_ext", &IRBuilder::AddSubExtended},
  {"SUB_ext", &IRBuilder::AddSubExtended}, {"SUBS_ext", &IRBuilder::AddSubExtended},
  {"ADC", &IRBuilder::ADC}, {"ADCS", &IRBuilder::ADCS}, {"SBC", &IRBuilder::SBC}, {"SBCS", &IRBuilder::SBCS},
  // Conditional compare and select.
  {"CCMN_reg", &IRBuilder::CondCompare}, {"CCMP_reg", &IRBuilder::CondCompare},
  {"CCMN_imm", &IRBuilder::CondCompare}, {"CCMP_imm", &IRBuilder::CondCompare},
  {"CSEL", &IRBuilder::CondSelect}, {"CSINC", &IRBuilder::CondSelect},
  {"CSINV", &IRBuilder::CondSelect}, {"CSNEG", &IRBuilder::CondSelect},
  // 3 source.
  {"MADD", &IRBuilder::MADD}, {"MSUB", &IRBuilder::MSUB},
  {"SMADDL", &IRBuilder::SMADDL}, {"SMSUBL", &IRBuilder::SMSUBL},
  {"UMADDL", &IRBuilder::UMADDL}, {"UMSUBL", &IRBuilder::UMSUBL},
  {"SMULH", &IRBuilder::SMULH}, {"UMULH", &IRBuilder::UMULH},
};
// clang-format on

InstHandler IRBuilder::FindHandler(std::string_view Name) {
  for (const auto& Entry : HandlerTable) {
    if (Entry.Name == Name) {
      return Entry.Handler;
    }
  }
  return nullptr;
}

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
    _Jump(It->second.BlockEntry);
  } else {
    ExitFunction(_InlineEntrypointOffset(OpSize::i64Bit, Target - Entry));
  }
  BlockSetPC = true;
}

void IRBuilder::EmitConditionalExit(IRPair<IROp_CondJump> Jump, uint64_t Target) {
  auto CurrentBlock = GetCurrentBlock();

  auto TakenBlock = CreateNewCodeBlockAfter(CurrentBlock);
  SetTrueJumpTarget(Jump, TakenBlock);
  SetCurrentCodeBlock(TakenBlock);
  ExitToPC(Target);

  auto NotTakenBlock = CreateNewCodeBlockAfter(TakenBlock);
  SetFalseJumpTarget(Jump, NotTakenBlock);
  SetCurrentCodeBlock(NotTakenBlock);
  ExitToPC(CurrentPC + INSTRUCTION_SIZE);
}

void IRBuilder::RaiseGuestSignal(uint64_t PC, BreakDefinition Reason) {
  _StoreContext(OpSize::i64Bit, RegClass::GPR, PCValue(PC), offsetof(FEXCore::Core::CPUState, pc));
  _Break(Reason);
  BlockSetPC = true;
}

bool IRBuilder::TranslateInstruction(const Decoder::DecodedInst& Inst) {
  CurrentPC = Inst.PC;

  const auto* Matcher = DecodeInstruction(Inst.Word);
  if (!Matcher || !Matcher->Handler || !(this->*(Matcher->Handler))(Inst.Word)) {
    // Handlers reject before emitting anything, so the signal is the whole
    // translation of this instruction.
    UnimplementedInstruction(Inst);
  }
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

// ---------------------------------------------------------------------------
// Guest register access
// ---------------------------------------------------------------------------

Ref IRBuilder::LoadGPRSlot(uint32_t Index) {
  const int Slot = GuestRegToSlot[Index];
  if (Slot >= 0) {
    return _LoadRegister(Slot, RegClass::GPR, OpSize::i64Bit);
  }
  return _LoadContext(OpSize::i64Bit, RegClass::GPR, FEXCore::Core::CPUState::GPROffset(Index));
}

void IRBuilder::StoreGPRSlot(uint32_t Index, Ref Value) {
  const int Slot = GuestRegToSlot[Index];
  if (Slot >= 0) {
    // StoreRegister carries its static slot as the node's fixed physical register.
    Ref Store = _StoreRegister(Value, OpSize::i64Bit);
    Store->Reg = PhysicalRegister(RegClass::GPRFixed, Slot).Raw;
  } else {
    _StoreContext(OpSize::i64Bit, RegClass::GPR, Value, FEXCore::Core::CPUState::GPROffset(Index));
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
