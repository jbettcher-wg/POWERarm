// SPDX-License-Identifier: MIT
//
// LD1R-LD4R (load one element and replicate it into every lane).
//
// Self-checking, so it needs no Pi golden: every lane of the result is
// required to equal the element that was loaded, which the test knows without
// a reference machine. The checker is itself checked -- see
// `control-detects-corruption` at the end, which proves a wrong buffer is
// actually reported rather than passed over.
//
// Why this exists: the whole LD1R family had NO test anywhere in the suite,
// in any form, while the backend lowering for it changed three times. It is
// now one instruction (lxvdsx for an 8-byte element on both ISA levels,
// lxvwsx for a 4-byte element on ISA 3.0), and the properties that lowering
// has to keep are exactly the ones a replicate-from-memory can get wrong:
//
//   * The EFFECTIVE ADDRESS IS NOT TRUNCATED. lvx and stvx mask EA to a
//     16-byte boundary; lxvdsx and lxvwsx do not. Every case below is run at
//     all 16 offsets from a 16-byte-aligned base, so a lowering that
//     reintroduced masking fails at 15 of the 16.
//   * ONLY the element's own bytes are read. A 4-byte LD1R lowered as an
//     8-byte load would work everywhere except at the end of a mapping --
//     so the last case puts the element flush against an unmapped page and
//     requires no fault. That is the one property that distinguishes lxvwsx
//     from widening to lxvdsx, and it is invisible to every other test.
//   * A 64-bit (non-Q) form ZEROES the upper half of the register.
//   * The register bank does not matter. Guest V16-V31 are pinned into the
//     low half of the host VSR file, which VMX-form instructions cannot name,
//     so the lowering differs between banks; every case runs in both.
//   * LD2R-LD4R walk CONSECUTIVE elements into CONSECUTIVE registers, and
//     wrap v31 -> v0.
//   * A post-indexed form updates the base register by the total size read.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int Failures = 0;
static int Checks = 0;

// A result buffer holds a whole 128-bit register; `str q` always writes 16
// bytes, so the upper half of a 64-bit form is checked rather than ignored.
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

// The expected image of a replicate: `elem` bytes from `src`, repeated until
// `fill` bytes are covered, then zeroes to 16.
static V128 replicate(const uint8_t* src, int elem, int fill) {
  V128 v;
  memset(v.b, 0, sizeof(v.b));
  for (int i = 0; i < fill; ++i) {
    v.b[i] = src[i % elem];
  }
  return v;
}

// One case per (element type, register). The register number is textual
// because the bank is the thing under test: a `v17` here is guest V17, which
// the backend keeps pinned in the host's low VSR bank.
#define LD1R_CASE(NAME, REG, TYPE, ELEM, FILL)                                                 \
  static void ld1r_##NAME(const uint8_t* src, V128* out) {                                     \
    asm volatile("ld1r {v" #REG "." TYPE "}, [%0]\n\t"                                         \
                 "str q" #REG ", [%1]\n\t"                                                     \
                 :                                                                             \
                 : "r"(src), "r"(out)                                                          \
                 : "memory", "v" #REG);                                                        \
  }

// Low bank (V0-V15) and high bank (V16-V31) for every element type and both
// Q values. 16b/8h/4s/2d are the full-width forms; 8b/4h/2s/1d are the
// 64-bit forms whose upper half must come back zero.
LD1R_CASE(b16_lo, 3, "16b", 1, 16)
LD1R_CASE(b16_hi, 19, "16b", 1, 16)
LD1R_CASE(b8_lo, 3, "8b", 1, 8)
LD1R_CASE(b8_hi, 19, "8b", 1, 8)
LD1R_CASE(h8_lo, 4, "8h", 2, 16)
LD1R_CASE(h8_hi, 20, "8h", 2, 16)
LD1R_CASE(h4_lo, 4, "4h", 2, 8)
LD1R_CASE(h4_hi, 20, "4h", 2, 8)
LD1R_CASE(s4_lo, 5, "4s", 4, 16)
LD1R_CASE(s4_hi, 21, "4s", 4, 16)
LD1R_CASE(s2_lo, 5, "2s", 4, 8)
LD1R_CASE(s2_hi, 21, "2s", 4, 8)
LD1R_CASE(d2_lo, 6, "2d", 8, 16)
LD1R_CASE(d2_hi, 22, "2d", 8, 16)
LD1R_CASE(d1_lo, 6, "1d", 8, 8)
LD1R_CASE(d1_hi, 22, "1d", 8, 8)

struct Case {
  const char* name;
  void (*fn)(const uint8_t*, V128*);
  int elem;
  int fill;
};

static const struct Case Cases[] = {
  {"ld1r_16b_v3", ld1r_b16_lo, 1, 16},  {"ld1r_16b_v19", ld1r_b16_hi, 1, 16},
  {"ld1r_8b_v3", ld1r_b8_lo, 1, 8},     {"ld1r_8b_v19", ld1r_b8_hi, 1, 8},
  {"ld1r_8h_v4", ld1r_h8_lo, 2, 16},    {"ld1r_8h_v20", ld1r_h8_hi, 2, 16},
  {"ld1r_4h_v4", ld1r_h4_lo, 2, 8},     {"ld1r_4h_v20", ld1r_h4_hi, 2, 8},
  {"ld1r_4s_v5", ld1r_s4_lo, 4, 16},    {"ld1r_4s_v21", ld1r_s4_hi, 4, 16},
  {"ld1r_2s_v5", ld1r_s2_lo, 4, 8},     {"ld1r_2s_v21", ld1r_s2_hi, 4, 8},
  {"ld1r_2d_v6", ld1r_d2_lo, 8, 16},    {"ld1r_2d_v22", ld1r_d2_hi, 8, 16},
  {"ld1r_1d_v6", ld1r_d1_lo, 8, 8},     {"ld1r_1d_v22", ld1r_d1_hi, 8, 8},
};

// LD2R-LD4R: consecutive elements into consecutive registers. The v30-v0 case
// is the register-number wrap.
static void ld2r_s(const uint8_t* src, V128* out) {
  asm volatile("ld2r {v7.4s, v8.4s}, [%0]\n\t"
               "str q7, [%1]\n\t"
               "str q8, [%1, #16]\n\t"
               :
               : "r"(src), "r"(out)
               : "memory", "v7", "v8");
}

static void ld3r_h(const uint8_t* src, V128* out) {
  asm volatile("ld3r {v23.8h, v24.8h, v25.8h}, [%0]\n\t"
               "str q23, [%1]\n\t"
               "str q24, [%1, #16]\n\t"
               "str q25, [%1, #32]\n\t"
               :
               : "r"(src), "r"(out)
               : "memory", "v23", "v24", "v25");
}

static void ld4r_d(const uint8_t* src, V128* out) {
  asm volatile("ld4r {v28.2d, v29.2d, v30.2d, v31.2d}, [%0]\n\t"
               "str q28, [%1]\n\t"
               "str q29, [%1, #16]\n\t"
               "str q30, [%1, #32]\n\t"
               "str q31, [%1, #48]\n\t"
               :
               : "r"(src), "r"(out)
               : "memory", "v28", "v29", "v30", "v31");
}

static void ld4r_wrap_b(const uint8_t* src, V128* out) {
  asm volatile("ld4r {v30.16b, v31.16b, v0.16b, v1.16b}, [%0]\n\t"
               "str q30, [%1]\n\t"
               "str q31, [%1, #16]\n\t"
               "str q0, [%1, #32]\n\t"
               "str q1, [%1, #48]\n\t"
               :
               : "r"(src), "r"(out)
               : "memory", "v30", "v31", "v0", "v1");
}

int main(void) {
  // A 16-byte-aligned window with an asymmetric pattern: every byte distinct,
  // so a wrong lane, a wrong element or a byte-swap all show up.
  static _Alignas(16) uint8_t Pool[128];
  for (unsigned i = 0; i < sizeof(Pool); ++i) {
    Pool[i] = (uint8_t)(0x31 + i * 7);
  }

  // Every case at every offset 0..15 from the aligned base. An EA masked to
  // the 16-byte boundary reads Pool[0..] whatever the offset, so 15 of these
  // 16 would come back wrong.
  for (unsigned c = 0; c < sizeof(Cases) / sizeof(Cases[0]); ++c) {
    for (int off = 0; off < 16; ++off) {
      const uint8_t* src = Pool + off;
      V128 got;
      memset(got.b, 0xA5, sizeof(got.b));
      Cases[c].fn(src, &got);
      const V128 want = replicate(src, Cases[c].elem, Cases[c].fill);
      char what[64];
      snprintf(what, sizeof(what), "%s off=%d", Cases[c].name, off);
      expect(what, &got, &want);
    }
  }

  for (int off = 0; off < 16; ++off) {
    const uint8_t* src = Pool + off;
    V128 got[4];

    memset(got, 0xA5, sizeof(got));
    ld2r_s(src, got);
    for (int s = 0; s < 2; ++s) {
      const V128 want = replicate(src + s * 4, 4, 16);
      char what[64];
      snprintf(what, sizeof(what), "ld2r_4s[%d] off=%d", s, off);
      expect(what, &got[s], &want);
    }

    memset(got, 0xA5, sizeof(got));
    ld3r_h(src, got);
    for (int s = 0; s < 3; ++s) {
      const V128 want = replicate(src + s * 2, 2, 16);
      char what[64];
      snprintf(what, sizeof(what), "ld3r_8h[%d] off=%d", s, off);
      expect(what, &got[s], &want);
    }

    memset(got, 0xA5, sizeof(got));
    ld4r_d(src, got);
    for (int s = 0; s < 4; ++s) {
      const V128 want = replicate(src + s * 8, 8, 16);
      char what[64];
      snprintf(what, sizeof(what), "ld4r_2d[%d] off=%d", s, off);
      expect(what, &got[s], &want);
    }

    memset(got, 0xA5, sizeof(got));
    ld4r_wrap_b(src, got);
    for (int s = 0; s < 4; ++s) {
      const V128 want = replicate(src + s, 1, 16);
      char what[64];
      snprintf(what, sizeof(what), "ld4r_16b_wrap[%d] off=%d", s, off);
      expect(what, &got[s], &want);
    }
  }

  // Post-index: the base register must advance by the total bytes read, and
  // the replicate must still be right. `ld1r {v.4s}, [x], #4` reads 4 and
  // `ld4r {v.2d,...}, [x], #32` reads 32.
  {
    const uint8_t* src = Pool + 3;
    V128 got;
    const uint8_t* after = NULL;
    memset(got.b, 0xA5, sizeof(got.b));
    asm volatile("mov x9, %1\n\t"
                 "ld1r {v9.4s}, [x9], #4\n\t"
                 "str q9, [%2]\n\t"
                 "mov %0, x9\n\t"
                 : "=r"(after)
                 : "r"(src), "r"(&got)
                 : "memory", "x9", "v9");
    const V128 want = replicate(src, 4, 16);
    expect("ld1r_4s_postindex", &got, &want);
    ++Checks;
    if (after != src + 4) {
      ++Failures;
      printf("FAIL ld1r_4s_postindex base: got +%ld want +4\n", (long)(after - src));
    }
  }
  {
    const uint8_t* src = Pool + 5;
    V128 got[4];
    const uint8_t* after = NULL;
    memset(got, 0xA5, sizeof(got));
    asm volatile("mov x9, %1\n\t"
                 "ld4r {v24.2d, v25.2d, v26.2d, v27.2d}, [x9], #32\n\t"
                 "str q24, [%2]\n\t"
                 "str q25, [%2, #16]\n\t"
                 "str q26, [%2, #32]\n\t"
                 "str q27, [%2, #48]\n\t"
                 "mov %0, x9\n\t"
                 : "=r"(after)
                 : "r"(src), "r"(got)
                 : "memory", "x9", "v24", "v25", "v26", "v27");
    for (int s = 0; s < 4; ++s) {
      const V128 want = replicate(src + s * 8, 8, 16);
      char what[64];
      snprintf(what, sizeof(what), "ld4r_2d_postindex[%d]", s);
      expect(what, &got[s], &want);
    }
    ++Checks;
    if (after != src + 32) {
      ++Failures;
      printf("FAIL ld4r_2d_postindex base: got +%ld want +32\n", (long)(after - src));
    }
  }

  // THE ONE THAT PINS THE INSTRUCTION CHOICE. Two pages, the second unmapped,
  // with each element flush against the end of the first. A lowering that
  // reads WIDER than the element -- an 8-byte load for a 4-byte LD1R, or a
  // 16-byte one for any of them -- touches the hole and takes SIGSEGV here
  // while passing every other case in this file.
  {
    const long PageSize = sysconf(_SC_PAGESIZE);
    uint8_t* two = mmap(NULL, (size_t)PageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (two == MAP_FAILED) {
      printf("FAIL edge mmap\n");
      ++Failures;
    } else if (munmap(two + PageSize, (size_t)PageSize) != 0) {
      printf("FAIL edge munmap\n");
      ++Failures;
    } else {
      uint8_t* end = two + PageSize;
      for (unsigned c = 0; c < sizeof(Cases) / sizeof(Cases[0]); ++c) {
        const int elem = Cases[c].elem;
        const uint8_t* src = end - elem;
        for (int i = 0; i < elem; ++i) {
          ((uint8_t*)src)[i] = (uint8_t)(0xC1 + i * 11);
        }
        V128 got;
        memset(got.b, 0xA5, sizeof(got.b));
        Cases[c].fn(src, &got);
        const V128 want = replicate(src, elem, Cases[c].fill);
        char what[80];
        snprintf(what, sizeof(what), "%s at-page-end", Cases[c].name);
        expect(what, &got, &want);
      }
    }
  }

  // The checker's own control: a deliberately corrupted copy MUST be caught.
  // Without this, a comparator that always agreed would make every line above
  // meaningless. Counted separately so it contributes no FAIL line.
  {
    V128 a = replicate(Pool, 4, 16);
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
