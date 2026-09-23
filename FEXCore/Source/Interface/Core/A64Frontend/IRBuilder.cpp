// SPDX-License-Identifier: MIT
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Context/Context.h"
#include "Interface/IR/RegisterAllocationData.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>

#include <algorithm>
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
  {"SVC", &IRBuilder::SVC}, {"BRK", &IRBuilder::BRK}, {"HLT", &IRBuilder::HLT},
  // System. Every hint (NOP, YIELD, WFE, WFI, SEV, SEVL, BTI, PAC*SP, ...) is a NOP.
  {"HINT", &IRBuilder::HINT}, {"NOP", &IRBuilder::HINT}, {"YIELD", &IRBuilder::HINT},
  {"WFE", &IRBuilder::HINT}, {"WFI", &IRBuilder::HINT}, {"SEV", &IRBuilder::HINT}, {"SEVL", &IRBuilder::HINT},
  // DSB/DMB carry ordering a weakly-ordered host must reproduce; only ISB is a
  // pure instruction-fetch barrier, which mtrack SMC tracking already covers.
  {"DSB", &IRBuilder::Barrier}, {"DMB", &IRBuilder::Barrier}, {"ISB", &IRBuilder::HINT},
  {"CLREX", &IRBuilder::CLREX},
  {"MRS", &IRBuilder::MRS}, {"MSR_reg", &IRBuilder::MSR_reg},
  {"DC_ZVA", &IRBuilder::DC_ZVA},
  {"DC_CVAU", &IRBuilder::CacheMaintenanceNop}, {"IC_IVAU", &IRBuilder::CacheMaintenanceNop},
  {"DC_CVAC", &IRBuilder::CacheMaintenanceNop}, {"DC_CIVAC", &IRBuilder::CacheMaintenanceNop},
  {"DC_CVAP", &IRBuilder::CacheMaintenanceNop},
  {"UnallocatedEncoding", &IRBuilder::UnallocatedEncoding},
  // Loads and stores.
  {"LDR_lit_gen", &IRBuilder::LDR_lit_gen}, {"LDRSW_lit", &IRBuilder::LDRSW_lit}, {"PRFM_lit", &IRBuilder::PRFM_lit},
  {"STP_LDP_gen", &IRBuilder::STP_LDP_gen}, {"STNP_LDNP_gen", &IRBuilder::STNP_LDNP_gen},
  {"STURx_LDURx", &IRBuilder::LoadStoreImm9}, {"STRx_LDRx_imm_1", &IRBuilder::LoadStoreImm9},
  {"STRx_LDRx_imm_2", &IRBuilder::STRx_LDRx_imm_2},
  {"STLURx_LDAPURx", &IRBuilder::STLURx_LDAPURx},
  {"PRFM_imm", &IRBuilder::PRFM_imm}, {"PRFM_unscaled_imm", &IRBuilder::PRFM_imm},
  // Unprivileged loads and stores behave as the unscaled forms at EL0.
  {"STTRB", &IRBuilder::LoadStoreImm9}, {"LDTRB", &IRBuilder::LoadStoreImm9}, {"LDTRSB", &IRBuilder::LoadStoreImm9},
  {"STTRH", &IRBuilder::LoadStoreImm9}, {"LDTRH", &IRBuilder::LoadStoreImm9}, {"LDTRSH", &IRBuilder::LoadStoreImm9},
  {"STTR", &IRBuilder::LoadStoreImm9}, {"LDTR", &IRBuilder::LoadStoreImm9}, {"LDTRSW", &IRBuilder::LoadStoreImm9},
  {"STRx_reg", &IRBuilder::LoadStoreRegOffset}, {"LDRx_reg", &IRBuilder::LoadStoreRegOffset},
  // Exclusive loads and stores (software monitor).
  {"LDXR", &IRBuilder::LoadExclusive}, {"LDAXR", &IRBuilder::LoadExclusive},
  {"STXR", &IRBuilder::StoreExclusive}, {"STLXR", &IRBuilder::StoreExclusive},
  {"LDXP", &IRBuilder::LoadExclusivePair}, {"LDAXP", &IRBuilder::LoadExclusivePair},
  {"STXP", &IRBuilder::StoreExclusivePair}, {"STLXP", &IRBuilder::StoreExclusivePair},
  {"LDAR", &IRBuilder::LoadStoreAtomicWidth}, {"LDLAR", &IRBuilder::LoadStoreAtomicWidth},
  // FEAT_LSE atomic memory operations, compare-and-swap and acquire load.
  {"LDADDB", &IRBuilder::AtomicMemOp}, {"LDCLRB", &IRBuilder::AtomicMemOp}, {"LDEORB", &IRBuilder::AtomicMemOp}, {"LDSETB", &IRBuilder::AtomicMemOp}, {"SWPB", &IRBuilder::AtomicMemOp},
  {"LDADDH", &IRBuilder::AtomicMemOp}, {"LDCLRH", &IRBuilder::AtomicMemOp}, {"LDEORH", &IRBuilder::AtomicMemOp}, {"LDSETH", &IRBuilder::AtomicMemOp}, {"SWPH", &IRBuilder::AtomicMemOp},
  {"LDADD", &IRBuilder::AtomicMemOp}, {"LDCLR", &IRBuilder::AtomicMemOp}, {"LDEOR", &IRBuilder::AtomicMemOp}, {"LDSET", &IRBuilder::AtomicMemOp}, {"SWP", &IRBuilder::AtomicMemOp},
  // The signed and unsigned min/max forms take the CAS retry loop instead.
  {"LDSMAXB", &IRBuilder::AtomicMinMax}, {"LDSMINB", &IRBuilder::AtomicMinMax}, {"LDUMAXB", &IRBuilder::AtomicMinMax}, {"LDUMINB", &IRBuilder::AtomicMinMax},
  {"LDSMAXH", &IRBuilder::AtomicMinMax}, {"LDSMINH", &IRBuilder::AtomicMinMax}, {"LDUMAXH", &IRBuilder::AtomicMinMax}, {"LDUMINH", &IRBuilder::AtomicMinMax},
  {"LDSMAX", &IRBuilder::AtomicMinMax}, {"LDSMIN", &IRBuilder::AtomicMinMax}, {"LDUMAX", &IRBuilder::AtomicMinMax}, {"LDUMIN", &IRBuilder::AtomicMinMax},
  {"LDAPRB", &IRBuilder::LDAPR}, {"LDAPRH", &IRBuilder::LDAPR}, {"LDAPR", &IRBuilder::LDAPR},
  {"CASB", &IRBuilder::CompareAndSwap}, {"CASH", &IRBuilder::CompareAndSwap}, {"CAS", &IRBuilder::CompareAndSwap},
  {"CASP", &IRBuilder::CompareAndSwapPair},
  {"STLR", &IRBuilder::LoadStoreAtomicWidth}, {"STLLR", &IRBuilder::LoadStoreAtomicWidth},
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
  // SIMD&FP register loads and stores.
  {"LDR_lit_fpsimd", &IRBuilder::LDR_lit_fpsimd}, {"STP_LDP_fpsimd", &IRBuilder::STP_LDP_fpsimd}, {"STNP_LDNP_fpsimd", &IRBuilder::STNP_LDNP_fpsimd},
  {"STUR_fpsimd", &IRBuilder::STUR_LDUR_fpsimd}, {"LDUR_fpsimd", &IRBuilder::STUR_LDUR_fpsimd},
  {"STR_imm_fpsimd_1", &IRBuilder::STR_LDR_imm_fpsimd_1}, {"LDR_imm_fpsimd_1", &IRBuilder::STR_LDR_imm_fpsimd_1},
  {"STR_imm_fpsimd_2", &IRBuilder::STR_LDR_imm_fpsimd_2}, {"LDR_imm_fpsimd_2", &IRBuilder::STR_LDR_imm_fpsimd_2},
  {"STR_reg_fpsimd", &IRBuilder::STR_LDR_reg_fpsimd}, {"LDR_reg_fpsimd", &IRBuilder::STR_LDR_reg_fpsimd},
  {"STx_mult_1", &IRBuilder::LDx_STx_mult}, {"STx_mult_2", &IRBuilder::LDx_STx_mult},
  {"LDx_mult_1", &IRBuilder::LDx_STx_mult}, {"LDx_mult_2", &IRBuilder::LDx_STx_mult},
  {"LD1_sngl_1", &IRBuilder::SIMDSingleStructure}, {"LD1_sngl_2", &IRBuilder::SIMDSingleStructure},
  {"LD2_sngl_1", &IRBuilder::SIMDSingleStructure}, {"LD2_sngl_2", &IRBuilder::SIMDSingleStructure},
  {"LD3_sngl_1", &IRBuilder::SIMDSingleStructure}, {"LD3_sngl_2", &IRBuilder::SIMDSingleStructure},
  {"LD4_sngl_1", &IRBuilder::SIMDSingleStructure}, {"LD4_sngl_2", &IRBuilder::SIMDSingleStructure},
  {"ST1_sngl_1", &IRBuilder::SIMDSingleStructure}, {"ST1_sngl_2", &IRBuilder::SIMDSingleStructure},
  {"ST2_sngl_1", &IRBuilder::SIMDSingleStructure}, {"ST2_sngl_2", &IRBuilder::SIMDSingleStructure},
  {"ST3_sngl_1", &IRBuilder::SIMDSingleStructure}, {"ST3_sngl_2", &IRBuilder::SIMDSingleStructure},
  {"ST4_sngl_1", &IRBuilder::SIMDSingleStructure}, {"ST4_sngl_2", &IRBuilder::SIMDSingleStructure},
  {"LD1R_1", &IRBuilder::SIMDSingleStructure}, {"LD1R_2", &IRBuilder::SIMDSingleStructure},
  {"LD2R_1", &IRBuilder::SIMDSingleStructure}, {"LD2R_2", &IRBuilder::SIMDSingleStructure},
  {"LD3R_1", &IRBuilder::SIMDSingleStructure}, {"LD3R_2", &IRBuilder::SIMDSingleStructure},
  {"LD4R_1", &IRBuilder::SIMDSingleStructure}, {"LD4R_2", &IRBuilder::SIMDSingleStructure},
  // Advanced SIMD. The translated set is the subset measured in
  // docs/powerarm/M1b-SIMD-SUBSET.md plus its cheap neighbours.
  // POWERARM-M1-TODO(simd): no translator yet for FCADD/FCMLA and the SHA-512/SHA-3/SM3/SM4 entries (neither on the reference A76).
  // Advanced SIMD: copy.
  {"DUP_gen", &IRBuilder::DUP_gen}, {"DUP_elt_1", &IRBuilder::DUP_elt_1}, {"DUP_elt_2", &IRBuilder::DUP_elt_2},
  {"UMOV", &IRBuilder::UMOV}, {"SMOV", &IRBuilder::SMOV}, {"INS_gen", &IRBuilder::INS_gen}, {"INS_elt", &IRBuilder::INS_elt},
  // Advanced SIMD: modified immediate.
  {"MOVI", &IRBuilder::MOVI}, {"FMOV_2", &IRBuilder::FMOV_vec_imm},
  // Advanced SIMD: three same.
  {"ADD_vector", &IRBuilder::ADD_vector}, {"SUB_2", &IRBuilder::SUB_2},
  {"CMEQ_reg_2", &IRBuilder::CMEQ_reg_2}, {"CMGT_reg_2", &IRBuilder::CMGT_reg_2}, {"CMGE_reg_2", &IRBuilder::CMGE_reg_2},
  {"CMHS_2", &IRBuilder::CMHS_2}, {"CMHI_2", &IRBuilder::CMHI_2}, {"CMTST_2", &IRBuilder::CMTST_2},
  {"UMAX", &IRBuilder::UMAX}, {"UMIN", &IRBuilder::UMIN}, {"SMAX", &IRBuilder::SMAX}, {"SMIN", &IRBuilder::SMIN},
  {"ADD_1", &IRBuilder::ADD_1}, {"SUB_1", &IRBuilder::SUB_1},
  {"CMEQ_reg_1", &IRBuilder::CMEQ_reg_1}, {"CMGT_reg_1", &IRBuilder::CMGT_reg_1}, {"CMGE_reg_1", &IRBuilder::CMGE_reg_1},
  {"CMHS_1", &IRBuilder::CMHS_1}, {"CMHI_1", &IRBuilder::CMHI_1}, {"CMTST_1", &IRBuilder::CMTST_1},
  {"AND_asimd", &IRBuilder::SIMDLogical}, {"BIC_asimd_reg", &IRBuilder::SIMDLogical},
  {"ORR_asimd_reg", &IRBuilder::SIMDLogical}, {"ORN_asimd", &IRBuilder::SIMDLogical},
  {"EOR_asimd", &IRBuilder::SIMDLogical}, {"BSL", &IRBuilder::SIMDLogical},
  {"BIT", &IRBuilder::SIMDLogical}, {"BIF", &IRBuilder::SIMDLogical},
  {"ADDP_vec", &IRBuilder::ADDP_vec}, {"UMAXP", &IRBuilder::UMAXP}, {"UMINP", &IRBuilder::UMINP},
  // Advanced SIMD: across lanes.
  {"ADDV", &IRBuilder::ADDV}, {"UMAXV", &IRBuilder::UMAXV}, {"UMINV", &IRBuilder::UMINV},
  {"FMAXV_2", &IRBuilder::FMAXV_2}, {"FMINV_2", &IRBuilder::FMINV_2},
  {"FMAXNMV_2", &IRBuilder::FMAXNMV_2}, {"FMINNMV_2", &IRBuilder::FMINNMV_2},
  // Advanced SIMD: two-register misc.
  {"CMEQ_zero_2", &IRBuilder::CMEQ_zero_2}, {"CMGT_zero_2", &IRBuilder::CMGT_zero_2}, {"CMGE_zero_2", &IRBuilder::CMGE_zero_2},
  {"CMLE_2", &IRBuilder::CMLE_2}, {"CMLT_2", &IRBuilder::CMLT_2},
  {"CMEQ_zero_1", &IRBuilder::CMEQ_zero_1}, {"CMGT_zero_1", &IRBuilder::CMGT_zero_1}, {"CMGE_zero_1", &IRBuilder::CMGE_zero_1},
  {"CMLE_1", &IRBuilder::CMLE_1}, {"CMLT_1", &IRBuilder::CMLT_1},
  {"CNT", &IRBuilder::CNT}, {"RBIT_asimd", &IRBuilder::RBIT_asimd}, {"NOT", &IRBuilder::NOT}, {"NEG_2", &IRBuilder::NEG_2}, {"ABS_2", &IRBuilder::ABS_2},
  {"REV64_asimd", &IRBuilder::REV64_asimd}, {"REV32_asimd", &IRBuilder::REV32_asimd},
  {"XTN", &IRBuilder::XTN},
  // Advanced SIMD: three different.
  {"SADDL", &IRBuilder::SADDL}, {"UADDL", &IRBuilder::UADDL}, {"SSUBL", &IRBuilder::SSUBL}, {"USUBL", &IRBuilder::USUBL},
  {"SADDW", &IRBuilder::SADDW}, {"UADDW", &IRBuilder::UADDW}, {"ADDHN", &IRBuilder::ADDHN}, {"SUBHN", &IRBuilder::SUBHN},
  // Advanced SIMD: shift by immediate.
  {"SSHR_2", &IRBuilder::SSHR_2}, {"USHR_2", &IRBuilder::USHR_2}, {"SHL_2", &IRBuilder::SHL_2},
  {"SSHR_1", &IRBuilder::SSHR_1}, {"USHR_1", &IRBuilder::USHR_1}, {"SHL_1", &IRBuilder::SHL_1},
  {"SHRN", &IRBuilder::SHRN}, {"SSHLL", &IRBuilder::SSHLL}, {"USHLL", &IRBuilder::USHLL},
  // Advanced SIMD: extract and permute.
  {"EXT", &IRBuilder::EXT},
  {"UZP1", &IRBuilder::UZP1}, {"UZP2", &IRBuilder::UZP2}, {"ZIP1", &IRBuilder::ZIP1}, {"ZIP2", &IRBuilder::ZIP2},
  {"TRN1", &IRBuilder::TRN1}, {"TRN2", &IRBuilder::TRN2},
  {"TBL", &IRBuilder::TBL}, {"TBX", &IRBuilder::TBX},
  // Advanced SIMD: M2 census gaps.
  {"USHL_2", &IRBuilder::USHL_2}, {"SSHL_2", &IRBuilder::SSHL_2}, {"USHL_1", &IRBuilder::USHL_1}, {"SSHL_1", &IRBuilder::SSHL_1},
  {"SRI_2", &IRBuilder::SRI_2}, {"SLI_2", &IRBuilder::SLI_2}, {"USRA_2", &IRBuilder::USRA_2}, {"SSRA_2", &IRBuilder::SSRA_2},
  {"UADDLP", &IRBuilder::UADDLP}, {"SADDLP", &IRBuilder::SADDLP}, {"UADALP", &IRBuilder::UADALP}, {"SADALP", &IRBuilder::SADALP},
  {"SSUBW", &IRBuilder::SSUBW}, {"USUBW", &IRBuilder::USUBW},
  {"UMULL_vec", &IRBuilder::UMULL_vec}, {"SMULL_vec", &IRBuilder::SMULL_vec}, {"UMLAL_vec", &IRBuilder::UMLAL_vec},
  {"SMLAL_vec", &IRBuilder::SMLAL_vec}, {"UMLSL_vec", &IRBuilder::UMLSL_vec}, {"SMLSL_vec", &IRBuilder::SMLSL_vec},
  {"MUL_vec", &IRBuilder::MUL_vec}, {"MLA_vec", &IRBuilder::MLA_vec}, {"MLS_vec", &IRBuilder::MLS_vec}, {"MUL_elt", &IRBuilder::MUL_elt},
  {"SMAXP", &IRBuilder::SMAXP}, {"SMINP", &IRBuilder::SMINP}, {"REV16_asimd", &IRBuilder::REV16_asimd},
  {"NEG_1", &IRBuilder::NEG_1}, {"ABS_1", &IRBuilder::ABS_1}, {"ADDP_pair", &IRBuilder::ADDP_pair}, {"UQSUB_1", &IRBuilder::UQSUB_1},
  {"FNEG_2", &IRBuilder::FNEG_2}, {"FABS_2", &IRBuilder::FABS_2}, {"FABD_2", &IRBuilder::FABD_2}, {"FABD_4", &IRBuilder::FABD_4},
  {"SCVTF_int_4", &IRBuilder::SCVTF_int_4}, {"UCVTF_int_4", &IRBuilder::UCVTF_int_4},
  // Advanced SIMD: saturating, rounding, halving, by-element and across-lane families.
  {"SQADD_2", &IRBuilder::SQADD_2}, {"SQSUB_2", &IRBuilder::SQSUB_2}, {"UQADD_2", &IRBuilder::UQADD_2}, {"UQSUB_2", &IRBuilder::UQSUB_2}, {"SQADD_1", &IRBuilder::SQADD_1}, {"SQSUB_1", &IRBuilder::SQSUB_1}, {"UQADD_1", &IRBuilder::UQADD_1}, {"SQABS_2", &IRBuilder::SQABS_2}, {"SQNEG_2", &IRBuilder::SQNEG_2}, {"SQABS_1", &IRBuilder::SQABS_1}, {"SQNEG_1", &IRBuilder::SQNEG_1},
  {"SQXTN_2", &IRBuilder::SQXTN_2}, {"UQXTN_2", &IRBuilder::UQXTN_2}, {"SQXTUN_2", &IRBuilder::SQXTUN_2}, {"SQXTN_1", &IRBuilder::SQXTN_1}, {"UQXTN_1", &IRBuilder::UQXTN_1}, {"SQXTUN_1", &IRBuilder::SQXTUN_1},
  {"RSHRN", &IRBuilder::RSHRN}, {"SQSHRN_2", &IRBuilder::SQSHRN_2}, {"SQRSHRN_2", &IRBuilder::SQRSHRN_2}, {"UQSHRN_2", &IRBuilder::UQSHRN_2}, {"UQRSHRN_2", &IRBuilder::UQRSHRN_2}, {"SQSHRUN_2", &IRBuilder::SQSHRUN_2}, {"SQRSHRUN_2", &IRBuilder::SQRSHRUN_2}, {"SQSHRN_1", &IRBuilder::SQSHRN_1}, {"UQSHRN_1", &IRBuilder::UQSHRN_1}, {"SQSHRUN_1", &IRBuilder::SQSHRUN_1}, {"SQRSHRN_1", &IRBuilder::SQRSHRN_1}, {"UQRSHRN_1", &IRBuilder::UQRSHRN_1}, {"SQRSHRUN_1", &IRBuilder::SQRSHRUN_1},
  {"SRI_1", &IRBuilder::SRI_1}, {"SLI_1", &IRBuilder::SLI_1}, {"USRA_1", &IRBuilder::USRA_1}, {"SSRA_1", &IRBuilder::SSRA_1},
  {"SRSHR_2", &IRBuilder::SRSHR_2}, {"URSHR_2", &IRBuilder::URSHR_2}, {"SRSRA_2", &IRBuilder::SRSRA_2}, {"URSRA_2", &IRBuilder::URSRA_2}, {"SRSHR_1", &IRBuilder::SRSHR_1}, {"URSHR_1", &IRBuilder::URSHR_1}, {"SRSRA_1", &IRBuilder::SRSRA_1}, {"URSRA_1", &IRBuilder::URSRA_1},
  {"SQSHL_imm_2", &IRBuilder::SQSHL_imm_2}, {"UQSHL_imm_2", &IRBuilder::UQSHL_imm_2}, {"SQSHLU_2", &IRBuilder::SQSHLU_2}, {"SQSHL_imm_1", &IRBuilder::SQSHL_imm_1}, {"UQSHL_imm_1", &IRBuilder::UQSHL_imm_1}, {"SQSHLU_1", &IRBuilder::SQSHLU_1},
  {"UHADD", &IRBuilder::UHADD}, {"SHADD", &IRBuilder::SHADD}, {"URHADD", &IRBuilder::URHADD}, {"SRHADD", &IRBuilder::SRHADD}, {"UHSUB", &IRBuilder::UHSUB}, {"SHSUB", &IRBuilder::SHSUB}, {"UABD", &IRBuilder::UABD}, {"SABD", &IRBuilder::SABD}, {"UABA", &IRBuilder::UABA}, {"SABA", &IRBuilder::SABA}, {"UABDL", &IRBuilder::UABDL}, {"SABDL", &IRBuilder::SABDL}, {"UABAL", &IRBuilder::UABAL}, {"SABAL", &IRBuilder::SABAL}, {"RADDHN", &IRBuilder::RADDHN}, {"RSUBHN", &IRBuilder::RSUBHN},
  {"SQDMULH_vec_2", &IRBuilder::SQDMULH_vec_2}, {"SQRDMULH_vec_2", &IRBuilder::SQRDMULH_vec_2}, {"SQDMULH_vec_1", &IRBuilder::SQDMULH_vec_1}, {"SQRDMULH_vec_1", &IRBuilder::SQRDMULH_vec_1}, {"SQDMULH_elt_2", &IRBuilder::SQDMULH_elt_2}, {"SQRDMULH_elt_2", &IRBuilder::SQRDMULH_elt_2}, {"SQDMULH_elt_1", &IRBuilder::SQDMULH_elt_1}, {"SQRDMULH_elt_1", &IRBuilder::SQRDMULH_elt_1},
  {"SQRDMLAH_vec_2", &IRBuilder::SQRDMLAH_vec_2}, {"SQRDMLAH_vec_1", &IRBuilder::SQRDMLAH_vec_1}, {"SQRDMLSH_vec_2", &IRBuilder::SQRDMLSH_vec_2}, {"SQRDMLSH_vec_1", &IRBuilder::SQRDMLSH_vec_1}, {"SQRDMLAH_elt_2", &IRBuilder::SQRDMLAH_elt_2}, {"SQRDMLAH_elt_1", &IRBuilder::SQRDMLAH_elt_1}, {"SQRDMLSH_elt_2", &IRBuilder::SQRDMLSH_elt_2}, {"SQRDMLSH_elt_1", &IRBuilder::SQRDMLSH_elt_1},
  {"MLA_elt", &IRBuilder::MLA_elt}, {"MLS_elt", &IRBuilder::MLS_elt}, {"SMULL_elt", &IRBuilder::SMULL_elt}, {"UMULL_elt", &IRBuilder::UMULL_elt}, {"SMLAL_elt", &IRBuilder::SMLAL_elt}, {"UMLAL_elt", &IRBuilder::UMLAL_elt}, {"SMLSL_elt", &IRBuilder::SMLSL_elt}, {"UMLSL_elt", &IRBuilder::UMLSL_elt},
  {"UADDLV", &IRBuilder::UADDLV}, {"SADDLV", &IRBuilder::SADDLV}, {"SMAXV", &IRBuilder::SMAXV}, {"SMINV", &IRBuilder::SMINV}, {"CLZ_asimd", &IRBuilder::CLZ_asimd}, {"CLS_asimd", &IRBuilder::CLS_asimd}, {"UDOT_vec", &IRBuilder::UDOT_vec}, {"SHLL", &IRBuilder::SHLL},
  {"SDOT_vec", &IRBuilder::SDOT_vec}, {"UDOT_elt", &IRBuilder::UDOT_elt}, {"SDOT_elt", &IRBuilder::SDOT_elt}, {"PMUL", &IRBuilder::PMUL},
  {"SQSHL_reg_2", &IRBuilder::SQSHL_reg_2}, {"UQSHL_reg_2", &IRBuilder::UQSHL_reg_2}, {"SRSHL_2", &IRBuilder::SRSHL_2}, {"URSHL_2", &IRBuilder::URSHL_2}, {"SQRSHL_2", &IRBuilder::SQRSHL_2}, {"UQRSHL_2", &IRBuilder::UQRSHL_2},
  {"SQSHL_reg_1", &IRBuilder::SQSHL_reg_1}, {"UQSHL_reg_1", &IRBuilder::UQSHL_reg_1}, {"SRSHL_1", &IRBuilder::SRSHL_1}, {"URSHL_1", &IRBuilder::URSHL_1}, {"SQRSHL_1", &IRBuilder::SQRSHL_1}, {"UQRSHL_1", &IRBuilder::UQRSHL_1},
  {"SUQADD_1", &IRBuilder::SUQADD_1}, {"SUQADD_2", &IRBuilder::SUQADD_2}, {"USQADD_1", &IRBuilder::USQADD_1}, {"USQADD_2", &IRBuilder::USQADD_2},
  {"SQDMULL_vec_1", &IRBuilder::SQDMULL_vec_1}, {"SQDMULL_vec_2", &IRBuilder::SQDMULL_vec_2}, {"SQDMLAL_vec_1", &IRBuilder::SQDMLAL_vec_1}, {"SQDMLAL_vec_2", &IRBuilder::SQDMLAL_vec_2}, {"SQDMLSL_vec_1", &IRBuilder::SQDMLSL_vec_1}, {"SQDMLSL_vec_2", &IRBuilder::SQDMLSL_vec_2},
  {"SQDMULL_elt_1", &IRBuilder::SQDMULL_elt_1}, {"SQDMULL_elt_2", &IRBuilder::SQDMULL_elt_2}, {"SQDMLAL_elt_1", &IRBuilder::SQDMLAL_elt_1}, {"SQDMLAL_elt_2", &IRBuilder::SQDMLAL_elt_2}, {"SQDMLSL_elt_1", &IRBuilder::SQDMLSL_elt_1}, {"SQDMLSL_elt_2", &IRBuilder::SQDMLSL_elt_2},
  {"FMULX_vec_2", &IRBuilder::FMULX_vec_2}, {"FMULX_vec_4", &IRBuilder::FMULX_vec_4}, {"FMULX_elt_2", &IRBuilder::FMULX_elt_2}, {"FMULX_elt_4", &IRBuilder::FMULX_elt_4},
  // Cryptographic extension and CRC32.
  {"AESE", &IRBuilder::AESE}, {"AESD", &IRBuilder::AESD}, {"AESMC", &IRBuilder::AESMC}, {"AESIMC", &IRBuilder::AESIMC}, {"PMULL", &IRBuilder::PMULL}, {"SHA1C", &IRBuilder::SHA1C}, {"SHA1M", &IRBuilder::SHA1M}, {"SHA1P", &IRBuilder::SHA1P}, {"SHA1H", &IRBuilder::SHA1H}, {"SHA1SU0", &IRBuilder::SHA1SU0}, {"SHA1SU1", &IRBuilder::SHA1SU1},
  {"SHA256H", &IRBuilder::SHA256H}, {"SHA256H2", &IRBuilder::SHA256H2}, {"SHA256SU0", &IRBuilder::SHA256SU0}, {"SHA256SU1", &IRBuilder::SHA256SU1}, {"CRC32", &IRBuilder::CRC32}, {"CRC32C", &IRBuilder::CRC32C},
  // Advanced SIMD: floating point.
  {"FADD_2", &IRBuilder::FADD_2}, {"FSUB_2", &IRBuilder::FSUB_2}, {"FMUL_vec_2", &IRBuilder::FMUL_vec_2}, {"FDIV_2", &IRBuilder::FDIV_2}, {"FMIN_2", &IRBuilder::FMIN_2}, {"FMAX_2", &IRBuilder::FMAX_2}, {"FMINNM_2", &IRBuilder::FMINNM_2}, {"FMAXNM_2", &IRBuilder::FMAXNM_2}, {"FMUL_elt_4", &IRBuilder::FMUL_elt_4}, {"FMUL_elt_2", &IRBuilder::FMUL_elt_2}, {"FMLA_elt_4", &IRBuilder::FMLA_elt_4}, {"FMLA_elt_2", &IRBuilder::FMLA_elt_2}, {"FMLS_elt_4", &IRBuilder::FMLS_elt_4}, {"FMLS_elt_2", &IRBuilder::FMLS_elt_2}, {"FMLA_vec_2", &IRBuilder::FMLA_vec_2}, {"FMLS_vec_2", &IRBuilder::FMLS_vec_2},
  {"FADDP_vec_2", &IRBuilder::FADDP_vec_2}, {"FMAXP_vec_2", &IRBuilder::FMAXP_vec_2}, {"FMINP_vec_2", &IRBuilder::FMINP_vec_2}, {"FMAXNMP_vec_2", &IRBuilder::FMAXNMP_vec_2}, {"FMINNMP_vec_2", &IRBuilder::FMINNMP_vec_2}, {"FADDP_pair_2", &IRBuilder::FADDP_pair_2}, {"FMAXP_pair_2", &IRBuilder::FMAXP_pair_2}, {"FMINP_pair_2", &IRBuilder::FMINP_pair_2}, {"FMAXNMP_pair_2", &IRBuilder::FMAXNMP_pair_2}, {"FMINNMP_pair_2", &IRBuilder::FMINNMP_pair_2},
  {"FCMEQ_reg_4", &IRBuilder::FCMEQ_reg_4}, {"FCMGE_reg_4", &IRBuilder::FCMGE_reg_4}, {"FCMGT_reg_4", &IRBuilder::FCMGT_reg_4}, {"FACGE_4", &IRBuilder::FACGE_4}, {"FACGT_4", &IRBuilder::FACGT_4}, {"FCMEQ_zero_4", &IRBuilder::FCMEQ_zero_4}, {"FCMGE_zero_4", &IRBuilder::FCMGE_zero_4}, {"FCMGT_zero_4", &IRBuilder::FCMGT_zero_4}, {"FCMLE_4", &IRBuilder::FCMLE_4}, {"FCMLT_4", &IRBuilder::FCMLT_4},
  {"FCMEQ_reg_2", &IRBuilder::FCMEQ_reg_2}, {"FCMGE_reg_2", &IRBuilder::FCMGE_reg_2}, {"FCMGT_reg_2", &IRBuilder::FCMGT_reg_2}, {"FACGE_2", &IRBuilder::FACGE_2}, {"FACGT_2", &IRBuilder::FACGT_2}, {"FCMEQ_zero_2", &IRBuilder::FCMEQ_zero_2}, {"FCMGE_zero_2", &IRBuilder::FCMGE_zero_2}, {"FCMGT_zero_2", &IRBuilder::FCMGT_zero_2}, {"FCMLE_2", &IRBuilder::FCMLE_2}, {"FCMLT_2", &IRBuilder::FCMLT_2},
  {"FRINTN_2", &IRBuilder::FRINTN_2}, {"FRINTP_2", &IRBuilder::FRINTP_2}, {"FRINTM_2", &IRBuilder::FRINTM_2}, {"FRINTZ_2", &IRBuilder::FRINTZ_2}, {"FRINTA_2", &IRBuilder::FRINTA_2}, {"FRINTX_2", &IRBuilder::FRINTX_2}, {"FRINTI_2", &IRBuilder::FRINTI_2}, {"FSQRT_2", &IRBuilder::FSQRT_2}, {"FNEG_1", &IRBuilder::FNEG_1}, {"FABS_1", &IRBuilder::FABS_1},
  {"FCVTNS_4", &IRBuilder::FCVTNS_4}, {"FCVTNU_4", &IRBuilder::FCVTNU_4}, {"FCVTPS_4", &IRBuilder::FCVTPS_4}, {"FCVTPU_4", &IRBuilder::FCVTPU_4}, {"FCVTMS_4", &IRBuilder::FCVTMS_4}, {"FCVTMU_4", &IRBuilder::FCVTMU_4}, {"FCVTZS_int_4", &IRBuilder::FCVTZS_int_4}, {"FCVTZU_int_4", &IRBuilder::FCVTZU_int_4}, {"FCVTAS_4", &IRBuilder::FCVTAS_4}, {"FCVTAU_4", &IRBuilder::FCVTAU_4},
  {"FCVTNS_2", &IRBuilder::FCVTNS_2}, {"FCVTNU_2", &IRBuilder::FCVTNU_2}, {"FCVTPS_2", &IRBuilder::FCVTPS_2}, {"FCVTPU_2", &IRBuilder::FCVTPU_2}, {"FCVTMS_2", &IRBuilder::FCVTMS_2}, {"FCVTMU_2", &IRBuilder::FCVTMU_2}, {"FCVTAS_2", &IRBuilder::FCVTAS_2}, {"FCVTAU_2", &IRBuilder::FCVTAU_2},
  {"SCVTF_fix_2", &IRBuilder::SCVTF_fix_2}, {"UCVTF_fix_2", &IRBuilder::UCVTF_fix_2}, {"FCVTZS_fix_2", &IRBuilder::FCVTZS_fix_2}, {"FCVTZU_fix_2", &IRBuilder::FCVTZU_fix_2}, {"SCVTF_fix_1", &IRBuilder::SCVTF_fix_1}, {"UCVTF_fix_1", &IRBuilder::UCVTF_fix_1}, {"FCVTZS_fix_1", &IRBuilder::FCVTZS_fix_1}, {"FCVTZU_fix_1", &IRBuilder::FCVTZU_fix_1},
  // Advanced SIMD: estimates and Newton-Raphson steps.
  {"FRECPE_2", &IRBuilder::FRECPE_2}, {"FRECPE_4", &IRBuilder::FRECPE_4}, {"FRSQRTE_2", &IRBuilder::FRSQRTE_2}, {"FRSQRTE_4", &IRBuilder::FRSQRTE_4}, {"FRECPX_2", &IRBuilder::FRECPX_2}, {"URECPE", &IRBuilder::URECPE}, {"URSQRTE", &IRBuilder::URSQRTE},
  {"FRECPS_2", &IRBuilder::FRECPS_2}, {"FRECPS_4", &IRBuilder::FRECPS_4}, {"FRSQRTS_2", &IRBuilder::FRSQRTS_2}, {"FRSQRTS_4", &IRBuilder::FRSQRTS_4},
  // Advanced SIMD: half precision.
  {"FRECPE_1", &IRBuilder::FRECPE_1}, {"FRECPE_3", &IRBuilder::FRECPE_3}, {"FRSQRTE_1", &IRBuilder::FRSQRTE_1}, {"FRSQRTE_3", &IRBuilder::FRSQRTE_3}, {"FRECPX_1", &IRBuilder::FRECPX_1},
  {"FADD_1", &IRBuilder::FADD_1}, {"FSUB_1", &IRBuilder::FSUB_1}, {"FMUL_vec_1", &IRBuilder::FMUL_vec_1}, {"FDIV_1", &IRBuilder::FDIV_1}, {"FMIN_1", &IRBuilder::FMIN_1}, {"FMAX_1", &IRBuilder::FMAX_1}, {"FMINNM_1", &IRBuilder::FMINNM_1}, {"FMAXNM_1", &IRBuilder::FMAXNM_1},
  {"FMULX_vec_3", &IRBuilder::FMULX_vec_3}, {"FMULX_vec_1", &IRBuilder::FMULX_vec_1}, {"FABD_3", &IRBuilder::FABD_3}, {"FABD_1", &IRBuilder::FABD_1}, {"FRECPS_3", &IRBuilder::FRECPS_3}, {"FRECPS_1", &IRBuilder::FRECPS_1}, {"FRSQRTS_3", &IRBuilder::FRSQRTS_3}, {"FRSQRTS_1", &IRBuilder::FRSQRTS_1},
  {"FMLA_vec_1", &IRBuilder::FMLA_vec_1}, {"FMLS_vec_1", &IRBuilder::FMLS_vec_1}, {"FMLA_elt_3", &IRBuilder::FMLA_elt_3}, {"FMLS_elt_3", &IRBuilder::FMLS_elt_3}, {"FMLA_elt_1", &IRBuilder::FMLA_elt_1}, {"FMLS_elt_1", &IRBuilder::FMLS_elt_1},
  {"FMUL_elt_3", &IRBuilder::FMUL_elt_3}, {"FMUL_elt_1", &IRBuilder::FMUL_elt_1}, {"FMULX_elt_3", &IRBuilder::FMULX_elt_3}, {"FMULX_elt_1", &IRBuilder::FMULX_elt_1},
  {"FADDP_vec_1", &IRBuilder::FADDP_vec_1}, {"FMAXP_vec_1", &IRBuilder::FMAXP_vec_1}, {"FMINP_vec_1", &IRBuilder::FMINP_vec_1}, {"FMAXNMP_vec_1", &IRBuilder::FMAXNMP_vec_1}, {"FMINNMP_vec_1", &IRBuilder::FMINNMP_vec_1},
  {"FADDP_pair_1", &IRBuilder::FADDP_pair_1}, {"FMAXP_pair_1", &IRBuilder::FMAXP_pair_1}, {"FMINP_pair_1", &IRBuilder::FMINP_pair_1}, {"FMAXNMP_pair_1", &IRBuilder::FMAXNMP_pair_1}, {"FMINNMP_pair_1", &IRBuilder::FMINNMP_pair_1},
  {"FMAXV_1", &IRBuilder::FMAXV_1}, {"FMINV_1", &IRBuilder::FMINV_1}, {"FMAXNMV_1", &IRBuilder::FMAXNMV_1}, {"FMINNMV_1", &IRBuilder::FMINNMV_1},
  {"FCMEQ_reg_3", &IRBuilder::FCMEQ_reg_3}, {"FCMGE_reg_3", &IRBuilder::FCMGE_reg_3}, {"FCMGT_reg_3", &IRBuilder::FCMGT_reg_3}, {"FACGE_3", &IRBuilder::FACGE_3}, {"FACGT_3", &IRBuilder::FACGT_3},
  {"FCMEQ_zero_3", &IRBuilder::FCMEQ_zero_3}, {"FCMGE_zero_3", &IRBuilder::FCMGE_zero_3}, {"FCMGT_zero_3", &IRBuilder::FCMGT_zero_3}, {"FCMLE_3", &IRBuilder::FCMLE_3}, {"FCMLT_3", &IRBuilder::FCMLT_3},
  {"FCMEQ_reg_1", &IRBuilder::FCMEQ_reg_1}, {"FCMGE_reg_1", &IRBuilder::FCMGE_reg_1}, {"FCMGT_reg_1", &IRBuilder::FCMGT_reg_1}, {"FACGE_1", &IRBuilder::FACGE_1}, {"FACGT_1", &IRBuilder::FACGT_1},
  {"FCMEQ_zero_1", &IRBuilder::FCMEQ_zero_1}, {"FCMGE_zero_1", &IRBuilder::FCMGE_zero_1}, {"FCMGT_zero_1", &IRBuilder::FCMGT_zero_1}, {"FCMLE_1", &IRBuilder::FCMLE_1}, {"FCMLT_1", &IRBuilder::FCMLT_1},
  {"FRINTN_1", &IRBuilder::FRINTN_1}, {"FRINTP_1", &IRBuilder::FRINTP_1}, {"FRINTM_1", &IRBuilder::FRINTM_1}, {"FRINTZ_1", &IRBuilder::FRINTZ_1}, {"FRINTA_1", &IRBuilder::FRINTA_1}, {"FRINTX_1", &IRBuilder::FRINTX_1}, {"FRINTI_1", &IRBuilder::FRINTI_1}, {"FSQRT_1", &IRBuilder::FSQRT_1},
  {"FCVTNS_3", &IRBuilder::FCVTNS_3}, {"FCVTNU_3", &IRBuilder::FCVTNU_3}, {"FCVTPS_3", &IRBuilder::FCVTPS_3}, {"FCVTPU_3", &IRBuilder::FCVTPU_3}, {"FCVTMS_3", &IRBuilder::FCVTMS_3}, {"FCVTMU_3", &IRBuilder::FCVTMU_3}, {"FCVTZS_int_3", &IRBuilder::FCVTZS_int_3}, {"FCVTZU_int_3", &IRBuilder::FCVTZU_int_3}, {"FCVTAS_3", &IRBuilder::FCVTAS_3}, {"FCVTAU_3", &IRBuilder::FCVTAU_3},
  {"FCVTNS_1", &IRBuilder::FCVTNS_1}, {"FCVTNU_1", &IRBuilder::FCVTNU_1}, {"FCVTPS_1", &IRBuilder::FCVTPS_1}, {"FCVTPU_1", &IRBuilder::FCVTPU_1}, {"FCVTMS_1", &IRBuilder::FCVTMS_1}, {"FCVTMU_1", &IRBuilder::FCVTMU_1}, {"FCVTZS_int_1", &IRBuilder::FCVTZS_int_1}, {"FCVTZU_int_1", &IRBuilder::FCVTZU_int_1}, {"FCVTAS_1", &IRBuilder::FCVTAS_1}, {"FCVTAU_1", &IRBuilder::FCVTAU_1},
  {"SCVTF_int_3", &IRBuilder::SCVTF_int_3}, {"UCVTF_int_3", &IRBuilder::UCVTF_int_3}, {"SCVTF_int_1", &IRBuilder::SCVTF_int_1}, {"UCVTF_int_1", &IRBuilder::UCVTF_int_1}, {"FMOV_3", &IRBuilder::FMOV_3},
  // Scalar floating point.
  {"FMOV_float_gen", &IRBuilder::FMOV_float_gen}, {"FMOV_float", &IRBuilder::FMOV_float}, {"FMOV_float_imm", &IRBuilder::FMOV_float_imm},
  {"FABS_float", &IRBuilder::FABS_float}, {"FNEG_float", &IRBuilder::FNEG_float}, {"FSQRT_float", &IRBuilder::FSQRT_float},
  {"FCVT_float", &IRBuilder::FCVT_float},
  {"FRINTN_float", &IRBuilder::FRINTN_float}, {"FRINTP_float", &IRBuilder::FRINTP_float}, {"FRINTM_float", &IRBuilder::FRINTM_float},
  {"FRINTZ_float", &IRBuilder::FRINTZ_float}, {"FRINTA_float", &IRBuilder::FRINTA_float}, {"FRINTX_float", &IRBuilder::FRINTX_float},
  {"FRINTI_float", &IRBuilder::FRINTI_float},
  {"FADD_float", &IRBuilder::FADD_float}, {"FSUB_float", &IRBuilder::FSUB_float}, {"FMUL_float", &IRBuilder::FMUL_float},
  {"FDIV_float", &IRBuilder::FDIV_float}, {"FNMUL_float", &IRBuilder::FNMUL_float},
  {"FMIN_float", &IRBuilder::FMIN_float}, {"FMAX_float", &IRBuilder::FMAX_float},
  {"FMINNM_float", &IRBuilder::FMINNM_float}, {"FMAXNM_float", &IRBuilder::FMAXNM_float},
  {"FMADD_float", &IRBuilder::FPThreeRegister}, {"FMSUB_float", &IRBuilder::FPThreeRegister},
  {"FNMADD_float", &IRBuilder::FPThreeRegister}, {"FNMSUB_float", &IRBuilder::FPThreeRegister},
  {"FCMP_float", &IRBuilder::FCMP_float}, {"FCMPE_float", &IRBuilder::FCMP_float},
  {"FCCMP_float", &IRBuilder::FCCMP_float}, {"FCCMPE_float", &IRBuilder::FCCMP_float},
  {"FCSEL_float", &IRBuilder::FCSEL_float},
  {"FCVTNS_float", &IRBuilder::FCVTNS_float}, {"FCVTNU_float", &IRBuilder::FCVTNU_float},
  {"FCVTPS_float", &IRBuilder::FCVTPS_float}, {"FCVTPU_float", &IRBuilder::FCVTPU_float},
  {"FCVTMS_float", &IRBuilder::FCVTMS_float}, {"FCVTMU_float", &IRBuilder::FCVTMU_float},
  {"FCVTZS_float_int", &IRBuilder::FCVTZS_float_int}, {"FCVTZU_float_int", &IRBuilder::FCVTZU_float_int},
  {"FCVTAS_float", &IRBuilder::FCVTAS_float}, {"FCVTAU_float", &IRBuilder::FCVTAU_float},
  {"SCVTF_float_int", &IRBuilder::SCVTF_float_int}, {"UCVTF_float_int", &IRBuilder::UCVTF_float_int},
  {"SCVTF_float_fix", &IRBuilder::SCVTF_float_fix}, {"UCVTF_float_fix", &IRBuilder::UCVTF_float_fix},
  {"FCVTZS_float_fix", &IRBuilder::FCVTZS_float_fix}, {"FCVTZU_float_fix", &IRBuilder::FCVTZU_float_fix},
  {"FCVTZS_int_2", &IRBuilder::FCVTZS_int_2}, {"FCVTZU_int_2", &IRBuilder::FCVTZU_int_2},
  {"SCVTF_int_2", &IRBuilder::SCVTF_int_2}, {"UCVTF_int_2", &IRBuilder::UCVTF_int_2},
  {"FCVTL", &IRBuilder::FCVTL}, {"FCVTN", &IRBuilder::FCVTN}, {"FCVTXN_1", &IRBuilder::FCVTXN_1}, {"FCVTXN_2", &IRBuilder::FCVTXN_2},
};
// clang-format on

InstHandler IRBuilder::FindHandler(std::string_view Name) {
  // Called once per a64.inc entry while the decode table is built, in every
  // process. A linear scan made that ~400K string compares per process start;
  // an index sorted by name makes it a binary search. The first entry for a
  // name wins, as it did with the scan.
  static const auto Index = [] {
    fextl::vector<const HandlerEntry*> Sorted;
    Sorted.reserve(std::size(HandlerTable));
    for (const auto& Entry : HandlerTable) {
      Sorted.push_back(&Entry);
    }
    std::stable_sort(Sorted.begin(), Sorted.end(), [](const HandlerEntry* A, const HandlerEntry* B) { return A->Name < B->Name; });
    return Sorted;
  }();
  auto It = std::lower_bound(Index.begin(), Index.end(), Name, [](const HandlerEntry* A, std::string_view N) { return A->Name < N; });
  if (It != Index.end() && (*It)->Name == Name) {
    return (*It)->Handler;
  }
  return nullptr;
}

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
