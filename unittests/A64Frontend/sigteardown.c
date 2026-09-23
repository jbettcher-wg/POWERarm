// SPDX-License-Identifier: MIT
//
// A guest signal handler returning through rt_sigreturn while the thread it
// belongs to is being torn down.
//
// POWERarm turns a guest rt_sigreturn into a jump to a word the dispatcher
// emits to be illegal on purpose (SignalHandlerReturnAddress{,RT}); the host
// SIGILL that produces is what SignalDelegator::HandleSIGILL recognises, and
// what it restores the thread from. SignalHandlerThunk has to get that SIGILL
// as far as HandleSIGILL, and it has two early bails -- no thread object, and
// a thread object that fails its liveness checks -- which used to hand the
// signal to the default disposition instead. Either one, taken at a sentinel
// PC, ends the whole process on an ILL_ILLOPC whose NIP sits in an anonymous
// r-xp mapping with no symbol: the VS Code (Electron) and Antigravity cores of
// 2026-09.
//
// Both bails are thread-teardown windows, so this hammers that window. Every
// worker is signalled by kernel TID with tgkill rather than pthread_kill, so
// the signals keep arriving straight through the worker's own exit and its
// join, where pthread_kill would be undefined behaviour.
//
// The assertion is the exit status: nothing here may take the process down,
// and stdout is fixed whatever the timing. On the build before this test it
// was not -- SIGUSR1 (SI_TKILL) reaching a worker past UninstallTLSState took
// the first bail, which restored SIG_DFL for SIGUSR1 PROCESS-wide, so the next
// delivery to any thread killed the run: 4 of 5 runs died with signal 10
// before printing a line.
//
// NOT extended to "exit while the workers are still live and still signalled",
// tempting as that is for the second bail. That shape hangs ~23 s and then
// dies in OSAllocator_64Bit::Munmap, from a worker still in ThreadHandler's
// UninstallTLSState while the exiting thread is inside ThreadManager::Stop.
// It reproduces identically on the build before this change, so it is a
// separate pre-existing defect and not this test's business.
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 8
#define ROUNDS 300
#define TARGET_SIGNALS 4
// Between sweeps. Uncapped, the signaller buries the workers in deliveries and
// the run costs minutes of system time under emulation for no extra coverage:
// what matters is that signals keep arriving across a teardown, not how many.
#define SWEEP_GAP_NS 5000L
// Nothing here may hang: a worker that stops receiving signals leaves anyway.
#define PHASE_DEADLINE_NS 3000000000LL

static atomic_ulong total_delivered;
// Written by this thread's own signal handler, read by its loop.
static _Thread_local volatile unsigned long my_signals;

static void on_signal(int sig, siginfo_t* si, void* uc) {
  (void)sig;
  (void)si;
  (void)uc;
  my_signals++;
  atomic_fetch_add_explicit(&total_delivered, 1, memory_order_relaxed);
}

static long long now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

struct worker {
  pthread_t thread;
  atomic_int tid;
};

static struct worker workers[WORKERS];
static atomic_int stop_signalling;
static atomic_ulong worker_target;

// Guest code, taking signals, for as long as it takes to collect the round's
// quota. The spin is what keeps the thread in translated code rather than
// parked in a syscall, so that a delivery has a guest frame to build and a
// rt_sigreturn to come back through.
static void* worker_main(void* arg) {
  struct worker* w = (struct worker*)arg;
  const unsigned long target = atomic_load(&worker_target);
  atomic_store(&w->tid, (int)syscall(SYS_gettid));
  const long long deadline = now_ns() + PHASE_DEADLINE_NS;
  volatile unsigned long spin = 0;
  while (my_signals < target) {
    spin += 1;
    if ((spin & 0xffff) == 0 && now_ns() > deadline) {
      break;
    }
  }
  return NULL;
}

// tgkill by kernel TID, not pthread_kill: a pthread_t is undefined once its
// thread has returned, and the whole point is to keep signalling past that.
// ESRCH on a reaped TID is the expected, ignored outcome.
static void* signaller_main(void* arg) {
  (void)arg;
  const pid_t pid = getpid();
  while (!atomic_load_explicit(&stop_signalling, memory_order_relaxed)) {
    for (int i = 0; i < WORKERS; i++) {
      const int tid = atomic_load_explicit(&workers[i].tid, memory_order_relaxed);
      if (tid != 0) {
        syscall(SYS_tgkill, pid, tid, SIGUSR1);
      }
    }
    const struct timespec gap = {0, SWEEP_GAP_NS};
    nanosleep(&gap, NULL);
  }
  return NULL;
}

static int start_phase(pthread_t* killer, unsigned long target) {
  atomic_store(&worker_target, target);
  atomic_store(&stop_signalling, 0);
  for (int i = 0; i < WORKERS; i++) {
    atomic_store(&workers[i].tid, 0);
  }
  if (pthread_create(killer, NULL, signaller_main, NULL) != 0) {
    return -1;
  }
  for (int i = 0; i < WORKERS; i++) {
    if (pthread_create(&workers[i].thread, NULL, worker_main, &workers[i]) != 0) {
      return -1;
    }
  }
  return 0;
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_signal;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1, &sa, NULL) != 0) {
    printf("FAIL sigteardown: sigaction\n");
    return 1;
  }

  for (int r = 0; r < ROUNDS; r++) {
    pthread_t killer;
    if (start_phase(&killer, TARGET_SIGNALS) != 0) {
      printf("FAIL sigteardown: pthread_create in round %d\n", r);
      return 1;
    }
    // Joined while the signaller is still firing at these TIDs, so every
    // worker takes signals throughout its exit.
    for (int i = 0; i < WORKERS; i++) {
      if (pthread_join(workers[i].thread, NULL) != 0) {
        printf("FAIL sigteardown: pthread_join in round %d\n", r);
        return 1;
      }
    }
    atomic_store(&stop_signalling, 1);
    pthread_join(killer, NULL);
  }
  printf("sigteardown: %d churn rounds, every worker joined\n", ROUNDS);

  if (atomic_load(&total_delivered) == 0) {
    printf("FAIL sigteardown: no signal was ever delivered\n");
    return 1;
  }
  printf("sigteardown: signals delivered through teardown\n");

  printf("PASS sigteardown\n");
  return 0;
}
