// SPDX-License-Identifier: MIT
// NO-GOLDEN
//
// The libc mem-routine redirect (docs/powerarm/THUNKS-DESIGN.md 4.2;
// POWERARM_MEMREDIRECT / POWERARM_MEMREDIRECTTHRESHOLD).
//
// Self-checking, and NO GOLDEN IS EVER MINTED FOR IT. Every expected value here
// is computed by the test out of its own byte loops, never taken from a run: the
// AArch64 reference machine is gone, so a golden could only come from POWERarm's
// own output, which compares equal to a failing run.
//
// The property under test is that the redirect is INVISIBLE. The same binary must
// produce byte-identical output with the switch off, with it on at the default
// threshold, and with it on at a threshold that forces every call through the
// host (0) or no call at all (huge) -- which is how run.sh drives it. So nothing
// below prints a size, an address or a count that could differ between those
// runs; it prints PASS/FAIL per case only.
//
// The one case where the runs are allowed to differ is the reported guest PC of a
// fault taken inside a redirected copy, and this test deliberately does not look
// at it. See the fault cases at the bottom for what it does assert instead.

#include <setjmp.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int Failures = 0;
static int Checks = 0;

static void Check(int Ok, const char* Name) {
  printf("%s %s\n", Ok ? "PASS" : "FAIL", Name);
  ++Checks;
  if (!Ok) {
    ++Failures;
  }
}

// Does every byte of [D, D+Len) hold Pattern(Base + i)? This is the comparison
// every copy case rests on, so the positive control below corrupts a copy and
// requires THIS function to notice -- without which a test that compared nothing
// would print the same all-PASS output.
static unsigned char Pattern(size_t i);
static int RangeMatches(const unsigned char* D, size_t Base, size_t Len) {
  for (size_t i = 0; i < Len; ++i) {
    if (D[i] != Pattern(Base + i)) {
      return 0;
    }
  }
  return 1;
}

// Called through volatile pointers throughout, so that -O2 cannot turn any of
// this into an inline copy or fold it away. An inlined copy never reaches the
// hooked routine and would test nothing: the redirect hooks a CALL.
static void* (*volatile VMemcpy)(void*, const void*, size_t) = memcpy;
static void* (*volatile VMemmove)(void*, const void*, size_t) = memmove;
static void* (*volatile VMemset)(void*, int, size_t) = memset;

enum {
  Guard = 0x5a,
  MaxLen = 2048,
  Slack = 64,     // guard bytes either side of every destination range
  MaxAlign = 16,  // the alignment sweep shifts the range this far into the buffer
  // Room for the range, a guard band either side, AND the alignment shift. The
  // trailing guard band is what needs the extra MaxAlign: it sits at
  // Slack + align + MaxLen and would otherwise run off the end of the buffer,
  // reading heap that is not Guard and failing a check that is really correct.
  BufSize = MaxLen + 2 * Slack + MaxAlign,
};

static unsigned char Pattern(size_t i) {
  // Deterministic, and not a constant: a copy that is off by a byte, or that
  // copies the right count from the wrong offset, changes the result.
  return (unsigned char)(i * 31u + 7u);
}

// --- memcpy / memset over a size and alignment matrix -----------------------
//
// The sizes straddle every threshold run.sh uses, so each one is exercised on
// both sides of the gate across the suite's runs. Both sides have to produce the
// same bytes, which is the whole claim.
static const size_t Sizes[] = {
  0,   1,   2,   3,   4,   7,   8,   15,  16,  17,  31,   32,   33,   63,  64,
  65,  127, 128, 129, 192, 240, 255, 256, 257, 258, 319,  320,  384,  511, 512,
  513, 767, 768, 1023, 1024, 1025, 1535, 1536, 2047, 2048,
};

static void TestCopyMatrix(void) {
  unsigned char* Src = malloc(BufSize);
  unsigned char* Dst = malloc(BufSize);
  if (!Src || !Dst) {
    Check(0, "copy-matrix-alloc");
    return;
  }

  int CopyOk = 1, SetOk = 1, MoveOk = 1, RvOk = 1;

  for (size_t si = 0; si < sizeof(Sizes) / sizeof(Sizes[0]); ++si) {
    const size_t Len = Sizes[si];
    for (size_t SrcAlign = 0; SrcAlign < MaxAlign; ++SrcAlign) {
      for (size_t DstAlign = 0; DstAlign < MaxAlign; ++DstAlign) {
        unsigned char* S = Src + Slack + SrcAlign;
        unsigned char* D = Dst + Slack + DstAlign;

        for (size_t i = 0; i < Len; ++i) {
          S[i] = Pattern(i);
        }

        // memcpy: in range exact, out of range untouched, return value == dest.
        memset(Dst, Guard, BufSize);
        if (VMemcpy(D, S, Len) != D) {
          RvOk = 0;
        }
        for (size_t i = 0; i < Len; ++i) {
          if (D[i] != Pattern(i)) {
            CopyOk = 0;
          }
        }
        for (size_t i = 0; i < Slack; ++i) {
          if (D[-(ptrdiff_t)i - 1] != Guard || D[Len + i] != Guard) {
            CopyOk = 0; // over-copy in either direction
          }
        }

        // memmove over non-overlapping ranges must agree with memcpy.
        memset(Dst, Guard, BufSize);
        if (VMemmove(D, S, Len) != D) {
          RvOk = 0;
        }
        for (size_t i = 0; i < Len; ++i) {
          if (D[i] != Pattern(i)) {
            MoveOk = 0;
          }
        }
        for (size_t i = 0; i < Slack; ++i) {
          if (D[-(ptrdiff_t)i - 1] != Guard || D[Len + i] != Guard) {
            MoveOk = 0;
          }
        }

        // memset: the fill byte is the LOW byte of the int argument, so a value
        // with high bits set is a real case, not a curiosity.
        memset(Dst, Guard, BufSize);
        const int Fill = (int)(0x11223300u | (unsigned)(si & 0xff));
        if (VMemset(D, Fill, Len) != D) {
          RvOk = 0;
        }
        for (size_t i = 0; i < Len; ++i) {
          if (D[i] != (unsigned char)Fill) {
            SetOk = 0;
          }
        }
        for (size_t i = 0; i < Slack; ++i) {
          if (D[-(ptrdiff_t)i - 1] != Guard || D[Len + i] != Guard) {
            SetOk = 0;
          }
        }
      }
    }
  }

  Check(CopyOk, "memcpy-size-alignment-matrix");
  Check(MoveOk, "memmove-nonoverlap-matrix");
  Check(SetOk, "memset-size-alignment-matrix");
  Check(RvOk, "return-values-are-destination");
  free(Src);
  free(Dst);
}

// --- the gate boundary ------------------------------------------------------
//
// A threshold can only sit between two sizes, so the pair (n-1, n) for every
// power-of-two-ish n in the matrix is where an off-by-one in the compare would
// show. Run at several thresholds by run.sh, one of these pairs always straddles
// the live one.
static void TestGateBoundary(void) {
  unsigned char* Src = malloc(BufSize);
  unsigned char* Dst = malloc(BufSize);
  if (!Src || !Dst) {
    Check(0, "gate-boundary-alloc");
    return;
  }
  int Ok = 1;
  for (size_t Len = 1; Len <= 1026; ++Len) {
    unsigned char* S = Src + Slack;
    unsigned char* D = Dst + Slack;
    for (size_t i = 0; i < Len; ++i) {
      S[i] = Pattern(i + Len);
    }
    memset(Dst, Guard, BufSize);
    if (VMemcpy(D, S, Len) != D) {
      Ok = 0;
    }
    for (size_t i = 0; i < Len; ++i) {
      if (D[i] != Pattern(i + Len)) {
        Ok = 0;
      }
    }
    if (D[Len] != Guard || D[-1] != Guard) {
      Ok = 0;
    }
  }
  Check(Ok, "gate-boundary-every-length-to-1026");
  free(Src);
  free(Dst);
}

// --- memmove overlap, both directions ---------------------------------------
//
// The reference is a byte loop through a temporary, which IS memmove's
// definition ("as if copied via a temporary array"). Nothing here is compared
// against a recorded run.
static void TestMemmoveOverlap(void) {
  enum { Span = 4096 };
  unsigned char* Buf = malloc(Span);
  unsigned char* Ref = malloc(Span);
  unsigned char* Tmp = malloc(Span);
  if (!Buf || !Ref || !Tmp) {
    Check(0, "memmove-overlap-alloc");
    return;
  }

  int Up = 1, Down = 1;
  // Shifts smaller than, equal to and larger than the host half's internal
  // chunking boundaries, and sizes either side of the gate.
  static const size_t Lens[] = {1, 15, 64, 255, 256, 257, 512, 1024, 2048};
  static const size_t Shifts[] = {1, 2, 7, 8, 15, 16, 31, 64, 127, 256, 1024};

  for (size_t li = 0; li < sizeof(Lens) / sizeof(Lens[0]); ++li) {
    for (size_t hi = 0; hi < sizeof(Shifts) / sizeof(Shifts[0]); ++hi) {
      const size_t Len = Lens[li];
      const size_t Shift = Shifts[hi];
      if (Len + Shift + 32 >= Span) {
        continue;
      }

      // dest > src: must behave as if the source were read first.
      for (size_t i = 0; i < Span; ++i) {
        Buf[i] = Pattern(i);
        Ref[i] = Pattern(i);
      }
      memcpy(Tmp, Ref + 16, Len);
      memcpy(Ref + 16 + Shift, Tmp, Len);
      if (VMemmove(Buf + 16 + Shift, Buf + 16, Len) != Buf + 16 + Shift) {
        Up = 0;
      }
      if (memcmp(Buf, Ref, Span) != 0) {
        Up = 0;
      }

      // dest < src.
      for (size_t i = 0; i < Span; ++i) {
        Buf[i] = Pattern(i);
        Ref[i] = Pattern(i);
      }
      memcpy(Tmp, Ref + 16 + Shift, Len);
      memcpy(Ref + 16, Tmp, Len);
      if (VMemmove(Buf + 16, Buf + 16 + Shift, Len) != Buf + 16) {
        Down = 0;
      }
      if (memcmp(Buf, Ref, Span) != 0) {
        Down = 0;
      }
    }
  }

  Check(Up, "memmove-overlap-dest-above-src");
  Check(Down, "memmove-overlap-dest-below-src");
  free(Buf);
  free(Ref);
  free(Tmp);
}

// --- a copy whose destination the guest then executes -----------------------
//
// This is the self-modifying-code question tested rather than reasoned about: V8
// copies code with memcpy, and under the redirect those bytes are written by HOST
// libc, not by guest stores the JIT emitted.
//
// Under the default SMCChecks=icache the guest owes the announcement, and
// __builtin___clear_cache is what issues it (IC IVAU per line, then DSB/ISB) --
// exactly what a guest JIT does. Under mtrack the destination granules are
// unprotected and invalidated before the host write instead. Both must end up
// executing the NEW bytes, and the second copy over the same page is what catches
// a stale translation.
//
// The payload is hand-assembled rather than copied from a compiled function, so
// that it is position independent by construction and the test does not depend on
// what the compiler generated.
static void TestExecuteCopiedCode(void) {
  // SMCChecks=none promises "No checks", so nothing -- not the guest's IC IVAU,
  // not a host write, not a guest store from the JIT's own translation -- causes
  // a rewritten code page to be re-translated. Executing memcpy'd code is
  // therefore outside what that mode guarantees, and it fails there identically
  // with the redirect off and on, which is the property this file is about. Skip
  // rather than assert, and say so.
  const char* SMC = getenv("POWERARM_SMCCHECKS");
  if (SMC && strcmp(SMC, "none") == 0) {
    printf("SKIP execute-memcpyd-code-twice (SMCChecks=none guarantees no invalidation)\n");
    return;
  }

  const size_t PageSize = (size_t)sysconf(_SC_PAGESIZE);
  // Comfortably past every threshold run.sh uses, so the copy is redirected.
  const size_t Span = PageSize > 8192 ? PageSize : 8192;

  unsigned char* Code = mmap(NULL, Span, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Code == MAP_FAILED) {
    Check(0, "execute-copied-code-mmap");
    return;
  }

  unsigned char* Image = malloc(Span);
  if (!Image) {
    Check(0, "execute-copied-code-alloc");
    munmap(Code, Span);
    return;
  }

  int Ok = 1;
  // Two bodies: `movz w0, #imm ; ret`, then NOPs to the end of the span so the
  // copy is large enough to be redirected.
  for (int Round = 0; Round < 2; ++Round) {
    const unsigned Imm = Round == 0 ? 0x111u : 0x222u;
    const unsigned MovzW0 = 0x52800000u | (Imm << 5); // MOVZ Wd=0, #Imm, LSL #0
    const unsigned Ret = 0xd65f03c0u;                 // RET (X30)
    const unsigned Nop = 0xd503201fu;

    unsigned* Words = (unsigned*)Image;
    Words[0] = MovzW0;
    Words[1] = Ret;
    for (size_t i = 2; i < Span / 4; ++i) {
      Words[i] = Nop;
    }

    if (VMemcpy(Code, Image, Span) != Code) {
      Ok = 0;
    }
    // The writer announces its own instructions. This is the guest's obligation
    // under SMCChecks=icache and is harmless under every other mode.
    __builtin___clear_cache((char*)Code, (char*)Code + Span);

    unsigned (*Fn)(void);
    *(void**)&Fn = (void*)Code;
    if (Fn() != Imm) {
      Ok = 0;
    }
  }

  Check(Ok, "execute-memcpyd-code-twice");
  free(Image);
  munmap(Code, Span);
}

// --- a copy into an unmapped page -------------------------------------------
//
// With the redirect ON the faulting store is in HOST libc, so this is the case
// that proves the fault is reported to the GUEST as its own, rather than dying as
// POWERarm's "FATAL host fault".
//
// What is asserted is what must be identical with the switch off and on: that the
// guest's own handler ran, and that si_addr lies inside the page that is not
// mapped. What is NOT asserted is si_addr's exact offset or the reported PC --
// the host routine and guest glibc's need not fault on the same byte, and the PC
// is the one thing the two runs are allowed to differ on.
static volatile sig_atomic_t HandlerRan;
static volatile sig_atomic_t AddrInBadPage;
static volatile sig_atomic_t FixupMode;
static void* volatile BadPage;
static size_t volatile BadPageLen;
static sigjmp_buf Escape;

static void SegvHandler(int Sig, siginfo_t* Info, void* Ucontext) {
  (void)Sig;
  (void)Ucontext;
  HandlerRan = 1;
  const unsigned char* Addr = (const unsigned char*)Info->si_addr;
  const unsigned char* Base = (const unsigned char*)BadPage;
  AddrInBadPage = (Addr >= Base && Addr < Base + BadPageLen);

  if (FixupMode) {
    // The guard-page / GC-write-barrier pattern: make the page writable and
    // RETURN. The kernel then retries the faulting store -- the host routine's
    // store, with the redirect on -- and the copy completes.
    if (mprotect((void*)Base, BadPageLen, PROT_READ | PROT_WRITE) != 0) {
      _exit(97);
    }
    return;
  }
  siglongjmp(Escape, 1);
}

static void TestFaultIntoUnmappedPage(void) {
  const size_t PageSize = (size_t)sysconf(_SC_PAGESIZE);

  struct sigaction Action, Previous;
  memset(&Action, 0, sizeof(Action));
  Action.sa_sigaction = SegvHandler;
  Action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&Action.sa_mask);
  if (sigaction(SIGSEGV, &Action, &Previous) != 0) {
    Check(0, "fault-sigaction");
    return;
  }

  for (int Mode = 0; Mode < 2; ++Mode) {
    // Two adjacent pages; the second is taken away. A copy that starts in the
    // first and runs past its end has to fault in the second.
    unsigned char* Pair = mmap(NULL, PageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (Pair == MAP_FAILED) {
      Check(0, Mode ? "fault-fixup-mmap" : "fault-escape-mmap");
      continue;
    }
    unsigned char* Second = Pair + PageSize;
    if (mprotect(Second, PageSize, PROT_NONE) != 0) {
      Check(0, Mode ? "fault-fixup-mprotect" : "fault-escape-mprotect");
      munmap(Pair, PageSize * 2);
      continue;
    }

    unsigned char* Source = malloc(PageSize);
    if (!Source) {
      Check(0, "fault-alloc");
      munmap(Pair, PageSize * 2);
      continue;
    }
    for (size_t i = 0; i < PageSize; ++i) {
      Source[i] = Pattern(i + 3);
    }

    HandlerRan = 0;
    AddrInBadPage = 0;
    FixupMode = Mode;
    BadPage = Second;
    BadPageLen = PageSize;

    // Start 64 bytes before the boundary and copy 1024, so the copy is over any
    // threshold run.sh uses and must cross into the unmapped page.
    const size_t Lead = 64;
    const size_t Len = 1024;
    unsigned char* Dest = Second - Lead;

    int Completed = 0;
    if (sigsetjmp(Escape, 1) == 0) {
      VMemcpy(Dest, Source, Len);
      Completed = 1;
    }

    if (Mode == 0) {
      // Escaped through siglongjmp out of a fault taken inside the copy.
      Check(HandlerRan && !Completed, "fault-unmapped-reaches-guest-handler");
      Check(AddrInBadPage, "fault-unmapped-si_addr-in-faulting-page");
    } else {
      // Handler healed the mapping and returned; the copy finished, and every
      // byte of it is correct -- including the part written before the fault.
      int Ok = HandlerRan && Completed;
      for (size_t i = 0; i < Len; ++i) {
        if (Dest[i] != Pattern(i + 3)) {
          Ok = 0;
        }
      }
      Check(Ok, "fault-unmapped-handler-fixup-and-resume-completes-copy");
    }

    free(Source);
    munmap(Pair, PageSize * 2);
  }

  sigaction(SIGSEGV, &Previous, NULL);
}

// The negative control, in the style of ld1r/vins: a copy that IS corrupted, and
// the checker has to say so. Without this, a comparison that silently did
// nothing would produce the same all-PASS output as a working one.
static void TestControl(void) {
  enum { Len = 1024 };
  unsigned char* Src = malloc(Len);
  unsigned char* Dst = malloc(Len);
  if (!Src || !Dst) {
    Check(0, "control-alloc");
    free(Src);
    free(Dst);
    return;
  }
  for (size_t i = 0; i < Len; ++i) {
    Src[i] = Pattern(i);
  }
  VMemcpy(Dst, Src, Len);
  Check(RangeMatches(Dst, 0, Len), "control-uncorrupted-copy-matches");
  // One flipped byte in the middle, which the checker must catch.
  Dst[Len / 2] ^= 0xff;
  Check(!RangeMatches(Dst, 0, Len), "control-detects-corruption");
  free(Src);
  free(Dst);
}

int main(void) {
  TestControl();
  TestCopyMatrix();
  TestGateBoundary();
  TestMemmoveOverlap();
  TestExecuteCopiedCode();
  TestFaultIntoUnmappedPage();

  // Summary line in the shape run.sh already parses for ld1r/vins. A test that
  // died before printing one is a failure even if its status was 0.
  printf("checks %d failures %d\n", Checks, Failures);
  printf("%s memredirect-overall\n", Failures == 0 ? "PASS" : "FAIL");
  return Failures == 0 ? 0 : 1;
}
