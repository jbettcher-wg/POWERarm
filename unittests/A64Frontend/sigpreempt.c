// SPDX-License-Identifier: MIT
//
// Asynchronous signals that redirect the guest the way Go's goroutine
// preemption does: the handler moves the interrupted PC into a trampoline,
// which saves NZCV and the registers it uses, clobbers the flags, restores
// everything and branches back to the interrupted PC. Hardware makes that
// exact at every instruction boundary, including between a compare and the
// B.cond that reads it.
//
// Under POWERarm an async signal is deferred and delivered at a drain point
// (a unit entry, or the backward leg of a branch), and the PC it reports comes
// from the unit's RIP table. When compare fusion has turned `cmp ; b.cc` into
// a direct compare and dropped the compare (its flags are dead afterwards),
// the flags were never written, so a drain point attributed to the B.cond
// would resume the B.cond on stale NZCV and take the wrong direction. The
// loops below are shaped so that the compare is dropped and the only drain
// point is the loop's back edge.
//
// The last loop takes the synchronous route: a load between the compare and
// its B.cond faults every iteration, and the SIGSEGV handler skips it, so the
// guest resumes at the B.cond through the signal frame. There the compare has
// to have been kept, fused branch or not.
//
// Output: one checksum per loop, each exact whatever the signal timing.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

uint64_t resume_pc;
static volatile uint64_t preempts;

void preempt_tramp(void);
void preempt_tramp_end(void);
uint64_t loop_cmp_lo(uint64_t n);
uint64_t loop_cmp_w_gt(uint64_t n);
uint64_t loop_subs_ne(uint64_t n);
uint64_t loop_cmn_ne(uint64_t n);
uint64_t loop_cmp_ne(uint64_t n);
uint64_t loop_cmp_w_hi(uint64_t n);
uint64_t loop_skip_fault(uint64_t n, uint64_t bad);

// The loops keep clear of x16 (the trampoline's scratch) and of the stack.
// Each ends with a second compare so that the loop compare's flags are dead
// on both legs of its branch.
__asm__(".text\n"
        ".globl preempt_tramp\n"
        "preempt_tramp:\n"
        "  sub sp, sp, #32\n"
        "  stp x0, x1, [sp]\n"
        "  mrs x0, nzcv\n"
        "  str x0, [sp, #16]\n"
        "  mov x1, #0\n"
        "  cmp x1, #1\n" // N=1 C=0: garbage flags
        "  cmn x0, x0\n"
        "  ldr x0, [sp, #16]\n"
        "  msr nzcv, x0\n"
        "  ldp x0, x1, [sp]\n"
        "  add sp, sp, #32\n"
        "  adrp x16, resume_pc\n"
        "  ldr x16, [x16, :lo12:resume_pc]\n"
        "  br x16\n"
        ".globl preempt_tramp_end\n"
        "preempt_tramp_end:\n"
        "  nop\n"

        // sum of 0..n-1: cmp x ; b.lo
        ".globl loop_cmp_lo\n"
        "loop_cmp_lo:\n"
        "  mov x1, #0\n"
        "  mov x2, #0\n"
        "1:\n"
        "  add x2, x2, x1\n"
        "  add x1, x1, #1\n"
        "  cmp x1, x0\n"
        "  b.lo 1b\n"
        "  cmp x2, #0\n"
        "  mov x0, x2\n"
        "  ret\n"

        // W counter from n down to 7, signed: cmp w, #imm ; b.gt
        ".globl loop_cmp_w_gt\n"
        "loop_cmp_w_gt:\n"
        "  mov x2, #0\n"
        "1:\n"
        "  eor x2, x2, x0, lsl #3\n"
        "  sub w0, w0, #1\n"
        "  cmp w0, #7\n"
        "  b.gt 1b\n"
        "  cmp x2, #0\n"
        "  mov x0, x2\n"
        "  ret\n"

        // subs ; b.ne (SubWithFlags)
        ".globl loop_subs_ne\n"
        "loop_subs_ne:\n"
        "  mov x2, #1\n"
        "1:\n"
        "  add x2, x2, x0, lsl #1\n"
        "  subs x0, x0, #1\n"
        "  b.ne 1b\n"
        "  cmp x2, #0\n"
        "  mov x0, x2\n"
        "  ret\n"

        // counter from -n up to -5: cmn x, #5 ; b.ne
        ".globl loop_cmn_ne\n"
        "loop_cmn_ne:\n"
        "  neg x1, x0\n"
        "  mov x2, #0\n"
        "1:\n"
        "  sub x2, x2, x1\n"
        "  add x1, x1, #1\n"
        "  cmn x1, #5\n"
        "  b.ne 1b\n"
        "  cmp x2, #0\n"
        "  mov x0, x2\n"
        "  ret\n"

        // cmp x, x ; b.ne
        ".globl loop_cmp_ne\n"
        "loop_cmp_ne:\n"
        "  mov x1, #0\n"
        "  mov x2, #3\n"
        "1:\n"
        "  eor x2, x2, x1\n"
        "  add x1, x1, #1\n"
        "  cmp x1, x0\n"
        "  b.ne 1b\n"
        "  cmp x2, #0\n"
        "  mov x0, x2\n"
        "  ret\n"

        // W counter across the sign boundary, unsigned: cmp w, w ; b.hi
        ".globl loop_cmp_w_hi\n"
        "loop_cmp_w_hi:\n"
        "  mov w1, #0x80000000\n"
        "  add w0, w1, w0\n"
        "  sub w1, w1, #16\n"
        "  mov x2, #0\n"
        "1:\n"
        "  add x2, x2, x0\n"
        "  sub w0, w0, #1\n"
        "  cmp w0, w1\n"
        "  b.hi 1b\n"
        "  cmp x2, #0\n"
        "  mov x0, x2\n"
        "  ret\n"

        // sum of 0..n-1 with a faulting load (x1 = bad address) between the
        // compare and the branch
        ".globl loop_skip_fault\n"
        "loop_skip_fault:\n"
        "  mov x4, x1\n"
        "  mov x1, #0\n"
        "  mov x2, #0\n"
        "1:\n"
        "  add x2, x2, x1\n"
        "  add x1, x1, #1\n"
        "  cmp x1, x0\n"
        "  ldr x3, [x4]\n"
        "  b.lo 1b\n"
        "  cmp x2, #0\n"
        "  mov x0, x2\n"
        "  ret\n");

#define BAD_ADDRESS 0x10ull
static volatile uint64_t skips;

// Skips the faulting load of loop_skip_fault; anything else is a real crash.
static void on_segv(int sig, siginfo_t* si, void* uc_) {
  ucontext_t* uc = uc_;
  if ((uint64_t)si->si_addr != BAD_ADDRESS) {
    signal(sig, SIG_DFL);
    return;
  }
  skips++;
  uc->uc_mcontext.pc += 4;
}

static void on_alarm(int sig, siginfo_t* si, void* uc_) {
  (void)sig;
  (void)si;
  ucontext_t* uc = uc_;
  const uint64_t pc = uc->uc_mcontext.pc;
  preempts++;
  // Not while the trampoline itself runs: it has not consumed resume_pc yet.
  if (pc >= (uint64_t)preempt_tramp && pc < (uint64_t)preempt_tramp_end) {
    return;
  }
  resume_pc = pc;
  uc->uc_mcontext.pc = (uint64_t)preempt_tramp;
}

static void timer(long usec) {
  struct itimerval it;
  memset(&it, 0, sizeof(it));
  it.it_interval.tv_usec = usec;
  it.it_value.tv_usec = usec;
  setitimer(ITIMER_REAL, &it, NULL);
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_alarm;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGALRM, &sa, NULL);

  const uint64_t n = 20000000;
  uint64_t r[6];
  timer(50);
  r[0] = loop_cmp_lo(n);
  r[1] = loop_cmp_w_gt(n);
  r[2] = loop_subs_ne(n);
  r[3] = loop_cmn_ne(n);
  r[4] = loop_cmp_ne(n);
  r[5] = loop_cmp_w_hi(n);
  timer(0);

  sa.sa_sigaction = on_segv;
  sigaction(SIGSEGV, &sa, NULL);
  const uint64_t faulting = loop_skip_fault(3000, BAD_ADDRESS);

  printf("cmp_lo    %016llx\n", (unsigned long long)r[0]);
  printf("cmp_w_gt  %016llx\n", (unsigned long long)r[1]);
  printf("subs_ne   %016llx\n", (unsigned long long)r[2]);
  printf("cmn_ne    %016llx\n", (unsigned long long)r[3]);
  printf("cmp_ne    %016llx\n", (unsigned long long)r[4]);
  printf("cmp_w_hi  %016llx\n", (unsigned long long)r[5]);
  printf("signals   %s\n", preempts > 100 ? "many" : "few");
  printf("fault     %016llx after %llu skipped loads\n", (unsigned long long)faulting, (unsigned long long)skips);
  return 0;
}
