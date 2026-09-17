/* neon_ref.h -- vector-level C reference model of the AArch64 Advanced SIMD
 * operations probed here, plus the corpus driver shared by the Pi golden
 * capture and the POWER parity probe.  Every function has the signature
 *   void f(V* r, const V* a, const V* b, const V* c, int W, int imm)
 * where W is the element width in bits of the *input* operands (the
 * narrowing/widening ops say which side) and imm is an immediate or 0.
 */
#ifndef NEON_REF_H
#define NEON_REF_H
#include "neon_corpus.h"

typedef void (*opfn)(V* r, const V* a, const V* b, const V* c, int W, int imm);

static inline uint64_t getl(const V* v, int i, int W) { switch (W) { case 8: return v->b[i]; case 16: return v->h[i]; case 32: return v->w[i]; default: return v->d[i]; } }
static inline void setl(V* v, int i, int W, uint64_t x) { switch (W) { case 8: v->b[i] = (uint8_t)x; break; case 16: v->h[i] = (uint16_t)x; break; case 32: v->w[i] = (uint32_t)x; break; default: v->d[i] = x; } }
static inline uint64_t ones(int W) { return W == 64 ? ~0ull : (1ull << W) - 1; }
#define LANES(W) (128 / (W))
#define FOR_LANES(W) for (int i = 0; i < LANES(W); i++)
#define A getl(a, i, W)
#define B getl(b, i, W)
#define SA sext(A, W)
#define SB sext(B, W)
#define R(x) setl(r, i, W, (uint64_t)(x))

/* ---- compares ---- */
static void ref_cmhi(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(A > B ? ones(W) : 0); }
static void ref_cmhs(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(A >= B ? ones(W) : 0); }
static void ref_cmgt(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(SA > SB ? ones(W) : 0); }
static void ref_cmge(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(SA >= SB ? ones(W) : 0); }
static void ref_cmtst(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R((A & B) ? ones(W) : 0); }
/* ---- saturating arithmetic ---- */
static void ref_sqadd(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(sat_s((__int128)SA + SB, W)); }
static void ref_uqadd(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(sat_u((__int128)A + B, W)); }
static void ref_sqsub(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(sat_s((__int128)SA - SB, W)); }
static void ref_uqsub(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(sat_u((__int128)A - B, W)); }
static void ref_suqadd(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(sat_s((__int128)SA + (__int128)B, W)); }
static void ref_usqadd(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(sat_u((__int128)A + (__int128)SB, W)); }
static void ref_sqabs(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(sat_s(SA < 0 ? -(__int128)SA : (__int128)SA, W)); }
static void ref_sqneg(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(sat_s(-(__int128)SA, W)); }
static void ref_abs(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(trunc_w(SA < 0 ? -(__int128)SA : (__int128)SA, W)); }
static void ref_neg(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(trunc_w(-(__int128)SA, W)); }
/* ---- multiplies ---- */
static void ref_mul(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(trunc_w((__int128)A * B, W)); }
static void ref_mla(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(trunc_w((__int128)getl(c, i, W) + (__int128)A * B, W)); }
static void ref_mls(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(trunc_w((__int128)getl(c, i, W) - (__int128)A * B, W)); }
static void ref_sqdmulh(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_sqdmulh_l(A, B, W, 0)); }
static void ref_sqrdmulh(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_sqdmulh_l(A, B, W, 1)); }
/* long forms: inputs are the low LANES(2W) elements of a and b at width W; output width 2W */
static void ref_sqdmull(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) setl(r, i, 2 * W, (uint64_t)sat_s((__int128)2 * sext(getl(a, i, W), W) * sext(getl(b, i, W), W), 2 * W)); }
static void ref_smull(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) setl(r, i, 2 * W, trunc_w((__int128)sext(getl(a, i, W), W) * sext(getl(b, i, W), W), 2 * W)); }
static void ref_umull(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) setl(r, i, 2 * W, trunc_w((__int128)getl(a, i, W) * getl(b, i, W), 2 * W)); }
static void ref_smull2(V* r, const V* a, const V* b, const V* c, int W, int imm) { int h = LANES(2 * W); for (int i = 0; i < h; i++) setl(r, i, 2 * W, trunc_w((__int128)sext(getl(a, i + h, W), W) * sext(getl(b, i + h, W), W), 2 * W)); }
static void ref_umull2(V* r, const V* a, const V* b, const V* c, int W, int imm) { int h = LANES(2 * W); for (int i = 0; i < h; i++) setl(r, i, 2 * W, trunc_w((__int128)getl(a, i + h, W) * getl(b, i + h, W), 2 * W)); }
/* ---- absolute difference ---- */
static void ref_sabd(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) { __int128 d = (__int128)SA - SB; R(trunc_w(d < 0 ? -d : d, W)); } }
static void ref_uabd(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) { __int128 d = (__int128)A - B; R(trunc_w(d < 0 ? -d : d, W)); } }
static void ref_uabdl(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) { __int128 d = (__int128)getl(a, i, W) - getl(b, i, W); setl(r, i, 2 * W, (uint64_t)(d < 0 ? -d : d)); } }
static void ref_sabdl(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) { __int128 d = (__int128)sext(getl(a, i, W), W) - sext(getl(b, i, W), W); setl(r, i, 2 * W, trunc_w(d < 0 ? -d : d, 2 * W)); } }
/* ---- shifts by register ---- */
static void ref_sshl(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_shl_reg(A, (uint8_t)B, W, 1, 0)); }
static void ref_ushl(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_shl_reg(A, (uint8_t)B, W, 0, 0)); }
static void ref_srshl(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_shl_reg(A, (uint8_t)B, W, 1, 1)); }
static void ref_urshl(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_shl_reg(A, (uint8_t)B, W, 0, 1)); }
static void ref_sqshl_r(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_qshl_reg(A, (uint8_t)B, W, 1, 0)); }
static void ref_uqshl_r(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_qshl_reg(A, (uint8_t)B, W, 0, 0)); }
static void ref_sqrshl(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_qshl_reg(A, (uint8_t)B, W, 1, 1)); }
static void ref_uqrshl(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_qshl_reg(A, (uint8_t)B, W, 0, 1)); }
/* ---- shifts by immediate (imm = shift, 1..W for right, 0..W-1 for left) ---- */
static void ref_sshr(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_shr_imm(A, imm, W, 1, 0)); }
static void ref_ushr(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_shr_imm(A, imm, W, 0, 0)); }
static void ref_srshr(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_shr_imm(A, imm, W, 1, 1)); }
static void ref_urshr(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_shr_imm(A, imm, W, 0, 1)); }
static void ref_shl(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(trunc_w((__int128)A << imm, W)); }
static void ref_sqshl_i(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(sat_s((__int128)SA << imm, W)); }
static void ref_uqshl_i(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(sat_u((__int128)A << imm, W)); }
static void ref_sqshlu(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_sqshlu_l(A, imm, W)); }
static void ref_ssra(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(trunc_w((__int128)getl(c, i, W) + (__int128)sext(ref_shr_imm(A, imm, W, 1, 0), W), W)); }
static void ref_usra(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(trunc_w((__int128)getl(c, i, W) + ref_shr_imm(A, imm, W, 0, 0), W)); }
/* narrowing shifts: input width 2W in a, output width W in the low half of r (upper half zero). */
#define NARROW(body) { memset(r, 0, 16); for (int i = 0; i < LANES(2 * W); i++) { uint64_t x = getl(a, i, 2 * W); setl(r, i, W, body); } }
static void ref_shrn(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW(ref_shrn_l(x, imm, 2 * W, 0))
static void ref_rshrn(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW(ref_shrn_l(x, imm, 2 * W, 1))
static void ref_sqshrn(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW(ref_qshrn_l(x, imm, 2 * W, 1, 1, 0))
static void ref_sqrshrn(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW(ref_qshrn_l(x, imm, 2 * W, 1, 1, 1))
static void ref_uqshrn(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW(ref_qshrn_l(x, imm, 2 * W, 0, 0, 0))
static void ref_uqrshrn(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW(ref_qshrn_l(x, imm, 2 * W, 0, 0, 1))
static void ref_sqshrun(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW(ref_qshrn_l(x, imm, 2 * W, 1, 0, 0))
static void ref_sqrshrun(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW(ref_qshrn_l(x, imm, 2 * W, 1, 0, 1))
static void ref_xtn(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW(trunc_w(x, W))
static void ref_sqxtn(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW((uint64_t)sat_s(sext(x, 2 * W), W))
static void ref_uqxtn(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW(sat_u(x, W))
static void ref_sqxtun(V* r, const V* a, const V* b, const V* c, int W, int imm) NARROW(sat_u(sext(x, 2 * W), W))
/* addhn/raddhn/subhn: inputs 2W, outputs W */
static void ref_addhn(V* r, const V* a, const V* b, const V* c, int W, int imm) { memset(r, 0, 16); for (int i = 0; i < LANES(2 * W); i++) setl(r, i, W, trunc_w((getl(a, i, 2 * W) + getl(b, i, 2 * W)) >> W, W)); }
static void ref_raddhn(V* r, const V* a, const V* b, const V* c, int W, int imm) { memset(r, 0, 16); for (int i = 0; i < LANES(2 * W); i++) setl(r, i, W, trunc_w(((unsigned __int128)getl(a, i, 2 * W) + getl(b, i, 2 * W) + (1ull << (W - 1))) >> W, W)); }
static void ref_subhn(V* r, const V* a, const V* b, const V* c, int W, int imm) { memset(r, 0, 16); for (int i = 0; i < LANES(2 * W); i++) setl(r, i, W, trunc_w((getl(a, i, 2 * W) - getl(b, i, 2 * W)) >> W, W)); }
/* widening: sxtl/uxtl take the low LANES(2W) elements of width W to 2W */
static void ref_sxtl(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) setl(r, i, 2 * W, (uint64_t)sext(getl(a, i, W), W)); }
static void ref_uxtl(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) setl(r, i, 2 * W, getl(a, i, W)); }
static void ref_sxtl2(V* r, const V* a, const V* b, const V* c, int W, int imm) { int h = LANES(2 * W); for (int i = 0; i < h; i++) setl(r, i, 2 * W, (uint64_t)sext(getl(a, i + h, W), W)); }
static void ref_uxtl2(V* r, const V* a, const V* b, const V* c, int W, int imm) { int h = LANES(2 * W); for (int i = 0; i < h; i++) setl(r, i, 2 * W, getl(a, i + h, W)); }
static void ref_saddl(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) setl(r, i, 2 * W, trunc_w((__int128)sext(getl(a, i, W), W) + sext(getl(b, i, W), W), 2 * W)); }
static void ref_uaddl(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) setl(r, i, 2 * W, trunc_w((__int128)getl(a, i, W) + getl(b, i, W), 2 * W)); }
/* pairwise widening: saddlp/uaddlp W -> 2W over the whole vector; sadalp/uadalp accumulate into c */
static void ref_saddlp(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) setl(r, i, 2 * W, trunc_w((__int128)sext(getl(a, 2 * i, W), W) + sext(getl(a, 2 * i + 1, W), W), 2 * W)); }
static void ref_uaddlp(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) setl(r, i, 2 * W, trunc_w((__int128)getl(a, 2 * i, W) + getl(a, 2 * i + 1, W), 2 * W)); }
static void ref_sadalp(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) setl(r, i, 2 * W, trunc_w((__int128)getl(c, i, 2 * W) + sext(getl(a, 2 * i, W), W) + sext(getl(a, 2 * i + 1, W), W), 2 * W)); }
static void ref_uadalp(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < LANES(2 * W); i++) setl(r, i, 2 * W, trunc_w((__int128)getl(c, i, 2 * W) + getl(a, 2 * i, W) + getl(a, 2 * i + 1, W), 2 * W)); }
/* pairwise: pairs of (b:a) */
#define PAIRWISE(op) { V t; int n = LANES(W); for (int i = 0; i < n; i++) { uint64_t x = i < n / 2 ? getl(a, 2 * i, W) : getl(b, 2 * i - n, W), y = i < n / 2 ? getl(a, 2 * i + 1, W) : getl(b, 2 * i + 1 - n, W); setl(&t, i, W, op); } *r = t; }
static void ref_addp(V* r, const V* a, const V* b, const V* c, int W, int imm) PAIRWISE(trunc_w((__int128)x + y, W))
static void ref_umaxp(V* r, const V* a, const V* b, const V* c, int W, int imm) PAIRWISE(x > y ? x : y)
static void ref_uminp(V* r, const V* a, const V* b, const V* c, int W, int imm) PAIRWISE(x < y ? x : y)
static void ref_smaxp(V* r, const V* a, const V* b, const V* c, int W, int imm) PAIRWISE(sext(x, W) > sext(y, W) ? x : y)
static void ref_sminp(V* r, const V* a, const V* b, const V* c, int W, int imm) PAIRWISE(sext(x, W) < sext(y, W) ? x : y)
/* across lanes: result in lane 0 at width W (or 2W for the long forms), rest zero */
static void ref_addv(V* r, const V* a, const V* b, const V* c, int W, int imm) { uint64_t s = 0; FOR_LANES(W) s += A; memset(r, 0, 16); setl(r, 0, W, trunc_w(s, W)); }
static void ref_saddlv(V* r, const V* a, const V* b, const V* c, int W, int imm) { __int128 s = 0; FOR_LANES(W) s += SA; memset(r, 0, 16); setl(r, 0, 2 * W, trunc_w(s, 2 * W)); }
static void ref_uaddlv(V* r, const V* a, const V* b, const V* c, int W, int imm) { __int128 s = 0; FOR_LANES(W) s += A; memset(r, 0, 16); setl(r, 0, 2 * W, trunc_w(s, 2 * W)); }
static void ref_umaxv(V* r, const V* a, const V* b, const V* c, int W, int imm) { uint64_t m = 0; FOR_LANES(W) if (A > m) m = A; memset(r, 0, 16); setl(r, 0, W, m); }
static void ref_uminv(V* r, const V* a, const V* b, const V* c, int W, int imm) { uint64_t m = ones(W); FOR_LANES(W) if (A < m) m = A; memset(r, 0, 16); setl(r, 0, W, m); }
static void ref_smaxv(V* r, const V* a, const V* b, const V* c, int W, int imm) { int64_t m = -(int64_t)(ones(W) / 2) - 1; FOR_LANES(W) if (SA > m) m = SA; memset(r, 0, 16); setl(r, 0, W, trunc_w(m, W)); }
static void ref_sminv(V* r, const V* a, const V* b, const V* c, int W, int imm) { int64_t m = (int64_t)(ones(W) / 2); FOR_LANES(W) if (SA < m) m = SA; memset(r, 0, 16); setl(r, 0, W, trunc_w(m, W)); }
/* ---- bit manipulation ---- */
static void ref_cnt(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < 16; i++) r->b[i] = (uint8_t)__builtin_popcount(a->b[i]); }
static void ref_clz(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_clz_l(A, W)); }
static void ref_cls(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) R(ref_cls_l(A, W)); }
static void ref_rbit(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < 16; i++) r->b[i] = ref_rbit8(a->b[i]); }
static void ref_rev64(V* r, const V* a, const V* b, const V* c, int W, int imm) { int n = 64 / W; FOR_LANES(W) R(getl(a, (i / n) * n + (n - 1 - i % n), W)); }
static void ref_rev32(V* r, const V* a, const V* b, const V* c, int W, int imm) { int n = 32 / W; FOR_LANES(W) R(getl(a, (i / n) * n + (n - 1 - i % n), W)); }
static void ref_rev16(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < 16; i++) r->b[i] = a->b[i ^ 1]; }
/* ---- permutes ---- */
static void ref_zip1(V* r, const V* a, const V* b, const V* c, int W, int imm) { V t; int n = LANES(W); for (int i = 0; i < n / 2; i++) { setl(&t, 2 * i, W, getl(a, i, W)); setl(&t, 2 * i + 1, W, getl(b, i, W)); } *r = t; }
static void ref_zip2(V* r, const V* a, const V* b, const V* c, int W, int imm) { V t; int n = LANES(W); for (int i = 0; i < n / 2; i++) { setl(&t, 2 * i, W, getl(a, i + n / 2, W)); setl(&t, 2 * i + 1, W, getl(b, i + n / 2, W)); } *r = t; }
static void ref_uzp1(V* r, const V* a, const V* b, const V* c, int W, int imm) { V t; int n = LANES(W); for (int i = 0; i < n / 2; i++) { setl(&t, i, W, getl(a, 2 * i, W)); setl(&t, i + n / 2, W, getl(b, 2 * i, W)); } *r = t; }
static void ref_uzp2(V* r, const V* a, const V* b, const V* c, int W, int imm) { V t; int n = LANES(W); for (int i = 0; i < n / 2; i++) { setl(&t, i, W, getl(a, 2 * i + 1, W)); setl(&t, i + n / 2, W, getl(b, 2 * i + 1, W)); } *r = t; }
static void ref_trn1(V* r, const V* a, const V* b, const V* c, int W, int imm) { V t; int n = LANES(W); for (int i = 0; i < n / 2; i++) { setl(&t, 2 * i, W, getl(a, 2 * i, W)); setl(&t, 2 * i + 1, W, getl(b, 2 * i, W)); } *r = t; }
static void ref_trn2(V* r, const V* a, const V* b, const V* c, int W, int imm) { V t; int n = LANES(W); for (int i = 0; i < n / 2; i++) { setl(&t, 2 * i, W, getl(a, 2 * i + 1, W)); setl(&t, 2 * i + 1, W, getl(b, 2 * i + 1, W)); } *r = t; }
static void ref_ext(V* r, const V* a, const V* b, const V* c, int W, int imm) { uint8_t t[32]; memcpy(t, a->b, 16); memcpy(t + 16, b->b, 16); memcpy(r->b, t + imm, 16); }
/* table lookups: tables in a (and b, c, and a 4th passed through imm-selected global) */
static V TBL_T3, TBL_T4;
static void ref_tbl(V* r, const V* a, const V* b, const V* c, int W, int imm) { /* imm = number of tables; a,b,TBL_T3,TBL_T4 tables; c = indices */
  uint8_t t[64]; memcpy(t, a->b, 16); memcpy(t + 16, b->b, 16); memcpy(t + 32, TBL_T3.b, 16); memcpy(t + 48, TBL_T4.b, 16);
  for (int i = 0; i < 16; i++) { unsigned k = c->b[i]; r->b[i] = k < 16u * imm ? t[k] : 0; } }
static V TBX_D;
static void ref_tbx(V* r, const V* a, const V* b, const V* c, int W, int imm) {
  uint8_t t[64]; memcpy(t, a->b, 16); memcpy(t + 16, b->b, 16); memcpy(t + 32, TBL_T3.b, 16); memcpy(t + 48, TBL_T4.b, 16);
  for (int i = 0; i < 16; i++) { unsigned k = c->b[i]; r->b[i] = k < 16u * imm ? t[k] : TBX_D.b[i]; } }
/* ---- dot products (bytes -> words, accumulate into c) ---- */
static void ref_udot(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < 4; i++) { uint32_t s = c->w[i]; for (int j = 0; j < 4; j++) s += (uint32_t)a->b[4 * i + j] * b->b[4 * i + j]; r->w[i] = s; } }
static void ref_sdot(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < 4; i++) { int32_t s = c->sw[i]; for (int j = 0; j < 4; j++) s += (int32_t)a->sb[4 * i + j] * b->sb[4 * i + j]; r->sw[i] = s; } }
static void ref_usdot(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < 4; i++) { int32_t s = c->sw[i]; for (int j = 0; j < 4; j++) s += (int32_t)a->b[4 * i + j] * b->sb[4 * i + j]; r->sw[i] = s; } }
/* ---- polynomial ---- */
static void ref_pmull(V* r, const V* a, const V* b, const V* c, int W, int imm) { clmul64(a->d[0], b->d[0], &r->d[0], &r->d[1]); }
static void ref_pmull2(V* r, const V* a, const V* b, const V* c, int W, int imm) { clmul64(a->d[1], b->d[1], &r->d[0], &r->d[1]); }
static void ref_pmull8(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < 8; i++) r->h[i] = clmul8(a->b[i], b->b[i]); }
static void ref_pmull8_2(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < 8; i++) r->h[i] = clmul8(a->b[i + 8], b->b[i + 8]); }
/* ---- AES ---- */
static void ref_aese_v(V* r, const V* a, const V* b, const V* c, int W, int imm) { ref_aese(r, a, b); }
static void ref_aesd_v(V* r, const V* a, const V* b, const V* c, int W, int imm) { ref_aesd(r, a, b); }
static void ref_aesmc_v(V* r, const V* a, const V* b, const V* c, int W, int imm) { ref_aesmc(r, a); }
static void ref_aesimc_v(V* r, const V* a, const V* b, const V* c, int W, int imm) { ref_aesimc(r, a); }
/* ---- float (W = 32 or 64) ---- */
static inline uint64_t f_get(const V* v, int i, int W) { return W == 32 ? v->w[i] : v->d[i]; }
static inline void f_set(V* v, int i, int W, uint64_t x) { if (W == 32) v->w[i] = (uint32_t)x; else v->d[i] = x; }
static inline uint64_t f_quiet(uint64_t x, int W) { if (W == 32) return f32_quiet((uint32_t)x); return ((x & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (x & 0xFFFFFFFFFFFFFull)) ? (x | 0x8000000000000ull) : x; }
static inline int f_isnan(uint64_t x, int W) { return W == 32 ? f32_isnan((uint32_t)x) : ((x & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (x & 0xFFFFFFFFFFFFFull)); }
static inline int f_issnan(uint64_t x, int W) { return f_isnan(x, W) && !(x & (W == 32 ? 0x400000ull : 0x8000000000000ull)); }
/* FPRoundInt with a given C rounding function; NaN in -> quieted NaN out. */
#define FRINT(name, expr32, expr64, mode) static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { \
  int old = fegetround(); fesetround(mode); FOR_LANES(W) { uint64_t x = f_get(a, i, W); if (f_isnan(x, W)) { f_set(r, i, W, f_quiet(x, W)); continue; } \
  if (W == 32) { float v = u2f((uint32_t)x); f_set(r, i, W, f2u(expr32)); } else { double v = u2d(x); f_set(r, i, W, d2u(expr64)); } } fesetround(old); }
FRINT(ref_frintn, rintf(v), rint(v), FE_TONEAREST)
FRINT(ref_frinta, roundf(v), round(v), FE_TONEAREST)
FRINT(ref_frintm, floorf(v), floor(v), FE_TONEAREST)
FRINT(ref_frintp, ceilf(v), ceil(v), FE_TONEAREST)
FRINT(ref_frintz, truncf(v), trunc(v), FE_TONEAREST)
/* FCVTZS/FCVTZU: saturating, NaN -> 0 */
static void ref_fcvtzs(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) { uint64_t x = f_get(a, i, W); if (f_isnan(x, W)) { f_set(r, i, W, 0); continue; } double v = W == 32 ? (double)u2f((uint32_t)x) : u2d(x); double mx = W == 32 ? 2147483647.0 : 9223372036854775807.0, mn = W == 32 ? -2147483648.0 : -9223372036854775808.0; v = trunc(v); int64_t q = v >= mx ? (W == 32 ? INT32_MAX : INT64_MAX) : v <= mn ? (W == 32 ? INT32_MIN : INT64_MIN) : (int64_t)v; f_set(r, i, W, (uint64_t)q); } }
static void ref_fcvtzu(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) { uint64_t x = f_get(a, i, W); if (f_isnan(x, W)) { f_set(r, i, W, 0); continue; } double v = W == 32 ? (double)u2f((uint32_t)x) : u2d(x); double mx = W == 32 ? 4294967295.0 : 18446744073709551615.0; v = trunc(v); uint64_t q = v >= mx ? (W == 32 ? UINT32_MAX : UINT64_MAX) : v <= 0 ? 0 : (uint64_t)v; f_set(r, i, W, q); } }
static void ref_scvtf(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) { if (W == 32) r->f[i] = (float)a->sw[i]; else r->df[i] = (double)a->sd[i]; } }
static void ref_ucvtf(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) { if (W == 32) r->f[i] = (float)a->w[i]; else r->df[i] = (double)a->d[i]; } }
static void ref_frecpe(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < 4; i++) r->w[i] = ref_frecpe32(a->w[i]); }
static void ref_frsqrte(V* r, const V* a, const V* b, const V* c, int W, int imm) { for (int i = 0; i < 4; i++) r->w[i] = ref_frsqrte32(a->w[i]); }
/* FPProcessNaNs: sNaN of op1, op2, (op3), then qNaN of op1, op2, (op3); result quieted. Returns 1 and sets *out when a NaN is produced. */
static inline int f_nans(uint64_t x, uint64_t y, uint64_t z, int nz, int W, uint64_t* out) {
  if (f_issnan(x, W)) { *out = f_quiet(x, W); return 1; } if (f_issnan(y, W)) { *out = f_quiet(y, W); return 1; } if (nz && f_issnan(z, W)) { *out = f_quiet(z, W); return 1; }
  if (f_isnan(x, W)) { *out = x; return 1; } if (f_isnan(y, W)) { *out = y; return 1; } if (nz && f_isnan(z, W)) { *out = z; return 1; } return 0; }
static inline uint64_t f_defnan(int W) { return W == 32 ? 0x7FC00000ull : 0x7FF8000000000000ull; }
static inline int f_isinf(uint64_t x, int W) { return W == 32 ? ((x & 0x7FFFFFFF) == 0x7F800000) : ((x & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull); }
static inline int f_iszero(uint64_t x, int W) { return W == 32 ? ((x & 0x7FFFFFFF) == 0) : ((x & 0x7FFFFFFFFFFFFFFFull) == 0); }
/* FMLA/FMLS vector: r = c + a*b (FMLS: c - a*b, implemented as FPMulAdd(c, -a, b)). Rounding mode from the current environment. */
static void fmla_impl(V* r, const V* a, const V* b, const V* c, int W, int neg) {
  FOR_LANES(W) { uint64_t x = f_get(a, i, W), y = f_get(b, i, W), z = f_get(c, i, W), o;
    if (neg) x ^= (W == 32 ? 0x80000000ull : 0x8000000000000000ull);
    /* FPMulAdd: the invalid inf*0 with a quiet NaN addend gives the default NaN */
    int infzero = (f_isinf(x, W) && f_iszero(y, W)) || (f_iszero(x, W) && f_isinf(y, W));
    if (f_isnan(z, W) && !f_issnan(z, W) && infzero && !f_issnan(x, W) && !f_issnan(y, W)) { f_set(r, i, W, f_defnan(W)); continue; }
    if (f_nans(z, x, y, 1, W, &o)) { f_set(r, i, W, o); continue; }
    if (infzero) { f_set(r, i, W, f_defnan(W)); continue; }
    if (W == 32) f_set(r, i, W, f2u(fmaf(u2f((uint32_t)x), u2f((uint32_t)y), u2f((uint32_t)z)))); else f_set(r, i, W, d2u(fma(u2d(x), u2d(y), u2d(z)))); } }
static void ref_fmla(V* r, const V* a, const V* b, const V* c, int W, int imm) { fmla_impl(r, a, b, c, W, 0); }
static void ref_fmls(V* r, const V* a, const V* b, const V* c, int W, int imm) { fmla_impl(r, a, b, c, W, 1); }
/* FMAX/FMIN (NaN propagating) and FMAXNM/FMINNM */
static void fminmax_impl(V* r, const V* a, const V* b, int W, int ismax, int nm) {
  FOR_LANES(W) { uint64_t x = f_get(a, i, W), y = f_get(b, i, W), o;
    if (nm) { int qx = f_isnan(x, W) && !f_issnan(x, W), qy = f_isnan(y, W) && !f_issnan(y, W);
      if (qx && !f_isnan(y, W)) x = ismax ? (W == 32 ? 0xFF800000ull : 0xFFF0000000000000ull) : (W == 32 ? 0x7F800000ull : 0x7FF0000000000000ull);
      else if (qy && !f_isnan(x, W)) y = ismax ? (W == 32 ? 0xFF800000ull : 0xFFF0000000000000ull) : (W == 32 ? 0x7F800000ull : 0x7FF0000000000000ull); }
    if (f_nans(x, y, 0, 0, W, &o)) { f_set(r, i, W, o); continue; }
    if (f_iszero(x, W) && f_iszero(y, W)) { uint64_t sx = x >> (W - 1), sy = y >> (W - 1); f_set(r, i, W, ismax ? (sx && sy ? x : (sx ? y : x)) : (sx || sy ? (sx ? x : y) : x)); continue; }
    if (W == 32) { float p = u2f((uint32_t)x), q = u2f((uint32_t)y); f_set(r, i, W, ismax ? (p > q ? x : y) : (p < q ? x : y)); } else { double p = u2d(x), q = u2d(y); f_set(r, i, W, ismax ? (p > q ? x : y) : (p < q ? x : y)); } } }
static void ref_fmax(V* r, const V* a, const V* b, const V* c, int W, int imm) { fminmax_impl(r, a, b, W, 1, 0); }
static void ref_fmin(V* r, const V* a, const V* b, const V* c, int W, int imm) { fminmax_impl(r, a, b, W, 0, 0); }
static void ref_fmaxnm(V* r, const V* a, const V* b, const V* c, int W, int imm) { fminmax_impl(r, a, b, W, 1, 1); }
static void ref_fminnm(V* r, const V* a, const V* b, const V* c, int W, int imm) { fminmax_impl(r, a, b, W, 0, 1); }
/* FRECPS: 2 - a*b, FRSQRTS: (3 - a*b)/2, with the inf*0 special cases */
static void ref_frecps(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) { uint64_t x = f_get(a, i, W) ^ (W == 32 ? 0x80000000ull : 0x8000000000000000ull), y = f_get(b, i, W), o; /* FPRecipStepFused negates op1 before FPProcessNaNs */ if (f_nans(x, y, 0, 0, W, &o)) { f_set(r, i, W, o); continue; }
  if ((f_isinf(x, W) && f_iszero(y, W)) || (f_iszero(x, W) && f_isinf(y, W))) { f_set(r, i, W, W == 32 ? 0x40000000ull : 0x4000000000000000ull); continue; }
  if (W == 32) f_set(r, i, W, f2u(fmaf(u2f((uint32_t)x), u2f((uint32_t)y), 2.0f))); else f_set(r, i, W, d2u(fma(u2d(x), u2d(y), 2.0))); } }
/* FRSQRTS = (3 - a*b)/2 with ONE rounding [MEASURED on the Pi: the halving is
 * inside the rounding].  Computed as fma(-a/2, b, 1.5): halving a is exact
 * unless a is denormal, in which case b is halved instead (both denormal:
 * the product underflows and the answer is 1.5 either way). */
static void ref_frsqrts(V* r, const V* a, const V* b, const V* c, int W, int imm) { FOR_LANES(W) { uint64_t x = f_get(a, i, W) ^ (W == 32 ? 0x80000000ull : 0x8000000000000000ull), y = f_get(b, i, W), o; if (f_nans(x, y, 0, 0, W, &o)) { f_set(r, i, W, o); continue; }
  if ((f_isinf(x, W) && f_iszero(y, W)) || (f_iszero(x, W) && f_isinf(y, W))) { f_set(r, i, W, W == 32 ? 0x3FC00000ull : 0x3FF8000000000000ull); continue; }
  if (W == 32) { float p = u2f((uint32_t)x), q = u2f((uint32_t)y); if (fabsf(p) >= 2.35098870e-38f || p == 0) p *= 0.5f; else q *= 0.5f; f_set(r, i, W, f2u(fmaf(p, q, 1.5f))); }
  else { double p = u2d(x), q = u2d(y); if (fabs(p) >= 4.4501477170144028e-308 || p == 0) p *= 0.5; else q *= 0.5; f_set(r, i, W, d2u(fma(p, q, 1.5))); } } }

static int immok(int W, int imm) { if (W == 8) return imm >= 1 && imm <= 8; if (W == 16) return imm >= 1 && imm <= 16; if (W == 32) return imm == 1 || imm == 2 || imm == 3 || imm == 5 || imm == 8 || imm == 15 || imm == 16 || imm == 17 || imm == 24 || imm == 31 || imm == 32; return imm == 1 || imm == 2 || imm == 7 || imm == 16 || imm == 31 || imm == 32 || imm == 33 || imm == 48 || imm == 63 || imm == 64; }
static int immok_l(int W, int imm) { return imm == 0 || (imm < W && immok(W, imm)); }
static int immok_lu(int W, int imm) { if (W == 8) return imm >= 0 && imm <= 7; if (W == 16) return imm == 0 || imm == 1 || imm == 7 || imm == 8 || imm == 15; if (W == 32) return imm == 0 || imm == 1 || imm == 16 || imm == 31; return imm == 0 || imm == 1 || imm == 32 || imm == 63; }
static int immok_n(int W, int imm) { if (W == 8) return imm >= 1 && imm <= 8; if (W == 16) return imm == 1 || imm == 2 || imm == 3 || imm == 5 || imm == 8 || imm == 15 || imm == 16; return imm == 1 || imm == 2 || imm == 7 || imm == 16 || imm == 31 || imm == 32; }

/* ---- corpus driver --------------------------------------------------- */
enum corpus_kind { C8X /* exhaustive 8-bit pairs */, CR16, CR32, CR64, CSH /* b = shift counts */, CIDX /* c = table indices */, CF32, CF64, CACC /* c = accumulator random */ };
#define NRAND 20000
typedef struct { const char* name; opfn ref; opfn impl; int W; int imm_lo, imm_hi; enum corpus_kind kind; } Case;

/* Runs ref and impl over the corpus; returns number of mismatching vectors; fills hashes. */
static int run_case(const Case* cs, uint64_t* href, uint64_t* himpl, char* first_mismatch, size_t fmlen) {
  int W = cs->W, bad = 0; uint64_t hr = FNV0, hi = FNV0; first_mismatch[0] = 0;
  rng_seed(0x1234567ull ^ (uint64_t)W ^ ((uint64_t)cs->kind << 8) ^ ((uint64_t)cs->imm_lo << 16));
  for (int imm = cs->imm_lo; imm <= cs->imm_hi; imm++) {
    int n = cs->kind == C8X ? N8PAIRS : NRAND;
    for (int k = 0; k < n; k++) {
      V a, b, c, r1, r2; memset(&r1, 0, 16); memset(&r2, 0, 16);
      switch (cs->kind) {
      case C8X: corpus8(k, &a, &b); randv(&c); break;
      case CR16: randv16(&a); randv16(&b); randv16(&c); break;
      case CR32: randv32(&a); randv32(&b); randv32(&c); break;
      case CR64: randv64(&a); randv64(&b); randv64(&c); break;
      case CSH: if (W == 8) { randv(&a); } else if (W == 16) randv16(&a); else if (W == 32) randv32(&a); else randv64(&a); randcount(&b, W, W / 8); randv(&c); break;
      case CIDX: randv(&a); randv(&b); randv(&TBL_T3); randv(&TBL_T4); randv(&TBX_D); for (int i = 0; i < 16; i++) c.b[i] = (rng() & 1) ? (uint8_t)(rng() % 80) : (uint8_t)rng(); break;
      case CF32: randf32(&a); randf32(&b); randf32(&c); break;
      case CF64: randf64(&a); randf64(&b); randf64(&c); break;
      case CACC: randv(&a); randv(&b); randv(&c); break;
      }
      cs->ref(&r1, &a, &b, &c, W, imm);
      if (cs->impl) cs->impl(&r2, &a, &b, &c, W, imm);
      hr = fnv(hr, r1.b, 16); hi = fnv(hi, r2.b, 16);
      if (cs->impl && memcmp(r1.b, r2.b, 16)) { if (!bad) snprintf(first_mismatch, fmlen, "imm=%d k=%d a=%016llx%016llx b=%016llx%016llx c=%016llx%016llx ref=%016llx%016llx got=%016llx%016llx", imm, k, (unsigned long long)a.d[1], (unsigned long long)a.d[0], (unsigned long long)b.d[1], (unsigned long long)b.d[0], (unsigned long long)c.d[1], (unsigned long long)c.d[0], (unsigned long long)r1.d[1], (unsigned long long)r1.d[0], (unsigned long long)r2.d[1], (unsigned long long)r2.d[0]); bad++; }
    }
  }
  *href = hr; *himpl = hi; return bad;
}
#endif
