// SPDX-License-Identifier: MIT
//
// FEAT_LSE CASP, the paired compare-and-swap: both register widths, both
// outcomes and every acquire/release variant, checking the pair left in
// memory and the pair returned in Rs:Rs+1.
//
// CASP is the first and only user of the IR's CASPair op in this tree, so its
// PPC64 lowering had never executed before this test -- there is no x86
// frontend here for whose CMPXCHG8B/CMPXCHG16B it was written. The two forms
// exercise the lowering's two quite different paths: the 32-bit form is the
// 8-byte-memory case that combines register halves and uses ldarx/stdcx_ (with
// a misalignment fallback), and the 64-bit form is the 16-byte case that uses
// lqarx/stqcx_ with its even/odd register-pair constraint.
//
// The miss cases are the ones that would catch the lowering failing to write
// its `Out` operands at all: on a miss the returned pair must be what memory
// held, not the expected values that were passed in.
//
// Registers are pinned with local register variables because CASP encodes Rs
// and Rt as the first of an even/odd pair, so the operands cannot be left to
// the compiler's choice.
//
// Not covered: an odd Rs or Rt, which is UNDEFINED and which the translator
// rejects so the instruction raises SIGILL. The assembler will not encode it,
// so reaching it needs a hand-assembled .inst, and it would kill the process
// mid-corpus. Its path is the same `return false` every other unallocated
// encoding in the suite already takes.
//
// Differential: the Pi runs the same binary and its stdout is the golden.
#include <stdint.h>
#include <stdio.h>

// The corpus is built without -march, so state the assembler arch here.
__asm__(".arch armv8.1-a");

#define LO64 0x0123456789abcdefULL
#define HI64 0xfedcba9876543210ULL
#define NEW_LO64 0x1122334455667788ULL
#define NEW_HI64 0x99aabbccddeeff00ULL

// 64-bit form: a 16-byte memory operand, naturally aligned.
#define CASP_X(name, exp_lo, exp_hi)                                                \
  do {                                                                              \
    __attribute__((aligned(16))) volatile uint64_t mem[2] = {LO64, HI64};           \
    register uint64_t s0 __asm__("x0") = (exp_lo);                                  \
    register uint64_t s1 __asm__("x1") = (exp_hi);                                  \
    register uint64_t t0 __asm__("x2") = NEW_LO64;                                  \
    register uint64_t t1 __asm__("x3") = NEW_HI64;                                  \
    __asm__ __volatile__(name " x0, x1, x2, x3, [%4]"                               \
                         : "+r"(s0), "+r"(s1)                                        \
                         : "r"(t0), "r"(t1), "r"(&mem[0])                            \
                         : "memory");                                                \
    printf("%-8s %-4s mem=%016llx:%016llx rs=%016llx:%016llx\n", name,              \
           ((exp_lo) == LO64 && (exp_hi) == HI64) ? "hit" : "miss",                 \
           (unsigned long long)mem[0], (unsigned long long)mem[1],                  \
           (unsigned long long)s0, (unsigned long long)s1);                          \
  } while (0)

// 32-bit form: an 8-byte memory operand, naturally aligned.
#define CASP_W(name, exp_lo, exp_hi)                                                \
  do {                                                                              \
    __attribute__((aligned(8))) volatile uint32_t mem[2] = {(uint32_t)LO64,          \
                                                            (uint32_t)HI64};         \
    register uint64_t s0 __asm__("x0") = (uint32_t)(exp_lo);                         \
    register uint64_t s1 __asm__("x1") = (uint32_t)(exp_hi);                         \
    register uint64_t t0 __asm__("x2") = (uint32_t)NEW_LO64;                         \
    register uint64_t t1 __asm__("x3") = (uint32_t)NEW_HI64;                         \
    __asm__ __volatile__(name " w0, w1, w2, w3, [%4]"                               \
                         : "+r"(s0), "+r"(s1)                                        \
                         : "r"(t0), "r"(t1), "r"(&mem[0])                            \
                         : "memory");                                                \
    printf("%-8s %-4s mem=%08x:%08x rs=%016llx:%016llx\n", name,                    \
           ((uint32_t)(exp_lo) == (uint32_t)LO64 &&                                  \
            (uint32_t)(exp_hi) == (uint32_t)HI64)                                    \
               ? "hit"                                                               \
               : "miss",                                                             \
           (uint32_t)mem[0], (uint32_t)mem[1], (unsigned long long)s0,               \
           (unsigned long long)s1);                                                  \
  } while (0)

// Hit, then three shapes of miss: low half wrong, high half wrong, both wrong.
// A half-wrong pair must not store, which is what a lowering that compared
// only one half would get wrong.
#define CASP_SET(mac, op)                                                           \
  mac(op, LO64, HI64);                                                              \
  mac(op, ~LO64, HI64);                                                             \
  mac(op, LO64, ~HI64);                                                             \
  mac(op, ~LO64, ~HI64)

#define CASP_ORDERINGS(mac)                                                         \
  CASP_SET(mac, "casp");                                                            \
  CASP_SET(mac, "caspa");                                                           \
  CASP_SET(mac, "caspl");                                                           \
  CASP_SET(mac, "caspal")

int main(void) {
  CASP_ORDERINGS(CASP_X);
  CASP_ORDERINGS(CASP_W);

  // A second register pair, to show the encoding is not tied to x0/x1 and that
  // the lowering's fixed temporaries do not collide with a different choice.
  {
    __attribute__((aligned(16))) volatile uint64_t mem[2] = {LO64, HI64};
    register uint64_t s0 __asm__("x4") = LO64;
    register uint64_t s1 __asm__("x5") = HI64;
    register uint64_t t0 __asm__("x6") = NEW_LO64;
    register uint64_t t1 __asm__("x7") = NEW_HI64;
    __asm__ __volatile__("caspal x4, x5, x6, x7, [%4]"
                         : "+r"(s0), "+r"(s1)
                         : "r"(t0), "r"(t1), "r"(&mem[0])
                         : "memory");
    printf("%-8s %-4s mem=%016llx:%016llx rs=%016llx:%016llx\n", "casp-x4", "hit",
           (unsigned long long)mem[0], (unsigned long long)mem[1],
           (unsigned long long)s0, (unsigned long long)s1);
  }

  // Rs == Rt: the expected pair is also the desired pair, so a hit rewrites the
  // same bytes and the returned pair must equal what was there.
  {
    __attribute__((aligned(16))) volatile uint64_t mem[2] = {LO64, HI64};
    register uint64_t s0 __asm__("x0") = LO64;
    register uint64_t s1 __asm__("x1") = HI64;
    __asm__ __volatile__("caspal x0, x1, x0, x1, [%2]"
                         : "+r"(s0), "+r"(s1)
                         : "r"(&mem[0])
                         : "memory");
    printf("%-8s %-4s mem=%016llx:%016llx rs=%016llx:%016llx\n", "casp-st", "hit",
           (unsigned long long)mem[0], (unsigned long long)mem[1],
           (unsigned long long)s0, (unsigned long long)s1);
  }
  return 0;
}
