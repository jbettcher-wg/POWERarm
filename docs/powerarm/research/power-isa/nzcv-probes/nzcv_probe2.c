// Second NZCV probe set (see nzcv_probe.c): pure-asm boundaries, and a
// synchronous sample of CR0/XER inside a loop whose compares are dead.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static volatile uint32_t fault_hist[16];
static volatile uint32_t faults;
static volatile uint64_t fault_pc, base_pc;
static volatile int usr1;

static void handler(int sig, siginfo_t* si, void* ctx) {
  (void)si;
  ucontext_t* uc = (ucontext_t*)ctx;
  if (sig == SIGSEGV) {
    fault_hist[(uc->uc_mcontext.pstate >> 28) & 0xF]++;
    faults++;
    fault_pc = uc->uc_mcontext.pc;
    uc->uc_mcontext.pc += 4;
  } else {
    usr1++;
  }
}

__asm__(
  ".text\n"
  ".balign 64\n"
  ".globl barrier2\n.type barrier2,%function\n"
  "barrier2:\n"
  "  ret\n"

  // uint64_t nzcv_across_signal(uint64_t nibble, uint64_t pid): MSR ; SVC kill(pid, SIGUSR1) ; MRS
  ".balign 64\n"
  ".globl nzcv_across_signal\n.type nzcv_across_signal,%function\n"
  "nzcv_across_signal:\n"
  "  lsl x0, x0, #28\n"
  "  msr nzcv, x0\n"
  "  mov x0, x1\n"
  "  mov x1, #10\n"         // SIGUSR1
  "  mov x8, #129\n"        // __NR_kill
  "  svc #0\n"
  "  mrs x0, nzcv\n"
  "  lsr x0, x0, #28\n"
  "  ret\n"

  // uint64_t loop_dropped_fault(uint64_t n, uint64_t sentinel, uint64_t* bad)
  // Same shape as loop_dropped, with a faulting load after the fused B.NE:
  // the SIGSEGV handler samples PSTATE.NZCV where the unit's own compares
  // are dead. Hardware shows the CMP's -ZC-; POWERarm shows CR0/XER as the
  // unit inherited them.
  ".balign 64\n"
  ".globl loop_dropped_fault\n.type loop_dropped_fault,%function\n"
  "loop_dropped_fault:\n"
  "  stp x29, x30, [sp, #-16]!\n"
  "  msr nzcv, x1\n"
  "  bl barrier2\n"
  ".globl loop_dropped_fault_top\n"
  "loop_dropped_fault_top:\n"
  "  cmp x0, x0\n"          // +0   Z=1 C=1; only reader is the fused B.NE
  "  b.ne 9f\n"             // +4
  "  ldr x3, [x2]\n"        // +8   faults; the handler samples NZCV and skips it
  "  subs x0, x0, #1\n"     // +12
  "  b.ne loop_dropped_fault_top\n" // +16
  "  cmp x0, x0\n"          // +20
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"
  "9:\n"
  "  mov x0, #-1\n"
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"

  // uint64_t loop_kept_fault(uint64_t n, uint64_t* bad): the CMP's reader is
  // after the faulting load, so the compare has to be kept (MayRaiseSignal).
  ".balign 64\n"
  ".globl loop_kept_fault\n.type loop_kept_fault,%function\n"
  "loop_kept_fault:\n"
  ".globl loop_kept_fault_top\n"
  "loop_kept_fault_top:\n"
  "  cmp x0, x0\n"          // +0
  "  ldr x3, [x1]\n"        // +4   faults
  "  b.ne 9f\n"             // +8
  "  subs x0, x0, #1\n"     // +12
  "  b.ne loop_kept_fault_top\n" // +16
  "  cmp x0, x0\n"
  "  ret\n"
  "9:\n"
  "  mov x0, #-1\n"
  "  ret\n"

  // uint64_t entry_via_syscall(uint64_t nibble): MSR ; SVC getpid ; (new unit) NOP ; MRS
  // The continuation after SVC is a unit of its own that reads NZCV at entry.
  ".balign 64\n"
  ".globl entry_via_syscall\n.type entry_via_syscall,%function\n"
  "entry_via_syscall:\n"
  "  lsl x0, x0, #28\n"
  "  msr nzcv, x0\n"
  "  mov x8, #172\n"
  "  svc #0\n"
  "  nop\n"
  "  mrs x0, nzcv\n"
  "  lsr x0, x0, #28\n"
  "  ret\n"

  // uint64_t sentinel_then_loop_then_read(uint64_t n, uint64_t nibble):
  // MSR ; BL ; (unit) loop with dead compares ; MRS after the loop.
  // Hardware: the last SUBS (Z=1 at exit). POWERarm: whatever it kept.
  ".balign 64\n"
  ".globl sentinel_loop_read\n.type sentinel_loop_read,%function\n"
  "sentinel_loop_read:\n"
  "  stp x29, x30, [sp, #-16]!\n"
  "  lsl x1, x1, #28\n"
  "  msr nzcv, x1\n"
  "  bl barrier2\n"
  "1:\n"
  "  cmp x0, x0\n"
  "  b.ne 9f\n"
  "  subs x0, x0, #1\n"
  "  b.ne 1b\n"
  "  mrs x0, nzcv\n"        // reads the SUBS flags: live, so SUBS is kept
  "  lsr x0, x0, #28\n"
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"
  "9:\n"
  "  mov x0, #-1\n"
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"
);

extern uint64_t nzcv_across_signal(uint64_t nibble, uint64_t pid);
extern uint64_t loop_dropped_fault(uint64_t n, uint64_t sentinel, uint64_t* bad);
extern uint64_t loop_kept_fault(uint64_t n, uint64_t* bad);
extern uint64_t entry_via_syscall(uint64_t nibble);
extern uint64_t sentinel_loop_read(uint64_t n, uint64_t nibble);
extern char loop_dropped_fault_top[], loop_kept_fault_top[];

static void print_fault_hist(const char* name, uint64_t r) {
  printf("%s: result=%lld faults=%u fault pc=+%lld\n", name, (long long)r, faults, (long long)(fault_pc - base_pc));
  for (int v = 0; v < 16; v++) {
    if (fault_hist[v]) {
      printf("  nzcv=%X %s%s%s%s count=%u\n", v, (v & 8) ? "N" : "-", (v & 4) ? "Z" : "-", (v & 2) ? "C" : "-", (v & 1) ? "V" : "-", fault_hist[v]);
    }
  }
  memset((void*)fault_hist, 0, sizeof fault_hist);
  faults = 0;
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGUSR1, &sa, NULL);

  printf("signal: msr 0xB ; svc kill(self,USR1) ; mrs -> %llX (handled %d)\n", (unsigned long long)nzcv_across_signal(0xB, getpid()), usr1);
  printf("signal: msr 0x5 ; svc kill(self,USR1) ; mrs -> %llX (handled %d)\n", (unsigned long long)nzcv_across_signal(0x5, getpid()), usr1);
  printf("entry via syscall: 0xB -> %llX, 0x5 -> %llX\n", (unsigned long long)entry_via_syscall(0xB), (unsigned long long)entry_via_syscall(0x5));
  printf("sentinel A, loop, mrs -> %llX (hardware: 6 = -ZC- from the last SUBS)\n", (unsigned long long)sentinel_loop_read(1000, 0xA));

  base_pc = (uint64_t)loop_dropped_fault_top;
  print_fault_hist("dropped_fault sentinel A", loop_dropped_fault(1000, 0xA0000000ull, NULL));
  base_pc = (uint64_t)loop_dropped_fault_top;
  print_fault_hist("dropped_fault sentinel 5", loop_dropped_fault(1000, 0x50000000ull, NULL));
  base_pc = (uint64_t)loop_kept_fault_top;
  print_fault_hist("kept_fault", loop_kept_fault(1000, NULL));
  return 0;
}
