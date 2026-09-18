// SPDX-License-Identifier: MIT
//
// Counts the clock_gettime and gettimeofday system calls a loop of libc clock
// reads makes. With a working vDSO the answer is zero: glibc calls the vDSO's
// __kernel_* entry points, which on the Pi read the clock in user space and
// under POWERarm are guest->host thunks. Without one (POWERarm before its guest
// vDSO existed), every read is a syscall.
//
// The count is deterministic: a seccomp filter makes both syscalls fail with
// EDOM instead of running, so a read that took the syscall path returns -1 and
// sets errno to EDOM, and one that did not returns 0. Raw syscall() reads are
// the positive control: they must all be caught, on both machines.
//
// CLOCK_MONOTONIC and CLOCK_REALTIME only: the arm64 kernel vDSO itself falls
// back to the syscall for the CPU-time clocks.
//
// Under POWERarm the guest's seccomp filters are emulated, and only with
// POWERARM_NEEDSSECCOMP=1 (run.sh reads it from vdso_syscalls.env).
//
// The seccomp ABI is spelled out below rather than taken from <linux/*.h>, so
// the file builds against a libc without kernel headers (musl) too.
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define N 1000

struct Insn {
  uint16_t code;
  uint8_t jt, jf;
  uint32_t k;
};
struct Program {
  unsigned short len;
  struct Insn* filter;
};
#define LD_W_ABS 0x20 // BPF_LD | BPF_W | BPF_ABS
#define JEQ_K 0x15    // BPF_JMP | BPF_JEQ | BPF_K
#define RET_K 0x06    // BPF_RET | BPF_K
#define OFFSET_NR 0   // struct seccomp_data: int nr; __u32 arch; ...
#define OFFSET_ARCH 4
#define ARCH_AARCH64 0xc00000b7u // AUDIT_ARCH_AARCH64
#define RET_ALLOW 0x7fff0000u
#define RET_ERRNO 0x00050000u
#define MODE_FILTER 1 // SECCOMP_SET_MODE_FILTER

static int InstallFilter(void) {
  struct Insn Filter[] = {
    {LD_W_ABS, 0, 0, OFFSET_ARCH},
    {JEQ_K, 1, 0, ARCH_AARCH64},
    {RET_K, 0, 0, RET_ALLOW},
    {LD_W_ABS, 0, 0, OFFSET_NR},
    {JEQ_K, 2, 0, SYS_clock_gettime},
    {JEQ_K, 1, 0, SYS_gettimeofday},
    {RET_K, 0, 0, RET_ALLOW},
    {RET_K, 0, 0, RET_ERRNO | EDOM},
  };
  struct Program Program = {.len = sizeof(Filter) / sizeof(Filter[0]), .filter = Filter};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    printf("FAIL prctl(PR_SET_NO_NEW_PRIVS) errno %d\n", errno);
    return 0;
  }
  if (syscall(SYS_seccomp, MODE_FILTER, 0, &Program) != 0) {
    printf("FAIL seccomp(SECCOMP_SET_MODE_FILTER) errno %d\n", errno);
    return 0;
  }
  return 1;
}

// A read that went through the syscall comes back as -1/EDOM; any other
// failure is reported separately so it cannot pass for a zero count.
static int Syscalls, Other;
static void Tally(int Result) {
  if (Result == -1 && errno == EDOM) {
    ++Syscalls;
  } else if (Result != 0) {
    ++Other;
  }
}

static void Report(const char* Name) {
  printf("%s: %d of %d reads were syscalls, %d other failures\n", Name, Syscalls, N, Other);
  Syscalls = 0;
  Other = 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (!InstallFilter()) {
    return 1;
  }

  struct timespec T;
  struct timeval TV;

  for (int i = 0; i < N; ++i) {
    errno = 0;
    Tally(clock_gettime(CLOCK_MONOTONIC, &T));
  }
  Report("clock_gettime(CLOCK_MONOTONIC)");

  for (int i = 0; i < N; ++i) {
    errno = 0;
    Tally(clock_gettime(CLOCK_REALTIME, &T));
  }
  Report("clock_gettime(CLOCK_REALTIME)");

  for (int i = 0; i < N; ++i) {
    errno = 0;
    Tally(gettimeofday(&TV, NULL));
  }
  Report("gettimeofday");

  // Positive control: the filter sees syscalls that are made.
  for (int i = 0; i < N; ++i) {
    errno = 0;
    Tally((int)syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &T));
  }
  Report("syscall(SYS_clock_gettime) control");

  for (int i = 0; i < N; ++i) {
    errno = 0;
    Tally((int)syscall(SYS_gettimeofday, &TV, NULL));
  }
  Report("syscall(SYS_gettimeofday) control");
  return 0;
}
