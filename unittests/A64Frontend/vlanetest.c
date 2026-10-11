// SPDX-License-Identifier: MIT
//
// "Did any lane match": the vector compare / horizontal reduction / FMOV /
// CBZ idiom, every form of it, and -- the point of the file -- the register
// state it leaves behind on BOTH edges of the branch.
//
// Self-checking, so it needs no reference machine. Three things are forced by
// the architecture and computed here from the input bytes:
//
//   * the lane mask: all-ones or all-zeros per lane, per the compare's
//     predicate,
//   * the reduction: UMAXV of that mask is a lane of ones iff ANY lane
//     matched, UMINV iff EVERY lane did, in the low element with everything
//     above it cleared,
//   * the GPR the FMOV writes, and therefore which way the CBZ/CBNZ went.
//
// The comparator is itself checked (`control-detects-corruption` at the end).
//
// Why this exists: on this host the whole idiom collapses to a record-form
// vector compare and the guest's own branch, because every VMX compare sets
// CR6 from the whole result (POWER-STRENGTHS.md 2.1, TryFuseVectorLaneTest,
// DEF_OP(VLaneTestCR)). The reduction and the FMOV are then never emitted on
// the fused path -- and v1 and w0 are REAL GUEST REGISTERS, which the guest
// may read after the branch. So every case below reads all three destinations
// after the join, which is the single thing a fusion like this can get wrong
// without any test of branch direction noticing.
//
// The matrix is the whole recognised set, because the mapping from an A64
// predicate to a host compare is a table and one wrong row is a wrong branch
// on one form and nothing else: ten compare forms (five register, five
// against zero -- and CMLT/CMLE are the swapped-operand forms, the rows most
// likely to be backwards), five arrangements (16B/8B/8H/4H/4S), both
// reductions, both branch polarities. 200 shapes, each over a pattern set
// that makes exactly one lane match for every lane index, no lane, and every
// lane.
//
// The last section is the shapes the fusion must REFUSE or must still get
// right: an instruction between the compare and the reduction (not the last
// four of the block, so no fusion), the mask destination aliasing its own
// source, the 64-bit FMOV and the 32-bit CBZ, and the high register bank.
// run.sh runs the whole file again with POWERARM_DISABLELANETESTFUSION=1 and
// requires byte-identical output, which is what makes the fused and unfused
// paths differential against each other on all 200 shapes at once; it also
// runs it with POWERARM_VSXCLASSES off and on.
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

typedef struct {
  V128 Mask;
  V128 Red;
  uint64_t Gpr;
  int Dir;
} Out;

static void fail_vec(const char* what, const char* which, const V128* got, const V128* want) {
  ++Failures;
  printf("FAIL %s %s\n  got ", what, which);
  for (int i = 0; i < 16; ++i) {
    printf("%02x", got->b[i]);
  }
  printf("\n want ");
  for (int i = 0; i < 16; ++i) {
    printf("%02x", want->b[i]);
  }
  printf("\n");
}

static void expect_out(const char* what, const Out* got, const Out* want) {
  ++Checks;
  if (memcmp(got->Mask.b, want->Mask.b, 16) != 0) {
    fail_vec(what, "mask", &got->Mask, &want->Mask);
    return;
  }
  if (memcmp(got->Red.b, want->Red.b, 16) != 0) {
    fail_vec(what, "reduction", &got->Red, &want->Red);
    return;
  }
  if (got->Gpr != want->Gpr) {
    ++Failures;
    printf("FAIL %s gpr\n  got %016llx want %016llx\n", what, (unsigned long long)got->Gpr, (unsigned long long)want->Gpr);
    return;
  }
  if (got->Dir != want->Dir) {
    ++Failures;
    printf("FAIL %s branch\n  got %d want %d\n", what, got->Dir, want->Dir);
    return;
  }
}

// ---------------------------------------------------------------------------
// The architecturally forced answer
// ---------------------------------------------------------------------------

enum { P_EQ, P_HI, P_HS, P_GT, P_GE, P_EQ0, P_GT0, P_GE0, P_LT0, P_LE0 };

static uint64_t load_le(const uint8_t* p, int W) {
  uint64_t V = 0;
  for (int i = 0; i < W; ++i) {
    V |= (uint64_t)p[i] << (8 * i);
  }
  return V;
}

static int64_t sext(uint64_t V, int W) {
  const int Shift = 64 - 8 * W;
  return (int64_t)(V << Shift) >> Shift;
}

static int lane_pred(int Pred, uint64_t A, uint64_t B, int W) {
  switch (Pred) {
  case P_EQ: return A == B;
  case P_HI: return A > B;
  case P_HS: return A >= B;
  case P_GT: return sext(A, W) > sext(B, W);
  case P_GE: return sext(A, W) >= sext(B, W);
  case P_EQ0: return A == 0;
  case P_GT0: return sext(A, W) > 0;
  case P_GE0: return sext(A, W) >= 0;
  case P_LT0: return sext(A, W) < 0;
  case P_LE0: return sext(A, W) <= 0;
  default: return 0;
  }
}

// Mask, reduction, GPR and branch direction, from the bytes alone.
static Out forced(int Pred, const V128* a, const V128* b, int W, int Q, int All, int Bnz, int Wide) {
  Out e;
  memset(&e, 0, sizeof(e));
  const int Bytes = Q ? 16 : 8;
  int Any = 0;
  int Every = 1;
  for (int off = 0; off < Bytes; off += W) {
    const uint64_t A = load_le(a->b + off, W);
    const uint64_t B = load_le(b->b + off, W);
    if (lane_pred(Pred, A, B, W)) {
      Any = 1;
      for (int i = 0; i < W; ++i) {
        e.Mask.b[off + i] = 0xff;
      }
    } else {
      Every = 0;
    }
  }
  const int RedSet = All ? Every : Any;
  if (RedSet) {
    for (int i = 0; i < W; ++i) {
      e.Red.b[i] = 0xff;
    }
  }
  // FMOV reads the low 32 bits (w, s) or the low 64 (x, d); the reduction
  // wrote W bytes and cleared everything above them, so both see the same
  // value. Writing a W register zero-extends to 64 bits either way.
  (void)Wide;
  e.Gpr = RedSet ? ((1ull << (8 * W)) - 1) : 0;
  e.Dir = Bnz ? (e.Gpr != 0) : (e.Gpr == 0);
  return e;
}

// ---------------------------------------------------------------------------
// The idiom itself
// ---------------------------------------------------------------------------
//
// v1 (the mask), v3 (the reduction) and x9 (the FMOV's destination) are
// poisoned before the compare, so an edge that writes none of them is visible
// rather than accidentally equal to a zeroed buffer. All three are read after
// the join label, i.e. in a block the fused branch reaches from either side.

#define IDIOM_BODY(CmpText, Arr, SR, RedMn, Br)                                         \
  uint64_t dir, g;                                                                    \
  asm volatile("ldr q0, [%[a]]\n\t"                                                   \
               "ldr q2, [%[b]]\n\t"                                                   \
               "movi v1.16b, #0xa5\n\t"                                               \
               "movi v3.16b, #0x5a\n\t"                                               \
               "mov x9, #0x1234\n\t" CmpText RedMn " " SR "3, v1." Arr "\n\t"           \
               "fmov w9, s3\n\t" Br " x9, 1f\n\t"                                     \
               "mov %[dir], #0\n\t"                                                   \
               "b 2f\n"                                                               \
               "1:\n\t"                                                               \
               "mov %[dir], #1\n"                                                      \
               "2:\n\t"                                                               \
               "str q1, [%[m]]\n\t"                                                   \
               "str q3, [%[r]]\n\t"                                                   \
               "mov %[g], x9\n\t"                                                     \
               : [dir] "=&r"(dir), [g] "=&r"(g)                                       \
               : [a] "r"(a->b), [b] "r"(b->b), [m] "r"(o->Mask.b), [r] "r"(o->Red.b)  \
               : "v0", "v1", "v2", "v3", "x9", "memory");                             \
  o->Gpr = g;                                                                         \
  o->Dir = (int)dir;

#define IDIOM_REG(Cmp, Arr, SR, RedMn, Br) IDIOM_BODY(Cmp " v1." Arr ", v0." Arr ", v2." Arr "\n\t", Arr, SR, RedMn, Br)
#define IDIOM_ZERO(Cmp, Arr, SR, RedMn, Br) IDIOM_BODY(Cmp " v1." Arr ", v0." Arr ", #0\n\t", Arr, SR, RedMn, Br)

#define DEFINE_SHAPE(CmpId, Cmp, Kind, Pred, ArrId, Arr, SR, W, Q, RedId, RedMn, All, BrId, Br, Bnz) \
  static void shape_##CmpId##_##ArrId##_##RedId##_##BrId(const V128* a, const V128* b, Out* o) {   \
    IDIOM_##Kind(Cmp, Arr, SR, RedMn, Br)                                                            \
  }

typedef struct {
  const char* Name;
  void (*Run)(const V128*, const V128*, Out*);
  int Pred, W, Q, All, Bnz;
} Shape;

#define TABLE_SHAPE(CmpId, Cmp, Kind, Pred, ArrId, Arr, SR, W, Q, RedId, RedMn, All, BrId, Br, Bnz) \
  {#CmpId "." Arr "/" RedMn "/" Br, shape_##CmpId##_##ArrId##_##RedId##_##BrId, Pred, W, Q, All, Bnz},

#define FOR_BRS(X, CmpId, Cmp, Kind, Pred, ArrId, Arr, SR, W, Q, RedId, RedMn, All) \
  X(CmpId, Cmp, Kind, Pred, ArrId, Arr, SR, W, Q, RedId, RedMn, All, cbz, "cbz", 0)  \
  X(CmpId, Cmp, Kind, Pred, ArrId, Arr, SR, W, Q, RedId, RedMn, All, cbnz, "cbnz", 1)

#define FOR_REDS(X, CmpId, Cmp, Kind, Pred, ArrId, Arr, SR, W, Q)              \
  FOR_BRS(X, CmpId, Cmp, Kind, Pred, ArrId, Arr, SR, W, Q, umaxv, "umaxv", 0)  \
  FOR_BRS(X, CmpId, Cmp, Kind, Pred, ArrId, Arr, SR, W, Q, uminv, "uminv", 1)

#define FOR_ARRS(X, CmpId, Cmp, Kind, Pred)                   \
  FOR_REDS(X, CmpId, Cmp, Kind, Pred, b16, "16b", "b", 1, 1)  \
  FOR_REDS(X, CmpId, Cmp, Kind, Pred, b8, "8b", "b", 1, 0)    \
  FOR_REDS(X, CmpId, Cmp, Kind, Pred, h8, "8h", "h", 2, 1)    \
  FOR_REDS(X, CmpId, Cmp, Kind, Pred, h4, "4h", "h", 2, 0)    \
  FOR_REDS(X, CmpId, Cmp, Kind, Pred, s4, "4s", "s", 4, 1)

// The ten A64 compare forms the fusion recognises. The five against zero are
// the ones whose operand order the host cannot take directly: CMLT/CMLE are
// `0 > a` and `0 >= a`, so they arrive at the IR op with the zero FIRST.
#define ALL_SHAPES(X)                      \
  FOR_ARRS(X, cmeq, "cmeq", REG, P_EQ)     \
  FOR_ARRS(X, cmhi, "cmhi", REG, P_HI)     \
  FOR_ARRS(X, cmhs, "cmhs", REG, P_HS)     \
  FOR_ARRS(X, cmgt, "cmgt", REG, P_GT)     \
  FOR_ARRS(X, cmge, "cmge", REG, P_GE)     \
  FOR_ARRS(X, cmeq0, "cmeq", ZERO, P_EQ0)  \
  FOR_ARRS(X, cmgt0, "cmgt", ZERO, P_GT0)  \
  FOR_ARRS(X, cmge0, "cmge", ZERO, P_GE0)  \
  FOR_ARRS(X, cmlt0, "cmlt", ZERO, P_LT0)  \
  FOR_ARRS(X, cmle0, "cmle", ZERO, P_LE0)

ALL_SHAPES(DEFINE_SHAPE)

static const Shape Shapes[] = {ALL_SHAPES(TABLE_SHAPE)};

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

static V128 Inputs[64][2];
static int NumInputs = 0;

static void add_input(const uint8_t* A, const uint8_t* B) {
  memcpy(Inputs[NumInputs][0].b, A, 16);
  memcpy(Inputs[NumInputs][1].b, B, 16);
  ++NumInputs;
}

static void build_inputs(void) {
  uint8_t A[16], B[16];
  // One byte differs, at every byte position: for every arrangement this is
  // "exactly one lane matches" for the ordering predicates and "exactly one
  // lane does not" for the equalities, at every lane index.
  for (int i = 0; i < 16; ++i) {
    memset(A, 0, 16);
    memset(B, 0, 16);
    A[i] = 0x01;
    add_input(A, B);
  }
  // The same, with the differing byte negative: this is where a signed
  // compare and an unsigned one must disagree.
  for (int i = 0; i < 16; i += 3) {
    memset(A, 0, 16);
    memset(B, 0, 16);
    A[i] = 0x80;
    add_input(A, B);
  }
  memset(A, 0, 16);
  memset(B, 0, 16);
  add_input(A, B); // every lane equal, every lane zero
  memset(A, 0xff, 16);
  memset(B, 0, 16);
  add_input(A, B); // -1 against 0: unsigned above, signed below
  memset(A, 0, 16);
  memset(B, 0xff, 16);
  add_input(A, B);
  memset(A, 0x80, 16);
  memset(B, 0x7f, 16);
  add_input(A, B); // the sign boundary in every lane
  memset(A, 0x7f, 16);
  memset(B, 0x80, 16);
  add_input(A, B);
  for (int i = 0; i < 16; ++i) {
    A[i] = (uint8_t)(i * 17);
    B[i] = (uint8_t)(0xff - i * 17);
  }
  add_input(A, B);
  for (int i = 0; i < 16; ++i) {
    A[i] = (uint8_t)(i * 17);
    B[i] = (uint8_t)(i * 17);
  }
  add_input(A, B); // every lane equal, no lane zero
  for (int i = 0; i < 16; ++i) {
    A[i] = (uint8_t)((i == 7) ? 0x00 : 0xff);
    B[i] = 0x00;
  }
  add_input(A, B);
  for (int i = 0; i < 16; ++i) {
    A[i] = (uint8_t)((i & 1) ? 0x00 : 0x80);
    B[i] = (uint8_t)((i & 1) ? 0x80 : 0x00);
  }
  add_input(A, B);
}

// ---------------------------------------------------------------------------
// The shapes the fusion must refuse, and the two operand forms outside the
// main matrix
// ---------------------------------------------------------------------------

// An instruction between the compare and the reduction: the idiom is no
// longer the last four instructions of the block, so nothing fuses and this
// is the old lowering. Same answers.
static void shape_refused_gap(const V128* a, const V128* b, Out* o) {
  uint64_t dir, g;
  asm volatile("ldr q0, [%[a]]\n\t"
               "ldr q2, [%[b]]\n\t"
               "movi v1.16b, #0xa5\n\t"
               "movi v3.16b, #0x5a\n\t"
               "mov x9, #0x1234\n\t"
               "cmeq v1.16b, v0.16b, v2.16b\n\t"
               "nop\n\t"
               "umaxv b3, v1.16b\n\t"
               "fmov w9, s3\n\t"
               "cbnz x9, 1f\n\t"
               "mov %[dir], #0\n\t"
               "b 2f\n"
               "1:\n\t"
               "mov %[dir], #1\n"
               "2:\n\t"
               "str q1, [%[m]]\n\t"
               "str q3, [%[r]]\n\t"
               "mov %[g], x9\n\t"
               : [dir] "=&r"(dir), [g] "=&r"(g)
               : [a] "r"(a->b), [b] "r"(b->b), [m] "r"(o->Mask.b), [r] "r"(o->Red.b)
               : "v0", "v1", "v2", "v3", "x9", "memory");
  o->Gpr = g;
  o->Dir = (int)dir;
}

// The mask destination IS one of the compare's sources, which is what a
// rematerialising edge has to survive: it re-reads v0 after the branch, and
// may only do so because the fused path wrote nothing.
static void shape_alias(const V128* a, const V128* b, Out* o) {
  uint64_t dir, g;
  asm volatile("ldr q0, [%[a]]\n\t"
               "ldr q2, [%[b]]\n\t"
               "movi v3.16b, #0x5a\n\t"
               "mov x9, #0x1234\n\t"
               "cmeq v0.16b, v0.16b, v2.16b\n\t"
               "umaxv b3, v0.16b\n\t"
               "fmov w9, s3\n\t"
               "cbnz x9, 1f\n\t"
               "mov %[dir], #0\n\t"
               "b 2f\n"
               "1:\n\t"
               "mov %[dir], #1\n"
               "2:\n\t"
               "str q0, [%[m]]\n\t"
               "str q3, [%[r]]\n\t"
               "mov %[g], x9\n\t"
               : [dir] "=&r"(dir), [g] "=&r"(g)
               : [a] "r"(a->b), [b] "r"(b->b), [m] "r"(o->Mask.b), [r] "r"(o->Red.b)
               : "v0", "v2", "v3", "x9", "memory");
  o->Gpr = g;
  o->Dir = (int)dir;
}

// The 64-bit FMOV (x, d) and UMINV: the reduction's own element is 2 bytes
// here, so the 64-bit read must still see exactly it and nothing above.
static void shape_fmov_x(const V128* a, const V128* b, Out* o) {
  uint64_t dir, g;
  asm volatile("ldr q0, [%[a]]\n\t"
               "ldr q2, [%[b]]\n\t"
               "movi v1.16b, #0xa5\n\t"
               "movi v3.16b, #0x5a\n\t"
               "mov x9, #0x1234\n\t"
               "cmhs v1.8h, v0.8h, v2.8h\n\t"
               "uminv h3, v1.8h\n\t"
               "fmov x9, d3\n\t"
               "cbz x9, 1f\n\t"
               "mov %[dir], #0\n\t"
               "b 2f\n"
               "1:\n\t"
               "mov %[dir], #1\n"
               "2:\n\t"
               "str q1, [%[m]]\n\t"
               "str q3, [%[r]]\n\t"
               "mov %[g], x9\n\t"
               : [dir] "=&r"(dir), [g] "=&r"(g)
               : [a] "r"(a->b), [b] "r"(b->b), [m] "r"(o->Mask.b), [r] "r"(o->Red.b)
               : "v0", "v1", "v2", "v3", "x9", "memory");
  o->Gpr = g;
  o->Dir = (int)dir;
}

// The 32-bit branch (cbz w9) rather than the 64-bit one.
static void shape_cbz_w(const V128* a, const V128* b, Out* o) {
  uint64_t dir, g;
  asm volatile("ldr q0, [%[a]]\n\t"
               "ldr q2, [%[b]]\n\t"
               "movi v1.16b, #0xa5\n\t"
               "movi v3.16b, #0x5a\n\t"
               "mov x9, #0x1234\n\t"
               "cmgt v1.4s, v0.4s, v2.4s\n\t"
               "umaxv s3, v1.4s\n\t"
               "fmov w9, s3\n\t"
               "cbz w9, 1f\n\t"
               "mov %[dir], #0\n\t"
               "b 2f\n"
               "1:\n\t"
               "mov %[dir], #1\n"
               "2:\n\t"
               "str q1, [%[m]]\n\t"
               "str q3, [%[r]]\n\t"
               "mov %[g], x9\n\t"
               : [dir] "=&r"(dir), [g] "=&r"(g)
               : [a] "r"(a->b), [b] "r"(b->b), [m] "r"(o->Mask.b), [r] "r"(o->Red.b)
               : "v0", "v1", "v2", "v3", "x9", "memory");
  o->Gpr = g;
  o->Dir = (int)dir;
}

// The high register bank: guest V16-V31 are pinned into the low half of the
// host VSR file, which a VMX-form instruction cannot name, so the fused
// compare and the rematerialised one both have to get them out of there.
static void shape_highbank(const V128* a, const V128* b, Out* o) {
  uint64_t dir, g;
  asm volatile("ldr q18, [%[a]]\n\t"
               "ldr q20, [%[b]]\n\t"
               "movi v19.16b, #0xa5\n\t"
               "movi v17.16b, #0x5a\n\t"
               "mov x9, #0x1234\n\t"
               "cmhi v19.16b, v18.16b, v20.16b\n\t"
               "umaxv b17, v19.16b\n\t"
               "fmov w9, s17\n\t"
               "cbnz x9, 1f\n\t"
               "mov %[dir], #0\n\t"
               "b 2f\n"
               "1:\n\t"
               "mov %[dir], #1\n"
               "2:\n\t"
               "str q19, [%[m]]\n\t"
               "str q17, [%[r]]\n\t"
               "mov %[g], x9\n\t"
               : [dir] "=&r"(dir), [g] "=&r"(g)
               : [a] "r"(a->b), [b] "r"(b->b), [m] "r"(o->Mask.b), [r] "r"(o->Red.b)
               : "v17", "v18", "v19", "v20", "x9", "memory");
  o->Gpr = g;
  o->Dir = (int)dir;
}

static const Shape Extra[] = {
  {"refused-gap.cmeq.16b/umaxv/cbnz", shape_refused_gap, P_EQ, 1, 1, 0, 1},
  {"alias.cmeq.16b/umaxv/cbnz", shape_alias, P_EQ, 1, 1, 0, 1},
  {"fmov-x.cmhs.8h/uminv/cbz", shape_fmov_x, P_HS, 2, 1, 1, 0},
  {"cbz-w.cmgt.4s/umaxv/cbz", shape_cbz_w, P_GT, 4, 1, 0, 0},
  {"highbank.cmhi.16b/umaxv/cbnz", shape_highbank, P_HI, 1, 1, 0, 1},
};

int main(void) {
  char what[96];
  build_inputs();

  for (unsigned s = 0; s < sizeof(Shapes) / sizeof(Shapes[0]); ++s) {
    const Shape* Sh = &Shapes[s];
    for (int i = 0; i < NumInputs; ++i) {
      Out Got;
      memset(&Got, 0, sizeof(Got));
      Sh->Run(&Inputs[i][0], &Inputs[i][1], &Got);
      const Out Want = forced(Sh->Pred, &Inputs[i][0], &Inputs[i][1], Sh->W, Sh->Q, Sh->All, Sh->Bnz, 0);
      snprintf(what, sizeof(what), "%s input%d", Sh->Name, i);
      expect_out(what, &Got, &Want);
    }
  }

  for (unsigned s = 0; s < sizeof(Extra) / sizeof(Extra[0]); ++s) {
    const Shape* Sh = &Extra[s];
    for (int i = 0; i < NumInputs; ++i) {
      Out Got;
      memset(&Got, 0, sizeof(Got));
      Sh->Run(&Inputs[i][0], &Inputs[i][1], &Got);
      const Out Want = forced(Sh->Pred, &Inputs[i][0], &Inputs[i][1], Sh->W, Sh->Q, Sh->All, Sh->Bnz, 0);
      snprintf(what, sizeof(what), "%s input%d", Sh->Name, i);
      expect_out(what, &Got, &Want);
    }
  }

  // Four anchors computed by hand rather than by the model above, so that a
  // wrong model cannot agree with a wrong emulator. 16 bytes of 0x01 against
  // 16 of 0x00:
  //   cmhi.16b  -> every lane 0xff; umaxv -> 0xff; cbnz taken
  //   cmhi.16b  -> every lane 0xff; uminv -> 0xff; cbnz taken
  //   cmeq.16b  -> every lane 0x00; umaxv -> 0x00; cbnz NOT taken, w0 == 0
  //   cmlt.16b against #0 of 0x80 -> every lane 0xff (0x80 is negative)
  {
    V128 Ones, Zeros, Neg;
    memset(Ones.b, 0x01, 16);
    memset(Zeros.b, 0x00, 16);
    memset(Neg.b, 0x80, 16);

    Out o;
    V128 AllOnes, AllZero, LowOne, LowZero;
    memset(AllOnes.b, 0xff, 16);
    memset(AllZero.b, 0x00, 16);
    memset(LowOne.b, 0x00, 16);
    LowOne.b[0] = 0xff;
    memset(LowZero.b, 0x00, 16);

    memset(&o, 0, sizeof(o));
    shape_cmhi_b16_umaxv_cbnz(&Ones, &Zeros, &o);
    Out w = {AllOnes, LowOne, 0xff, 1};
    expect_out("anchor cmhi.16b/umaxv/cbnz 1>0", &o, &w);

    memset(&o, 0, sizeof(o));
    shape_cmhi_b16_uminv_cbnz(&Ones, &Zeros, &o);
    Out w2 = {AllOnes, LowOne, 0xff, 1};
    expect_out("anchor cmhi.16b/uminv/cbnz 1>0", &o, &w2);

    memset(&o, 0, sizeof(o));
    shape_cmeq_b16_umaxv_cbnz(&Ones, &Zeros, &o);
    Out w3 = {AllZero, LowZero, 0, 0};
    expect_out("anchor cmeq.16b/umaxv/cbnz 1==0", &o, &w3);

    memset(&o, 0, sizeof(o));
    shape_cmlt0_b16_uminv_cbnz(&Neg, &Zeros, &o);
    Out w4 = {AllOnes, LowOne, 0xff, 1};
    expect_out("anchor cmlt.16b#0/uminv/cbnz 0x80<0", &o, &w4);
  }

  // The checker's own control: a deliberately corrupted copy MUST be caught.
  // Counted separately so it contributes no FAIL line.
  {
    V128 Ones, Zeros;
    memset(Ones.b, 0x01, 16);
    memset(Zeros.b, 0x00, 16);
    Out a = forced(P_EQ, &Ones, &Zeros, 1, 1, 0, 1, 0);
    Out b = a;
    b.Mask.b[5] ^= 0x80;
    if (memcmp(a.Mask.b, b.Mask.b, 16) == 0) {
      printf("FAIL control-detects-corruption: comparator passed a corrupt buffer\n");
      ++Failures;
    } else {
      printf("PASS control-detects-corruption\n");
    }
  }

  printf("checks %d failures %d\n", Checks, Failures);
  return Failures ? 1 : 0;
}
