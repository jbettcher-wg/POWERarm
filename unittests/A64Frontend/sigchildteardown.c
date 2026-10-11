// SPDX-License-Identifier: MIT
// NO-GOLDEN
//
// A process-directed SIGCHLD generated while one of this process's threads is
// inside a FEX thread-teardown window must still reach a thread that can
// deliver it.
//
// THE WINDOW. A thread that has entered its exit path will never execute guest
// code again, so from that point on it can deliver nothing to the guest -- and
// it was failing in two different ways, both of which this test caught:
//
//   * Past the TLS_ThreadObject clear at the end of UninstallTLSState, and
//     past the alt-stack munmap that takes the back-pointer with it,
//     SignalHandlerThunk can find no thread object at all. Its bail used to
//     just return, which CONSUMES the signal: the `no-thread-object sig=17
//     code=1 ... action=drop' line in the logs.
//   * Earlier on the same path, with the thread object still reachable, an
//     asynchronous signal arriving inside a signal-deferring section is parked
//     on DeferredSignalFrames to be drained at the thread's next guest block
//     boundary. An exiting thread has no next block boundary, so the queued
//     frame dies with it, silently, with nothing in the log.
//
// Either way, for a tgkill aimed at this one thread losing it is correct: the
// sender raced the exit and there was nobody else to pick. For a signal aimed
// at the PROCESS it is a lost notification, and the kernel is actively likely
// to put one here -- complete_signal() hands a group-directed signal to the
// first thread of the group that does not block it, PF_EXITING is not set
// until the kernel's own exit path runs, which is after the SYS_exit and so
// long after the window opens, and a thread in the window blocks nothing.
// Measured under headless Chrome: one to three SIGCHLD/CLD_EXITED lost per
// session, every session. Silent, because SIGCHLD's default disposition is
// ignore -- but a guest that installed a handler to reap its children, which
// Chrome does, never learns the child exited.
//
// On the build before the fix this test lost 4 of its first 5 children: three
// to the bail, with that line on stderr, and one to the deferred-signal queue
// with no line at all.
//
// STEERING. Left to chance the kernel usually picks a thread that is simply
// running, and the race shows up a few times an hour. So the mask decides it
// instead:
//
//   * main and the spawner threads BLOCK SIGCHLD, so wants_signal() rejects
//     them and they are never candidates. main is also the forking thread,
//     which matters: do_notify_parent() aims the SIGCHLD at the thread that
//     forked the child and complete_signal() tries that thread first, so a
//     forker with SIGCHLD unblocked would take every delivery itself.
//   * each short-lived worker UNBLOCKS SIGCHLD as its first act and then
//     leaves at once through a raw SYS_exit.
//
// So every SIGCHLD this test generates can only be taken by a worker, and a
// worker's whole eligible lifetime IS its own exit: the guest `exit' syscall,
// FEX's UninstallTLSState and DestroyThread, the host thread exit. A large
// part of that is the no-thread-object window, which is why this hits in tens
// of rounds rather than hours. The stream of workers is also what keeps FEX's
// 64-bit allocator mutex contended -- three mmap/munmap pairs per thread --
// and that contention is what widens the window from a few instructions into
// something a signal lands in; the same mechanism threadexit.c relies on.
//
// ACCOUNTING. Exactly one child at a time. SIGCHLD is a standard signal, so a
// second instance coalesces with a pending first and the arithmetic would stop
// meaning anything. Each round forks one child, holds it on a pipe until the
// handler knows which pid to expect, lets it go, reaps it, and then requires
// the handler to have seen it. The handler matches on si_pid, so the emulator's
// own helper processes -- the code-cache writer forks a child of its own at
// exit -- cannot be mistaken for the round's child; they are counted apart and
// reported.
//
// A round that sees nothing is not yet a failure. If every thread in the group
// happened to block SIGCHLD at the instant it was sent -- which is exactly what
// the fix makes possible -- the signal is sitting in the group's shared_pending
// queue, not lost, and the first thread to become eligible will take it. So the
// round then unblocks SIGCHLD in main and gives it a moment to arrive. That
// drain is the discrimination this test turns on: a merely PENDING signal is
// recovered there and counts as delivered, while a signal a dying thread
// DEQUEUED and threw away cannot be recovered by anything. The only way this
// test fails is the second one.
//
// Self-checking, and it must stay that way: no `.golden'. The expected answer
// is "every child this process reaped was announced to this process", which the
// file computes from its own round count, and a reference run taken from the
// emulator under test is precisely what could not tell you that.
//
// It also guards against proving nothing: if no delivery ever landed on an
// exiting worker, the steering above did not work, the rounds were all
// satisfied by main's drain, and the FAIL says so rather than reporting a
// green run.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Rounds, i.e. children. Each costs a fork of the emulator, so this is the
// term that sets the runtime; it is high enough that a window open as wide as
// the measured one cannot stay unhit.
#define ROUNDS 64
// Threads feeding the stream of exiting workers.
#define SPAWNERS 4
// Stop after this many lost children. The count is more useful than the first
// one alone, and the cap keeps a failing run from spending ROUNDS x the full
// deadline below.
#define MAX_LOSSES 4
// How long a round waits for the handler before trying the drain. Delivery is
// immediate when it happens at all -- the kernel takes the signal on the next
// return to user mode -- so this is pure slack for an emulated, loaded box.
#define DELIVER_DEADLINE_NS 1500000000LL
// How long the drain gives a signal sitting in shared_pending once main has
// unblocked it. Same reasoning, smaller margin: main is running and the signal
// is already queued.
#define DRAIN_NS 150000000L
// Workers that must have got going before the rounds start, so the first fork
// already happens into a running stream.
#define WARMUP_WORKERS 64
// Ceiling on the stream, so a slow box cannot turn this into an unbounded
// thread mill. Reaching it is reported.
#define WORKER_CAP 60000

static atomic_int go;
static atomic_long main_tid;
static atomic_long workers_started;
static atomic_long workers_created;

// The pid of the round's child, published before the child is allowed to exit.
static atomic_int expected_pid;
// Deliveries for that child, split by the thread that took them.
static atomic_int round_on_worker;
static atomic_int round_on_main;
// Totals, for the report and for the did-this-prove-anything check.
static atomic_long total_on_worker;
static atomic_long total_on_main;
// SIGCHLD for something that is not the round's child: the emulator's own
// helper processes. Reported, never fatal.
static atomic_long foreign;
// si_code of the last matching delivery, to confirm it arrived kernel-shaped
// (CLD_EXITED, si_code > 0) rather than flattened on the way through.
static atomic_int last_code;

static void on_sigchld(int sig, siginfo_t* si, void* uc) {
  (void)sig;
  (void)uc;
  if (si->si_pid != atomic_load_explicit(&expected_pid, memory_order_relaxed)) {
    atomic_fetch_add_explicit(&foreign, 1, memory_order_relaxed);
    return;
  }
  atomic_store_explicit(&last_code, si->si_code, memory_order_relaxed);
  if ((long)syscall(SYS_gettid) == atomic_load_explicit(&main_tid, memory_order_relaxed)) {
    atomic_fetch_add_explicit(&round_on_main, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&total_on_main, 1, memory_order_relaxed);
  } else {
    atomic_fetch_add_explicit(&round_on_worker, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&total_on_worker, 1, memory_order_relaxed);
  }
}

static long long now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void spin(unsigned long n) {
  for (volatile unsigned long i = 0; i < n; ++i) {
  }
}

static int round_saw_it(void) {
  return atomic_load(&round_on_worker) + atomic_load(&round_on_main) > 0;
}

// Unblocks SIGCHLD, leaves it in only as long as it takes to pick up anything
// already queued, then blocks it again -- main must not be a candidate for the
// next round's delivery. Nothing can arrive during the gap but the signal being
// drained: the round's child is already reaped and the next is not yet forked.
static void drain_pending(void) {
  sigset_t chld;
  struct timespec gap = {0, DRAIN_NS};
  sigemptyset(&chld);
  sigaddset(&chld, SIGCHLD);
  pthread_sigmask(SIG_UNBLOCK, &chld, NULL);
  while (nanosleep(&gap, &gap) == -1 && errno == EINTR) {
  }
  pthread_sigmask(SIG_BLOCK, &chld, NULL);
}

// The whole of a worker: become a candidate for a process-directed SIGCHLD,
// then leave through the guest `exit' syscall, which is the path that runs
// UninstallTLSState. Nothing in between, so almost all of the time it spends
// eligible is time spent inside FEX's teardown.
static void* worker(void* arg) {
  sigset_t chld;
  (void)arg;
  sigemptyset(&chld);
  sigaddset(&chld, SIGCHLD);
  pthread_sigmask(SIG_UNBLOCK, &chld, NULL);
  atomic_fetch_add_explicit(&workers_started, 1, memory_order_relaxed);
  syscall(SYS_exit, 0);
  return NULL;
}

static void* spawner(void* arg) {
  pthread_attr_t at;
  (void)arg;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  while (atomic_load_explicit(&go, memory_order_relaxed)) {
    pthread_t t;
    if (atomic_fetch_add_explicit(&workers_created, 1, memory_order_relaxed) >= WORKER_CAP) {
      break;
    }
    if (pthread_create(&t, &at, worker, NULL) != 0) {
      // Out of stacks for the moment; let some drain.
      spin(3000);
    }
  }
  pthread_attr_destroy(&at);
  return NULL;
}

// Forks one child, publishes its pid, lets it exit, reaps it. The pipe is what
// makes the publish race-free: without it the child can be gone before
// expected_pid is set, and the handler would score its SIGCHLD as foreign.
// Returns 0 on success, -1 if the fixture itself failed.
static int run_child(void) {
  int fds[2];
  pid_t pid;
  int status = 0;
  char go_byte = 'g';

  if (pipe(fds) != 0) {
    return -1;
  }
  pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    char b;
    close(fds[1]);
    // Held here until the parent has published the pid. EOF counts as go, so a
    // parent that dies cannot leave this child behind.
    while (read(fds[0], &b, 1) == -1 && errno == EINTR) {
    }
    _exit(0);
  }
  close(fds[0]);
  atomic_store(&round_on_worker, 0);
  atomic_store(&round_on_main, 0);
  atomic_store(&expected_pid, (int)pid);
  while (write(fds[1], &go_byte, 1) == -1 && errno == EINTR) {
  }
  close(fds[1]);
  // Blocking, on this pid alone: the emulator's own children must not be
  // reaped here, and returning means the child is a zombie, which means
  // do_notify_parent has already run and the SIGCHLD has already been
  // generated -- delivered, queued, or thrown away.
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
  }
  return 0;
}

int main(void) {
  struct sigaction sa;
  sigset_t chld;
  pthread_t spawners[SPAWNERS];
  int started = 0;
  int lost = 0;
  int drained = 0;
  int fixture_failed = 0;
  int r;
  long long warm_deadline;

  atomic_store(&main_tid, (long)syscall(SYS_gettid));

  // Blocked in main FIRST, so every thread created below inherits it blocked
  // and no thread is a candidate until it says otherwise. It also keeps the
  // spawners -- and the startup window of a worker, before FEX has published
  // its thread object, which is a different window and not this test's
  // business -- out of the picture entirely.
  sigemptyset(&chld);
  sigaddset(&chld, SIGCHLD);
  if (pthread_sigmask(SIG_BLOCK, &chld, NULL) != 0) {
    printf("FAIL sigchildteardown: could not block SIGCHLD in main\n");
    return 1;
  }

  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_sigchld;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGCHLD, &sa, NULL) != 0) {
    printf("FAIL sigchildteardown: sigaction\n");
    return 1;
  }

  atomic_store(&go, 1);
  for (r = 0; r < SPAWNERS; ++r) {
    if (pthread_create(&spawners[r], NULL, spawner, NULL) != 0) {
      break;
    }
    started++;
  }
  if (started == 0) {
    printf("FAIL sigchildteardown: no spawner thread could be created\n");
    return 1;
  }

  // Into a running stream, not a cold one.
  warm_deadline = now_ns() + 5000000000LL;
  while (atomic_load(&workers_started) < WARMUP_WORKERS && now_ns() < warm_deadline) {
    spin(200);
  }
  if (atomic_load(&workers_started) == 0) {
    atomic_store(&go, 0);
    printf("FAIL sigchildteardown: the worker stream never started\n");
    return 1;
  }

  for (r = 0; r < ROUNDS && lost < MAX_LOSSES; ++r) {
    long long deadline;
    if (run_child() != 0) {
      fixture_failed = 1;
      break;
    }
    deadline = now_ns() + DELIVER_DEADLINE_NS;
    while (!round_saw_it() && now_ns() < deadline) {
      spin(100);
    }
    if (!round_saw_it()) {
      // Possibly only pending, because every thread blocked it at the moment
      // it was sent. Pending is not lost.
      drain_pending();
      if (round_saw_it()) {
        drained++;
      } else {
        lost++;
      }
    }
  }

  atomic_store(&go, 0);
  for (int i = 0; i < started; ++i) {
    pthread_join(spawners[i], NULL);
  }

  printf("sigchildteardown: %d rounds, %ld workers torn down, deliveries %ld on an exiting worker / %ld on main after a drain\n", r,
         atomic_load(&workers_started), atomic_load(&total_on_worker), atomic_load(&total_on_main));
  printf("sigchildteardown: %d rounds needed the drain, %ld SIGCHLD from the emulator's own children, last si_code %d\n", drained,
         atomic_load(&foreign), atomic_load(&last_code));

  if (fixture_failed) {
    printf("FAIL sigchildteardown: fork/pipe fixture failed in round %d (%s)\n", r, strerror(errno));
    return 1;
  }
  // The loss comes first because it is the informative failure; the ceiling
  // below only ever truncates the stream, which costs coverage (and trips the
  // did-this-prove-anything check) but cannot manufacture a loss.
  if (lost != 0) {
    printf("FAIL sigchildteardown: %d of %d process-directed SIGCHLD were consumed by a thread in teardown and lost\n", lost, r);
    return 1;
  }
  if (atomic_load(&workers_created) >= WORKER_CAP) {
    printf("FAIL sigchildteardown: hit the %d worker ceiling before finishing the rounds\n", WORKER_CAP);
    return 1;
  }
  printf("PASS sigchildteardown: every one of %d reaped children was announced to this process\n", r);

  // Says nothing about the fix if the kernel never once chose an exiting
  // worker: the rounds would all have been satisfied by main's drain, and this
  // would be a test of nothing.
  if (atomic_load(&total_on_worker) == 0) {
    printf("FAIL sigchildteardown: no SIGCHLD ever reached an exiting worker, so the steering did not work\n");
    return 1;
  }
  printf("PASS sigchildteardown: %ld deliveries landed on a worker inside its own exit\n", atomic_load(&total_on_worker));

  if (atomic_load(&last_code) != CLD_EXITED) {
    printf("FAIL sigchildteardown: si_code %d, expected CLD_EXITED (%d) -- kernel-generated and process-directed\n",
           atomic_load(&last_code), CLD_EXITED);
    return 1;
  }
  printf("PASS sigchildteardown: siginfo arrived as CLD_EXITED\n");
  return 0;
}
