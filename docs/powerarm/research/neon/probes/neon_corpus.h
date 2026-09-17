/* neon_corpus.h -- deterministic operand corpora and a C reference model of
 * AArch64 Advanced SIMD semantics, shared by the Raspberry Pi golden capture
 * (neon_golden_pi.c) and the POWER parity probe (neon_parity_p9.c).
 *
 * Everything here is plain C.  The reference functions follow the Arm ARM
 * pseudocode (unbounded-integer arithmetic then truncation, FPRoundInt,
 * FPRecipEstimate tables, AES steps) and are validated on the Pi against the
 * real instructions before they are used as the oracle on POWER.
 *
 * Lane order everywhere: element i of an N-lane vector is bytes
 * [i*W, i*W+W) of the 16-byte memory image, little-endian.
 */
#ifndef NEON_CORPUS_H
#define NEON_CORPUS_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <fenv.h>

typedef union { uint8_t b[16]; uint16_t h[8]; uint32_t w[4]; uint64_t d[2];
                int8_t sb[16]; int16_t sh[8]; int32_t sw[4]; int64_t sd[2];
                float f[4]; double df[2]; } V;

/* ---- PRNG and hashing ------------------------------------------------- */
static uint64_t rng_s = 0x9E3779B97F4A7C15ull;
static inline uint64_t rng(void) { uint64_t x = rng_s; x ^= x >> 12; x ^= x << 25; x ^= x >> 27; rng_s = x; return x * 0x2545F4914F6CDD1Dull; }
static inline void rng_seed(uint64_t s) { rng_s = s ? s : 1; }
static inline uint64_t fnv(uint64_t h, const void* p, size_t n) { const uint8_t* q = (const uint8_t*)p; for (size_t i = 0; i < n; i++) { h ^= q[i]; h *= 0x100000001B3ull; } return h; }
#define FNV0 0xCBF29CE484222325ull

/* ---- corpora ---------------------------------------------------------- */
/* 8-bit binary ops: exhaustive over the 65536 (a,b) pairs, as 4096 vector pairs. */
#define N8PAIRS 4096
static inline void corpus8(unsigned k, V* a, V* b) { for (int i = 0; i < 16; i++) { unsigned p = k * 16 + i; a->b[i] = p >> 8; b->b[i] = p & 255; } }

static const uint16_t E16[] = { 0, 1, 2, 3, 0x7F, 0x80, 0x81, 0xFF, 0x100, 0x7FFE, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF, 0x4000, 0xC000, 0x00FF, 0xFF00, 0x0F0F, 0xF0F0, 0x5555, 0xAAAA, 0x1234, 0xEDCB, 0x0010, 0xFFF0 };
#define NE16 (sizeof E16 / sizeof E16[0])
static const uint32_t E32[] = { 0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFE, 0x7FFFFFFF, 0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x40000000, 0xC0000000u, 0x00FF00FF, 0xFF00FF00u, 0x12345678, 0xDEADBEEFu, 0x0000FFFF, 0xFFFF0000u, 0x00010000, 0x3F800000, 0xBF800000u };
#define NE32 (sizeof E32 / sizeof E32[0])
static const uint64_t E64[] = { 0, 1, 2, 0x7F, 0xFF, 0x7FFF, 0xFFFF, 0x7FFFFFFF, 0x80000000ull, 0xFFFFFFFFull, 0x100000000ull, 0x7FFFFFFFFFFFFFFEull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x8000000000000001ull, 0xFFFFFFFFFFFFFFFEull, 0xFFFFFFFFFFFFFFFFull, 0x4000000000000000ull, 0xC000000000000000ull, 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull, 0x00FF00FF00FF00FFull, 0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull, 0x0000000100000000ull, 0x3FF0000000000000ull };
#define NE64 (sizeof E64 / sizeof E64[0])

/* Random-filled vectors with a bias towards edge values in some lanes. */
static inline void randv(V* v) { v->d[0] = rng(); v->d[1] = rng(); }
static inline void randv16(V* v) { for (int i = 0; i < 8; i++) v->h[i] = (rng() & 3) == 0 ? E16[rng() % NE16] : (uint16_t)rng(); }
static inline void randv32(V* v) { for (int i = 0; i < 4; i++) v->w[i] = (rng() & 3) == 0 ? E32[rng() % NE32] : (uint32_t)rng(); }
static inline void randv64(V* v) { for (int i = 0; i < 2; i++) v->d[i] = (rng() & 3) == 0 ? E64[rng() % NE64] : rng(); }
/* Shift-count vectors: every lane's low byte covers [-2W, 2W] and random
 * values; the bytes above bit 7 of a lane are random (they are ignored). */
static inline void randcount(V* v, int W, int esz) { for (int i = 0; i < 16; i += esz) { int c = (rng() & 1) ? (int)(rng() % (4 * W + 1)) - 2 * W : (int8_t)rng(); uint64_t hi = rng(); v->b[i] = (uint8_t)c; for (int j = 1; j < esz; j++) v->b[i + j] = (uint8_t)(hi >> (8 * j)); } }

static const uint32_t F32E[] = { 0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7FC00123, 0x7F800001, 0x7FA00ABC, 0xFF800042, 0x00000001, 0x007FFFFF, 0x00800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x3F800000, 0xBF800000, 0x3F000000, 0xBF000000, 0x3FC00000, 0xBFC00000, 0x40200000, 0xC0200000, 0x40400000, 0x3F400000, 0x4EFFFFFF, 0x4F000000, 0xCF000000, 0xCF000001, 0x4F7FFFFF, 0x4F800000, 0x5EFFFFFF, 0x5F000000, 0xDF000000, 0x5F800000, 0x3EAAAAAB, 0x40490FDB, 0x4B000000, 0x4B7FFFFF, 0xCB000000, 0x4A800000, 0x33800000, 0x3F7FFFFF, 0x3F800001, 0x40000000, 0x41200000, 0x501502F9 };
#define NF32E (sizeof F32E / sizeof F32E[0])
static inline void randf32(V* v) { for (int i = 0; i < 4; i++) v->w[i] = (rng() & 1) ? F32E[rng() % NF32E] : (uint32_t)rng(); }
static const uint64_t F64E[] = { 0, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF8000000000123ull, 0x7FF0000000000001ull, 0xFFF0000000000042ull, 1, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x3FE0000000000000ull, 0xBFE0000000000000ull, 0x3FF8000000000000ull, 0x4004000000000000ull, 0xC004000000000000ull, 0x41DFFFFFFFC00000ull, 0x41E0000000000000ull, 0xC1E0000000000000ull, 0x43DFFFFFFFFFFFFFull, 0x43E0000000000000ull, 0xC3E0000000000000ull, 0x43F0000000000000ull, 0x4330000000000000ull, 0x432FFFFFFFFFFFFFull, 0x4000000000000000ull, 0x3FD5555555555555ull };
#define NF64E (sizeof F64E / sizeof F64E[0])
static inline void randf64(V* v) { for (int i = 0; i < 2; i++) v->d[i] = (rng() & 1) ? F64E[rng() % NF64E] : rng(); }

/* ---- integer reference semantics --------------------------------------- */
static inline int64_t sat_s(__int128 x, int W) { __int128 mx = ((__int128)1 << (W - 1)) - 1, mn = -((__int128)1 << (W - 1)); return (int64_t)(x > mx ? mx : x < mn ? mn : x); }
static inline uint64_t sat_u(__int128 x, int W) { __int128 mx = ((__int128)1 << W) - 1; return (uint64_t)(x > mx ? mx : x < 0 ? 0 : x); }
static inline uint64_t trunc_w(__int128 x, int W) { return W == 64 ? (uint64_t)x : (uint64_t)x & ((1ull << W) - 1); }
static inline int64_t sext(uint64_t x, int W) { return W == 64 ? (int64_t)x : (int64_t)(x << (64 - W)) >> (64 - W); }

/* SSHL/USHL/SRSHL/URSHL (register): shift = SInt(count<7:0>), unbounded arithmetic. */
static inline uint64_t ref_shl_reg(uint64_t x, uint8_t cnt, int W, int is_signed, int round) {
  int s = (int8_t)cnt;
  __int128 v = is_signed ? (__int128)sext(x, W) : (__int128)x;
  if (s >= 0) { if (s >= W) return 0; return trunc_w(v << s, W); }
  int r = -s;
  if (r >= 127) return round ? 0 : (is_signed && v < 0 ? trunc_w(-1, W) : 0);
  if (round) v += (__int128)1 << (r - 1);
  return trunc_w(v >> r, W);
}
/* SQSHL/UQSHL/SQRSHL/UQRSHL (register): saturate on left shifts. */
static inline uint64_t ref_qshl_reg(uint64_t x, uint8_t cnt, int W, int is_signed, int round) {
  int s = (int8_t)cnt;
  __int128 v = is_signed ? (__int128)sext(x, W) : (__int128)x;
  if (s >= 0) {
    if (s >= 64) { if (v == 0) return 0; return is_signed ? (uint64_t)sat_s(v < 0 ? -((__int128)1 << 100) : ((__int128)1 << 100), W) : sat_u((__int128)1 << 100, W); }
    __int128 r = v << s;
    return is_signed ? (uint64_t)sat_s(r, W) : sat_u(r, W);
  }
  int r = -s; if (r >= 127) return round ? 0 : (is_signed && v < 0 ? trunc_w(-1, W) : 0);
  if (round) v += (__int128)1 << (r - 1);
  return trunc_w(v >> r, W);
}
/* SQSHLU: signed input, unsigned saturation, left shift by immediate. */
static inline uint64_t ref_sqshlu_l(uint64_t x, int n, int W) { return sat_u((__int128)sext(x, W) << n, W); }
/* Right shifts by immediate with optional rounding: SSHR/USHR/SRSHR/URSHR. n in 1..W. */
static inline uint64_t ref_shr_imm(uint64_t x, int n, int W, int is_signed, int round) {
  __int128 v = is_signed ? (__int128)sext(x, W) : (__int128)x; if (round) v += (__int128)1 << (n - 1); return trunc_w(v >> n, W);
}
/* Narrowing right shifts from 2W to W: SHRN/RSHRN (unsigned view), SQSHRN/SQRSHRN, UQSHRN/UQRSHRN, SQSHRUN/SQRSHRUN. n in 1..W. */
static inline uint64_t ref_shrn_l(uint64_t x, int n, int W2, int round) { __int128 v = (__int128)x; if (round) v += (__int128)1 << (n - 1); return trunc_w(v >> n, W2 / 2); }
static inline uint64_t ref_qshrn_l(uint64_t x, int n, int W2, int in_signed, int out_signed, int round) {
  __int128 v = in_signed ? (__int128)sext(x, W2) : (__int128)x; if (round) v += (__int128)1 << (n - 1); v >>= n;
  return out_signed ? (uint64_t)sat_s(v, W2 / 2) : sat_u(v, W2 / 2);
}
static inline uint64_t ref_sqdmulh_l(uint64_t a, uint64_t b, int W, int round) {
  __int128 p = (__int128)2 * sext(a, W) * sext(b, W); if (round) p += (__int128)1 << (W - 1); return (uint64_t)sat_s(p >> W, W);
}
static inline uint64_t ref_sqdmull_l(uint64_t a, uint64_t b, int W) { return (uint64_t)sat_s((__int128)2 * sext(a, W) * sext(b, W), 2 * W); }
static inline uint8_t ref_rbit8(uint8_t x) { uint8_t r = 0; for (int i = 0; i < 8; i++) if (x & (1 << i)) r |= 0x80 >> i; return r; }
static inline int ref_clz_l(uint64_t x, int W) { int n = 0; for (int i = W - 1; i >= 0 && !((x >> i) & 1); i--) n++; return n; }
static inline int ref_cls_l(uint64_t x, int W) { int s = (x >> (W - 1)) & 1, n = 0; for (int i = W - 2; i >= 0 && ((x >> i) & 1) == (unsigned)s; i--) n++; return n; }
/* FPRecipEstimate / FPRSqrtEstimate table steps (Arm ARM shared/functions/float). */
static inline uint32_t arm_recip_estimate(uint32_t a /* 256..511 */) { uint32_t b = 2 * a + 1; uint32_t r = (1u << 19) / b; r = (r + 1) >> 1; return r; /* 256..511 */ }
/* RecipSqrtEstimate(a), Arm ARM shared/functions/float: a in 128..511. */
static inline uint32_t arm_rsqrt_estimate(uint32_t a) {
  a = a < 256 ? a * 2 + 1 : (((a >> 1) << 1) + 1) * 2;   /* 257..511 (0.25..0.5) or 514..1022 (0.5..1.0) */
  uint32_t b = 512;
  while (a * (b + 1) * (b + 1) < (1u << 28)) b++;
  return (b + 1) / 2;                                /* 256..511 */
}
/* FPRecipEstimate, single precision, FPCR.FZ = 0, DN = 0 (Linux default). */
static inline uint32_t ref_frecpe32(uint32_t x) {
  uint32_t sign = x & 0x80000000u; int exp = (x >> 23) & 0xFF; uint32_t frac = x & 0x7FFFFF;
  if (exp == 0xFF) { if (frac) return x | 0x400000; return sign; }          /* NaN quieted; +-inf -> +-0 */
  if (exp == 0 && frac == 0) return sign | 0x7F800000;                       /* +-0 -> +-inf */
  if (exp == 0 && frac < (1u << 21)) return sign | 0x7F800000;               /* |x| < 2^-128: overflow -> inf */
  if (exp == 0) { if (!(frac & (1u << 22))) { exp = -1; frac = (frac << 2) & 0x7FFFFF; } else { exp = 0; frac = (frac << 1) & 0x7FFFFF; } }
  uint32_t scaled = 256 + (frac >> 15);
  int result_exp = 253 - exp;                                                /* 255 - exp - 2: 1/x ~ 2^-(e+1) */
  uint32_t est = arm_recip_estimate(scaled);
  uint32_t fraction = (est & 0xFF) << 15;
  if (result_exp == 0) { fraction = (1u << 22) | (fraction >> 1); }                        /* denormal result (input exp 253) */
  else if (result_exp == -1) { fraction = (1u << 21) | (fraction >> 2); result_exp = 0; }   /* input exp 254 */
  return sign | ((uint32_t)result_exp << 23) | fraction;
}
/* FPRSqrtEstimate, single precision. */
static inline uint32_t ref_frsqrte32(uint32_t x) {
  uint32_t sign = x & 0x80000000u; int exp = (x >> 23) & 0xFF; uint32_t frac = x & 0x7FFFFF;
  if (exp == 0xFF && frac) return x | 0x400000;
  if (exp == 0 && frac == 0) return sign | 0x7F800000;                       /* +-0 -> +-inf */
  if (sign) return 0x7FC00000;                                               /* negative -> default NaN */
  if (exp == 0xFF) return 0;                                                 /* +inf -> +0 */
  if (exp == 0) { while (!(frac & (1u << 22))) { frac <<= 1; exp--; } frac = (frac << 1) & 0x7FFFFF; }
  uint32_t scaled = (exp & 1) ? 128 + (frac >> 16) : 256 + (frac >> 15);   /* biased exponent odd: '01':fraction<22:16> [MEASURED on the Pi] */
  int result_exp = (380 - exp) / 2;                                          /* 380 - exp > 0 here */
  uint32_t est = arm_rsqrt_estimate(scaled);
  return sign | ((uint32_t)result_exp << 23) | ((est & 0xFF) << 15);
}
/* AES */
static const uint8_t SBOX[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };
static uint8_t ISBOX[256];
static inline void aes_init(void) { for (int i = 0; i < 256; i++) ISBOX[SBOX[i]] = (uint8_t)i; }
static inline uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static inline uint8_t gm(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; }
static inline void aes_shiftrows(uint8_t* s) { uint8_t t[16]; for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[4 * c + r] = s[4 * ((c + r) & 3) + r]; memcpy(s, t, 16); }
static inline void aes_invshiftrows(uint8_t* s) { uint8_t t[16]; for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[4 * ((c + r) & 3) + r] = s[4 * c + r]; memcpy(s, t, 16); }
static inline void aes_subbytes(uint8_t* s, const uint8_t* box) { for (int i = 0; i < 16; i++) s[i] = box[s[i]]; }
static inline void aes_mixcolumns(uint8_t* s) { for (int c = 0; c < 4; c++) { uint8_t* p = s + 4 * c; uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3]; p[0] = gm(a0, 2) ^ gm(a1, 3) ^ a2 ^ a3; p[1] = a0 ^ gm(a1, 2) ^ gm(a2, 3) ^ a3; p[2] = a0 ^ a1 ^ gm(a2, 2) ^ gm(a3, 3); p[3] = gm(a0, 3) ^ a1 ^ a2 ^ gm(a3, 2); } }
static inline void aes_invmixcolumns(uint8_t* s) { for (int c = 0; c < 4; c++) { uint8_t* p = s + 4 * c; uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3]; p[0] = gm(a0, 14) ^ gm(a1, 11) ^ gm(a2, 13) ^ gm(a3, 9); p[1] = gm(a0, 9) ^ gm(a1, 14) ^ gm(a2, 11) ^ gm(a3, 13); p[2] = gm(a0, 13) ^ gm(a1, 9) ^ gm(a2, 14) ^ gm(a3, 11); p[3] = gm(a0, 11) ^ gm(a1, 13) ^ gm(a2, 9) ^ gm(a3, 14); } }
static inline void ref_aese(V* r, const V* s, const V* k) { for (int i = 0; i < 16; i++) r->b[i] = s->b[i] ^ k->b[i]; aes_shiftrows(r->b); aes_subbytes(r->b, SBOX); }
static inline void ref_aesd(V* r, const V* s, const V* k) { for (int i = 0; i < 16; i++) r->b[i] = s->b[i] ^ k->b[i]; aes_invshiftrows(r->b); aes_subbytes(r->b, ISBOX); }
static inline void ref_aesmc(V* r, const V* s) { *r = *s; aes_mixcolumns(r->b); }
static inline void ref_aesimc(V* r, const V* s) { *r = *s; aes_invmixcolumns(r->b); }
/* carry-less multiply 64x64 -> 128 */
static inline void clmul64(uint64_t a, uint64_t b, uint64_t* lo, uint64_t* hi) { unsigned __int128 r = 0; for (int i = 0; i < 64; i++) if ((b >> i) & 1) r ^= (unsigned __int128)a << i; *lo = (uint64_t)r; *hi = (uint64_t)(r >> 64); }
static inline uint16_t clmul8(uint8_t a, uint8_t b) { uint16_t r = 0; for (int i = 0; i < 8; i++) if ((b >> i) & 1) r ^= (uint16_t)a << i; return r; }

/* Float helpers: NaN quieting per FPProcessNaN. */
static inline uint32_t f32_quiet(uint32_t x) { return ((x & 0x7F800000) == 0x7F800000 && (x & 0x7FFFFF)) ? (x | 0x400000) : x; }
static inline int f32_isnan(uint32_t x) { return (x & 0x7F800000) == 0x7F800000 && (x & 0x7FFFFF); }
static inline int f32_issnan(uint32_t x) { return f32_isnan(x) && !(x & 0x400000); }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint64_t d2u(double f) { uint64_t u; memcpy(&u, &f, 8); return u; }
static inline double u2d(uint64_t u) { double f; memcpy(&f, &u, 8); return f; }

#endif
