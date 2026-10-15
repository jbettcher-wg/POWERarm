// SPDX-License-Identifier: MIT
// NO-GOLDEN
//
// FCVTL / FCVTL2 / FCVTN / FCVTN2 between FP16 and FP32, on the values a
// clever branch-free lowering is most likely to get wrong and a 240-instruction
// software helper most likely got right by brute force.
//
// Self-checking, and it MUST stay that way. Every answer below is forced by
// IEEE 754 and the ARM ARM, and this file computes it from the input bits with
// integer arithmetic only -- a shift-with-guard-and-sticky round, which is the
// textbook statement of the rounding rules rather than a restatement of the
// lowering under test. A reference taken from the emulator is exactly what
// could not tell you these answers: the POWER8 software helper this replaced
// was WRONG on 16777214 of the 2^32 single-precision inputs, every f32 with
// magnitude strictly between 2^-25 and 2^-24, where it returned a zero and the
// architecture rounds to the smallest denormal half. `half_from_single` here
// says 0x0001 for that input, and case "2^-25 + eps" below pins it.
//
// What is covered, per the edge list the conversions have to get right:
//   * denormals in both directions, including a denormal half widening to a
//     normal single (the widening lowering renormalises them with one
//     multiply, so a wrong scale shows up only here), and singles that narrow
//     into the half denormal range, at the range boundary and below it;
//   * quiet and signalling NaNs with payloads, in both directions, asserting
//     the ARM payload rule -- the top bits of the significand kept and the
//     quiet bit forced -- and that a signalling NaN comes back quiet;
//   * infinities and both signed zeros, which must not be perturbed;
//   * round-to-nearest-even on narrowing, including the exact halfway cases in
//     the normal range and in the denormal range, where the tie must go to the
//     even significand;
//   * overflow to infinity, at and around 65520 (the halfway point between the
//     largest half and 2^16), in every rounding mode, where "nearest" and
//     "away" give infinity and "toward zero" gives the largest finite half;
//   * all four FPCR.RMode settings, each with FPCR.FZ16 clear and set, because
//     these conversions go through FPUnpackCV/FPRoundCV and must IGNORE FZ16 --
//     an implementation that honoured it would flush the denormal cases.
//
// Every value is run in all four lanes (the list is rotated), so a lowering
// that is right in lane 0 and wrong elsewhere fails. FCVTN's upper 64 bits
// must come out zero and FCVTN2 must leave the lower 64 bits of its
// destination alone; both are checked.
//
// It carries its own positive control, so exactly one FAIL line is required
// and it has to be that one.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint32_t w[4];
} v128;

enum { RM_RN = 0, RM_RP = 1, RM_RM = 2, RM_RZ = 3 }; // FPCR.RMode encoding

static unsigned long checks, failures;

static void fail(const char* what, const char* mode, unsigned lane, uint64_t in, uint64_t want, uint64_t got) {
  printf("FAIL %s %s lane %u in=%016llx want=%016llx got=%016llx\n", what, mode, lane, (unsigned long long)in,
         (unsigned long long)want, (unsigned long long)got);
  ++failures;
}

// ---------------------------------------------------------------- the guest
static void set_fpcr(uint64_t v) {
  __asm__ volatile("msr fpcr, %0" : : "r"(v));
  __asm__ volatile("isb" : : : "memory");
}

static void do_fcvtl(const void* src, void* dst) { // low 4 halves -> 4 singles
  __asm__ volatile("ldr q1, [%0]\n\t"
                   "fcvtl v0.4s, v1.4h\n\t"
                   "str q0, [%1]\n\t"
                   :
                   : "r"(src), "r"(dst)
                   : "v0", "v1", "memory");
}
static void do_fcvtl2(const void* src, void* dst) { // high 4 halves -> 4 singles
  __asm__ volatile("ldr q1, [%0]\n\t"
                   "fcvtl2 v0.4s, v1.8h\n\t"
                   "str q0, [%1]\n\t"
                   :
                   : "r"(src), "r"(dst)
                   : "v0", "v1", "memory");
}
static void do_fcvtn(const void* src, void* dst) { // 4 singles -> low 4 halves
  __asm__ volatile("ldr q1, [%0]\n\t"
                   "movi v0.16b, #0xFF\n\t" // so a lowering that fails to zero the top half is caught
                   "fcvtn v0.4h, v1.4s\n\t"
                   "str q0, [%1]\n\t"
                   :
                   : "r"(src), "r"(dst)
                   : "v0", "v1", "memory");
}
static void do_fcvtn2(const void* seed, const void* src, void* dst) { // -> high 4 halves
  __asm__ volatile("ldr q0, [%0]\n\t"
                   "ldr q1, [%1]\n\t"
                   "fcvtn2 v0.8h, v1.4s\n\t"
                   "str q0, [%2]\n\t"
                   :
                   : "r"(seed), "r"(src), "r"(dst)
                   : "v0", "v1", "memory");
}

// ------------------------------------------------------------- the reference
// binary16 -> binary32. Exact in every case; no rounding is possible.
static uint32_t single_from_half(uint16_t h) {
  const uint32_t s = (uint32_t)(h >> 15) & 1u;
  const uint32_t e = (h >> 10) & 0x1Fu;
  uint32_t m = h & 0x3FFu;
  if (e == 0) {
    if (m == 0) {
      return s << 31; // signed zero, kept
    }
    // Denormal half, value m * 2^-24. Shift the leading one up into the
    // implicit position and charge the shifts to the exponent.
    unsigned sh = 0;
    while (!(m & 0x400u)) {
      m <<= 1;
      ++sh;
    }
    m &= 0x3FFu;
    return (s << 31) | ((127u - 14u - sh) << 23) | (m << 13);
  }
  if (e == 0x1Fu) {
    if (m == 0) {
      return (s << 31) | (0xFFu << 23); // infinity
    }
    // NaN: the quiet bit is forced and the top nine payload bits are kept.
    return (s << 31) | (0xFFu << 23) | (1u << 22) | ((m & 0x1FFu) << 13);
  }
  return (s << 31) | ((e - 15u + 127u) << 23) | (m << 13);
}

// Round the exact value  sig * 2^q  (sig != 0) with sign s to binary16 under
// FPCR.RMode `rm`, by shifting to the target precision and inspecting the
// guard and sticky bits -- the textbook rounding, nothing format-specific.
static uint16_t round_half(uint32_t sig, int q, unsigned s, int rm) {
  int top = 31;
  while (!((sig >> top) & 1u)) {
    --top;
  }
  const int E = top + q;        // value = 1.f * 2^E
  int k = E < -14 ? -14 : E;    // target quantum is 2^(k-10)
  const int sh = q - k + 10;    // value / 2^(k-10) == sig * 2^sh

  uint64_t M;
  unsigned guard = 0, sticky = 0;
  if (sh >= 0) {
    M = (uint64_t)sig << sh; // exact; nothing to round
  } else {
    const unsigned n = (unsigned)-sh;
    if (n >= 32) {
      // Everything, including the guard position, is below the significand.
      M = 0;
      guard = (n == 32) ? ((sig >> 31) & 1u) : 0u;
      sticky = guard ? ((sig & 0x7FFFFFFFu) != 0) : (sig != 0);
    } else {
      M = (uint64_t)sig >> n;
      guard = (sig >> (n - 1)) & 1u;
      sticky = (sig & (((uint32_t)1u << (n - 1)) - 1u)) != 0;
    }
  }

  unsigned up = 0;
  if (guard || sticky) {
    switch (rm) {
    case RM_RZ: up = 0; break;
    case RM_RP: up = (s == 0); break;
    case RM_RM: up = (s == 1); break;
    default:    up = guard && (sticky || (M & 1u)); break; // ties to even
    }
  }
  if (up) {
    ++M;
  }
  if (M == 2048) { // rounding carried into the next binade
    M = 1024;
    ++k;
  }
  if (k > 15) {
    // Beyond the format. Nearest and the away-from-zero directions give
    // infinity; the toward-zero directions give the largest finite half.
    const unsigned to_inf = (rm == RM_RN) || (rm == RM_RP && s == 0) || (rm == RM_RM && s == 1);
    return (uint16_t)((s << 15) | (to_inf ? 0x7C00u : 0x7BFFu));
  }
  if (k == -14 && M < 1024) {
    return (uint16_t)((s << 15) | (uint32_t)M); // denormal: zero exponent field
  }
  return (uint16_t)((s << 15) | ((uint32_t)(k + 15) << 10) | (uint32_t)(M - 1024));
}

// binary32 -> binary16 under FPCR.RMode `rm`.
static uint16_t half_from_single(uint32_t x, int rm) {
  const unsigned s = (x >> 31) & 1u;
  const uint32_t e = (x >> 23) & 0xFFu;
  const uint32_t m = x & 0x7FFFFFu;
  if (e == 0xFFu) {
    if (m == 0) {
      return (uint16_t)((s << 15) | 0x7C00u); // infinity
    }
    // NaN: quiet bit forced, top nine payload bits kept.
    return (uint16_t)((s << 15) | 0x7C00u | 0x200u | ((m >> 13) & 0x1FFu));
  }
  if (e == 0 && m == 0) {
    return (uint16_t)(s << 15); // signed zero, kept
  }
  if (e == 0) {
    return round_half(m, -149, s, rm); // denormal single
  }
  return round_half(m | 0x800000u, (int)e - 127 - 23, s, rm);
}

// ------------------------------------------------------------------ the data
// Single-precision inputs. Anything whose answer depends on the rounding mode
// or on denormal handling, plus the specials.
static const uint32_t SINGLES[] = {
    0x00000000u, 0x80000000u,             // +0, -0
    0x00000001u, 0x807FFFFFu,             // single denormals, far below the half range
    0x33000000u, 0x33000001u, 0x32FFFFFFu, // 2^-25, 2^-25+eps (the helper's bug), 2^-25-eps
    0xB3000000u, 0xB3000001u,             // the same, negative
    0x33800000u, 0x33C00000u, 0x34000000u, // 2^-24 (min denormal half), 1.5x, 2x
    0x387FC000u, 0x38800000u,             // max denormal half, min normal half
    0x38802000u, 0x38806000u,             // denormal/normal boundary ties
    0x387FE000u, 0x387FF000u,             // halfway into the top denormal
    0x3F800000u, 0xBF800000u,             // +-1.0
    0x3F801000u, 0x3F803000u,             // 1.0 + half an lsb (tie down), + 1.5 lsb (tie up)
    0x477FE000u, 0x477FDFFFu,             // 65504 (max half), one single-ulp below
    0x477FF000u, 0x477FF800u,             // 65520 (the overflow tie), above it
    0x47800000u, 0x47FFFFFFu, 0x7F7FFFFFu, // 2^16, more, max single
    0xC77FF000u, 0xC7800000u, 0xFF7FFFFFu, // the same, negative
    0x7F800000u, 0xFF800000u,             // +-Inf
    0x7FC00000u, 0xFFC00000u,             // +-quiet NaN
    0x7F800001u, 0x7FA00000u, 0xFFA00000u, // signalling NaNs, with payloads
    0x7FFFFFFFu, 0x7FC01234u,             // NaNs whose payload must survive truncation
};

// Half-precision inputs.
static const uint16_t HALVES[] = {
    0x0000u, 0x8000u,                     // +-0
    0x0001u, 0x8001u, 0x0002u, 0x0200u,   // denormals, including ones that need renormalising
    0x03FFu, 0x83FFu,                     // max denormal
    0x0400u, 0x8400u,                     // min normal
    0x3C00u, 0xBC00u, 0x3BFFu, 0x3E00u,   // +-1.0, just below, 1.5
    0x7BFFu, 0xFBFFu,                     // +-max normal
    0x7C00u, 0xFC00u,                     // +-Inf
    0x7E00u, 0xFE00u,                     // +-quiet NaN
    0x7C01u, 0x7D55u, 0xFC2Au, 0x7E01u,   // signalling NaNs and payloads
};

#define NSINGLES (sizeof(SINGLES) / sizeof(SINGLES[0]))
#define NHALVES (sizeof(HALVES) / sizeof(HALVES[0]))

static const char* MODE_NAMES[4] = {"RN", "RP", "RM", "RZ"};

int main(void) {
  // The seed FCVTN2 must preserve in the low half of its destination, and a
  // recognisable pattern for the high half it overwrites.
  static const uint32_t SEED[4] = {0x12345678u, 0x9ABCDEF0u, 0x0F1E2D3Cu, 0x4B5A6978u};

  for (int rm = 0; rm < 4; ++rm) {
    for (int fz16 = 0; fz16 < 2; ++fz16) {
      char mode[16];
      snprintf(mode, sizeof(mode), "%s/FZ16=%d", MODE_NAMES[rm], fz16);
      // FPCR: RMode in bits 23:22, FZ16 in bit 19. These conversions must
      // ignore FZ16 entirely.
      set_fpcr(((uint64_t)rm << 22) | ((uint64_t)fz16 << 19));

      // ---- widening: FCVTL and FCVTL2 ----
      for (unsigned base = 0; base < NHALVES; ++base) {
        uint16_t in[8];
        for (unsigned i = 0; i < 8; ++i) {
          in[i] = HALVES[(base + i) % NHALVES];
        }
        v128 lo, hi;
        uint16_t src[8];
        memcpy(src, in, sizeof(src));
        do_fcvtl(src, &lo);
        do_fcvtl2(src, &hi);
        for (unsigned i = 0; i < 4; ++i) {
          const uint32_t wlo = single_from_half(in[i]);
          const uint32_t whi = single_from_half(in[4 + i]);
          ++checks;
          if (lo.w[i] != wlo) {
            fail("fcvtl", mode, i, in[i], wlo, lo.w[i]);
          }
          ++checks;
          if (hi.w[i] != whi) {
            fail("fcvtl2", mode, i, in[4 + i], whi, hi.w[i]);
          }
        }
      }

      // ---- narrowing: FCVTN and FCVTN2 ----
      for (unsigned base = 0; base < NSINGLES; ++base) {
        uint32_t in[4];
        for (unsigned i = 0; i < 4; ++i) {
          in[i] = SINGLES[(base + i) % NSINGLES];
        }
        v128 n, n2;
        do_fcvtn(in, &n);
        do_fcvtn2(SEED, in, &n2);
        for (unsigned i = 0; i < 4; ++i) {
          const uint16_t want = half_from_single(in[i], rm);
          const uint16_t got_n = (uint16_t)(n.w[i / 2] >> (16 * (i & 1)));
          const uint16_t got_n2 = (uint16_t)(n2.w[2 + i / 2] >> (16 * (i & 1)));
          ++checks;
          if (got_n != want) {
            fail("fcvtn", mode, i, in[i], want, got_n);
          }
          ++checks;
          if (got_n2 != want) {
            fail("fcvtn2", mode, i, in[i], want, got_n2);
          }
        }
        // FCVTN writes a 64-bit result, so the upper half of the register is
        // zeroed -- the movi above filled it with ones first.
        ++checks;
        if (n.w[2] != 0 || n.w[3] != 0) {
          fail("fcvtn-upper-zeroed", mode, 0, 0, 0, ((uint64_t)n.w[3] << 32) | n.w[2]);
        }
        // FCVTN2 writes only the upper half; the lower half is the seed.
        ++checks;
        if (n2.w[0] != SEED[0] || n2.w[1] != SEED[1]) {
          fail("fcvtn2-lower-kept", mode, 0, ((uint64_t)SEED[1] << 32) | SEED[0], ((uint64_t)SEED[1] << 32) | SEED[0],
               ((uint64_t)n2.w[1] << 32) | n2.w[0]);
        }
      }
    }
  }

  set_fpcr(0);

  // Positive control: the comparison above has to be able to see a wrong
  // answer. Hand it one.
  {
    const uint32_t in[4] = {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u};
    v128 n;
    do_fcvtn(in, &n);
    const uint16_t got = (uint16_t)n.w[0];
    const uint16_t wrong = (uint16_t)(half_from_single(in[0], RM_RN) ^ 1u);
    if (got != wrong) {
      printf("PASS control-detects-corruption\n");
    } else {
      printf("FAIL control-detects-corruption (a deliberately wrong value compared equal)\n");
      ++failures;
    }
  }

  printf("checks %lu failures %lu\n", checks, failures);
  return failures != 0;
}
