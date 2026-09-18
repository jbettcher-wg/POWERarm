// SPDX-License-Identifier: MIT
//
// The EL0-readable half of the generic timer. V8 reads CNTVCT_EL0 as its
// high-resolution clock, so without it code-server's bundled Node dies on the
// first JIT warmup (MRS x0, CNTVCT_EL0, 0xd53be040).
//
// Exactly two registers are readable from EL0 on the reference machine. The
// Pi 5 (Cortex-A76, Linux 6.18) SIGILLs on CNTPCT_EL0, on the FEAT_ECV
// self-synchronising forms CNTVCTSS_EL0/CNTPCTSS_EL0 (the A76 has no FEAT_ECV,
// which our ID registers also say), and on CNTKCTL_EL1. Those must keep
// faulting here, so this test asserts the faults as firmly as the reads.
//
// Self-checking, so the Pi golden and the emulated run are the same text. That
// matters more than usual here: CNTFRQ_EL0 deliberately differs between the two
// machines -- the Pi reports 54000000 and POWERarm reports the real host
// timebase -- because CNTFRQ describes the counter the guest actually gets, and
// faking the Pi's value would cost a multiply-shift on every read (see the note
// in SystemRegisters.h). So this file must never print a frequency, a raw
// counter value, or anything derived from one except a ratio.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static sigjmp_buf JumpBuffer;
static void OnSigill(int Signal) {
  (void)Signal;
  siglongjmp(JumpBuffer, 1);
}

static void Check(const char* Name, int Ok) {
  printf("%s %s\n", Ok ? "PASS" : "FAIL", Name);
}

// Encodings are written in the generic s3_<op1>_c<CRn>_c<CRm>_<op2> form rather
// than by name: the corpus builds without -march, and the FEAT_ECV names would
// not assemble at the baseline the other tests use.
#define READ_SYS(Var, Trapped, Encoding)                      \
  do {                                                        \
    Var = 0;                                                  \
    Trapped = 0;                                              \
    if (sigsetjmp(JumpBuffer, 1) == 0) {                      \
      __asm__ __volatile__("mrs %0, " Encoding : "=r"(Var));  \
    } else {                                                  \
      Trapped = 1;                                            \
    }                                                         \
  } while (0)

#define CNTFRQ_EL0   "s3_3_c14_c0_0"
#define CNTPCT_EL0   "s3_3_c14_c0_1"
#define CNTVCT_EL0   "s3_3_c14_c0_2"
#define CNTPCTSS_EL0 "s3_3_c14_c0_5"
#define CNTVCTSS_EL0 "s3_3_c14_c0_6"
#define CNTKCTL_EL1  "s3_0_c14_c1_0"

static double Seconds(const struct timespec* T) {
  return (double)T->tv_sec + (double)T->tv_nsec / 1e9;
}

int main(void) {
  struct sigaction Action;
  memset(&Action, 0, sizeof Action);
  Action.sa_handler = OnSigill;
  Action.sa_flags = SA_NODEFER;
  sigaction(SIGILL, &Action, NULL);

  uint64_t Value;
  int Trapped;

  // The two that must be readable.
  READ_SYS(Value, Trapped, CNTFRQ_EL0);
  const uint64_t Frequency = Value;
  Check("cntfrq.readable", !Trapped);
  // Nonzero matters on its own: a guest divides by this, and every arm64 clock
  // source treats zero as "timer not configured".
  Check("cntfrq.nonzero", !Trapped && Frequency != 0);

  READ_SYS(Value, Trapped, CNTVCT_EL0);
  Check("cntvct.readable", !Trapped);

  // The four that must fault, exactly as they do on the Pi.
  READ_SYS(Value, Trapped, CNTPCT_EL0);
  Check("cntpct.sigill", Trapped);
  READ_SYS(Value, Trapped, CNTVCTSS_EL0);
  Check("cntvctss.sigill", Trapped);
  READ_SYS(Value, Trapped, CNTPCTSS_EL0);
  Check("cntpctss.sigill", Trapped);
  READ_SYS(Value, Trapped, CNTKCTL_EL1);
  Check("cntkctl.sigill", Trapped);

  if (Frequency == 0) {
    // Nothing below is meaningful without a frequency to divide by, and it
    // would divide by zero. The failures above already say what went wrong.
    return 0;
  }

  // Monotonic and actually advancing, measured against CLOCK_MONOTONIC over a
  // fixed wall-clock interval rather than a fixed iteration count -- the loop
  // runs at wildly different speeds on the two machines, and the checks below
  // are ratios so that does not matter.
  struct timespec StartTime, NowTime;
  uint64_t Start, Now = 0;
  int WentBackwards = 0;

  __asm__ __volatile__("mrs %0, " CNTVCT_EL0 : "=r"(Start));
  clock_gettime(CLOCK_MONOTONIC, &StartTime);
  uint64_t Previous = Start;
  for (;;) {
    __asm__ __volatile__("mrs %0, " CNTVCT_EL0 : "=r"(Now));
    if (Now < Previous) {
      WentBackwards = 1;
    }
    Previous = Now;
    clock_gettime(CLOCK_MONOTONIC, &NowTime);
    if (Seconds(&NowTime) - Seconds(&StartTime) >= 0.10) {
      break;
    }
  }

  Check("cntvct.monotonic", !WentBackwards);
  Check("cntvct.advances", Now > Start);

  // The counter's own idea of elapsed time must match the kernel's. This is
  // what catches CNTFRQ describing a different clock than CNTVCT reads -- the
  // one way these two registers can be individually plausible and jointly
  // wrong.
  const double ByCounter = (double)(Now - Start) / (double)Frequency;
  const double ByClock = Seconds(&NowTime) - Seconds(&StartTime);
  const double Error = (ByCounter > ByClock ? ByCounter - ByClock : ByClock - ByCounter) / ByClock;
  Check("cntvct.tracks_clock_1pct", Error < 0.01);
  Check("cntvct.tracks_clock_5pct", Error < 0.05);
  return 0;
}
