// Third probe: a compare whose both legs stay in the unit (so POWERarm drops
// it), followed by a faulting load. The SIGSEGV handler samples PSTATE.NZCV.
// Hardware: the CMP's -ZC-. POWERarm: the flags the unit inherited at entry.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
static volatile uint32_t hist[16];
static volatile uint32_t faults;
static void handler(int sig, siginfo_t* si, void* ctx) {
  (void)sig; (void)si;
  ucontext_t* uc = (ucontext_t*)ctx;
  hist[(uc->uc_mcontext.pstate >> 28) & 0xF]++;
  faults++;
  uc->uc_mcontext.pc += 4;
}
__asm__(
  ".text\n.balign 64\n"
  ".globl barrier3\n.type barrier3,%function\n"
  "barrier3:\n  ret\n"
  // uint64_t loop_inunit_fault(uint64_t n, uint64_t sentinel, uint64_t* bad)
  ".balign 64\n"
  ".globl loop_inunit_fault\n.type loop_inunit_fault,%function\n"
  "loop_inunit_fault:\n"
  "  stp x29, x30, [sp, #-16]!\n"
  "  msr nzcv, x1\n"
  "  bl barrier3\n"
  "1:\n"
  "  cmp x0, x0\n"          // Z=1 C=1; readers: B.NE whose both legs stay in the unit
  "  b.ne 2f\n"
  "  nop\n"
  "2:\n"
  "  ldr x3, [x2]\n"        // faults; handler samples NZCV
  "  subs x0, x0, #1\n"
  "  b.ne 1b\n"
  "  cmp x0, x0\n"
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"
  // uint64_t loop_inunit_async(uint64_t n, uint64_t sentinel): same, no fault; for timer sampling elsewhere
);
extern uint64_t loop_inunit_fault(uint64_t n, uint64_t sentinel, uint64_t* bad);
static void show(const char* name, uint64_t r) {
  printf("%s: result=%lld faults=%u\n", name, (long long)r, faults);
  for (int v = 0; v < 16; v++) if (hist[v]) printf("  nzcv=%X %s%s%s%s count=%u\n", v, (v&8)?"N":"-", (v&4)?"Z":"-", (v&2)?"C":"-", (v&1)?"V":"-", hist[v]);
  memset((void*)hist, 0, sizeof hist); faults = 0;
}
int main(void) {
  struct sigaction sa; memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  show("inunit_fault sentinel A", loop_inunit_fault(1000, 0xA0000000ull, NULL));
  show("inunit_fault sentinel 5", loop_inunit_fault(1000, 0x50000000ull, NULL));
  show("inunit_fault sentinel 0", loop_inunit_fault(1000, 0x00000000ull, NULL));
  return 0;
}
