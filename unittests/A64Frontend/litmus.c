// SPDX-License-Identifier: MIT
//
// Memory-ordering litmus tests. Each one runs a classic shape (store
// buffering, message passing, IRIW) many times across threads and checks that
// an outcome the AArch64 memory model FORBIDS never appears. Only forbidden
// outcomes are asserted, never how often an allowed weak outcome shows up, so
// the output is PASS/FAIL text that is the same on every correct machine and
// can be compared byte for byte with the Pi.
//
// Why this exists: single-threaded tests cannot see reordering. POWERarm maps
// AArch64's other-multi-copy-atomic model onto POWER's weaker non-multi-copy-
// atomic one with explicit fences (DMB/DSB and LDAR/STLR/LDAPR in the frontend,
// a hwsync/isync bracket around every atomic in AtomicOps.cpp), and the first
// time those fences were missing, guests lost wakeups under load. Checklist row
// P7 wants to make those fences lighter. It is only safe to do with these tests
// in place, and each test here guards a specific choice P7 will make:
//
//  * SB/MP with DMB, LDAR/STLR, LDAPR: the barrier and acquire/release
//    mappings themselves.
//  * SB/MP with LSE read-modify-writes carrying A/L bits: the ordering an
//    atomic with acquire or release semantics must still provide.
//  * SB/MP with *relaxed* LSE RMWs plus a DMB: the gate for dropping the fence
//    bracket from relaxed RMWs. The ordering there comes from the DMB, so these
//    must keep passing once the RMW's own fences are gone.
//  * SB+LDADDL/LDADDA: a release followed by an acquire is ordered on AArch64
//    (the acquire and release forms are RCsc), which is what forbids a
//    Dekker-shaped handoff from failing. This is the case a switch from the
//    backend's leading-sync convention to trailing-sync could break.
//  * IRIW with LDAR and with DMB: AArch64 forbids two readers disagreeing on the
//    order of two independent writes once their loads are ordered. POWER only
//    forbids it with hwsync between the loads, not lwsync, so this is the test
//    that catches an over-eager lwsync.
//
// The contended atomics at the end (LDADD, CASAL, LDUMAX, LDSMIN counters)
// check atomicity rather than ordering. LDUMAX and LDSMIN under contention are
// what finally exercise the CAS retry loop's back edge in AtomicMinMax, which
// no single-threaded test can reach.
//
// Run with --controls for the sensitivity checks: the same shapes with no
// ordering at all, where the weak outcome is ALLOWED. They print how often it
// was seen and are not part of the golden, because how often is not
// deterministic. What they show is that the harness can observe weak behaviour
// at all on the machine it runs on -- a litmus test that never sees anything is
// not evidence of anything.
//
// WHAT A PASS HERE DOES AND DOES NOT PROVE (measured on the POWER9 AC922,
// 2026-09-17, 100000 iterations, three runs each):
//
//  * Store->load and load->load reordering are readily visible. Plain SB shows
//    its weak outcome in roughly 1-24% of iterations and plain MP in about 1%,
//    so the fenced SB and MP tests passing is real evidence.
//  * Store->store reordering was NEVER observed: a plain unordered writer
//    against an LDAR reader (control:mp-plainwriter+ldar) gave 0 of 100000 on
//    every run, while the same control fired on the Pi's Cortex-A76 (1 of
//    100000). So the control works, and it is POWER9 that does not produce the
//    reordering; every weak MP outcome here comes from the reader side. These
//    tests therefore cannot catch a release-side mistake on this machine. The
//    Power ISA does permit the reordering, and another core or another timing
//    could expose it, so any release-side change in P7 has to be justified
//    from the ISA and must keep at least lwsync before a release, whatever this
//    file says.
//  * The external positive control is a POWERarm built with the hwsync/isync
//    bracket removed from every atomic RMW. Under it, mp+ldaddl+ldadda FAILs on
//    every run and everything else still passes: acquire-side weakening is
//    caught, release-side is not (as above), and RMW-against-RMW store
//    buffering is never seen even unfenced -- in practice a stcx. completing
//    orders the following larx, though the architecture does not promise it.
//    That build also aborted V8 in 3 of 4 runs, so the fences are load-bearing
//    in real code even where this file cannot show it.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// LDAPR is ARMv8.3; the corpus builds without -march.
__asm__(".arch armv8.3-a");

#define ITERS 100000
#define COUNT 100000
// POWER9's cache line is 128 bytes; keep every location on its own line on
// both machines so false sharing cannot mask or fake an outcome.
#define LINE 128

typedef struct {
  volatile uint32_t V;
  char Pad[LINE - sizeof(uint32_t)];
} __attribute__((aligned(LINE))) Cell;

static Cell *X, *Y;
static uint32_t *R0, *R1, *R2, *R3;
static volatile uint32_t Arrive __attribute__((aligned(LINE)));

// ---- AArch64 primitives. volatile + "memory" keeps the compiler from moving
// ---- them, so what is being tested is the hardware or the emulator.
static inline void St(volatile uint32_t* P, uint32_t V) {
  __asm__ __volatile__("str %w0, [%1]" ::"r"(V), "r"(P) : "memory");
}
static inline uint32_t Ld(volatile uint32_t* P) {
  uint32_t V;
  __asm__ __volatile__("ldr %w0, [%1]" : "=r"(V) : "r"(P) : "memory");
  return V;
}
static inline void Stlr(volatile uint32_t* P, uint32_t V) {
  __asm__ __volatile__("stlr %w0, [%1]" ::"r"(V), "r"(P) : "memory");
}
static inline uint32_t Ldar(volatile uint32_t* P) {
  uint32_t V;
  __asm__ __volatile__("ldar %w0, [%1]" : "=r"(V) : "r"(P) : "memory");
  return V;
}
static inline uint32_t Ldapr(volatile uint32_t* P) {
  uint32_t V;
  __asm__ __volatile__("ldapr %w0, [%1]" : "=r"(V) : "r"(P) : "memory");
  return V;
}
static inline void DmbIsh(void) { __asm__ __volatile__("dmb ish" ::: "memory"); }
static inline void DmbIshld(void) { __asm__ __volatile__("dmb ishld" ::: "memory"); }
static inline void DmbIshst(void) { __asm__ __volatile__("dmb ishst" ::: "memory"); }

#define RMW(Name, Mnemonic)                                                                        \
  static inline uint32_t Name(volatile uint32_t* P, uint32_t V) {                                  \
    uint32_t Old;                                                                                  \
    __asm__ __volatile__(Mnemonic " %w1, %w0, [%2]" : "=&r"(Old) : "r"(V), "r"(P) : "memory");     \
    return Old;                                                                                    \
  }
RMW(Ldadd, "ldadd")
RMW(Ldadda, "ldadda")
RMW(Ldaddl, "ldaddl")
RMW(Ldaddal, "ldaddal")
RMW(Swpl, "swpl")
RMW(Swpal, "swpal")
RMW(Ldumax, "ldumax")
RMW(Ldsmin, "ldsmin")

static inline uint32_t Casal(volatile uint32_t* P, uint32_t Expected, uint32_t Desired) {
  __asm__ __volatile__("casal %w0, %w1, [%2]" : "+r"(Expected) : "r"(Desired), "r"(P) : "memory");
  return Expected;
}

// ---- Harness. Threads meet at a counting barrier before every iteration so
// ---- their bodies overlap in time; each iteration uses fresh locations, so no
// ---- reset is needed between them.
static void Barrier(uint32_t Threads, uint32_t Round) {
  __atomic_fetch_add(&Arrive, 1, __ATOMIC_SEQ_CST);
  const uint32_t Target = (Round + 1) * Threads;
  while (__atomic_load_n(&Arrive, __ATOMIC_ACQUIRE) < Target) {
  }
}

typedef void (*Body)(uint32_t I);
typedef int (*Check)(uint32_t I);

struct Test {
  const char* Name;
  int Threads;
  Body Bodies[4];
  Check Forbidden;
  int Control;
};

struct Arg {
  Body B;
  uint32_t Threads;
};

static void* Worker(void* P) {
  const struct Arg* A = P;
  for (uint32_t I = 0; I < ITERS; ++I) {
    Barrier(A->Threads, I);
    A->B(I);
  }
  return NULL;
}

static uint32_t Run(const struct Test* T) {
  memset((void*)X, 0, ITERS * sizeof(Cell));
  memset((void*)Y, 0, ITERS * sizeof(Cell));
  memset(R0, 0, ITERS * sizeof(uint32_t));
  memset(R1, 0, ITERS * sizeof(uint32_t));
  memset(R2, 0, ITERS * sizeof(uint32_t));
  memset(R3, 0, ITERS * sizeof(uint32_t));
  Arrive = 0;
  pthread_t Th[4];
  struct Arg A[4];
  for (int I = 0; I < T->Threads; ++I) {
    A[I] = (struct Arg){T->Bodies[I], (uint32_t)T->Threads};
    pthread_create(&Th[I], NULL, Worker, &A[I]);
  }
  for (int I = 0; I < T->Threads; ++I) {
    pthread_join(Th[I], NULL);
  }
  uint32_t Bad = 0;
  for (uint32_t I = 0; I < ITERS; ++I) {
    Bad += T->Forbidden(I) ? 1 : 0;
  }
  return Bad;
}

#define x (&X[I].V)
#define y (&Y[I].V)

// ---- Store buffering: T0 writes x then reads y, T1 writes y then reads x.
// ---- Both reading 0 means both reads were satisfied before either write.
static int SBBad(uint32_t I) { return R0[I] == 0 && R1[I] == 0; }

static void SBPlain0(uint32_t I) { St(x, 1); R0[I] = Ld(y); }
static void SBPlain1(uint32_t I) { St(y, 1); R1[I] = Ld(x); }
static void SBDmb0(uint32_t I) { St(x, 1); DmbIsh(); R0[I] = Ld(y); }
static void SBDmb1(uint32_t I) { St(y, 1); DmbIsh(); R1[I] = Ld(x); }
static void SBRel0(uint32_t I) { Stlr(x, 1); R0[I] = Ldar(y); }
static void SBRel1(uint32_t I) { Stlr(y, 1); R1[I] = Ldar(x); }
static void SBSwpal0(uint32_t I) { Swpal(x, 1); R0[I] = Ldar(y); }
static void SBSwpal1(uint32_t I) { Swpal(y, 1); R1[I] = Ldar(x); }
static void SBAddal0(uint32_t I) { Ldaddal(x, 1); R0[I] = Ldaddal(y, 0); }
static void SBAddal1(uint32_t I) { Ldaddal(y, 1); R1[I] = Ldaddal(x, 0); }
static void SBAddLA0(uint32_t I) { Ldaddl(x, 1); R0[I] = Ldadda(y, 0); }
static void SBAddLA1(uint32_t I) { Ldaddl(y, 1); R1[I] = Ldadda(x, 0); }
static void SBAddRlxDmb0(uint32_t I) { Ldadd(x, 1); DmbIsh(); R0[I] = Ldadd(y, 0); }
static void SBAddRlxDmb1(uint32_t I) { Ldadd(y, 1); DmbIsh(); R1[I] = Ldadd(x, 0); }

// ---- Message passing: T0 writes data x then flag y, T1 reads flag y then data
// ---- x. Seeing the flag but not the data is the forbidden outcome.
static int MPBad(uint32_t I) { return R0[I] == 1 && R1[I] == 0; }

static void MPPlain0(uint32_t I) { St(x, 1); St(y, 1); }
static void MPPlain1(uint32_t I) { R0[I] = Ld(y); R1[I] = Ld(x); }
static void MPDmb0(uint32_t I) { St(x, 1); DmbIsh(); St(y, 1); }
static void MPDmb1(uint32_t I) { R0[I] = Ld(y); DmbIsh(); R1[I] = Ld(x); }
static void MPDmbSt0(uint32_t I) { St(x, 1); DmbIshst(); St(y, 1); }
static void MPDmbLd1(uint32_t I) { R0[I] = Ld(y); DmbIshld(); R1[I] = Ld(x); }
static void MPStlr0(uint32_t I) { St(x, 1); Stlr(y, 1); }
static void MPLdar1(uint32_t I) { R0[I] = Ldar(y); R1[I] = Ld(x); }
static void MPLdapr1(uint32_t I) { R0[I] = Ldapr(y); R1[I] = Ld(x); }
static void MPAddL0(uint32_t I) { St(x, 1); Ldaddl(y, 1); }
static void MPAddA1(uint32_t I) { R0[I] = Ldadda(y, 0); R1[I] = Ld(x); }
static void MPSwpl0(uint32_t I) { St(x, 1); Swpl(y, 1); }
static void MPAddRlxDmb0(uint32_t I) { St(x, 1); DmbIsh(); Ldadd(y, 1); }
static void MPAddRlxDmb1(uint32_t I) { R0[I] = Ldadd(y, 0); DmbIsh(); R1[I] = Ld(x); }
// Writer unordered, reader ordered: isolates whether this machine ever lets
// two stores to different lines become visible out of order. If it never
// does, release-side weakening cannot be caught empirically here and has to
// be argued from the architecture instead.

// ---- IRIW: two independent writers, two readers reading in opposite orders.
// ---- The readers disagreeing on which write happened first is forbidden once
// ---- each reader's loads are ordered.
static int IRIWBad(uint32_t I) { return R0[I] == 1 && R1[I] == 0 && R2[I] == 1 && R3[I] == 0; }

static void IRIWW0(uint32_t I) { St(x, 1); }
static void IRIWW1(uint32_t I) { St(y, 1); }
static void IRIWLdar2(uint32_t I) { R0[I] = Ldar(x); R1[I] = Ldar(y); }
static void IRIWLdar3(uint32_t I) { R2[I] = Ldar(y); R3[I] = Ldar(x); }
static void IRIWDmb2(uint32_t I) { R0[I] = Ld(x); DmbIsh(); R1[I] = Ld(y); }
static void IRIWDmb3(uint32_t I) { R2[I] = Ld(y); DmbIsh(); R3[I] = Ld(x); }

#undef x
#undef y

static const struct Test Tests[] = {
  {"sb+dmb.ish", 2, {SBDmb0, SBDmb1}, SBBad, 0},
  {"sb+stlr+ldar", 2, {SBRel0, SBRel1}, SBBad, 0},
  {"sb+swpal+ldar", 2, {SBSwpal0, SBSwpal1}, SBBad, 0},
  {"sb+ldaddal", 2, {SBAddal0, SBAddal1}, SBBad, 0},
  {"sb+ldaddl+ldadda", 2, {SBAddLA0, SBAddLA1}, SBBad, 0},
  {"sb+ldadd.relaxed+dmb", 2, {SBAddRlxDmb0, SBAddRlxDmb1}, SBBad, 0},
  {"mp+dmb.ish", 2, {MPDmb0, MPDmb1}, MPBad, 0},
  {"mp+dmb.ishst+dmb.ishld", 2, {MPDmbSt0, MPDmbLd1}, MPBad, 0},
  {"mp+stlr+ldar", 2, {MPStlr0, MPLdar1}, MPBad, 0},
  {"mp+stlr+ldapr", 2, {MPStlr0, MPLdapr1}, MPBad, 0},
  {"mp+ldaddl+ldadda", 2, {MPAddL0, MPAddA1}, MPBad, 0},
  {"mp+swpl+ldar", 2, {MPSwpl0, MPLdar1}, MPBad, 0},
  {"mp+ldadd.relaxed+dmb", 2, {MPAddRlxDmb0, MPAddRlxDmb1}, MPBad, 0},
  {"iriw+ldar", 4, {IRIWW0, IRIWW1, IRIWLdar2, IRIWLdar3}, IRIWBad, 0},
  {"iriw+dmb.ish", 4, {IRIWW0, IRIWW1, IRIWDmb2, IRIWDmb3}, IRIWBad, 0},
  // Sensitivity controls: no ordering, so the weak outcome is ALLOWED.
  {"control:sb-plain", 2, {SBPlain0, SBPlain1}, SBBad, 1},
  {"control:mp-plain", 2, {MPPlain0, MPPlain1}, MPBad, 1},
  {"control:mp-plainwriter+ldar", 2, {MPPlain0, MPLdar1}, MPBad, 1},
};

// ---- Contended atomicity. No barrier: both threads hammer one location.
static volatile uint32_t Shared __attribute__((aligned(LINE)));
static volatile uint32_t Regressions __attribute__((aligned(LINE)));

static void* CountLdadd(void* P) {
  (void)P;
  for (uint32_t I = 0; I < COUNT; ++I) {
    Ldadd(&Shared, 1);
  }
  return NULL;
}
static void* CountCasal(void* P) {
  (void)P;
  for (uint32_t I = 0; I < COUNT; ++I) {
    uint32_t Old = Ld(&Shared);
    for (;;) {
      const uint32_t Seen = Casal(&Shared, Old, Old + 1);
      if (Seen == Old) {
        break;
      }
      Old = Seen;
    }
  }
  return NULL;
}
// LDUMAX with an increasing operand: memory only ever grows, so the old values
// one thread sees must never go down (coherence), and the final value must be
// the largest operand anyone applied.
static void* MaxUp(void* P) {
  const uint32_t Base = (uint32_t)(uintptr_t)P;
  uint32_t Prev = 0;
  for (uint32_t I = 0; I < COUNT; ++I) {
    const uint32_t Old = Ldumax(&Shared, 2 * I + Base);
    if (Old < Prev) {
      __atomic_fetch_add(&Regressions, 1, __ATOMIC_RELAXED);
    }
    Prev = Old;
  }
  return NULL;
}
// LDSMIN with a decreasing signed operand, the mirror image.
static void* MinDown(void* P) {
  const int32_t Base = (int32_t)(intptr_t)P;
  int32_t Prev = 0;
  for (int32_t I = 0; I < COUNT; ++I) {
    const int32_t Old = (int32_t)Ldsmin(&Shared, (uint32_t)(-2 * I - Base));
    if (Old > Prev) {
      __atomic_fetch_add(&Regressions, 1, __ATOMIC_RELAXED);
    }
    Prev = Old;
  }
  return NULL;
}

static void Pair(void* (*Fn)(void*), uint32_t Initial) {
  Shared = Initial;
  Regressions = 0;
  pthread_t A, B;
  pthread_create(&A, NULL, Fn, (void*)(uintptr_t)0);
  pthread_create(&B, NULL, Fn, (void*)(uintptr_t)1);
  pthread_join(A, NULL);
  pthread_join(B, NULL);
}

static void Report(const char* Name, int Ok) { printf("%s %s\n", Ok ? "PASS" : "FAIL", Name); }

int main(int Argc, char** Argv) {
  const int Controls = Argc > 1 && strcmp(Argv[1], "--controls") == 0;
  X = aligned_alloc(LINE, ITERS * sizeof(Cell));
  Y = aligned_alloc(LINE, ITERS * sizeof(Cell));
  R0 = calloc(ITERS, sizeof(uint32_t));
  R1 = calloc(ITERS, sizeof(uint32_t));
  R2 = calloc(ITERS, sizeof(uint32_t));
  R3 = calloc(ITERS, sizeof(uint32_t));
  if (!X || !Y || !R0 || !R1 || !R2 || !R3) {
    return 2;
  }

  for (size_t T = 0; T < sizeof(Tests) / sizeof(Tests[0]); ++T) {
    if (Tests[T].Control != Controls) {
      continue;
    }
    const uint32_t Bad = Run(&Tests[T]);
    if (Controls) {
      printf("%s %s (%u of %u)\n", Bad ? "OBSERVED" : "NOT-OBSERVED", Tests[T].Name, Bad, ITERS);
    } else {
      Report(Tests[T].Name, Bad == 0);
    }
  }
  if (Controls) {
    return 0;
  }

  Pair(CountLdadd, 0);
  Report("count.ldadd.relaxed", Shared == 2u * COUNT);
  Pair(CountCasal, 0);
  Report("count.casal", Shared == 2u * COUNT);
  Pair(MaxUp, 0);
  Report("ldumax.contended.final", Shared == 2u * (COUNT - 1) + 1);
  Report("ldumax.contended.monotonic", Regressions == 0);
  Pair(MinDown, 0);
  Report("ldsmin.contended.final", (int32_t)Shared == -2 * (COUNT - 1) - 1);
  Report("ldsmin.contended.monotonic", Regressions == 0);
  return 0;
}
