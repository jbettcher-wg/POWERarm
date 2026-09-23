// SPDX-License-Identifier: MIT
//
// A fault-shaped signal that no instruction raised.
//
// rt_tgsigqueueinfo lets a thread queue a siginfo of its own making at any
// thread in its own group, and do_rt_tgsigqueueinfo only rejects si_code >= 0
// when the target group belongs to somebody else. So ILL_ILLOPC -- the si_code
// the CPU uses for an illegal opcode -- is a value a program can legitimately
// send itself, and Linux then treats the result as an ordinary SIGILL. Nothing
// here handles it, so the process dies of it and `queued` is the last line.
//
// POWERarm read si_code the other way round. SignalDelegator's host-fault gate
// takes `si_code > 0` to mean the host CPU raised this signal at the
// instruction the handler interrupted, so a queued SIGILL arriving while the
// thread sits in a FEX syscall body was reported as a fatal host fault, handed
// to SIG_DFL, and then RETURNED from. Returning re-runs a faulting instruction;
// it merely CONSUMES a queued signal. The process ran on with the host SIGILL
// disposition left at SIG_DFL -- and that is the disposition
// SignalDelegator::HandleSIGILL needs to recognise the deliberate `Emit32(0)`
// sentinel words the dispatcher emits, so the next guest signal handler to
// return died at SignalHandlerReturnAddressRT with ILL_ILLOPC and an empty
// sighold, an unexplainable wild jump into an anonymous r-xp mapping. That is
// the VS Code / Antigravity crash, cores 1841112 and 3538584.
//
// The check is therefore just: does the queue kill us, the way it does
// natively? A `survived` line means the signal was swallowed, and whatever
// comes after it says what the lost disposition costs.
//
// Phase 1 is a positive control for the machinery the bug destroys: a guest
// handler that returns goes back through rt_sigreturn, which is what executes
// the sentinel. If `sentinel ok` is missing, the sentinel path was already
// broken before the queue and the rest of this test means nothing.
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile sig_atomic_t Handled;

static void OnUsr1(int Signal) {
  (void)Signal;
  Handled = 1;
}

// One guest signal delivered and returned from, i.e. one trip through
// rt_sigreturn and the dispatcher's RT sentinel word.
static int SentinelRoundTrip(void) {
  Handled = 0;
  raise(SIGUSR1);
  return Handled == 1;
}

int main(void) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_handler = OnUsr1;
  sigemptyset(&Action.sa_mask);
  if (sigaction(SIGUSR1, &Action, NULL) != 0) {
    printf("sigaction failed: %s\n", strerror(errno));
    return 1;
  }

  printf("sentinel %s\n", SentinelRoundTrip() ? "ok" : "lost");
  fflush(stdout);

  // A siginfo the CPU would have produced for an illegal opcode, at an address
  // that is not where this thread is executing -- because nothing about it came
  // from an instruction.
  siginfo_t Info;
  memset(&Info, 0, sizeof(Info));
  Info.si_signo = SIGILL;
  Info.si_code = ILL_ILLOPC;
  Info.si_addr = (void*)(unsigned long)&main;

  const long Tid = syscall(SYS_gettid);
  printf("queued\n");
  fflush(stdout);
  if (syscall(SYS_rt_tgsigqueueinfo, getpid(), Tid, SIGILL, &Info) != 0) {
    printf("queue failed: %s\n", strerror(errno));
    return 1;
  }

  // Unreachable: the kernel delivers the queued SIGILL on the way out of that
  // syscall and its default action dumps core.
  printf("survived\n");
  fflush(stdout);

  // And this is the consequence, when it is reached: with SIGILL disarmed
  // process-wide the round trip that worked above now dies on the sentinel
  // instead of returning from it.
  printf("sentinel %s\n", SentinelRoundTrip() ? "ok" : "lost");
  return 0;
}
