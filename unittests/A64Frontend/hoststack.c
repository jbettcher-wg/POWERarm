// SPDX-License-Identifier: MIT
//
// The emulator's host stack must never reach the guest's stack.
//
// POWERarm runs the guest's main thread on the process's own host stack, and
// the ELF loader puts the guest's main stack near the top of the address
// space, where the host stack lives too. Each nested signal handler below
// keeps host stack in use for as long as it runs (the frames of the syscall
// it interrupted and the host context saved for it), so DEPTH levels take the
// host stack far below where it started. The loader used to place the guest
// stack flush against the bottom of the host stack's mapping, and the host
// frames then went straight into the top of the guest stack: the argument
// and environment strings, the auxiliary vector and main's frame. run.sh runs
// this test with ASLR off and an unlimited stack rlimit, the layout in which
// that placement was certain.
//
// Checks that every level ran and that the top of the stack, from main's
// frame to the end of the last string the kernel put there, is unchanged.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

#define DEPTH 400

extern char** environ;

static volatile int level, deepest;
static char saved[1 << 20];

static void on_usr1(int sig) {
  (void)sig;
  const int here = ++level;
  if (here > deepest) {
    deepest = here;
  }
  if (here < DEPTH) {
    raise(SIGUSR1);
  }
  --level;
}

static char* string_end(char* hi, const char* s) {
  char* end = (char*)s + strlen(s) + 1;
  return end > hi ? end : hi;
}

__attribute__((noinline)) static int run(char* lo, char** argv) {
  char* hi = lo;
  for (char** p = argv; *p; ++p) {
    hi = string_end(hi, *p);
  }
  for (char** p = environ; *p; ++p) {
    hi = string_end(hi, *p);
  }
  const char* execfn = (const char*)getauxval(AT_EXECFN);
  if (execfn) {
    hi = string_end(hi, execfn);
  }
  const size_t size = (size_t)(hi - lo);
  if (size > sizeof(saved)) {
    printf("stack top larger than %zu bytes\n", sizeof(saved));
    return 1;
  }
  memcpy(saved, lo, size);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_usr1;
  sa.sa_flags = SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, NULL);
  raise(SIGUSR1);

  const int same = memcmp(saved, lo, size) == 0;
  printf("nested handlers: %d of %d\n", deepest, DEPTH);
  printf("stack top: %s\n", same ? "unchanged" : "CHANGED");
  return same && deepest == DEPTH ? 0 : 1;
}

int main(int argc, char** argv) {
  (void)argc;
  // The low end of the checked range: everything above it belongs to main,
  // the C runtime's start-up frames and what the kernel put on the stack.
  char marker[64];
  memset(marker, 0x5a, sizeof(marker));
  __asm__ volatile("" ::"r"(marker) : "memory");
  return run(marker, argv);
}
