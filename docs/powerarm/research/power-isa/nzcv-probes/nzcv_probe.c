// NZCV observability probes for docs/powerarm/research/power-isa/NZCV-LIVENESS.md.
//
// Built static on the Pi 5 (gcc -static -O2), run there and under POWERarm.
// Each probe prints what a guest observer sees of PSTATE.NZCV at a boundary:
//   async    an interval timer's SIGALRM handler samples uc_mcontext.pstate
//            inside a loop whose compares are dead in-unit (POWERarm drops them)
//   edit     the handler clears Z in the frame once; the loop's B.NE must take it
//   sync     a faulting load between CMP and B.EQ; the SIGSEGV handler samples
//            NZCV and skips the load
//   bounds   NZCV across SVC, BL/RET, a delivered-and-returned signal,
//            swapcontext and siglongjmp
//   entry    flags produced in one unit and read at the entry of another:
//            BL target, after RET, far B.cond target (CSET / CCMP / ADC / B.cond)
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#include <setjmp.h>
#include <stdlib.h>

enum { P_ASYNC = 0, P_EDIT, P_SYNC, P_BOUNDS, NPROBES };
#define MAXOFF 32

static volatile int cur_probe;
static volatile uint64_t cur_base;
static volatile uint32_t hist[NPROBES][MAXOFF][16];
static volatile uint32_t hist_out[NPROBES][16];
static volatile uint32_t samples[NPROBES];
static volatile int edit_armed;
static volatile uint64_t edit_pc;
static volatile uint32_t edit_seen_nzcv;
static volatile int usr1_count;

static void handler(int sig, siginfo_t* si, void* ctx) {
  (void)si;
  ucontext_t* uc = (ucontext_t*)ctx;
  uint64_t pc = uc->uc_mcontext.pc;
  uint32_t nzcv = (uc->uc_mcontext.pstate >> 28) & 0xF;
  int p = cur_probe;
  int64_t off = (int64_t)(pc - cur_base);
  if (off >= 0 && off < MAXOFF * 4 && (off & 3) == 0) {
    hist[p][off / 4][nzcv]++;
  } else {
    hist_out[p][nzcv]++;
  }
  samples[p]++;
  if (sig == SIGUSR1) {
    usr1_count++;
  }
  if (p == P_EDIT && edit_armed && samples[p] >= 3) {
    edit_armed = 0;
    edit_pc = pc;
    edit_seen_nzcv = nzcv;
    uc->uc_mcontext.pstate &= ~(0xFull << 28); // clear NZCV: Z=0 makes B.NE taken
  }
  if (sig == SIGSEGV) {
    uc->uc_mcontext.pc += 4; // skip the faulting load
  }
}

// ---------------------------------------------------------------------------
// Assembly probes
// ---------------------------------------------------------------------------
__asm__(
  ".text\n"
  ".balign 64\n"
  ".globl flag_barrier\n.type flag_barrier,%function\n"
  "flag_barrier:\n"
  "  ret\n"

  // uint64_t loop_dropped(uint64_t n, uint64_t sentinel)
  // Sentinel written by MSR NZCV, kept alive across the BL exit. The loop's
  // three flag producers each have only in-unit B.cond readers and are dead
  // afterwards, so POWERarm's fusion drops them; hardware executes them.
  ".balign 64\n"
  ".globl loop_dropped\n.type loop_dropped,%function\n"
  "loop_dropped:\n"
  "  stp x29, x30, [sp, #-16]!\n"
  "  mov x2, #-1\n"
  "  msr nzcv, x1\n"
  "  bl flag_barrier\n"
  ".globl loop_dropped_top\n"
  "loop_dropped_top:\n"
  "  cmp x0, x0\n"          // +0   Z=1 C=1
  "  b.ne 9f\n"             // +4
  "  cmp x0, x2\n"          // +8   x0 - (-1): C=0, Z=0, N=0 for small x0
  "  b.eq 9f\n"             // +12
  "  subs x0, x0, #1\n"     // +16
  "  b.ne loop_dropped_top\n" // +20 back edge: drain point at loop_dropped_top
  "  cmp x0, x0\n"          // +24 kills the subs flags on the fall-through leg
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"
  "9:\n"
  "  mov x0, #-1\n"
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"

  // uint64_t loop_edit(uint64_t n): flags live around the loop, no writer in it.
  ".balign 64\n"
  ".globl loop_edit\n.type loop_edit,%function\n"
  "loop_edit:\n"
  "  cmp x0, x0\n"
  ".globl loop_edit_top\n"
  "loop_edit_top:\n"
  "  b.ne 9f\n"             // +0   taken only once a handler has cleared Z
  "  sub x0, x0, #1\n"      // +4
  "  cbnz x0, loop_edit_top\n" // +8 back edge
  "  mov x0, #0\n"
  "  ret\n"
  "9:\n"
  "  ret\n"                 // returns the remaining count

  // uint64_t loop_sync(uint64_t n, uint64_t* bad): a faulting load between CMP and B.EQ.
  ".balign 64\n"
  ".globl loop_sync\n.type loop_sync,%function\n"
  "loop_sync:\n"
  ".globl loop_sync_top\n"
  "loop_sync_top:\n"
  "  cmp x0, #7\n"          // +0
  "  ldr x2, [x1]\n"        // +4   SIGSEGV; the handler samples NZCV and skips it
  "  b.eq 9f\n"             // +8
  "  sub x0, x0, #1\n"      // +12
  "  cbnz x0, loop_sync_top\n" // +16
  "  mov x0, #0\n"
  "  ret\n"
  "9:\n"
  "  ret\n"                 // 7

  // uint64_t read_nzcv(void)
  ".balign 16\n"
  ".globl read_nzcv\n.type read_nzcv,%function\n"
  "read_nzcv:\n"
  "  mrs x0, nzcv\n"
  "  lsr x0, x0, #28\n"
  "  ret\n"

  // void write_nzcv(uint64_t nibble)
  ".globl write_nzcv\n.type write_nzcv,%function\n"
  "write_nzcv:\n"
  "  lsl x0, x0, #28\n"
  "  msr nzcv, x0\n"
  "  ret\n"

  // uint64_t nzcv_across_svc(uint64_t nibble): MSR ; SVC getpid ; MRS
  ".globl nzcv_across_svc\n.type nzcv_across_svc,%function\n"
  "nzcv_across_svc:\n"
  "  lsl x0, x0, #28\n"
  "  msr nzcv, x0\n"
  "  mov x8, #172\n"        // __NR_getpid
  "  svc #0\n"
  "  mrs x0, nzcv\n"
  "  lsr x0, x0, #28\n"
  "  ret\n"

  // uint64_t nzcv_across_call(uint64_t nibble): MSR ; BL leaf ; MRS
  ".globl nzcv_across_call\n.type nzcv_across_call,%function\n"
  "nzcv_across_call:\n"
  "  stp x29, x30, [sp, #-16]!\n"
  "  lsl x0, x0, #28\n"
  "  msr nzcv, x0\n"
  "  bl flag_barrier\n"
  "  mrs x0, nzcv\n"
  "  lsr x0, x0, #28\n"
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"

  // --- cross-unit entry readers ---
  // uint64_t reads_eq_at_entry(void): first instruction reads Z.
  ".balign 64\n"
  ".globl reads_eq_at_entry\n.type reads_eq_at_entry,%function\n"
  "reads_eq_at_entry:\n"
  "  cset x0, eq\n"
  "  ret\n"

  // uint64_t entry_bl(uint64_t a, uint64_t b): CMP ; BL reads_eq_at_entry
  ".balign 64\n"
  ".globl entry_bl\n.type entry_bl,%function\n"
  "entry_bl:\n"
  "  stp x29, x30, [sp, #-16]!\n"
  "  cmp x0, x1\n"
  "  bl reads_eq_at_entry\n"
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"

  // uint64_t entry_after_ret(uint64_t a, uint64_t b): CMP ; BL leaf ; CSET
  ".balign 64\n"
  ".globl entry_after_ret\n.type entry_after_ret,%function\n"
  "entry_after_ret:\n"
  "  stp x29, x30, [sp, #-16]!\n"
  "  cmp x0, x1\n"
  "  bl flag_barrier\n"
  "  cset x0, eq\n"
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"

  // uint64_t cmp_then_ret(uint64_t a, uint64_t b): the callee produces, the caller reads after RET.
  ".balign 64\n"
  ".globl cmp_then_ret\n.type cmp_then_ret,%function\n"
  "cmp_then_ret:\n"
  "  cmp x0, x1\n"
  "  ret\n"
  ".globl entry_ret_reader\n.type entry_ret_reader,%function\n"
  "entry_ret_reader:\n"
  "  stp x29, x30, [sp, #-16]!\n"
  "  bl cmp_then_ret\n"
  "  cset x0, lo\n"
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"

  // Far B.cond targets (more than the 128-byte region window away), each
  // reading NZCV as its first instruction.
  // uint64_t entry_far(uint64_t a, uint64_t b, uint64_t which)
  ".balign 64\n"
  ".globl entry_far\n.type entry_far,%function\n"
  "entry_far:\n"
  "  cmp x2, #1\n"
  "  b.eq 1f\n"
  "  cmp x2, #2\n"
  "  b.eq 2f\n"
  "  cmp x2, #3\n"
  "  b.eq 3f\n"
  "  cmp x0, x1\n"
  "  b.lo far_bcond\n"       // which == 0: far target starts with B.cond (reads V/N/Z)
  "  mov x0, #100\n"
  "  ret\n"
  "1:\n"
  "  cmp x0, x1\n"
  "  b far_cset\n"
  "2:\n"
  "  cmp x0, x1\n"
  "  b far_ccmp\n"
  "3:\n"
  "  cmp x0, x1\n"
  "  b far_adc\n"
  ".fill 96, 4, 0xd503201f\n"   // 384 bytes of NOP: farther than the window
  "far_bcond:\n"
  "  b.ge 4f\n"
  "  mov x0, #1\n"            // lo && !ge  -> a < b unsigned and signed
  "  ret\n"
  "4:\n"
  "  mov x0, #2\n"            // lo && ge   -> a < b unsigned, a >= b signed
  "  ret\n"
  ".fill 96, 4, 0xd503201f\n"
  "far_cset:\n"
  "  cset x0, hi\n"
  "  ret\n"
  ".fill 96, 4, 0xd503201f\n"
  "far_ccmp:\n"
  "  ccmp x0, #5, #0b0100, ne\n" // if a != b: flags = cmp a,5 ; else Z=1
  "  cset x0, eq\n"
  "  ret\n"
  ".fill 96, 4, 0xd503201f\n"
  "far_adc:\n"
  "  adc x0, x0, x1\n"        // a + b + C
  "  ret\n"
);

extern uint64_t loop_dropped(uint64_t n, uint64_t sentinel);
extern uint64_t loop_edit(uint64_t n);
extern uint64_t loop_sync(uint64_t n, uint64_t* bad);
extern uint64_t read_nzcv(void);
extern void write_nzcv(uint64_t nibble);
extern uint64_t nzcv_across_svc(uint64_t nibble);
extern uint64_t nzcv_across_call(uint64_t nibble);
extern uint64_t entry_bl(uint64_t a, uint64_t b);
extern uint64_t entry_after_ret(uint64_t a, uint64_t b);
extern uint64_t entry_ret_reader(uint64_t a, uint64_t b);
extern uint64_t entry_far(uint64_t a, uint64_t b, uint64_t which);
extern char loop_dropped_top[], loop_edit_top[], loop_sync_top[];

static void print_hist(int p) {
  for (int off = 0; off < MAXOFF; off++) {
    for (int v = 0; v < 16; v++) {
      if (hist[p][off][v]) {
        printf("  pc=+%-3d nzcv=%X %s%s%s%s count=%u\n", off * 4, v, (v & 8) ? "N" : "-", (v & 4) ? "Z" : "-", (v & 2) ? "C" : "-",
               (v & 1) ? "V" : "-", hist[p][off][v]);
      }
    }
  }
  for (int v = 0; v < 16; v++) {
    if (hist_out[p][v]) {
      printf("  pc=outside nzcv=%X count=%u\n", v, hist_out[p][v]);
    }
  }
}

static void timer_on(long usec) {
  struct itimerval it = {{0, usec}, {0, usec}};
  setitimer(ITIMER_REAL, &it, NULL);
}
static void timer_off(void) {
  struct itimerval it = {{0, 0}, {0, 0}};
  setitimer(ITIMER_REAL, &it, NULL);
}

static ucontext_t co_main, co_other;
static volatile uint64_t co_seen;
static void coroutine(void) {
  co_seen = read_nzcv();
  write_nzcv(0x0);
  swapcontext(&co_other, &co_main);
}
static char co_stack[65536];
static sigjmp_buf jb;

int main(int argc, char** argv) {
  uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 0) : 100000000ull;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGALRM, &sa, NULL);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGUSR1, &sa, NULL);

  // --- async ---
  cur_probe = P_ASYNC;
  cur_base = (uint64_t)loop_dropped_top;
  timer_on(200);
  uint64_t r = loop_dropped(n, 0xA0000000ull);
  timer_off();
  printf("async: loop_dropped(n) = %llu, samples = %u (sentinel N-C-, hardware at +0 is --C-)\n", (unsigned long long)r, samples[P_ASYNC]);
  print_hist(P_ASYNC);

  // --- edit ---
  cur_probe = P_EDIT;
  cur_base = (uint64_t)loop_edit_top;
  edit_armed = 1;
  timer_on(200);
  r = loop_edit(n);
  timer_off();
  printf("edit: loop_edit(n) = %llu (nonzero: the handler's Z=0 was honoured), samples = %u, edit at pc=+%lld saw nzcv=%X\n",
         (unsigned long long)r, samples[P_EDIT], (long long)(edit_pc - cur_base), edit_seen_nzcv);
  print_hist(P_EDIT);

  // --- sync ---
  cur_probe = P_SYNC;
  cur_base = (uint64_t)loop_sync_top;
  r = loop_sync(2000, NULL);
  printf("sync: loop_sync(2000, NULL) = %llu (expect 7), faults = %u\n", (unsigned long long)r, samples[P_SYNC]);
  print_hist(P_SYNC);

  // --- bounds ---
  cur_probe = P_BOUNDS;
  cur_base = 0;
  printf("bounds: svc 0xB -> %llX, 0x5 -> %llX\n", (unsigned long long)nzcv_across_svc(0xB), (unsigned long long)nzcv_across_svc(0x5));
  printf("bounds: bl/ret 0xB -> %llX, 0x5 -> %llX\n", (unsigned long long)nzcv_across_call(0xB), (unsigned long long)nzcv_across_call(0x5));
  write_nzcv(0xB);
  raise(SIGUSR1);
  printf("bounds: raise(SIGUSR1) 0xB -> %llX (usr1 handled %d)\n", (unsigned long long)read_nzcv(), usr1_count);
  write_nzcv(0xD);
  kill(getpid(), SIGUSR1);
  printf("bounds: kill(self,SIGUSR1) 0xD -> %llX (usr1 handled %d)\n", (unsigned long long)read_nzcv(), usr1_count);
  getcontext(&co_other);
  co_other.uc_stack.ss_sp = co_stack;
  co_other.uc_stack.ss_size = sizeof co_stack;
  co_other.uc_link = &co_main;
  makecontext(&co_other, coroutine, 0);
  write_nzcv(0xF);
  swapcontext(&co_main, &co_other);
  printf("bounds: swapcontext 0xF -> coroutine saw %llX, wrote 0 -> back in main %llX\n", (unsigned long long)co_seen, (unsigned long long)read_nzcv());
  if (sigsetjmp(jb, 1) == 0) {
    write_nzcv(0x9);
    siglongjmp(jb, 1);
  }
  printf("bounds: siglongjmp with 0x9 written before the jump -> %llX\n", (unsigned long long)read_nzcv());
  write_nzcv(0x3);
  usleep(1000);
  printf("bounds: nanosleep 0x3 -> %llX\n", (unsigned long long)read_nzcv());

  // --- entry ---
  printf("entry: bl->cset eq (3,3)=%llu (3,4)=%llu\n", (unsigned long long)entry_bl(3, 3), (unsigned long long)entry_bl(3, 4));
  printf("entry: cmp;bl leaf;cset eq (3,3)=%llu (3,4)=%llu\n", (unsigned long long)entry_after_ret(3, 3), (unsigned long long)entry_after_ret(3, 4));
  printf("entry: bl cmp;ret;cset lo (3,4)=%llu (4,3)=%llu\n", (unsigned long long)entry_ret_reader(3, 4), (unsigned long long)entry_ret_reader(4, 3));
  printf("entry: far b.cond (1,2)=%llu (-1,2)=%llu (5,2)=%llu\n", (unsigned long long)entry_far(1, 2, 0), (unsigned long long)entry_far((uint64_t)-1, 2, 0),
         (unsigned long long)entry_far(5, 2, 0));
  printf("entry: far cset hi (1,2)=%llu (3,2)=%llu (2,2)=%llu\n", (unsigned long long)entry_far(1, 2, 1), (unsigned long long)entry_far(3, 2, 1),
         (unsigned long long)entry_far(2, 2, 1));
  printf("entry: far ccmp (5,1)=%llu (6,1)=%llu (2,2)=%llu\n", (unsigned long long)entry_far(5, 1, 2), (unsigned long long)entry_far(6, 1, 2),
         (unsigned long long)entry_far(2, 2, 2));
  printf("entry: far adc (1,2)=%llu (2,1)=%llu (2,2)=%llu\n", (unsigned long long)entry_far(1, 2, 3), (unsigned long long)entry_far(2, 1, 3),
         (unsigned long long)entry_far(2, 2, 3));
  return 0;
}
