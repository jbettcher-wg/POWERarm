// SPDX-License-Identifier: MIT
//
// PRFM: every hint kind, every addressing form, against every kind of page.
//
// Self-checking, so it needs no reference machine: a prefetch is a hint. The
// architecture forces it to change nothing a program can observe and forbids
// it from raising a data abort, whatever the address, so the required answer
// is "no signal, no byte moved, and the code around it still runs" -- and
// this file checks exactly that.
//
// Why it exists: the A64 handlers used to discard the hint and emit nothing.
// They now lower PLD* to `dcbt` and PST* to `dcbtst`, which on POWER are
// defined not to cause a data storage exception -- a touch of an unmapped, a
// protected or an mtrack-write-protected page is dropped by the hardware.
// That is the whole correctness argument, and this test is what pins it:
// every address below is one where a real load or store would fault, or
// where the 128-byte host cache line runs off the end of the mapping.
//
// `control-unmapped-page-really-faults` is the positive control: it proves
// the PROT_NONE page used above is genuinely inaccessible, so that "no
// signal" means the prefetch was dropped rather than that the test prefetched
// a readable page by accident.

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int Failures = 0;
static int Checks = 0;

static sigjmp_buf FaultJump;
static volatile sig_atomic_t SawFault;

static void fault_handler(int Sig, siginfo_t* Info, void* UC) {
  (void)Sig;
  (void)Info;
  (void)UC;
  SawFault = 1;
  siglongjmp(FaultJump, 1);
}

// Every hint kind at the plain [Xn] form, plus the other three addressing
// forms. Each prefetch is followed by an addition the compiler cannot fold
// away, so the block's own result proves the instructions after the prefetch
// still executed -- a prefetch that corrupted a register or skipped an
// instruction shows up as a wrong sum rather than as nothing at all.
static uint64_t prefetch_all(const void* p, uint64_t Index) {
  uint64_t Sum = 0;
  asm volatile("prfm pldl1keep, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pldl1strm, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pldl2keep, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pldl2strm, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pldl3keep, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pldl3strm, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm plil1keep, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm plil2keep, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm plil3strm, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pstl1keep, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pstl1strm, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pstl2keep, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pstl3strm, [%1]\n\t"
               "add %0, %0, #1\n\t"
               // The unallocated prfops: type 11, and the level-11 encodings
               // (SLC only from ARMv8.7; this guest profile is an A76).
               "prfm #0x18, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm #0x1f, [%1]\n\t"
               "add %0, %0, #1\n\t"
               "prfm #0x06, [%1]\n\t"
               "add %0, %0, #1\n\t"
               // Unsigned scaled offset, both directions of the unscaled
               // form, and the register index with and without a scale and
               // with a 32-bit extend.
               "prfm pldl1keep, [%1, #8]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pstl1keep, [%1, #4088]\n\t"
               "add %0, %0, #1\n\t"
               "prfum pldl1keep, [%1, #-1]\n\t"
               "add %0, %0, #1\n\t"
               "prfum pstl1strm, [%1, #255]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pldl1keep, [%1, %2]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pldl1keep, [%1, %2, lsl #3]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pstl1keep, [%1, %w2, uxtw #3]\n\t"
               "add %0, %0, #1\n\t"
               "prfm pstl1keep, [%1, %w2, sxtw]\n\t"
               "add %0, %0, #1\n\t"
               // The literal forms, which target this very instruction
               // stream: a guest-code page, write-protected by the emulator's
               // own code tracking once this block is compiled.
               "prfm pldl1keep, 1f\n\t"
               "add %0, %0, #1\n\t"
               "prfm pstl1keep, 1f\n\t"
               "add %0, %0, #1\n\t"
               "1:\n\t"
               "add %0, %0, #1"
               : "+r"(Sum)
               : "r"(p), "r"(Index)
               : "memory");
  return Sum;
}

#define PREFETCH_COUNT 27

static void run_against(const char* What, const void* p, uint64_t Index) {
  ++Checks;
  SawFault = 0;
  if (sigsetjmp(FaultJump, 1) != 0) {
    printf("FAIL prfm %s: raised a signal\n", What);
    ++Failures;
    return;
  }
  const uint64_t Sum = prefetch_all(p, Index);
  if (Sum != PREFETCH_COUNT) {
    printf("FAIL prfm %s: %llu of %d instructions after the prefetches ran\n", What, (unsigned long long)Sum, PREFETCH_COUNT);
    ++Failures;
  }
}

int main(void) {
  struct sigaction SA;
  memset(&SA, 0, sizeof(SA));
  SA.sa_sigaction = fault_handler;
  SA.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSEGV, &SA, NULL);
  sigaction(SIGBUS, &SA, NULL);

  const long PageSize = sysconf(_SC_PAGESIZE);
  // Four pages: [0] read-write, [1] read-only, [2] PROT_NONE, [3] unmapped
  // (released below, so the page after the region is a hole as well).
  uint8_t* Region = mmap(NULL, (size_t)PageSize * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Region == MAP_FAILED) {
    printf("FAIL setup: mmap failed\n");
    printf("checks 0 failures 1\n");
    return 1;
  }
  uint8_t* RW = Region;
  uint8_t* RO = Region + PageSize;
  uint8_t* None = Region + PageSize * 2;
  uint8_t* Hole = Region + PageSize * 3;

  // A pattern over the writable page, re-checked at the end: not one byte of
  // it may move, least of all through a `dcbtst`, which asks for the line in
  // a state where a store MAY happen -- and must not perform one.
  for (long i = 0; i < PageSize; ++i) {
    RW[i] = (uint8_t)(i * 7 + 3);
  }

  if (mprotect(RO, (size_t)PageSize, PROT_READ) != 0 || mprotect(None, (size_t)PageSize, PROT_NONE) != 0 ||
      munmap(Hole, (size_t)PageSize) != 0) {
    printf("FAIL setup: mprotect/munmap failed\n");
    printf("checks 0 failures 1\n");
    return 1;
  }

  // A mapped, writable page: the ordinary case.
  run_against("mapped-rw", RW, 64);
  // Read-only: `dcbtst` asks for store permission the page does not grant.
  run_against("read-only", RO, 64);
  // PROT_NONE and an unmapped hole: a real access here is a SIGSEGV.
  run_against("prot-none", None, 64);
  run_against("unmapped-hole", Hole, 64);
  // The last byte of the mapping, so the host's 128-byte line -- and every
  // offset form above -- runs off the end of it.
  run_against("mapping-last-byte", RW + PageSize - 1, 64);
  // Addresses no mapping will ever cover.
  run_against("null-page", (const void*)0x1000, 64);
  run_against("non-canonical-ish", (const void*)0x00007FFFFFFFF000ULL, 64);
  // The index forms with an index that carries the address far out of any
  // mapping on its own.
  run_against("far-index", RW, 0x0000400000000000ULL);
  // Our own code page.
  run_against("code-page", (const void*)(uintptr_t)&main, 64);
  // The stack and a read-only data object.
  {
    uint64_t Local = 0;
    run_against("stack", &Local, 64);
  }

  // Not one byte of the writable page may have changed.
  {
    ++Checks;
    int Bad = -1;
    for (long i = 0; i < PageSize; ++i) {
      if (RW[i] != (uint8_t)(i * 7 + 3)) {
        Bad = (int)i;
        break;
      }
    }
    if (Bad >= 0) {
      printf("FAIL memory-unchanged: byte %d is %02x\n", Bad, RW[Bad]);
      ++Failures;
    }
  }

  // The positive control: the PROT_NONE page must really be inaccessible, or
  // "the prefetch did not fault" above would be a statement about a readable
  // page. Counted separately so it contributes no FAIL line.
  {
    SawFault = 0;
    volatile uint8_t Sink = 0;
    if (sigsetjmp(FaultJump, 1) == 0) {
      Sink = *(volatile uint8_t*)None;
      (void)Sink;
      printf("FAIL control-unmapped-page-really-faults: a load from the PROT_NONE page succeeded\n");
      ++Failures;
    } else {
      printf("PASS control-unmapped-page-really-faults\n");
    }
  }

  printf("checks %d failures %d\n", Checks, Failures);
  return Failures ? 1 : 0;
}
