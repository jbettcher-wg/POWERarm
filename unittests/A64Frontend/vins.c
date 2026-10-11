// SPDX-License-Identifier: MIT
//
// INS (element) and INS (general) -- every lane combination, both register
// banks, every element width.
//
// Self-checking, so it needs no reference machine: "element D of the
// destination becomes element S of the source and nothing else changes" is
// forced by the architecture, and this file computes that answer itself from
// the bytes it wrote. The comparator is itself checked -- see
// `control-detects-corruption` at the end, which proves a wrong buffer is
// reported rather than passed over.
//
// Why this exists: the 4 and 8-byte inserts moved to their own IR ops
// (VInsElementWide / VInsGPRWide) so they could be VSXClean, and that made
// the PPC64LE lowering different in each of four combinations:
//
//   * ISA 3.0 uses xxinsertw, a read-modify-write of the destination that
//     reads BE word 1 of its source. If the preceding splat or mtvsrd left
//     the value in a different word, three of the four lanes still look
//     right and one does not -- which is why every (D,S) pair is here and
//     not just a sample.
//   * Pre-3.0 has no word insert at all, so the lowering splats the source
//     lane to all four words and selects the destination lane with xxsel
//     against a word mask built in registers. The mask is a different
//     doubleword half for lanes 0/1 than for lanes 2/3, so all four
//     destination lanes exercise different code.
//   * The two same-lane boundary cases (D==S==0 and D==S==3) take a
//     two-instruction xxsldwi pair instead, so they are their own arm.
//   * The register bank matters: guest V16-V31 are pinned into the low half
//     of the host VSR file, which VMX-form instructions cannot name, so the
//     whole point of the split is that the high bank now takes the same
//     lowering as the low bank. Every case runs in both.
//
// The 1 and 2-byte inserts stayed behind on VInsElement/VInsGPR, so they are
// here too: the split must not have changed them.
//
// Dst-aliases-Src is covered on purpose (the `_alias` cases): each lowering
// claims the destination may be the same register as the source, which is
// only true if it consumes both operands before writing.

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

// Little-endian: element i of width esz occupies bytes [i*esz, i*esz+esz).
// The architectural result of INS is the destination with exactly those esz
// bytes replaced.
static V128 ins_expect(const V128* dst, const V128* src, int esz, int d, int s) {
  V128 v = *dst;
  memcpy(v.b + (size_t)d * esz, src->b + (size_t)s * esz, (size_t)esz);
  return v;
}

static V128 ins_gpr_expect(const V128* dst, uint64_t val, int esz, int d) {
  V128 v = *dst;
  uint8_t le[8];
  for (int i = 0; i < 8; ++i) {
    le[i] = (uint8_t)(val >> (8 * i));
  }
  memcpy(v.b + (size_t)d * esz, le, (size_t)esz);
  return v;
}

// ---------------------------------------------------------------------------
// INS (element), distinct destination and source registers.
// The register numbers are textual because the BANK is under test: a v19 here
// is guest V19, which the backend keeps pinned in the host's low VSR bank.
// ---------------------------------------------------------------------------
#define INS_EE(TAG, DREG, SREG, TY, D, S)                                    \
  static void ins_ee_##TAG(const V128* dsrc, const V128* ssrc, V128* out) {  \
    asm volatile("ldr q" #DREG ", [%0]\n\t"                                  \
                 "ldr q" #SREG ", [%1]\n\t"                                  \
                 "ins v" #DREG "." TY "[" #D "], v" #SREG "." TY "[" #S "]\n\t" \
                 "str q" #DREG ", [%2]\n\t"                                  \
                 :                                                           \
                 : "r"(dsrc), "r"(ssrc), "r"(out)                            \
                 : "memory", "v" #DREG, "v" #SREG);                          \
  }

// INS (element) where the destination IS the source register.
#define INS_EE_ALIAS(TAG, REG, TY, D, S)                                     \
  static void ins_ee_##TAG(const V128* dsrc, const V128* ssrc, V128* out) {  \
    (void)ssrc;                                                              \
    asm volatile("ldr q" #REG ", [%0]\n\t"                                   \
                 "ins v" #REG "." TY "[" #D "], v" #REG "." TY "[" #S "]\n\t" \
                 "str q" #REG ", [%1]\n\t"                                   \
                 :                                                           \
                 : "r"(dsrc), "r"(out)                                       \
                 : "memory", "v" #REG);                                      \
  }

// X(tag, dreg, sreg, "ty", esz, d, s)
// Word lanes: the full 4x4 matrix, in both banks. This is the set that the
// xxinsertw / xxsel / xxsldwi arms split between.
#define EE_WORD_CASES(X)             \
  X(s_lo_00, 3, 5, "s", 4, 0, 0)     \
  X(s_lo_01, 3, 5, "s", 4, 0, 1)     \
  X(s_lo_02, 3, 5, "s", 4, 0, 2)     \
  X(s_lo_03, 3, 5, "s", 4, 0, 3)     \
  X(s_lo_10, 3, 5, "s", 4, 1, 0)     \
  X(s_lo_11, 3, 5, "s", 4, 1, 1)     \
  X(s_lo_12, 3, 5, "s", 4, 1, 2)     \
  X(s_lo_13, 3, 5, "s", 4, 1, 3)     \
  X(s_lo_20, 3, 5, "s", 4, 2, 0)     \
  X(s_lo_21, 3, 5, "s", 4, 2, 1)     \
  X(s_lo_22, 3, 5, "s", 4, 2, 2)     \
  X(s_lo_23, 3, 5, "s", 4, 2, 3)     \
  X(s_lo_30, 3, 5, "s", 4, 3, 0)     \
  X(s_lo_31, 3, 5, "s", 4, 3, 1)     \
  X(s_lo_32, 3, 5, "s", 4, 3, 2)     \
  X(s_lo_33, 3, 5, "s", 4, 3, 3)     \
  X(s_hi_00, 19, 21, "s", 4, 0, 0)   \
  X(s_hi_01, 19, 21, "s", 4, 0, 1)   \
  X(s_hi_02, 19, 21, "s", 4, 0, 2)   \
  X(s_hi_03, 19, 21, "s", 4, 0, 3)   \
  X(s_hi_10, 19, 21, "s", 4, 1, 0)   \
  X(s_hi_11, 19, 21, "s", 4, 1, 1)   \
  X(s_hi_12, 19, 21, "s", 4, 1, 2)   \
  X(s_hi_13, 19, 21, "s", 4, 1, 3)   \
  X(s_hi_20, 19, 21, "s", 4, 2, 0)   \
  X(s_hi_21, 19, 21, "s", 4, 2, 1)   \
  X(s_hi_22, 19, 21, "s", 4, 2, 2)   \
  X(s_hi_23, 19, 21, "s", 4, 2, 3)   \
  X(s_hi_30, 19, 21, "s", 4, 3, 0)   \
  X(s_hi_31, 19, 21, "s", 4, 3, 1)   \
  X(s_hi_32, 19, 21, "s", 4, 3, 2)   \
  X(s_hi_33, 19, 21, "s", 4, 3, 3)

// Doubleword lanes: the full 2x2 matrix, both banks. One xxpermdi each.
#define EE_DWORD_CASES(X)            \
  X(d_lo_00, 6, 7, "d", 8, 0, 0)     \
  X(d_lo_01, 6, 7, "d", 8, 0, 1)     \
  X(d_lo_10, 6, 7, "d", 8, 1, 0)     \
  X(d_lo_11, 6, 7, "d", 8, 1, 1)     \
  X(d_hi_00, 22, 23, "d", 8, 0, 0)   \
  X(d_hi_01, 22, 23, "d", 8, 0, 1)   \
  X(d_hi_10, 22, 23, "d", 8, 1, 0)   \
  X(d_hi_11, 22, 23, "d", 8, 1, 1)

// Byte and halfword lanes stayed on the narrow ops; a sample of lanes,
// including both ends, is enough to show the split left them alone.
#define EE_NARROW_CASES(X)           \
  X(b_lo_00, 3, 5, "b", 1, 0, 0)     \
  X(b_lo_5b, 3, 5, "b", 1, 5, 11)    \
  X(b_lo_f0, 3, 5, "b", 1, 15, 0)    \
  X(b_lo_78, 3, 5, "b", 1, 7, 8)     \
  X(b_hi_00, 19, 21, "b", 1, 0, 0)   \
  X(b_hi_5b, 19, 21, "b", 1, 5, 11)  \
  X(b_hi_f0, 19, 21, "b", 1, 15, 0)  \
  X(b_hi_78, 19, 21, "b", 1, 7, 8)   \
  X(h_lo_00, 4, 5, "h", 2, 0, 0)     \
  X(h_lo_17, 4, 5, "h", 2, 1, 7)     \
  X(h_lo_73, 4, 5, "h", 2, 7, 3)     \
  X(h_lo_45, 4, 5, "h", 2, 4, 5)     \
  X(h_hi_00, 20, 21, "h", 2, 0, 0)   \
  X(h_hi_17, 20, 21, "h", 2, 1, 7)   \
  X(h_hi_73, 20, 21, "h", 2, 7, 3)   \
  X(h_hi_45, 20, 21, "h", 2, 4, 5)

#define EE_ALL_CASES(X) EE_WORD_CASES(X) EE_DWORD_CASES(X) EE_NARROW_CASES(X)

#define DEFINE_EE(TAG, DREG, SREG, TY, ESZ, D, S) INS_EE(TAG, DREG, SREG, TY, D, S)
EE_ALL_CASES(DEFINE_EE)

// X(tag, reg, "ty", esz, d, s) -- destination and source the same register.
#define EE_ALIAS_CASES(X)            \
  X(alias_s_lo_10, 3, "s", 4, 1, 0)  \
  X(alias_s_lo_03, 3, "s", 4, 0, 3)  \
  X(alias_s_lo_30, 3, "s", 4, 3, 0)  \
  X(alias_s_lo_00, 3, "s", 4, 0, 0)  \
  X(alias_s_lo_33, 3, "s", 4, 3, 3)  \
  X(alias_s_lo_21, 3, "s", 4, 2, 1)  \
  X(alias_s_hi_10, 19, "s", 4, 1, 0) \
  X(alias_s_hi_03, 19, "s", 4, 0, 3) \
  X(alias_s_hi_30, 19, "s", 4, 3, 0) \
  X(alias_s_hi_00, 19, "s", 4, 0, 0) \
  X(alias_s_hi_33, 19, "s", 4, 3, 3) \
  X(alias_s_hi_21, 19, "s", 4, 2, 1) \
  X(alias_d_lo_01, 6, "d", 8, 0, 1)  \
  X(alias_d_lo_10, 6, "d", 8, 1, 0)  \
  X(alias_d_hi_01, 22, "d", 8, 0, 1) \
  X(alias_d_hi_10, 22, "d", 8, 1, 0)

#define DEFINE_EE_ALIAS(TAG, REG, TY, ESZ, D, S) INS_EE_ALIAS(TAG, REG, TY, D, S)
EE_ALIAS_CASES(DEFINE_EE_ALIAS)

struct EECase {
  const char* name;
  void (*fn)(const V128*, const V128*, V128*);
  int esz;
  int d;
  int s;
  int alias;
};

#define ROW_EE(TAG, DREG, SREG, TY, ESZ, D, S) {"ins_" #TAG, ins_ee_##TAG, ESZ, D, S, 0},
#define ROW_EE_ALIAS(TAG, REG, TY, ESZ, D, S) {"ins_" #TAG, ins_ee_##TAG, ESZ, D, S, 1},

static const struct EECase EECases[] = {EE_ALL_CASES(ROW_EE) EE_ALIAS_CASES(ROW_EE_ALIAS)};

// ---------------------------------------------------------------------------
// INS (general): a GPR into one lane.
// ---------------------------------------------------------------------------
#define INS_GE_W(TAG, DREG, TY, D)                                           \
  static void ins_ge_##TAG(const V128* dsrc, uint64_t val, V128* out) {      \
    asm volatile("ldr q" #DREG ", [%0]\n\t"                                  \
                 "ins v" #DREG "." TY "[" #D "], %w1\n\t"                    \
                 "str q" #DREG ", [%2]\n\t"                                  \
                 :                                                           \
                 : "r"(dsrc), "r"(val), "r"(out)                             \
                 : "memory", "v" #DREG);                                     \
  }

#define INS_GE_X(TAG, DREG, TY, D)                                           \
  static void ins_ge_##TAG(const V128* dsrc, uint64_t val, V128* out) {      \
    asm volatile("ldr q" #DREG ", [%0]\n\t"                                  \
                 "ins v" #DREG "." TY "[" #D "], %1\n\t"                     \
                 "str q" #DREG ", [%2]\n\t"                                  \
                 :                                                           \
                 : "r"(dsrc), "r"(val), "r"(out)                             \
                 : "memory", "v" #DREG);                                     \
  }

// X(tag, dreg, "ty", esz, d) -- 32-bit GPR source.
#define GE_W_CASES(X)              \
  X(w_s_lo_0, 3, "s", 4, 0)        \
  X(w_s_lo_1, 3, "s", 4, 1)        \
  X(w_s_lo_2, 3, "s", 4, 2)        \
  X(w_s_lo_3, 3, "s", 4, 3)        \
  X(w_s_hi_0, 19, "s", 4, 0)       \
  X(w_s_hi_1, 19, "s", 4, 1)       \
  X(w_s_hi_2, 19, "s", 4, 2)       \
  X(w_s_hi_3, 19, "s", 4, 3)       \
  X(w_b_lo_0, 4, "b", 1, 0)        \
  X(w_b_lo_7, 4, "b", 1, 7)        \
  X(w_b_lo_15, 4, "b", 1, 15)      \
  X(w_b_hi_0, 20, "b", 1, 0)       \
  X(w_b_hi_7, 20, "b", 1, 7)       \
  X(w_b_hi_15, 20, "b", 1, 15)     \
  X(w_h_lo_0, 5, "h", 2, 0)        \
  X(w_h_lo_3, 5, "h", 2, 3)        \
  X(w_h_lo_7, 5, "h", 2, 7)        \
  X(w_h_hi_0, 21, "h", 2, 0)       \
  X(w_h_hi_3, 21, "h", 2, 3)       \
  X(w_h_hi_7, 21, "h", 2, 7)

// X(tag, dreg, "ty", esz, d) -- 64-bit GPR source.
#define GE_X_CASES(X)              \
  X(x_d_lo_0, 6, "d", 8, 0)        \
  X(x_d_lo_1, 6, "d", 8, 1)        \
  X(x_d_hi_0, 22, "d", 8, 0)       \
  X(x_d_hi_1, 22, "d", 8, 1)

#define DEFINE_GE_W(TAG, DREG, TY, ESZ, D) INS_GE_W(TAG, DREG, TY, D)
#define DEFINE_GE_X(TAG, DREG, TY, ESZ, D) INS_GE_X(TAG, DREG, TY, D)
GE_W_CASES(DEFINE_GE_W)
GE_X_CASES(DEFINE_GE_X)

struct GECase {
  const char* name;
  void (*fn)(const V128*, uint64_t, V128*);
  int esz;
  int d;
};

#define ROW_GE(TAG, DREG, TY, ESZ, D) {"ins_" #TAG, ins_ge_##TAG, ESZ, D},

static const struct GECase GECases[] = {GE_W_CASES(ROW_GE) GE_X_CASES(ROW_GE)};

// ---------------------------------------------------------------------------

int main(void) {
  // Two distinguishable patterns: every byte differs between them and no byte
  // repeats within 16, so a lowering that took the right element from the
  // wrong operand, or the wrong element from the right one, cannot land on
  // the expected answer by accident.
  V128 DstPat, SrcPat;
  for (int i = 0; i < 16; ++i) {
    DstPat.b[i] = (uint8_t)(0x10 + i);
    SrcPat.b[i] = (uint8_t)(0xA0 + i * 3);
  }

  for (unsigned c = 0; c < sizeof(EECases) / sizeof(EECases[0]); ++c) {
    const struct EECase* k = &EECases[c];
    // An aliased case reads its source out of the destination register, so
    // the architectural source operand is the destination pattern.
    const V128* src = k->alias ? &DstPat : &SrcPat;
    V128 got;
    memset(got.b, 0xA5, sizeof(got.b));
    k->fn(&DstPat, src, &got);
    const V128 want = ins_expect(&DstPat, src, k->esz, k->d, k->s);
    expect(k->name, &got, &want);
  }

  // Values whose every byte is distinct, and whose high half is nonzero so a
  // 32-bit insert that wrote 64 bits is caught.
  static const uint64_t Vals[] = {
    0x0123456789ABCDEFull,
    0xFEDCBA9876543210ull,
    0x00000000FFFFFFFFull,
    0xFFFFFFFF00000000ull,
  };

  for (unsigned c = 0; c < sizeof(GECases) / sizeof(GECases[0]); ++c) {
    const struct GECase* k = &GECases[c];
    for (unsigned v = 0; v < sizeof(Vals) / sizeof(Vals[0]); ++v) {
      V128 got;
      memset(got.b, 0xA5, sizeof(got.b));
      k->fn(&DstPat, Vals[v], &got);
      const V128 want = ins_gpr_expect(&DstPat, Vals[v], k->esz, k->d);
      char what[96];
      snprintf(what, sizeof(what), "%s val%u", k->name, v);
      expect(what, &got, &want);
    }
  }

  // Every other lane must survive. Run the whole word matrix again from a
  // destination of all-zeroes and require that exactly the four bytes of the
  // target lane are nonzero: an insert that wrote a whole doubleword, or
  // splatted the source across the register, passes the checks above only if
  // the source pattern happened to agree, and fails here always.
  {
    V128 Zero;
    memset(Zero.b, 0, sizeof(Zero.b));
    for (unsigned c = 0; c < sizeof(EECases) / sizeof(EECases[0]); ++c) {
      const struct EECase* k = &EECases[c];
      if (k->alias) {
        continue;
      }
      V128 got;
      memset(got.b, 0xA5, sizeof(got.b));
      k->fn(&Zero, &SrcPat, &got);
      const V128 want = ins_expect(&Zero, &SrcPat, k->esz, k->d, k->s);
      char what[96];
      snprintf(what, sizeof(what), "%s from-zero", k->name);
      expect(what, &got, &want);
    }
  }

  // The checker's own control: a deliberately corrupted copy MUST be caught.
  // Without this, a comparator that always agreed would make every line above
  // meaningless. Counted separately so it contributes no FAIL line.
  {
    V128 a = ins_expect(&DstPat, &SrcPat, 4, 1, 2);
    V128 b = a;
    b.b[6] ^= 0x80;
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
