// SPDX-License-Identifier: MIT
//
// A signal taken BETWEEN two reads of the same guest V register inside one
// guest block, whose handler edits that register in the frame.
//
// It was written as the correctness gate for a frontend vector register cache
// (docs/powerarm/research/power-isa/VSX-STAGE2-AND-VECTOR-CACHE.md section 5),
// which was built, measured at 0.022% once the memory ops became VSX-clean,
// and NOT landed -- see the branch named in HANDOVER.md. The test outlived it,
// because the property it pins is the resume path's, not the cache's: it is
// the only test in the suite where a handler EDITS a vector register in the
// frame from a signal delivered mid-block. Everything else either reads the
// frame or takes the signal at a block boundary.
//
// Were such a cache ever landed, this is what would catch it: the first read
// of V20 would leave the value in a dynamic VMX pool register and the second
// would be served from it rather than from the pinned vs20, while a handler's
// edit reaches only CPUState and the pinned register -- so a resume that let
// the interrupted translation continue would silently lose the edit.
//
// What makes it safe is not the cache's own discipline but the resume path:
// RestoreFrame_Arm64 (LinuxSyscalls/SignalDelegator/GuestFramesManagement.cpp)
// compares the frame with the state it was built from, and when a handler has
// edited anything and the signal arrived in JIT code, it does NOT resume the
// interrupted host context -- it resumes through the dispatcher at the frame's
// PC, which refills every static register from CPUState and enters a fresh
// translation. No in-flight SSA value survives that, cached or not. This test
// is what says so, and it is the one test that would fail if that path were
// ever changed to resume the host context on an edited frame.
//
// sigfploop covers the same registers but its synchronous delivery is an `svc`,
// which ends the guest block, so no cached value is live across it. The
// delivery here is a SIGSEGV from a PROT_NONE page in the MIDDLE of a block
// that reads V20 on both sides of it -- deterministic, no timer, so the test
// is exact rather than probabilistic.
//
// Self-checking: every line is PASS or FAIL, and the run is worthless unless
// the positive control at the end FAILs. No golden is needed or used; the
// answers are forced by the architecture, not by this host.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef FPSIMD_MAGIC
#define FPSIMD_MAGIC 0x46508001
#endif

// The value V20 carries in, and the one the handler replaces it with. Distinct
// in both doublewords and in every 32-bit lane, so a partial write, a
// doubleword swap or a read of the other half of the VSX file is a different
// number rather than a coincidence.
#define IN_LO 0x1111111122222222ull
#define IN_HI 0x3333333344444444ull
#define ED_LO 0xAAAAAAAABBBBBBBBull
#define ED_HI 0xCCCCCCCCDDDDDDDDull

static void* guard;
static volatile int delivered;
static volatile int edit_done;
static volatile int frame_ok;
static volatile int want_edit;

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
  delivered++;
  // Let the faulting load through on resume.
  mprotect(guard, 4096, PROT_READ | PROT_WRITE);
  struct fpsimd_context* f = find_fpsimd((ucontext_t*)ctx);
  if (!f) {
    return;
  }
  // The architectural value of V20 at the interrupted instruction. Every store
  // the translation made is in place, so this must be what the guest set.
  if ((uint64_t)f->vregs[20] == IN_LO && (uint64_t)(f->vregs[20] >> 64) == IN_HI) {
    frame_ok = 1;
  }
  if (want_edit) {
    f->vregs[20] = ((__uint128_t)ED_HI << 64) | ED_LO;
    edit_done = 1;
  }
}

// One guest block that reads V20, faults, and reads V20 again. The two reads
// are four guest instructions apart, inside the cache's window, so with the
// cache on the second is served from the pool copy of the first -- unless the
// resume threw that copy away, which is the property under test.
//
// `out0` receives V20 as read before the fault, `out1` as read after it. The
// faulting load targets a PROT_NONE page; the handler makes it readable, so on
// resume the load completes and execution continues in the same block.
static void vround(const uint64_t* in, uint64_t* out0, uint64_t* out1, volatile void* bad) {
  __asm__ volatile(
    "ldr q20, [%[in]]\n"
    // Read 1 of V20.
    "mov v20.d[0], v20.d[0]\n"   // no-op that keeps V20 live and read
    "str q20, [%[o0]]\n"
    // The fault, mid-block.
    "ldr q5, [%[bad]]\n"
    // Read 2 of V20, after the handler edited it.
    "str q20, [%[o1]]\n"
    :
    : [in] "r"(in), [o0] "r"(out0), [o1] "r"(out1), [bad] "r"(bad)
    : "memory", "v5", "v20");
}

static int check(const char* name, int ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  return ok;
}

int main(void) {
  static uint64_t in[2], out0[2], out1[2];
  in[0] = IN_LO;
  in[1] = IN_HI;

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);

  guard = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (guard == MAP_FAILED) {
    printf("FAIL mmap-guard\n");
    return 1;
  }

  // --- 1. the handler EDITS V20 mid-block -------------------------------
  want_edit = 1;
  delivered = edit_done = frame_ok = 0;
  memset(out0, 0, sizeof(out0));
  memset(out1, 0, sizeof(out1));
  mprotect(guard, 4096, PROT_NONE);
  vround(in, out0, out1, guard);

  check("delivered-once", delivered == 1);
  check("frame-held-architectural-V20", frame_ok == 1);
  check("handler-edited", edit_done == 1);
  check("read-before-fault-unedited", out0[0] == IN_LO && out0[1] == IN_HI);
  // The property this test exists for: the read AFTER the edited frame must see
  // the edit, not a cached pool copy of the pre-signal value.
  const int after_ok = (out1[0] == ED_LO && out1[1] == ED_HI);
  check("read-after-fault-sees-handler-edit", after_ok);

  // --- 2. the handler does NOT edit: the frame must round-trip exactly ---
  want_edit = 0;
  delivered = frame_ok = 0;
  memset(out0, 0, sizeof(out0));
  memset(out1, 0, sizeof(out1));
  mprotect(guard, 4096, PROT_NONE);
  vround(in, out0, out1, guard);

  check("unedited-delivered-once", delivered == 1);
  check("unedited-frame-held-architectural-V20", frame_ok == 1);
  check("unedited-V20-survives-round-trip",
        out0[0] == IN_LO && out0[1] == IN_HI && out1[0] == IN_LO && out1[1] == IN_HI);

  // --- 3. positive control ----------------------------------------------
  // A run whose checks all pass proves nothing unless a deliberately wrong
  // check is reported as a failure. One such test has shipped here before.
  check("control-deliberately-wrong", 0);
  return 0;
}
