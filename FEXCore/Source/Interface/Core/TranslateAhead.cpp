// SPDX-License-Identifier: MIT
#include "Interface/Context/Context.h"
#include "Interface/Core/LookupCache.h"
#include "Interface/Core/TranslateAhead.h"

#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/LogManager.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace FEXCore::Context {

TranslateAheadService::~TranslateAheadService() {
  Shutdown();
}

void TranslateAheadService::Queue(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP) {
  if (!GuestRIP || StopRequested.load(std::memory_order_relaxed)) {
    return;
  }

  // Already translated: the common case for a backedge or a call to something
  // hot. This is the requesting thread's own lookup cache, the structure it
  // just used to get here, so the probe is free of new sharing.
  if (Thread->LookupCache->FindBlock(Thread, GuestRIP)) {
    return;
  }

  // Never hand the decoder an address the syscall layer does not call
  // executable: the helper is not a guest thread and must not take a fault on
  // guest memory, and a NOEXEC unit installed for a RIP the guest never
  // reaches is pure loss.
  if (!CTX->SyscallHandler || CTX->SyscallHandler->QueryGuestExecutableRange(Thread, GuestRIP).Size == 0) {
    return;
  }

  // Depth 1 from a guest thread, one deeper than the unit being compiled when
  // the helper itself is the caller. Running out of budget simply stops the
  // chain.
  const bool FromHelper = IsHelperThreadState(Thread);
  const uint8_t Depth = FromHelper ? static_cast<uint8_t>(CurrentDepth + 1) : uint8_t {1};
  if (Depth > MaxDepth) {
    return;
  }

  if (!FromHelper) {
    StartIfNeeded(Thread);
  }

  const uint32_t Index = WriteIndex.fetch_add(1, std::memory_order_relaxed);
  SlotDepth[Index & kQueueMask] = Depth;
  Slots[Index & kQueueMask].store(GuestRIP, std::memory_order_release);
}

void TranslateAheadService::StartIfNeeded(FEXCore::Core::InternalThreadState* Thread) {
  if (Started.load(std::memory_order_acquire)) {
    return;
  }

  // One starter wins; everybody else moves on without waiting. A process that
  // never compiles a unit with a constant exit never pays for the thread.
  uint32_t Expected = 0;
  if (!StartLock.compare_exchange_strong(Expected, 1, std::memory_order_acq_rel)) {
    return;
  }

  RequesterCPU.store(::sched_getcpu(), std::memory_order_relaxed);
  HelperOwnerPid.store(::getpid(), std::memory_order_relaxed);

  // Create the helper with every signal blocked so the kernel can never pick
  // it to deliver a process-directed guest signal: it has no guest context and
  // the signal delegator knows nothing about it.
  sigset_t All;
  sigset_t Saved;
  sigfillset(&All);
  pthread_sigmask(SIG_BLOCK, &All, &Saved);
  const int Err = pthread_create(&Helper, nullptr, &TranslateAheadService::ThreadEntry, this);
  pthread_sigmask(SIG_SETMASK, &Saved, nullptr);

  if (Err != 0) {
    LogMan::Msg::DFmt("TranslateAhead: helper thread creation failed ({}); disabled", Err);
    StopRequested.store(true, std::memory_order_release);
    return;
  }
  Started.store(true, std::memory_order_release);
  (void)Thread;
}

void* TranslateAheadService::ThreadEntry(void* Self) {
  static_cast<TranslateAheadService*>(Self)->Run();
  return nullptr;
}

// Keep the helper off the requesting thread's SMT siblings where the process's
// cpuset allows it, and make it unable to preempt any guest thread.
void TranslateAheadService::PlaceSelf(int Requester) {
  // SCHED_IDLE: runs only when nothing else on the runqueue wants the CPU, so
  // a translate-ahead can never cost the guest a timeslice. nice(19) is the
  // fallback where SCHED_IDLE is refused (it only reweights, it does not
  // guarantee, which is why it is second choice).
  sched_param Param {};
  if (::sched_setscheduler(0, SCHED_IDLE, &Param) != 0) {
    ::setpriority(PRIO_PROCESS, 0, 19);
  }

  if (Requester < 0) {
    return;
  }

  cpu_set_t Allowed;
  CPU_ZERO(&Allowed);
  if (::sched_getaffinity(0, sizeof(Allowed), &Allowed) != 0) {
    return;
  }

  // POWER9 is SMT4: a sibling shares the core's issue and cache resources, so
  // "another thread" is only free translation if it is another *core*.
  char Path[128];
  snprintf(Path, sizeof(Path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", Requester);
  FILE* F = ::fopen(Path, "re");
  if (!F) {
    return;
  }
  char Line[256] {};
  const bool Read = ::fgets(Line, sizeof(Line), F) != nullptr;
  ::fclose(F);
  if (!Read) {
    return;
  }

  cpu_set_t Preferred = Allowed;
  // "0-3" or "0,4,8,12" or a mix of both.
  for (const char* P = Line; *P;) {
    char* End = nullptr;
    const long First = ::strtol(P, &End, 10);
    if (End == P) {
      break;
    }
    long Last = First;
    if (*End == '-') {
      P = End + 1;
      Last = ::strtol(P, &End, 10);
    }
    for (long C = First; C <= Last && C < CPU_SETSIZE; ++C) {
      CPU_CLR(static_cast<int>(C), &Preferred);
    }
    P = (*End == ',') ? End + 1 : End;
    while (*P == ' ' || *P == '\n') {
      ++P;
    }
  }

  if (CPU_COUNT(&Preferred) > 0) {
    ::sched_setaffinity(0, sizeof(Preferred), &Preferred);
  }
}

void TranslateAheadService::Run() {
  Running.store(true, std::memory_order_release);
  pthread_setname_np(pthread_self(), "POWERarmXlate");
  PlaceSelf(RequesterCPU.load(std::memory_order_relaxed));

  // A compile-only thread state: everything CompileBlock needs (frontend,
  // passes, RA, backend, a lookup cache) and nothing that executes guest code.
  // Exactly what the offline compiler and the AOT generator use.
  FEXCore::Core::InternalThreadState* Thread = CTX->CreateThread(0, 0, nullptr);
  HelperThreadState.store(Thread, std::memory_order_release);

  // Idle backoff: spin while the guest is feeding us (a cold run keeps the
  // queue non-empty), then sleep in growing steps up to a millisecond so an
  // idle process does not poll. A millisecond of latency on the first unit
  // after an idle period costs nothing: the guest compiles it itself if it
  // gets there first.
  constexpr uint64_t kMinSleepNS = 20'000;
  constexpr uint64_t kMaxSleepNS = 1'000'000;
  uint64_t SleepNS = 0;

  while (!StopRequested.load(std::memory_order_acquire)) {
    const uint32_t Write = WriteIndex.load(std::memory_order_acquire);
    if (ReadIndex == Write) {
      if (SleepNS == 0) {
        SleepNS = kMinSleepNS;
        ::sched_yield();
      } else {
        struct timespec TS {
          .tv_sec = 0, .tv_nsec = static_cast<long>(SleepNS)
        };
        ::nanosleep(&TS, nullptr);
        SleepNS = std::min(SleepNS * 2, kMaxSleepNS);
      }
      continue;
    }

    // A producer that lapped us overwrote slots we never read; skip whatever
    // is older than the ring.
    if (Write - ReadIndex > kQueueSize) {
      ReadIndex = Write - kQueueSize;
    }

    const uint64_t GuestRIP = Slots[ReadIndex & kQueueMask].exchange(0, std::memory_order_acq_rel);
    CurrentDepth = SlotDepth[ReadIndex & kQueueMask];
    ++ReadIndex;
    SleepNS = 0;
    if (!GuestRIP) {
      continue;
    }

    // No lookup-cache probe here: the guest may well have compiled this unit
    // since it was queued, but CompileBlock opens with exactly that probe --
    // under the shared CodeInvalidationMutex, which is the only place it is
    // safe to read those structures from a thread that is not holding the
    // lock for another reason.
    CTX->CompileBlock(Thread->CurrentFrame, GuestRIP);
    Compiled.fetch_add(1, std::memory_order_relaxed);
  }

  HelperThreadState.store(nullptr, std::memory_order_release);
  CTX->DestroyThread(Thread);
  Running.store(false, std::memory_order_release);
}

void TranslateAheadService::Shutdown() {
  StopRequested.store(true, std::memory_order_release);
  if (!Started.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (HelperOwnerPid.load(std::memory_order_relaxed) != ::getpid()) {
    // A forked child inherited the flag but not the thread. Joining a
    // pthread_t that names no thread in this process blocks forever on a
    // futex nothing will ever wake. The code-cache writer child reaches here
    // through CodeCacheImageExit; ResetAfterFork covers guest forks, which go
    // through the LockBeforeFork handshake and arrive here already reset.
    return;
  }
  // The helper checks StopRequested between units, so the wait is bounded by
  // one compile plus one backoff sleep.
  ::pthread_join(Helper, nullptr);
}

void TranslateAheadService::ResetAfterFork() {
  // The helper thread did not come across the fork. Everything it owned is
  // therefore unreachable, and the queue may hold entries it had not read.
  // Nothing here needs a lock: fork() ran with CodeInvalidationMutex held
  // exclusively, so no producer was in flight either.
  Started.store(false, std::memory_order_relaxed);
  Running.store(false, std::memory_order_relaxed);
  StopRequested.store(false, std::memory_order_relaxed);
  StartLock.store(0, std::memory_order_relaxed);
  HelperOwnerPid.store(0, std::memory_order_relaxed);
  HelperThreadState.store(nullptr, std::memory_order_relaxed);
  Compiled.store(0, std::memory_order_relaxed);
  WriteIndex.store(0, std::memory_order_relaxed);
  ReadIndex = 0;
  for (auto& Slot : Slots) {
    Slot.store(0, std::memory_order_relaxed);
  }
  CurrentDepth = 0;
}

} // namespace FEXCore::Context
