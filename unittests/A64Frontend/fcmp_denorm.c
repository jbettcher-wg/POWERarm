// SPDX-License-Identifier: MIT
//
// Vector float compares over denormal, NaN, signed-zero and infinity inputs,
// at both element widths and in both register banks.
//
// Self-checking, and deliberately so: every answer below is forced by the
// architecture, so this file states it as a literal rather than comparing
// against a reference run. No .golden is captured for it, and none could be
// trusted -- a golden taken from the emulator under test agrees with a
// failing emulator.
//
// Why it exists: the 4-byte arms of VFCMPEQ/VFCMPLT/VFCMPLE/VFCMPUNO used
// the VMX vcmpeqfp/vcmpgtfp/vcmpgefp, and now use their VSX twins
// xvcmpeqsp/xvcmpgtsp/xvcmpgesp so the ops can name a pinned low-bank
// register. The VMX float compares are "Java/IEEE-mode" instructions: with
// VSCR.NJ set they flush denormal INPUTS to zero, and the VSX ones never do.
// So the swap is only safe while NJ is 0, and the thing that would go wrong
// if it were not is exactly a denormal compared against zero:
//
//     fcmeq(1e-40f, 0.0f)   must be 0           -- flushing says "equal"
//     fcmgt(1e-40f, 0.0f)   must be all-ones    -- flushing says "not greater"
//     fcmgt(2e-40f, 1e-40f) must be all-ones    -- flushing says "not greater"
//
// Every such case is here for .4s AND .2d against the SAME expected answer,
// which is the sharper property: the two arms of one IR op are different
// host instructions, and the claim the swap rests on is that they agree.
// A denormal-flushing 4-byte arm fails while the 8-byte arm passes, and the
// line that fails says which.
//
// FPCR is left alone, so FZ=0 and denormals are not flushed by the guest
// either. (The emulator ignores guest FPCR.FZ on both arms alike -- that
// divergence is documented at the backend's SetRoundingMode and is not what
// this test is about.)
//
// The NaN rows matter for a second reason: xvcmp*sp sets FPSCR VXSNAN/VXVC
// where vcmp*fp set nothing. Nothing in the emulator reads those bits, and
// FPSCR.VE is 0 so there is no trap -- but a NaN input must still compare
// false every way, which is what these rows assert.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int Failures = 0;
static int Checks = 0;

typedef struct {
  uint8_t b[16];
} V128;

static void expect(const char* what, const V128* got, const V128* want) {
  ++Checks;
  if (memcmp(got->b, want->b, 16) == 0) {
    return;
  }
  ++Failures;
  printf("FAIL %s\n  got ", what);
  for (int i = 0; i < 16; ++i) {
    printf("%02x", got->b[i]);
  }
  printf("\n want ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x", want->b[i]);
  }
  printf("\n");
}

static V128 splat32(uint32_t bits) {
  V128 v;
  for (int i = 0; i < 4; ++i) {
    memcpy(v.b + 4 * i, &bits, 4);
  }
  return v;
}

static V128 splat64(uint64_t bits) {
  V128 v;
  for (int i = 0; i < 2; ++i) {
    memcpy(v.b + 8 * i, &bits, 8);
  }
  return v;
}

// A compare result is all-ones or all-zeroes PER LANE, at the lane width.
static V128 mask32(int on) {
  return splat32(on ? 0xFFFFFFFFu : 0u);
}
static V128 mask64(int on) {
  return splat64(on ? ~(uint64_t)0 : 0);
}

// ---------------------------------------------------------------------------
// Bit patterns. Written as bits, not as C literals, so the compiler's own
// float handling cannot quietly round or flush one of them.
// ---------------------------------------------------------------------------
#define F32_ONE 0x3F800000u
#define F32_TWO 0x40000000u
#define F32_NEG_ONE 0xBF800000u
#define F32_POS_ZERO 0x00000000u
#define F32_NEG_ZERO 0x80000000u
#define F32_DEN_SMALL 0x00000001u // smallest positive denormal
#define F32_DEN_BIG 0x00400000u   // exponent 0, so still denormal
#define F32_DEN_NEG 0x80000001u
#define F32_QNAN 0x7FC00000u
#define F32_SNAN 0x7F800001u
#define F32_POS_INF 0x7F800000u
#define F32_NEG_INF 0xFF800000u

#define F64_ONE 0x3FF0000000000000ull
#define F64_TWO 0x4000000000000000ull
#define F64_NEG_ONE 0xBFF0000000000000ull
#define F64_POS_ZERO 0x0000000000000000ull
#define F64_NEG_ZERO 0x8000000000000000ull
#define F64_DEN_SMALL 0x0000000000000001ull
#define F64_DEN_BIG 0x0008000000000000ull
#define F64_DEN_NEG 0x8000000000000001ull
#define F64_QNAN 0x7FF8000000000000ull
#define F64_SNAN 0x7FF0000000000001ull
#define F64_POS_INF 0x7FF0000000000000ull
#define F64_NEG_INF 0xFFF0000000000000ull

// ---------------------------------------------------------------------------
// One function per (instruction, width, register bank). The register numbers
// are textual because the BANK is under test: a v19 here is guest V19, which
// the backend keeps pinned in the host's low VSR bank, where a VMX-form
// compare cannot reach it.
// ---------------------------------------------------------------------------
#define CMP_FN(TAG, MNEM, TY, AREG, BREG, DREG)                              \
  static void fn_##TAG(const V128* a, const V128* b, V128* out) {            \
    asm volatile("ldr q" #AREG ", [%0]\n\t"                                  \
                 "ldr q" #BREG ", [%1]\n\t" MNEM " v" #DREG "." TY ", v" #AREG "." TY ", v" #BREG "." TY "\n\t" \
                 "str q" #DREG ", [%2]\n\t"                                  \
                 :                                                           \
                 : "r"(a), "r"(b), "r"(out)                                  \
                 : "memory", "v" #AREG, "v" #BREG, "v" #DREG);               \
  }

// X(tag, "mnemonic", "ty", areg, breg, dreg, which, is64)
// `which` indexes the expected-answer field; `is64` picks the lane width.
#define CMP_CASES(X)                                      \
  X(eq_s_lo, "fcmeq", "4s", 1, 2, 3, 0, 0)                \
  X(gt_s_lo, "fcmgt", "4s", 1, 2, 3, 1, 0)                \
  X(ge_s_lo, "fcmge", "4s", 1, 2, 3, 2, 0)                \
  X(acgt_s_lo, "facgt", "4s", 1, 2, 3, 3, 0)              \
  X(acge_s_lo, "facge", "4s", 1, 2, 3, 4, 0)              \
  X(eq_s_hi, "fcmeq", "4s", 17, 18, 19, 0, 0)             \
  X(gt_s_hi, "fcmgt", "4s", 17, 18, 19, 1, 0)             \
  X(ge_s_hi, "fcmge", "4s", 17, 18, 19, 2, 0)             \
  X(acgt_s_hi, "facgt", "4s", 17, 18, 19, 3, 0)           \
  X(acge_s_hi, "facge", "4s", 17, 18, 19, 4, 0)           \
  X(eq_d_lo, "fcmeq", "2d", 4, 5, 6, 0, 1)                \
  X(gt_d_lo, "fcmgt", "2d", 4, 5, 6, 1, 1)                \
  X(ge_d_lo, "fcmge", "2d", 4, 5, 6, 2, 1)                \
  X(acgt_d_lo, "facgt", "2d", 4, 5, 6, 3, 1)              \
  X(acge_d_lo, "facge", "2d", 4, 5, 6, 4, 1)              \
  X(eq_d_hi, "fcmeq", "2d", 20, 21, 22, 0, 1)             \
  X(gt_d_hi, "fcmgt", "2d", 20, 21, 22, 1, 1)             \
  X(ge_d_hi, "fcmge", "2d", 20, 21, 22, 2, 1)             \
  X(acgt_d_hi, "facgt", "2d", 20, 21, 22, 3, 1)           \
  X(acge_d_hi, "facge", "2d", 20, 21, 22, 4, 1)

#define DEFINE_CMP(TAG, MNEM, TY, AREG, BREG, DREG, WHICH, IS64) CMP_FN(TAG, MNEM, TY, AREG, BREG, DREG)
CMP_CASES(DEFINE_CMP)

struct CmpFn {
  const char* name;
  void (*fn)(const V128*, const V128*, V128*);
  int which;
  int is64;
};

#define ROW_CMP(TAG, MNEM, TY, AREG, BREG, DREG, WHICH, IS64) {#TAG, fn_##TAG, WHICH, IS64},
static const struct CmpFn CmpFns[] = {CMP_CASES(ROW_CMP)};

// The expected answers, in the order eq, gt, ge, facgt, facge. Every one is
// what IEEE-754 and the A64 definition of these instructions force for the
// pair, with FPCR.FZ clear: an unordered pair (any NaN operand) compares
// false every way, +0 equals -0, and a denormal is a perfectly ordinary
// finite number that is greater than zero.
struct Pair {
  const char* name;
  uint32_t a32, b32;
  uint64_t a64, b64;
  int want[5];
};

static const struct Pair Pairs[] = {
  // name                a32            b32            a64            b64             eq gt ge ac> ac>=
  {"eq_normal",   F32_ONE,       F32_ONE,       F64_ONE,       F64_ONE,       {1, 0, 1, 0, 1}},
  {"gt_normal",   F32_TWO,       F32_ONE,       F64_TWO,       F64_ONE,       {0, 1, 1, 1, 1}},
  {"lt_normal",   F32_ONE,       F32_TWO,       F64_ONE,       F64_TWO,       {0, 0, 0, 0, 0}},
  {"signed_zero", F32_POS_ZERO,  F32_NEG_ZERO,  F64_POS_ZERO,  F64_NEG_ZERO,  {1, 0, 1, 0, 1}},
  // The denormal rows. A flushing compare fails all four.
  {"den_vs_zero", F32_DEN_SMALL, F32_POS_ZERO,  F64_DEN_SMALL, F64_POS_ZERO,  {0, 1, 1, 1, 1}},
  {"den_vs_den",  F32_DEN_BIG,   F32_DEN_SMALL, F64_DEN_BIG,   F64_DEN_SMALL, {0, 1, 1, 1, 1}},
  {"den_eq",      F32_DEN_SMALL, F32_DEN_SMALL, F64_DEN_SMALL, F64_DEN_SMALL, {1, 0, 1, 0, 1}},
  {"negden_zero", F32_DEN_NEG,   F32_POS_ZERO,  F64_DEN_NEG,   F64_POS_ZERO,  {0, 0, 0, 1, 1}},
  // Unordered: every compare is false, every way round.
  {"qnan_num",    F32_QNAN,      F32_ONE,       F64_QNAN,      F64_ONE,       {0, 0, 0, 0, 0}},
  {"num_qnan",    F32_ONE,       F32_QNAN,      F64_ONE,       F64_QNAN,      {0, 0, 0, 0, 0}},
  {"qnan_qnan",   F32_QNAN,      F32_QNAN,      F64_QNAN,      F64_QNAN,      {0, 0, 0, 0, 0}},
  {"snan_num",    F32_SNAN,      F32_ONE,       F64_SNAN,      F64_ONE,       {0, 0, 0, 0, 0}},
  {"num_snan",    F32_ONE,       F32_SNAN,      F64_ONE,       F64_SNAN,      {0, 0, 0, 0, 0}},
  {"inf_num",     F32_POS_INF,   F32_ONE,       F64_POS_INF,   F64_ONE,       {0, 1, 1, 1, 1}},
  {"inf_inf",     F32_POS_INF,   F32_POS_INF,   F64_POS_INF,   F64_POS_INF,   {1, 0, 1, 0, 1}},
  {"neginf_inf",  F32_NEG_INF,   F32_POS_INF,   F64_NEG_INF,   F64_POS_INF,   {0, 0, 0, 0, 1}},
  {"one_negone",  F32_ONE,       F32_NEG_ONE,   F64_ONE,       F64_NEG_ONE,   {0, 1, 1, 0, 1}},
  {"den_vs_one",  F32_DEN_SMALL, F32_ONE,       F64_DEN_SMALL, F64_ONE,       {0, 0, 0, 0, 0}},
};

// ---------------------------------------------------------------------------
// VFCMPUNO is not reachable from a guest compare: the frontend emits it for
// the NaN handling inside FRECPS/FRSQRTS. Those have exact non-NaN results
// (frecps(a,b) = 2 - a*b, frsqrts(a,b) = (3 - a*b)/2), so both halves of
// that op can be asserted -- the arithmetic answer where neither input is a
// NaN, and "the result is some NaN" where one is.
// ---------------------------------------------------------------------------
#define RECPS_FN(TAG, MNEM, TY, AREG, BREG, DREG)                            \
  static void rfn_##TAG(const V128* a, const V128* b, V128* out) {           \
    asm volatile("ldr q" #AREG ", [%0]\n\t"                                  \
                 "ldr q" #BREG ", [%1]\n\t" MNEM " v" #DREG "." TY ", v" #AREG "." TY ", v" #BREG "." TY "\n\t" \
                 "str q" #DREG ", [%2]\n\t"                                  \
                 :                                                           \
                 : "r"(a), "r"(b), "r"(out)                                  \
                 : "memory", "v" #AREG, "v" #BREG, "v" #DREG);               \
  }

#define RECPS_CASES(X)                                 \
  X(recps_s_lo, "frecps", "4s", 7, 8, 9, 0, 0)         \
  X(recps_s_hi, "frecps", "4s", 23, 24, 25, 0, 0)      \
  X(recps_d_lo, "frecps", "2d", 7, 8, 9, 0, 1)         \
  X(recps_d_hi, "frecps", "2d", 23, 24, 25, 0, 1)      \
  X(rsqrts_s_lo, "frsqrts", "4s", 10, 11, 12, 1, 0)    \
  X(rsqrts_s_hi, "frsqrts", "4s", 26, 27, 28, 1, 0)    \
  X(rsqrts_d_lo, "frsqrts", "2d", 10, 11, 12, 1, 1)    \
  X(rsqrts_d_hi, "frsqrts", "2d", 26, 27, 28, 1, 1)

#define DEFINE_RECPS(TAG, MNEM, TY, AREG, BREG, DREG, KIND, IS64) RECPS_FN(TAG, MNEM, TY, AREG, BREG, DREG)
RECPS_CASES(DEFINE_RECPS)

struct RFn {
  const char* name;
  void (*fn)(const V128*, const V128*, V128*);
  int kind; // 0 = frecps, 1 = frsqrts
  int is64;
};

#define ROW_RECPS(TAG, MNEM, TY, AREG, BREG, DREG, KIND, IS64) {#TAG, rfn_##TAG, KIND, IS64},
static const struct RFn RFns[] = {RECPS_CASES(ROW_RECPS)};

static int all_lanes_nan(const V128* v, int is64) {
  if (is64) {
    for (int i = 0; i < 2; ++i) {
      uint64_t d;
      memcpy(&d, v->b + 8 * i, 8);
      if ((d & 0x7FF0000000000000ull) != 0x7FF0000000000000ull || (d & 0x000FFFFFFFFFFFFFull) == 0) {
        return 0;
      }
    }
  } else {
    for (int i = 0; i < 4; ++i) {
      uint32_t w;
      memcpy(&w, v->b + 4 * i, 4);
      if ((w & 0x7F800000u) != 0x7F800000u || (w & 0x007FFFFFu) == 0) {
        return 0;
      }
    }
  }
  return 1;
}

int main(void) {
  for (unsigned p = 0; p < sizeof(Pairs) / sizeof(Pairs[0]); ++p) {
    const struct Pair* pr = &Pairs[p];
    const V128 a32 = splat32(pr->a32), b32 = splat32(pr->b32);
    const V128 a64 = splat64(pr->a64), b64 = splat64(pr->b64);

    for (unsigned f = 0; f < sizeof(CmpFns) / sizeof(CmpFns[0]); ++f) {
      const struct CmpFn* k = &CmpFns[f];
      const V128* a = k->is64 ? &a64 : &a32;
      const V128* b = k->is64 ? &b64 : &b32;
      V128 got;
      memset(got.b, 0xA5, sizeof(got.b));
      k->fn(a, b, &got);
      const V128 want = k->is64 ? mask64(pr->want[k->which]) : mask32(pr->want[k->which]);
      char what[96];
      snprintf(what, sizeof(what), "%s %s", pr->name, k->name);
      expect(what, &got, &want);
    }
  }

  // FRECPS / FRSQRTS: the exact arithmetic answers, then NaN in -> NaN out.
  {
    // frecps(2,1) = 2 - 2*1 = 0 ; frsqrts(3,1) = (3 - 3*1)/2 = 0
    const uint32_t three32 = 0x40400000u;
    const uint64_t three64 = 0x4008000000000000ull;
    for (unsigned f = 0; f < sizeof(RFns) / sizeof(RFns[0]); ++f) {
      const struct RFn* k = &RFns[f];
      const int rs = k->kind; // frsqrts needs a=3 for a zero result
      const V128 a32 = splat32(rs ? three32 : F32_TWO), b32 = splat32(F32_ONE);
      const V128 a64 = splat64(rs ? three64 : F64_TWO), b64 = splat64(F64_ONE);
      V128 got;
      memset(got.b, 0xA5, sizeof(got.b));
      k->fn(k->is64 ? &a64 : &a32, k->is64 ? &b64 : &b32, &got);
      const V128 want = k->is64 ? splat64(F64_POS_ZERO) : splat32(F32_POS_ZERO);
      char what[96];
      snprintf(what, sizeof(what), "%s exact-zero", k->name);
      expect(what, &got, &want);
    }

    // A NaN operand must come back as a NaN, whichever side it is on. This is
    // the arm that reaches VFCMPUNO.
    for (unsigned f = 0; f < sizeof(RFns) / sizeof(RFns[0]); ++f) {
      const struct RFn* k = &RFns[f];
      for (int side = 0; side < 2; ++side) {
        const V128 a32 = splat32(side ? F32_ONE : F32_QNAN);
        const V128 b32 = splat32(side ? F32_QNAN : F32_ONE);
        const V128 a64 = splat64(side ? F64_ONE : F64_QNAN);
        const V128 b64 = splat64(side ? F64_QNAN : F64_ONE);
        V128 got;
        memset(got.b, 0xA5, sizeof(got.b));
        k->fn(k->is64 ? &a64 : &a32, k->is64 ? &b64 : &b32, &got);
        ++Checks;
        if (!all_lanes_nan(&got, k->is64)) {
          ++Failures;
          printf("FAIL %s nan-side%d: result is not a NaN in every lane\n  got ", k->name, side);
          for (int i = 0; i < 16; ++i) {
            printf("%02x", got.b[i]);
          }
          printf("\n");
        }
      }
    }
  }

  // The checker's own control: a deliberately corrupted copy MUST be caught,
  // and the NaN recogniser must reject a non-NaN. Without these, a
  // comparator that always agreed would make every line above meaningless.
  // Counted separately so they contribute no FAIL line.
  {
    int ok = 1;
    V128 a = mask32(1);
    V128 b = a;
    b.b[5] ^= 0x01;
    if (memcmp(a.b, b.b, 16) == 0) {
      ok = 0;
    }
    const V128 one = splat32(F32_ONE);
    const V128 nan = splat32(F32_QNAN);
    if (all_lanes_nan(&one, 0) || !all_lanes_nan(&nan, 0)) {
      ok = 0;
    }
    const V128 one64 = splat64(F64_ONE);
    const V128 nan64 = splat64(F64_QNAN);
    if (all_lanes_nan(&one64, 1) || !all_lanes_nan(&nan64, 1)) {
      ok = 0;
    }
    if (ok) {
      printf("PASS control-detects-corruption\n");
    } else {
      printf("FAIL control-detects-corruption: comparator or NaN recogniser is blind\n");
      ++Failures;
    }
  }

  printf("checks %d failures %d\n", Checks, Failures);
  return Failures ? 1 : 0;
}
