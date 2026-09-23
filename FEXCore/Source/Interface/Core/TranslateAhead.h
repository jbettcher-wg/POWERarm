// SPDX-License-Identifier: MIT
#pragma once

// ===========================================================================
// Translate-ahead helper (cold research 4.4 / 5 G2)
// ===========================================================================
//
// THE PROBLEM. A cold one-shot spends most of its time translating on the
// guest thread's critical path: 55-60% of `gcc -c empty.c`, ~70% of a Claude
// launch. Every unit is compiled the instant the guest first needs it, so the
// guest waits out the whole 10-20 us.
//
// THE LEVER. The decoder already knows where a unit will go next: an exit with
// a compile-time-constant destination RIP names its successor. When a unit is
// published, its constant exit targets that are not already translated go on a
// queue, and one helper thread translates them through the same CompileBlock
// path a second guest thread would use. By the time the guest exits the unit,
// the successor is usually in the lookup cache and the guest never pays for
// it. The compile races the guest; CompileBlock's own "did someone beat me to
// it" check (Core.cpp, the FindBlock after GenerateIR) resolves ties, which is
// the same path a multi-threaded guest already exercises.
//
// WHY THIS IS NOT C12. The rejected background *install* of cached blocks
// (CODE-CACHE.md) lost because it did 1-3 us of work per block while
// contending with the guest thread on the lookup cache and the code buffer --
// the contention cost as much as the work saved. Here the work is the 10-20 us
// compile and the shared-structure writes are the same 1-3 us install, so the
// ratio is inverted. Two further things keep the helper off the guest's hot
// structures:
//   * the queue is a lock-free ring; a producer does one relaxed store and one
//     fetch_add, never a syscall and never a wait;
//   * emission no longer holds CodeBufferWriteMutex (the staging buffer
//     change), so the helper's translation does not block the guest's.
//
// PLACEMENT. The helper runs SCHED_IDLE so it can never take the guest's
// timeslice, and its affinity excludes the SMT siblings of the core the
// requesting guest thread was on when the helper started (P11: siblings share
// a POWER9 core's execution resources), as long as the process's cpuset leaves
// something else. Everything degrades to "inherit whatever we have" if the
// cpuset is tight or the topology cannot be read.
//
// SAFETY.
//   * The helper is not a guest thread. It has its own compile-only
//     InternalThreadState (ContextImpl::CreateThread, the same one the AOT
//     compiler uses), it never executes JIT code, and it blocks every signal
//     so the kernel never picks it to deliver a guest signal.
//   * It only queues addresses the syscall layer says are in an executable
//     guest range, so the decoder cannot fault on unmapped memory.
//   * It compiles through ContextImpl::CompileBlock, which takes the shared
//     CodeInvalidationMutex for its whole body, so an SMC invalidation cannot
//     run underneath it -- the same guarantee a guest thread compiling has.
//   * fork(): LockBeforeFork takes CodeInvalidationMutex exclusively, so no
//     compile (guest or helper) can be in flight when the child is created.
//     The child has no helper thread, so ResetAfterFork clears the queue and
//     lets the child start a fresh helper of its own on demand.
//   * Shutdown joins the helper before the context tears down, including at
//     the last guest thread's exit: a guest that leaves through SYS_exit (not
//     exit_group) ends only its own thread, and a live helper would hold the
//     process open as a thread of a zombie thread group.
//
// MEASURED, AND OFF BY DEFAULT. On cold `gcc -c empty.c` this moved 1.4k of
// 23.5k compiles off the guest thread at depth 1 (1.0k at depth 8), grew the
// code cache by 32%/77%, and cost wall time: the successors it can name are
// mostly exits the guest does not take, and the ones it does take are needed
// a microsecond after they are queued. Worse, the helper is observable --
// /proc/self/task's nlink is 2 + threads, which Chromium's sandbox uses to
// refuse a multi-threaded zygote -- and it costs address space under a guest
// RLIMIT_AS. See the TranslateAhead option's description for the numbers.

#include <FEXCore/Core/Context.h>

#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <sys/types.h>

namespace FEXCore::Core {
struct InternalThreadState;
}

namespace FEXCore::Context {
class ContextImpl;

class TranslateAheadService final {
public:
  explicit TranslateAheadService(ContextImpl* CTX)
    : CTX(CTX) {}

  void SetMaxDepth(uint8_t Depth) {
    MaxDepth = Depth;
  }
  ~TranslateAheadService();

  // Queue one speculative translation target. Called on the compile path from
  // the guest thread (depth 1) and from the helper itself (one deeper than the
  // unit it is compiling); lock free, allocation free, and a no-op once the
  // queue is full or the depth budget is spent.
  void Queue(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP);

  // Stops and joins the helper. Idempotent. Called from the context's
  // destructor and before the final code-cache save.
  void Shutdown();

  // Child side of a fork: the helper thread does not exist here.
  void ResetAfterFork();

  uint64_t GetCompiledCount() const {
    return Compiled.load(std::memory_order_relaxed);
  }

  // True for the helper's own compile-only thread state. CompileBlock uses it
  // to keep speculation one level deep (see the queueing site in Core.cpp).
  bool IsHelperThreadState(const FEXCore::Core::InternalThreadState* Thread) const {
    return Thread == HelperThreadState.load(std::memory_order_relaxed);
  }

private:
  static void* ThreadEntry(void* Self);
  void Run();
  void StartIfNeeded(FEXCore::Core::InternalThreadState* Thread);
  void PlaceSelf(int RequesterCPU);

  ContextImpl* CTX;

  // Power-of-two ring. Producers are guest threads, the consumer is the single
  // helper. A producer that laps the consumer overwrites an unconsumed slot:
  // that loses a speculation, which is by design cheaper than any backpressure.
  static constexpr uint32_t kQueueSize = 512;
  static constexpr uint32_t kQueueMask = kQueueSize - 1;
  std::atomic<uint64_t> Slots[kQueueSize] {};
  // Speculation depth of the matching slot, written before its RIP is
  // released and read after it is acquired.
  uint8_t SlotDepth[kQueueSize] {};
  std::atomic<uint32_t> WriteIndex {0};
  uint32_t ReadIndex {0}; // consumer-private

  // How far ahead of the guest the helper may run. Depth 1 alone (only the
  // guest queues) is useless in practice: the guest reaches a unit's successor
  // about a microsecond after the unit is published, while compiling it takes
  // ten to twenty, so the helper loses nearly every race and both threads
  // compile the same unit. Measured on cold `gcc -c empty.c`: depth 1 moved 6%
  // of the blocks off the guest thread and saved no time at all. The helper
  // has to be working on what the guest will want in ~100 us, not ~1 us.
  //
  // The cost of overshooting is translating units the guest never reaches:
  // wasted (SCHED_IDLE) CPU, code-buffer bytes and cache entries. The queue
  // bound plus this depth bound cap that per burst.
  // Set from the TranslateAheadDepth option; see its description.
  uint8_t MaxDepth {2};

  // Depth of the unit the helper is compiling right now. Consumer-private: the
  // helper is the only thread that reads or writes it, and the only caller of
  // Queue that consults it is CompileBlock running on the helper itself.
  uint8_t CurrentDepth {0};

  pthread_t Helper {};
  std::atomic<bool> Started {false};
  std::atomic<bool> StopRequested {false};
  std::atomic<bool> Running {false};
  std::atomic<int> RequesterCPU {-1};
  // pid that created the helper; a forked child must not try to join it.
  std::atomic<pid_t> HelperOwnerPid {0};
  std::atomic<FEXCore::Core::InternalThreadState*> HelperThreadState {nullptr};
  std::atomic<uint64_t> Compiled {0};
  std::atomic<uint32_t> StartLock {0};
};

} // namespace FEXCore::Context
