// SPDX-License-Identifier: MIT
//
// A dead guest thread must not cost the process anything permanent.
//
// Every guest thread FEX creates gets its own LookupCache: one private,
// writable reservation of L2 table + entry pool + L1, 258 MiB at the default
// 64 GiB VirtualMemSize, whose 2 MiB L1 is prefaulted writable at creation so
// it is resident rather than lazy. ~LookupCache is the only thing that ever
// gives that back, and ThreadManager::HandleThreadDeletion deliberately never
// deletes the thread state: the kernel can deliver an in-flight signal after
// the alt stack is disabled, and the handler reads the ThreadStateObject out
// of alt-stack memory, so both objects are leaked on purpose to keep those
// reads valid. What was leaked with them was everything the thread OWNED.
//
// The bill does not arrive as RSS. Each leaked reservation is private and
// writable, so the kernel charges it again every time it copies the mm, and a
// guest that churns threads -- which is every Node/Bun/Electron guest --
// eventually cannot fork at all: clone() starts returning ENOMEM and every
// posix_spawn fails, down to a bare `true`, while the process keeps reading,
// writing and computing perfectly well on a machine with hundreds of
// gigabytes free. That is the wedge in HANDOVER's trap list.
//
// So the test is growth, not a crash. Create and join threads in a loop from
// one long-lived process and watch the process's own mapped footprint:
// unbounded growth before the fix (measured at 260 MiB per dead thread,
// perfectly linear), flat after. Then fork and reap a few times, because
// forking is the thing that actually fails, and because a spawn loop must be
// shown NOT to be the leak: posix_spawn is CLONE_VM|CLONE_VFORK without
// CLONE_THREAD, so FEX routes it through ForkGuest and no guest thread is
// created.
//
// /proc/<pid>/maps, not /proc/self/maps. On a 64K host FEX synthesises
// /proc/self/maps from its guest VMA tracking, so the emulator's own mappings
// -- the whole point here -- are invisible in it; the numeric path is passed
// through to the host file. Natively the two are the same file and there are
// no emulator mappings at all, so the check passes trivially and the golden is
// the same two lines either way.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

// Long enough that the emulator has finished its own one-time growth (code
// buffer, allocator arenas, thread-stack pool) before the window opens.
#define WARMUP 64
// The measured window. Before the fix this alone leaked ~33 GiB.
#define MEASURED 128
#define FORKS 32

// Generous on purpose: the pass/fail gap is three orders of magnitude (tens of
// GiB against a few hundred MiB), and the emulator is still allowed the odd
// code-buffer rotation inside the window.
#define LIMIT_MIB 2048

static unsigned long MappedBytes(void) {
  char Path[64];
  snprintf(Path, sizeof(Path), "/proc/%d/maps", (int)getpid());
  FILE* F = fopen(Path, "re");
  if (!F) {
    return 0;
  }
  char Line[1024];
  unsigned long Total = 0;
  while (fgets(Line, sizeof(Line), F)) {
    unsigned long Lo, Hi;
    char Prot[8];
    if (sscanf(Line, "%lx-%lx %4s", &Lo, &Hi, Prot) != 3) {
      continue;
    }
    // Skip PROT_NONE: FEX reserves its 128 TiB address-space region that way
    // and carves allocations out of it, so only the carved-out (mapped) part
    // is what a dead thread leaves behind.
    if (Prot[0] == '-' && Prot[1] == '-' && Prot[2] == '-') {
      continue;
    }
    Total += Hi - Lo;
  }
  fclose(F);
  return Total;
}

static unsigned long GrowthMiB(unsigned long Before, unsigned long After) {
  return After > Before ? (After - Before) >> 20 : 0;
}

static void* Noop(void* Arg) {
  (void)Arg;
  return NULL;
}

static int Churn(int Count) {
  for (int i = 0; i < Count; ++i) {
    pthread_t T;
    if (pthread_create(&T, NULL, Noop, NULL) != 0) {
      return 0;
    }
    pthread_join(T, NULL);
  }
  return 1;
}

int main(void) {
  if (!Churn(WARMUP)) {
    printf("threadleak: threads FAIL (pthread_create)\n");
    return 1;
  }

  const unsigned long Before = MappedBytes();
  if (!Churn(MEASURED)) {
    printf("threadleak: threads FAIL (pthread_create)\n");
    return 1;
  }
  const unsigned long AfterThreads = MappedBytes();

  const unsigned long ThreadGrowth = GrowthMiB(Before, AfterThreads);
  if (ThreadGrowth > LIMIT_MIB) {
    printf("threadleak: threads FAIL (grew %lu MiB over %d dead threads)\n", ThreadGrowth, MEASURED);
  } else {
    printf("threadleak: threads ok\n");
  }

  // Forking is what actually breaks when the leak is present, and a fork that
  // is refused is the whole observable symptom, so check the return value
  // rather than the footprint alone.
  int Forked = 0;
  for (int i = 0; i < FORKS; ++i) {
    const pid_t Pid = fork();
    if (Pid == 0) {
      _exit(0);
    }
    if (Pid < 0) {
      break;
    }
    int Status = 0;
    waitpid(Pid, &Status, 0);
    ++Forked;
  }
  const unsigned long ForkGrowth = GrowthMiB(AfterThreads, MappedBytes());
  if (Forked != FORKS) {
    printf("threadleak: forks FAIL (%d of %d)\n", Forked, FORKS);
  } else if (ForkGrowth > LIMIT_MIB) {
    printf("threadleak: forks FAIL (grew %lu MiB over %d forks)\n", ForkGrowth, FORKS);
  } else {
    printf("threadleak: forks ok\n");
  }
  return 0;
}
