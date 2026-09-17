// SPDX-License-Identifier: MIT
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Context/Context.h"
#include "Interface/IR/RegisterAllocationData.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>

#include <algorithm>
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
  // Loads and stores.
  {"LDR_lit_gen", &IRBuilder::LDR_lit_gen}, {"LDRSW_lit", &IRBuilder::LDRSW_lit}, {"PRFM_lit", &IRBuilder::PRFM_lit},
  {"STP_LDP_gen", &IRBuilder::STP_LDP_gen},
  {"STURx_LDURx", &IRBuilder::LoadStoreImm9}, {"STRx_LDRx_imm_1", &IRBuilder::LoadStoreImm9},
  {"STRx_LDRx_imm_2", &IRBuilder::STRx_LDRx_imm_2},
  {"PRFM_imm", &IRBuilder::PRFM_imm}, {"PRFM_unscaled_imm", &IRBuilder::PRFM_imm},
  // Unprivileged loads and stores behave as the unscaled forms at EL0.
  {"STTRB", &IRBuilder::LoadStoreImm9}, {"LDTRB", &IRBuilder::LoadStoreImm9}, {"LDTRSB", &IRBuilder::LoadStoreImm9},
  {"STTRH", &IRBuilder::LoadStoreImm9}, {"LDTRH", &IRBuilder::LoadStoreImm9}, {"LDTRSH", &IRBuilder::LoadStoreImm9},
  {"STTR", &IRBuilder::LoadStoreImm9}, {"LDTR", &IRBuilder::LoadStoreImm9}, {"LDTRSW", &IRBuilder::LoadStoreImm9},
  {"STRx_reg", &IRBuilder::LoadStoreRegOffset}, {"LDRx_reg", &IRBuilder::LoadStoreRegOffset},
  // Exclusive loads and stores (software monitor).
  {"LDXR", &IRBuilder::LoadExclusive}, {"LDAXR", &IRBuilder::LoadExclusive},
  {"STXR", &IRBuilder::StoreExclusive}, {"STLXR", &IRBuilder::StoreExclusive},
  {"LDAR", &IRBuilder::LoadStoreAtomicWidth}, {"LDLAR", &IRBuilder::LoadStoreAtomicWidth},
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
  {"LDR_lit_fpsimd", &IRBuilder::LDR_lit_fpsimd}, {"STP_LDP_fpsimd", &IRBuilder::STP_LDP_fpsimd},
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
  // POWERARM-M1-TODO(simd): no translator yet for saturating arithmetic and narrowing (SQADD, UQSUB, SQXTN, SQSHRN, ...), MUL/PMUL/PMULL, the multiply-accumulate and doubling families, TBL/TBX, register and rounding shifts (SSHL, URSHR, RSHRN, ...), SLI/SRI, CLS/CLZ/RBIT vector, ABD/ABA, pairwise-long adds, vector FP arithmetic, compares and conversions (FADD vector, FCMEQ, FCVTZS vector, ...), FRECPE/FRSQRTE, FMOV of a half-precision vector immediate, and the AES/SHA/SHA512/SHA3 entries; none is in the measured subset.
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
  // Advanced SIMD: two-register misc.
  {"CMEQ_zero_2", &IRBuilder::CMEQ_zero_2}, {"CMGT_zero_2", &IRBuilder::CMGT_zero_2}, {"CMGE_zero_2", &IRBuilder::CMGE_zero_2},
  {"CMLE_2", &IRBuilder::CMLE_2}, {"CMLT_2", &IRBuilder::CMLT_2},
  {"CMEQ_zero_1", &IRBuilder::CMEQ_zero_1}, {"CMGT_zero_1", &IRBuilder::CMGT_zero_1}, {"CMGE_zero_1", &IRBuilder::CMGE_zero_1},
  {"CMLE_1", &IRBuilder::CMLE_1}, {"CMLT_1", &IRBuilder::CMLT_1},
  {"CNT", &IRBuilder::CNT}, {"NOT", &IRBuilder::NOT}, {"NEG_2", &IRBuilder::NEG_2}, {"ABS_2", &IRBuilder::ABS_2},
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
  {"FCVTL", &IRBuilder::FCVTL}, {"FCVTN", &IRBuilder::FCVTN},
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
  // A successor that is a block of this compile unit is the CondJump's target
  // itself. Routing it through a new block holding only a Jump cost the host
  // code a `b` hop per edge (up to three taken branches per guest B.cond).
  // Only successors outside the unit get a block, which holds their exit.
  Ref LastBlock = GetCurrentBlock();
  if (auto It = JumpTargets.find(Target); It != JumpTargets.end()) {
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
// frontend's back: at a code block change (a new block starts with no SSA
// values, and the syscall and exception paths all end the block), at a new
// compile, and for a stored value that is not 64 bits wide, whose upper half
// is not the stored one. The window is short so that the reused value does not
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
  auto& Cached = GPRCache[Index];
  if (Cached.Value && Cached.Block == GetCurrentBlock() && CurrentPC >= Cached.PC && CurrentPC - Cached.PC <= GPR_CACHE_WINDOW) {
    Cached.PC = CurrentPC;
    return Cached.Value;
  }
  Ref Value = _LoadContext(OpSize::i64Bit, RegClass::GPR, FEXCore::Core::CPUState::GPROffset(Index));
  Cached = {.Value = Value, .Block = GetCurrentBlock(), .PC = CurrentPC};
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
    if (GetOpSize(Value) == OpSize::i64Bit) {
      GPRCache[Index] = {.Value = Value, .Block = GetCurrentBlock(), .PC = CurrentPC};
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
