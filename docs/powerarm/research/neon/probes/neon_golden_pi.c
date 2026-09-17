/* neon_golden_pi.c -- runs the shared corpus through the real AArch64
 * Advanced SIMD instructions (arm_neon.h intrinsics) on a Raspberry Pi 5 and
 * checks the C reference model in neon_ref.h against them lane by lane.
 * Prints one line per case: name W ref-hash native-hash OK|MISMATCH.
 * The ref-hash column is what the POWER parity probe must reproduce.
 *
 * Build (Pi):  gcc -O2 -march=armv8.2-a+crypto+dotprod -o neon_golden_pi neon_golden_pi.c -lm
 * Run:         ./neon_golden_pi > pi.golden
 */
#include <arm_neon.h>
#include "neon_ref.h"

#define LD8(v)  vld1q_u8((v)->b)
#define LDS8(v) vld1q_s8((v)->sb)
#define LD16(v) vld1q_u16((v)->h)
#define LDS16(v) vld1q_s16((v)->sh)
#define LD32(v) vld1q_u32((v)->w)
#define LDS32(v) vld1q_s32((v)->sw)
#define LD64(v) vld1q_u64((v)->d)
#define LDS64(v) vld1q_s64((v)->sd)
#define ST8(r, x) vst1q_u8((r)->b, (x))
#define STS8(r, x) vst1q_s8((r)->sb, (x))
#define ST16(r, x) vst1q_u16((r)->h, (x))
#define STS16(r, x) vst1q_s16((r)->sh, (x))
#define ST32(r, x) vst1q_u32((r)->w, (x))
#define STS32(r, x) vst1q_s32((r)->sw, (x))
#define ST64(r, x) vst1q_u64((r)->d, (x))
#define STS64(r, x) vst1q_s64((r)->sd, (x))
#define ZERO(r) memset((r)->b, 0, 16)

/* unsigned binary at all widths */
#define UBIN(name, i8, i16, i32, i64) static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { \
  switch (W) { case 8: ST8(r, i8(LD8(a), LD8(b))); break; case 16: ST16(r, i16(LD16(a), LD16(b))); break; \
  case 32: ST32(r, i32(LD32(a), LD32(b))); break; default: ST64(r, i64(LD64(a), LD64(b))); } }
#define SBIN(name, i8, i16, i32, i64) static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { \
  switch (W) { case 8: STS8(r, i8(LDS8(a), LDS8(b))); break; case 16: STS16(r, i16(LDS16(a), LDS16(b))); break; \
  case 32: STS32(r, i32(LDS32(a), LDS32(b))); break; default: STS64(r, i64(LDS64(a), LDS64(b))); } }
#define SBIN3(name, i8, i16, i32) static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { \
  switch (W) { case 8: STS8(r, i8(LDS8(a), LDS8(b))); break; case 16: STS16(r, i16(LDS16(a), LDS16(b))); break; \
  default: STS32(r, i32(LDS32(a), LDS32(b))); } }
#define UBIN3(name, i8, i16, i32) static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { \
  switch (W) { case 8: ST8(r, i8(LD8(a), LD8(b))); break; case 16: ST16(r, i16(LD16(a), LD16(b))); break; \
  default: ST32(r, i32(LD32(a), LD32(b))); } }
/* a is data (signed or unsigned), b is the signed count vector */
#define SHREG_U(name, i8, i16, i32, i64) static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { \
  switch (W) { case 8: ST8(r, i8(LD8(a), LDS8(b))); break; case 16: ST16(r, i16(LD16(a), LDS16(b))); break; \
  case 32: ST32(r, i32(LD32(a), LDS32(b))); break; default: ST64(r, i64(LD64(a), LDS64(b))); } }
#define SHREG_S(name, i8, i16, i32, i64) static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { \
  switch (W) { case 8: STS8(r, i8(LDS8(a), LDS8(b))); break; case 16: STS16(r, i16(LDS16(a), LDS16(b))); break; \
  case 32: STS32(r, i32(LDS32(a), LDS32(b))); break; default: STS64(r, i64(LDS64(a), LDS64(b))); } }

UBIN(nat_cmhi, vcgtq_u8, vcgtq_u16, vcgtq_u32, vcgtq_u64)
UBIN(nat_cmhs, vcgeq_u8, vcgeq_u16, vcgeq_u32, vcgeq_u64)
static void nat_cmgt(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: ST8(r, vcgtq_s8(LDS8(a), LDS8(b))); break; case 16: ST16(r, vcgtq_s16(LDS16(a), LDS16(b))); break; case 32: ST32(r, vcgtq_s32(LDS32(a), LDS32(b))); break; default: ST64(r, vcgtq_s64(LDS64(a), LDS64(b))); } }
static void nat_cmge(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: ST8(r, vcgeq_s8(LDS8(a), LDS8(b))); break; case 16: ST16(r, vcgeq_s16(LDS16(a), LDS16(b))); break; case 32: ST32(r, vcgeq_s32(LDS32(a), LDS32(b))); break; default: ST64(r, vcgeq_s64(LDS64(a), LDS64(b))); } }
UBIN(nat_cmtst, vtstq_u8, vtstq_u16, vtstq_u32, vtstq_u64)
SBIN(nat_sqadd, vqaddq_s8, vqaddq_s16, vqaddq_s32, vqaddq_s64)
UBIN(nat_uqadd, vqaddq_u8, vqaddq_u16, vqaddq_u32, vqaddq_u64)
SBIN(nat_sqsub, vqsubq_s8, vqsubq_s16, vqsubq_s32, vqsubq_s64)
UBIN(nat_uqsub, vqsubq_u8, vqsubq_u16, vqsubq_u32, vqsubq_u64)
static void nat_suqadd(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: STS8(r, vuqaddq_s8(LDS8(a), LD8(b))); break; case 16: STS16(r, vuqaddq_s16(LDS16(a), LD16(b))); break; case 32: STS32(r, vuqaddq_s32(LDS32(a), LD32(b))); break; default: STS64(r, vuqaddq_s64(LDS64(a), LD64(b))); } }
static void nat_usqadd(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: ST8(r, vsqaddq_u8(LD8(a), LDS8(b))); break; case 16: ST16(r, vsqaddq_u16(LD16(a), LDS16(b))); break; case 32: ST32(r, vsqaddq_u32(LD32(a), LDS32(b))); break; default: ST64(r, vsqaddq_u64(LD64(a), LDS64(b))); } }
static void nat_sqabs(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: STS8(r, vqabsq_s8(LDS8(a))); break; case 16: STS16(r, vqabsq_s16(LDS16(a))); break; case 32: STS32(r, vqabsq_s32(LDS32(a))); break; default: STS64(r, vqabsq_s64(LDS64(a))); } }
static void nat_sqneg(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: STS8(r, vqnegq_s8(LDS8(a))); break; case 16: STS16(r, vqnegq_s16(LDS16(a))); break; case 32: STS32(r, vqnegq_s32(LDS32(a))); break; default: STS64(r, vqnegq_s64(LDS64(a))); } }
static void nat_abs(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: STS8(r, vabsq_s8(LDS8(a))); break; case 16: STS16(r, vabsq_s16(LDS16(a))); break; case 32: STS32(r, vabsq_s32(LDS32(a))); break; default: STS64(r, vabsq_s64(LDS64(a))); } }
static void nat_neg(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: STS8(r, vnegq_s8(LDS8(a))); break; case 16: STS16(r, vnegq_s16(LDS16(a))); break; case 32: STS32(r, vnegq_s32(LDS32(a))); break; default: STS64(r, vnegq_s64(LDS64(a))); } }
UBIN3(nat_mul, vmulq_u8, vmulq_u16, vmulq_u32)
static void nat_mla(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: ST8(r, vmlaq_u8(LD8(c), LD8(a), LD8(b))); break; case 16: ST16(r, vmlaq_u16(LD16(c), LD16(a), LD16(b))); break; default: ST32(r, vmlaq_u32(LD32(c), LD32(a), LD32(b))); } }
static void nat_mls(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: ST8(r, vmlsq_u8(LD8(c), LD8(a), LD8(b))); break; case 16: ST16(r, vmlsq_u16(LD16(c), LD16(a), LD16(b))); break; default: ST32(r, vmlsq_u32(LD32(c), LD32(a), LD32(b))); } }
static void nat_sqdmulh(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 16) STS16(r, vqdmulhq_s16(LDS16(a), LDS16(b))); else STS32(r, vqdmulhq_s32(LDS32(a), LDS32(b))); }
static void nat_sqrdmulh(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 16) STS16(r, vqrdmulhq_s16(LDS16(a), LDS16(b))); else STS32(r, vqrdmulhq_s32(LDS32(a), LDS32(b))); }
static void nat_sqdmull(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 16) STS32(r, vqdmull_s16(vget_low_s16(LDS16(a)), vget_low_s16(LDS16(b)))); else STS64(r, vqdmull_s32(vget_low_s32(LDS32(a)), vget_low_s32(LDS32(b)))); }
static void nat_smull(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: STS16(r, vmull_s8(vget_low_s8(LDS8(a)), vget_low_s8(LDS8(b)))); break; case 16: STS32(r, vmull_s16(vget_low_s16(LDS16(a)), vget_low_s16(LDS16(b)))); break; default: STS64(r, vmull_s32(vget_low_s32(LDS32(a)), vget_low_s32(LDS32(b)))); } }
static void nat_umull(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: ST16(r, vmull_u8(vget_low_u8(LD8(a)), vget_low_u8(LD8(b)))); break; case 16: ST32(r, vmull_u16(vget_low_u16(LD16(a)), vget_low_u16(LD16(b)))); break; default: ST64(r, vmull_u32(vget_low_u32(LD32(a)), vget_low_u32(LD32(b)))); } }
static void nat_smull2(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: STS16(r, vmull_high_s8(LDS8(a), LDS8(b))); break; case 16: STS32(r, vmull_high_s16(LDS16(a), LDS16(b))); break; default: STS64(r, vmull_high_s32(LDS32(a), LDS32(b))); } }
static void nat_umull2(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: ST16(r, vmull_high_u8(LD8(a), LD8(b))); break; case 16: ST32(r, vmull_high_u16(LD16(a), LD16(b))); break; default: ST64(r, vmull_high_u32(LD32(a), LD32(b))); } }
SBIN3(nat_sabd, vabdq_s8, vabdq_s16, vabdq_s32)
UBIN3(nat_uabd, vabdq_u8, vabdq_u16, vabdq_u32)
static void nat_uabdl(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: ST16(r, vabdl_u8(vget_low_u8(LD8(a)), vget_low_u8(LD8(b)))); break; case 16: ST32(r, vabdl_u16(vget_low_u16(LD16(a)), vget_low_u16(LD16(b)))); break; default: ST64(r, vabdl_u32(vget_low_u32(LD32(a)), vget_low_u32(LD32(b)))); } }
static void nat_sabdl(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: STS16(r, vabdl_s8(vget_low_s8(LDS8(a)), vget_low_s8(LDS8(b)))); break; case 16: STS32(r, vabdl_s16(vget_low_s16(LDS16(a)), vget_low_s16(LDS16(b)))); break; default: STS64(r, vabdl_s32(vget_low_s32(LDS32(a)), vget_low_s32(LDS32(b)))); } }
SHREG_S(nat_sshl, vshlq_s8, vshlq_s16, vshlq_s32, vshlq_s64)
SHREG_U(nat_ushl, vshlq_u8, vshlq_u16, vshlq_u32, vshlq_u64)
SHREG_S(nat_srshl, vrshlq_s8, vrshlq_s16, vrshlq_s32, vrshlq_s64)
SHREG_U(nat_urshl, vrshlq_u8, vrshlq_u16, vrshlq_u32, vrshlq_u64)
SHREG_S(nat_sqshl_r, vqshlq_s8, vqshlq_s16, vqshlq_s32, vqshlq_s64)
SHREG_U(nat_uqshl_r, vqshlq_u8, vqshlq_u16, vqshlq_u32, vqshlq_u64)
SHREG_S(nat_sqrshl, vqrshlq_s8, vqrshlq_s16, vqrshlq_s32, vqrshlq_s64)
SHREG_U(nat_uqrshl, vqrshlq_u8, vqrshlq_u16, vqrshlq_u32, vqrshlq_u64)

/* immediates: expand a switch over the legal range */
#define SW8(X)  case 1: X(1); break; case 2: X(2); break; case 3: X(3); break; case 4: X(4); break; case 5: X(5); break; case 6: X(6); break; case 7: X(7); break; case 8: X(8); break;
#define SW16(X) SW8(X) case 9: X(9); break; case 10: X(10); break; case 11: X(11); break; case 12: X(12); break; case 13: X(13); break; case 14: X(14); break; case 15: X(15); break; case 16: X(16); break;
#define SW32(X) SW16(X) case 17: X(17); break; case 18: X(18); break; case 19: X(19); break; case 20: X(20); break; case 21: X(21); break; case 22: X(22); break; case 23: X(23); break; case 24: X(24); break; case 25: X(25); break; case 26: X(26); break; case 27: X(27); break; case 28: X(28); break; case 29: X(29); break; case 30: X(30); break; case 31: X(31); break; case 32: X(32); break;
#define SW64(X) SW32(X) case 33: X(33); break; case 34: X(34); break; case 35: X(35); break; case 36: X(36); break; case 37: X(37); break; case 38: X(38); break; case 39: X(39); break; case 40: X(40); break; case 41: X(41); break; case 42: X(42); break; case 43: X(43); break; case 44: X(44); break; case 45: X(45); break; case 46: X(46); break; case 47: X(47); break; case 48: X(48); break; case 49: X(49); break; case 50: X(50); break; case 51: X(51); break; case 52: X(52); break; case 53: X(53); break; case 54: X(54); break; case 55: X(55); break; case 56: X(56); break; case 57: X(57); break; case 58: X(58); break; case 59: X(59); break; case 60: X(60); break; case 61: X(61); break; case 62: X(62); break; case 63: X(63); break; case 64: X(64); break;
#define SW0_7(X) case 0: X(0); break; case 1: X(1); break; case 2: X(2); break; case 3: X(3); break; case 4: X(4); break; case 5: X(5); break; case 6: X(6); break; case 7: X(7); break;
#define SW0_15(X) SW0_7(X) case 8: X(8); break; case 9: X(9); break; case 10: X(10); break; case 11: X(11); break; case 12: X(12); break; case 13: X(13); break; case 14: X(14); break; case 15: X(15); break;
#define SW0_31(X) SW0_15(X) case 16: X(16); break; case 17: X(17); break; case 18: X(18); break; case 19: X(19); break; case 20: X(20); break; case 21: X(21); break; case 22: X(22); break; case 23: X(23); break; case 24: X(24); break; case 25: X(25); break; case 26: X(26); break; case 27: X(27); break; case 28: X(28); break; case 29: X(29); break; case 30: X(30); break; case 31: X(31); break;
#define SW0_63(X) SW0_31(X) case 32: X(32); break; case 33: X(33); break; case 34: X(34); break; case 35: X(35); break; case 36: X(36); break; case 37: X(37); break; case 38: X(38); break; case 39: X(39); break; case 40: X(40); break; case 41: X(41); break; case 42: X(42); break; case 43: X(43); break; case 44: X(44); break; case 45: X(45); break; case 46: X(46); break; case 47: X(47); break; case 48: X(48); break; case 49: X(49); break; case 50: X(50); break; case 51: X(51); break; case 52: X(52); break; case 53: X(53); break; case 54: X(54); break; case 55: X(55); break; case 56: X(56); break; case 57: X(57); break; case 58: X(58); break; case 59: X(59); break; case 60: X(60); break; case 61: X(61); break; case 62: X(62); break; case 63: X(63); break;

/* Simpler: per-width helper functions with macro-expanded switches. */
#define DEF_SHR(name, W, IN, LD, ST) static void name##_##W(V* r, const V* a, int imm) { switch (imm) { \
  SW##W(name##_X##W) default: ZERO(r); } }
#define DEFSHR_ALL(name, intr8, intr16, intr32, intr64, LD, ST, LD16, ST16, LD32, ST32, LD64, ST64) \
  static void name##_8(V* r, const V* a, int imm) { switch (imm) { \
    case 1: ST(r, intr8(LD(a), 1)); break; case 2: ST(r, intr8(LD(a), 2)); break; case 3: ST(r, intr8(LD(a), 3)); break; case 4: ST(r, intr8(LD(a), 4)); break; \
    case 5: ST(r, intr8(LD(a), 5)); break; case 6: ST(r, intr8(LD(a), 6)); break; case 7: ST(r, intr8(LD(a), 7)); break; case 8: ST(r, intr8(LD(a), 8)); break; default: ZERO(r); } } \
  static void name##_16(V* r, const V* a, int imm) { switch (imm) { \
    case 1: ST16(r, intr16(LD16(a), 1)); break; case 2: ST16(r, intr16(LD16(a), 2)); break; case 3: ST16(r, intr16(LD16(a), 3)); break; case 4: ST16(r, intr16(LD16(a), 4)); break; \
    case 5: ST16(r, intr16(LD16(a), 5)); break; case 6: ST16(r, intr16(LD16(a), 6)); break; case 7: ST16(r, intr16(LD16(a), 7)); break; case 8: ST16(r, intr16(LD16(a), 8)); break; \
    case 9: ST16(r, intr16(LD16(a), 9)); break; case 10: ST16(r, intr16(LD16(a), 10)); break; case 11: ST16(r, intr16(LD16(a), 11)); break; case 12: ST16(r, intr16(LD16(a), 12)); break; \
    case 13: ST16(r, intr16(LD16(a), 13)); break; case 14: ST16(r, intr16(LD16(a), 14)); break; case 15: ST16(r, intr16(LD16(a), 15)); break; case 16: ST16(r, intr16(LD16(a), 16)); break; default: ZERO(r); } } \
  static void name##_32(V* r, const V* a, int imm) { switch (imm) { \
    case 1: ST32(r, intr32(LD32(a), 1)); break; case 2: ST32(r, intr32(LD32(a), 2)); break; case 3: ST32(r, intr32(LD32(a), 3)); break; case 5: ST32(r, intr32(LD32(a), 5)); break; \
    case 8: ST32(r, intr32(LD32(a), 8)); break; case 15: ST32(r, intr32(LD32(a), 15)); break; case 16: ST32(r, intr32(LD32(a), 16)); break; case 17: ST32(r, intr32(LD32(a), 17)); break; \
    case 24: ST32(r, intr32(LD32(a), 24)); break; case 31: ST32(r, intr32(LD32(a), 31)); break; case 32: ST32(r, intr32(LD32(a), 32)); break; default: ZERO(r); } } \
  static void name##_64(V* r, const V* a, int imm) { switch (imm) { \
    case 1: ST64(r, intr64(LD64(a), 1)); break; case 2: ST64(r, intr64(LD64(a), 2)); break; case 7: ST64(r, intr64(LD64(a), 7)); break; case 16: ST64(r, intr64(LD64(a), 16)); break; \
    case 31: ST64(r, intr64(LD64(a), 31)); break; case 32: ST64(r, intr64(LD64(a), 32)); break; case 33: ST64(r, intr64(LD64(a), 33)); break; case 48: ST64(r, intr64(LD64(a), 48)); break; \
    case 63: ST64(r, intr64(LD64(a), 63)); break; case 64: ST64(r, intr64(LD64(a), 64)); break; default: ZERO(r); } } \
  static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: name##_8(r, a, imm); break; case 16: name##_16(r, a, imm); break; case 32: name##_32(r, a, imm); break; default: name##_64(r, a, imm); } }
DEFSHR_ALL(nat_sshr, vshrq_n_s8, vshrq_n_s16, vshrq_n_s32, vshrq_n_s64, LDS8, STS8, LDS16, STS16, LDS32, STS32, LDS64, STS64)
DEFSHR_ALL(nat_ushr, vshrq_n_u8, vshrq_n_u16, vshrq_n_u32, vshrq_n_u64, LD8, ST8, LD16, ST16, LD32, ST32, LD64, ST64)
DEFSHR_ALL(nat_srshr, vrshrq_n_s8, vrshrq_n_s16, vrshrq_n_s32, vrshrq_n_s64, LDS8, STS8, LDS16, STS16, LDS32, STS32, LDS64, STS64)
DEFSHR_ALL(nat_urshr, vrshrq_n_u8, vrshrq_n_u16, vrshrq_n_u32, vrshrq_n_u64, LD8, ST8, LD16, ST16, LD32, ST32, LD64, ST64)
/* The shift-immediate case list: only the immediates listed in DEFSHR_ALL for 32/64 are exercised (the Case table uses lo..hi and the default ZERO for others; we mirror that in the ref by skipping unlisted immediates via IMMOK). */
/* left shifts 0..W-1: sqshl/uqshl/sqshlu/shl by imm; using the same immediate subsets (0 excluded for 32/64 sets that start at 1; handle 0 by copying) */
#define DEFSHL_ALL(name, intr8, intr16, intr32, intr64, LD, ST, LD16, ST16, LD32, ST32, LD64, ST64) \
  static void name##_8(V* r, const V* a, int imm) { switch (imm) { case 0: ST(r, intr8(LD(a), 0)); break; \
    case 1: ST(r, intr8(LD(a), 1)); break; case 2: ST(r, intr8(LD(a), 2)); break; case 3: ST(r, intr8(LD(a), 3)); break; case 4: ST(r, intr8(LD(a), 4)); break; \
    case 5: ST(r, intr8(LD(a), 5)); break; case 6: ST(r, intr8(LD(a), 6)); break; case 7: ST(r, intr8(LD(a), 7)); break; default: ZERO(r); } } \
  static void name##_16(V* r, const V* a, int imm) { switch (imm) { case 0: ST16(r, intr16(LD16(a), 0)); break; \
    case 1: ST16(r, intr16(LD16(a), 1)); break; case 2: ST16(r, intr16(LD16(a), 2)); break; case 3: ST16(r, intr16(LD16(a), 3)); break; case 4: ST16(r, intr16(LD16(a), 4)); break; \
    case 5: ST16(r, intr16(LD16(a), 5)); break; case 6: ST16(r, intr16(LD16(a), 6)); break; case 7: ST16(r, intr16(LD16(a), 7)); break; case 8: ST16(r, intr16(LD16(a), 8)); break; \
    case 9: ST16(r, intr16(LD16(a), 9)); break; case 10: ST16(r, intr16(LD16(a), 10)); break; case 11: ST16(r, intr16(LD16(a), 11)); break; case 12: ST16(r, intr16(LD16(a), 12)); break; \
    case 13: ST16(r, intr16(LD16(a), 13)); break; case 14: ST16(r, intr16(LD16(a), 14)); break; case 15: ST16(r, intr16(LD16(a), 15)); break; default: ZERO(r); } } \
  static void name##_32(V* r, const V* a, int imm) { switch (imm) { case 0: ST32(r, intr32(LD32(a), 0)); break; \
    case 1: ST32(r, intr32(LD32(a), 1)); break; case 2: ST32(r, intr32(LD32(a), 2)); break; case 3: ST32(r, intr32(LD32(a), 3)); break; case 5: ST32(r, intr32(LD32(a), 5)); break; \
    case 8: ST32(r, intr32(LD32(a), 8)); break; case 15: ST32(r, intr32(LD32(a), 15)); break; case 16: ST32(r, intr32(LD32(a), 16)); break; case 17: ST32(r, intr32(LD32(a), 17)); break; \
    case 24: ST32(r, intr32(LD32(a), 24)); break; case 31: ST32(r, intr32(LD32(a), 31)); break; default: ZERO(r); } } \
  static void name##_64(V* r, const V* a, int imm) { switch (imm) { case 0: ST64(r, intr64(LD64(a), 0)); break; \
    case 1: ST64(r, intr64(LD64(a), 1)); break; case 2: ST64(r, intr64(LD64(a), 2)); break; case 7: ST64(r, intr64(LD64(a), 7)); break; case 16: ST64(r, intr64(LD64(a), 16)); break; \
    case 31: ST64(r, intr64(LD64(a), 31)); break; case 32: ST64(r, intr64(LD64(a), 32)); break; case 33: ST64(r, intr64(LD64(a), 33)); break; case 48: ST64(r, intr64(LD64(a), 48)); break; \
    case 63: ST64(r, intr64(LD64(a), 63)); break; default: ZERO(r); } } \
  static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (W) { case 8: name##_8(r, a, imm); break; case 16: name##_16(r, a, imm); break; case 32: name##_32(r, a, imm); break; default: name##_64(r, a, imm); } }
DEFSHL_ALL(nat_shl, vshlq_n_u8, vshlq_n_u16, vshlq_n_u32, vshlq_n_u64, LD8, ST8, LD16, ST16, LD32, ST32, LD64, ST64)
DEFSHL_ALL(nat_sqshl_i, vqshlq_n_s8, vqshlq_n_s16, vqshlq_n_s32, vqshlq_n_s64, LDS8, STS8, LDS16, STS16, LDS32, STS32, LDS64, STS64)
DEFSHL_ALL(nat_uqshl_i, vqshlq_n_u8, vqshlq_n_u16, vqshlq_n_u32, vqshlq_n_u64, LD8, ST8, LD16, ST16, LD32, ST32, LD64, ST64)
/* sqshlu: signed in, unsigned out */
#define LDSU8 LDS8
static void nat_sqshlu(V* r, const V* a, const V* b, const V* c, int W, int imm) {
  switch (W) {
  case 8: switch (imm) { case 0: ST8(r, vqshluq_n_s8(LDS8(a), 0)); break; case 1: ST8(r, vqshluq_n_s8(LDS8(a), 1)); break; case 2: ST8(r, vqshluq_n_s8(LDS8(a), 2)); break; case 3: ST8(r, vqshluq_n_s8(LDS8(a), 3)); break; case 4: ST8(r, vqshluq_n_s8(LDS8(a), 4)); break; case 5: ST8(r, vqshluq_n_s8(LDS8(a), 5)); break; case 6: ST8(r, vqshluq_n_s8(LDS8(a), 6)); break; case 7: ST8(r, vqshluq_n_s8(LDS8(a), 7)); break; default: ZERO(r); } break;
  case 16: switch (imm) { case 0: ST16(r, vqshluq_n_s16(LDS16(a), 0)); break; case 1: ST16(r, vqshluq_n_s16(LDS16(a), 1)); break; case 7: ST16(r, vqshluq_n_s16(LDS16(a), 7)); break; case 8: ST16(r, vqshluq_n_s16(LDS16(a), 8)); break; case 15: ST16(r, vqshluq_n_s16(LDS16(a), 15)); break; default: ZERO(r); } break;
  case 32: switch (imm) { case 0: ST32(r, vqshluq_n_s32(LDS32(a), 0)); break; case 1: ST32(r, vqshluq_n_s32(LDS32(a), 1)); break; case 16: ST32(r, vqshluq_n_s32(LDS32(a), 16)); break; case 31: ST32(r, vqshluq_n_s32(LDS32(a), 31)); break; default: ZERO(r); } break;
  default: switch (imm) { case 0: ST64(r, vqshluq_n_s64(LDS64(a), 0)); break; case 1: ST64(r, vqshluq_n_s64(LDS64(a), 1)); break; case 32: ST64(r, vqshluq_n_s64(LDS64(a), 32)); break; case 63: ST64(r, vqshluq_n_s64(LDS64(a), 63)); break; default: ZERO(r); } }
}
/* narrowing shifts: W is the output width; source 2W; imm 1..W */
#define DEFSHRN(name, i16, i32, i64, LDsrc16, LDsrc32, LDsrc64, STd8, STd16, STd32) \
  static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); \
    if (W == 8) { switch (imm) { case 1: STd8(r, i16(LDsrc16(a), 1)); break; case 2: STd8(r, i16(LDsrc16(a), 2)); break; case 3: STd8(r, i16(LDsrc16(a), 3)); break; case 4: STd8(r, i16(LDsrc16(a), 4)); break; case 5: STd8(r, i16(LDsrc16(a), 5)); break; case 6: STd8(r, i16(LDsrc16(a), 6)); break; case 7: STd8(r, i16(LDsrc16(a), 7)); break; case 8: STd8(r, i16(LDsrc16(a), 8)); break; } } \
    else if (W == 16) { switch (imm) { case 1: STd16(r, i32(LDsrc32(a), 1)); break; case 2: STd16(r, i32(LDsrc32(a), 2)); break; case 3: STd16(r, i32(LDsrc32(a), 3)); break; case 5: STd16(r, i32(LDsrc32(a), 5)); break; case 8: STd16(r, i32(LDsrc32(a), 8)); break; case 15: STd16(r, i32(LDsrc32(a), 15)); break; case 16: STd16(r, i32(LDsrc32(a), 16)); break; } } \
    else { switch (imm) { case 1: STd32(r, i64(LDsrc64(a), 1)); break; case 2: STd32(r, i64(LDsrc64(a), 2)); break; case 7: STd32(r, i64(LDsrc64(a), 7)); break; case 16: STd32(r, i64(LDsrc64(a), 16)); break; case 31: STd32(r, i64(LDsrc64(a), 31)); break; case 32: STd32(r, i64(LDsrc64(a), 32)); break; } } }
#define STD8(r, x) vst1_u8((r)->b, (x))
#define STDS8(r, x) vst1_s8((r)->sb, (x))
#define STD16(r, x) vst1_u16((r)->h, (x))
#define STDS16(r, x) vst1_s16((r)->sh, (x))
#define STD32(r, x) vst1_u32((r)->w, (x))
#define STDS32(r, x) vst1_s32((r)->sw, (x))
DEFSHRN(nat_shrn, vshrn_n_u16, vshrn_n_u32, vshrn_n_u64, LD16, LD32, LD64, STD8, STD16, STD32)
DEFSHRN(nat_rshrn, vrshrn_n_u16, vrshrn_n_u32, vrshrn_n_u64, LD16, LD32, LD64, STD8, STD16, STD32)
DEFSHRN(nat_sqshrn, vqshrn_n_s16, vqshrn_n_s32, vqshrn_n_s64, LDS16, LDS32, LDS64, STDS8, STDS16, STDS32)
DEFSHRN(nat_sqrshrn, vqrshrn_n_s16, vqrshrn_n_s32, vqrshrn_n_s64, LDS16, LDS32, LDS64, STDS8, STDS16, STDS32)
DEFSHRN(nat_uqshrn, vqshrn_n_u16, vqshrn_n_u32, vqshrn_n_u64, LD16, LD32, LD64, STD8, STD16, STD32)
DEFSHRN(nat_uqrshrn, vqrshrn_n_u16, vqrshrn_n_u32, vqrshrn_n_u64, LD16, LD32, LD64, STD8, STD16, STD32)
DEFSHRN(nat_sqshrun, vqshrun_n_s16, vqshrun_n_s32, vqshrun_n_s64, LDS16, LDS32, LDS64, STD8, STD16, STD32)
DEFSHRN(nat_sqrshrun, vqrshrun_n_s16, vqrshrun_n_s32, vqrshrun_n_s64, LDS16, LDS32, LDS64, STD8, STD16, STD32)
static void nat_xtn(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) STD8(r, vmovn_u16(LD16(a))); else if (W == 16) STD16(r, vmovn_u32(LD32(a))); else STD32(r, vmovn_u64(LD64(a))); }
static void nat_sqxtn(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) STDS8(r, vqmovn_s16(LDS16(a))); else if (W == 16) STDS16(r, vqmovn_s32(LDS32(a))); else STDS32(r, vqmovn_s64(LDS64(a))); }
static void nat_uqxtn(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) STD8(r, vqmovn_u16(LD16(a))); else if (W == 16) STD16(r, vqmovn_u32(LD32(a))); else STD32(r, vqmovn_u64(LD64(a))); }
static void nat_sqxtun(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) STD8(r, vqmovun_s16(LDS16(a))); else if (W == 16) STD16(r, vqmovun_s32(LDS32(a))); else STD32(r, vqmovun_s64(LDS64(a))); }
static void nat_addhn(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) STD8(r, vaddhn_u16(LD16(a), LD16(b))); else if (W == 16) STD16(r, vaddhn_u32(LD32(a), LD32(b))); else STD32(r, vaddhn_u64(LD64(a), LD64(b))); }
static void nat_raddhn(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) STD8(r, vraddhn_u16(LD16(a), LD16(b))); else if (W == 16) STD16(r, vraddhn_u32(LD32(a), LD32(b))); else STD32(r, vraddhn_u64(LD64(a), LD64(b))); }
static void nat_subhn(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) STD8(r, vsubhn_u16(LD16(a), LD16(b))); else if (W == 16) STD16(r, vsubhn_u32(LD32(a), LD32(b))); else STD32(r, vsubhn_u64(LD64(a), LD64(b))); }
static void nat_sxtl(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) STS16(r, vmovl_s8(vget_low_s8(LDS8(a)))); else if (W == 16) STS32(r, vmovl_s16(vget_low_s16(LDS16(a)))); else STS64(r, vmovl_s32(vget_low_s32(LDS32(a)))); }
static void nat_uxtl(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) ST16(r, vmovl_u8(vget_low_u8(LD8(a)))); else if (W == 16) ST32(r, vmovl_u16(vget_low_u16(LD16(a)))); else ST64(r, vmovl_u32(vget_low_u32(LD32(a)))); }
static void nat_sxtl2(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) STS16(r, vmovl_high_s8(LDS8(a))); else if (W == 16) STS32(r, vmovl_high_s16(LDS16(a))); else STS64(r, vmovl_high_s32(LDS32(a))); }
static void nat_uxtl2(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) ST16(r, vmovl_high_u8(LD8(a))); else if (W == 16) ST32(r, vmovl_high_u16(LD16(a))); else ST64(r, vmovl_high_u32(LD32(a))); }
static void nat_saddl(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) STS16(r, vaddl_s8(vget_low_s8(LDS8(a)), vget_low_s8(LDS8(b)))); else if (W == 16) STS32(r, vaddl_s16(vget_low_s16(LDS16(a)), vget_low_s16(LDS16(b)))); else STS64(r, vaddl_s32(vget_low_s32(LDS32(a)), vget_low_s32(LDS32(b)))); }
static void nat_uaddl(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) ST16(r, vaddl_u8(vget_low_u8(LD8(a)), vget_low_u8(LD8(b)))); else if (W == 16) ST32(r, vaddl_u16(vget_low_u16(LD16(a)), vget_low_u16(LD16(b)))); else ST64(r, vaddl_u32(vget_low_u32(LD32(a)), vget_low_u32(LD32(b)))); }
static void nat_saddlp(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) STS16(r, vpaddlq_s8(LDS8(a))); else if (W == 16) STS32(r, vpaddlq_s16(LDS16(a))); else STS64(r, vpaddlq_s32(LDS32(a))); }
static void nat_uaddlp(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) ST16(r, vpaddlq_u8(LD8(a))); else if (W == 16) ST32(r, vpaddlq_u16(LD16(a))); else ST64(r, vpaddlq_u32(LD32(a))); }
static void nat_sadalp(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) STS16(r, vpadalq_s8(LDS16(c), LDS8(a))); else if (W == 16) STS32(r, vpadalq_s16(LDS32(c), LDS16(a))); else STS64(r, vpadalq_s32(LDS64(c), LDS32(a))); }
static void nat_uadalp(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) ST16(r, vpadalq_u8(LD16(c), LD8(a))); else if (W == 16) ST32(r, vpadalq_u16(LD32(c), LD16(a))); else ST64(r, vpadalq_u32(LD64(c), LD32(a))); }
UBIN(nat_addp, vpaddq_u8, vpaddq_u16, vpaddq_u32, vpaddq_u64)
UBIN3(nat_umaxp, vpmaxq_u8, vpmaxq_u16, vpmaxq_u32)
UBIN3(nat_uminp, vpminq_u8, vpminq_u16, vpminq_u32)
SBIN3(nat_smaxp, vpmaxq_s8, vpmaxq_s16, vpmaxq_s32)
SBIN3(nat_sminp, vpminq_s8, vpminq_s16, vpminq_s32)
static void nat_addv(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) r->b[0] = vaddvq_u8(LD8(a)); else if (W == 16) r->h[0] = vaddvq_u16(LD16(a)); else r->w[0] = vaddvq_u32(LD32(a)); }
static void nat_saddlv(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) r->sh[0] = vaddlvq_s8(LDS8(a)); else if (W == 16) r->sw[0] = vaddlvq_s16(LDS16(a)); else r->sd[0] = vaddlvq_s32(LDS32(a)); }
static void nat_uaddlv(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) r->h[0] = vaddlvq_u8(LD8(a)); else if (W == 16) r->w[0] = vaddlvq_u16(LD16(a)); else r->d[0] = vaddlvq_u32(LD32(a)); }
static void nat_umaxv(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) r->b[0] = vmaxvq_u8(LD8(a)); else if (W == 16) r->h[0] = vmaxvq_u16(LD16(a)); else r->w[0] = vmaxvq_u32(LD32(a)); }
static void nat_uminv(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) r->b[0] = vminvq_u8(LD8(a)); else if (W == 16) r->h[0] = vminvq_u16(LD16(a)); else r->w[0] = vminvq_u32(LD32(a)); }
static void nat_smaxv(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) r->sb[0] = vmaxvq_s8(LDS8(a)); else if (W == 16) r->sh[0] = vmaxvq_s16(LDS16(a)); else r->sw[0] = vmaxvq_s32(LDS32(a)); }
static void nat_sminv(V* r, const V* a, const V* b, const V* c, int W, int imm) { ZERO(r); if (W == 8) r->sb[0] = vminvq_s8(LDS8(a)); else if (W == 16) r->sh[0] = vminvq_s16(LDS16(a)); else r->sw[0] = vminvq_s32(LDS32(a)); }
static void nat_cnt(V* r, const V* a, const V* b, const V* c, int W, int imm) { ST8(r, vcntq_u8(LD8(a))); }
static void nat_clz(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) ST8(r, vclzq_u8(LD8(a))); else if (W == 16) ST16(r, vclzq_u16(LD16(a))); else ST32(r, vclzq_u32(LD32(a))); }
static void nat_cls(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) STS8(r, vclsq_s8(LDS8(a))); else if (W == 16) STS16(r, vclsq_s16(LDS16(a))); else STS32(r, vclsq_s32(LDS32(a))); }
static void nat_rbit(V* r, const V* a, const V* b, const V* c, int W, int imm) { ST8(r, vrbitq_u8(LD8(a))); }
static void nat_rev64(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) ST8(r, vrev64q_u8(LD8(a))); else if (W == 16) ST16(r, vrev64q_u16(LD16(a))); else ST32(r, vrev64q_u32(LD32(a))); }
static void nat_rev32(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 8) ST8(r, vrev32q_u8(LD8(a))); else ST16(r, vrev32q_u16(LD16(a))); }
static void nat_rev16(V* r, const V* a, const V* b, const V* c, int W, int imm) { ST8(r, vrev16q_u8(LD8(a))); }
UBIN(nat_zip1, vzip1q_u8, vzip1q_u16, vzip1q_u32, vzip1q_u64)
UBIN(nat_zip2, vzip2q_u8, vzip2q_u16, vzip2q_u32, vzip2q_u64)
UBIN(nat_uzp1, vuzp1q_u8, vuzp1q_u16, vuzp1q_u32, vuzp1q_u64)
UBIN(nat_uzp2, vuzp2q_u8, vuzp2q_u16, vuzp2q_u32, vuzp2q_u64)
UBIN(nat_trn1, vtrn1q_u8, vtrn1q_u16, vtrn1q_u32, vtrn1q_u64)
UBIN(nat_trn2, vtrn2q_u8, vtrn2q_u16, vtrn2q_u32, vtrn2q_u64)
#define EXTX(n) ST8(r, vextq_u8(LD8(a), LD8(b), n))
static void nat_ext(V* r, const V* a, const V* b, const V* c, int W, int imm) { switch (imm) { SW0_15(EXTX) } }
static void nat_tbl(V* r, const V* a, const V* b, const V* c, int W, int imm) {
  uint8x16_t idx = LD8(c);
  switch (imm) {
  case 1: ST8(r, vqtbl1q_u8(LD8(a), idx)); break;
  case 2: { uint8x16x2_t t = { { LD8(a), LD8(b) } }; ST8(r, vqtbl2q_u8(t, idx)); break; }
  case 3: { uint8x16x3_t t = { { LD8(a), LD8(b), LD8(&TBL_T3) } }; ST8(r, vqtbl3q_u8(t, idx)); break; }
  default: { uint8x16x4_t t = { { LD8(a), LD8(b), LD8(&TBL_T3), LD8(&TBL_T4) } }; ST8(r, vqtbl4q_u8(t, idx)); break; } } }
static void nat_tbx(V* r, const V* a, const V* b, const V* c, int W, int imm) {
  uint8x16_t idx = LD8(c), d = LD8(&TBX_D);
  switch (imm) {
  case 1: ST8(r, vqtbx1q_u8(d, LD8(a), idx)); break;
  case 2: { uint8x16x2_t t = { { LD8(a), LD8(b) } }; ST8(r, vqtbx2q_u8(d, t, idx)); break; }
  case 3: { uint8x16x3_t t = { { LD8(a), LD8(b), LD8(&TBL_T3) } }; ST8(r, vqtbx3q_u8(d, t, idx)); break; }
  default: { uint8x16x4_t t = { { LD8(a), LD8(b), LD8(&TBL_T3), LD8(&TBL_T4) } }; ST8(r, vqtbx4q_u8(d, t, idx)); break; } } }
static void nat_udot(V* r, const V* a, const V* b, const V* c, int W, int imm) { ST32(r, vdotq_u32(LD32(c), LD8(a), LD8(b))); }
static void nat_sdot(V* r, const V* a, const V* b, const V* c, int W, int imm) { STS32(r, vdotq_s32(LDS32(c), LDS8(a), LDS8(b))); }
static void nat_pmull(V* r, const V* a, const V* b, const V* c, int W, int imm) { poly128_t p = vmull_p64((poly64_t)a->d[0], (poly64_t)b->d[0]); memcpy(r->b, &p, 16); }
static void nat_pmull2(V* r, const V* a, const V* b, const V* c, int W, int imm) { poly128_t p = vmull_high_p64(vreinterpretq_p64_u64(LD64(a)), vreinterpretq_p64_u64(LD64(b))); memcpy(r->b, &p, 16); }
static void nat_pmull8(V* r, const V* a, const V* b, const V* c, int W, int imm) { poly16x8_t p = vmull_p8(vreinterpret_p8_u8(vget_low_u8(LD8(a))), vreinterpret_p8_u8(vget_low_u8(LD8(b)))); vst1q_u16(r->h, vreinterpretq_u16_p16(p)); }
static void nat_pmull8_2(V* r, const V* a, const V* b, const V* c, int W, int imm) { poly16x8_t p = vmull_high_p8(vreinterpretq_p8_u8(LD8(a)), vreinterpretq_p8_u8(LD8(b))); vst1q_u16(r->h, vreinterpretq_u16_p16(p)); }
static void nat_aese_v(V* r, const V* a, const V* b, const V* c, int W, int imm) { ST8(r, vaeseq_u8(LD8(a), LD8(b))); }
static void nat_aesd_v(V* r, const V* a, const V* b, const V* c, int W, int imm) { ST8(r, vaesdq_u8(LD8(a), LD8(b))); }
static void nat_aesmc_v(V* r, const V* a, const V* b, const V* c, int W, int imm) { ST8(r, vaesmcq_u8(LD8(a))); }
static void nat_aesimc_v(V* r, const V* a, const V* b, const V* c, int W, int imm) { ST8(r, vaesimcq_u8(LD8(a))); }
/* float */
#define LDF(v) vld1q_f32((v)->f)
#define STF(r, x) vst1q_f32((r)->f, (x))
#define LDD(v) vld1q_f64((v)->df)
#define STD(r, x) vst1q_f64((r)->df, (x))
#define FUN(name, i32, i64) static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 32) STF(r, i32(LDF(a))); else STD(r, i64(LDD(a))); }
FUN(nat_frintn, vrndnq_f32, vrndnq_f64)
FUN(nat_frinta, vrndaq_f32, vrndaq_f64)
FUN(nat_frintm, vrndmq_f32, vrndmq_f64)
FUN(nat_frintp, vrndpq_f32, vrndpq_f64)
FUN(nat_frintz, vrndq_f32, vrndq_f64)
static void nat_fcvtzs(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 32) STS32(r, vcvtq_s32_f32(LDF(a))); else STS64(r, vcvtq_s64_f64(LDD(a))); }
static void nat_fcvtzu(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 32) ST32(r, vcvtq_u32_f32(LDF(a))); else ST64(r, vcvtq_u64_f64(LDD(a))); }
static void nat_scvtf(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 32) STF(r, vcvtq_f32_s32(LDS32(a))); else STD(r, vcvtq_f64_s64(LDS64(a))); }
static void nat_ucvtf(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 32) STF(r, vcvtq_f32_u32(LD32(a))); else STD(r, vcvtq_f64_u64(LD64(a))); }
static void nat_frecpe(V* r, const V* a, const V* b, const V* c, int W, int imm) { STF(r, vrecpeq_f32(LDF(a))); }
static void nat_frsqrte(V* r, const V* a, const V* b, const V* c, int W, int imm) { STF(r, vrsqrteq_f32(LDF(a))); }
static void nat_fmla(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 32) STF(r, vfmaq_f32(LDF(c), LDF(a), LDF(b))); else STD(r, vfmaq_f64(LDD(c), LDD(a), LDD(b))); }
static void nat_fmls(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 32) STF(r, vfmsq_f32(LDF(c), LDF(a), LDF(b))); else STD(r, vfmsq_f64(LDD(c), LDD(a), LDD(b))); }
#define FBIN(name, i32, i64) static void name(V* r, const V* a, const V* b, const V* c, int W, int imm) { if (W == 32) STF(r, i32(LDF(a), LDF(b))); else STD(r, i64(LDD(a), LDD(b))); }
FBIN(nat_fmax, vmaxq_f32, vmaxq_f64)
FBIN(nat_fmin, vminq_f32, vminq_f64)
FBIN(nat_fmaxnm, vmaxnmq_f32, vmaxnmq_f64)
FBIN(nat_fminnm, vminnmq_f32, vminnmq_f64)
FBIN(nat_frecps, vrecpsq_f32, vrecpsq_f64)
FBIN(nat_frsqrts, vrsqrtsq_f32, vrsqrtsq_f64)

#define IMPL(n) nat_##n
#include "neon_cases.h"

int main(void) {
  aes_init();
  int fails = 0;
  for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
    Case cs = CASES[i];
    if (!cs.impl) continue;
    uint64_t hr, hn; char fm[400];
    int bad = run_case(&cs, &hr, &hn, fm, sizeof fm);
    printf("%-10s %2d %016llx %016llx %s%s%s\n", cs.name, cs.W, (unsigned long long)hr, (unsigned long long)hn, bad ? "MISMATCH" : "OK", bad ? " " : "", bad ? fm : "");
    if (bad) fails++;
  }
  /* rounding-mode sweep for the fused ops and conversions: the golden hash is per mode */
  static const int modes[] = { FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO }; static const char* mn[] = { "rn", "ru", "rd", "rz" };
  for (int m = 0; m < 4; m++) {
    fesetround(modes[m]);
    for (size_t i = 0; i < sizeof RMCASES / sizeof RMCASES[0]; i++) {
      Case cs = RMCASES[i]; uint64_t hr, hn; char fm[400];
      int bad = run_case(&cs, &hr, &hn, fm, sizeof fm);
      printf("%-7s.%s %2d %016llx %016llx %s%s%s\n", cs.name, mn[m], cs.W, (unsigned long long)hr, (unsigned long long)hn, bad ? "MISMATCH" : "OK", bad ? " " : "", bad ? fm : "");
      if (bad) fails++;
    }
    fesetround(FE_TONEAREST);
  }
  printf("%s (%d failing cases)\n", fails ? "REFERENCE-MODEL-MISMATCH" : "REFERENCE-MODEL-VERIFIED", fails);
  return fails != 0;
}
