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

#include <array>
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
  // A guest call (BL/BLR): ReturnAddress is the X30 value as an inline
  // EntrypointOffset, so the backend can read it after register allocation.
  IRPair<FEXCore::IR::IROp_ExitFunction> ExitCall(Ref NewPC, Ref ReturnAddress) {
    return _ExitFunction(FEXCore::IR::OpSize::i64Bit, NewPC, FEXCore::IR::BranchHint::Call, ReturnAddress, InvalidNode);
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
  bool SVC(uint32_t Word); bool BRK(uint32_t Word); bool HLT(uint32_t Word);
  bool HINT(uint32_t Word); bool Barrier(uint32_t Word); bool CLREX(uint32_t Word);
  bool MRS(uint32_t Word); bool MSR_reg(uint32_t Word);
  bool DC_ZVA(uint32_t Word); bool CacheMaintenanceNop(uint32_t Word);
  bool UnallocatedEncoding(uint32_t Word);
  // Loads and stores.
  bool LDR_lit_gen(uint32_t Word); bool LDRSW_lit(uint32_t Word); bool PRFM_lit(uint32_t Word);
  bool STP_LDP_gen(uint32_t Word); bool STNP_LDNP_gen(uint32_t Word);
  bool LoadStoreImm9(uint32_t Word); bool STRx_LDRx_imm_2(uint32_t Word); bool STLURx_LDAPURx(uint32_t Word);
  bool PRFM_imm(uint32_t Word);
  bool LoadStoreRegOffset(uint32_t Word);
  bool LoadExclusive(uint32_t Word); bool StoreExclusive(uint32_t Word); bool LoadStoreAtomicWidth(uint32_t Word);
  bool AtomicMemOp(uint32_t Word); bool AtomicMinMax(uint32_t Word); bool LDAPR(uint32_t Word);
  bool CompareAndSwap(uint32_t Word); bool CompareAndSwapPair(uint32_t Word);
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
  // SIMD&FP register loads and stores.
  bool LDR_lit_fpsimd(uint32_t Word); bool STP_LDP_fpsimd(uint32_t Word); bool STNP_LDNP_fpsimd(uint32_t Word);
  bool STUR_LDUR_fpsimd(uint32_t Word); bool STR_LDR_imm_fpsimd_1(uint32_t Word); bool STR_LDR_imm_fpsimd_2(uint32_t Word);
  bool STR_LDR_reg_fpsimd(uint32_t Word);
  bool LDx_STx_mult(uint32_t Word); bool SIMDSingleStructure(uint32_t Word);
  // Advanced SIMD integer.
  bool DUP_gen(uint32_t Word); bool DUP_elt_1(uint32_t Word); bool DUP_elt_2(uint32_t Word);
  bool UMOV(uint32_t Word); bool SMOV(uint32_t Word); bool INS_gen(uint32_t Word); bool INS_elt(uint32_t Word);
  bool MOVI(uint32_t Word); bool FMOV_vec_imm(uint32_t Word);
  bool ADD_vector(uint32_t Word); bool SUB_2(uint32_t Word);
  bool CMEQ_reg_2(uint32_t Word); bool CMGT_reg_2(uint32_t Word); bool CMGE_reg_2(uint32_t Word);
  bool CMHS_2(uint32_t Word); bool CMHI_2(uint32_t Word); bool CMTST_2(uint32_t Word);
  bool UMAX(uint32_t Word); bool UMIN(uint32_t Word); bool SMAX(uint32_t Word); bool SMIN(uint32_t Word);
  bool ADD_1(uint32_t Word); bool SUB_1(uint32_t Word);
  bool CMEQ_reg_1(uint32_t Word); bool CMGT_reg_1(uint32_t Word); bool CMGE_reg_1(uint32_t Word);
  bool CMHS_1(uint32_t Word); bool CMHI_1(uint32_t Word); bool CMTST_1(uint32_t Word);
  bool SIMDLogical(uint32_t Word);
  bool ADDP_vec(uint32_t Word); bool UMAXP(uint32_t Word); bool UMINP(uint32_t Word);
  bool ADDV(uint32_t Word); bool UMAXV(uint32_t Word); bool UMINV(uint32_t Word);
  bool CMEQ_zero_2(uint32_t Word); bool CMGT_zero_2(uint32_t Word); bool CMGE_zero_2(uint32_t Word);
  bool CMLE_2(uint32_t Word); bool CMLT_2(uint32_t Word);
  bool CMEQ_zero_1(uint32_t Word); bool CMGT_zero_1(uint32_t Word); bool CMGE_zero_1(uint32_t Word);
  bool CMLE_1(uint32_t Word); bool CMLT_1(uint32_t Word);
  bool RBIT_asimd(uint32_t Word);
  bool CNT(uint32_t Word); bool NOT(uint32_t Word); bool NEG_2(uint32_t Word); bool ABS_2(uint32_t Word);
  bool REV64_asimd(uint32_t Word); bool REV32_asimd(uint32_t Word);
  bool XTN(uint32_t Word);
  bool SADDL(uint32_t Word); bool UADDL(uint32_t Word); bool SSUBL(uint32_t Word); bool USUBL(uint32_t Word);
  bool SADDW(uint32_t Word); bool UADDW(uint32_t Word); bool ADDHN(uint32_t Word); bool SUBHN(uint32_t Word);
  bool SSHR_2(uint32_t Word); bool USHR_2(uint32_t Word); bool SHL_2(uint32_t Word);
  bool SSHR_1(uint32_t Word); bool USHR_1(uint32_t Word); bool SHL_1(uint32_t Word);
  bool SHRN(uint32_t Word); bool SSHLL(uint32_t Word); bool USHLL(uint32_t Word);
  bool EXT(uint32_t Word);
  bool UZP1(uint32_t Word); bool UZP2(uint32_t Word); bool ZIP1(uint32_t Word); bool ZIP2(uint32_t Word);
  bool TRN1(uint32_t Word); bool TRN2(uint32_t Word);
  bool TBL(uint32_t Word); bool TBX(uint32_t Word);
  bool USHL_2(uint32_t Word); bool SSHL_2(uint32_t Word); bool USHL_1(uint32_t Word); bool SSHL_1(uint32_t Word);
  bool SRI_2(uint32_t Word); bool SLI_2(uint32_t Word); bool USRA_2(uint32_t Word); bool SSRA_2(uint32_t Word);
  bool UADDLP(uint32_t Word); bool SADDLP(uint32_t Word); bool UADALP(uint32_t Word); bool SADALP(uint32_t Word);
  bool SSUBW(uint32_t Word); bool USUBW(uint32_t Word);
  bool UMULL_vec(uint32_t Word); bool SMULL_vec(uint32_t Word); bool UMLAL_vec(uint32_t Word); bool SMLAL_vec(uint32_t Word);
  bool UMLSL_vec(uint32_t Word); bool SMLSL_vec(uint32_t Word);
  bool MUL_vec(uint32_t Word); bool MLA_vec(uint32_t Word); bool MLS_vec(uint32_t Word); bool MUL_elt(uint32_t Word);
  bool SMAXP(uint32_t Word); bool SMINP(uint32_t Word); bool REV16_asimd(uint32_t Word);
  bool NEG_1(uint32_t Word); bool ABS_1(uint32_t Word); bool ADDP_pair(uint32_t Word); bool UQSUB_1(uint32_t Word);
  bool FNEG_2(uint32_t Word); bool FABS_2(uint32_t Word); bool FABD_2(uint32_t Word); bool FABD_4(uint32_t Word);
  bool SCVTF_int_4(uint32_t Word); bool UCVTF_int_4(uint32_t Word);
  // Scalar floating point.
  bool FMOV_float_gen(uint32_t Word); bool FMOV_float(uint32_t Word); bool FMOV_float_imm(uint32_t Word);
  bool FABS_float(uint32_t Word); bool FNEG_float(uint32_t Word); bool FSQRT_float(uint32_t Word); bool FCVT_float(uint32_t Word);
  bool FRINTN_float(uint32_t Word); bool FRINTP_float(uint32_t Word); bool FRINTM_float(uint32_t Word); bool FRINTZ_float(uint32_t Word);
  bool FRINTA_float(uint32_t Word); bool FRINTX_float(uint32_t Word); bool FRINTI_float(uint32_t Word);
  bool FADD_float(uint32_t Word); bool FSUB_float(uint32_t Word); bool FMUL_float(uint32_t Word); bool FDIV_float(uint32_t Word);
  bool FNMUL_float(uint32_t Word); bool FMIN_float(uint32_t Word); bool FMAX_float(uint32_t Word);
  bool FMINNM_float(uint32_t Word); bool FMAXNM_float(uint32_t Word);
  bool FPThreeRegister(uint32_t Word);
  bool FCMP_float(uint32_t Word); bool FCCMP_float(uint32_t Word); bool FCSEL_float(uint32_t Word);
  bool FCVTNS_float(uint32_t Word); bool FCVTNU_float(uint32_t Word); bool FCVTPS_float(uint32_t Word); bool FCVTPU_float(uint32_t Word);
  bool FCVTMS_float(uint32_t Word); bool FCVTMU_float(uint32_t Word); bool FCVTZS_float_int(uint32_t Word); bool FCVTZU_float_int(uint32_t Word);
  bool FCVTAS_float(uint32_t Word); bool FCVTAU_float(uint32_t Word);
  bool SCVTF_float_int(uint32_t Word); bool UCVTF_float_int(uint32_t Word); bool SCVTF_float_fix(uint32_t Word); bool UCVTF_float_fix(uint32_t Word);
  bool FCVTZS_float_fix(uint32_t Word); bool FCVTZU_float_fix(uint32_t Word);
  bool FCVTZS_int_2(uint32_t Word); bool FCVTZU_int_2(uint32_t Word); bool SCVTF_int_2(uint32_t Word); bool UCVTF_int_2(uint32_t Word);
  bool FCVTL(uint32_t Word); bool FCVTN(uint32_t Word); bool FCVTXN_1(uint32_t Word); bool FCVTXN_2(uint32_t Word);
  // Advanced SIMD floating point (TranslateSIMDFloat.cpp).
  bool FADD_2(uint32_t Word); bool FSUB_2(uint32_t Word); bool FMUL_vec_2(uint32_t Word); bool FDIV_2(uint32_t Word);
  bool FMIN_2(uint32_t Word); bool FMAX_2(uint32_t Word); bool FMINNM_2(uint32_t Word); bool FMAXNM_2(uint32_t Word);
  bool FMUL_elt_4(uint32_t Word); bool FMUL_elt_2(uint32_t Word); bool FMLA_elt_4(uint32_t Word); bool FMLA_elt_2(uint32_t Word);
  bool FMLS_elt_4(uint32_t Word); bool FMLS_elt_2(uint32_t Word); bool FMLA_vec_2(uint32_t Word); bool FMLS_vec_2(uint32_t Word);
  bool FADDP_vec_2(uint32_t Word); bool FMAXP_vec_2(uint32_t Word); bool FMINP_vec_2(uint32_t Word);
  bool FMAXNMP_vec_2(uint32_t Word); bool FMINNMP_vec_2(uint32_t Word);
  bool FMAXV_2(uint32_t Word); bool FMINV_2(uint32_t Word); bool FMAXNMV_2(uint32_t Word); bool FMINNMV_2(uint32_t Word);
  bool FADDP_pair_2(uint32_t Word); bool FMAXP_pair_2(uint32_t Word); bool FMINP_pair_2(uint32_t Word);
  bool FMAXNMP_pair_2(uint32_t Word); bool FMINNMP_pair_2(uint32_t Word);
  bool FCMEQ_reg_4(uint32_t Word); bool FCMGE_reg_4(uint32_t Word); bool FCMGT_reg_4(uint32_t Word);
  bool FACGE_4(uint32_t Word); bool FACGT_4(uint32_t Word);
  bool FCMEQ_zero_4(uint32_t Word); bool FCMGE_zero_4(uint32_t Word); bool FCMGT_zero_4(uint32_t Word);
  bool FCMLE_4(uint32_t Word); bool FCMLT_4(uint32_t Word);
  bool FCMEQ_reg_2(uint32_t Word); bool FCMGE_reg_2(uint32_t Word); bool FCMGT_reg_2(uint32_t Word);
  bool FACGE_2(uint32_t Word); bool FACGT_2(uint32_t Word);
  bool FCMEQ_zero_2(uint32_t Word); bool FCMGE_zero_2(uint32_t Word); bool FCMGT_zero_2(uint32_t Word);
  bool FCMLE_2(uint32_t Word); bool FCMLT_2(uint32_t Word);
  bool FRINTN_2(uint32_t Word); bool FRINTP_2(uint32_t Word); bool FRINTM_2(uint32_t Word); bool FRINTZ_2(uint32_t Word);
  bool FRINTA_2(uint32_t Word); bool FRINTX_2(uint32_t Word); bool FRINTI_2(uint32_t Word);
  bool FSQRT_2(uint32_t Word); bool FNEG_1(uint32_t Word); bool FABS_1(uint32_t Word);
  bool FCVTNS_4(uint32_t Word); bool FCVTNU_4(uint32_t Word); bool FCVTPS_4(uint32_t Word); bool FCVTPU_4(uint32_t Word);
  bool FCVTMS_4(uint32_t Word); bool FCVTMU_4(uint32_t Word); bool FCVTZS_int_4(uint32_t Word); bool FCVTZU_int_4(uint32_t Word);
  bool FCVTAS_4(uint32_t Word); bool FCVTAU_4(uint32_t Word);
  bool FCVTNS_2(uint32_t Word); bool FCVTNU_2(uint32_t Word); bool FCVTPS_2(uint32_t Word); bool FCVTPU_2(uint32_t Word);
  bool FCVTMS_2(uint32_t Word); bool FCVTMU_2(uint32_t Word); bool FCVTAS_2(uint32_t Word); bool FCVTAU_2(uint32_t Word);
  bool SCVTF_fix_2(uint32_t Word); bool UCVTF_fix_2(uint32_t Word); bool FCVTZS_fix_2(uint32_t Word); bool FCVTZU_fix_2(uint32_t Word);
  bool SCVTF_fix_1(uint32_t Word); bool UCVTF_fix_1(uint32_t Word); bool FCVTZS_fix_1(uint32_t Word); bool FCVTZU_fix_1(uint32_t Word);
  // Advanced SIMD estimates and Newton-Raphson steps (TranslateSIMDEstimate.cpp).
  bool FRECPE_2(uint32_t Word); bool FRECPE_4(uint32_t Word); bool FRSQRTE_2(uint32_t Word); bool FRSQRTE_4(uint32_t Word);
  bool FRECPX_2(uint32_t Word); bool URECPE(uint32_t Word); bool URSQRTE(uint32_t Word);
  bool FRECPE_1(uint32_t Word); bool FRECPE_3(uint32_t Word); bool FRSQRTE_1(uint32_t Word); bool FRSQRTE_3(uint32_t Word);
  bool FRECPX_1(uint32_t Word);
  bool FRECPS_2(uint32_t Word); bool FRECPS_4(uint32_t Word); bool FRSQRTS_2(uint32_t Word); bool FRSQRTS_4(uint32_t Word);
  // Advanced SIMD half precision (TranslateSIMDHalf.cpp).
  bool FADD_1(uint32_t Word); bool FSUB_1(uint32_t Word); bool FMUL_vec_1(uint32_t Word); bool FDIV_1(uint32_t Word);
  bool FMIN_1(uint32_t Word); bool FMAX_1(uint32_t Word); bool FMINNM_1(uint32_t Word); bool FMAXNM_1(uint32_t Word);
  bool FMULX_vec_3(uint32_t Word); bool FMULX_vec_1(uint32_t Word); bool FABD_3(uint32_t Word); bool FABD_1(uint32_t Word);
  bool FRECPS_3(uint32_t Word); bool FRECPS_1(uint32_t Word); bool FRSQRTS_3(uint32_t Word); bool FRSQRTS_1(uint32_t Word);
  bool FMLA_vec_1(uint32_t Word); bool FMLS_vec_1(uint32_t Word); bool FMLA_elt_3(uint32_t Word); bool FMLS_elt_3(uint32_t Word);
  bool FMLA_elt_1(uint32_t Word); bool FMLS_elt_1(uint32_t Word); bool FMUL_elt_3(uint32_t Word); bool FMUL_elt_1(uint32_t Word);
  bool FMULX_elt_3(uint32_t Word); bool FMULX_elt_1(uint32_t Word);
  bool FADDP_vec_1(uint32_t Word); bool FMAXP_vec_1(uint32_t Word); bool FMINP_vec_1(uint32_t Word);
  bool FMAXNMP_vec_1(uint32_t Word); bool FMINNMP_vec_1(uint32_t Word);
  bool FADDP_pair_1(uint32_t Word); bool FMAXP_pair_1(uint32_t Word); bool FMINP_pair_1(uint32_t Word);
  bool FMAXNMP_pair_1(uint32_t Word); bool FMINNMP_pair_1(uint32_t Word);
  bool FMAXV_1(uint32_t Word); bool FMINV_1(uint32_t Word); bool FMAXNMV_1(uint32_t Word); bool FMINNMV_1(uint32_t Word);
  bool FCMEQ_reg_3(uint32_t Word); bool FCMGE_reg_3(uint32_t Word); bool FCMGT_reg_3(uint32_t Word);
  bool FACGE_3(uint32_t Word); bool FACGT_3(uint32_t Word);
  bool FCMEQ_zero_3(uint32_t Word); bool FCMGE_zero_3(uint32_t Word); bool FCMGT_zero_3(uint32_t Word);
  bool FCMLE_3(uint32_t Word); bool FCMLT_3(uint32_t Word);
  bool FCMEQ_reg_1(uint32_t Word); bool FCMGE_reg_1(uint32_t Word); bool FCMGT_reg_1(uint32_t Word);
  bool FACGE_1(uint32_t Word); bool FACGT_1(uint32_t Word);
  bool FCMEQ_zero_1(uint32_t Word); bool FCMGE_zero_1(uint32_t Word); bool FCMGT_zero_1(uint32_t Word);
  bool FCMLE_1(uint32_t Word); bool FCMLT_1(uint32_t Word);
  bool FRINTN_1(uint32_t Word); bool FRINTP_1(uint32_t Word); bool FRINTM_1(uint32_t Word); bool FRINTZ_1(uint32_t Word);
  bool FRINTA_1(uint32_t Word); bool FRINTX_1(uint32_t Word); bool FRINTI_1(uint32_t Word); bool FSQRT_1(uint32_t Word);
  bool FCVTNS_3(uint32_t Word); bool FCVTNU_3(uint32_t Word); bool FCVTPS_3(uint32_t Word); bool FCVTPU_3(uint32_t Word);
  bool FCVTMS_3(uint32_t Word); bool FCVTMU_3(uint32_t Word); bool FCVTZS_int_3(uint32_t Word); bool FCVTZU_int_3(uint32_t Word);
  bool FCVTAS_3(uint32_t Word); bool FCVTAU_3(uint32_t Word);
  bool FCVTNS_1(uint32_t Word); bool FCVTNU_1(uint32_t Word); bool FCVTPS_1(uint32_t Word); bool FCVTPU_1(uint32_t Word);
  bool FCVTMS_1(uint32_t Word); bool FCVTMU_1(uint32_t Word); bool FCVTZS_int_1(uint32_t Word); bool FCVTZU_int_1(uint32_t Word);
  bool FCVTAS_1(uint32_t Word); bool FCVTAU_1(uint32_t Word);
  bool SCVTF_int_3(uint32_t Word); bool UCVTF_int_3(uint32_t Word); bool SCVTF_int_1(uint32_t Word); bool UCVTF_int_1(uint32_t Word);
  bool FMOV_3(uint32_t Word);
  // Advanced SIMD saturating, rounding and halving families (TranslateSIMDSaturate.cpp).
  bool SQADD_2(uint32_t Word); bool SQSUB_2(uint32_t Word); bool UQADD_2(uint32_t Word); bool UQSUB_2(uint32_t Word);
  bool SQADD_1(uint32_t Word); bool SQSUB_1(uint32_t Word); bool UQADD_1(uint32_t Word);
  bool SQABS_2(uint32_t Word); bool SQNEG_2(uint32_t Word); bool SQABS_1(uint32_t Word); bool SQNEG_1(uint32_t Word);
  bool SQXTN_2(uint32_t Word); bool UQXTN_2(uint32_t Word); bool SQXTUN_2(uint32_t Word);
  bool SQXTN_1(uint32_t Word); bool UQXTN_1(uint32_t Word); bool SQXTUN_1(uint32_t Word);
  bool RSHRN(uint32_t Word); bool SQSHRN_2(uint32_t Word); bool SQRSHRN_2(uint32_t Word); bool UQSHRN_2(uint32_t Word);
  bool UQRSHRN_2(uint32_t Word); bool SQSHRUN_2(uint32_t Word); bool SQRSHRUN_2(uint32_t Word);
  bool SQSHRN_1(uint32_t Word); bool UQSHRN_1(uint32_t Word); bool SQSHRUN_1(uint32_t Word);
  bool SQRSHRN_1(uint32_t Word); bool UQRSHRN_1(uint32_t Word); bool SQRSHRUN_1(uint32_t Word);
  bool SRI_1(uint32_t Word); bool SLI_1(uint32_t Word); bool USRA_1(uint32_t Word); bool SSRA_1(uint32_t Word);
  bool SRSHR_2(uint32_t Word); bool URSHR_2(uint32_t Word); bool SRSRA_2(uint32_t Word); bool URSRA_2(uint32_t Word);
  bool SRSHR_1(uint32_t Word); bool URSHR_1(uint32_t Word); bool SRSRA_1(uint32_t Word); bool URSRA_1(uint32_t Word);
  bool SQSHL_imm_2(uint32_t Word); bool UQSHL_imm_2(uint32_t Word); bool SQSHLU_2(uint32_t Word);
  bool SQSHL_imm_1(uint32_t Word); bool UQSHL_imm_1(uint32_t Word); bool SQSHLU_1(uint32_t Word);
  bool UHADD(uint32_t Word); bool SHADD(uint32_t Word); bool URHADD(uint32_t Word); bool SRHADD(uint32_t Word);
  bool UHSUB(uint32_t Word); bool SHSUB(uint32_t Word);
  bool UABD(uint32_t Word); bool SABD(uint32_t Word); bool UABA(uint32_t Word); bool SABA(uint32_t Word);
  bool UABDL(uint32_t Word); bool SABDL(uint32_t Word); bool UABAL(uint32_t Word); bool SABAL(uint32_t Word);
  bool RADDHN(uint32_t Word); bool RSUBHN(uint32_t Word);
  bool SQDMULH_vec_2(uint32_t Word); bool SQRDMULH_vec_2(uint32_t Word); bool SQDMULH_vec_1(uint32_t Word); bool SQRDMULH_vec_1(uint32_t Word);
  bool SQDMULH_elt_2(uint32_t Word); bool SQRDMULH_elt_2(uint32_t Word); bool SQDMULH_elt_1(uint32_t Word); bool SQRDMULH_elt_1(uint32_t Word);
  bool SQRDMLAH_vec_2(uint32_t Word); bool SQRDMLAH_vec_1(uint32_t Word);
  bool SQRDMLSH_vec_2(uint32_t Word); bool SQRDMLSH_vec_1(uint32_t Word);
  bool SQRDMLAH_elt_2(uint32_t Word); bool SQRDMLAH_elt_1(uint32_t Word);
  bool SQRDMLSH_elt_2(uint32_t Word); bool SQRDMLSH_elt_1(uint32_t Word);
  bool SIMDDoublingMultiplyAccumulateHigh(uint32_t Word, bool Subtract, bool Scalar, bool ByElement);
  bool MLA_elt(uint32_t Word); bool MLS_elt(uint32_t Word);
  bool SMULL_elt(uint32_t Word); bool UMULL_elt(uint32_t Word); bool SMLAL_elt(uint32_t Word); bool UMLAL_elt(uint32_t Word);
  bool SMLSL_elt(uint32_t Word); bool UMLSL_elt(uint32_t Word);
  bool UADDLV(uint32_t Word); bool SADDLV(uint32_t Word); bool SMAXV(uint32_t Word); bool SMINV(uint32_t Word);
  bool CLZ_asimd(uint32_t Word); bool CLS_asimd(uint32_t Word); bool UDOT_vec(uint32_t Word); bool SHLL(uint32_t Word);
  bool SDOT_vec(uint32_t Word); bool UDOT_elt(uint32_t Word); bool SDOT_elt(uint32_t Word); bool PMUL(uint32_t Word);
  bool SQSHL_reg_2(uint32_t Word); bool UQSHL_reg_2(uint32_t Word); bool SRSHL_2(uint32_t Word); bool URSHL_2(uint32_t Word);
  bool SQRSHL_2(uint32_t Word); bool UQRSHL_2(uint32_t Word);
  bool SQSHL_reg_1(uint32_t Word); bool UQSHL_reg_1(uint32_t Word); bool SRSHL_1(uint32_t Word); bool URSHL_1(uint32_t Word);
  bool SQRSHL_1(uint32_t Word); bool UQRSHL_1(uint32_t Word);
  bool SUQADD_1(uint32_t Word); bool SUQADD_2(uint32_t Word); bool USQADD_1(uint32_t Word); bool USQADD_2(uint32_t Word);
  bool SQDMULL_vec_1(uint32_t Word); bool SQDMULL_vec_2(uint32_t Word); bool SQDMLAL_vec_1(uint32_t Word); bool SQDMLAL_vec_2(uint32_t Word);
  bool SQDMLSL_vec_1(uint32_t Word); bool SQDMLSL_vec_2(uint32_t Word); bool SQDMULL_elt_1(uint32_t Word); bool SQDMULL_elt_2(uint32_t Word);
  bool SQDMLAL_elt_1(uint32_t Word); bool SQDMLAL_elt_2(uint32_t Word); bool SQDMLSL_elt_1(uint32_t Word); bool SQDMLSL_elt_2(uint32_t Word);
  bool FMULX_vec_2(uint32_t Word); bool FMULX_vec_4(uint32_t Word); bool FMULX_elt_2(uint32_t Word); bool FMULX_elt_4(uint32_t Word);
  // Cryptographic extension and CRC32 (TranslateCrypto.cpp).
  bool AESE(uint32_t Word); bool AESD(uint32_t Word); bool AESMC(uint32_t Word); bool AESIMC(uint32_t Word);
  bool PMULL(uint32_t Word);
  bool SHA1C(uint32_t Word); bool SHA1M(uint32_t Word); bool SHA1P(uint32_t Word); bool SHA1H(uint32_t Word);
  bool SHA1SU0(uint32_t Word); bool SHA1SU1(uint32_t Word);
  bool SHA256H(uint32_t Word); bool SHA256H2(uint32_t Word); bool SHA256SU0(uint32_t Word); bool SHA256SU1(uint32_t Word);
  bool CRC32(uint32_t Word); bool CRC32C(uint32_t Word);
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

  // --- Vector register access -------------------------------------------------
  // V0-V15 live in static host vector registers, V16-V31 in the context.
  // Values are always 128 bits; element 0 is the low half (see CoreState.h).
  Ref LoadV(uint32_t Reg);
  void StoreV(uint32_t Reg, Ref Value);
  // A64 writes to B/H/S/D registers and 64-bit vector results clear every
  // bit above the written size.
  void StoreVSized(uint32_t Reg, OpSize Size, Ref Value) {
    StoreV(Reg, Size == OpSize::i128Bit ? Value : _VMov(Size, Value).Node);
  }
  // Q ? 128-bit result : 64-bit result with the upper half cleared.
  void StoreVQ(uint32_t Reg, bool Q, Ref Value) {
    StoreVSized(Reg, Q ? OpSize::i128Bit : OpSize::i64Bit, Value);
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
  bool BranchRegister(uint32_t Word, FEXCore::IR::BranchHint Hint);
  // One load or store of Size bytes at Address, with the A64 opc decode already done.
  // Offset, when given, is an InlineConstant byte offset added to Address by
  // the memory op itself (a D-form displacement on the host).
  void LoadStoreSingle(bool IsLoad, OpSize Size, bool SignExtend, bool Is64Dest, uint32_t Rt, Ref Address, Ref Offset = nullptr);
  // One SIMD&FP register load or store of Size bytes at Address.
  void LoadStoreV(bool IsLoad, OpSize Size, uint32_t Rt, Ref Address);

  // Advanced SIMD shared bodies (TranslateSIMD.cpp).
  enum class ThreeSameOp { Add, Sub, CmEq, CmGt, CmGe, CmHs, CmHi, CmTst, UMax, UMin, SMax, SMin };
  enum class PairwiseOp { Add, UMax, UMin, SMax, SMin };
  enum class CompareZeroOp { Eq, Gt, Ge, Le, Lt };
  enum class ThreeDifferentOp { SAddL, UAddL, SSubL, USubL, SAddW, UAddW, AddHN, SubHN, SSubW, USubW, UMull, SMull, UMlal, SMlal, UMlsl, SMlsl };
  enum class ShiftInsertOp { Sri, Sli, Usra, Ssra };
  enum class ShiftImmOp { SShr, UShr, Shl, Shrn, SShll, UShll };
  enum class PermuteOp { Uzp1, Uzp2, Zip1, Zip2, Trn1, Trn2 };
  bool SIMDThreeSame(uint32_t Word, ThreeSameOp Op, bool Scalar);
  bool SIMDPairwise(uint32_t Word, PairwiseOp Op);
  bool SIMDAcrossLanesMinMax(uint32_t Word, bool IsMax);
  bool SIMDCompareZero(uint32_t Word, CompareZeroOp Op, bool Scalar);
  bool SIMDThreeDifferent(uint32_t Word, ThreeDifferentOp Op);
  bool SIMDShiftImm(uint32_t Word, ShiftImmOp Op, bool Scalar);
  bool SIMDPermute(uint32_t Word, PermuteOp Op);
  bool SIMDShiftRegister(uint32_t Word, bool Signed, bool Scalar);
  bool SIMDShiftInsertAccumulate(uint32_t Word, ShiftInsertOp Op, bool Scalar);
  bool SIMDAddLongPairwise(uint32_t Word, bool Signed, bool Accumulate);
  bool SIMDMultiply(uint32_t Word, int Accumulate);
  bool FPVectorUnary(uint32_t Word, bool IsNeg);
  bool FPAbsoluteDifference(uint32_t Word, bool Scalar);
  bool FPVectorIntToFloat(uint32_t Word, bool Signed);
  void StoreNarrow(uint32_t Rd, bool Upper, Ref Narrow);
  // Byte permutation of up to four 16-byte sources (TranslateSIMDLoadStore.cpp).
  Ref PermuteBytes(const std::array<Ref, 4>& Sources, uint32_t NumSources, const std::array<uint8_t, 16>& Map);
  bool TableLookup(uint32_t Word, bool IsTBX);
  // A 128-bit vector with Pattern in both 64-bit lanes.
  Ref VectorConstant64(uint64_t Pattern);

  // Scalar FP shared bodies (TranslateFP.cpp).
  enum class FPUnaryOp { Abs, Neg, Sqrt };
  enum class FPBinaryOp { Add, Sub, Mul, Div, NMul, Min, Max, MinNum, MaxNum };
  bool FPOneRegister(uint32_t Word, FPUnaryOp Op);
  enum class FPRounding { TiesEven, PosInf, NegInf, Zero, TiesAway, Current };
  bool FPRoundInt(uint32_t Word, FPRounding Mode);
  Ref FPRoundToIntegral(Ref X, OpSize ElementSize, FPRounding Mode);
  bool FPTwoRegister(uint32_t Word, FPBinaryOp Op);
  bool FPConvertToInt(uint32_t Word, uint8_t Rounding, bool Signed);
  bool FPConvertFromInt(uint32_t Word, bool Signed, bool Fixed);
  bool FPConvertToFixed(uint32_t Word, bool Signed);
  bool FPScalarSIMDConvert(uint32_t Word, bool ToInt, bool Signed);
  // Every ElementSize lane of the 128-bit result holds Bits.
  Ref FPConstant(uint64_t Bits, OpSize ElementSize);
  // FPAdd/FPSub/FPMul/FPDiv on every ElementSize lane, with A64 NaN precedence
  // built into the backend op (one host instruction plus a NaN check that
  // branches to an out-of-line cold block; docs/powerarm/COLD-BLOCK-DESIGN.md).
  Ref A64Arith(OpSize ElementSize, Ref A, Ref B, FPBinaryOp Op);
  // Operand 1 for a VSX arithmetic op so that the op propagates NaNs the A64 way.
  // Still used by FPMinMax and FPMulAddLanes, which have their own NaN rules and
  // their own cold bodies still to land (checklist F2/F3).
  Ref PropagateNaNOperand(OpSize ElementSize, Ref A, Ref B);
  Ref FPMinMax(OpSize ElementSize, Ref A, Ref B, bool IsMax, bool IsNumber);
  // All-ones 64-bit lanes when FPCR.FZ16 is set, zero otherwise.
  Ref FZ16Mask();
  // Element 0 half precision -> double, with the FZ16 input flush unless it
  // is an FP-to-FP conversion. With KeepSignalling a signalling NaN stays
  // signalling for operand precedence.
  Ref HalfToDouble(Ref V, bool KeepSignalling, bool ApplyFZ16);
  // Element 0 double -> half precision with one rounding, and the FZ16
  // output flush unless it is an FP-to-FP conversion.
  Ref DoubleToHalf(Ref D, bool ApplyFZ16);
  bool SIMDConvertRoundToOdd(uint32_t Word, bool Scalar);
  // Host rounding mode <- FPCR.RMode of the given FPCR value.
  void SyncHostRoundingMode(Ref FPCR);

  // Advanced SIMD floating-point shared bodies (TranslateSIMDFloat.cpp).
  enum class FloatCompareKind { Eq, Ge, Gt, AbsGe, AbsGt, EqZero, GeZero, GtZero, LeZero, LtZero };
  // FPMulAdd(A, N, M) per lane with A64 NaN handling (TranslateFP.cpp).
  Ref FPMulAddLanes(OpSize Size, Ref A, Ref N, Ref M);
  Ref FPBinaryLanes(FPBinaryOp Op, OpSize ES, Ref A, Ref B);
  void StoreFloatLanes(uint32_t Word, bool Scalar, OpSize ES, Ref Result);
  bool FloatElementOperand(uint32_t Word, OpSize ES, Ref* Element);
  bool SIMDFloatThreeSame(uint32_t Word, FPBinaryOp Op, bool Scalar);
  bool SIMDFloatMulElement(uint32_t Word, int Accumulate, bool Scalar);
  bool SIMDFloatMulAccumulate(uint32_t Word, bool Subtract);
  bool SIMDFloatPairwise(uint32_t Word, FPBinaryOp Op, bool Scalar);
  bool SIMDFloatAcrossLanes(uint32_t Word, FPBinaryOp Op);
  bool SIMDFloatCompare(uint32_t Word, FloatCompareKind Kind, bool Scalar);
  bool SIMDFloatRound(uint32_t Word, FPRounding Mode);
  bool SIMDHalfSign(uint32_t Word, bool IsNeg);
  bool SIMDFloatToInt(uint32_t Word, uint8_t Rounding, bool Signed, bool Scalar);
  bool SIMDFixedConvert(uint32_t Word, bool ToFloat, bool Signed, bool Scalar);
  Ref FPMulXLanes(OpSize ES, Ref A, Ref B);
  bool SIMDFloatMulX(uint32_t Word, bool Scalar, bool ByElement);

  // Estimates and steps (TranslateSIMDEstimate.cpp).
  Ref RecipEstimateTable(OpSize ES, Ref A);
  Ref RecipSqrtEstimateTable(OpSize ES, Ref A);
  Ref OverflowToInfinityMask(OpSize ES, Ref X);
  Ref FPRecipEstimateLanes(OpSize ES, Ref X, bool HalfInWord);
  Ref FPRSqrtEstimateLanes(OpSize ES, Ref X, bool HalfInWord);
  Ref FPStepFusedLanes(OpSize ES, Ref Op1, Ref Op2, bool Sqrt);
  bool SIMDFloatEstimate(uint32_t Word, bool Sqrt, bool Scalar);

  // Half precision (TranslateSIMDHalf.cpp).
  enum class HalfOp { Add, Sub, Mul, Div, Min, Max, MinNum, MaxNum, MulX, Abd, RecipStep, RSqrtStep };
  template<typename F>
  Ref HalfLanes(uint32_t Lanes, std::array<Ref, 3> Operands, bool KeepSignalling, F&& Body);
  void StoreHalfLanes(uint32_t Word, bool Scalar, Ref Result);
  Ref HalfBinaryLane(HalfOp Op, Ref A, Ref B);
  Ref HalfElementOperand(uint32_t Word);
  bool SIMDHalfThreeSame(uint32_t Word, HalfOp Op, bool Scalar);
  bool SIMDHalfMulElement(uint32_t Word, HalfOp Op, bool Scalar);
  bool SIMDHalfMulAccumulate(uint32_t Word, bool Subtract, bool ByElement, bool Scalar);
  bool SIMDHalfPairwise(uint32_t Word, HalfOp Op, bool Scalar);
  bool SIMDHalfAcrossLanes(uint32_t Word, HalfOp Op);
  bool SIMDHalfCompare(uint32_t Word, FloatCompareKind Kind, bool Scalar);
  bool SIMDHalfRound(uint32_t Word, FPRounding Mode);
  bool SIMDHalfToInt(uint32_t Word, uint8_t Rounding, bool Signed, bool Scalar, uint32_t FBits);
  bool SIMDIntToHalf(uint32_t Word, bool Signed, bool Scalar, uint32_t FBits);
  bool SIMDHalfFixedConvert(uint32_t Word, bool ToFloat, bool Signed, bool Scalar);
  bool SIMDHalfEstimate(uint32_t Word, bool Sqrt, bool Scalar);
  bool SIMDFloatRecpX(uint32_t Word, OpSize ES);
  bool SIMDUnsignedEstimate(uint32_t Word, bool Sqrt);
  bool SIMDFloatStep(uint32_t Word, bool Sqrt, bool Scalar);

  bool CRC32Common(uint32_t Word, bool Castagnoli);
  // Advanced SIMD saturating families (TranslateSIMDSaturate.cpp).
  enum class NarrowKind { Truncate, SignedToSigned, UnsignedToUnsigned, SignedToUnsigned };
  enum class ShiftLeftKind { Signed, Unsigned, SignedToUnsigned };
  enum class HalvingOp { UHAdd, SHAdd, URHAdd, SRHAdd, UHSub, SHSub };
  // Every ES lane holds Value.
  Ref LaneConstant(uint64_t Value, OpSize ES);
  // FPSR.QC |= (any bit of Mask set).
  void SetQCIfAny(Ref Mask);
  Ref UsedLanes(Ref Mask, bool Scalar, bool Q, OpSize ES);
  void StoreIntLanes(uint32_t Word, bool Scalar, OpSize ES, Ref Result);
  Ref SaturatingAddSub(OpSize ES, Ref A, Ref B, bool Sub, bool Signed, Ref* Saturated);
  Ref SaturateNarrow(Ref V, OpSize WideES, NarrowKind Kind, Ref* Saturated);
  Ref RoundingShiftRight(Ref V, OpSize ES, uint32_t Shift, bool Signed);
  Ref AbsoluteDifference(OpSize ES, Ref A, Ref B, bool Signed);
  Ref DoublingMultiplyHigh(OpSize ES, Ref A, Ref B, bool Rounding, Ref* Saturated);
  bool IntElementOperand(uint32_t Word, Ref* Element);
  bool SIMDSaturatingAddSub(uint32_t Word, bool Sub, bool Signed, bool Scalar);
  bool SIMDSaturatingAbsNeg(uint32_t Word, bool IsNeg, bool Scalar);
  bool SIMDSaturatingExtractNarrow(uint32_t Word, NarrowKind Kind, bool Scalar);
  bool SIMDShiftRightNarrow(uint32_t Word, NarrowKind Kind, bool Rounding, bool SignedShift, bool Scalar);
  bool SIMDRoundingShiftRight(uint32_t Word, bool Signed, bool Accumulate, bool Scalar);
  bool SIMDSaturatingShiftLeft(uint32_t Word, ShiftLeftKind Kind, bool Scalar);
  bool SIMDHalving(uint32_t Word, HalvingOp Op);
  bool SIMDAbsoluteDifference(uint32_t Word, bool Signed, bool Accumulate, bool Long);
  bool SIMDRoundingHighNarrow(uint32_t Word, bool Sub);
  bool SIMDDoublingMultiplyHigh(uint32_t Word, bool Rounding, bool Scalar, bool ByElement);
  bool SIMDMultiplyElement(uint32_t Word, int Accumulate);
  bool SIMDMultiplyLongElement(uint32_t Word, bool Signed, int Accumulate);
  bool SIMDAddLongAcrossLanes(uint32_t Word, bool Signed);
  bool SIMDSignedAcrossLanesMinMax(uint32_t Word, bool IsMax);
  bool SIMDCountLeading(uint32_t Word, bool Sign);
  bool SIMDDotProduct(uint32_t Word, bool Signed, bool ByElement);
  bool SIMDShiftByRegister(uint32_t Word, bool Signed, bool Rounding, bool Saturating, bool Scalar);
  bool SIMDSaturatingAccumulate(uint32_t Word, bool SignedAcc, bool Scalar);
  bool SIMDDoublingMultiplyLong(uint32_t Word, int Accumulate, bool Scalar, bool ByElement);

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

  // Last known value of each context-backed guest GPR (LoadGPRSlot).
  static constexpr uint64_t GPR_CACHE_WINDOW = 8 * INSTRUCTION_SIZE;
  struct GPRCacheEntry {
    Ref Value {};
    Ref Block {};
    uint64_t PC {};
  };
  std::array<GPRCacheEntry, 31> GPRCache {};
  bool BlockSetPC {};
  bool ShouldDump {};
};

} // namespace FEXCore::A64
