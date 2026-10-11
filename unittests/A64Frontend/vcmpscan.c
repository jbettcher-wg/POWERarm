// SPDX-License-Identifier: MIT
//
// "Did any lane of this compare match": the CMEQ / UMAXP|ADDP / FMOV / CBZ
// idiom that glibc's strlen and memchr are built out of, and -- the point of
// the file -- the register state that idiom must leave behind on BOTH edges
// of the branch.
//
// Self-checking, so it needs no reference machine. Everything it asserts is
// forced by the architecture and computed here from the input bytes:
//
//   * the lane mask CMEQ writes: all-ones or all-zeros per lane,
//   * the pairwise reduction of that mask by itself: UMAXP's unsigned max or
//     ADDP's modular sum of each adjacent pair, and because both source
//     operands are the mask, the result's upper half repeats its lower half,
//   * the GPR the FMOV writes, which is the reduction's low 64 (or 32) bits,
//     and therefore which way the CBZ/CBNZ went.
//
// The comparator is itself checked (`control-detects-corruption` at the end).
//
// Why this exists: on this host the whole idiom collapses to a record-form
// vector compare and the guest's own branch, because every VMX compare sets
// CR6 from the whole result -- CR6[2] says no lane matched
// (TryFuseVectorScan, DEF_OP(CondJump)'s vector-compare arm). The mask, the
// reduction and the FMOV are then never emitted on the fused branch itself,
// and v1, v3 and x9 are REAL GUEST REGISTERS that the guest may read after
// the branch. So every case below reads all three destinations AFTER THE
// JOIN -- in a block the branch reaches from either side -- with all three
// poisoned beforehand, so a stale value is visible rather than accidentally
// equal to a zeroed buffer. That is the single thing a fusion like this can
// get wrong without any test of branch direction noticing, and the reason
// this file exists rather than a branch-direction test.
//
// The matrix is the recognised set: CMEQ against a register and against #0,
// the 16B/8H/4S arrangements under both reductions plus ADDP's 2D (UMAXP has
// no 2D form), and both branch polarities. 28 shapes, each over inputs that
// make no lane match, exactly one lane match at every lane index of every
// width, and every lane match.
//
// The last section is the shapes outside that matrix, which must either
// still fuse correctly or be refused and still be right:
//
//   * the loop form, where the no-match edge branches BACKWARDS to a block
//     the compile unit already has. That is the strlen shape the fusion
//     exists for, and the edge there is an existing block rather than a
//     fresh one -- so it is the case where "jump straight to the target"
//     would skip the register writes entirely. It is checked at the TOP of
//     the following iteration, which is the only place a back edge's
//     register state can be read.
//   * the mask destination aliasing its own compare source, which a fused
//     edge may only rematerialise because it wrote nothing before it,
//   * the reduction destination aliasing the mask, so that the two writes
//     land on one register and their ORDER decides the answer,
//   * the 32-bit FMOV and branch, which this fuser refuses,
//   * an instruction between the compare and the reduction, so the idiom is
//     not the last four of its block and nothing fuses,
//   * the high register bank, V16-V31.
//
// run.sh runs the whole file again with POWERARM_VCMPFUSION=0 and requires
// byte-identical output, which makes the fused and the unfused lowering
// differential against each other on every shape at once, and counts in the
// IR dump that the fusion fired at all.
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

static uint64_t load_le(const uint8_t* p, int W) {
  uint64_t V = 0;
  for (int i = 0; i < W; ++i) {
    V |= (uint64_t)p[i] << (8 * i);
  }
  return V;
}

static void store_le(uint8_t* p, uint64_t V, int W) {
  for (int i = 0; i < W; ++i) {
    p[i] = (uint8_t)(V >> (8 * i));
  }
}

// Mask, reduction, GPR and branch direction, from the bytes alone.
//
// W is the element width in bytes, Zero selects the "against #0" form, Add
// selects ADDP over UMAXP, Wide the 64-bit FMOV over the 32-bit one, and Bnz
// CBNZ over CBZ. Every shape here is Q == 1, which the fuser requires.
static Out forced(const V128* a, const V128* b, int W, int Zero, int Add, int Wide, int Bnz) {
  Out e;
  memset(&e, 0, sizeof(e));

  // CMEQ: all-ones where the lanes are equal, all-zeros where they are not.
  for (int off = 0; off < 16; off += W) {
    const uint64_t A = load_le(a->b + off, W);
    const uint64_t B = Zero ? 0 : load_le(b->b + off, W);
    if (A == B) {
      memset(e.Mask.b + off, 0xff, (size_t)W);
    }
  }

  // UMAXP/ADDP Vd, Vn, Vm reduces adjacent pairs of the concatenation Vm:Vn.
  // Both operands are the mask here, so the low half of the result is the
  // pairwise reduction of the whole mask and the high half repeats it.
  const int N = 16 / W;
  uint64_t Pair[8];
  for (int i = 0; i < N / 2; ++i) {
    const uint64_t X = load_le(e.Mask.b + (2 * i) * W, W);
    const uint64_t Y = load_le(e.Mask.b + (2 * i + 1) * W, W);
    uint64_t V = Add ? (X + Y) : (X > Y ? X : Y);
    if (W < 8) {
      V &= (1ull << (8 * W)) - 1;
    }
    Pair[i] = V;
  }
  for (int i = 0; i < N; ++i) {
    store_le(e.Red.b + i * W, Pair[i % (N / 2)], W);
  }

  // FMOV x, d reads the low 64 bits; FMOV w, s the low 32, zero-extended
  // into the X register.
  e.Gpr = load_le(e.Red.b, 8);
  if (!Wide) {
    e.Gpr &= 0xffffffffull;
  }
  e.Dir = Bnz ? (e.Gpr != 0) : (e.Gpr == 0);
  return e;
}

// ---------------------------------------------------------------------------
// The idiom itself
// ---------------------------------------------------------------------------
//
// v1 (the mask), v3 (the reduction) and x9 (the FMOV's destination) are
// poisoned before the compare, and all three are read after the join label,
// i.e. in a block the fused branch reaches from either side.

#define IDIOM_BODY(CmpText, Arr, RedMn, Br)                                                  \
  uint64_t dir, g;                                                                           \
  asm volatile("ldr q0, [%[a]]\n\t"                                                          \
               "ldr q2, [%[b]]\n\t"                                                          \
               "movi v1.16b, #0xa5\n\t"                                                      \
               "movi v3.16b, #0x5a\n\t"                                                      \
               "mov x9, #0x1234\n\t" CmpText RedMn " v3." Arr ", v1." Arr ", v1." Arr "\n\t" \
               "fmov x9, d3\n\t" Br " x9, 1f\n\t"                                            \
               "mov %[dir], #0\n\t"                                                          \
               "b 2f\n"                                                                      \
               "1:\n\t"                                                                      \
               "mov %[dir], #1\n"                                                            \
               "2:\n\t"                                                                      \
               "str q1, [%[m]]\n\t"                                                          \
               "str q3, [%[r]]\n\t"                                                          \
               "mov %[g], x9\n\t"                                                            \
               : [dir] "=&r"(dir), [g] "=&r"(g)                                              \
               : [a] "r"(a->b), [b] "r"(b->b), [m] "r"(o->Mask.b), [r] "r"(o->Red.b)         \
               : "v0", "v1", "v2", "v3", "x9", "memory");                                    \
  o->Gpr = g;                                                                                \
  o->Dir = (int)dir;

#define IDIOM_REG(Arr, RedMn, Br) IDIOM_BODY("cmeq v1." Arr ", v0." Arr ", v2." Arr "\n\t", Arr, RedMn, Br)
#define IDIOM_ZERO(Arr, RedMn, Br) IDIOM_BODY("cmeq v1." Arr ", v0." Arr ", #0\n\t", Arr, RedMn, Br)

#define DEFINE_SHAPE(KindId, Kind, Zero, ArrId, Arr, W, RedId, RedMn, Add, BrId, Br, Bnz)         \
  static void shape_##KindId##_##ArrId##_##RedId##_##BrId(const V128* a, const V128* b, Out* o) { \
    IDIOM_##Kind(Arr, RedMn, Br)                                                                  \
  }

typedef struct {
  const char* Name;
  void (*Run)(const V128*, const V128*, Out*);
  int W, Zero, Add, Wide, Bnz;
  // The reduction's destination IS the mask's, so the register read back
  // holds the reduction: the later of the two guest writes.
  int RedOverMask;
} Shape;

#define TABLE_SHAPE(KindId, Kind, Zero, ArrId, Arr, W, RedId, RedMn, Add, BrId, Br, Bnz) \
  {#KindId "." Arr "/" RedMn "/" Br, shape_##KindId##_##ArrId##_##RedId##_##BrId, W, Zero, Add, 1, Bnz, 0},

#define FOR_BRS(X, KindId, Kind, Zero, ArrId, Arr, W, RedId, RedMn, Add) \
  X(KindId, Kind, Zero, ArrId, Arr, W, RedId, RedMn, Add, cbz, "cbz", 0) \
  X(KindId, Kind, Zero, ArrId, Arr, W, RedId, RedMn, Add, cbnz, "cbnz", 1)

#define FOR_REDS(X, KindId, Kind, Zero, ArrId, Arr, W)             \
  FOR_BRS(X, KindId, Kind, Zero, ArrId, Arr, W, umaxp, "umaxp", 0) \
  FOR_BRS(X, KindId, Kind, Zero, ArrId, Arr, W, addp, "addp", 1)

// UMAXP has no .2D form at all, so that arrangement is ADDP only.
#define FOR_ARRS(X, KindId, Kind, Zero)          \
  FOR_REDS(X, KindId, Kind, Zero, b16, "16b", 1) \
  FOR_REDS(X, KindId, Kind, Zero, h8, "8h", 2)   \
  FOR_REDS(X, KindId, Kind, Zero, s4, "4s", 4)   \
  FOR_BRS(X, KindId, Kind, Zero, d2, "2d", 8, addp, "addp", 1)

#define ALL_SHAPES(X)       \
  FOR_ARRS(X, cmeq, REG, 0) \
  FOR_ARRS(X, cmeq0, ZERO, 1)

ALL_SHAPES(DEFINE_SHAPE)

static const Shape Shapes[] = {ALL_SHAPES(TABLE_SHAPE)};

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

static V128 Inputs[80][2];
static int NumInputs = 0;

static void add_input(const uint8_t* A, const uint8_t* B) {
  memcpy(Inputs[NumInputs][0].b, A, 16);
  memcpy(Inputs[NumInputs][1].b, B, 16);
  ++NumInputs;
}

static void build_inputs(void) {
  uint8_t A[16], B[16];

  // No lane equal at any width: the no-match edge, where all three
  // destinations are forced to zero and nothing in the fused branch
  // computes them.
  memset(A, 0x00, 16);
  memset(B, 0xff, 16);
  add_input(A, B);
  memset(A, 0xff, 16);
  memset(B, 0x00, 16);
  add_input(A, B);

  // Every lane equal, at zero and at non-zero values. ADDP of an all-ones
  // mask is 0xfe per lane rather than 0xff, which is the arm that a model
  // reading "the reduction is all ones" would get wrong.
  memset(A, 0x00, 16);
  memset(B, 0x00, 16);
  add_input(A, B);
  memset(A, 0xff, 16);
  memset(B, 0xff, 16);
  add_input(A, B);
  for (int i = 0; i < 16; ++i) {
    A[i] = (uint8_t)(i * 17);
    B[i] = A[i];
  }
  add_input(A, B);

  // Exactly one lane equal, at every lane index of every width: A is all
  // 0xff but for one W-byte window of zeros and B is all zero. At width W
  // that window is one matching lane at index off/W; at a wider width it is
  // no matching lane at all, which is the no-match edge again. The same
  // inputs drive the "against #0" form, where the predicate is A == 0.
  for (int W = 1; W <= 8; W <<= 1) {
    for (int off = 0; off < 16; off += W) {
      memset(A, 0xff, 16);
      memset(B, 0x00, 16);
      memset(A + off, 0x00, (size_t)W);
      add_input(A, B);
    }
  }

  // One byte differing, at every byte index: every lane but one matches, so
  // the reduction runs over a mask that is neither uniform nor single-lane.
  for (int i = 0; i < 16; ++i) {
    memset(A, 0x00, 16);
    memset(B, 0x00, 16);
    A[i] = 0x01;
    add_input(A, B);
  }

  // Alternating lanes, so no pairwise reduction can collapse by accident:
  // every adjacent pair is one matching lane and one not.
  for (int i = 0; i < 16; ++i) {
    A[i] = (uint8_t)((i & 1) ? 0x00 : 0x01);
    B[i] = 0x00;
  }
  add_input(A, B);
  for (int i = 0; i < 16; ++i) {
    A[i] = (uint8_t)((i & 1) ? 0x01 : 0x00);
    B[i] = 0x00;
  }
  add_input(A, B);
  for (int i = 0; i < 16; ++i) {
    A[i] = (uint8_t)((i & 2) ? 0x00 : 0x01);
    B[i] = 0x00;
  }
  add_input(A, B);
}

// ---------------------------------------------------------------------------
// The shapes outside the matrix
// ---------------------------------------------------------------------------

// An instruction between the compare and the reduction: the idiom is no
// longer the last four instructions of the block, so nothing fuses and this
// is the old lowering. Same answers.
static void shape_gap(const V128* a, const V128* b, Out* o) {
  uint64_t dir, g;
  asm volatile("ldr q0, [%[a]]\n\t"
               "ldr q2, [%[b]]\n\t"
               "movi v1.16b, #0xa5\n\t"
               "movi v3.16b, #0x5a\n\t"
               "mov x9, #0x1234\n\t"
               "cmeq v1.16b, v0.16b, v2.16b\n\t"
               "nop\n\t"
               "umaxp v3.16b, v1.16b, v1.16b\n\t"
               "fmov x9, d3\n\t"
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

// The mask destination IS one of the compare's sources. A fused edge that
// rematerialises the compare may only re-read v0 after the branch because
// nothing before the branch wrote it -- which is also why the forced edge
// cannot be served by zeroing the destinations BEFORE the branch.
static void shape_alias_src(const V128* a, const V128* b, Out* o) {
  uint64_t dir, g;
  asm volatile("ldr q0, [%[a]]\n\t"
               "ldr q2, [%[b]]\n\t"
               "movi v3.16b, #0x5a\n\t"
               "mov x9, #0x1234\n\t"
               "cmeq v0.16b, v0.16b, v2.16b\n\t"
               "umaxp v3.16b, v0.16b, v0.16b\n\t"
               "fmov x9, d3\n\t"
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

// The reduction's destination IS the mask's, so both guest writes land on
// one register and the register that survives is the LATER one. This is the
// case that makes the order of a forced edge's constant writes observable
// rather than a matter of taste.
static void shape_alias_dst(const V128* a, const V128* b, Out* o) {
  uint64_t dir, g;
  asm volatile("ldr q0, [%[a]]\n\t"
               "ldr q2, [%[b]]\n\t"
               "movi v1.16b, #0xa5\n\t"
               "mov x9, #0x1234\n\t"
               "cmeq v1.8h, v0.8h, v2.8h\n\t"
               "addp v1.8h, v1.8h, v1.8h\n\t"
               "fmov x9, d1\n\t"
               "cbz x9, 1f\n\t"
               "mov %[dir], #0\n\t"
               "b 2f\n"
               "1:\n\t"
               "mov %[dir], #1\n"
               "2:\n\t"
               "str q1, [%[m]]\n\t"
               "str q1, [%[r]]\n\t"
               "mov %[g], x9\n\t"
               : [dir] "=&r"(dir), [g] "=&r"(g)
               : [a] "r"(a->b), [b] "r"(b->b), [m] "r"(o->Mask.b), [r] "r"(o->Red.b)
               : "v0", "v1", "v2", "x9", "memory");
  o->Gpr = g;
  o->Dir = (int)dir;
}

// The 32-bit FMOV and the 32-bit branch, which this fuser refuses: the GPR
// is then the reduction's low 32 bits zero-extended, and the branch reads
// only those. Refused or not, the answer is the same.
static void shape_fmov_w(const V128* a, const V128* b, Out* o) {
  uint64_t dir, g;
  asm volatile("ldr q0, [%[a]]\n\t"
               "ldr q2, [%[b]]\n\t"
               "movi v1.16b, #0xa5\n\t"
               "movi v3.16b, #0x5a\n\t"
               "mov x9, #0x1234\n\t"
               "cmeq v1.4s, v0.4s, v2.4s\n\t"
               "umaxp v3.4s, v1.4s, v1.4s\n\t"
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
               "cmeq v19.16b, v18.16b, v20.16b\n\t"
               "addp v17.16b, v19.16b, v19.16b\n\t"
               "fmov x9, d17\n\t"
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
  {.Name = "gap.cmeq.16b/umaxp/cbnz", .Run = shape_gap, .W = 1, .Zero = 0, .Add = 0, .Wide = 1, .Bnz = 1, .RedOverMask = 0},
  {.Name = "alias-src.cmeq.16b/umaxp/cbnz", .Run = shape_alias_src, .W = 1, .Zero = 0, .Add = 0, .Wide = 1, .Bnz = 1, .RedOverMask = 0},
  {.Name = "alias-dst.cmeq.8h/addp/cbz", .Run = shape_alias_dst, .W = 2, .Zero = 0, .Add = 1, .Wide = 1, .Bnz = 0, .RedOverMask = 1},
  {.Name = "fmov-w.cmeq.4s/umaxp/cbz", .Run = shape_fmov_w, .W = 4, .Zero = 0, .Add = 0, .Wide = 0, .Bnz = 0, .RedOverMask = 0},
  {.Name = "highbank.cmeq.16b/addp/cbnz", .Run = shape_highbank, .W = 1, .Zero = 0, .Add = 1, .Wide = 1, .Bnz = 1, .RedOverMask = 0},
};

// ---------------------------------------------------------------------------
// The loop form: a no-match edge that branches backwards into this compile
// unit, which is the shape glibc's strlen actually has.
// ---------------------------------------------------------------------------
//
// The three destinations are read at the TOP of the loop, before they are
// poisoned again -- the only place a back edge's register state can be
// observed. The last iteration is the one that exits, so the values this
// records are the ones the PREVIOUS iteration's no-match edge left: an
// all-zero mask, an all-zero reduction and a zero GPR.
//
// The buffer's first 16 bytes contain no zero byte and its second 16 do, so
// the loop runs exactly twice and the recorded state is the back edge's.
static void loop_scan(const uint8_t* p, Out* o) {
  uint64_t g, off;
  asm volatile("movi v1.16b, #0xa5\n\t"
               "movi v3.16b, #0x5a\n\t"
               "mov x9, #0x1234\n\t"
               "mov %[off], #0\n"
               "1:\n\t"
               // What the previous iteration's no-match edge left behind.
               "str q1, [%[m]]\n\t"
               "str q3, [%[r]]\n\t"
               "mov %[g], x9\n\t"
               "ldr q0, [%[p], %[off]]\n\t"
               "add %[off], %[off], #16\n\t"
               "cmeq v1.16b, v0.16b, #0\n\t"
               "umaxp v3.16b, v1.16b, v1.16b\n\t"
               "fmov x9, d3\n\t"
               "cbz x9, 1b\n\t"
               : [g] "=&r"(g), [off] "=&r"(off)
               : [p] "r"(p), [m] "r"(o->Mask.b), [r] "r"(o->Red.b)
               : "v0", "v1", "v3", "x9", "memory");
  o->Gpr = g;
  o->Dir = (int)(off / 16);
}

int main(void) {
  char what[96];
  build_inputs();

  for (unsigned s = 0; s < sizeof(Shapes) / sizeof(Shapes[0]); ++s) {
    const Shape* Sh = &Shapes[s];
    for (int i = 0; i < NumInputs; ++i) {
      Out Got;
      memset(&Got, 0, sizeof(Got));
      Sh->Run(&Inputs[i][0], &Inputs[i][1], &Got);
      Out Want = forced(&Inputs[i][0], &Inputs[i][1], Sh->W, Sh->Zero, Sh->Add, Sh->Wide, Sh->Bnz);
      if (Sh->RedOverMask) {
        Want.Mask = Want.Red;
      }
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
      Out Want = forced(&Inputs[i][0], &Inputs[i][1], Sh->W, Sh->Zero, Sh->Add, Sh->Wide, Sh->Bnz);
      if (Sh->RedOverMask) {
        Want.Mask = Want.Red;
      }
      snprintf(what, sizeof(what), "%s input%d", Sh->Name, i);
      expect_out(what, &Got, &Want);
    }
  }

  // The loop form. Two iterations, and what the first one's no-match edge
  // left is all zeros in both vectors and in the GPR.
  {
    uint8_t Buf[48];
    memset(Buf, 0xff, sizeof(Buf));
    Buf[20] = 0x00;
    Out Got;
    memset(&Got, 0, sizeof(Got));
    memset(Got.Mask.b, 0x11, 16);
    memset(Got.Red.b, 0x11, 16);
    loop_scan(Buf, &Got);
    Out Want;
    memset(&Want, 0, sizeof(Want));
    Want.Dir = 2; // iterations
    expect_out("loop back edge leaves all three zero", &Got, &Want);
  }

  // Anchors computed by hand rather than by the model above, so that a wrong
  // model cannot agree with a wrong emulator. 16 bytes of 0x00 against 16 of
  // 0xff is "no lane equal" at every width, which is the forced edge:
  //   mask 0, reduction 0, x9 0, and cbnz NOT taken / cbz taken.
  // 16 bytes of 0x00 against 16 of 0x00 is "every lane equal":
  //   mask all 0xff; umaxp.16b -> all 0xff and x9 0xffffffffffffffff;
  //   addp.16b -> 0xff + 0xff = 0xfe per byte, x9 0xfefefefefefefefe;
  //   addp.2d  -> -1 + -1 per 8-byte lane, x9 0xfffffffffffffffe.
  {
    V128 Zeros, Ones;
    memset(Zeros.b, 0x00, 16);
    memset(Ones.b, 0xff, 16);

    V128 AllZero, AllOnes, AllFE, Dec2D;
    memset(AllZero.b, 0x00, 16);
    memset(AllOnes.b, 0xff, 16);
    memset(AllFE.b, 0xfe, 16);
    memset(Dec2D.b, 0xff, 16);
    Dec2D.b[0] = 0xfe;
    Dec2D.b[8] = 0xfe;

    Out o;

    memset(&o, 0, sizeof(o));
    shape_cmeq_b16_umaxp_cbnz(&Zeros, &Ones, &o);
    Out w1 = {AllZero, AllZero, 0, 0};
    expect_out("anchor cmeq.16b/umaxp/cbnz nolane", &o, &w1);

    memset(&o, 0, sizeof(o));
    shape_cmeq_b16_umaxp_cbz(&Zeros, &Ones, &o);
    Out w2 = {AllZero, AllZero, 0, 1};
    expect_out("anchor cmeq.16b/umaxp/cbz nolane", &o, &w2);

    memset(&o, 0, sizeof(o));
    shape_cmeq_b16_umaxp_cbnz(&Zeros, &Zeros, &o);
    Out w3 = {AllOnes, AllOnes, 0xffffffffffffffffull, 1};
    expect_out("anchor cmeq.16b/umaxp/cbnz everylane", &o, &w3);

    memset(&o, 0, sizeof(o));
    shape_cmeq_b16_addp_cbnz(&Zeros, &Zeros, &o);
    Out w4 = {AllOnes, AllFE, 0xfefefefefefefefeull, 1};
    expect_out("anchor cmeq.16b/addp/cbnz everylane", &o, &w4);

    memset(&o, 0, sizeof(o));
    shape_cmeq0_d2_addp_cbnz(&Zeros, &Ones, &o);
    Out w5 = {AllOnes, Dec2D, 0xfffffffffffffffeull, 1};
    expect_out("anchor cmeq.2d#0/addp/cbnz everylane", &o, &w5);

    memset(&o, 0, sizeof(o));
    shape_cmeq0_d2_addp_cbz(&Ones, &Zeros, &o);
    Out w6 = {AllZero, AllZero, 0, 1};
    expect_out("anchor cmeq.2d#0/addp/cbz nolane", &o, &w6);
  }

  // The checker's own control: a deliberately corrupted copy MUST be caught.
  // Counted separately so it contributes no FAIL line.
  {
    V128 Zeros;
    memset(Zeros.b, 0x00, 16);
    Out a = forced(&Zeros, &Zeros, 1, 0, 0, 1, 1);
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
