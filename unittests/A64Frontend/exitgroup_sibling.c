// SPDX-License-Identifier: MIT
// NO-GOLDEN
//
// exit_group from a thread that is not the initial one must end the process,
// promptly and with that call's status.
//
// It is the ordinary shape of program exit for a large class of guests: Go's
// os.Exit runs on whichever thread holds the main goroutine, and a Rust or C++
// program that exits from a worker does the same. Both codex (Rust) and agy
// (Go) did it on every quit, and every quit left the user's shell waiting on
// an emulator that never returned.
//
// THE BUG. exit_group's handler calls ThreadManager::Stop(), which signals
// every sibling -- the initial thread included -- and then waits on IdleWaitCV
// for them to go. Stopping the initial thread returns ExecuteThread on the
// host's main thread, and main then started a process exit of its own: its
// Stop() signalled the thread that was still inside exit_group. The stop
// handler hijacks any thread in the dispatcher, host code included, so that
// thread was torn out of its IdleWaitCV wait without leaving it. Two things
// followed. The guest's exit_group never ran, so the process reported the
// initial thread's status instead of the one asked for; and glibc's
// pthread_cond_destroy, run by ~ThreadManager at the end of main, waits for
// every waiter to leave -- that one never would, and the process hung there
// for good in futex_do_wait with its code-cache save child left defunct.
//
// Three shapes, one per argument, because the stop signal finds the initial
// thread in a different place in each:
//
//   join   the initial thread is blocked in a syscall (pthread_join's futex)
//   spin   the initial thread is running guest code
//   many   eight threads race to exit_group at once; the first must win and
//          the rest must neither hang the exit nor change its status
//
// In "many" each racer first runs the very instructions it will exit through,
// with getpid in place of exit_group, so nothing is compiled at the moment of
// the race. That is deliberate and not cosmetic: eight threads entering cold
// code together contend the JIT's allocations, and a stop that lands while a
// thread holds the 64-bit allocator's mutex abandons it there -- the thread
// then deadlocks on its own lock in UninstallTLSState and the exit stalls for
// Stop's full second (or hangs, when the exit path needs that lock too). That
// is a separate bug in how a stop treats a thread inside a signal-deferring
// section, and this test is about who owns the exit, so it stays out of it.
//
// Self-checking: the only correct result is exit status 42 within the
// timeout run.sh gives it. Any other status, or a timeout, is a failure.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define STATUS 42
#define RACERS 8

static atomic_int Go;
static atomic_int Ready;

static void ExitGroup(void) {
  syscall(SYS_exit_group, STATUS);
}

static void* Exiter(void* Arg) {
  (void)Arg;
  ExitGroup();
  return 0;
}

// Round 0 warms the path with getpid; round 1 takes it with exit_group. The
// syscall number is the only thing that differs between the two.
static void* Racer(void* Arg) {
  (void)Arg;
  for (int Round = 0; Round < 2; ++Round) {
    atomic_fetch_add(&Ready, 1);
    while (atomic_load(&Go) <= Round) {
    }
    syscall(Round == 0 ? SYS_getpid : SYS_exit_group, STATUS);
  }
  return 0;
}

int main(int argc, char** argv) {
  const char* Mode = argc > 1 ? argv[1] : "join";
  pthread_t T[RACERS];

  if (strcmp(Mode, "join") == 0) {
    if (pthread_create(&T[0], 0, Exiter, 0) != 0) {
      return 1;
    }
    pthread_join(T[0], 0);
  } else if (strcmp(Mode, "spin") == 0) {
    if (pthread_create(&T[0], 0, Exiter, 0) != 0) {
      return 1;
    }
    for (volatile unsigned long Spins = 0;; ++Spins) {
    }
  } else if (strcmp(Mode, "many") == 0) {
    for (int i = 0; i < RACERS; ++i) {
      if (pthread_create(&T[i], 0, Racer, 0) != 0) {
        return 1;
      }
    }
    for (int Round = 0; Round < 2; ++Round) {
      while (atomic_load(&Ready) != RACERS * (Round + 1)) {
      }
      atomic_store(&Go, Round + 1);
    }
    pthread_join(T[0], 0);
  } else {
    return 1;
  }
  // exit_group never returns, so reaching here means it did not end the process.
  return 2;
}
