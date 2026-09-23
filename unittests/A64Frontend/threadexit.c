// SPDX-License-Identifier: MIT
//
// Thread teardown racing process exit.
//
// Spawners keep a stream of short-lived threads going; each of those leaves
// through a raw SYS_exit -- the guest `exit` syscall, which is the path that
// uninstalls the thread's host signal alt stack. Main leaves through
// exit_group while the stream is at full rate, joining nothing. exit_group
// makes ThreadManager::Stop() tgkill SIGNAL_FOR_PAUSE at every thread still
// on its list, and a thread inside its own teardown is still on that list
// (DestroyThread removes it later), so the signal lands squarely in the
// teardown window.
//
// A stream rather than a batch on purpose. Each thread FEX creates and
// destroys costs three of its own mmap/munmap pairs through the 64-bit
// allocator -- pivot stack, alt stack, call-ret stack -- so a stream keeps
// that allocator's mutex contended, and the alt-stack munmap in the window
// blocks on it. That contention is what widens the window from a couple of
// instructions into something a signal reliably lands in; it is also what the
// crash cores showed, with several threads queued in Munmap at once. Guest
// mmap does not touch that mutex (it goes straight to the host), so guest
// allocation traffic is no substitute. It is the shape of the real workload
// too: ugrep, whose worker pool exits with searches still in flight.
//
// The window used to be fatal twice over. UninstallTLSState freed the alt
// stack before telling the kernel to stop using it, so setup_rt_frame wrote
// the signal frame into memory the 64-bit allocator had just re-reserved
// PROT_NONE; the kernel answers a failed frame write with force_sigsegv(),
// which is SIGSEGV/SI_KERNEL and unblockable, taking the whole process down
// (exit 139). And once the thread object was unreachable, SignalHandlerThunk's
// escape set SIG_DFL for the signal it could not dispatch -- process-wide, for
// real-time signal 63, whose default action is terminate (exit 191).
//
// Prints nothing and exits 0. One run proves nothing about a race; run.sh
// loops it, and before the fix roughly half of those runs died.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SPAWNERS 6
#define PER_SPAWNER 200
// How many short-lived threads must have got going before main pulls the
// plug. High enough that the stream is at rate, low enough that the run stays
// well under a second.
#define TRIGGER 80

static atomic_int Started;
static atomic_int Go;

static void Spin(unsigned long N) {
  for (volatile unsigned long i = 0; i < N; ++i) {
  }
}

static void* Short(void* Arg) {
  (void)Arg;
  atomic_fetch_add(&Started, 1);
  Spin(1500);
  syscall(SYS_exit, 0);
  return NULL;
}

static void* Spawner(void* Arg) {
  (void)Arg;
  pthread_attr_t At;
  pthread_attr_init(&At);
  pthread_attr_setdetachstate(&At, PTHREAD_CREATE_DETACHED);
  for (int i = 0; i < PER_SPAWNER && atomic_load(&Go); ++i) {
    pthread_t T;
    if (pthread_create(&T, &At, Short, NULL) != 0) {
      // Out of stacks for the moment; let some drain.
      Spin(3000);
    }
  }
  syscall(SYS_exit, 0);
  return NULL;
}

int main(void) {
  atomic_store(&Go, 1);

  pthread_attr_t At;
  pthread_attr_init(&At);
  pthread_attr_setdetachstate(&At, PTHREAD_CREATE_DETACHED);
  for (int i = 0; i < SPAWNERS; ++i) {
    pthread_t T;
    if (pthread_create(&T, &At, Spawner, NULL) != 0) {
      return 1;
    }
  }

  while (atomic_load(&Started) < TRIGGER) {
    Spin(50);
  }

  // Deliberately no join: leave while threads are still tearing down.
  syscall(SYS_exit_group, 0);
  return 0;
}
