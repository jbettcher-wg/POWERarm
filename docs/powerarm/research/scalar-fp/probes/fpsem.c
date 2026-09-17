/* fpsem.c -- parity of scalar FP lowerings against the Arm ARM model.
 *
 *   aarch64 (Raspberry Pi 5):  ./fpsem check   -> real A64 instructions vs armref.h
 *   ppc64le (POWER9):          ./fpsem check   -> each candidate POWER sequence vs armref.h
 *   either:                    ./fpsem golden  -> reference results, one line per case
 *
 * The corpus is an edge list (signed zeros, denormals, rounding boundaries,
 * saturation edges, quiet and signalling NaNs with payloads, both signs)
 * crossed with itself, under all four FPCR rounding modes, plus a fixed-seed
 * random sample. Every candidate has a verdict line "PASS"/"FAIL n"; the
 * candidates whose names end in "_ctl" are positive controls that are
 * expected to FAIL (a raw host instruction with the wrong NaN or sign rule).
 *
 * Build: aarch64: clang -O2 -march=armv8.2-a+fp16 -o fpsem fpsem.c -lm
 *        ppc64le: clang -O2 -mcpu=power9 -DP9 -o fpsem-p9 fpsem.c -lm
 *                 clang -O2 -mcpu=power8       -o fpsem-p8 fpsem.c -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "armref.h"

/* ---------------- corpus ---------------- */
static const uint64_t D64[] = {
  0x0000000000000000ull, 0x8000000000000000ull,           /* +0 -0 */
  0x0000000000000001ull, 0x8000000000000001ull,           /* min denormal */
  0x000FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull,           /* max denormal */
  0x0010000000000000ull, 0x8010000000000000ull,           /* min normal */
  0x3FF0000000000000ull, 0xBFF0000000000000ull,           /* 1 */
  0x3FE0000000000000ull, 0xBFE0000000000000ull,           /* 0.5 */
  0x3FF8000000000000ull, 0xBFF8000000000000ull,           /* 1.5 */
  0x4004000000000000ull, 0xC004000000000000ull,           /* 2.5 */
  0x400C000000000000ull, 0xC00C000000000000ull,           /* 3.5 */
  0x3FEFFFFFFFFFFFFFull, 0xBFEFFFFFFFFFFFFFull,           /* 1 - 2^-53 */
  0x3FF0000000000001ull,                                  /* 1 + 2^-52 */
  0x4330000000000000ull, 0xC330000000000000ull,           /* 2^52 */
  0x4340000000000000ull, 0xC340000000000000ull,           /* 2^53 */
  0x41DFFFFFFFC00000ull, 0xC1E0000000000000ull,           /* 2^31-1, -2^31 */
  0x41E0000000000000ull, 0xC1E0000000200000ull,           /* 2^31, -2^31-1 */
  0x41EFFFFFFFE00000ull, 0x41F0000000000000ull,           /* 2^32-1, 2^32 */
  0x43DFFFFFFFFFFFFFull, 0x43E0000000000000ull,           /* 2^63-2^10, 2^63 */
  0xC3E0000000000000ull, 0xC3E0000000000001ull,           /* -2^63, -2^63-2^11 */
  0x43EFFFFFFFFFFFFFull, 0x43F0000000000000ull,           /* 2^64-2^11, 2^64 */
  0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull,           /* max */
  0x7FF0000000000000ull, 0xFFF0000000000000ull,           /* inf */
  0x7FF8000000000000ull, 0xFFF8000000000000ull,           /* default qNaN */
  0x7FF8000000ABCDEFull, 0xFFF8000000ABCDEFull,           /* qNaN payload */
  0x7FF0000000000001ull, 0xFFF0000000000001ull,           /* sNaN */
  0x7FF4000000123456ull, 0xFFF7FFFFFFFFFFFFull,           /* sNaN payload */
  0x3E80000000000000ull, 0x40F0000000000000ull,           /* 2^-23, 65536 */
  0x40EFFC0000000000ull, 0x40EFFE0000000000ull,           /* 65504, 65520 (half boundary) */
  0x3F00000000000000ull, 0x3E70000000000000ull,           /* 2^-15, 2^-24 (half denormal) */
  0x3E6FFFFFFFFFFFFFull, 0x3E60000000000000ull,           /* just below 2^-24, 2^-25 */
  0x4059000000000000ull, 0xC059000000000000ull,           /* 100 */
  0x3FB999999999999Aull,                                  /* 0.1 */
  0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull,           /* FLT_MAX, above FLT_MAX (rounds) */
  0x380FFFFFFFFFFFFFull, 0x36A0000000000000ull,           /* below FLT_MIN, FLT denormal min */
  0x3690000000000000ull,                                  /* half of the FLT denormal min */
};
static const uint32_t F32[] = {
  0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,
  0x00800000u, 0x80800000u, 0x3F800000u, 0xBF800000u, 0x3F000000u, 0xBF000000u,
  0x3FC00000u, 0xBFC00000u, 0x40200000u, 0xC0200000u, 0x40600000u, 0xC0600000u,
  0x3F7FFFFFu, 0x3F800001u, 0x4B000000u, 0xCB000000u, 0x4B800000u,
  0x4EFFFFFFu, 0x4F000000u, 0xCF000000u, 0xCF000001u, 0x4F7FFFFFu, 0x4F800000u,
  0x5EFFFFFFu, 0x5F000000u, 0xDF000000u, 0xDF000001u, 0x5F7FFFFFu, 0x5F800000u,
  0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u,
  0x7FC00000u, 0xFFC00000u, 0x7FC12345u, 0xFFC12345u, 0x7F800001u, 0xFF800001u,
  0x7FA12345u, 0xFFBFFFFFu, 0x42C80000u, 0xC2C80000u, 0x3DCCCCCDu, 0x477FE000u,
  0x477FF000u, 0x33800000u, 0x33000000u, 0x38000000u, 0x387FFFFFu,
};
#define ND ((int)(sizeof D64 / sizeof D64[0]))
#define NF ((int)(sizeof F32 / sizeof F32[0]))

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
/* random doubles biased to interesting exponents */
static uint64_t rnd64(void) {
  uint64_t r = rnd();
  switch (r & 7) {
  case 0: return r;
  case 1: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(1023 + (int)(r >> 56) % 70 - 35) << 52);
  case 2: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(1023 + 62 + (int)(r >> 56) % 5 - 2) << 52);
  case 3: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(1023 + 30 + (int)(r >> 56) % 5 - 2) << 52);
  case 4: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)((r >> 56) % 3) << 52);          /* tiny */
  case 5: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(1023 + (int)(r >> 56) % 20 - 10) << 52) | 0x8000000000000ull;
  default: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(1023 - 30 + (int)(r >> 56) % 8) << 52);
  }
}
static uint32_t rnd32(void) {
  uint64_t r = rnd();
  switch (r & 3) {
  case 0: return (uint32_t)r;
  case 1: return ((uint32_t)r & 0x807FFFFFu) | ((uint32_t)(127 + (int)(r >> 40) % 70 - 35) << 23);
  case 2: return ((uint32_t)r & 0x807FFFFFu) | ((uint32_t)(127 + 30 + (int)(r >> 40) % 5 - 2) << 23);
  default: return ((uint32_t)r & 0x807FFFFFu) | ((uint32_t)((r >> 40) % 3) << 23);
  }
}
#define NRAND 4000

/* ---------------- result bookkeeping ---------------- */
static int total_fail = 0, golden = 0;
typedef struct { const char* name; long n, bad; int ctl; char first[6][200]; } stat_t;
static stat_t stats[400]; static int nstats;
static stat_t* st(const char* name, int ctl) {
  for (int i = 0; i < nstats; i++) if (!strcmp(stats[i].name, name)) return &stats[i];
  stats[nstats].name = name; stats[nstats].ctl = ctl; return &stats[nstats++];
}
static void rec(stat_t* s, int ok, const char* fmt, ...) {
  s->n++;
  if (!ok) {
    if (s->bad < 6) { va_list ap; va_start(ap, fmt); vsnprintf(s->first[s->bad], 200, fmt, ap); va_end(ap); }
    s->bad++;
  }
}
#include <stdarg.h>
static void report(void) {
  for (int i = 0; i < nstats; i++) {
    stat_t* s = &stats[i];
    int expect_fail = s->ctl;
    const char* verdict = s->bad ? (expect_fail ? "FAIL(expected, control)" : "FAIL") : (expect_fail ? "PASS(control did not fire!)" : "PASS");
    printf("%-34s n=%-8ld bad=%-7ld %s\n", s->name, s->n, s->bad, verdict);
    for (int k = 0; k < 6 && k < s->bad; k++) printf("    %s\n", s->first[k]);
    if ((s->bad && !expect_fail) || (!s->bad && expect_fail)) total_fail++;
  }
}

/* ---------------- machine-specific implementations ---------------- */
#if defined(__aarch64__)
#define HW_NAME "a64"
/* FPCR.RMode field: 0 RN, 1 RP, 2 RM, 3 RZ (same numbering as RM_*) */
static void set_rmode(int rm) {
  uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
  fpcr = (fpcr & ~(3ull << 22)) | ((uint64_t)rm << 22);
  __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
}
#define A64BIN(name, insn, T, C) \
  static T name(T a, T b) { T r; __asm__ volatile(insn " %" C "0, %" C "1, %" C "2" : "=w"(r) : "w"(a), "w"(b)); return r; }
A64BIN(a64_fadd_d, "fadd", double, "d") A64BIN(a64_fsub_d, "fsub", double, "d")
A64BIN(a64_fmul_d, "fmul", double, "d") A64BIN(a64_fdiv_d, "fdiv", double, "d")
A64BIN(a64_fmax_d, "fmax", double, "d") A64BIN(a64_fmin_d, "fmin", double, "d")
A64BIN(a64_fmaxnm_d, "fmaxnm", double, "d") A64BIN(a64_fminnm_d, "fminnm", double, "d")
A64BIN(a64_fadd_s, "fadd", float, "s") A64BIN(a64_fsub_s, "fsub", float, "s")
A64BIN(a64_fmul_s, "fmul", float, "s") A64BIN(a64_fdiv_s, "fdiv", float, "s")
A64BIN(a64_fmax_s, "fmax", float, "s") A64BIN(a64_fmin_s, "fmin", float, "s")
A64BIN(a64_fmaxnm_s, "fmaxnm", float, "s") A64BIN(a64_fminnm_s, "fminnm", float, "s")
#define A64FMA(name, insn, T, C) \
  static T name(T n, T m, T a) { T r; __asm__ volatile(insn " %" C "0, %" C "1, %" C "2, %" C "3" : "=w"(r) : "w"(n), "w"(m), "w"(a)); return r; }
A64FMA(a64_fmadd_d, "fmadd", double, "d") A64FMA(a64_fmsub_d, "fmsub", double, "d")
A64FMA(a64_fnmadd_d, "fnmadd", double, "d") A64FMA(a64_fnmsub_d, "fnmsub", double, "d")
A64FMA(a64_fmadd_s, "fmadd", float, "s") A64FMA(a64_fmsub_s, "fmsub", float, "s")
A64FMA(a64_fnmadd_s, "fnmadd", float, "s") A64FMA(a64_fnmsub_s, "fnmsub", float, "s")
static double a64_fsqrt_d(double a) { double r; __asm__ volatile("fsqrt %d0, %d1" : "=w"(r) : "w"(a)); return r; }
static float a64_fsqrt_s(float a) { float r; __asm__ volatile("fsqrt %s0, %s1" : "=w"(r) : "w"(a)); return r; }
static unsigned a64_fcmp_d(double a, double b) { uint64_t n; __asm__ volatile("fcmp %d1, %d2\n\tmrs %0, nzcv" : "=r"(n) : "w"(a), "w"(b) : "cc"); return (n >> 28) & 0xF; }
static unsigned a64_fcmp_s(float a, float b) { uint64_t n; __asm__ volatile("fcmp %s1, %s2\n\tmrs %0, nzcv" : "=r"(n) : "w"(a), "w"(b) : "cc"); return (n >> 28) & 0xF; }
#define A64CVT(name, insn, TS, C) static uint64_t name(TS a) { uint64_t r; __asm__ volatile(insn " %0, %" C "1" : "=r"(r) : "w"(a)); return r; }
#define A64CVTW(name, insn, TS, C) static uint64_t name(TS a) { uint32_t r; __asm__ volatile(insn " %w0, %" C "1" : "=r"(r) : "w"(a)); return r; }
A64CVT(a64_fcvtzs_xd, "fcvtzs", double, "d") A64CVT(a64_fcvtzu_xd, "fcvtzu", double, "d")
A64CVT(a64_fcvtns_xd, "fcvtns", double, "d") A64CVT(a64_fcvtnu_xd, "fcvtnu", double, "d")
A64CVT(a64_fcvtps_xd, "fcvtps", double, "d") A64CVT(a64_fcvtpu_xd, "fcvtpu", double, "d")
A64CVT(a64_fcvtms_xd, "fcvtms", double, "d") A64CVT(a64_fcvtmu_xd, "fcvtmu", double, "d")
A64CVT(a64_fcvtas_xd, "fcvtas", double, "d") A64CVT(a64_fcvtau_xd, "fcvtau", double, "d")
A64CVTW(a64_fcvtzs_wd, "fcvtzs", double, "d") A64CVTW(a64_fcvtzu_wd, "fcvtzu", double, "d")
A64CVTW(a64_fcvtns_wd, "fcvtns", double, "d") A64CVTW(a64_fcvtnu_wd, "fcvtnu", double, "d")
A64CVTW(a64_fcvtps_wd, "fcvtps", double, "d") A64CVTW(a64_fcvtpu_wd, "fcvtpu", double, "d")
A64CVTW(a64_fcvtms_wd, "fcvtms", double, "d") A64CVTW(a64_fcvtmu_wd, "fcvtmu", double, "d")
A64CVTW(a64_fcvtas_wd, "fcvtas", double, "d") A64CVTW(a64_fcvtau_wd, "fcvtau", double, "d")
A64CVT(a64_fcvtzs_xs, "fcvtzs", float, "s") A64CVT(a64_fcvtzu_xs, "fcvtzu", float, "s")
A64CVT(a64_fcvtns_xs, "fcvtns", float, "s") A64CVT(a64_fcvtas_xs, "fcvtas", float, "s")
A64CVTW(a64_fcvtzs_ws, "fcvtzs", float, "s") A64CVTW(a64_fcvtzu_ws, "fcvtzu", float, "s")
A64CVTW(a64_fcvtns_ws, "fcvtns", float, "s") A64CVTW(a64_fcvtau_ws, "fcvtau", float, "s")
static uint64_t a64_fcvtzs_xd_fix(double a, int fb) { uint64_t r;
  switch (fb) { case 1: __asm__ volatile("fcvtzs %0, %d1, #1" : "=r"(r) : "w"(a)); break;
  case 32: __asm__ volatile("fcvtzs %0, %d1, #32" : "=r"(r) : "w"(a)); break;
  default: __asm__ volatile("fcvtzs %0, %d1, #63" : "=r"(r) : "w"(a)); break; } return r; }
static uint64_t a64_fcvtzu_xd_fix(double a, int fb) { uint64_t r;
  switch (fb) { case 1: __asm__ volatile("fcvtzu %0, %d1, #1" : "=r"(r) : "w"(a)); break;
  case 32: __asm__ volatile("fcvtzu %0, %d1, #32" : "=r"(r) : "w"(a)); break;
  default: __asm__ volatile("fcvtzu %0, %d1, #64" : "=r"(r) : "w"(a)); break; } return r; }
static double a64_scvtf_dx(int64_t i) { double r; __asm__ volatile("scvtf %d0, %1" : "=w"(r) : "r"(i)); return r; }
static double a64_ucvtf_dx(uint64_t i) { double r; __asm__ volatile("ucvtf %d0, %1" : "=w"(r) : "r"(i)); return r; }
static float a64_scvtf_sx(int64_t i) { float r; __asm__ volatile("scvtf %s0, %1" : "=w"(r) : "r"(i)); return r; }
static float a64_ucvtf_sx(uint64_t i) { float r; __asm__ volatile("ucvtf %s0, %1" : "=w"(r) : "r"(i)); return r; }
static double a64_scvtf_dw(int32_t i) { double r; __asm__ volatile("scvtf %d0, %w1" : "=w"(r) : "r"(i)); return r; }
static double a64_ucvtf_dw(uint32_t i) { double r; __asm__ volatile("ucvtf %d0, %w1" : "=w"(r) : "r"(i)); return r; }
static float a64_scvtf_sw(int32_t i) { float r; __asm__ volatile("scvtf %s0, %w1" : "=w"(r) : "r"(i)); return r; }
static float a64_fcvt_sd(double a) { float r; __asm__ volatile("fcvt %s0, %d1" : "=w"(r) : "w"(a)); return r; }
static double a64_fcvt_ds(float a) { double r; __asm__ volatile("fcvt %d0, %s1" : "=w"(r) : "w"(a)); return r; }
static uint16_t a64_fcvt_hd(double a) { uint32_t r; __asm__ volatile("fcvt h0, %d1\n\tfmov %w0, s0" : "=r"(r) : "w"(a) : "v0"); return (uint16_t)r; }
static double a64_fcvt_dh(uint16_t h) { double r; uint32_t w = h; __asm__ volatile("fmov s0, %w1\n\tfcvt %d0, h0" : "=w"(r) : "r"(w) : "v0"); return r; }
static uint16_t a64_fcvt_hs(float a) { uint32_t r; __asm__ volatile("fcvt h0, %s1\n\tfmov %w0, s0" : "=r"(r) : "w"(a) : "v0"); return (uint16_t)r; }
static float a64_fcvt_sh(uint16_t h) { float r; uint32_t w = h; __asm__ volatile("fmov s0, %w1\n\tfcvt %s0, h0" : "=w"(r) : "r"(w) : "v0"); return r; }

#elif defined(__powerpc64__)
#define HW_NAME "ppc"
/* FPSCR.RN: 0 RN, 1 RZ, 2 RP, 3 RM. Map the FPCR numbering onto it, which
 * is exactly what the JIT's SetRoundingMode does for MSR FPCR. */
static void set_rmode(int rm) {
  static const int map[4] = { 0, 2, 3, 1 };
  set_c_rounding(rm);
  /* belt and braces: verify FPSCR.RN */
  double f; __asm__ volatile("mffs %0" : "=d"(f));
  if ((d2u(f) & 3) != (uint64_t)map[rm]) { fprintf(stderr, "FPSCR.RN mismatch\n"); exit(2); }
}
typedef double v2d __attribute__((vector_size(16)));
typedef float v4f __attribute__((vector_size(16)));
typedef uint64_t v2u __attribute__((vector_size(16)));
typedef uint32_t v4u __attribute__((vector_size(16)));
/* Guest register image: element 0 is VSX doubleword 1 (the low LE half).
 * For f32 element 0 is the low LE word, which is VSX word 3. */
static inline v2d vd(double a) { v2u v = { d2u(a), 0x40C81C8000000000ull }; return (v2d)v; }          /* dw1 = a, dw0 = garbage */
/* Built from the integer bits so that the compiler never converts the
 * float through an FPR (that path can quiet a signalling NaN with a
 * signalling xscvdpsp before the candidate sees it). */
static inline v4f vf(float a) { v4u v = { f2u(a), 0x3FC00000u, 0xC0000000u, 0x429A0000u }; return (v4f)v; } /* w3 = a */
static inline float lane0(v4f r) { return u2f(((v4u)r)[0]); }
/* Scalar-form candidates take the value in doubleword 0 (that is what
 * xs* instructions read), i.e. after the JIT's positioning xxpermdi. */

/* -- binary ops: f64 -- */
#define PPC_XS_RAW(name, insn) \
  static double name(double a, double b) { double r; __asm__ volatile(insn " %x0, %x1, %x2" : "=wa"(r) : "wa"(a), "wa"(b)); return r; }
PPC_XS_RAW(p_xsadddp, "xsadddp") PPC_XS_RAW(p_xssubdp, "xssubdp") PPC_XS_RAW(p_xsmuldp, "xsmuldp") PPC_XS_RAW(p_xsdivdp, "xsdivdp")
PPC_XS_RAW(p_xsmaxdp, "xsmaxdp") PPC_XS_RAW(p_xsmindp, "xsmindp")
#ifdef P9
PPC_XS_RAW(p_xsmaxjdp, "xsmaxjdp") PPC_XS_RAW(p_xsminjdp, "xsminjdp")
PPC_XS_RAW(p_xsmaxcdp, "xsmaxcdp") PPC_XS_RAW(p_xsmincdp, "xsmincdp")
#endif
/* Recommended lowering: pre-check with the unordered compare of the two
 * operands into CR1, fall through on the ordered path. The cold path swaps
 * the operands when operand 1 is a quiet NaN and operand 2 a signalling
 * NaN, which is the only case where the POWER order (operand 1 first)
 * differs from the A64 order (first signalling NaN, else first quiet NaN).
 * GPR tests: (x<<1)>>52 == 0xFFF  <=> x is a quiet NaN;
 *            (x<<1)>>52 == 0xFFE and low 51 bits != 0 <=> signalling NaN. */
#define PPC_XS_FIX(name, insn) \
  static double name(double a, double b) { double r; uint64_t t1, t2, t3; \
    __asm__ volatile( \
      "xscmpudp 1, %x1, %x2\n\t" \
      "bso 1, 1f\n\t" \
      insn " %x0, %x1, %x2\n\t" \
      "b 3f\n" \
      "1:\n\t" \
      "mfvsrd %3, %x1\n\t" \
      "mfvsrd %4, %x2\n\t" \
      "rldicl %3, %3, 13, 52\n\t" \
      "cmpldi 1, %3, 0xFFF\n\t" \
      "bne 1, 2f\n\t" \
      "rldicl %3, %4, 13, 52\n\t" \
      "cmpldi 1, %3, 0xFFE\n\t" \
      "bne 1, 2f\n\t" \
      "clrldi %5, %4, 13\n\t" \
      "cmpldi 1, %5, 0\n\t" \
      "beq 1, 2f\n\t" \
      insn " %x0, %x2, %x1\n\t" \
      "b 3f\n" \
      "2:\n\t" \
      insn " %x0, %x1, %x2\n" \
      "3:\n\t" \
      : "=&wa"(r), "+wa"(a), "+wa"(b), "=&r"(t1), "=&r"(t2), "=&r"(t3) :: "cr1"); \
    return r; }
PPC_XS_FIX(p_fix_xsadddp, "xsadddp") PPC_XS_FIX(p_fix_xssubdp, "xssubdp")
PPC_XS_FIX(p_fix_xsmuldp, "xsmuldp") PPC_XS_FIX(p_fix_xsdivdp, "xsdivdp")
/* Checked branch-free form: mask built with vector compares, one xxsel, then the op. */
static double p_bfsel_op(double a, double b, int op) {
  v2d va = { a, a }, vb = { b, b }, r, sel;
  v2u q = { 1ull << 51, 1ull << 51 }, z = { 0, 0 }, oa, ob, qa, qb, m;
  __asm__ volatile("xvcmpeqdp %x0, %x1, %x1" : "=wa"(oa) : "wa"(va));   /* ordered(a) */
  __asm__ volatile("xvcmpeqdp %x0, %x1, %x1" : "=wa"(ob) : "wa"(vb));   /* ordered(b) */
  __asm__ volatile("xxland %x0, %x1, %x2" : "=wa"(qa) : "wa"(va), "wa"(q));
  __asm__ volatile("xxland %x0, %x1, %x2" : "=wa"(qb) : "wa"(vb), "wa"(q));
  __asm__ volatile("vcmpequd %0, %1, %2" : "=v"(qa) : "v"(qa), "v"(z)); /* a quiet bit clear */
  __asm__ volatile("vcmpequd %0, %1, %2" : "=v"(qb) : "v"(qb), "v"(z)); /* b quiet bit clear */
  __asm__ volatile("xxlor %x0, %x1, %x2" : "=wa"(m) : "wa"(oa), "wa"(ob));
  __asm__ volatile("xxlor %x0, %x1, %x2" : "=wa"(m) : "wa"(m), "wa"(qa));
  __asm__ volatile("xxlandc %x0, %x1, %x2" : "=wa"(m) : "wa"(qb), "wa"(m)); /* mask = qNaN(a) & sNaN(b) */
  __asm__ volatile("xxsel %x0, %x1, %x2, %x3" : "=wa"(sel) : "wa"(va), "wa"(vb), "wa"(m));
  switch (op) {
  case 0: __asm__ volatile("xvadddp %x0, %x1, %x2" : "=wa"(r) : "wa"(sel), "wa"(vb)); break;
  case 1: __asm__ volatile("xvsubdp %x0, %x1, %x2" : "=wa"(r) : "wa"(sel), "wa"(vb)); break;
  case 2: __asm__ volatile("xvmuldp %x0, %x1, %x2" : "=wa"(r) : "wa"(sel), "wa"(vb)); break;
  default: __asm__ volatile("xvdivdp %x0, %x1, %x2" : "=wa"(r) : "wa"(sel), "wa"(vb)); break;
  }
  return r[0];
}
/* -- binary ops: f32 in SP format, vector lane op -- */
#define PPC_XV_RAW32(name, insn) \
  static float name(float a, float b) { v4f va = vf(a), vb = vf(b), r; __asm__ volatile(insn " %x0, %x1, %x2" : "=wa"(r) : "wa"(va), "wa"(vb)); return lane0(r); }
PPC_XV_RAW32(p_xvaddsp, "xvaddsp") PPC_XV_RAW32(p_xvsubsp, "xvsubsp") PPC_XV_RAW32(p_xvmulsp, "xvmulsp") PPC_XV_RAW32(p_xvdivsp, "xvdivsp")
PPC_XV_RAW32(p_xvmaxsp, "xvmaxsp") PPC_XV_RAW32(p_xvminsp, "xvminsp")
/* f32 pre-check: widen both with xscvspdpn (bit-exact, keeps sNaN), xscmpudp, bso. */
#define PPC_XV_FIX32(name, insn) \
  static float name(float a, float b) { v4f va = vf(a), vb = vf(b), r; uint64_t t1, t2, t3; \
    __asm__ volatile( \
      "xxsldwi 32, %x1, %x1, 3\n\t"   /* word 0 <- element 0 (LE word 0 = BE word 3) */ \
      "xxsldwi 33, %x2, %x2, 3\n\t" \
      "xscvspdpn 32, 32\n\t" \
      "xscvspdpn 33, 33\n\t" \
      "xscmpudp 1, 32, 33\n\t" \
      "bso 1, 1f\n\t" \
      insn " %x0, %x1, %x2\n\t" \
      "b 3f\n" \
      "1:\n\t" \
      "mfvsrd %3, 32\n\t" \
      "mfvsrd %4, 33\n\t" \
      "rldicl %3, %3, 13, 52\n\t" \
      "cmpldi 1, %3, 0xFFF\n\t" \
      "bne 1, 2f\n\t" \
      "rldicl %3, %4, 13, 52\n\t" \
      "cmpldi 1, %3, 0xFFE\n\t" \
      "bne 1, 2f\n\t" \
      "clrldi %5, %4, 13\n\t" \
      "cmpldi 1, %5, 0\n\t" \
      "beq 1, 2f\n\t" \
      insn " %x0, %x2, %x1\n\t" \
      "b 3f\n" \
      "2:\n\t" \
      insn " %x0, %x1, %x2\n" \
      "3:\n\t" \
      : "=&wa"(r), "+wa"(va), "+wa"(vb), "=&r"(t1), "=&r"(t2), "=&r"(t3) :: "cr1", "v0", "v1"); \
    return lane0(r); }
PPC_XV_FIX32(p_fix_xvaddsp, "xvaddsp") PPC_XV_FIX32(p_fix_xvsubsp, "xvsubsp")
PPC_XV_FIX32(p_fix_xvmulsp, "xvmulsp") PPC_XV_FIX32(p_fix_xvdivsp, "xvdivsp")

/* -- min/max f64 with the NaN pre-check: any NaN -> cold path computing
 * the A64 propagation (same swap rule, then xsadddp of the NaN with
 * itself to quiet it, which is what FPProcessNaN does). -- */
#define PPC_MINMAX_FIX(name, insn, isnum) \
  static double name(double a, double b) { double r; uint64_t t1, t2, t3; \
    __asm__ volatile( \
      "xscmpudp 1, %x1, %x2\n\t" \
      "bso 1, 1f\n\t" \
      insn " %x0, %x1, %x2\n\t" \
      "b 3f\n" \
      "1:\n\t" \
      ".if " #isnum "\n\t" \
      /* FMAXNM/FMINNM: a lone quiet NaN loses; the host op already does that. */ \
      /* Only the (a quiet, b signalling), (a signalling, b anything NaN) and (both quiet) orders need care: */ \
      ".endif\n\t" \
      "mfvsrd %3, %x1\n\t" \
      "mfvsrd %4, %x2\n\t" \
      "rldicl %5, %3, 13, 52\n\t" \
      "cmpldi 1, %5, 0xFFE\n\t"      /* a signalling (or Inf) ? */ \
      "bne 1, 5f\n\t" \
      "clrldi %5, %3, 13\n\t" \
      "cmpldi 1, %5, 0\n\t" \
      "beq 1, 5f\n\t" \
      "xsadddp %x0, %x1, %x1\n\t"    /* a is sNaN: result = quiet(a) */ \
      "b 3f\n" \
      "5:\n\t" \
      "rldicl %5, %4, 13, 52\n\t" \
      "cmpldi 1, %5, 0xFFE\n\t"      /* b signalling? */ \
      "bne 1, 6f\n\t" \
      "clrldi %5, %4, 13\n\t" \
      "cmpldi 1, %5, 0\n\t" \
      "beq 1, 6f\n\t" \
      "xsadddp %x0, %x2, %x2\n\t"    /* b is sNaN: result = quiet(b) */ \
      "b 3f\n" \
      "6:\n\t" \
      /* only quiet NaNs remain */ \
      ".if " #isnum "\n\t" \
      "rldicl %5, %3, 13, 52\n\t" \
      "cmpldi 1, %5, 0xFFF\n\t" \
      "bne 1, 7f\n\t"                /* a not NaN -> b is the lone qNaN -> result a */ \
      "rldicl %5, %4, 13, 52\n\t" \
      "cmpldi 1, %5, 0xFFF\n\t" \
      "bne 1, 8f\n\t"                /* b not NaN -> result b */ \
      "xxlor %x0, %x1, %x1\n\t"      /* both quiet: first (a) */ \
      "b 3f\n" \
      "7:\n\t" \
      "xxlor %x0, %x1, %x1\n\t" \
      "b 3f\n" \
      "8:\n\t" \
      "xxlor %x0, %x2, %x2\n\t" \
      "b 3f\n" \
      ".else\n\t" \
      "rldicl %5, %3, 13, 52\n\t" \
      "cmpldi 1, %5, 0xFFF\n\t" \
      "bne 1, 8f\n\t"                /* a not NaN -> result = b (a qNaN) */ \
      "xxlor %x0, %x1, %x1\n\t" \
      "b 3f\n" \
      "8:\n\t" \
      "xxlor %x0, %x2, %x2\n\t" \
      "b 3f\n" \
      ".endif\n" \
      "3:\n\t" \
      : "=&wa"(r), "+wa"(a), "+wa"(b), "=&r"(t1), "=&r"(t2), "=&r"(t3) :: "cr1"); \
    return r; }
PPC_MINMAX_FIX(p_fix_xsmaxdp, "xsmaxdp", 0) PPC_MINMAX_FIX(p_fix_xsmindp, "xsmindp", 0)
PPC_MINMAX_FIX(p_fixnm_xsmaxdp, "xsmaxdp", 1) PPC_MINMAX_FIX(p_fixnm_xsmindp, "xsmindp", 1)

/* -- FMA f64: a-form. FMADD a+n*m -> xsmaddadp(T=a,A=n,B=m).
 * FNMSUB n*m-a -> xsmsubadp. FMSUB a-n*m -> negate n, xsmaddadp.
 * FNMADD -a-n*m -> negate n, xsmsubadp. Controls: xsnmsubadp / xsnmaddadp. */
static double p_xsmaddadp(double n, double m, double a) { __asm__ volatile("xsmaddadp %x0, %x1, %x2" : "+wa"(a) : "wa"(n), "wa"(m)); return a; }
static double p_xsmsubadp(double n, double m, double a) { __asm__ volatile("xsmsubadp %x0, %x1, %x2" : "+wa"(a) : "wa"(n), "wa"(m)); return a; }
static double p_xsnmaddadp(double n, double m, double a) { __asm__ volatile("xsnmaddadp %x0, %x1, %x2" : "+wa"(a) : "wa"(n), "wa"(m)); return a; }
static double p_xsnmsubadp(double n, double m, double a) { __asm__ volatile("xsnmsubadp %x0, %x1, %x2" : "+wa"(a) : "wa"(n), "wa"(m)); return a; }
static double p_neg(double x) { __asm__ volatile("xsnegdp %x0, %x0" : "+wa"(x)); return x; }
static double p_fma_fmsub(double n, double m, double a) { return p_xsmaddadp(p_neg(n), m, a); }
static double p_fma_fnmadd(double n, double m, double a) { return p_xsmsubadp(p_neg(n), m, a); }
/* FMA with the post-check on the result (NaN result -> cold path that
 * applies FPProcessNaNs3 in the A64 order on the ORIGINAL operands). The
 * cold path is done in C here; in the JIT it is an out-of-line block. */
static double p_fma_post(double n, double m, double a, int variant) {
  double r; uint64_t n0 = d2u(n), m0 = d2u(m), a0 = d2u(a);
  switch (variant) { case 0: r = p_xsmaddadp(n, m, a); break; case 1: r = p_xsmsubadp(n, m, a); break;
    case 2: r = p_fma_fmsub(n, m, a); break; default: r = p_fma_fnmadd(n, m, a); break; }
  int so; __asm__ volatile("xscmpudp 1, %x1, %x1\n\tmfocrf %0, 0x40\n\trlwinm %0, %0, 8, 31, 31" : "=r"(so) : "wa"(r) : "cr1");
  if (!so) return r;
  /* cold: the A64 rule on the negated operands (FPNeg happens before FPMulAdd) */
  uint64_t an = a0, nn = n0;
  if (variant == 2) nn ^= 1ull << 63;                  /* FMSUB: a + (-n)*m */
  if (variant == 3) { nn ^= 1ull << 63; an ^= 1ull << 63; } /* FNMADD: (-a) + (-n)*m */
  if (variant == 1) an ^= 1ull << 63;                  /* FNMSUB: (-a) + n*m */
  return u2d(arm_muladd64(an, nn, m0));
}
static float p_xvmaddasp(float n, float m, float a) { v4f vn = vf(n), vm = vf(m), va = vf(a); __asm__ volatile("xvmaddasp %x0, %x1, %x2" : "+wa"(va) : "wa"(vn), "wa"(vm)); return lane0(va); }
static float p_xvmsubasp(float n, float m, float a) { v4f vn = vf(n), vm = vf(m), va = vf(a); __asm__ volatile("xvmsubasp %x0, %x1, %x2" : "+wa"(va) : "wa"(vn), "wa"(vm)); return lane0(va); }
static float p_xvnmaddasp(float n, float m, float a) { v4f vn = vf(n), vm = vf(m), va = vf(a); __asm__ volatile("xvnmaddasp %x0, %x1, %x2" : "+wa"(va) : "wa"(vn), "wa"(vm)); return lane0(va); }
static float p_xvnmsubasp(float n, float m, float a) { v4f vn = vf(n), vm = vf(m), va = vf(a); __asm__ volatile("xvnmsubasp %x0, %x1, %x2" : "+wa"(va) : "wa"(vn), "wa"(vm)); return lane0(va); }
static float p_negf(float x) { v4f v = vf(x); __asm__ volatile("xvnegsp %x0, %x0" : "+wa"(v)); return lane0(v); }

/* -- sqrt -- */
static double p_xssqrtdp(double a) { double r; __asm__ volatile("xssqrtdp %x0, %x1" : "=wa"(r) : "wa"(a)); return r; }
static double p_xvsqrtdp(double a) { v2d v = vd(a), r; __asm__ volatile("xvsqrtdp %x0, %x1" : "=wa"(r) : "wa"(v)); return u2d(((v2u)r)[0]); }
static float p_xvsqrtsp(float a) { v4f v = vf(a), r; __asm__ volatile("xvsqrtsp %x0, %x1" : "=wa"(r) : "wa"(v)); return lane0(r); }
/* -- fcmp -> NZCV via CR bits: N=LT Z=EQ C=!LT V=SO -- */
static unsigned p_fcmp_d(double a, double b) { uint64_t cr; __asm__ volatile("xscmpudp 1, %x1, %x2\n\tmfocrf %0, 0x40\n\trlwinm %0, %0, 8, 28, 31" : "=r"(cr) : "wa"(a), "wa"(b) : "cr1");
  unsigned lt = (cr >> 3) & 1, eq = (cr >> 1) & 1, so = cr & 1; return (lt << 3) | (eq << 2) | ((lt ^ 1) << 1) | so; }
static unsigned p_fcmpu_d(double a, double b) { uint64_t cr; __asm__ volatile("fcmpu 1, %1, %2\n\tmfocrf %0, 0x40\n\trlwinm %0, %0, 8, 28, 31" : "=r"(cr) : "d"(a), "d"(b) : "cr1");
  unsigned lt = (cr >> 3) & 1, eq = (cr >> 1) & 1, so = cr & 1; return (lt << 3) | (eq << 2) | ((lt ^ 1) << 1) | so; }
static unsigned p_fcmp_s(float a, float b) { double da, db; v4f va = vf(a), vb = vf(b);
  __asm__ volatile("xxsldwi %x0, %x1, %x1, 3\n\txscvspdp %x0, %x0" : "=wa"(da) : "wa"(va));
  __asm__ volatile("xxsldwi %x0, %x1, %x1, 3\n\txscvspdp %x0, %x0" : "=wa"(db) : "wa"(vb));
  return p_fcmp_d(da, db); }

/* -- conversions to integer. The value is positioned in doubleword 0. -- */
static double p_round(double a, int rounding, int variant) {
  /* rounding: RM_* (TIEAWAY = xsrdpi; ZERO = none; POSINF/NEGINF directed;
   * TIEEVEN via a sandwich: variant 0 = mffs/mtfsb0/mtfsf (POWER8),
   * variant 1 = mffscrni/mffscrn (POWER9), variant 2 = assume RN==nearest, plain xsrdpic) */
  switch (rounding) {
  case RM_TIEAWAY: __asm__ volatile("xsrdpi %x0, %x0" : "+wa"(a)); break;
  case RM_POSINF:  __asm__ volatile("xsrdpip %x0, %x0" : "+wa"(a)); break;
  case RM_NEGINF:  __asm__ volatile("xsrdpim %x0, %x0" : "+wa"(a)); break;
  case RM_ZERO:    break;
  default:
    if (variant == 0) { double s; __asm__ volatile("mffs %0\n\tmtfsb0 30\n\tmtfsb0 31\n\txsrdpic %x1, %x1\n\tmtfsf 1, %0" : "=&d"(s), "+wa"(a)); }
#ifdef P9
    else if (variant == 1) { double s; __asm__ volatile("mffscrni %0, 0\n\txsrdpic %x1, %x1\n\tmffscrn %0, %0" : "=&d"(s), "+wa"(a)); }
#endif
    else __asm__ volatile("xsrdpic %x0, %x0" : "+wa"(a));
    break;
  }
  return a;
}
static uint64_t p_cvt_sx64_branch(double a) { uint64_t r; __asm__ volatile(
  "xscmpudp 1, %x1, %x1\n\txscvdpsxds %x1, %x1\n\tmfvsrd %0, %x1\n\tbns 1, 1f\n\tli %0, 0\n1:\n\t" : "=&r"(r), "+wa"(a) :: "cr1"); return r; }
static uint64_t p_cvt_sx64_isel(double a) { uint64_t r; __asm__ volatile(
  "xscmpudp 1, %x1, %x1\n\txscvdpsxds %x1, %x1\n\tmfvsrd %0, %x1\n\tisel %0, 0, %0, 7\n\t" : "=&r"(r), "+wa"(a) :: "cr1"); return r; }
static uint64_t p_cvt_sx64_mask(double a) { uint64_t r; double t; __asm__ volatile(
  "xvcmpeqdp %x2, %x1, %x1\n\txscvdpsxds %x1, %x1\n\txxland %x1, %x1, %x2\n\tmfvsrd %0, %x1\n\t" : "=&r"(r), "+wa"(a), "=&wa"(t)); return r; }
static uint64_t p_cvt_sx64_fctidz(double a) { uint64_t r; __asm__ volatile(
  "fcmpu 1, %1, %1\n\tfctidz %1, %1\n\tmfvsrd %0, %x1\n\tisel %0, 0, %0, 7\n\t" : "=&r"(r), "+d"(a) :: "cr1"); return r; }
static uint64_t p_cvt_sx64_ctl(double a) { uint64_t r; __asm__ volatile("xscvdpsxds %x1, %x1\n\tmfvsrd %0, %x1" : "=&r"(r), "+wa"(a)); return r; }
static uint64_t p_cvt_ux64(double a) { uint64_t r; __asm__ volatile("xscvdpuxds %x1, %x1\n\tmfvsrd %0, %x1" : "=&r"(r), "+wa"(a)); return r; }
static uint64_t p_cvt_ux64_fctiduz(double a) { uint64_t r; __asm__ volatile("fctiduz %1, %1\n\tmfvsrd %0, %x1" : "=&r"(r), "+d"(a)); return r; }
static uint64_t p_cvt_sx32_isel(double a) { uint64_t r; __asm__ volatile(
  "xscmpudp 1, %x1, %x1\n\txscvdpsxws %x1, %x1\n\tmfvsrwz %0, %x1\n\tisel %0, 0, %0, 7\n\t" : "=&r"(r), "+wa"(a) :: "cr1"); return r; }
static uint64_t p_cvt_sx32_ctl(double a) { uint64_t r; __asm__ volatile("xscvdpsxws %x1, %x1\n\tmfvsrwz %0, %x1" : "=&r"(r), "+wa"(a)); return r; }
static uint64_t p_cvt_ux32(double a) { uint64_t r; __asm__ volatile("xscvdpuxws %x1, %x1\n\tmfvsrwz %0, %x1" : "=&r"(r), "+wa"(a)); return r; }
/* fctid uses RN: valid for FCVTNS only when FPCR.RMode == nearest. */
static uint64_t p_cvt_fctid_isel(double a) { uint64_t r; __asm__ volatile(
  "fcmpu 1, %1, %1\n\tfctid %1, %1\n\tmfvsrd %0, %x1\n\tisel %0, 0, %0, 7\n\t" : "=&r"(r), "+d"(a) :: "cr1"); return r; }
static double p_widen_f32(float a) { v4f v = vf(a); double d; __asm__ volatile("xxsldwi %x0, %x1, %x1, 3\n\txscvspdpn %x0, %x0" : "=wa"(d) : "wa"(v)); return d; }

/* -- conversions from integer -- */
static double p_cvt_sxd(int64_t i) { double r; __asm__ volatile("mtvsrd %x0, %1\n\txscvsxddp %x0, %x0" : "=wa"(r) : "r"(i)); return r; }
static double p_cvt_uxd(uint64_t i) { double r; __asm__ volatile("mtvsrd %x0, %1\n\txscvuxddp %x0, %x0" : "=wa"(r) : "r"(i)); return r; }
static float p_cvt_sxs(int64_t i) { double r; float f; __asm__ volatile("mtvsrd %x0, %1\n\txscvsxdsp %x0, %x0" : "=wa"(r) : "r"(i)); f = (float)r; /* exact: already SP-valued */ return f; }
static float p_cvt_uxs(uint64_t i) { double r; float f; __asm__ volatile("mtvsrd %x0, %1\n\txscvuxdsp %x0, %x0" : "=wa"(r) : "r"(i)); f = (float)r; return f; }
static float p_cvt_sxs_fcfids(int64_t i) { double r; __asm__ volatile("mtvsrd %x0, %1\n\tfcfids %0, %0" : "=d"(r) : "r"(i)); return (float)r; }
static float p_cvt_sxs_ctl(int64_t i) { double r; __asm__ volatile("mtvsrd %x0, %1\n\tfcfid %0, %0\n\tfrsp %0, %0" : "=d"(r) : "r"(i)); return (float)r; }
static double p_cvt_swd_mtvsrwa(int32_t i) { double r; __asm__ volatile("mtvsrwa %x0, %1\n\txscvsxddp %x0, %x0" : "=wa"(r) : "r"(i)); return r; }
static double p_cvt_uwd_mtvsrwz(uint32_t i) { double r; __asm__ volatile("mtvsrwz %x0, %1\n\txscvuxddp %x0, %x0" : "=wa"(r) : "r"(i)); return r; }
static float p_cvt_sws_mtvsrwa(int32_t i) { double r; __asm__ volatile("mtvsrwa %x0, %1\n\txscvsxdsp %x0, %x0" : "=wa"(r) : "r"(i)); return (float)r; }

/* -- precision conversions -- */
static float p_xscvdpsp(double a) { v4f r; __asm__ volatile("xscvdpsp %x0, %x1" : "=wa"(r) : "wa"(a)); return u2f(((v4u)r)[3]); /* word 0 (BE) = LE element 3 */ }
static double p_xscvspdp(float a) { v4f v = vf(a); double r; __asm__ volatile("xxsldwi %x0, %x1, %x1, 3\n\txscvspdp %x0, %x0" : "=wa"(r) : "wa"(v)); return r; }
static double p_xscvspdpn_ctl(float a) { v4f v = vf(a); double r; __asm__ volatile("xxsldwi %x0, %x1, %x1, 3\n\txscvspdpn %x0, %x0" : "=wa"(r) : "wa"(v)); return r; }
#ifdef P9
static uint16_t p_xscvdphp(double a) { uint64_t r; __asm__ volatile("xscvdphp %x1, %x1\n\tmfvsrd %0, %x1" : "=r"(r), "+wa"(a)); return (uint16_t)r; }
static double p_xscvhpdp(uint16_t h) { double r; uint64_t in = h; __asm__ volatile("mtvsrd %x0, %1\n\txscvhpdp %x0, %x0" : "=wa"(r) : "r"(in)); return r; }
#endif
#endif /* arch */

/* ---------------- the checks ---------------- */
#define CHK64(sname, ctl, expr_got, expr_exp, ...) do { uint64_t g_ = (expr_got), e_ = (expr_exp); \
  rec(st(sname, ctl), g_ == e_, __VA_ARGS__); } while (0)

static void check_binop64(void) {
  static const char* nm[4] = { "fadd", "fsub", "fmul", "fdiv" };
  for (int rm = 0; rm < 4; rm++) {
    set_rmode(rm);
    for (int i = 0; i < ND + NRAND; i++) for (int j = 0; j < ND + NRAND; j++) {
      if (i >= ND && j >= ND && (i + j) % 97) continue; /* thin the random x random block */
      uint64_t a = i < ND ? D64[i] : rnd64(), b = j < ND ? D64[j] : rnd64();
      for (int op = 0; op < 4; op++) {
        uint64_t exp = arm_binop64((enum binop)op, a, b);
        char name[64];
#if defined(__aarch64__)
        double (*f)(double, double) = op == 0 ? a64_fadd_d : op == 1 ? a64_fsub_d : op == 2 ? a64_fmul_d : a64_fdiv_d;
        snprintf(name, 64, "a64.%s_d", nm[op]);
        CHK64(strdup(name), 0, d2u(f(u2d(a), u2d(b))), exp, "rm%d %016llx %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)b, (unsigned long long)exp, (unsigned long long)d2u(f(u2d(a), u2d(b))));
        if (golden) printf("%s rm%d %016llx %016llx %016llx\n", nm[op], rm, (unsigned long long)a, (unsigned long long)b, (unsigned long long)exp);
#elif defined(__powerpc64__)
        double (*raw)(double, double) = op == 0 ? p_xsadddp : op == 1 ? p_xssubdp : op == 2 ? p_xsmuldp : p_xsdivdp;
        double (*fix)(double, double) = op == 0 ? p_fix_xsadddp : op == 1 ? p_fix_xssubdp : op == 2 ? p_fix_xsmuldp : p_fix_xsdivdp;
        uint64_t g;
        snprintf(name, 64, "ppc.%s_d.xs_raw_ctl", nm[op]); g = d2u(raw(u2d(a), u2d(b)));
        CHK64(strdup(name), 1, g, exp, "rm%d %016llx %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)b, (unsigned long long)exp, (unsigned long long)g);
        snprintf(name, 64, "ppc.%s_d.precheck_branch", nm[op]); g = d2u(fix(u2d(a), u2d(b)));
        CHK64(strdup(name), 0, g, exp, "rm%d %016llx %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)b, (unsigned long long)exp, (unsigned long long)g);
        snprintf(name, 64, "ppc.%s_d.branchfree_sel", nm[op]); g = d2u(p_bfsel_op(u2d(a), u2d(b), op));
        CHK64(strdup(name), 0, g, exp, "rm%d %016llx %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)b, (unsigned long long)exp, (unsigned long long)g);
#endif
      }
    }
  }
}
static void check_binop32(void) {
  static const char* nm[4] = { "fadd", "fsub", "fmul", "fdiv" };
  for (int rm = 0; rm < 4; rm++) {
    set_rmode(rm);
    for (int i = 0; i < NF + NRAND; i++) for (int j = 0; j < NF + NRAND; j++) {
      if (i >= NF && j >= NF && (i + j) % 97) continue;
      uint32_t a = i < NF ? F32[i] : rnd32(), b = j < NF ? F32[j] : rnd32();
      for (int op = 0; op < 4; op++) {
        uint32_t exp = arm_binop32((enum binop)op, a, b);
        char name[64];
#if defined(__aarch64__)
        float (*f)(float, float) = op == 0 ? a64_fadd_s : op == 1 ? a64_fsub_s : op == 2 ? a64_fmul_s : a64_fdiv_s;
        snprintf(name, 64, "a64.%s_s", nm[op]);
        uint32_t g = f2u(f(u2f(a), u2f(b)));
        CHK64(strdup(name), 0, g, exp, "rm%d %08x %08x exp %08x got %08x", rm, a, b, exp, g);
#elif defined(__powerpc64__)
        float (*raw)(float, float) = op == 0 ? p_xvaddsp : op == 1 ? p_xvsubsp : op == 2 ? p_xvmulsp : p_xvdivsp;
        float (*fix)(float, float) = op == 0 ? p_fix_xvaddsp : op == 1 ? p_fix_xvsubsp : op == 2 ? p_fix_xvmulsp : p_fix_xvdivsp;
        uint32_t g;
        snprintf(name, 64, "ppc.%s_s.xv_raw_ctl", nm[op]); g = f2u(raw(u2f(a), u2f(b)));
        CHK64(strdup(name), 1, g, exp, "rm%d %08x %08x exp %08x got %08x", rm, a, b, exp, g);
        snprintf(name, 64, "ppc.%s_s.precheck_branch", nm[op]); g = f2u(fix(u2f(a), u2f(b)));
        CHK64(strdup(name), 0, g, exp, "rm%d %08x %08x exp %08x got %08x", rm, a, b, exp, g);
#endif
      }
    }
  }
}
static void check_minmax(void) {
  set_rmode(0);
  for (int i = 0; i < ND + 1000; i++) for (int j = 0; j < ND + 1000; j++) {
    if (i >= ND && j >= ND && (i + j) % 31) continue;
    uint64_t a = i < ND ? D64[i] : rnd64(), b = j < ND ? D64[j] : rnd64();
    for (int k = 0; k < 4; k++) {
      int ismax = k & 1, isnum = k >> 1;
      const char* nm = k == 0 ? "fmin" : k == 1 ? "fmax" : k == 2 ? "fminnm" : "fmaxnm";
      uint64_t exp = arm_minmax64(a, b, ismax, isnum), g; char name[64];
#if defined(__aarch64__)
      double (*f)(double, double) = k == 0 ? a64_fmin_d : k == 1 ? a64_fmax_d : k == 2 ? a64_fminnm_d : a64_fmaxnm_d;
      g = d2u(f(u2d(a), u2d(b))); snprintf(name, 64, "a64.%s_d", nm);
      CHK64(strdup(name), 0, g, exp, "%016llx %016llx exp %016llx got %016llx", (unsigned long long)a, (unsigned long long)b, (unsigned long long)exp, (unsigned long long)g);
#elif defined(__powerpc64__)
      g = d2u((ismax ? p_xsmaxdp : p_xsmindp)(u2d(a), u2d(b))); snprintf(name, 64, "ppc.%s_d.xsmaxdp_ctl", nm);
      CHK64(strdup(name), 1, g, exp, "%016llx %016llx exp %016llx got %016llx", (unsigned long long)a, (unsigned long long)b, (unsigned long long)exp, (unsigned long long)g);
      if (isnum) { g = d2u((ismax ? p_fixnm_xsmaxdp : p_fixnm_xsmindp)(u2d(a), u2d(b))); snprintf(name, 64, "ppc.%s_d.xsmaxdp_precheck", nm); }
      else { g = d2u((ismax ? p_fix_xsmaxdp : p_fix_xsmindp)(u2d(a), u2d(b))); snprintf(name, 64, "ppc.%s_d.xsmaxdp_precheck", nm); }
      CHK64(strdup(name), 0, g, exp, "%016llx %016llx exp %016llx got %016llx", (unsigned long long)a, (unsigned long long)b, (unsigned long long)exp, (unsigned long long)g);
#ifdef P9
      g = d2u((ismax ? p_xsmaxjdp : p_xsminjdp)(u2d(a), u2d(b))); snprintf(name, 64, "ppc.%s_d.xsmaxjdp_ctl", nm);
      CHK64(strdup(name), 1, g, exp, "%016llx %016llx exp %016llx got %016llx", (unsigned long long)a, (unsigned long long)b, (unsigned long long)exp, (unsigned long long)g);
      g = d2u((ismax ? p_xsmaxcdp : p_xsmincdp)(u2d(a), u2d(b))); snprintf(name, 64, "ppc.%s_d.xsmaxcdp_ctl", nm);
      CHK64(strdup(name), 1, g, exp, "%016llx %016llx exp %016llx got %016llx", (unsigned long long)a, (unsigned long long)b, (unsigned long long)exp, (unsigned long long)g);
#endif
#endif
    }
  }
  /* f32 */
  for (int i = 0; i < NF; i++) for (int j = 0; j < NF; j++) {
    uint32_t a = F32[i], b = F32[j];
    for (int k = 0; k < 4; k++) {
      int ismax = k & 1, isnum = k >> 1;
      const char* nm = k == 0 ? "fmin" : k == 1 ? "fmax" : k == 2 ? "fminnm" : "fmaxnm";
      uint32_t exp = arm_minmax32(a, b, ismax, isnum), g; char name[64];
#if defined(__aarch64__)
      float (*f)(float, float) = k == 0 ? a64_fmin_s : k == 1 ? a64_fmax_s : k == 2 ? a64_fminnm_s : a64_fmaxnm_s;
      g = f2u(f(u2f(a), u2f(b))); snprintf(name, 64, "a64.%s_s", nm);
      CHK64(strdup(name), 0, g, exp, "%08x %08x exp %08x got %08x", a, b, exp, g);
#elif defined(__powerpc64__)
      g = f2u((ismax ? p_xvmaxsp : p_xvminsp)(u2f(a), u2f(b))); snprintf(name, 64, "ppc.%s_s.xvmaxsp_ctl", nm);
      CHK64(strdup(name), 1, g, exp, "%08x %08x exp %08x got %08x", a, b, exp, g);
#endif
    }
  }
}
static void check_fma(void) {
  /* triples from a reduced list, all four modes */
  static const int pick[] = { 0, 1, 2, 6, 8, 9, 10, 12, 14, 20, 22, 24, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 62 };
  const int NP = sizeof pick / sizeof pick[0];
  for (int rm = 0; rm < 4; rm++) {
    set_rmode(rm);
    for (int i = 0; i < NP + 40; i++) for (int j = 0; j < NP + 40; j++) for (int k = 0; k < NP + 40; k++) {
      uint64_t n = i < NP ? D64[pick[i]] : rnd64(), m = j < NP ? D64[pick[j]] : rnd64(), a = k < NP ? D64[pick[k]] : rnd64();
      for (int v = 0; v < 4; v++) {
        /* v: 0 FMADD a+n*m, 1 FNMSUB -a+n*m, 2 FMSUB a-n*m, 3 FNMADD -a-n*m */
        uint64_t an = a, nn = n;
        if (v == 1) an ^= 1ull << 63;
        if (v == 2) nn ^= 1ull << 63;
        if (v == 3) { an ^= 1ull << 63; nn ^= 1ull << 63; }
        uint64_t exp = arm_muladd64(an, nn, m), g; char name[64];
        const char* nm = v == 0 ? "fmadd" : v == 1 ? "fnmsub" : v == 2 ? "fmsub" : "fnmadd";
#if defined(__aarch64__)
        double (*f)(double, double, double) = v == 0 ? a64_fmadd_d : v == 1 ? a64_fnmsub_d : v == 2 ? a64_fmsub_d : a64_fnmadd_d;
        g = d2u(f(u2d(n), u2d(m), u2d(a))); snprintf(name, 64, "a64.%s_d", nm);
        CHK64(strdup(name), 0, g, exp, "rm%d n=%016llx m=%016llx a=%016llx exp %016llx got %016llx", rm, (unsigned long long)n, (unsigned long long)m, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
#elif defined(__powerpc64__)
        double (*raw)(double, double, double) = v == 0 ? p_xsmaddadp : v == 1 ? p_xsmsubadp : v == 2 ? p_fma_fmsub : p_fma_fnmadd;
        g = d2u(raw(u2d(n), u2d(m), u2d(a))); snprintf(name, 64, "ppc.%s_d.fused_raw_ctl", nm);
        CHK64(strdup(name), 1, g, exp, "rm%d n=%016llx m=%016llx a=%016llx exp %016llx got %016llx", rm, (unsigned long long)n, (unsigned long long)m, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
        g = d2u(p_fma_post(u2d(n), u2d(m), u2d(a), v)); snprintf(name, 64, "ppc.%s_d.fused_postcheck", nm);
        CHK64(strdup(name), 0, g, exp, "rm%d n=%016llx m=%016llx a=%016llx exp %016llx got %016llx", rm, (unsigned long long)n, (unsigned long long)m, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
        if (v == 2) { g = d2u(p_xsnmsubadp(u2d(n), u2d(m), u2d(a))); snprintf(name, 64, "ppc.fmsub_d.xsnmsubadp_ctl");
          CHK64(strdup(name), 1, g, exp, "rm%d n=%016llx m=%016llx a=%016llx exp %016llx got %016llx", rm, (unsigned long long)n, (unsigned long long)m, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g); }
        if (v == 3) { g = d2u(p_xsnmaddadp(u2d(n), u2d(m), u2d(a))); snprintf(name, 64, "ppc.fnmadd_d.xsnmaddadp_ctl");
          CHK64(strdup(name), 1, g, exp, "rm%d n=%016llx m=%016llx a=%016llx exp %016llx got %016llx", rm, (unsigned long long)n, (unsigned long long)m, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g); }
#endif
      }
    }
  }
  /* f32: edge list only */
  set_rmode(0);
  for (int i = 0; i < NF; i += 2) for (int j = 0; j < NF; j += 2) for (int k = 0; k < NF; k += 2) {
    uint32_t n = F32[i], m = F32[j], a = F32[k];
    for (int v = 0; v < 4; v++) {
      uint32_t an = a, nn = n;
      if (v == 1) an ^= 1u << 31;
      if (v == 2) nn ^= 1u << 31;
      if (v == 3) { an ^= 1u << 31; nn ^= 1u << 31; }
      uint32_t exp = arm_muladd32(an, nn, m), g; char name[64];
      const char* nm = v == 0 ? "fmadd" : v == 1 ? "fnmsub" : v == 2 ? "fmsub" : "fnmadd";
#if defined(__aarch64__)
      float (*f)(float, float, float) = v == 0 ? a64_fmadd_s : v == 1 ? a64_fnmsub_s : v == 2 ? a64_fmsub_s : a64_fnmadd_s;
      g = f2u(f(u2f(n), u2f(m), u2f(a))); snprintf(name, 64, "a64.%s_s", nm);
      CHK64(strdup(name), 0, g, exp, "n=%08x m=%08x a=%08x exp %08x got %08x", n, m, a, exp, g);
#elif defined(__powerpc64__)
      if (v == 0) g = f2u(p_xvmaddasp(u2f(n), u2f(m), u2f(a)));
      else if (v == 1) g = f2u(p_xvmsubasp(u2f(n), u2f(m), u2f(a)));
      else if (v == 2) g = f2u(p_xvmaddasp(p_negf(u2f(n)), u2f(m), u2f(a)));
      else g = f2u(p_xvmsubasp(p_negf(u2f(n)), u2f(m), u2f(a)));
      snprintf(name, 64, "ppc.%s_s.fused_raw_ctl", nm);
      CHK64(strdup(name), 1, g, exp, "n=%08x m=%08x a=%08x exp %08x got %08x", n, m, a, exp, g);
      if (v == 2) { g = f2u(p_xvnmsubasp(u2f(n), u2f(m), u2f(a))); CHK64("ppc.fmsub_s.xvnmsubasp_ctl", 1, g, exp, "n=%08x m=%08x a=%08x exp %08x got %08x", n, m, a, exp, g); }
      if (v == 3) { g = f2u(p_xvnmaddasp(u2f(n), u2f(m), u2f(a))); CHK64("ppc.fnmadd_s.xvnmaddasp_ctl", 1, g, exp, "n=%08x m=%08x a=%08x exp %08x got %08x", n, m, a, exp, g); }
#endif
    }
  }
}
static void check_unary(void) {
  for (int rm = 0; rm < 4; rm++) {
    set_rmode(rm);
    for (int i = 0; i < ND + NRAND; i++) {
      uint64_t a = i < ND ? D64[i] : rnd64();
      uint64_t exp = arm_sqrt64(a), g;
#if defined(__aarch64__)
      g = d2u(a64_fsqrt_d(u2d(a))); CHK64("a64.fsqrt_d", 0, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
#elif defined(__powerpc64__)
      g = d2u(p_xssqrtdp(u2d(a))); CHK64("ppc.fsqrt_d.xssqrtdp", 0, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
      g = d2u(p_xvsqrtdp(u2d(a))); CHK64("ppc.fsqrt_d.xvsqrtdp", 0, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
#endif
      /* d -> s */
      uint32_t e32 = arm_d2s(a, rm), g32;
#if defined(__aarch64__)
      g32 = f2u(a64_fcvt_sd(u2d(a))); CHK64("a64.fcvt_sd", 0, g32, e32, "rm%d %016llx exp %08x got %08x", rm, (unsigned long long)a, e32, g32);
      uint16_t e16 = arm_d2h(a, rm), g16 = a64_fcvt_hd(u2d(a));
      CHK64("a64.fcvt_hd", 0, g16, e16, "rm%d %016llx exp %04x got %04x", rm, (unsigned long long)a, e16, g16);
#elif defined(__powerpc64__)
      g32 = f2u(p_xscvdpsp(u2d(a))); CHK64("ppc.fcvt_sd.xscvdpsp", 0, g32, e32, "rm%d %016llx exp %08x got %08x", rm, (unsigned long long)a, e32, g32);
#ifdef P9
      uint16_t e16 = arm_d2h(a, rm), g16 = p_xscvdphp(u2d(a));
      CHK64("ppc.fcvt_hd.xscvdphp", 0, g16, e16, "rm%d %016llx exp %04x got %04x", rm, (unsigned long long)a, e16, g16);
#endif
#endif
    }
    for (int i = 0; i < NF + NRAND; i++) {
      uint32_t a = i < NF ? F32[i] : rnd32();
      uint32_t exp = arm_sqrt32(a), g;
      uint64_t e64 = widen_s2d(a), g64;
#if defined(__aarch64__)
      g = f2u(a64_fsqrt_s(u2f(a))); CHK64("a64.fsqrt_s", 0, g, exp, "rm%d %08x exp %08x got %08x", rm, a, exp, g);
      g64 = d2u(a64_fcvt_ds(u2f(a))); CHK64("a64.fcvt_ds", 0, g64, e64, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)e64, (unsigned long long)g64);
      uint16_t e16 = arm_d2h(e64, rm), g16 = a64_fcvt_hs(u2f(a));
      CHK64("a64.fcvt_hs", 0, g16, e16, "rm%d %08x exp %04x got %04x", rm, a, e16, g16);
#elif defined(__powerpc64__)
      g = f2u(p_xvsqrtsp(u2f(a))); CHK64("ppc.fsqrt_s.xvsqrtsp", 0, g, exp, "rm%d %08x exp %08x got %08x", rm, a, exp, g);
      g64 = d2u(p_xscvspdp(u2f(a))); CHK64("ppc.fcvt_ds.xscvspdp", 0, g64, e64, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)e64, (unsigned long long)g64);
      g64 = d2u(p_xscvspdpn_ctl(u2f(a))); CHK64("ppc.fcvt_ds.xscvspdpn_ctl", 1, g64, e64, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)e64, (unsigned long long)g64);
#endif
    }
  }
  /* h -> d, h -> s: exhaustive */
  set_rmode(0);
  for (uint32_t h = 0; h < 0x10000; h++) {
    uint64_t e64 = widen_h2d((uint16_t)h), g64; uint32_t e32 = arm_h2s((uint16_t)h), g32;
#if defined(__aarch64__)
    g64 = d2u(a64_fcvt_dh((uint16_t)h)); CHK64("a64.fcvt_dh", 0, g64, e64, "%04x exp %016llx got %016llx", h, (unsigned long long)e64, (unsigned long long)g64);
    g32 = f2u(a64_fcvt_sh((uint16_t)h)); CHK64("a64.fcvt_sh", 0, g32, e32, "%04x exp %08x got %08x", h, e32, g32);
#elif defined(__powerpc64__)
#ifdef P9
    g64 = d2u(p_xscvhpdp((uint16_t)h)); CHK64("ppc.fcvt_dh.xscvhpdp", 0, g64, e64, "%04x exp %016llx got %016llx", h, (unsigned long long)e64, (unsigned long long)g64);
#endif
    (void)e32; (void)g32;
#endif
  }
}
static void check_fcmp(void) {
  set_rmode(0);
  for (int i = 0; i < ND; i++) for (int j = 0; j < ND; j++) {
    uint64_t a = D64[i], b = D64[j]; unsigned exp = arm_fcmp64(a, b), g;
#if defined(__aarch64__)
    g = a64_fcmp_d(u2d(a), u2d(b)); CHK64("a64.fcmp_d", 0, g, exp, "%016llx %016llx exp %x got %x", (unsigned long long)a, (unsigned long long)b, exp, g);
#elif defined(__powerpc64__)
    g = p_fcmp_d(u2d(a), u2d(b)); CHK64("ppc.fcmp_d.xscmpudp_nzcv", 0, g, exp, "%016llx %016llx exp %x got %x", (unsigned long long)a, (unsigned long long)b, exp, g);
    g = p_fcmpu_d(u2d(a), u2d(b)); CHK64("ppc.fcmp_d.fcmpu_nzcv", 0, g, exp, "%016llx %016llx exp %x got %x", (unsigned long long)a, (unsigned long long)b, exp, g);
#endif
  }
  for (int i = 0; i < NF; i++) for (int j = 0; j < NF; j++) {
    uint32_t a = F32[i], b = F32[j]; unsigned exp = arm_fcmp32(a, b), g;
#if defined(__aarch64__)
    g = a64_fcmp_s(u2f(a), u2f(b)); CHK64("a64.fcmp_s", 0, g, exp, "%08x %08x exp %x got %x", a, b, exp, g);
#elif defined(__powerpc64__)
    g = p_fcmp_s(u2f(a), u2f(b)); CHK64("ppc.fcmp_s.xscvspdp_xscmpudp", 0, g, exp, "%08x %08x exp %x got %x", a, b, exp, g);
#endif
  }
}
static void check_toint(void) {
  static const char* rn[5] = { "n", "p", "m", "z", "a" };
  for (int rm = 0; rm < 4; rm++) {
    set_rmode(rm);
    for (int i = 0; i < ND + NRAND * 4; i++) {
      uint64_t a = i < ND ? D64[i] : rnd64();
      for (int r = 0; r < 5; r++) for (int u = 0; u < 2; u++) for (int w = 0; w < 2; w++) {
        int bits = w ? 32 : 64;
        uint64_t exp = arm_fptofixed64(a, 0, u, r, bits), g; char name[64];
        snprintf(name, 64, "fcvt%s%s_%sd", rn[r], u ? "u" : "s", w ? "w" : "x");
#if defined(__aarch64__)
        uint64_t (*f)(double) = NULL;
        if (!w) switch (r) { case 0: f = u ? a64_fcvtnu_xd : a64_fcvtns_xd; break; case 1: f = u ? a64_fcvtpu_xd : a64_fcvtps_xd; break;
          case 2: f = u ? a64_fcvtmu_xd : a64_fcvtms_xd; break; case 3: f = u ? a64_fcvtzu_xd : a64_fcvtzs_xd; break; default: f = u ? a64_fcvtau_xd : a64_fcvtas_xd; }
        else switch (r) { case 0: f = u ? a64_fcvtnu_wd : a64_fcvtns_wd; break; case 1: f = u ? a64_fcvtpu_wd : a64_fcvtps_wd; break;
          case 2: f = u ? a64_fcvtmu_wd : a64_fcvtms_wd; break; case 3: f = u ? a64_fcvtzu_wd : a64_fcvtzs_wd; break; default: f = u ? a64_fcvtau_wd : a64_fcvtas_wd; }
        g = f(u2d(a)); char nm2[80]; snprintf(nm2, 80, "a64.%s", name);
        CHK64(strdup(nm2), 0, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
        if (golden && i < ND) printf("%s rm%d %016llx %016llx\n", name, rm, (unsigned long long)a, (unsigned long long)exp);
#elif defined(__powerpc64__)
        /* rounding step variants for TIEEVEN: 0 = P8 sandwich, 1 = P9 sandwich, 2 = plain xsrdpic (valid only when RN==nearest) */
        int nvar = r == 0 ? 3 : 1;
        for (int var = 0; var < nvar; var++) {
#ifndef P9
          if (var == 1) continue;
#endif
          double rr = p_round(u2d(a), r, var);
          const char* vn = r != 0 ? "" : var == 0 ? ".p8sandwich" : var == 1 ? ".p9mffscrn" : ".assumeRN";
          int ctl = (var == 2 && rm != 0);
          char nm2[96];
          if (!u && !w) {
            g = p_cvt_sx64_isel(rr); snprintf(nm2, 96, "ppc.%s%s.isel%s", name, vn, ctl ? "_ctl" : "");
            CHK64(strdup(nm2), ctl, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
            if (var != 2) {
              g = p_cvt_sx64_branch(rr); snprintf(nm2, 96, "ppc.%s%s.branch", name, vn);
              CHK64(strdup(nm2), 0, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
              g = p_cvt_sx64_mask(rr); snprintf(nm2, 96, "ppc.%s%s.mask", name, vn);
              CHK64(strdup(nm2), 0, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
              g = p_cvt_sx64_fctidz(rr); snprintf(nm2, 96, "ppc.%s%s.fctidz_isel", name, vn);
              CHK64(strdup(nm2), 0, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
              g = p_cvt_sx64_ctl(rr); snprintf(nm2, 96, "ppc.%s%s.noNaNfix_ctl", name, vn);
              CHK64(strdup(nm2), 1, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
            }
            if (r == 0 && var == 2) { /* fctid: rounds with RN directly, no xsrdpic */
              g = p_cvt_fctid_isel(u2d(a)); snprintf(nm2, 96, "ppc.%s.fctid_assumeRN%s", name, ctl ? "_ctl" : "");
              CHK64(strdup(nm2), ctl, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
            }
          } else if (u && !w) {
            g = p_cvt_ux64(rr); snprintf(nm2, 96, "ppc.%s%s.xscvdpuxds%s", name, vn, ctl ? "_ctl" : "");
            CHK64(strdup(nm2), ctl, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
            if (var != 2) { g = p_cvt_ux64_fctiduz(rr); snprintf(nm2, 96, "ppc.%s%s.fctiduz", name, vn);
              CHK64(strdup(nm2), 0, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g); }
          } else if (!u && w) {
            g = p_cvt_sx32_isel(rr); snprintf(nm2, 96, "ppc.%s%s.isel%s", name, vn, ctl ? "_ctl" : "");
            CHK64(strdup(nm2), ctl, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
            if (var != 2) { g = p_cvt_sx32_ctl(rr); snprintf(nm2, 96, "ppc.%s%s.noNaNfix_ctl", name, vn);
              CHK64(strdup(nm2), 1, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g); }
          } else {
            g = p_cvt_ux32(rr); snprintf(nm2, 96, "ppc.%s%s.xscvdpuxws%s", name, vn, ctl ? "_ctl" : "");
            CHK64(strdup(nm2), ctl, g, exp, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)a, (unsigned long long)exp, (unsigned long long)g);
          }
        }
#endif
      }
      /* fixed point, truncating: fbits 1, 32, 63/64 */
      for (int fb = 0; fb < 3; fb++) {
        int fbits_s = fb == 0 ? 1 : fb == 1 ? 32 : 63, fbits_u = fb == 0 ? 1 : fb == 1 ? 32 : 64;
        uint64_t es = arm_fptofixed64(a, fbits_s, 0, RM_ZERO, 64), eu = arm_fptofixed64(a, fbits_u, 1, RM_ZERO, 64), g;
#if defined(__aarch64__)
        g = a64_fcvtzs_xd_fix(u2d(a), fbits_s); CHK64("a64.fcvtzs_fix_xd", 0, g, es, "fb%d %016llx exp %016llx got %016llx", fbits_s, (unsigned long long)a, (unsigned long long)es, (unsigned long long)g);
        g = a64_fcvtzu_xd_fix(u2d(a), fbits_u); CHK64("a64.fcvtzu_fix_xd", 0, g, eu, "fb%d %016llx exp %016llx got %016llx", fbits_u, (unsigned long long)a, (unsigned long long)eu, (unsigned long long)g);
#elif defined(__powerpc64__)
        /* scale by 2^fbits with xsmuldp (exact or overflows to inf), then convert */
        double sc = u2d((uint64_t)(1023 + fbits_s) << 52), scu = u2d((uint64_t)(1023 + fbits_u) << 52);
        g = p_cvt_sx64_isel(p_xsmuldp(u2d(a), sc)); CHK64("ppc.fcvtzs_fix_xd.xsmuldp_isel", 0, g, es, "fb%d %016llx exp %016llx got %016llx", fbits_s, (unsigned long long)a, (unsigned long long)es, (unsigned long long)g);
        g = p_cvt_ux64(p_xsmuldp(u2d(a), scu)); CHK64("ppc.fcvtzu_fix_xd.xsmuldp_xscvdpuxds", 0, g, eu, "fb%d %016llx exp %016llx got %016llx", fbits_u, (unsigned long long)a, (unsigned long long)eu, (unsigned long long)g);
#endif
      }
    }
    /* f32 sources: via exact widening */
    for (int i = 0; i < NF + NRAND; i++) {
      uint32_t a = i < NF ? F32[i] : rnd32();
      uint64_t wide = widen_s2d(a);
      uint64_t ezs = arm_fptofixed64(wide, 0, 0, RM_ZERO, 64), ezu = arm_fptofixed64(wide, 0, 1, RM_ZERO, 64);
      uint64_t ens = arm_fptofixed64(wide, 0, 0, RM_TIEEVEN, 64), eas = arm_fptofixed64(wide, 0, 0, RM_TIEAWAY, 64);
      uint64_t ezsw = arm_fptofixed64(wide, 0, 0, RM_ZERO, 32), ezuw = arm_fptofixed64(wide, 0, 1, RM_ZERO, 32);
      uint64_t ensw = arm_fptofixed64(wide, 0, 0, RM_TIEEVEN, 32), eauw = arm_fptofixed64(wide, 0, 1, RM_TIEAWAY, 32), g;
#if defined(__aarch64__)
      g = a64_fcvtzs_xs(u2f(a)); CHK64("a64.fcvtzs_xs", 0, g, ezs, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)ezs, (unsigned long long)g);
      g = a64_fcvtzu_xs(u2f(a)); CHK64("a64.fcvtzu_xs", 0, g, ezu, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)ezu, (unsigned long long)g);
      g = a64_fcvtns_xs(u2f(a)); CHK64("a64.fcvtns_xs", 0, g, ens, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)ens, (unsigned long long)g);
      g = a64_fcvtas_xs(u2f(a)); CHK64("a64.fcvtas_xs", 0, g, eas, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)eas, (unsigned long long)g);
      g = a64_fcvtzs_ws(u2f(a)); CHK64("a64.fcvtzs_ws", 0, g, ezsw, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)ezsw, (unsigned long long)g);
      g = a64_fcvtzu_ws(u2f(a)); CHK64("a64.fcvtzu_ws", 0, g, ezuw, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)ezuw, (unsigned long long)g);
      g = a64_fcvtns_ws(u2f(a)); CHK64("a64.fcvtns_ws", 0, g, ensw, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)ensw, (unsigned long long)g);
      g = a64_fcvtau_ws(u2f(a)); CHK64("a64.fcvtau_ws", 0, g, eauw, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)eauw, (unsigned long long)g);
#elif defined(__powerpc64__)
      double d = p_widen_f32(u2f(a));
      g = p_cvt_sx64_isel(d); CHK64("ppc.fcvtzs_xs.widen_isel", 0, g, ezs, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)ezs, (unsigned long long)g);
      g = p_cvt_ux64(d); CHK64("ppc.fcvtzu_xs.widen_xscvdpuxds", 0, g, ezu, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)ezu, (unsigned long long)g);
      g = p_cvt_sx64_isel(p_round(d, RM_TIEAWAY, 0)); CHK64("ppc.fcvtas_xs.widen_xsrdpi_isel", 0, g, eas, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)eas, (unsigned long long)g);
      g = p_cvt_sx32_isel(d); CHK64("ppc.fcvtzs_ws.widen_isel", 0, g, ezsw, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)ezsw, (unsigned long long)g);
      g = p_cvt_ux32(d); CHK64("ppc.fcvtzu_ws.widen_xscvdpuxws", 0, g, ezuw, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)ezuw, (unsigned long long)g);
      g = p_cvt_ux32(p_round(d, RM_TIEAWAY, 0)); CHK64("ppc.fcvtau_ws.widen_xsrdpi", 0, g, eauw, "rm%d %08x exp %016llx got %016llx", rm, a, (unsigned long long)eauw, (unsigned long long)g);
      (void)ens; (void)ensw;
#endif
    }
  }
}
static void check_fromint(void) {
  static const uint64_t I64[] = { 0, 1, (uint64_t)-1, 2, 3, 0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFull, 0x100000000ull,
    0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x8000000000000001ull, 0xFFFFFFFFFFFFFFFFull,
    0x0020000000000001ull, 0x0020000000000003ull, 0x0040000000000005ull, /* 2^53+1, 2^53+3, 2^54+5: double ties */
    0x8000004000000001ull, 0x8000004000000000ull, 0x800000C000000000ull, /* 2^63 + 2^38 (+1): float double-rounding */
    0x0000000001000001ull, 0x0000000001000003ull, 0x0000000003000005ull, /* 2^24+1, 2^24+3, ties for float */
    0x123456789ABCDEF0ull, 0xFEDCBA9876543210ull, 0x00000000FFFFFF80ull, 0x00000000FFFFFFC0ull, 0x00000000FFFFFFE0ull,
    0x0000000080000080ull, 0x00000000800000C0ull, 0x00000000800000A0ull };
  const int NI = sizeof I64 / sizeof I64[0];
  for (int rm = 0; rm < 4; rm++) {
    set_rmode(rm);
    for (int i = 0; i < NI + 20000; i++) {
      uint64_t v = i < NI ? I64[i] : (rnd() >> (rnd() & 63)) ^ (rnd() & 0x8000000000000000ull);
      uint64_t es = arm_fixedtofp64(v, 0, rm), eu = arm_fixedtofp64(v, 1, rm), g;
      uint32_t es32 = arm_fixedtofp32(v, 0, rm), eu32 = arm_fixedtofp32(v, 1, rm), g32;
      uint64_t esw = arm_fixedtofp64((uint64_t)(int64_t)(int32_t)v, 0, rm), euw = arm_fixedtofp64((uint32_t)v, 1, rm);
      uint32_t esw32 = arm_fixedtofp32((uint64_t)(int64_t)(int32_t)v, 0, rm);
#if defined(__aarch64__)
      g = d2u(a64_scvtf_dx((int64_t)v)); CHK64("a64.scvtf_dx", 0, g, es, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)v, (unsigned long long)es, (unsigned long long)g);
      g = d2u(a64_ucvtf_dx(v)); CHK64("a64.ucvtf_dx", 0, g, eu, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)v, (unsigned long long)eu, (unsigned long long)g);
      g32 = f2u(a64_scvtf_sx((int64_t)v)); CHK64("a64.scvtf_sx", 0, g32, es32, "rm%d %016llx exp %08x got %08x", rm, (unsigned long long)v, es32, g32);
      g32 = f2u(a64_ucvtf_sx(v)); CHK64("a64.ucvtf_sx", 0, g32, eu32, "rm%d %016llx exp %08x got %08x", rm, (unsigned long long)v, eu32, g32);
      g = d2u(a64_scvtf_dw((int32_t)v)); CHK64("a64.scvtf_dw", 0, g, esw, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)v, (unsigned long long)esw, (unsigned long long)g);
      g = d2u(a64_ucvtf_dw((uint32_t)v)); CHK64("a64.ucvtf_dw", 0, g, euw, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)v, (unsigned long long)euw, (unsigned long long)g);
      g32 = f2u(a64_scvtf_sw((int32_t)v)); CHK64("a64.scvtf_sw", 0, g32, esw32, "rm%d %016llx exp %08x got %08x", rm, (unsigned long long)v, esw32, g32);
#elif defined(__powerpc64__)
      g = d2u(p_cvt_sxd((int64_t)v)); CHK64("ppc.scvtf_dx.xscvsxddp", 0, g, es, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)v, (unsigned long long)es, (unsigned long long)g);
      g = d2u(p_cvt_uxd(v)); CHK64("ppc.ucvtf_dx.xscvuxddp", 0, g, eu, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)v, (unsigned long long)eu, (unsigned long long)g);
      g32 = f2u(p_cvt_sxs((int64_t)v)); CHK64("ppc.scvtf_sx.xscvsxdsp", 0, g32, es32, "rm%d %016llx exp %08x got %08x", rm, (unsigned long long)v, es32, g32);
      g32 = f2u(p_cvt_uxs(v)); CHK64("ppc.ucvtf_sx.xscvuxdsp", 0, g32, eu32, "rm%d %016llx exp %08x got %08x", rm, (unsigned long long)v, eu32, g32);
      g32 = f2u(p_cvt_sxs_fcfids((int64_t)v)); CHK64("ppc.scvtf_sx.fcfids", 0, g32, es32, "rm%d %016llx exp %08x got %08x", rm, (unsigned long long)v, es32, g32);
      g32 = f2u(p_cvt_sxs_ctl((int64_t)v)); CHK64("ppc.scvtf_sx.fcfid_frsp_ctl", 1, g32, es32, "rm%d %016llx exp %08x got %08x", rm, (unsigned long long)v, es32, g32);
      g = d2u(p_cvt_swd_mtvsrwa((int32_t)v)); CHK64("ppc.scvtf_dw.mtvsrwa_xscvsxddp", 0, g, esw, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)v, (unsigned long long)esw, (unsigned long long)g);
      g = d2u(p_cvt_uwd_mtvsrwz((uint32_t)v)); CHK64("ppc.ucvtf_dw.mtvsrwz_xscvuxddp", 0, g, euw, "rm%d %016llx exp %016llx got %016llx", rm, (unsigned long long)v, (unsigned long long)euw, (unsigned long long)g);
      g32 = f2u(p_cvt_sws_mtvsrwa((int32_t)v)); CHK64("ppc.scvtf_sw.mtvsrwa_xscvsxdsp", 0, g32, esw32, "rm%d %016llx exp %08x got %08x", rm, (unsigned long long)v, esw32, g32);
#endif
    }
  }
}

int main(int argc, char** argv) {
  golden = argc > 1 && !strcmp(argv[1], "golden");
  setvbuf(stdout, NULL, _IONBF, 0);
  check_binop64();
  check_binop32();
  check_minmax();
  check_fma();
  check_unary();
  check_fcmp();
  check_toint();
  check_fromint();
  set_rmode(0);
  if (!golden) {
    printf("== %s: %d corpus doubles, %d floats, %d random per axis\n", HW_NAME, ND, NF, NRAND);
    report();
    printf("== verdict: %s (%d candidate(s) wrong or control(s) silent)\n", total_fail ? "FAIL" : "PASS", total_fail);
  }
  return total_fail ? 1 : 0;
}
