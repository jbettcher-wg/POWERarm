// SPDX-License-Identifier: MIT
//
// A guest with fault handlers like a crash reporter's (SA_NODEFER |
// SA_ONSTACK | SA_SIGINFO on SIGSEGV, SIGBUS, SIGILL, SIGTRAP and SIGFPE, as
// Firefox's and Chromium's install) that then calls getppid. Natively that is
// all it does, and that is its golden.
//
// run.sh also runs it with POWERARM_HOSTFAULT_INJECT=<getppid>,segv,unwind,
// which makes POWERarm fault in its own syscall body under a return address
// its unwinder cannot read (hostfault_report). The fatal host-fault report
// must then print its line first and once, survive the fault its own
// backtrace takes, never deliver the host fault to these handlers, and end the
// process with the original SIGSEGV. It used to re-enter itself through the
// unwinder's fault (SA_NODEFER lets it in), dozens of levels deep, burying the
// line and truncating the core; with the handlers left out (--no-handlers)
// the unwinder's fault, taken with SIGSEGV blocked, killed it in the
// unwinder.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static void Handler(int Signal, siginfo_t* Info, void* Context) {
  (void)Info;
  (void)Context;
  static const char Msg[] = "guest handler ran\n";
  write(STDOUT_FILENO, Msg, sizeof(Msg) - 1);
  _exit(64 + Signal);
}

int main(int argc, char** argv) {
  const int WithHandlers = !(argc > 1 && strcmp(argv[1], "--no-handlers") == 0);
  if (WithHandlers) {
    static char AltStack[1 << 16];
    stack_t Stack = {.ss_sp = AltStack, .ss_size = sizeof(AltStack), .ss_flags = 0};
    sigaltstack(&Stack, NULL);
    struct sigaction Action;
    memset(&Action, 0, sizeof(Action));
    Action.sa_sigaction = Handler;
    Action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&Action.sa_mask);
    for (int Signal = 1; Signal < 32; ++Signal) {
      if (Signal == SIGSEGV || Signal == SIGBUS || Signal == SIGILL || Signal == SIGTRAP || Signal == SIGFPE) {
        sigaction(Signal, &Action, NULL);
      }
    }
  }
  printf("handlers: %s\n", WithHandlers ? "installed" : "none");
  fflush(stdout);
  const long Parent = syscall(SYS_getppid);
  printf("getppid: %s\n", Parent > 0 ? "ok" : "failed");
  return 0;
}
