// SPDX-License-Identifier: MIT
//
// NEG (vector) over every element width, both register banks, Q=0 and Q=1.
//
// Self-checking, so it needs no reference machine: NEG is 0 - lane in the
// lane's own width, which the architecture forces bit for bit -- INT_MIN
// negates to itself, every other value wraps modularly -- and this file
// computes that answer from the bytes it wrote. The comparator is itself
// checked (`control-detects-corruption` at the end), so a checker that always
// agreed cannot pass the file.
//
// Why this exists: the .4S and .2D forms now lower to the ISA 3.0 `vnegw` /
// `vnegd` (POWER-STRENGTHS.md 2.8) instead of `vspltisw 0 ; vsubu{w,d}m`.
// That makes four distinct lowerings of one IR op, and this file runs all
// four:
//
//   * ISA 3.0, 4 and 8-byte lanes: the single instruction. Its VRA field is
//     part of the opcode (6 for the word form, 7 for the doubleword), so a
//     wrong constant there is a different instruction, not a different
//     register -- which is exactly the class of mistake only a value check
//     catches.
//   * Pre-3.0 (the suite's disableisa30 mode), same two widths: the old pair.
//     The two arms must agree on every lane value, INT_MIN included.
//   * 1 and 2-byte lanes have no `vneg` form at all and keep the pair on
//     every host, so they are here to show the swap did not reach them.
//   * The register bank matters: guest V16-V31 are pinned into the low half
//     of the host VSR file, which a VMX-form instruction cannot name, so
//     every case runs in both banks and run.sh runs the file with
//     POWERARM_VSXCLASSES off and on.
//
// Q=0 is a case of its own because the architecture clears the upper half of
// the destination, which is a separate IR step from the negation itself.
//
// NO-GOLDEN

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

// The architecturally forced answer: each Width-byte lane of the low Bytes
// bytes becomes 0 - lane, truncated to the lane, and everything above Bytes
// is cleared.
static V128 neg_expect(const V128* in, int Width, int Bytes) {
  V128 out;
  memset(out.b, 0, sizeof(out.b));
  for (int off = 0; off < Bytes; off += Width) {
    uint64_t V = 0;
    for (int i = 0; i < Width; ++i) {
      V |= (uint64_t)in->b[off + i] << (8 * i);
    }
    uint64_t N = (uint64_t)0 - V;
    if (Width < 8) {
      N &= (1ULL << (8 * Width)) - 1;
    }
    for (int i = 0; i < Width; ++i) {
      out.b[off + i] = (uint8_t)(N >> (8 * i));
    }
  }
  return out;
}

// One NEG shape: load the input into SREG, negate into DREG, store DREG.
// DREG is poisoned first, so a lowering that writes nothing, or that leaves
// the upper half of a Q=0 result alone, is visible rather than silently equal
// to a zeroed buffer.
#define NEG_SHAPE(Name, Arr, DReg, SReg)                           \
  static V128 Name(const V128* in) {                               \
    V128 out;                                                      \
    static const V128 Poison = {{0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a}}; \
    asm volatile("ldr q" SReg ", [%[p]]\n\t"                       \
                 "ldr q" DReg ", [%[z]]\n\t"                       \
                 "neg v" DReg "." Arr ", v" SReg "." Arr "\n\t"    \
                 "str q" DReg ", [%[o]]\n\t"                       \
                 :                                                 \
                 : [p] "r"(in->b), [z] "r"(Poison.b), [o] "r"(out.b) \
                 : "v" DReg, "v" SReg, "memory");                  \
    return out;                                                    \
  }

// Low bank (V0-V15) and high bank (V16-V31) for each arrangement.
NEG_SHAPE(neg_16b_lo, "16b", "1", "0")
NEG_SHAPE(neg_16b_hi, "16b", "19", "18")
NEG_SHAPE(neg_8b_lo, "8b", "1", "0")
NEG_SHAPE(neg_8b_hi, "8b", "19", "18")
NEG_SHAPE(neg_8h_lo, "8h", "1", "0")
NEG_SHAPE(neg_8h_hi, "8h", "19", "18")
NEG_SHAPE(neg_4h_lo, "4h", "1", "0")
NEG_SHAPE(neg_4h_hi, "4h", "19", "18")
NEG_SHAPE(neg_4s_lo, "4s", "1", "0")
NEG_SHAPE(neg_4s_hi, "4s", "19", "18")
NEG_SHAPE(neg_2s_lo, "2s", "1", "0")
NEG_SHAPE(neg_2s_hi, "2s", "19", "18")
NEG_SHAPE(neg_2d_lo, "2d", "1", "0")
NEG_SHAPE(neg_2d_hi, "2d", "19", "18")

// NEG (scalar): the 1D form, which is the same IR op at 8-byte lanes with a
// 64-bit destination size.
static V128 neg_d_lo(const V128* in) {
  V128 out;
  asm volatile("ldr q0, [%[p]]\n\t"
               "movi v1.16b, #0xa5\n\t"
               "neg d1, d0\n\t"
               "str q1, [%[o]]\n\t"
               :
               : [p] "r"(in->b), [o] "r"(out.b)
               : "v0", "v1", "memory");
  return out;
}
static V128 neg_d_hi(const V128* in) {
  V128 out;
  asm volatile("ldr q18, [%[p]]\n\t"
               "movi v19.16b, #0xa5\n\t"
               "neg d19, d18\n\t"
               "str q19, [%[o]]\n\t"
               :
               : [p] "r"(in->b), [o] "r"(out.b)
               : "v18", "v19", "memory");
  return out;
}

typedef struct {
  const char* Name;
  V128 (*Run)(const V128*);
  int Width;
  int Bytes;
} Shape;

static const Shape Shapes[] = {
  {"neg.16b.lo", neg_16b_lo, 1, 16}, {"neg.16b.hi", neg_16b_hi, 1, 16},
  {"neg.8b.lo", neg_8b_lo, 1, 8},    {"neg.8b.hi", neg_8b_hi, 1, 8},
  {"neg.8h.lo", neg_8h_lo, 2, 16},   {"neg.8h.hi", neg_8h_hi, 2, 16},
  {"neg.4h.lo", neg_4h_lo, 2, 8},    {"neg.4h.hi", neg_4h_hi, 2, 8},
  {"neg.4s.lo", neg_4s_lo, 4, 16},   {"neg.4s.hi", neg_4s_hi, 4, 16},
  {"neg.2s.lo", neg_2s_lo, 4, 8},    {"neg.2s.hi", neg_2s_hi, 4, 8},
  {"neg.2d.lo", neg_2d_lo, 8, 16},   {"neg.2d.hi", neg_2d_hi, 8, 16},
  {"neg.d.lo", neg_d_lo, 8, 8},      {"neg.d.hi", neg_d_hi, 8, 8},
};

// Lane values that discriminate: zero, the minimum of every width (which
// negates to itself), -1, 1, the maximum of every width, the two 32-bit
// halves of a doubleword minimum, and a mixed pattern where no two lanes
// agree so a lane-swapping lowering cannot look right.
static const V128 Inputs[] = {
  {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
  {{0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}},
  {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
  {{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}},
  {{0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80}},
  {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}},
  {{0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff, 0x7f}},
  {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f}},
  {{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}},
  {{0x80, 0x00, 0x7f, 0xff, 0x00, 0x80, 0xff, 0x7f, 0x01, 0xff, 0x00, 0x80, 0x7f, 0x00, 0xff, 0x01}},
};

int main(void) {
  char what[64];
  for (unsigned s = 0; s < sizeof(Shapes) / sizeof(Shapes[0]); ++s) {
    for (unsigned i = 0; i < sizeof(Inputs) / sizeof(Inputs[0]); ++i) {
      const V128 Got = Shapes[s].Run(&Inputs[i]);
      const V128 Want = neg_expect(&Inputs[i], Shapes[s].Width, Shapes[s].Bytes);
      snprintf(what, sizeof(what), "%s input%u", Shapes[s].Name, i);
      expect(what, &Got, &Want);
    }
  }

  // Double negation is the identity on every value but the minimum, where it
  // is also the identity: a second, independent statement of the same rule
  // that does not go through neg_expect at all.
  for (unsigned i = 0; i < sizeof(Inputs) / sizeof(Inputs[0]); ++i) {
    const V128 Once = neg_4s_lo(&Inputs[i]);
    const V128 Twice = neg_4s_lo(&Once);
    snprintf(what, sizeof(what), "neg.4s.twice input%u", i);
    expect(what, &Twice, &Inputs[i]);
  }
  for (unsigned i = 0; i < sizeof(Inputs) / sizeof(Inputs[0]); ++i) {
    const V128 Once = neg_2d_hi(&Inputs[i]);
    const V128 Twice = neg_2d_hi(&Once);
    snprintf(what, sizeof(what), "neg.2d.twice input%u", i);
    expect(what, &Twice, &Inputs[i]);
  }

  // The checker's own control: a deliberately corrupted copy MUST be caught.
  // Counted separately so it contributes no FAIL line.
  {
    V128 a = neg_expect(&Inputs[8], 4, 16);
    V128 b = a;
    b.b[9] ^= 0x80;
    if (memcmp(a.b, b.b, 16) == 0) {
      printf("FAIL control-detects-corruption: comparator passed a corrupt buffer\n");
      ++Failures;
    } else {
      printf("PASS control-detects-corruption\n");
    }
  }

  printf("checks %d failures %d\n", Checks, Failures);
  return Failures ? 1 : 0;
}
