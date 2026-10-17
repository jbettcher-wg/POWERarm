/* Does this POWER9 implement the ISA 3.0 atomic-memory-operation instructions
 * (lwat/ldat/stwat/stdat) that ISA-OPPORTUNITIES 3.14 proposes for the LSE RMW
 * lowering? Each is tried under a SIGILL/SIGBUS/SIGSEGV handler. */
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
static sigjmp_buf jb;
static volatile int trapped;
static void h(int s) { trapped = s; siglongjmp(jb, 1); }
static uint64_t slot[8] __attribute__((aligned(64)));
#define TRY(label, stmt)                                                     \
  do {                                                                       \
    trapped = 0;                                                             \
    if (sigsetjmp(jb, 1) == 0) { stmt; printf("%-8s EXECUTED\n", label); }    \
    else { printf("%-8s trapped with signal %d\n", label, trapped); }         \
  } while (0)
int main(void) {
  struct sigaction sa; sa.sa_handler = h; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
  sigaction(SIGILL, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGSEGV, &sa, 0); sigaction(SIGTRAP, &sa, 0);
  uint64_t* p = slot; uint64_t out = 0, in = 1;
  printf("# FC=0 is Fetch and Add for the load forms, Store Add for the store forms.\n");
  TRY("ldat", __asm__ volatile("ldat %0,%1,0" : "=&r"(out) : "r"(p) : "memory"));
  TRY("lwat", __asm__ volatile("lwat %0,%1,0" : "=&r"(out) : "r"(p) : "memory"));
  TRY("stdat", __asm__ volatile("stdat %0,%1,0" :: "r"(in), "r"(p) : "memory"));
  TRY("stwat", __asm__ volatile("stwat %0,%1,0" :: "r"(in), "r"(p) : "memory"));
  printf("# slot[0] = %llu (nonzero means an add actually happened)\n", (unsigned long long)slot[0]);
  return 0;
}
