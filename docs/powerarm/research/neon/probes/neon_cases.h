/* neon_cases.h -- the case table shared by the Pi golden capture and the
 * POWER parity probe.  IMPL(name) is defined by the includer (nat_name on the
 * Pi, low_name on POWER; NULL where a lowering is not probed). */
#ifndef NEON_CASES_H
#define NEON_CASES_H
/* Immediates exercised for the shift-by-immediate families (the native
 * switch on the Pi covers exactly these; others hash as zero on both sides). */
#define GW(name, ok) static void ref_##name##_g(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (!ok(W, imm)) { memset(r, 0, 16); return; } ref_##name(r, a, b, c, W, imm); }
GW(sshr, immok) GW(ushr, immok) GW(srshr, immok) GW(urshr, immok)
GW(shl, immok_l) GW(sqshl_i, immok_l) GW(uqshl_i, immok_l) GW(sqshlu, immok_lu)
GW(shrn, immok_n) GW(rshrn, immok_n) GW(sqshrn, immok_n) GW(sqrshrn, immok_n) GW(uqshrn, immok_n) GW(uqrshrn, immok_n) GW(sqshrun, immok_n) GW(sqrshrun, immok_n)

#define C(name, W, kind) { #name, ref_##name, IMPL(name), W, 0, 0, kind }
#define CI(name, W, lo, hi, kind) { #name, ref_##name##_g, IMPL(name), W, lo, hi, kind }
#define CT(name, W, lo, hi, kind) { #name, ref_##name, IMPL(name), W, lo, hi, kind }
static const Case CASES[] = {
  C(cmhi, 8, C8X), C(cmhi, 16, CR16), C(cmhi, 32, CR32), C(cmhi, 64, CR64),
  C(cmhs, 8, C8X), C(cmhs, 16, CR16), C(cmhs, 32, CR32), C(cmhs, 64, CR64),
  C(cmgt, 8, C8X), C(cmgt, 64, CR64), C(cmge, 8, C8X), C(cmge, 64, CR64),
  C(cmtst, 8, C8X), C(cmtst, 16, CR16), C(cmtst, 32, CR32), C(cmtst, 64, CR64),
  C(sqadd, 8, C8X), C(sqadd, 16, CR16), C(sqadd, 32, CR32), C(sqadd, 64, CR64),
  C(uqadd, 8, C8X), C(uqadd, 16, CR16), C(uqadd, 32, CR32), C(uqadd, 64, CR64),
  C(sqsub, 8, C8X), C(sqsub, 16, CR16), C(sqsub, 32, CR32), C(sqsub, 64, CR64),
  C(uqsub, 8, C8X), C(uqsub, 16, CR16), C(uqsub, 32, CR32), C(uqsub, 64, CR64),
  C(suqadd, 8, C8X), C(suqadd, 32, CR32), C(usqadd, 8, C8X), C(usqadd, 32, CR32),
  C(sqabs, 8, C8X), C(sqabs, 32, CR32), C(sqabs, 64, CR64), C(sqneg, 8, C8X), C(sqneg, 32, CR32), C(sqneg, 64, CR64),
  C(abs, 8, C8X), C(abs, 64, CR64), C(neg, 8, C8X), C(neg, 64, CR64),
  C(mul, 8, C8X), C(mul, 16, CR16), C(mul, 32, CR32),
  C(mla, 8, C8X), C(mla, 16, CR16), C(mla, 32, CR32), C(mls, 16, CR16), C(mls, 32, CR32),
  C(sqdmulh, 16, CR16), C(sqdmulh, 32, CR32), C(sqrdmulh, 16, CR16), C(sqrdmulh, 32, CR32),
  C(sqdmull, 16, CR16), C(sqdmull, 32, CR32),
  C(smull, 8, C8X), C(smull, 16, CR16), C(smull, 32, CR32), C(umull, 8, C8X), C(umull, 16, CR16), C(umull, 32, CR32),
  C(smull2, 8, C8X), C(smull2, 16, CR16), C(smull2, 32, CR32), C(umull2, 8, C8X), C(umull2, 32, CR32),
  C(sabd, 8, C8X), C(sabd, 16, CR16), C(sabd, 32, CR32), C(uabd, 8, C8X), C(uabd, 16, CR16), C(uabd, 32, CR32),
  C(uabdl, 8, C8X), C(uabdl, 16, CR16), C(uabdl, 32, CR32), C(sabdl, 8, C8X), C(sabdl, 32, CR32),
  C(sshl, 8, CSH), C(sshl, 16, CSH), C(sshl, 32, CSH), C(sshl, 64, CSH),
  C(ushl, 8, CSH), C(ushl, 16, CSH), C(ushl, 32, CSH), C(ushl, 64, CSH),
  C(srshl, 8, CSH), C(srshl, 16, CSH), C(srshl, 32, CSH), C(srshl, 64, CSH),
  C(urshl, 8, CSH), C(urshl, 16, CSH), C(urshl, 32, CSH), C(urshl, 64, CSH),
  C(sqshl_r, 8, CSH), C(sqshl_r, 16, CSH), C(sqshl_r, 32, CSH), C(sqshl_r, 64, CSH),
  C(uqshl_r, 8, CSH), C(uqshl_r, 16, CSH), C(uqshl_r, 32, CSH), C(uqshl_r, 64, CSH),
  C(sqrshl, 8, CSH), C(sqrshl, 32, CSH), C(uqrshl, 8, CSH), C(uqrshl, 32, CSH),
  CI(sshr, 8, 1, 8, C8X), CI(sshr, 16, 1, 16, CR16), CI(sshr, 32, 1, 32, CR32), CI(sshr, 64, 1, 64, CR64),
  CI(ushr, 8, 1, 8, C8X), CI(ushr, 16, 1, 16, CR16), CI(ushr, 32, 1, 32, CR32), CI(ushr, 64, 1, 64, CR64),
  CI(srshr, 8, 1, 8, C8X), CI(srshr, 16, 1, 16, CR16), CI(srshr, 32, 1, 32, CR32), CI(srshr, 64, 1, 64, CR64),
  CI(urshr, 8, 1, 8, C8X), CI(urshr, 16, 1, 16, CR16), CI(urshr, 32, 1, 32, CR32), CI(urshr, 64, 1, 64, CR64),
  CI(shl, 8, 0, 7, C8X), CI(shl, 16, 0, 15, CR16), CI(shl, 32, 0, 31, CR32), CI(shl, 64, 0, 63, CR64),
  CI(sqshl_i, 8, 0, 7, C8X), CI(sqshl_i, 16, 0, 15, CR16), CI(sqshl_i, 32, 0, 31, CR32), CI(sqshl_i, 64, 0, 63, CR64),
  CI(uqshl_i, 8, 0, 7, C8X), CI(uqshl_i, 16, 0, 15, CR16), CI(uqshl_i, 32, 0, 31, CR32), CI(uqshl_i, 64, 0, 63, CR64),
  CI(sqshlu, 8, 0, 7, C8X), CI(sqshlu, 16, 0, 15, CR16), CI(sqshlu, 32, 0, 31, CR32), CI(sqshlu, 64, 0, 63, CR64),
  CI(shrn, 8, 1, 8, CR16), CI(shrn, 16, 1, 16, CR32), CI(shrn, 32, 1, 32, CR64),
  CI(rshrn, 8, 1, 8, CR16), CI(rshrn, 16, 1, 16, CR32), CI(rshrn, 32, 1, 32, CR64),
  CI(sqshrn, 8, 1, 8, CR16), CI(sqshrn, 16, 1, 16, CR32), CI(sqshrn, 32, 1, 32, CR64),
  CI(sqrshrn, 8, 1, 8, CR16), CI(sqrshrn, 16, 1, 16, CR32), CI(sqrshrn, 32, 1, 32, CR64),
  CI(uqshrn, 8, 1, 8, CR16), CI(uqshrn, 16, 1, 16, CR32), CI(uqshrn, 32, 1, 32, CR64),
  CI(uqrshrn, 8, 1, 8, CR16), CI(uqrshrn, 16, 1, 16, CR32), CI(uqrshrn, 32, 1, 32, CR64),
  CI(sqshrun, 8, 1, 8, CR16), CI(sqshrun, 16, 1, 16, CR32), CI(sqshrun, 32, 1, 32, CR64),
  CI(sqrshrun, 8, 1, 8, CR16), CI(sqrshrun, 16, 1, 16, CR32), CI(sqrshrun, 32, 1, 32, CR64),
  C(xtn, 8, CR16), C(xtn, 16, CR32), C(xtn, 32, CR64), C(sqxtn, 8, CR16), C(sqxtn, 16, CR32), C(sqxtn, 32, CR64),
  C(uqxtn, 8, CR16), C(uqxtn, 16, CR32), C(uqxtn, 32, CR64), C(sqxtun, 8, CR16), C(sqxtun, 16, CR32), C(sqxtun, 32, CR64),
  C(addhn, 8, CR16), C(addhn, 16, CR32), C(addhn, 32, CR64), C(raddhn, 8, CR16), C(raddhn, 16, CR32), C(raddhn, 32, CR64), C(subhn, 8, CR16), C(subhn, 32, CR64),
  C(sxtl, 8, C8X), C(sxtl, 16, CR16), C(sxtl, 32, CR32), C(uxtl, 8, C8X), C(uxtl, 16, CR16), C(uxtl, 32, CR32),
  C(sxtl2, 8, C8X), C(sxtl2, 16, CR16), C(sxtl2, 32, CR32), C(uxtl2, 8, C8X), C(uxtl2, 16, CR16), C(uxtl2, 32, CR32),
  C(saddl, 8, C8X), C(saddl, 16, CR16), C(saddl, 32, CR32), C(uaddl, 8, C8X), C(uaddl, 32, CR32),
  C(saddlp, 8, C8X), C(saddlp, 16, CR16), C(saddlp, 32, CR32), C(uaddlp, 8, C8X), C(uaddlp, 16, CR16), C(uaddlp, 32, CR32),
  C(sadalp, 8, C8X), C(sadalp, 16, CR16), C(uadalp, 8, C8X), C(uadalp, 16, CR16),
  C(addp, 8, C8X), C(addp, 16, CR16), C(addp, 32, CR32), C(addp, 64, CR64),
  C(umaxp, 8, C8X), C(umaxp, 16, CR16), C(umaxp, 32, CR32), C(uminp, 8, C8X), C(uminp, 16, CR16), C(uminp, 32, CR32),
  C(smaxp, 8, C8X), C(smaxp, 32, CR32), C(sminp, 8, C8X), C(sminp, 32, CR32),
  C(addv, 8, C8X), C(addv, 16, CR16), C(addv, 32, CR32),
  C(saddlv, 8, C8X), C(saddlv, 16, CR16), C(saddlv, 32, CR32), C(uaddlv, 8, C8X), C(uaddlv, 16, CR16), C(uaddlv, 32, CR32),
  C(umaxv, 8, C8X), C(umaxv, 16, CR16), C(umaxv, 32, CR32), C(uminv, 8, C8X), C(uminv, 16, CR16), C(uminv, 32, CR32),
  C(smaxv, 8, C8X), C(smaxv, 16, CR16), C(smaxv, 32, CR32), C(sminv, 8, C8X), C(sminv, 16, CR16), C(sminv, 32, CR32),
  C(cnt, 8, C8X), C(clz, 8, C8X), C(clz, 16, CR16), C(clz, 32, CR32), C(cls, 8, C8X), C(cls, 16, CR16), C(cls, 32, CR32),
  C(rbit, 8, C8X), C(rev64, 8, C8X), C(rev64, 16, CR16), C(rev64, 32, CR32), C(rev32, 8, C8X), C(rev32, 16, CR16), C(rev16, 8, C8X),
  C(zip1, 8, C8X), C(zip1, 16, CR16), C(zip1, 32, CR32), C(zip1, 64, CR64), C(zip2, 8, C8X), C(zip2, 16, CR16), C(zip2, 32, CR32), C(zip2, 64, CR64),
  C(uzp1, 8, C8X), C(uzp1, 16, CR16), C(uzp1, 32, CR32), C(uzp1, 64, CR64), C(uzp2, 8, C8X), C(uzp2, 16, CR16), C(uzp2, 32, CR32), C(uzp2, 64, CR64),
  C(trn1, 8, C8X), C(trn1, 16, CR16), C(trn1, 32, CR32), C(trn1, 64, CR64), C(trn2, 8, C8X), C(trn2, 16, CR16), C(trn2, 32, CR32), C(trn2, 64, CR64),
  CT(ext, 8, 0, 15, C8X),
  CT(tbl, 8, 1, 4, CIDX), CT(tbx, 8, 1, 4, CIDX),
  C(udot, 8, CACC), C(sdot, 8, CACC),
  C(pmull, 64, CR64), C(pmull2, 64, CR64), C(pmull8, 8, C8X), C(pmull8_2, 8, C8X),
  C(aese_v, 8, CACC), C(aesd_v, 8, CACC), C(aesmc_v, 8, CACC), C(aesimc_v, 8, CACC),
  C(frintn, 32, CF32), C(frintn, 64, CF64), C(frinta, 32, CF32), C(frinta, 64, CF64), C(frintm, 32, CF32), C(frintm, 64, CF64),
  C(frintp, 32, CF32), C(frintp, 64, CF64), C(frintz, 32, CF32), C(frintz, 64, CF64),
  C(fcvtzs, 32, CF32), C(fcvtzs, 64, CF64), C(fcvtzu, 32, CF32), C(fcvtzu, 64, CF64), C(scvtf, 32, CR32), C(scvtf, 64, CR64), C(ucvtf, 32, CR32), C(ucvtf, 64, CR64),
  C(frecpe, 32, CF32), C(frsqrte, 32, CF32),
  C(fmax, 32, CF32), C(fmin, 32, CF32), C(fmaxnm, 32, CF32), C(fminnm, 32, CF32), C(fmax, 64, CF64), C(fminnm, 64, CF64),
  C(frecps, 32, CF32), C(frsqrts, 32, CF32),
};
/* Cases re-run under each FPCR rounding mode. */
#ifndef RM_IMPL
#define RM_IMPL(n) IMPL(n)
#endif
static const Case RMCASES[] = { { "fmla", ref_fmla, RM_IMPL(fmla), 32, 0, 0, CF32 }, { "fmls", ref_fmls, RM_IMPL(fmls), 32, 0, 0, CF32 }, { "fmla", ref_fmla, RM_IMPL(fmla), 64, 0, 0, CF64 }, { "fmls", ref_fmls, RM_IMPL(fmls), 64, 0, 0, CF64 }, C(scvtf, 32, CR32), C(ucvtf, 64, CR64) };
#endif
