// SPDX-License-Identifier: MIT
//
// A64 guest IR builder.
//
// Replaces the x86 OpDispatchBuilder as the IREmitter subclass a thread's
// compiler drives. It owns the per-compilation block bookkeeping (one IR code
// block per decoded guest block, entry/exit plumbing) and the per-instruction
// A64 translators.
//
// Translators are member functions named after their dynarmic decode table
// entry (a64.inc) and registered in HandlerTable (IRBuilder.cpp). A translator
// returns false when the word is an unallocated or unsupported combination of
// its fields; the instruction then raises SIGILL exactly like a word with no
// translator at all.
//
// Contracts the translators keep with the ppc64le backend:
//  * Guest NZCV lives in host CR0/XER between flag-producing and
//    flag-consuming ops. Only the NZCV ops, the *WithFlags ops and LoadNZCV/
//    StoreNZCV touch it; SpillStaticRegs/FillStaticRegs carry it across the
//    dispatcher, Syscall and Break.
//  * A W-register write stores a value whose bits 63:32 are zero (StoreW).
//    Values read from X registers for 32-bit operations may carry high bits;
//    the i32 IR ops the translators use only read the low half.
//  * Division never reaches divd/divw with a zero divisor or with
//    INT_MIN / -1 (both undefined on POWER); see the DIV translators.
#pragma once

#include "Interface/Core/A64Frontend/Decoder.h"
#include "Interface/Core/A64Frontend/DecodeTable.h"
#include "Interface/IR/IR.h"
#include "Interface/IR/IREmitter.h"

#include <FEXCore/fextl/map.h>
#include <FEXCore/fextl/vector.h>

#include <cstdint>
#include <string_view>

namespace FEXCore::Context {
class ContextImpl;
}

namespace FEXCore::A64 {

class IRBuilder final : public FEXCore::IR::IREmitter {
public:
  using Ref = FEXCore::IR::Ref;
  using OpSize = FEXCore::IR::OpSize;

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

  // Closes the current guest block at NextPC if this was its last instruction.
  // Returns true when no further instruction of the block may be translated:
  // the instruction ended the block (branch, SVC, signal), or the block was
  // closed here.
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

  // Translator lookup by decode table name; nullptr if none is registered.
  static InstHandler FindHandler(std::string_view Name);

  // clang-format off
  // Translators, grouped as in a64.inc. Defined in Translate*.cpp.
  // PC-relative addressing, add/sub, logical, move wide, bitfield, extract.
  bool ADR(uint32_t Word);   bool ADRP(uint32_t Word);
  bool ADD_imm(uint32_t Word); bool ADDS_imm(uint32_t Word); bool SUB_imm(uint32_t Word); bool SUBS_imm(uint32_t Word);
  bool AND_imm(uint32_t Word); bool ORR_imm(uint32_t Word); bool EOR_imm(uint32_t Word); bool ANDS_imm(uint32_t Word);
  bool MOVN(uint32_t Word); bool MOVZ(uint32_t Word); bool MOVK(uint32_t Word);
  bool SBFM(uint32_t Word); bool BFM(uint32_t Word); bool UBFM(uint32_t Word);
  bool EXTR(uint32_t Word);
  // Branches, exceptions, system.
  bool B_cond(uint32_t Word); bool B_uncond(uint32_t Word); bool BL(uint32_t Word);
  bool CBZ(uint32_t Word); bool CBNZ(uint32_t Word); bool TBZ(uint32_t Word); bool TBNZ(uint32_t Word);
  bool BR(uint32_t Word); bool BLR(uint32_t Word); bool RET(uint32_t Word);
  bool SVC(uint32_t Word); bool BRK(uint32_t Word);
  bool HINT(uint32_t Word); bool CLREX(uint32_t Word);
  bool MRS(uint32_t Word); bool MSR_reg(uint32_t Word);
  bool DC_ZVA(uint32_t Word); bool CacheMaintenanceNop(uint32_t Word);
  bool UnallocatedEncoding(uint32_t Word);
  // Loads and stores.
  bool LDR_lit_gen(uint32_t Word); bool LDRSW_lit(uint32_t Word); bool PRFM_lit(uint32_t Word);
  bool STP_LDP_gen(uint32_t Word);
  bool LoadStoreImm9(uint32_t Word); bool STRx_LDRx_imm_2(uint32_t Word);
  bool PRFM_imm(uint32_t Word);
  bool LoadStoreRegOffset(uint32_t Word);
  // Data processing (register).
  bool UDIV(uint32_t Word); bool SDIV(uint32_t Word);
  bool LSLV(uint32_t Word); bool LSRV(uint32_t Word); bool ASRV(uint32_t Word); bool RORV(uint32_t Word);
  bool RBIT_int(uint32_t Word); bool REV16_int(uint32_t Word); bool REV(uint32_t Word); bool REV32_int(uint32_t Word);
  bool CLZ_int(uint32_t Word); bool CLS_int(uint32_t Word);
  bool LogicalShifted(uint32_t Word);
  bool AddSubShifted(uint32_t Word); bool AddSubExtended(uint32_t Word);
  bool ADC(uint32_t Word); bool ADCS(uint32_t Word); bool SBC(uint32_t Word); bool SBCS(uint32_t Word);
  bool CondCompare(uint32_t Word);
  bool CondSelect(uint32_t Word);
  bool MADD(uint32_t Word); bool MSUB(uint32_t Word);
  bool SMADDL(uint32_t Word); bool SMSUBL(uint32_t Word); bool UMADDL(uint32_t Word); bool UMSUBL(uint32_t Word);
  bool SMULH(uint32_t Word); bool UMULH(uint32_t Word);
  // clang-format on

private:
  // --- Guest register access -------------------------------------------------
  // Register 31 means XZR for LoadX/StoreX and SP for LoadXSP/StoreXSP.
  Ref LoadX(uint32_t Reg);
  Ref LoadXSP(uint32_t Reg);
  void StoreX(uint32_t Reg, Ref Value);
  void StoreXSP(uint32_t Reg, Ref Value);
  // Writes a W register: the stored value is zero-extended from bit 31.
  void StoreW(uint32_t Reg, Ref Value);
  void StoreWSP(uint32_t Reg, Ref Value);
  // Is64 ? StoreX : StoreW, and the SP-capable pair.
  void StoreReg(uint32_t Reg, bool Is64, Ref Value) {
    Is64 ? StoreX(Reg, Value) : StoreW(Reg, Value);
  }
  void StoreRegSP(uint32_t Reg, bool Is64, Ref Value) {
    Is64 ? StoreXSP(Reg, Value) : StoreWSP(Reg, Value);
  }
  void StoreGPRSlot(uint32_t Index, Ref Value);
  Ref LoadGPRSlot(uint32_t Index);

  Ref ZeroExtend32(Ref Value) {
    return _Bfe(OpSize::i64Bit, 32, 0, Value);
  }

  static OpSize SizeFor(bool Is64) {
    return Is64 ? OpSize::i64Bit : OpSize::i32Bit;
  }

  // --- Shared operand forms --------------------------------------------------
  Ref ShiftReg(Ref Value, uint32_t ShiftType, uint32_t Amount, bool Is64);
  Ref ExtendReg(Ref Value, uint32_t Option, uint32_t Shift);

  // --- Control flow ----------------------------------------------------------
  Ref PCValue(uint64_t Target) {
    return _EntrypointOffset(OpSize::i64Bit, Target - Entry);
  }
  // Leaves the block for a constant guest PC (a Jump when the target is a
  // block of this compile unit, otherwise an ExitFunction).
  void ExitToPC(uint64_t Target);
  // Branches on an already emitted CondJump: true -> Target, false -> the next instruction.
  void EmitConditionalExit(IRPair<FEXCore::IR::IROp_CondJump> Jump, uint64_t Target);

  // Stores PC to the guest context and raises the given synchronous guest signal.
  void RaiseGuestSignal(uint64_t PC, FEXCore::IR::BreakDefinition Reason);
  void UnimplementedInstruction(const Decoder::DecodedInst& Inst);

  // Shared bodies behind several decode table entries.
  bool AddSubImmediate(uint32_t Word, bool IsSub, bool SetFlags);
  bool LogicalImmediate(uint32_t Word);
  bool MoveWide(uint32_t Word);
  bool Bitfield(uint32_t Word);
  bool ShiftVariable(uint32_t Word, FEXCore::IR::IROps Op);
  bool AddSubCarry(uint32_t Word, bool IsSub, bool SetFlags);
  bool MultiplyAddSubLong(uint32_t Word, bool IsSigned, bool IsSub);
  bool CompareBranch(uint32_t Word, bool IsNonZero);
  bool TestBranch(uint32_t Word, bool IsNonZero);
  bool BranchRegister(uint32_t Word, bool Link);
  // One load or store of Size bytes at Address, with the A64 opc decode already done.
  void LoadStoreSingle(bool IsLoad, OpSize Size, bool SignExtend, bool Is64Dest, uint32_t Rt, Ref Address);

  struct JumpTargetInfo {
    Ref BlockEntry;
    bool HaveEmitted;
    bool IsEntryPoint;
  };

  struct HandlerEntry {
    std::string_view Name;
    InstHandler Handler;
  };
  static const HandlerEntry HandlerTable[];

  [[maybe_unused]] FEXCore::Context::ContextImpl* CTX;
  fextl::map<uint64_t, JumpTargetInfo> JumpTargets;
  FEXCore::IR::IROp_IRHeader* CurrentHeader {};
  uint64_t CurrentPC {};
  bool BlockSetPC {};
  bool ShouldDump {};
};

} // namespace FEXCore::A64
