// SPDX-License-Identifier: MIT
//
// fork() from a multi-threaded process, then execve() in the child without
// ever returning to the parent's code: the shape of Firefox spawning helpers
// from its ~80-thread main process, and of glycin (GTK's image loaders),
// which sets an address-space limit in the forked child before it execs
// bwrap.
//
// Under POWERarm the execve in the child is where the code cache saves the
// blocks the child compiled, and every save lands on the same cache files, so
// successive children fill a file's segment names and one of them compacts
// it. Two rounds of children:
//
//   plain    the child runs code the parent never ran and execs this program
//            again, which prints a line (with the RLIMIT_AS it inherited) and
//            exits with a status the parent checks.
//   rlimit   the same, with RLIMIT_AS lowered to 4 GiB first, as glycin does,
//            and the child's own code run after that. getrlimit must read the
//            new limit back and the exec'd image must start under it.
//
// The rlimit round is the one that crashed: POWERarm's address-space
// reservation (128 TiB) is far over any such limit, and with the process over
// its RLIMIT_AS the kernel refuses every mmap, so the emulator could not
// allocate between the setrlimit and the execve. Its 64-bit allocator then
// returned an address it had failed to map, and the pre-exec code-cache save
// read a segment through it.
//
// Four threads keep running in the parent throughout.
//
// The exec'd image re-runs this binary, so run.sh runs it with
// POWERARM_PORTABLE=1 (the child must stay on the build under test) and a
// fresh code cache directory (so the children really compile, save and
// compact every run).
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define THREADS 4
#define CHILDREN 12

static atomic_int Stop;
static atomic_ulong Work[THREADS];

static void* Spin(void* Arg) {
  const int Id = (int)(intptr_t)Arg;
  uint64_t X = 0x9e3779b97f4a7c15ull * (uint64_t)(Id + 1);
  while (!atomic_load_explicit(&Stop, memory_order_relaxed)) {
    X ^= X << 13;
    X ^= X >> 7;
    X ^= X << 17;
    atomic_fetch_add_explicit(&Work[Id], 1, memory_order_relaxed);
  }
  return (void*)(uintptr_t)X;
}

// Only children run these, so their blocks are compiled after the fork: 16
// small functions per child, a different set for each child, so every child
// has blocks of its own to save and none can load them from the cache.
#define LEAF(N, K) \
  __attribute__((noinline)) static uint64_t Leaf##N##_##K(uint64_t V) { \
    for (int i = 0; i < 3 + K; ++i) { \
      V = V * 6364136223846793005ull + 1442695040888963407ull + (N * 16 + K); \
      if (V & (1ull << (K + 7))) { \
        V ^= V >> (K % 29 + 3); \
      } else { \
        V += V << (N % 13 + 1); \
      } \
    } \
    return V; \
  }
#define CHILD_FN(N) \
  LEAF(N, 0) LEAF(N, 1) LEAF(N, 2) LEAF(N, 3) LEAF(N, 4) LEAF(N, 5) LEAF(N, 6) LEAF(N, 7) \
  LEAF(N, 8) LEAF(N, 9) LEAF(N, 10) LEAF(N, 11) LEAF(N, 12) LEAF(N, 13) LEAF(N, 14) LEAF(N, 15) \
  __attribute__((noinline)) static uint64_t ChildWork##N(uint64_t V) { \
    V = Leaf##N##_0(V); V = Leaf##N##_1(V); V = Leaf##N##_2(V); V = Leaf##N##_3(V); \
    V = Leaf##N##_4(V); V = Leaf##N##_5(V); V = Leaf##N##_6(V); V = Leaf##N##_7(V); \
    V = Leaf##N##_8(V); V = Leaf##N##_9(V); V = Leaf##N##_10(V); V = Leaf##N##_11(V); \
    V = Leaf##N##_12(V); V = Leaf##N##_13(V); V = Leaf##N##_14(V); V = Leaf##N##_15(V); \
    return V; \
  }
CHILD_FN(0)
CHILD_FN(1)
CHILD_FN(2)
CHILD_FN(3)
CHILD_FN(4)
CHILD_FN(5)
CHILD_FN(6)
CHILD_FN(7)
CHILD_FN(8)
CHILD_FN(9)
CHILD_FN(10)
CHILD_FN(11)
CHILD_FN(12)
CHILD_FN(13)
CHILD_FN(14)
CHILD_FN(15)
CHILD_FN(16)
CHILD_FN(17)
CHILD_FN(18)
CHILD_FN(19)
CHILD_FN(20)
CHILD_FN(21)
CHILD_FN(22)
CHILD_FN(23)
// Children of the plain round run the first half, of the rlimit round the
// second: a set one child saved would be loaded from the cache, not compiled.
static uint64_t (*const ChildFns[2 * CHILDREN])(uint64_t) = {
  ChildWork0,  ChildWork1,  ChildWork2,  ChildWork3,  ChildWork4,  ChildWork5,  ChildWork6,  ChildWork7,
  ChildWork8,  ChildWork9,  ChildWork10, ChildWork11, ChildWork12, ChildWork13, ChildWork14, ChildWork15,
  ChildWork16, ChildWork17, ChildWork18, ChildWork19, ChildWork20, ChildWork21, ChildWork22, ChildWork23,
};

static int RunChild(const char* Self, const char* Round, int Index) {
  const pid_t Pid = fork();
  if (Pid < 0) {
    printf("%s %d: fork failed\n", Round, Index);
    return -1;
  }
  if (Pid == 0) {
    uint64_t V;
    if (strcmp(Round, "rlimit") == 0) {
      const struct rlimit Limit = {4ull << 30, 4ull << 30};
      struct rlimit Now;
      if (setrlimit(RLIMIT_AS, &Limit) != 0) {
        _exit(120);
      }
      if (getrlimit(RLIMIT_AS, &Now) != 0 || Now.rlim_cur != Limit.rlim_cur || Now.rlim_max != Limit.rlim_max) {
        _exit(122);
      }
      // Compiled with the limit in place.
      V = ChildFns[Index + CHILDREN]((uint64_t)Index);
    } else {
      V = ChildFns[Index]((uint64_t)Index);
    }
    char Arg[32];
    snprintf(Arg, sizeof(Arg), "%d", Index);
    char Value[32];
    snprintf(Value, sizeof(Value), "%lu", (unsigned long)(V & 0xffff));
    char* const Argv[] = {(char*)Self, (char*)"--exec", (char*)Round, Arg, Value, NULL};
    execv(Self, Argv);
    _exit(121);
  }
  int Status = 0;
  if (waitpid(Pid, &Status, 0) != Pid) {
    printf("%s %d: waitpid failed\n", Round, Index);
    return -1;
  }
  if (WIFEXITED(Status)) {
    printf("%s %d: exit %d\n", Round, Index, WEXITSTATUS(Status));
  } else if (WIFSIGNALED(Status)) {
    printf("%s %d: signal %d\n", Round, Index, WTERMSIG(Status));
  }
  fflush(stdout);
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 5 && strcmp(argv[1], "--exec") == 0) {
    // The exec'd image: its stdout is the parent's, so this line lands
    // between the parent's own lines, in order. It inherits the address-space
    // limit the child set.
    struct rlimit Limit;
    char Text[32] = "error";
    if (getrlimit(RLIMIT_AS, &Limit) == 0) {
      if (Limit.rlim_cur == RLIM_INFINITY) {
        snprintf(Text, sizeof(Text), "unlimited");
      } else {
        snprintf(Text, sizeof(Text), "%lu MiB", (unsigned long)(Limit.rlim_cur >> 20));
      }
    }
    printf("  exec'd %s %s value %s, RLIMIT_AS %s\n", argv[2], argv[3], argv[4], Text);
    fflush(stdout);
    return 40 + atoi(argv[3]);
  }

  setvbuf(stdout, NULL, _IOLBF, 0);
  pthread_t Threads[THREADS];
  for (int i = 0; i < THREADS; ++i) {
    pthread_create(&Threads[i], NULL, Spin, (void*)(intptr_t)i);
  }

  for (int i = 0; i < CHILDREN; ++i) {
    RunChild(argv[0], "plain", i);
  }
  for (int i = 0; i < CHILDREN; ++i) {
    RunChild(argv[0], "rlimit", i);
  }

  atomic_store(&Stop, 1);
  int Ran = 0;
  for (int i = 0; i < THREADS; ++i) {
    pthread_join(Threads[i], NULL);
    Ran += atomic_load(&Work[i]) != 0;
  }
  printf("threads ran: %d of %d\n", Ran, THREADS);
  return 0;
}
