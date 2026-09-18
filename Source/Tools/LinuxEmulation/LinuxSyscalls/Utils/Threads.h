// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/fextl/deque.h>
#include <FEXCore/fextl/memory.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace FEXCore::Threads {
class Thread;
}

namespace FEX::HLE {
struct ThreadStateObject;
}

namespace FEX::LinuxEmulation::Threads {
/**
 * @brief Size of the stack that this interface creates.
 */
constexpr size_t STACK_SIZE = 8 * 1024 * 1024;

/**
 * @brief PROT_NONE guard mapped below each stack that this interface creates.
 *
 * The stacks are anonymous mappings placed by the kernel among the guest's
 * own, so without it an overflowing host thread writes straight into whatever
 * the guest has mapped below. It is larger than any single host frame, so a
 * frame cannot step over it.
 */
constexpr size_t STACK_GUARD_SIZE = 1024 * 1024;

/**
 * @brief The address range reserved for the main thread's host stack.
 *
 * The guest's main thread runs on the stack the kernel gave the process. The
 * kernel grows that mapping down on demand, to at most RLIMIT_STACK below its
 * top, into whatever is unmapped there. The guest's main stack must not be
 * mapped in that range: host frames would then land in it with no fault at
 * all. See ReserveMainThreadStack().
 */
struct MainThreadStackRange {
  uint64_t GuardBase;   ///< Bottom of the guard.
  uint64_t GrowthLimit; ///< Top of the guard: the lowest address the stack may grow to.
  uint64_t Top;         ///< Top of the stack mapping.
};

/**
 * @brief Reserves the calling thread's stack range, which must be the main thread's.
 *
 * Maps a PROT_NONE guard of the kernel's stack guard gap (256 host pages)
 * directly below the lowest address the stack may grow to, and records the
 * guard and the growth range as host-owned, so a guest MAP_FIXED cannot put
 * memory there either. Call once, before the guest's stack is mapped, and keep
 * the guest stack below GuardBase or at or above Top.
 */
MainThreadStackRange ReserveMainThreadStack();

// Stack pool handling
struct StackPoolItem {
  void* Ptr;
  size_t Size;
};

struct DeadStackPoolItem {
  void* Ptr;
  size_t Size;
  bool ReadyToBeReaped;
};

class StackTracker final : public FEXCore::Allocator::FEXAllocOperators {
public:
  void* AllocateStackObject();
  bool* AddStackToDeadPool(void* Ptr);
  void AddStackToLivePool(void* Ptr);
  void RemoveStackFromLivePool(void* Ptr);

  void DeallocateStackObjectImmediately(void* Ptr);

  [[noreturn]]
  void DeallocateStackObjectAndExit(void* Ptr, int Status);

  void CleanupAfterFork_PThread();

  void Shutdown();

private:
  std::mutex DeadStackPoolMutex {};
  std::mutex LiveStackPoolMutex {};

  fextl::deque<DeadStackPoolItem> DeadStackPool {};
  fextl::deque<StackPoolItem> LiveStackPool {};
};

/**
 * @brief Allocates a stack object from the internally managed stack pool.
 */
void* AllocateStackObject();

/**
 * @brief Deallocates a stack from the internally managed stack pool.
 *
 * Will not free the memory immediately, instead saving for reuse temporarily to solve race conditions on stack usage while stack tears down.
 *
 * @param Ptr The stack base from `AllocateStackObject`
 * @param Status The status to pass to the exit syscall.
 */
[[noreturn]]
void DeallocateStackObjectAndExit(void* Ptr, int Status);

void* GetStackBase(FEXCore::Threads::Thread* ThreadObject);

[[noreturn]]
void LongjumpDeallocateAndExit(FEX::HLE::ThreadStateObject* ThreadObject, int Status);

/**
 * @brief Registers thread creation handlers with FEXCore.
 */
fextl::unique_ptr<StackTracker> SetupThreadHandlers();

/**
 * @brief Cleans up any remaining stack objects in the pools.
 */
void Shutdown(fextl::unique_ptr<StackTracker> STracker);
} // namespace FEX::LinuxEmulation::Threads
