// SPDX-License-Identifier: MIT
//
// LDXR/STXR retry loops under contention, in the shape POWERarm lowers onto
// POWER's native lwarx/stwcx. reservation (TranslateExclusive.cpp,
// TryFuseExclusiveLoop): a guest loop whose CBNZ branches back to its own LDXR.
//
// Every assertion here is architecturally FORCED, so the golden can be
// generated on any conforming AArch64 machine -- including this repository's
// build-powerarm/guest-toolchain/aarch64-gcc under the emulator itself -- and
// still be the right answer:
//
//   * N threads each performing M atomic increments through an exclusive loop
//     leave exactly N*M in the counter. An implementation that loses one update
//     is wrong, not merely different: the load-exclusive/store-exclusive pair is
//     atomic when the store reports success, and the loop only exits on success.
//   * A store-exclusive may fail spuriously, so the *number of iterations* is
//     not determined -- and nothing here asserts it.
//   * None of LDXR, STXR, CBNZ or the register-only instructions between them
//     writes PSTATE.NZCV, so NZCV is unchanged across a loop. Forced.
//
// Nothing implementation-defined is asserted. In particular there is no
// non-exclusive store between a load and its store-exclusive, and no size
// mismatch: those two cases are CONSTRAINED UNPREDICTABLE and live in
// exclusive_pair.c, which only requires a well-formed status bit for them.
//
// The counters are all monotonically increasing and are sized so they never
// wrap during a run. That is deliberate: POWERarm's software-monitor fallback
// (which is what runs whenever the loop is not fused, e.g. under
// POWERARM_MAXINST=1) compares the loaded value rather than holding a
// reservation, so a counter that returned to a previously observed value could
// legitimately let a stale store succeed. A wrapping counter would make this
// test a lottery on the fallback path rather than a check of it.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define THREADS 4

// Small enough that the byte counter below cannot wrap (4 * 50 = 200 < 256),
// large enough that several threads genuinely collide on every counter.
#define ITERS_B 50
#define ITERS 4000

#define CHECK(cond, msg) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s at line %d\n", msg, __LINE__); \
      exit(1); \
    } \
  } while (0)

static volatile uint8_t cnt8;
static volatile uint16_t cnt16;
static volatile uint32_t cnt32;
static volatile uint64_t cnt64;
// One counter per ordering variant of the pair, so all four are contended.
static volatile uint32_t cnt_plain;  // LDXR  / STXR
static volatile uint32_t cnt_acq;    // LDAXR / STXR
static volatile uint32_t cnt_rel;    // LDXR  / STLXR
// Set-bits loop: a two-instruction body, and monotone for the same reason the
// counters are.
static volatile uint64_t bits;
// The shape that is deliberately NOT fused: a compare-exchange loop, whose
// early-out branch between the load and the store splits the pair across two
// translation blocks. It must still count exactly.
static volatile uint64_t cnt_cas;

// Each of these is written so the compiler cannot reshape it: the whole retry
// loop is one asm block, and the CBNZ branches to the label the LDXR sits on.
#define INC_LOOP(name, ldop, stop, reg, ptr) \
  static void name(void) { \
    uint64_t tmp; \
    uint32_t st; \
    __asm__ volatile("1: " ldop " %" reg "[t], [%[p]]\n" \
                     "   add %" reg "[t], %" reg "[t], #1\n" \
                     "   " stop " %w[s], %" reg "[t], [%[p]]\n" \
                     "   cbnz %w[s], 1b\n" \
                     : [t] "=&r"(tmp), [s] "=&r"(st) \
                     : [p] "r"(ptr) \
                     : "memory"); \
  }

INC_LOOP(inc8, "ldaxrb", "stlxrb", "w", &cnt8)
INC_LOOP(inc16, "ldaxrh", "stlxrh", "w", &cnt16)
INC_LOOP(inc32, "ldaxr", "stlxr", "w", &cnt32)
INC_LOOP(inc64, "ldaxr", "stlxr", "", &cnt64)
INC_LOOP(inc_plain, "ldxr", "stxr", "w", &cnt_plain)
INC_LOOP(inc_acq, "ldaxr", "stxr", "w", &cnt_acq)
INC_LOOP(inc_rel, "ldxr", "stlxr", "w", &cnt_rel)

// Two-instruction body: bits |= 1 << lane.
static void set_bit(unsigned lane) {
  uint64_t tmp;
  uint64_t mask = 1ULL << lane;
  uint32_t st;
  __asm__ volatile("1: ldaxr %[t], [%[p]]\n"
                   "   orr %[t], %[t], %[m]\n"
                   "   stlxr %w[s], %[t], [%[p]]\n"
                   "   cbnz %w[s], 1b\n"
                   : [t] "=&r"(tmp), [s] "=&r"(st)
                   : [p] "r"(&bits), [m] "r"(mask)
                   : "memory");
}

// Compare-exchange shape: the b.ne between the pair is what keeps this off the
// fused path.
static void inc_cas(void) {
  uint64_t old, next;
  uint32_t st;
  __asm__ volatile("1: ldaxr %[o], [%[p]]\n"
                   "   add %[n], %[o], #1\n"
                   "   cmp %[o], %[n]\n"
                   "   b.eq 2f\n"
                   "   stlxr %w[s], %[n], [%[p]]\n"
                   "   cbnz %w[s], 1b\n"
                   "2:\n"
                   : [o] "=&r"(old), [n] "=&r"(next), [s] "=&r"(st)
                   : [p] "r"(&cnt_cas)
                   : "memory", "cc");
}

static void* worker(void* arg) {
  const unsigned lane = (unsigned)(uintptr_t)arg;
  for (unsigned i = 0; i < ITERS_B; ++i) {
    inc8();
  }
  for (unsigned i = 0; i < ITERS; ++i) {
    inc16();
    inc32();
    inc64();
    inc_plain();
    inc_acq();
    inc_rel();
    inc_cas();
  }
  // Every thread walks all 64 bits, starting from a different one so they
  // collide on the same word without ever agreeing on which bit. The union is
  // all ones whatever order the stores land in.
  for (unsigned i = 0; i < ITERS; ++i) {
    set_bit((lane + i) % 64);
  }
  return NULL;
}

static void test_contention(void) {
  pthread_t t[THREADS];
  for (unsigned i = 0; i < THREADS; ++i) {
    CHECK(pthread_create(&t[i], NULL, worker, (void*)(uintptr_t)i) == 0, "pthread_create");
  }
  for (unsigned i = 0; i < THREADS; ++i) {
    pthread_join(t[i], NULL);
  }

  CHECK(cnt8 == (uint8_t)(THREADS * ITERS_B), "byte counter exact");
  CHECK(cnt16 == (uint16_t)(THREADS * ITERS), "halfword counter exact");
  CHECK(cnt32 == (uint32_t)(THREADS * ITERS), "word counter exact");
  CHECK(cnt64 == (uint64_t)(THREADS * ITERS), "doubleword counter exact");
  CHECK(cnt_plain == (uint32_t)(THREADS * ITERS), "LDXR/STXR counter exact");
  CHECK(cnt_acq == (uint32_t)(THREADS * ITERS), "LDAXR/STXR counter exact");
  CHECK(cnt_rel == (uint32_t)(THREADS * ITERS), "LDXR/STLXR counter exact");
  CHECK(cnt_cas == (uint64_t)(THREADS * ITERS), "compare-exchange counter exact");
  CHECK(bits == ~0ULL, "every bit set");

  printf("test_contention: PASS\n");
}

// An exclusive loop with no body at all: the exchange shape.
static void test_exchange(void) {
  volatile uint64_t mem = 0x1122334455667788ULL;
  uint64_t seen;
  uint32_t st;
  const uint64_t put = 0xfeedfacecafebeefULL;
  __asm__ volatile("1: ldaxr %[o], [%[p]]\n"
                   "   stlxr %w[s], %[v], [%[p]]\n"
                   "   cbnz %w[s], 1b\n"
                   : [o] "=&r"(seen), [s] "=&r"(st)
                   : [p] "r"(&mem), [v] "r"(put)
                   : "memory");
  CHECK(seen == 0x1122334455667788ULL, "exchange returned the old value");
  CHECK(mem == put, "exchange stored the new value");
  CHECK(st == 0, "the loop only exits on success");
  printf("test_exchange: PASS\n");
}

// NZCV across a whole exclusive loop. Nothing in the loop writes the flags, so
// every one of the sixteen states must come back unchanged.
static void test_nzcv(void) {
  volatile uint32_t mem = 7;
  for (uint32_t flags = 0; flags < 16; ++flags) {
    const uint64_t in = ((uint64_t)flags) << 28;
    uint64_t out = 0;
    uint64_t tmp;
    uint32_t st;
    __asm__ volatile("msr nzcv, %[in]\n"
                     "1: ldaxr %w[t], [%[p]]\n"
                     "   add %w[t], %w[t], #1\n"
                     "   stlxr %w[s], %w[t], [%[p]]\n"
                     "   cbnz %w[s], 1b\n"
                     "   mrs %[out], nzcv\n"
                     : [t] "=&r"(tmp), [s] "=&r"(st), [out] "=&r"(out)
                     : [p] "r"(&mem), [in] "r"(in)
                     : "memory", "cc");
    CHECK((out >> 28) == flags, "NZCV preserved across an exclusive loop");
  }
  CHECK(mem == 7 + 16, "the sixteen loops each stored once");
  printf("test_nzcv: PASS\n");
}

// A loop whose base register is written by the body is not the fusable shape;
// it must still behave. Here the body recomputes the address from a second
// register, so the guest's own retry and ours have to agree.
static void test_body_writes_base(void) {
  volatile uint64_t mem[2] = {10, 20};
  uint64_t tmp;
  uint32_t st;
  uint64_t base = (uint64_t)&mem[0];
  __asm__ volatile("1: ldaxr %[t], [%[b]]\n"
                   "   add %[t], %[t], #1\n"
                   "   mov %[b], %[b]\n"
                   "   stlxr %w[s], %[t], [%[b]]\n"
                   "   cbnz %w[s], 1b\n"
                   : [t] "=&r"(tmp), [s] "=&r"(st), [b] "+r"(base)
                   : : "memory");
  CHECK(mem[0] == 11, "base-rewriting loop stored once");
  CHECK(mem[1] == 20, "base-rewriting loop left its neighbour alone");
  printf("test_body_writes_base: PASS\n");
}

int main(void) {
  test_exchange();
  test_nzcv();
  test_body_writes_base();
  test_contention();
  printf("ALL EXCLUSIVE CONTENTION TESTS PASSED\n");
  return 0;
}
