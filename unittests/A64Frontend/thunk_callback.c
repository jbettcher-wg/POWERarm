// SPDX-License-Identifier: MIT
//
// Host->guest callbacks through a thunk: the guest calls a thunk, the host
// function calls guest code back, the callbacks return to the host, and the
// thunk returns to the guest caller.
//
// POWERarm has a built-in thunk for exactly this, fex:callback_selftest
// (Source/Tools/LinuxEmulation/Thunks.cpp): given (Fn, Arg0, Arg1, Count) it
// calls Fn(Arg0, Arg1 + i) for i < Count through CallCallback, the path every
// thunked library's callbacks take. Under POWERarm that exercises the
// dispatcher's callback entry (X30 = ThunkCallbackRet, the crossing's X30 saved
// below SP), CallbackReturn (HLT #0x0F3E), and the crossing's full refill and
// return to X30 afterwards.
//
// The Pi has no such host: the stub's HLT raises SIGILL there, and the test
// then makes the same calls directly. Both machines print the same text, so
// the Pi run is the golden. thunk_callback.env sets THUNK_CALLBACK_REQUIRE_HOST
// for the POWERarm run, which then fails unless the host path was taken.
//
// What is checked, with results that do not depend on the machine:
//   basic       100 callbacks, their sum, the thunk's return value
//   callee      x19-x28 and d8-d15 of the thunk's caller survive a thunk whose
//               callbacks overwrite them all (and restore them, per AAPCS64);
//               SP too
//   nested      callbacks that call the thunk again, three levels deep
//   signals     a signal raised and handled inside a callback
//   alignment   SP is 16-byte aligned in every callback
//   threads     four threads running callbacks at once
//   async       an interval timer's signals landing wherever they land --
//               in callbacks, in the host between them, in the crossing --
//               while batches of callbacks run; every batch's count and sum
//               still come out right. Timing decides how many signals arrive,
//               so only the verdict is printed.
//   frames      timer signals that interrupt a loop inside a callback see the
//               loop's registers (X3-X8, held constant) in their ucontext
//   canary      the caller's stack frame is untouched
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <ucontext.h>

struct SelftestArgs {
  uint64_t Fn;
  uint64_t Arg0;
  uint64_t Arg1;
  uint64_t Count;
  uint64_t rv;
};

// The thunk stub: HLT #0x0F3F and sha256("fex:callback_selftest"), as
// ThunkLibs/include/common/Guest.h emits one.
void selftest_stub(struct SelftestArgs* Args);
asm(".text\n"
    ".balign 4\n"
    ".globl selftest_stub\n"
    ".type selftest_stub, %function\n"
    "selftest_stub:\n"
    ".inst 0xd441e7e0\n"
    ".byte 0xa1, 0xe7, 0xfc, 0xbd, 0x77, 0x1c, 0xd0, 0x3f, 0x15, 0xca, 0xcf, 0xe5, 0x75, 0x87, 0x1b, 0x83\n"
    ".byte 0x47, 0x66, 0xe2, 0xa0, 0xa7, 0xfd, 0x4a, 0xa4, 0xf4, 0x96, 0x50, 0x0e, 0xa5, 0x82, 0xa4, 0xa1\n"
    ".size selftest_stub, . - selftest_stub\n");

// uint64_t call_checking_callee_saved(void (*Fn)(void*), void* Arg)
// Loads x19-x28 and d8-d15 with patterns, calls Fn(Arg), and returns a mask
// of what did not survive: bit n for x(19+n), bit 10+n for d(8+n), bit 18 for
// SP.
//
// void clobber_callee_saved(void)
// Saves x19-x28 and d8-d15, overwrites them all, restores them, returns: what
// any AAPCS64 callee may do.
asm(R"(
  .text
  .macro setx reg, val
  movz \reg, #\val
  movk \reg, #\val, lsl #48
  .endm
  .macro setd reg, val
  movz x10, #\val
  movk x10, #\val, lsl #32
  fmov \reg, x10
  .endm
  .macro chkx reg, val, bit
  movz x10, #\val
  movk x10, #\val, lsl #48
  cmp \reg, x10
  cset x11, ne
  orr x12, x12, x11, lsl #\bit
  .endm
  .macro chkd reg, val, bit
  fmov x13, \reg
  movz x10, #\val
  movk x10, #\val, lsl #32
  cmp x13, x10
  cset x11, ne
  orr x12, x12, x11, lsl #\bit
  .endm

  .balign 4
  .globl call_checking_callee_saved
  .type call_checking_callee_saved, %function
call_checking_callee_saved:
  stp x29, x30, [sp, #-176]!
  mov x29, sp
  stp x19, x20, [sp, #16]
  stp x21, x22, [sp, #32]
  stp x23, x24, [sp, #48]
  stp x25, x26, [sp, #64]
  stp x27, x28, [sp, #80]
  stp d8, d9, [sp, #96]
  stp d10, d11, [sp, #112]
  stp d12, d13, [sp, #128]
  stp d14, d15, [sp, #144]
  mov x10, sp
  str x10, [sp, #160]
  mov x9, x0
  mov x0, x1
  setx x19, 0x1919
  setx x20, 0x2020
  setx x21, 0x2121
  setx x22, 0x2222
  setx x23, 0x2323
  setx x24, 0x2424
  setx x25, 0x2525
  setx x26, 0x2626
  setx x27, 0x2727
  setx x28, 0x2828
  setd d8, 0xd8d8
  setd d9, 0xd9d9
  setd d10, 0xdada
  setd d11, 0xdbdb
  setd d12, 0xdcdc
  setd d13, 0xdddd
  setd d14, 0xdede
  setd d15, 0xdfdf
  blr x9
  mov x12, #0
  chkx x19, 0x1919, 0
  chkx x20, 0x2020, 1
  chkx x21, 0x2121, 2
  chkx x22, 0x2222, 3
  chkx x23, 0x2323, 4
  chkx x24, 0x2424, 5
  chkx x25, 0x2525, 6
  chkx x26, 0x2626, 7
  chkx x27, 0x2727, 8
  chkx x28, 0x2828, 9
  chkd d8, 0xd8d8, 10
  chkd d9, 0xd9d9, 11
  chkd d10, 0xdada, 12
  chkd d11, 0xdbdb, 13
  chkd d12, 0xdcdc, 14
  chkd d13, 0xdddd, 15
  chkd d14, 0xdede, 16
  chkd d15, 0xdfdf, 17
  ldr x10, [x29, #160]
  mov x13, sp
  cmp x10, x13
  cset x11, ne
  orr x12, x12, x11, lsl #18
  mov x0, x12
  ldp x19, x20, [sp, #16]
  ldp x21, x22, [sp, #32]
  ldp x23, x24, [sp, #48]
  ldp x25, x26, [sp, #64]
  ldp x27, x28, [sp, #80]
  ldp d8, d9, [sp, #96]
  ldp d10, d11, [sp, #112]
  ldp d12, d13, [sp, #128]
  ldp d14, d15, [sp, #144]
  ldp x29, x30, [sp], #176
  ret
  .size call_checking_callee_saved, . - call_checking_callee_saved

  .balign 4
  .globl clobber_callee_saved
  .type clobber_callee_saved, %function
clobber_callee_saved:
  stp x29, x30, [sp, #-160]!
  mov x29, sp
  stp x19, x20, [sp, #16]
  stp x21, x22, [sp, #32]
  stp x23, x24, [sp, #48]
  stp x25, x26, [sp, #64]
  stp x27, x28, [sp, #80]
  stp d8, d9, [sp, #96]
  stp d10, d11, [sp, #112]
  stp d12, d13, [sp, #128]
  stp d14, d15, [sp, #144]
  setx x19, 0xbad0
  setx x20, 0xbad1
  setx x21, 0xbad2
  setx x22, 0xbad3
  setx x23, 0xbad4
  setx x24, 0xbad5
  setx x25, 0xbad6
  setx x26, 0xbad7
  setx x27, 0xbad8
  setx x28, 0xbad9
  setd d8, 0xbada
  setd d9, 0xbadb
  setd d10, 0xbadc
  setd d11, 0xbadd
  setd d12, 0xbade
  setd d13, 0xbadf
  setd d14, 0xbae0
  setd d15, 0xbae1
  ldp x19, x20, [sp, #16]
  ldp x21, x22, [sp, #32]
  ldp x23, x24, [sp, #48]
  ldp x25, x26, [sp, #64]
  ldp x27, x28, [sp, #80]
  ldp d8, d9, [sp, #96]
  ldp d10, d11, [sp, #112]
  ldp d12, d13, [sp, #128]
  ldp d14, d15, [sp, #144]
  ldp x29, x30, [sp], #160
  ret
  .size clobber_callee_saved, . - clobber_callee_saved
)");
uint64_t call_checking_callee_saved(void (*Fn)(void*), void* Arg);
void clobber_callee_saved(void);

// void spin_with_constants(uint64_t Iterations)
// Counts X0 down to zero with X3-X8 holding fixed constants (0x3333... to
// 0x8888...) throughout the loop [spin_loop_begin, spin_loop_end). A signal
// handler that interrupts the loop must see exactly those values in its
// ucontext.
asm(R"(
  .text
  .balign 4
  .globl spin_with_constants
  .type spin_with_constants, %function
spin_with_constants:
  setx x3, 0x3333
  setx x4, 0x4444
  setx x5, 0x5555
  setx x6, 0x6666
  setx x7, 0x7777
  setx x8, 0x8888
  mov x1, #0
  .globl spin_loop_begin
spin_loop_begin:
  add x1, x1, x0
  subs x0, x0, #1
  b.ne spin_loop_begin
  .globl spin_loop_end
spin_loop_end:
  ret
  .size spin_with_constants, . - spin_with_constants
)");
void spin_with_constants(uint64_t Iterations);
extern const char spin_loop_begin[];
extern const char spin_loop_end[];

// void fault_with_constants(const void* Inaccessible)
// Loads X3-X8 with the same constants, then loads from Inaccessible at
// fault_insn. The SIGSEGV handler checks X3-X8 in its ucontext and resumes
// after the load.
asm(R"(
  .text
  .balign 4
  .globl fault_with_constants
  .type fault_with_constants, %function
fault_with_constants:
  setx x3, 0x3333
  setx x4, 0x4444
  setx x5, 0x5555
  setx x6, 0x6666
  setx x7, 0x7777
  setx x8, 0x8888
  .globl fault_insn
fault_insn:
  ldr x9, [x0]
  ret
  .size fault_with_constants, . - fault_with_constants
)");
void fault_with_constants(const void* Inaccessible);
extern const char fault_insn[];

// --- Running callbacks, through the host or directly --------------------------

static int HaveHost;

typedef void (*Callback)(void*, uint64_t);

static uint64_t RunCallbacks(Callback Fn, void* Arg0, uint64_t Arg1, uint64_t Count) {
  if (HaveHost) {
    struct SelftestArgs Args = {(uintptr_t)Fn, (uintptr_t)Arg0, Arg1, Count, ~0ULL};
    selftest_stub(&Args);
    return Args.rv;
  }
  for (uint64_t i = 0; i < Count; ++i) {
    Fn(Arg0, Arg1 + i);
  }
  return Count;
}

static sigjmp_buf ProbeJump;
static void OnProbeSigill(int Signal) {
  (void)Signal;
  siglongjmp(ProbeJump, 1);
}

static void ProbeHost(void) {
  struct sigaction Action, Old;
  memset(&Action, 0, sizeof Action);
  Action.sa_handler = OnProbeSigill;
  sigaction(SIGILL, &Action, &Old);
  if (sigsetjmp(ProbeJump, 1) == 0) {
    struct SelftestArgs Args = {0, 0, 0, 0, ~0ULL};
    selftest_stub(&Args);
    HaveHost = Args.rv == 0;
  } else {
    HaveHost = 0;
  }
  sigaction(SIGILL, &Old, NULL);
}

static uintptr_t CurrentSP(void) {
  uintptr_t SP;
  asm volatile("mov %0, sp" : "=r"(SP));
  return SP;
}

// --- The callbacks ------------------------------------------------------------

struct State {
  uint64_t Calls;
  uint64_t Sum;
  uint64_t Misaligned;
  uint64_t Deepest;
  uint64_t Depth;
};

static void Accumulate(void* P, uint64_t I) {
  struct State* S = P;
  S->Calls++;
  S->Sum += I;
  if (CurrentSP() & 15) {
    S->Misaligned++;
  }
}

static void AccumulateAndClobber(void* P, uint64_t I) {
  Accumulate(P, I);
  clobber_callee_saved();
}

static void Nested(void* P, uint64_t I) {
  struct State* S = P;
  Accumulate(P, I);
  S->Depth++;
  if (S->Depth > S->Deepest) {
    S->Deepest = S->Depth;
  }
  if (S->Depth < 3) {
    RunCallbacks(Nested, P, 0, 3);
  }
  S->Depth--;
}

static volatile sig_atomic_t Signals;
static void OnSigusr1(int Signal) {
  (void)Signal;
  Signals++;
}

static void Signalling(void* P, uint64_t I) {
  Accumulate(P, I);
  if (I % 10 == 0) {
    raise(SIGUSR1);
  }
}

// A callback that runs long enough for timer signals to land inside it, with
// its working state live in X0-X7 (the caller-saved registers GCC allocates
// first). A signal delivered mid-loop that saved or restored any of them wrong
// changes the result.
static uint64_t __attribute__((noinline)) SpinValue(uint64_t I) {
  uint64_t A = I, B = I * 7 + 1, C = 3, D = 5, E = 11, F = 13;
  for (int K = 0; K < 4000; ++K) {
    A = A * 6364136223846793005ULL + B;
    B ^= A >> 17;
    C += A ^ D;
    D = (D << 3) ^ C;
    E += D;
    F ^= E;
  }
  return A ^ B ^ C ^ D ^ E ^ F;
}

static void Spin(void* P, uint64_t I) {
  struct State* S = P;
  S->Calls++;
  S->Sum += SpinValue(I);
  if (CurrentSP() & 15) {
    S->Misaligned++;
  }
}

// Timer signals that interrupt spin_with_constants inside a callback: how many
// did, and how many saw anything but the constants in X3-X8.
static volatile sig_atomic_t InLoop, BadFrames;
static void OnAlarmCheckFrame(int Signal, siginfo_t* Info, void* Context) {
  (void)Signal;
  (void)Info;
  const ucontext_t* UC = Context;
  const uintptr_t PC = UC->uc_mcontext.pc;
  Signals++;
  if (PC >= (uintptr_t)spin_loop_begin && PC < (uintptr_t)spin_loop_end) {
    InLoop++;
    for (int R = 3; R <= 8; ++R) {
      const uint64_t Expected = 0x1111000000001111ULL * (uint64_t)R;
      if (UC->uc_mcontext.regs[R] != Expected) {
        BadFrames++;
        break;
      }
    }
  }
}

// SIGSEGVs raised by fault_with_constants inside a callback: how many, and how
// many saw anything but the constants in X3-X8. The handler skips the load.
static volatile sig_atomic_t Faults, BadFaultFrames;
static void OnSegvCheckFrame(int Signal, siginfo_t* Info, void* Context) {
  (void)Signal;
  (void)Info;
  ucontext_t* UC = Context;
  if (UC->uc_mcontext.pc != (uintptr_t)fault_insn) {
    abort();
  }
  Faults++;
  for (int R = 3; R <= 8; ++R) {
    if (UC->uc_mcontext.regs[R] != 0x1111000000001111ULL * (uint64_t)R) {
      BadFaultFrames++;
      break;
    }
  }
  UC->uc_mcontext.pc += 4;
}

static void* InaccessiblePage;
static void FaultInCallback(void* P, uint64_t I) {
  struct State* S = P;
  (void)I;
  fault_with_constants(InaccessiblePage);
  S->Calls++;
}

static void SpinWithConstants(void* P, uint64_t I) {
  struct State* S = P;
  (void)I;
  spin_with_constants(20000);
  S->Calls++;
}

static void CalleeSavedWork(void* P) {
  RunCallbacks(AccumulateAndClobber, P, 0, 50);
}

// --- Threads ------------------------------------------------------------------

enum { THREADS = 4, PER_THREAD = 2000 };

struct ThreadWork {
  struct State S;
  uint64_t Returned;
  uint64_t Mask;
};

static void* ThreadMain(void* P) {
  struct ThreadWork* W = P;
  W->Returned = RunCallbacks(Accumulate, &W->S, 1, PER_THREAD / 2);
  W->Returned += RunCallbacks(Nested, &W->S, 0, PER_THREAD / 2 / 13);
  W->Mask = call_checking_callee_saved(CalleeSavedWork, &W->S);
  return NULL;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  ProbeHost();
  const char* Require = getenv("THUNK_CALLBACK_REQUIRE_HOST");
  if (Require && *Require == '1' && !HaveHost) {
    printf("FAIL fex:callback_selftest is not reachable\n");
    return 1;
  }

  struct sigaction Action;
  memset(&Action, 0, sizeof Action);
  Action.sa_handler = OnSigusr1;
  sigaction(SIGUSR1, &Action, NULL);

  // A canary block in this frame, checked after everything else.
  volatile uint64_t Canary[16];
  for (int i = 0; i < 16; ++i) {
    Canary[i] = 0xc0ffee0000000000ULL + i;
  }

  {
    struct State S = {0};
    const uint64_t Returned = RunCallbacks(Accumulate, &S, 1000, 100);
    printf("basic: %llu callbacks, sum %llu, returned %llu\n", (unsigned long long)S.Calls, (unsigned long long)S.Sum,
           (unsigned long long)Returned);
  }

  {
    struct State S = {0};
    const uint64_t Mask = call_checking_callee_saved(CalleeSavedWork, &S);
    printf("callee_saved: %llu callbacks, lost mask %#llx\n", (unsigned long long)S.Calls, (unsigned long long)Mask);
  }

  {
    struct State S = {0};
    RunCallbacks(Nested, &S, 0, 3);
    printf("nested: %llu callbacks, deepest %llu, sum %llu\n", (unsigned long long)S.Calls, (unsigned long long)S.Deepest,
           (unsigned long long)S.Sum);
  }

  {
    struct State S = {0};
    Signals = 0;
    RunCallbacks(Signalling, &S, 0, 100);
    printf("signals: %llu callbacks, %d handled inside them\n", (unsigned long long)S.Calls, (int)Signals);
  }

  {
    pthread_t Threads[THREADS];
    static struct ThreadWork Work[THREADS];
    for (int i = 0; i < THREADS; ++i) {
      pthread_create(&Threads[i], NULL, ThreadMain, &Work[i]);
    }
    uint64_t Calls = 0, Sum = 0, Returned = 0, Mask = 0, Misaligned = 0;
    for (int i = 0; i < THREADS; ++i) {
      pthread_join(Threads[i], NULL);
      Calls += Work[i].S.Calls;
      Sum += Work[i].S.Sum;
      Returned += Work[i].Returned;
      Mask |= Work[i].Mask;
      Misaligned += Work[i].S.Misaligned;
    }
    printf("threads: %d threads, %llu callbacks, sum %llu, returned %llu, lost mask %#llx, misaligned %llu\n", THREADS,
           (unsigned long long)Calls, (unsigned long long)Sum, (unsigned long long)Returned, (unsigned long long)Mask,
           (unsigned long long)Misaligned);
  }

  {
    struct State S = {0};
    RunCallbacks(Accumulate, &S, 0, 1000);
    printf("alignment: %llu callbacks, %llu with SP not 16-byte aligned\n", (unsigned long long)S.Calls,
           (unsigned long long)S.Misaligned);
  }

  {
    // Batches of 10 Spin callbacks (arguments b*10 .. b*10+9) with an interval
    // timer running, until 50 of its signals have arrived and at least 20
    // batches have run. Then the same calls again, directly and with the timer
    // stopped, as the reference.
    struct sigaction Alarm;
    memset(&Alarm, 0, sizeof Alarm);
    Alarm.sa_handler = OnSigusr1;
    sigaction(SIGALRM, &Alarm, NULL);
    Signals = 0;
    struct itimerval Timer = {{0, 200}, {0, 200}};
    setitimer(ITIMER_REAL, &Timer, NULL);
    struct State S = {0};
    uint64_t Batch = 0, Returned = 0;
    for (; Batch < 100000 && (Signals < 50 || Batch < 20); ++Batch) {
      Returned += RunCallbacks(Spin, &S, Batch * 10, 10);
    }
    struct itimerval Stop = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &Stop, NULL);
    const int Enough = Signals >= 50;

    struct State Reference = {0};
    for (uint64_t I = 0; I < Batch * 10; ++I) {
      Spin(&Reference, I);
    }
    const int Ok = Returned == Batch * 10 && S.Calls == Reference.Calls && S.Sum == Reference.Sum && !S.Misaligned;
    printf("async: timer signals during callbacks: %s, results %s\n", Enough ? "at least 50" : "TOO FEW",
           Ok ? "match the direct calls" : "WRONG");
  }

  {
    // Timer signals interrupting a loop inside a callback must see the loop's
    // registers in their ucontext. Run until 20 have landed in the loop.
    struct sigaction Alarm;
    memset(&Alarm, 0, sizeof Alarm);
    Alarm.sa_sigaction = OnAlarmCheckFrame;
    Alarm.sa_flags = SA_SIGINFO;
    sigaction(SIGALRM, &Alarm, NULL);
    Signals = 0;
    InLoop = 0;
    BadFrames = 0;
    struct itimerval Timer = {{0, 200}, {0, 200}};
    setitimer(ITIMER_REAL, &Timer, NULL);
    struct State S = {0};
    for (uint64_t Batch = 0; Batch < 100000 && InLoop < 20; ++Batch) {
      RunCallbacks(SpinWithConstants, &S, 0, 10);
    }
    struct itimerval Stop = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &Stop, NULL);
    printf("frames: timer signals inside a callback's loop: %s, their X3-X8 %s\n", InLoop >= 20 ? "at least 20" : "TOO FEW",
           BadFrames == 0 ? "exact" : "WRONG");
  }

  {
    // A synchronous fault inside a callback is delivered at the faulting
    // instruction, not at a block boundary: its ucontext must hold the
    // registers as they were there.
    InaccessiblePage = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct sigaction Segv;
    memset(&Segv, 0, sizeof Segv);
    Segv.sa_sigaction = OnSegvCheckFrame;
    Segv.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &Segv, NULL);
    struct State S = {0};
    RunCallbacks(FaultInCallback, &S, 0, 20);
    signal(SIGSEGV, SIG_DFL);
    printf("faults: %llu callbacks, %d SIGSEGVs inside them, their X3-X8 %s\n", (unsigned long long)S.Calls, (int)Faults,
           BadFaultFrames == 0 ? "exact" : "WRONG");
  }

  int CanaryOk = 1;
  for (int i = 0; i < 16; ++i) {
    if (Canary[i] != 0xc0ffee0000000000ULL + i) {
      CanaryOk = 0;
    }
  }
  printf("canary: %s\n", CanaryOk ? "intact" : "CORRUPTED");
  return 0;
}
