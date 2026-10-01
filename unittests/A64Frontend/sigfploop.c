// SPDX-License-Identifier: MIT
//
// A signal taken inside a loop whose live floating-point state is in D16-D31,
// with the handler reading -- and editing -- the fpsimd_context of the frame.
//
// Under POWERarm those guest registers are pinned in the FPR-aliased low bank
// vs16-vs31 of the host VSX file. With POWERARM_VSXCLASSES=1 the register
// allocator stops copying them into the VMX half for every operation and lets
// the arithmetic read and write the pinned register in place
// (docs/powerarm/research/power-isa/VSX-REGISTER-CLASSES.md). That makes the
// guest write land at the producing instruction rather than at a trailing
// move -- the same hoist V0-V15 have lived with since M1 -- and this is the
// test that says so for the low bank:
//
//   * the handler must see the architectural value of every one of V16-V31 at
//     the instruction the signal interrupted, not a stale one and not the
//     other half of the register file;
//   * an edit the handler makes to the frame must reach the guest on resume,
//     which for a pinned register means the resume path has to put it back
//     into the pinned register and not into a pool copy;
//   * and the whole register file has to survive the round trip unchanged
//     otherwise.
//
// Two deliveries, both exact and repeatable:
//
//   1. SYNCHRONOUS. A `kill` syscall is issued from inline assembly with
//      D16-D31 live in that same asm block, so nothing between setting them
//      and taking the signal can touch them -- a libc call could not be used
//      here, because the AArch64 PCS makes V16-V31 caller-saved. The handler
//      checks all sixteen and writes a sentinel into V20.
//   2. ASYNCHRONOUS, from a timer, inside an accumulate loop. The timing is
//      not repeatable, so the handler checks only what is true at EVERY
//      instruction boundary of that loop -- V18-V31 constant, V16 an exact
//      non-negative integer no larger than the trip count -- and the output
//      is the mismatch count, which is zero however the timer lands.
//
// Output is four fixed lines.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef FPSIMD_MAGIC
#define FPSIMD_MAGIC 0x46508001
#endif

// The payload each of V16-V31 carries into the synchronous delivery. Distinct
// in both doublewords, so a read of the wrong half of the VSX file, or of the
// wrong doubleword, is a different number rather than a coincidence.
static inline uint64_t lo_of(int r) {
  return 0x5147000000000000ull | ((uint64_t)r << 32) | (0x0A0B0000u + r);
}
static inline uint64_t hi_of(int r) {
  return 0x1234000000000000ull ^ ((uint64_t)r * 0x0101010101010101ull);
}

#define SENTINEL_LO 0xFEEDFACECAFEB00Dull
#define SENTINEL_HI 0x0BADC0DE12345678ull

static volatile int sync_seen;
static volatile int sync_bad;
static volatile int async_seen;
static volatile int async_bad;
static volatile int phase;          // 0 = synchronous, 1 = asynchronous
static volatile uint64_t async_trip;

static struct fpsimd_context* find_fpsimd(ucontext_t* uc) {
  struct _aarch64_ctx* h = (struct _aarch64_ctx*)uc->uc_mcontext.__reserved;
  while (h->magic != 0 && h->size != 0) {
    if (h->magic == FPSIMD_MAGIC) {
      return (struct fpsimd_context*)h;
    }
    h = (struct _aarch64_ctx*)((char*)h + h->size);
  }
  return NULL;
}

static void handler(int sig, siginfo_t* si, void* ctx) {
  (void)sig;
  (void)si;
  struct fpsimd_context* f = find_fpsimd((ucontext_t*)ctx);
  if (!f) {
    if (phase == 0) { sync_bad++; } else { async_bad++; }
    return;
  }

  if (phase == 0) {
    sync_seen++;
    for (int r = 16; r < 32; r++) {
      uint64_t lo = (uint64_t)f->vregs[r];
      uint64_t hi = (uint64_t)(f->vregs[r] >> 64);
      if (lo != lo_of(r) || hi != hi_of(r)) {
        sync_bad++;
      }
    }
    // The edit the guest has to observe on resume.
    f->vregs[20] = ((__uint128_t)SENTINEL_HI << 64) | SENTINEL_LO;
    return;
  }

  async_seen++;
  // V18-V31 are loaded once and never written by the loop, so they are
  // constant at every instruction boundary.
  for (int r = 18; r < 32; r++) {
    uint64_t lo = (uint64_t)f->vregs[r];
    uint64_t hi = (uint64_t)(f->vregs[r] >> 64);
    if (lo != lo_of(r) || hi != hi_of(r)) {
      async_bad++;
    }
  }
  // V16 is the accumulator: it counts up by one per iteration from zero, so
  // whenever the signal lands it holds an exact integer in range, and its
  // upper doubleword is zero because the guest only ever writes a D register.
  double acc;
  memcpy(&acc, &f->vregs[16], sizeof(acc));
  if (!(acc >= 0.0 && acc <= (double)async_trip && acc == (double)(uint64_t)acc)) {
    async_bad++;
  }
  if ((uint64_t)(f->vregs[16] >> 64) != 0) {
    async_bad++;
  }
}

// Load V16-V31 from `in`, raise SIGUSR1 with a bare syscall so that nothing
// between the loads and the delivery can touch them, then store all sixteen
// back to `out`. One asm block, no call, no spill the compiler could insert.
static void sync_round(const uint64_t* in, uint64_t* out, long pid) {
  register long x0 __asm__("x0") = pid;
  register long x1 __asm__("x1") = SIGUSR1;
  register long x8 __asm__("x8") = SYS_kill;
  __asm__ volatile(
    "ldp q16, q17, [%[in], #0]\n"
    "ldp q18, q19, [%[in], #32]\n"
    "ldp q20, q21, [%[in], #64]\n"
    "ldp q22, q23, [%[in], #96]\n"
    "ldp q24, q25, [%[in], #128]\n"
    "ldp q26, q27, [%[in], #160]\n"
    "ldp q28, q29, [%[in], #192]\n"
    "ldp q30, q31, [%[in], #224]\n"
    "svc #0\n"
    "stp q16, q17, [%[out], #0]\n"
    "stp q18, q19, [%[out], #32]\n"
    "stp q20, q21, [%[out], #64]\n"
    "stp q22, q23, [%[out], #96]\n"
    "stp q24, q25, [%[out], #128]\n"
    "stp q26, q27, [%[out], #160]\n"
    "stp q28, q29, [%[out], #192]\n"
    "stp q30, q31, [%[out], #224]\n"
    : "+r"(x0)
    : [in] "r"(in), [out] "r"(out), "r"(x1), "r"(x8)
    : "memory", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
      "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}

// The accumulate loop the timer fires into: V16 counts, V17 is the step, and
// V18-V31 are live constants the handler checks.
static double async_loop(const uint64_t* in, uint64_t trips, uint64_t* out) {
  double result;
  __asm__ volatile(
    "ldp q18, q19, [%[in], #32]\n"
    "ldp q20, q21, [%[in], #64]\n"
    "ldp q22, q23, [%[in], #96]\n"
    "ldp q24, q25, [%[in], #128]\n"
    "ldp q26, q27, [%[in], #160]\n"
    "ldp q28, q29, [%[in], #192]\n"
    "ldp q30, q31, [%[in], #224]\n"
    "fmov d16, xzr\n"
    "fmov d17, #1.0\n"
    "mov x9, %[trips]\n"
    "1:\n"
    "fadd d16, d16, d17\n"
    "subs x9, x9, #1\n"
    "b.ne 1b\n"
    "fmov %d[res], d16\n"
    "stp q18, q19, [%[out], #32]\n"
    "stp q20, q21, [%[out], #64]\n"
    "stp q22, q23, [%[out], #96]\n"
    "stp q24, q25, [%[out], #128]\n"
    "stp q26, q27, [%[out], #160]\n"
    "stp q28, q29, [%[out], #192]\n"
    "stp q30, q31, [%[out], #224]\n"
    : [res] "=w"(result)
    : [in] "r"(in), [out] "r"(out), [trips] "r"(trips)
    : "memory", "x9", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
      "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
  return result;
}

int main(void) {
  static uint64_t in[64], out[64];
  for (int r = 16; r < 32; r++) {
    in[(r - 16) * 2 + 0] = lo_of(r);
    in[(r - 16) * 2 + 1] = hi_of(r);
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, NULL);
  sigaction(SIGALRM, &sa, NULL);

  const long pid = (long)getpid();

  // --- 1. synchronous, four rounds ---------------------------------------
  int restored = 0, edited = 0;
  for (int round = 0; round < 4; round++) {
    memset(out, 0, sizeof(out));
    sync_round(in, out, pid);
    for (int r = 16; r < 32; r++) {
      const uint64_t lo = out[(r - 16) * 2 + 0], hi = out[(r - 16) * 2 + 1];
      if (r == 20) {
        edited += (lo == SENTINEL_LO && hi == SENTINEL_HI);
      } else {
        restored += (lo == lo_of(r) && hi == hi_of(r));
      }
    }
  }
  printf("sync   deliveries %d, frame mismatches %d\n", sync_seen, sync_bad);
  printf("sync   V16-V31 minus the edited one restored %d of 60\n", restored);
  printf("sync   handler edit of V20 observed %d of 4\n", edited);

  // --- 2. asynchronous, from a timer, inside the loop ---------------------
  phase = 1;
  async_trip = 4000000;
  struct itimerval it;
  it.it_value.tv_sec = 0;
  it.it_value.tv_usec = 2000;
  it.it_interval.tv_sec = 0;
  it.it_interval.tv_usec = 2000;
  setitimer(ITIMER_REAL, &it, NULL);
  memset(out, 0, sizeof(out));
  const double acc = async_loop(in, async_trip, out);
  it.it_value.tv_usec = 0;
  it.it_interval.tv_usec = 0;
  setitimer(ITIMER_REAL, &it, NULL);

  int live = 0;
  for (int r = 18; r < 32; r++) {
    live += (out[(r - 16) * 2 + 0] == lo_of(r) && out[(r - 16) * 2 + 1] == hi_of(r));
  }
  // The delivery COUNT is timing-dependent and deliberately not printed.
  printf("async  accumulator %.0f, live constants %d of 14, frame mismatches %d\n",
         acc, live, async_bad);
  return 0;
}
