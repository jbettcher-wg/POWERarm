// SPDX-License-Identifier: MIT
/*
$info$
category: glue ~ Logic that binds various parts together
meta: glue|driver ~ Emulation mainloop related glue logic
tags: glue|driver
desc: Glues Frontend, OpDispatcher and IR Opts & Compilation, LookupCache, Dispatcher and provides the Execution loop entrypoint
$end_info$
*/

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#ifndef ARCHITECTURE_ppc64le
#include "Interface/Core/ArchHelpers/Arm64Emitter.h"
#endif
#include "Interface/Core/LookupCache.h"
#include "Interface/Core/SMCSoftInvalidate.h"
#include "Interface/Core/SMCSemanticPatch.h"
#include "Interface/Core/CPUBackend.h"
#include "Interface/Core/A64Frontend/Decoder.h"
#include "Interface/Core/A64Frontend/IRBuilder.h"
#ifdef ARCHITECTURE_ppc64le
#include "Interface/Core/JIT/PPC64LE/JITClass.h"
#include "Interface/Core/JIT/PPC64LE/PPC64Dispatcher.h"
#else
#include "Interface/Core/JIT/JITClass.h"
#include "Interface/Core/Dispatcher/Dispatcher.h"
#endif
#include <Interface/GDBJIT/GDBJIT.h>
#include "Interface/IR/IR.h"
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/Passes/RegisterAllocationPass.h"
#include "Interface/IR/Passes.h"
#include "Interface/IR/PassManager.h"
#include "Interface/IR/RegisterAllocationData.h"
#include "Utils/Allocator.h"
#include "Utils/Allocator/HostAllocator.h"
#include <FEXCore/Utils/SpinWaitLock.h>
#include "Utils/variable_length_integer.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/Thunks.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/HLE/SourcecodeResolver.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/Event.h>
#include <FEXCore/Utils/File.h>
#include <FEXCore/Utils/LogManager.h>
#include "FEXCore/Utils/SignalScopeGuards.h"
#include <FEXCore/Utils/Threads.h>
#include <FEXCore/Utils/Profiler.h>
#include <FEXCore/Utils/SHMStats.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/set.h>
#include <FEXCore/fextl/sstream.h>
#include <FEXCore/fextl/vector.h>
#include <FEXHeaderUtils/Syscalls.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <signal.h>
#include <stdio.h>
#include <string_view>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <xxhash.h>
#ifdef ARCHITECTURE_ppc64le
#include <sys/platform/ppc.h>
#endif

// FEX_SMC_AUDIT compile-side sibling of the logger in SyscallsSMCTracking.cpp
// (same env var, same file, opened O_APPEND so lines from both interleave).
static int SMCAuditCompileFD() {
  static int fd = [] {
    const char* p = getenv("FEX_SMC_AUDIT");
    if (!p) {
      return -1;
    }
    return ::open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
  }();
  return fd;
}

// ---------------------------------------------------------------------------
// FEX_COMPILELOG=<path>: compile-rate diagnostic (append-only, raw fd, one
// dprintf per line so a SIGKILL'd game still leaves a readable timeline).
//
// Answers "what is the JIT compiling, how fast, and why" for a live process:
// per ~1 s window it appends the number of blocks actually compiled (races
// that found another thread's block are not counted), host bytes emitted,
// compiler wall time, invalidation calls/bytes, and the busiest guest 16 MiB
// regions since process start, so a compile storm can be attributed to a
// module range (a PE image base is stable for the life of the process) and
// separated from invalidation churn or a code-buffer rotation (which
// FEX_BUFSTATS logs; point both at different files).
//
// Off (no env var) the cost is one relaxed load of a null pointer per
// compile. On, the counters are relaxed atomics and the bucket table is a
// fixed open-addressed array with atomic keys, so no lock is taken on any
// path and nothing here may block a compiling thread.
namespace {
struct CompileLogState {
  int FD {-1};
  uint64_t BaseNS {};
  std::atomic<uint64_t> Compiles {};
  std::atomic<uint64_t> HostBytes {};
  std::atomic<uint64_t> CompileNS {};
  std::atomic<uint64_t> InvalCalls {};
  std::atomic<uint64_t> InvalBytes {};
  std::atomic<uint64_t> LastFlushSec {};
  // Last flushed totals, written only by the thread that won the flush.
  uint64_t PrevCompiles {}, PrevHostBytes {}, PrevCompileNS {}, PrevInvalCalls {}, PrevInvalBytes {};

  static constexpr size_t BUCKETS = 512;
  struct Bucket {
    std::atomic<uint64_t> Key {}; // (GuestRIP >> 24) + 1; 0 = empty
    std::atomic<uint64_t> Count {};
  };
  Bucket Buckets[BUCKETS];

  static uint64_t NowNS() {
    timespec Now {};
    clock_gettime(CLOCK_MONOTONIC, &Now);
    return static_cast<uint64_t>(Now.tv_sec) * 1000000000ull + static_cast<uint64_t>(Now.tv_nsec);
  }

  void RecordCompile(uint64_t GuestRIP, uint64_t Bytes, uint64_t NS) {
    Compiles.fetch_add(1, std::memory_order_relaxed);
    HostBytes.fetch_add(Bytes, std::memory_order_relaxed);
    CompileNS.fetch_add(NS, std::memory_order_relaxed);

    const uint64_t Key = (GuestRIP >> 24) + 1;
    size_t Idx = (Key * 0x9E3779B97F4A7C15ull) >> 55; // 9 bits
    for (size_t Probe = 0; Probe < BUCKETS; ++Probe, Idx = (Idx + 1) & (BUCKETS - 1)) {
      uint64_t Cur = Buckets[Idx].Key.load(std::memory_order_relaxed);
      if (Cur == 0) {
        if (!Buckets[Idx].Key.compare_exchange_strong(Cur, Key, std::memory_order_relaxed)) {
          if (Cur != Key) {
            continue;
          }
        }
        Cur = Key;
      }
      if (Cur == Key) {
        Buckets[Idx].Count.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
    MaybeFlush();
  }

  void RecordInvalidate(uint64_t Bytes) {
    InvalCalls.fetch_add(1, std::memory_order_relaxed);
    InvalBytes.fetch_add(Bytes, std::memory_order_relaxed);
  }

  void MaybeFlush() {
    const uint64_t Now = NowNS();
    const uint64_t Sec = (Now - BaseNS) / 1000000000ull;
    uint64_t Last = LastFlushSec.load(std::memory_order_relaxed);
    if (Sec == Last || !LastFlushSec.compare_exchange_strong(Last, Sec, std::memory_order_acq_rel)) {
      return;
    }
    const uint64_t C = Compiles.load(std::memory_order_relaxed);
    const uint64_t B = HostBytes.load(std::memory_order_relaxed);
    const uint64_t N = CompileNS.load(std::memory_order_relaxed);
    const uint64_t IC = InvalCalls.load(std::memory_order_relaxed);
    const uint64_t IB = InvalBytes.load(std::memory_order_relaxed);
    const uint64_t dC = C - PrevCompiles, dB = B - PrevHostBytes, dN = N - PrevCompileNS;
    const uint64_t dIC = IC - PrevInvalCalls, dIB = IB - PrevInvalBytes;
    PrevCompiles = C;
    PrevHostBytes = B;
    PrevCompileNS = N;
    PrevInvalCalls = IC;
    PrevInvalBytes = IB;

    // Top 6 buckets by cumulative count.
    struct Top {
      uint64_t Key, Count;
    } Tops[6] {};
    for (auto& Bkt : Buckets) {
      const uint64_t Key = Bkt.Key.load(std::memory_order_relaxed);
      if (!Key) {
        continue;
      }
      const uint64_t Count = Bkt.Count.load(std::memory_order_relaxed);
      for (size_t i = 0; i < 6; ++i) {
        if (Count > Tops[i].Count) {
          for (size_t j = 5; j > i; --j) {
            Tops[j] = Tops[j - 1];
          }
          Tops[i] = {Key, Count};
          break;
        }
      }
    }
    char TopBuf[6 * 40];
    int Off = 0;
    for (auto& T : Tops) {
      if (!T.Key) {
        break;
      }
      Off += snprintf(TopBuf + Off, sizeof(TopBuf) - Off, " %#" PRIx64 ":%" PRIu64, (T.Key - 1) << 24, T.Count);
    }
    const uint64_t Ms = (Now - BaseNS) / 1000000ull;
    dprintf(FD,
            "[pid %d +%" PRIu64 ".%03" PRIu64 "s] compiles=+%" PRIu64 " (%" PRIu64 ") hostbytes=+%" PRIu64 " (%" PRIu64 ") "
            "compile_ms=+%" PRIu64 " avg_us=%" PRIu64 " inval=+%" PRIu64 "/%" PRIu64 "B (%" PRIu64 "/%" PRIu64 "B) top16MiB:%s\n",
            getpid(), Ms / 1000, Ms % 1000, dC, C, dB, B, dN / 1000000ull, dC ? (dN / 1000ull) / dC : 0, dIC, dIB, IC, IB, TopBuf);
  }
};

CompileLogState* GetCompileLog() {
  static CompileLogState* State = [] () -> CompileLogState* {
    const char* p = getenv("FEX_COMPILELOG");
    if (!p) {
      return nullptr;
    }
    const int fd = ::open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
      return nullptr;
    }
    auto* S = new CompileLogState();
    S->FD = fd;
    S->BaseNS = CompileLogState::NowNS();
    dprintf(fd, "[pid %d +0.000s] compilelog start\n", getpid());
    return S;
  }();
  return State;
}
} // anonymous namespace

namespace FEXCore::Context {
// Host timebase frequency, used to scale the guest cycle counter (was in the x86 CPUID emulation).
static uint64_t GetCycleCounterFrequency() {
#if defined(ARCHITECTURE_ppc64le)
  return __ppc_get_timebase_freq();
#else
  return 0;
#endif
}

ContextImpl::ContextImpl(const FEXCore::HostFeatures& Features)
  : HostFeatures {Features}
  , CodeCache {*this} {
  FEXCore::SetCodeCacheHostFeatures(Features);
  if (Config.BlockJITNaming() || Config.GlobalJITNaming() || Config.LibraryJITNaming()) {
    // Only initialize symbols file if enabled. Ensures we don't pollute /tmp with empty files.
    Symbols.InitFile();
  }

  uint64_t FrequencyCounter = GetCycleCounterFrequency();
  if (FrequencyCounter && FrequencyCounter < FEXCore::Context::TSC_SCALE_MAXIMUM && Config.SmallTSCScale()) {
    // Scale TSC until it is at the minimum required.
    while (FrequencyCounter < FEXCore::Context::TSC_SCALE_MAXIMUM) {
      FrequencyCounter <<= 1;
      ++Config.TSCScale;
    }
  }

  // Track atomic TSO emulation configuration.
  UpdateAtomicTSOEmulationConfig();

  // SMC Idea 3: the code-granule bitmap exists purely to accelerate the SMC
  // store fast paths, so it is gated on exactly the flags that consult it -- no
  // new flag of its own. With all three off nothing is allocated and no reader
  // ever looks at it. This must be decided here, before the first CodeBuffer
  // (and therefore the first GuestToHostMap) is created; see the constructor in
  // Interface/Core/LookupCache.cpp for why it can never be switched on later.
  if (Config.SMCStoreEmulation() || Config.SMCStoreBackpatch() || Config.SMCSemanticPatch()) {
    FEXCore::SMC::CodeGranuleTrackingEnabled.store(true, std::memory_order_release);
  }

}

// Maps a host PC to the JIT block containing it.
//
// Audit P1: this used to read CpuStateFrame::State.InlineJITBlockHeader, which
// every JIT block's EntryPoint prologue stored on entry (5 instructions per
// block entry on ppc64le). That store is now elided by default
// (FEX_NOBLOCKHEADER), and the mapping instead comes from a per-CodeBuffer
// host-PC -> block index that the compiler fills in at CompileCode time
// (CPUBackend.h, CodeBuffer::AppendBlock / FindBlockHeader). "The current
// block" is therefore now defined as "the block containing HostPC", which is
// what every caller actually meant.
//
// ASYNC-SIGNAL-SAFE (CodeBuffer::FindBlockHeader is; this adds only a null
// check). Returns {nullptr, nullptr} when HostPC is in no block.
struct GetFrameBlockInfoResult {
  const CPU::CPUBackend::JITCodeHeader* InlineHeader;
  const CPU::CPUBackend::JITCodeTail* InlineTail;
};
static GetFrameBlockInfoResult GetFrameBlockInfo(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC) {
  // NonMovableUniquePtr has no operator bool — .get() is the null test.
  if (!Thread->CPUBackend.get()) {
    return {nullptr, nullptr};
  }

  auto InlineHeader = Thread->CPUBackend->FindBlockHeader(HostPC);
  if (!InlineHeader) {
    return {nullptr, nullptr};
  }

  auto InlineTail =
    reinterpret_cast<const CPU::CPUBackend::JITCodeTail*>(reinterpret_cast<uintptr_t>(InlineHeader) + InlineHeader->OffsetToBlockTail);
  return {InlineHeader, InlineTail};
}

bool ContextImpl::IsAddressInCurrentBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC, uint64_t Address, uint64_t Size) {
  auto [_, InlineTail] = GetFrameBlockInfo(Thread, HostPC);
  return InlineTail && (Address + Size > InlineTail->RIP && Address < InlineTail->RIP + InlineTail->GuestSize);
}

bool ContextImpl::IsCurrentBlockSingleInst(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC) {
  auto [_, InlineTail] = GetFrameBlockInfo(Thread, HostPC);
  return InlineTail && InlineTail->SingleInst;
}

uint64_t ContextImpl::GetGuestBlockEntry(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC) {
  auto [_, InlineTail] = GetFrameBlockInfo(Thread, HostPC);
  return InlineTail ? InlineTail->RIP : 0;
}

// FEX_RIPFALLBACKTRAP — instrumentation for "how often does RestoreRIPFromHostPC
// fail to reconstruct and fall back to Frame->State.pc, with a host PC that is
// inside the JIT code buffer?"
//
// That question decides whether the JIT is allowed to stop maintaining
// State.rip on block-transfer fast paths. Every exit currently spends a
// `std RIPReg, State.rip(STATE)` -- and, on a linked constant exit, the whole
// 3-4 instruction RIP materialisation that feeds it -- purely so this fallback
// has something sane to return.
//
// Audit P1 changed what the fallback means. Block lookup no longer depends on
// a store the entering block performs: the host PC is resolved through the
// per-CodeBuffer block index (CPUBackend.h), which is complete the moment
// CompileCode returns. There is therefore no longer a prologue-shaped window
// in which an in-JIT PC resolves to no block. What remains under kind 0 is a
// host PC that is inside a code buffer but inside no *block*: the aux
// allocations the SMC machinery carves out of the buffer's free tail
// (Context::AllocateJITAuxMemory), and the free tail itself.
//
// Modes (parsed once):
//   unset / "0"  off (default) -- one predictable branch on a static bool
//   "1"          log the first 64 occurrences of each kind, then just count
//   "abort"      ERROR_AND_DIE on the first occurrence
//
// This is the same retire-an-invariant methodology as FEX_DEADPROLOGUE=trap in
// the PPC64LE backend (JIT.cpp): prove the thing is unreachable at runtime
// before deleting the code that defends it, rather than by inspection.
//
// Reachable from signal handlers, so every arm must be async-signal-safe.
// The first version of this used LogMan::Msg::EFmt, which gates only on the
// compile-time MSG_LEVEL and then runs fmt formatting plus handler dispatch —
// neither is signal-safe, and the call sits on a path where FEX already holds
// signal-machinery locks. Running it against a real title crashed the title
// (Witcher 3, 2026-08-16, chased as a JIT bug before the trap was exonerated).
// Now: two relaxed atomics, a raw pre-formatted write(2) for the first 64 hits
// of each bucket, and an atexit dump so a run that never crashes still yields
// the counts it was started to collect. The mode static and the atexit
// registration are forced at library load (single-threaded, no guest, no
// signals) so neither can first-fire inside a handler.
namespace {
enum class RIPFallbackTrapMode { Off, Log, Abort };

RIPFallbackTrapMode GetRIPFallbackTrapMode() {
  static const RIPFallbackTrapMode Mode = []() {
    const char* Env = getenv("FEX_RIPFALLBACKTRAP");
    if (!Env || Env[0] == '\0' || (Env[0] == '0' && Env[1] == '\0')) {
      return RIPFallbackTrapMode::Off;
    }
    if (::strcmp(Env, "abort") == 0) {
      return RIPFallbackTrapMode::Abort;
    }
    return RIPFallbackTrapMode::Log;
  }();
  return Mode;
}

// [0] = host PC inside a code buffer but inside no indexed block (an aux SMC
//       stub allocation, or the buffer's free tail). Post-P1 this no longer
//       covers a block-transfer window -- there isn't one -- so a nonzero
//       count here now means "executing outside any block", which is a real
//       finding rather than a tolerated race.
// [1] = host PC inside a block, but the block carries no RIP table.
std::atomic<uint64_t> RIPFallbackHits[2] {};

// Minimal fixed-buffer line builder. No allocation, no locks, no fmt; the
// same shape as Syscalls.cpp's CloneTrace hex writer.
size_t RIPFallbackHex(char* Dst, uint64_t V) {
  char Tmp[16];
  size_t N = 0;
  if (V == 0) {
    Tmp[N++] = '0';
  }
  while (V) {
    const unsigned D = V & 0xF;
    Tmp[N++] = static_cast<char>(D < 10 ? '0' + D : 'a' + D - 10);
    V >>= 4;
  }
  size_t Len = 0;
  Dst[Len++] = '0';
  Dst[Len++] = 'x';
  while (N > 0) {
    Dst[Len++] = Tmp[--N];
  }
  return Len;
}

size_t RIPFallbackAppend(char* Dst, size_t At, const char* Text) {
  while (*Text) {
    Dst[At++] = *Text++;
  }
  return At;
}

void RIPFallbackWriteLine(unsigned Kind, uint64_t N, uint64_t HostPC, uint64_t BlockBegin, uint64_t StateRIP) {
  // Worst case well under 192 bytes: five hex values at 18 chars each plus the
  // fixed text.
  char Line[192];
  size_t At = RIPFallbackAppend(Line, 0, "RIPFallbackTrap[");
  Line[At++] = static_cast<char>('0' + (Kind & 1));
  At = RIPFallbackAppend(Line, At, "] #");
  At += RIPFallbackHex(Line + At, N);
  At = RIPFallbackAppend(Line, At, ": hostPC ");
  At += RIPFallbackHex(Line + At, HostPC);
  At = RIPFallbackAppend(Line, At, " hdr ");
  At += RIPFallbackHex(Line + At, BlockBegin);
  At = RIPFallbackAppend(Line, At, " -> stale State.rip ");
  At += RIPFallbackHex(Line + At, StateRIP);
  Line[At++] = '\n';
  ssize_t Ignored = ::write(STDERR_FILENO, Line, At);
  (void)Ignored;
}

// atexit: a completed run reports its counts even when no hit ever reached the
// first-64 window logging (and a zero/zero line is the positive result the
// instrument exists to produce). Raw write for consistency; atexit runs on the
// normal-exit path where that is merely cheap rather than required.
void RIPFallbackDumpCounters() {
  char Line[128];
  size_t At = RIPFallbackAppend(Line, 0, "RIPFallbackTrap exit: window=");
  At += RIPFallbackHex(Line + At, RIPFallbackHits[0].load(std::memory_order_relaxed));
  At = RIPFallbackAppend(Line, At, " noRIPTable=");
  At += RIPFallbackHex(Line + At, RIPFallbackHits[1].load(std::memory_order_relaxed));
  Line[At++] = '\n';
  ssize_t Ignored = ::write(STDERR_FILENO, Line, At);
  (void)Ignored;
}

// Forces the mode static (getenv) and registers the exit dump at library load,
// before main, before any guest thread and before any signal can deliver.
struct RIPFallbackInitializer {
  RIPFallbackInitializer() {
    if (GetRIPFallbackTrapMode() != RIPFallbackTrapMode::Off) {
      ::atexit(RIPFallbackDumpCounters);
    }
  }
} RIPFallbackInitializerInstance;

// FEX_RIPRECONLOG -- audit P1 cross-check.
//
// With FEX_NOBLOCKHEADER=0 the JIT still emits the legacy
// InlineJITBlockHeader store, so both block-lookup mechanisms are live at the
// same time. Setting FEX_RIPRECONLOG makes every RestoreRIPFromHostPC also
// compute the answer the OLD way (walk the block named by
// Frame->State.InlineJITBlockHeader when the host PC is inside it, else
// Frame->State.pc) and emit one line per call:
//
//   RIPRECON hostpc=0x.. tbl=0x.. hdr=0x.. same=0|1
//
// tbl = the per-CodeBuffer block-index answer (the new path, the one that
// ships), hdr = the legacy header-store answer. Run a signal storm under both
// and any `same=0` line is a divergence to explain before the store is
// retired for good.
//
// Same async-signal-safety contract as the fallback trap above: getenv is
// forced at library load, and the log arm is a fixed stack buffer plus one
// raw write(2). No fmt, no locks, no allocation.
bool GetRIPReconLogEnabled() {
  static const bool Enabled = []() {
    const char* Env = getenv("FEX_RIPRECONLOG");
    return Env && Env[0] != '\0' && !(Env[0] == '0' && Env[1] == '\0');
  }();
  return Enabled;
}

void RIPReconWriteLine(uint64_t HostPC, uint64_t TableRIP, uint64_t HeaderRIP) {
  // "RIPRECON hostpc=" + 18 + " tbl=" + 18 + " hdr=" + 18 + " same=X\n"
  // is well under 128 bytes.
  char Line[128];
  size_t At = RIPFallbackAppend(Line, 0, "RIPRECON hostpc=");
  At += RIPFallbackHex(Line + At, HostPC);
  At = RIPFallbackAppend(Line, At, " tbl=");
  At += RIPFallbackHex(Line + At, TableRIP);
  At = RIPFallbackAppend(Line, At, " hdr=");
  At += RIPFallbackHex(Line + At, HeaderRIP);
  At = RIPFallbackAppend(Line, At, " same=");
  Line[At++] = TableRIP == HeaderRIP ? '1' : '0';
  Line[At++] = '\n';
  ssize_t Ignored = ::write(STDERR_FILENO, Line, At);
  (void)Ignored;
}

// Walks a block's vl64pair per-instruction RIP table for HostPC. The caller
// has already established that HostPC lies inside [BlockBegin, BlockBegin +
// Tail->Size) and that the block has at least one entry.
uint64_t WalkBlockRIPTable(uint64_t BlockBegin, const CPU::CPUBackend::JITCodeHeader* Header,
                           const CPU::CPUBackend::JITCodeTail* Tail, uint64_t HostPC) {
  auto RIPEntry = reinterpret_cast<const uint8_t*>(BlockBegin + Header->OffsetToBlockTail + Tail->OffsetToRIPEntries);

  uint64_t StartingHostPC = BlockBegin;
  uint64_t StartingGuestRIP = Tail->RIP;

  for (uint32_t i = 0; i < Tail->NumberOfRIPEntries; ++i) {
    auto Offset = FEXCore::Utils::vl64pair::Decode(RIPEntry);
    RIPEntry += Offset.Size;
    if (HostPC >= (StartingHostPC + Offset.IntegerARMPC)) {
      // We are beyond this entry, keep going forward.
      StartingHostPC += Offset.IntegerARMPC;
      StartingGuestRIP += Offset.IntegerX86RIP;
    } else {
      // Passed where the Host PC is at. Break now.
      break;
    }
  }
  return StartingGuestRIP;
}

// The pre-P1 answer, computed from the legacy InlineJITBlockHeader store.
// Only meaningful when the JIT is still emitting that store
// (FEX_NOBLOCKHEADER=0); with the store elided the field stays whatever it
// last was, which is exactly the staleness this cross-check is meant to
// expose. Async-signal-safe: pure loads over JIT-owned memory.
uint64_t LegacyRIPFromInlineHeader(FEXCore::Core::CpuStateFrame* Frame, uint64_t HostPC) {
  const uint64_t BlockBegin = Frame->State.InlineJITBlockHeader;
  if (!BlockBegin) {
    return Frame->State.pc;
  }

  auto Header = reinterpret_cast<const CPU::CPUBackend::JITCodeHeader*>(BlockBegin);
  auto Tail = reinterpret_cast<const CPU::CPUBackend::JITCodeTail*>(BlockBegin + Header->OffsetToBlockTail);
  if (HostPC < BlockBegin || HostPC >= BlockBegin + Tail->Size || Tail->NumberOfRIPEntries == 0) {
    return Frame->State.pc;
  }

  return WalkBlockRIPTable(BlockBegin, Header, Tail, HostPC);
}

// Forces the FEX_RIPRECONLOG getenv at library load, before main, before any
// guest thread and before any signal can deliver — the same reason
// RIPFallbackInitializer exists. A function-local static initialised inside a
// signal handler would take the guard variable's lock there.
struct RIPReconInitializer {
  RIPReconInitializer() {
    GetRIPReconLogEnabled();
  }
} RIPReconInitializerInstance;

void NoteRIPFallback(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC, uint64_t BlockBegin, unsigned Kind) {
  const auto Mode = GetRIPFallbackTrapMode();
  if (Mode == RIPFallbackTrapMode::Off) {
    return;
  }
  // Only in-JIT PCs are interesting. A host PC in the dispatcher, in a FABI
  // stub or in C++ is an expected fallback and always has been.
  // NonMovableUniquePtr has no operator bool — .get() is the null test.
  if (!Thread->CPUBackend.get() || !Thread->CPUBackend->IsAddressInCodeBuffer(HostPC)) {
    return;
  }
  const uint64_t N = RIPFallbackHits[Kind].fetch_add(1, std::memory_order_relaxed);
  if (Mode == RIPFallbackTrapMode::Abort) {
    // Raw write + abort rather than ERROR_AND_DIE_FMT: dying is the point, but
    // deadlocking inside fmt/LogMan while holding signal-machinery locks is a
    // hang, not a diagnostic.
    RIPFallbackWriteLine(Kind, N, HostPC, BlockBegin, Thread->CurrentFrame->State.pc);
    ::abort();
  }
  if (N < 64) {
    RIPFallbackWriteLine(Kind, N, HostPC, BlockBegin, Thread->CurrentFrame->State.pc);
  }
}
} // namespace

uint64_t ContextImpl::RestoreRIPFromHostPC(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC) {
  const auto Frame = Thread->CurrentFrame;

  // Audit P1: the block containing HostPC comes from the per-CodeBuffer block
  // index, not from a store the block made on entry. FindBlockHeader has
  // already range-checked HostPC against [BlockBegin, BlockBegin + Tail->Size),
  // so reaching a non-null header means the PC really is inside this block.
  auto [InlineHeader, InlineTail] = GetFrameBlockInfo(Thread, HostPC);
  const uint64_t BlockBegin = reinterpret_cast<uint64_t>(InlineHeader);

  uint64_t Result;
  if (InlineHeader) {
    // If the block did not emit a per-instruction RIP table, fall through
    // to Frame->State.pc. Without this guard the reconstruction would
    // return the block-entry RIP, which is coarser than the syscall-site
    // RIP that Frame->State.pc already stores today (SeccompEmulator
    // reads .instruction_pointer via this path). Header-only blocks —
    // e.g. blocks with no CanHaveSideEffects IR ops that would emit
    // GuestOpcode markers — hit this branch. (Ppc64le emits entries since
    // P3.1 landed as 244075383; the old comment claiming otherwise was
    // stale, per P5.0 review.)
    if (InlineTail->NumberOfRIPEntries == 0) {
      NoteRIPFallback(Thread, HostPC, BlockBegin, 1);
      Result = Frame->State.pc;
    } else {
      // Reconstruct RIP from JIT entries for this block.
      Result = WalkBlockRIPTable(BlockBegin, InlineHeader, InlineTail, HostPC);
    }
  } else {
    // Host PC is in no block: the dispatcher, a FABI stub, C++, an aux SMC
    // stub allocation, or the code buffer's free tail. Fall back to what is
    // stored in the RIP currently.
    NoteRIPFallback(Thread, HostPC, BlockBegin, 0);
    Result = Frame->State.pc;
  }

  if (GetRIPReconLogEnabled()) {
    // Cross-check against the legacy header-store path. Only meaningful with
    // FEX_NOBLOCKHEADER=0, which is what keeps that store alive.
    RIPReconWriteLine(HostPC, Result, LegacyRIPFromInlineHeader(Frame, HostPC));
  }

  return Result;
}

bool ContextImpl::InitCore() {
  // Initialize the CPU core signal handlers & DispatcherConfig
#ifdef ARCHITECTURE_ppc64le
  Dispatcher = FEXCore::CPU::PPC64Dispatcher::Create(this);
#else
  Dispatcher = FEXCore::CPU::Dispatcher::Create(this);
#endif

  // Set up the SignalDelegator config so it knows our dispatcher's code range,
  // SRA mappings, and handler addresses.
  SignalDelegation->SetConfig(Dispatcher->MakeSignalDelegatorConfig());

#if defined(_WIN32) && !defined(ARCHITECTURE_arm64ec)
  // WOW64 always needs the interrupt fault check to be enabled.
  Config.NeedsPendingInterruptFaultCheck = true;
#endif

  if (Config.GdbServer) {
    // If gdbserver is enabled then this needs to be enabled.
    Config.NeedsPendingInterruptFaultCheck = true;
  }

  // One-shot warning: LockOnlyTSO is a knowingly-unsound option. It is not a
  // tuning knob with a theoretical risk — the MP litmus shape fires under it
  // and never fires without it, on the same guest binary. Emitted once here
  // rather than at the per-instruction consumption site below (which runs
  // inside the block compiler) so the cost is a single line per process.
  //
  // Written straight to stderr, NOT through LogMan. FEX_SILENTLOG defaults to
  // 1, and on that path FEXInterpreter uninstalls the LogMan message handler
  // outright (Source/Tools/FEXInterpreter/FEXInterpreter.cpp:107-109) — a
  // LogMan::Msg::EFmt here is dropped on the floor in the default
  // configuration, which is precisely the configuration a user enabling this
  // knob is in. A warning nobody sees is not a warning. Three lines, once per
  // process, and only when the user explicitly asked for unsound lowering.
  if (Config.LockOnlyTSO() && Config.TSOEnabled()) {
    static constexpr std::string_view LockOnlyTSOWarning =
      "POWERarm: POWERARM_LOCKONLYTSO=1 is UNSOUND. Only LOCK-prefixed guest operations keep TSO\n"
      "     ordering; plain loads and stores lose their barriers, so the guest can observe\n"
      "     memory orderings x86 forbids.\n"
      "     Measured on this port, same guest binary: the MP litmus shape fired 659, 12 and\n"
      "     51 times per 30000 rounds with it on, and 0 times in 150000 rounds with it off.\n"
      "     IRIW agrees: 552/1000000 on, 0/67200000 off. A seq_cst-shaped test does NOT\n"
      "     catch it. See powerpc64le-handbook/probes/atomics_litmus.c.\n"
      "     Use only where a wrong answer is acceptable.\n";
    // write(2) rather than fwrite: no locale, no buffering, nothing to flush,
    // and no interleaving with a guest that has its own stdio state.
    [[maybe_unused]] const auto Written = ::write(STDERR_FILENO, LockOnlyTSOWarning.data(), LockOnlyTSOWarning.size());
  }

  return true;
}

void ContextImpl::HandleCallback(FEXCore::Core::InternalThreadState* Thread, uint64_t RIP) {
  static_cast<ContextImpl*>(Thread->CTX)->Dispatcher->ExecuteJITCallback(Thread->CurrentFrame, RIP);
}

void ContextImpl::ExecuteThread(FEXCore::Core::InternalThreadState* Thread) {
  // Update the thread pointer for Thunk return to the latest.
  Thread->CurrentFrame->Pointers.ThunkCallbackRet = SignalDelegation->GetThunkCallbackRET();

  Dispatcher->ExecuteDispatch(Thread->CurrentFrame);

  // If it is the parent thread that died then just leave
  // TODO: This doesn't make sense when the parent thread doesn't outlive its children
}

void ContextImpl::InitializeCompiler(FEXCore::Core::InternalThreadState* Thread) {
  Thread->OpDispatcher = fextl::make_unique<FEXCore::A64::IRBuilder>(this);
  Thread->LookupCache = fextl::make_unique<FEXCore::LookupCache>(this);
  Thread->FrontendDecoder = fextl::make_unique<FEXCore::A64::Decoder>(Thread);
  Thread->PassManager = fextl::make_unique<FEXCore::IR::PassManager>();

  Thread->CurrentFrame->State.L1Pointer = Thread->LookupCache->GetL1Pointer();
  Thread->CurrentFrame->State.L1Mask = Thread->LookupCache->GetScaledL1PointerMask();

  Thread->CurrentFrame->Pointers.L2Pointer = Thread->LookupCache->GetPagePointer();

  Dispatcher->InitThreadPointers(Thread);

  Thread->PassManager->AddDefaultPasses(this);
  Thread->PassManager->AddDefaultValidationPasses();

  Thread->PassManager->RegisterSyscallHandler(SyscallHandler);

  // Create CPU backend
  Thread->PassManager->InsertRegisterAllocationPass(this);
#ifdef ARCHITECTURE_ppc64le
  Thread->CPUBackend = FEXCore::CPU::CreatePPC64JITCore(this, Thread);
#else
  Thread->CPUBackend = FEXCore::CPU::CreateArm64JITCore(this, Thread);
#endif

  Thread->PassManager->Finalize();
}

FEXCore::Core::InternalThreadState*
ContextImpl::CreateThread(uint64_t InitialRIP, uint64_t StackPointer, const FEXCore::Core::CPUState* NewThreadState) {
  FEXCore::Core::InternalThreadState* Thread = new FEXCore::Core::InternalThreadState {
    .CTX = this,
  };
  FEXCore::Allocator::VirtualName("POWERarmMem_ThreadState", Thread, sizeof(*Thread));

  // One host page for the deferred-signal interrupt fault page. It is mapped
  // separately from the thread state precisely so that its address is host-page
  // aligned whatever the host page size is: the arming mprotect in the signal
  // delegator needs that, and used to get it by accident from an alignas(4096)
  // thread state that only worked on a 4K kernel.
  {
    const size_t PageSize = FEXCore::HostPage::Size();
    void* FaultPage = ::mmap(nullptr, PageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (FaultPage == MAP_FAILED) {
      ERROR_AND_DIE_FMT("Failed to allocate the interrupt fault page for a new thread");
    }
    FEXCore::Allocator::VirtualName("POWERarmMem_InterruptFaultPage", FaultPage, PageSize);
    Thread->BaseFrameState.InterruptFaultPagePtr = static_cast<uint8_t*>(FaultPage);
  }

  Thread->CurrentFrame->State.sp = StackPointer;
  Thread->CurrentFrame->State.pc = InitialRIP;

  // Copy over the new thread state to the new object
  if (NewThreadState) {
    memcpy(&Thread->CurrentFrame->State, NewThreadState, sizeof(FEXCore::Core::CPUState));
  }

  // Set up the thread manager state
  Thread->CurrentFrame->Thread = Thread;

  InitializeCompiler(Thread);

  Thread->CurrentFrame->State.DeferredSignalRefCount.Store(0);

  if (Config.BlockJITNaming() || Config.GlobalJITNaming() || Config.LibraryJITNaming()) {
    // Allocate a JIT symbol buffer only if enabled.
    Thread->SymbolBuffer = JITSymbols::AllocateBuffer();
  }

  return Thread;
}

void ContextImpl::DestroyThread(FEXCore::Core::InternalThreadState* Thread) {
  // The page may be sitting at PROT_NONE (a deferred signal was armed and never
  // drained). Restore it before the mapping goes away so nothing that is still
  // unwinding takes a fault on a dangling address, then release it.
  if (auto* FaultPage = Thread->CurrentFrame->InterruptFaultPagePtr) {
    const size_t PageSize = FEXCore::HostPage::Size();
    FEXCore::Allocator::VirtualProtect(FaultPage, PageSize, Allocator::ProtectOptions::Read | Allocator::ProtectOptions::Write);
    Thread->CurrentFrame->InterruptFaultPagePtr = nullptr;
    ::munmap(FaultPage, PageSize);
  }
  delete Thread;
}

#ifndef _WIN32
void ContextImpl::UnlockAfterFork(FEXCore::Core::InternalThreadState* LiveThread, bool Child) {
  Allocator::UnlockAfterFork(LiveThread, Child);

  Profiler::PostForkAction(Child);
  if (Child) {
    if (CodeMapWriter) {
      CodeMapWriter->ResetAfterFork();
    }
    CodeCache.ResetAfterFork();

    CodeInvalidationMutex.StealAndDropActiveLocks();
    if (Config.StrictInProcessSplitLocks) {
      StrictSplitLockMutex = 0;
    }
  } else {
    CodeInvalidationMutex.unlock();
    if (Config.StrictInProcessSplitLocks) {
      FEXCore::Utils::SpinWaitLock::unlock(&StrictSplitLockMutex);
    }
    return;
  }
}

void ContextImpl::LockBeforeFork(FEXCore::Core::InternalThreadState* Thread) {
  CodeInvalidationMutex.lock();
  Allocator::LockBeforeFork(Thread);
  if (Config.StrictInProcessSplitLocks) {
    FEXCore::Utils::SpinWaitLock::lock(&StrictSplitLockMutex);
  }
}
#endif

void ContextImpl::OnCodeBufferAllocated(const fextl::shared_ptr<CPU::CodeBuffer>& Buffer) {
  if (Config.GlobalJITNaming()) {
    Symbols.RegisterJITSpace(Buffer->Ptr, Buffer->AllocatedSize);
  }

  {
    std::scoped_lock lk {CodeBufferListLock};
    CodeBufferList.emplace_back(Buffer);
  }
}

void ContextImpl::ClearCodeCache(FEXCore::Core::InternalThreadState* Thread, bool NewCodeBuffer) {
  FEXCORE_PROFILE_INSTANT("ClearCodeCache");

  if (NewCodeBuffer) {
    // Every offset in the relocation sink is relative to the buffer that is
    // about to be abandoned, and every block it describes goes with it. Applying
    // them to the new buffer would patch unrelated code.
    CodeCache.ResetRelocations();
    CodeCache.BlocksSinceSave.store(0, std::memory_order_relaxed);

    // Allocate new CodeBuffer + L3 LookupCache and clear L1+L2 caches
    Thread->CPUBackend->ClearCache();
  } else {
    // Clear L1+L2 cache of this thread, and clear L3 cache across any threads using it.
    //
    // HAZARD: GuestToHostMap::ClearCache (LookupCache.cpp:101-106) drops the
    // BlockLinks map without running any delinker callback. The code buffer,
    // however, stays live in this branch (NewCodeBuffer=false) — its patched
    // callsites still branch to what USED to be linked HostCode entries. Once
    // BlockLinks is gone, those callsites resolve into now-freed slots and any
    // subsequent dispatch through them is undefined.
    //
    // Trip a Release-visible trap here rather than a LOGMAN_* assertion because
    // those compile out in Release (LogManager.h:69-74,130-136) and would let
    // the hazard reach users silently. Today the only caller is
    // ThreadManager::Step (ThreadManager.cpp:535), which is itself stubbed
    // ("not implemented"), so this guard is unreachable in normal execution;
    // if Step gets a real implementation or a new caller appears, they must
    // land a delink walk (or a full code-buffer rotation via NewCodeBuffer=true)
    // before this path becomes correct to take.
    ERROR_AND_DIE_FMT("ClearCodeCache(NewCodeBuffer=false) reached without a delink walk; "
                      "live code buffer still contains patched callsites into cleared BlockLinks");
    auto lk = Thread->LookupCache->AcquireWriteLock();
    Thread->LookupCache->ClearCache(lk);
  }
  Allocator::VirtualDontNeed(Thread->CallRetStackBase, FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE);
}

static void IRDumper(FEXCore::Core::InternalThreadState* Thread, IR::IREmitter* IREmitter, uint64_t GuestRIP) {
  FEXCore::File::File FD = FEXCore::File::File::GetStdERR();
  fextl::stringstream out;
  auto NewIR = IREmitter->ViewIR();
  FEXCore::IR::Dump(&out, &NewIR);
  fextl::fmt::print(FD, "IR-ShouldDump-{} 0x{:x}:\n{}\n@@@@@\n", NewIR.PostRA() ? "post" : "pre", GuestRIP, out.str());
};

bool ContextImpl::CheckIfBlockIsCacheable(FEXCore::Core::InternalThreadState& Thread, uint64_t GuestRIP, uint64_t MaxInst) {
  return Thread.FrontendDecoder->CheckIfCacheable(Thread, GuestRIP, MaxInst);
}

ContextImpl::GenerateIRResult
ContextImpl::GenerateIR(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP, bool ExtendedDebugInfo, uint64_t MaxInst) {
  FEXCORE_PROFILE_SCOPED("GenerateIR");

  Thread->OpDispatcher->ResetWorkingList();

  uint64_t TotalInstructions {0};
  uint64_t TotalInstructionsLength {0};

  bool HasCustomIR {};

  // POWERARM-M0-TODO(smc): SMC Idea 4 (FEX_SMCSEMANTICPATCH) site tables stay empty; the x86 rel32/mov-imm site decoders have no A64 counterpart yet (ADRP/MOVZ/B imm26 are the analogues).
  FEXCore::SMC::BranchImmSites BranchImmSites;
  FEXCore::SMC::MovImmSites MovImmSites;

  if (HasCustomIRHandlers.load(std::memory_order_relaxed)) {
    std::shared_lock lk(CustomIRMutex);
    auto Handler = CustomIRHandlers.find(GuestRIP);
    if (Handler != CustomIRHandlers.end()) {
      TotalInstructions = 1;
      TotalInstructionsLength = 1;
      Handler->second.Handler(GuestRIP, Thread->OpDispatcher.get());
      HasCustomIR = true;
    }
  }

  if (!HasCustomIR) {
    Thread->FrontendDecoder->DecodeInstructionsAtEntry(Thread, GuestRIP, MaxInst);

    auto BlockInfo = Thread->FrontendDecoder->GetDecodedBlockInfo();
    auto CodeBlocks = &BlockInfo->Blocks;

    // FEX_SMCGRANULEMIXED (64K hosts): a guest page whose host granule mtrack
    // has stopped write-protecting gets the SMCCHECKS=full treatment per block
    // instead -- every instruction validated against its decoded bytes. See
    // LinuxSyscalls/SMCHostGranule.h.
    bool CodePagesValidateOnly = false;
    if (SyscallHandler && Config.SMCChecks == FEXCore::Config::CONFIG_SMC_MTRACK) {
      for (auto CodePage : BlockInfo->CodePages) {
        if (SyscallHandler->GuestCodePageValidateOnly(CodePage)) {
          CodePagesValidateOnly = true;
          break;
        }
      }
    }

    Thread->OpDispatcher->BeginFunction(GuestRIP, CodeBlocks, BlockInfo->TotalInstructionCount);

    for (size_t j = 0; j < CodeBlocks->size(); ++j) {
      const FEXCore::A64::Decoder::DecodedBlocks& Block = CodeBlocks->at(j);
      const bool FullSMCValidation =
        Config.SMCChecks == FEXCore::Config::CONFIG_SMC_FULL || Block.ForceFullSMCDetection || CodePagesValidateOnly;

      Thread->OpDispatcher->SetNewBlockIfChanged(Block.Entry);
      Thread->OpDispatcher->StartNewBlock();

      uint64_t BlockInstructionsLength {};
      const uint64_t InstsInBlock = Block.NumInstructions;

      if (InstsInBlock == 0) {
        Thread->OpDispatcher->ExitFunction(Thread->OpDispatcher->_InlineEntrypointOffset(IR::OpSize::i64Bit, Block.Entry - GuestRIP));
      }

      for (size_t i = 0; i < InstsInBlock; ++i) {
        const auto& DecodedInfo = Block.DecodedInstructions[i];
        const uint64_t InstAddress = DecodedInfo.PC;

        // One RIP-table marker per instruction keeps signal resume instruction-granular.
        Thread->OpDispatcher->_GuestOpcode(InstAddress - GuestRIP);

        if (Block.BlockStatus != FEXCore::A64::Decoder::DecodedBlockStatus::SUCCESS) {
          // Only reachable for the entry instruction: nothing before it could have made it valid.
          Thread->OpDispatcher->NoExecInstruction(InstAddress);
          ++TotalInstructions;
          break;
        }

        if (FullSMCValidation) {
          auto InstAddressReg = Thread->OpDispatcher->_EntrypointOffset(IR::OpSize::i64Bit, InstAddress - GuestRIP);
          // Snapshot the word the decoder consumed, not a re-read of live guest memory.
          std::array<uint8_t, 0x10> CodeOriginal {};
          memcpy(CodeOriginal.data(), &DecodedInfo.Word, sizeof(DecodedInfo.Word));
          auto CodeChanged = Thread->OpDispatcher->_ValidateCode(CodeOriginal, InstAddressReg, FEXCore::A64::INSTRUCTION_SIZE);

          auto InvalidateCodeCond = Thread->OpDispatcher->CondJump(CodeChanged);

          auto CurrentBlock = Thread->OpDispatcher->GetCurrentBlock();
          auto CodeWasChangedBlock = Thread->OpDispatcher->CreateNewCodeBlockAtEnd();
          Thread->OpDispatcher->SetTrueJumpTarget(InvalidateCodeCond, CodeWasChangedBlock);

          Thread->OpDispatcher->SetCurrentCodeBlock(CodeWasChangedBlock);
          Thread->OpDispatcher->_ThreadRemoveCodeEntry();
          Thread->OpDispatcher->ExitFunction(Thread->OpDispatcher->_InlineEntrypointOffset(IR::OpSize::i64Bit, InstAddress - GuestRIP));

          auto NextOpBlock = Thread->OpDispatcher->CreateNewCodeBlockAfter(CurrentBlock);

          Thread->OpDispatcher->SetFalseJumpTarget(InvalidateCodeCond, NextOpBlock);
          Thread->OpDispatcher->SetCurrentCodeBlock(NextOpBlock);
          Thread->OpDispatcher->StartContinuationBlock();
        }

        if (!Thread->OpDispatcher->TranslateInstruction(DecodedInfo)) {
          if (TotalInstructions == 0) {
            Thread->OpDispatcher->DelayedDisownBuffer();
            return {std::nullopt, 0, 0, 0, 0};
          }
          Thread->OpDispatcher->ExitFunction(
            Thread->OpDispatcher->_InlineEntrypointOffset(IR::OpSize::i64Bit, Block.Entry + BlockInstructionsLength - GuestRIP));
          break;
        }

        BlockInstructionsLength += FEXCore::A64::INSTRUCTION_SIZE;
        TotalInstructionsLength += FEXCore::A64::INSTRUCTION_SIZE;
        ++TotalInstructions;

        if (Thread->OpDispatcher->FinishOp(InstAddress + FEXCore::A64::INSTRUCTION_SIZE, i + 1 == InstsInBlock)) {
          break;
        }
      }
    }

    Thread->OpDispatcher->Finalize();

    Thread->FrontendDecoder->DelayedDisownBuffer();
  }

  IR::IREmitter* IREmitter = Thread->OpDispatcher.get();

  auto ShouldDump = Thread->OpDispatcher->ShouldDumpIR();
  // Debug
  if (ShouldDump) {
    IRDumper(Thread, IREmitter, GuestRIP);
  }

  // Run the passmanager over the IR from the dispatcher
  Thread->PassManager->Run(IREmitter);

  // Debug
  if (ShouldDump) {
    IRDumper(Thread, IREmitter, GuestRIP);
  }

  return {
    .IRView = IREmitter->ViewIR(),
    .TotalInstructions = TotalInstructions,
    .TotalInstructionsLength = TotalInstructionsLength,
    .StartAddr = Thread->FrontendDecoder->DecodedMinAddress,
    .Length = Thread->FrontendDecoder->DecodedMaxAddress - Thread->FrontendDecoder->DecodedMinAddress,
    .NeedsAddGuestCodeRanges = !HasCustomIR,
    .BranchImmSites = std::move(BranchImmSites),
    .MovImmSites = std::move(MovImmSites),
  };
}

ContextImpl::CompileCodeResult ContextImpl::CompileCode(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP, uint64_t MaxInst) {
  if (SourcecodeResolver && Config.GDBSymbols()) {
    auto MappedSection = SyscallHandler->LookupExecutableFileSection(Thread, GuestRIP);
    if (MappedSection) {
      MappedSection->FileInfo.SourcecodeMap =
        SourcecodeResolver->GenerateMap(MappedSection->FileInfo.Filename, CodeMap::GetBaseFilename(MappedSection->FileInfo, false));
    }
  }

  // Generate IR + Meta Info
  auto [IRView, TotalInstructions, TotalInstructionsLength, StartAddr, Length, NeedsAddGuestCodeRanges, BranchImmSites, MovImmSites] =
    GenerateIR(Thread, GuestRIP, Config.GDBSymbols(), MaxInst);
  if (!IRView) {
    // OpDispatcher IR already released in this case.
    return {{}, nullptr, 0, 0, false, {}, {}};
  }

  // Attempt to get the CPU backend to compile this code
  // Re-check if another thread raced us in compiling this block.
  // We could lock CodeBufferWriteMutex earlier to prevent this from happening,
  // but this would increase lock contention. Redundant frontend runs aren't
  // as expensive and are easily reverted.
  if (MaxInst != 1) {
    if (auto Block = Thread->LookupCache->FindBlock(Thread, GuestRIP)) {
      // Raced to compile, release the OpDispatcher IR.
      Thread->OpDispatcher->DelayedDisownBuffer();
      return {.CompiledCode = {.BlockBegin = reinterpret_cast<uint8_t*>(Block), .EntryPoints = {{GuestRIP, reinterpret_cast<uint8_t*>(Block)}}},
              .DebugData = nullptr,
              .StartAddr = 0,
              .Length = 0,
              .NeedsAddGuestCodeRanges = false,
              .BranchImmSites = {},
              .MovImmSites = {}};
    }
  }

  auto DebugData = fextl::make_unique<FEXCore::Core::DebugData>();

  // POWERARM-M0-TODO(backend): the x86 trap-flag single-step check is gone; A64 EL0 has no TF equivalent (software step comes via ptrace/gdbserver).
  const bool TFSet = false;

  auto CompiledCode = Thread->CPUBackend->CompileCode(GuestRIP, Length, TotalInstructions == 1, &*IRView, DebugData.get(), TFSet);

  // Release the IR
  Thread->OpDispatcher->DelayedDisownBuffer();

  return {
    .CompiledCode = std::move(CompiledCode),
    .DebugData = std::move(DebugData),
    .StartAddr = StartAddr,
    .Length = Length,
    .NeedsAddGuestCodeRanges = NeedsAddGuestCodeRanges,
    .BranchImmSites = std::move(BranchImmSites),
    .MovImmSites = std::move(MovImmSites),
  };
}

uintptr_t ContextImpl::CompileBlock(FEXCore::Core::CpuStateFrame* Frame, uint64_t GuestRIP, uint64_t MaxInst) {
  auto Thread = Frame->Thread;
  FEXCORE_PROFILE_SCOPED("CompileBlock");
  FEXCORE_PROFILE_ACCUMULATION(Thread, AccumulatedJITTime);

  static_cast<ContextImpl*>(Thread->CTX)->SyscallHandler->PreCompile();

  // FEX_SMCLAZYINVAL drain point (a). This thread is about to run code it does
  // not already have in its L1, so settle any deferred SMC invalidations first:
  // pages dirtied since the last drain get soft-invalidated now, so the lookup
  // below either misses (and recompiles) or relinks against a validated hash,
  // instead of publishing a translation of bytes that have been overwritten.
  //
  // MUST stay above the shared CodeInvalidationMutex guard: the drain takes the
  // exclusive side of that same mutex and calls ReleaseAllPendingSharedLocks to
  // get there, which would yank a lock this function relies on for its whole
  // body (the v2 hazard, see SMCSoftInvalidate.h note (d)). Null pointer =>
  // option off => one predictable load and a not-taken branch.
  if (auto* LazyCount = SyscallHandler->LazySMCDirtyCount; LazyCount) {
    // FEX_SMCLAZYSCRUB: this thread's scrub debt (if any) is settled by the
    // unconditional drain below, so consume it here too. CompileBlock is
    // normally reached *through* the lookup slow path, which already took it;
    // this covers the direct callers (CompileRIP, HandleCallback, AOT).
    Thread->LookupCache->TakeLazySMCDrainPending();
    if (LazyCount->load(std::memory_order_acquire) != 0) {
      SyscallHandler->DrainLazySMCInvalidations(Thread);
    }
  }

  // Invalidate might take a unique lock on this, to guarantee that during invalidation no code gets compiled
  auto lk = GuardSignalDeferringSection<std::shared_lock>(CodeInvalidationMutex, Thread);

  // Is the code in the cache?
  // The backends only check L1 and L2, not L3
  if (auto HostCode = Thread->LookupCache->FindBlock(Thread, GuestRIP)) {
    return HostCode;
  }

  // SMC v3: the lookup miss may be a soft-invalidated block whose guest bytes
  // never actually changed. Revalidating by hash costs microseconds where a
  // recompile costs tens. See Interface/Core/SMCSoftInvalidate.h.
  // Runs under this function's shared CodeInvalidationMutex, which is what
  // makes it mutually exclusive with any concurrent (soft-)invalidation.
  if (Config.SMCSoftInvalidate()) {
    if (auto HostCode = TryRelinkSoftInvalidatedBlock(Thread, GuestRIP)) {
      return HostCode;
    }
  }

  // Code cache: install a stored translation of this block instead of
  // compiling it. See CodeCache::TryLoadBlock for what it checks first.
  if (CodeCache.CanLoad()) {
    if (auto Loaded = CodeCache.TryLoadBlock(Thread, GuestRIP)) {
      return RegisterCachedBlock(Thread, GuestRIP, *Loaded);
    }
  }

  // Accumulate a JIT count now, as even if another thread raced us, it should count as a compile.
  FEXCORE_PROFILE_INSTANT_INCREMENT(Thread, AccumulatedJITCount, 1);

  auto* CompileLog = GetCompileLog();
  const uint64_t CompileStartNS = CompileLog ? CompileLogState::NowNS() : 0;
  auto [CompiledCode, DebugData, StartAddr, Length, NeedsAddGuestCodeRanges, BranchImmSites, MovImmSites] =
    CompileCode(Thread, GuestRIP, MaxInst);
  auto CodePtr = CompiledCode.EntryPoints[GuestRIP];
  if (CodePtr == nullptr) {
    return 0;
  } else if (!DebugData) {
    // DebugData wasn't populated, indicating another thread raced us for compiling this block
    return reinterpret_cast<uintptr_t>(CodePtr);
  }
  if (CompileLog) {
    CompileLog->RecordCompile(GuestRIP, CompiledCode.Size, CompileLogState::NowNS() - CompileStartNS);
  }

  // The core managed to compile the code.
  if (Config.BlockJITNaming()) {
    auto FragmentBasePtr = CompiledCode.BlockBegin;

    auto GuestRIPLookup = SyscallHandler->LookupExecutableFileSection(Thread, GuestRIP);

    if (DebugData->Subblocks.size()) {
      for (auto& Subblock : DebugData->Subblocks) {
        auto BlockBasePtr = FragmentBasePtr + Subblock.HostCodeOffset;
        if (GuestRIPLookup) {
          Symbols.Register(Thread->SymbolBuffer.get(), BlockBasePtr, CompiledCode.Size, GuestRIPLookup->FileInfo.Filename,
                           GuestRIP - GuestRIPLookup->FileStartVA);
        } else {
          Symbols.Register(Thread->SymbolBuffer.get(), BlockBasePtr, GuestRIP, Subblock.HostCodeSize);
        }
      }
    } else {
      if (GuestRIPLookup) {
        Symbols.Register(Thread->SymbolBuffer.get(), FragmentBasePtr, CompiledCode.Size, GuestRIPLookup->FileInfo.Filename,
                         GuestRIP - GuestRIPLookup->FileStartVA);
      } else {
        Symbols.Register(Thread->SymbolBuffer.get(), FragmentBasePtr, GuestRIP, CompiledCode.Size);
      }
    }
  }

  if (Config.LibraryJITNaming() || Config.GDBSymbols()) {
    auto MappedSection = SyscallHandler->LookupExecutableFileSection(Thread, GuestRIP);
    if (MappedSection) {
      if (Config.LibraryJITNaming()) {
        // Register the whole block, not the entry point. CodePtr is
        // EntryPoints[GuestRIP], which sits past the JITCodeHeader, so
        // [CodePtr, CodePtr + HostCodeSize) would run off the end of the block
        // and shadow the start of the next one. [BlockBegin, BlockBegin + Size)
        // is exactly the compiled range.
        Symbols.RegisterNamedRegion(Thread->SymbolBuffer.get(), CompiledCode.BlockBegin, CompiledCode.Size,
                                    MappedSection->FileInfo.Filename);
      }

      if (Config.GDBSymbols()) {
        GDBJITRegister(MappedSection->FileInfo, MappedSection->FileStartVA, GuestRIP, (uintptr_t)CodePtr, *DebugData);
      }
    } else if (Config.LibraryJITNaming()) {
      // No file section — the code may live in a manually-loaded image in
      // anonymous memory (wine's main PE), which the frontend can still
      // identify. Naming only: GDBJIT needs a real file to read and is
      // deliberately not fed a synthesized label.
      if (const char* AnonImage = SyscallHandler->LookupAnonymousExecImageName(Thread, GuestRIP)) {
        Symbols.RegisterNamedRegion(Thread->SymbolBuffer.get(), CompiledCode.BlockBegin, CompiledCode.Size, AnonImage);
      }
    }
  }

  // Clear any relocations that might have been generated
  if (!CodeCache.IsGeneratingCache) {
    Thread->CPUBackend->ClearRelocations();
  } else {
    // Cache generation: hand them to the context-wide sink instead of leaving
    // them to pile up in this thread's backend. See CodeCache::AbsorbRelocations
    // — a runtime cache writer saves from whichever thread reaches a safe point
    // first, and per-thread relocation lists would make it save a cache missing
    // every relocation another thread produced.
    CodeCache.AbsorbRelocations(*Thread, GuestRIP);
    CodeCache.BlocksSinceSave.fetch_add(1, std::memory_order_relaxed);
  }

  fextl::vector<uint64_t> CodePages;

  if (NeedsAddGuestCodeRanges) {
    // Track in the guest to host map all entrypoints for all pages the compiled block touches, if any page didn't previously
    // contain code, inform the frontend so it can setup SMC detection.
    auto BlockInfo = Thread->FrontendDecoder->GetDecodedBlockInfo();
    CodePages.reserve(BlockInfo->CodePages.size());
    CodePages.insert(CodePages.end(), BlockInfo->CodePages.begin(), BlockInfo->CodePages.end());
    for (auto CodePage : BlockInfo->CodePages) {
      // SMC Idea 3: [StartAddr, StartAddr+Length) is the frontend's decoded
      // guest span; passing it gives the granule bitmap 64-byte precision for
      // this block instead of a whole-page mark. It MUST be recorded here,
      // before MarkGuestExecutableRange below arms the page's write protection
      // -- see the ordering argument in Interface/Core/SMCCodeGranules.h.
      const bool NewPage =
        Thread->LookupCache->AddBlockExecutableRange(Thread, BlockInfo->EntryPoints, CodePage, FEXCore::Utils::FEX_GUEST_PAGE_SIZE, StartAddr, Length);
      if (NewPage) {
        SyscallHandler->MarkGuestExecutableRange(Thread, CodePage, FEXCore::Utils::FEX_GUEST_PAGE_SIZE);
      }
      if (SMCAuditCompileFD() >= 0) {
        dprintf(SMCAuditCompileFD(), "compile rip=%lx page=%lx newpage=%d nentry=%zu\n", GuestRIP, CodePage, NewPage ? 1 : 0,
                BlockInfo->EntryPoints.size());
      }
    }
    if (SMCAuditCompileFD() >= 0 && BlockInfo->CodePages.empty()) {
      dprintf(SMCAuditCompileFD(), "compile rip=%lx NO-PAGES\n", GuestRIP);
    }
  }

  // SMC v3: record a content hash of the block's guest source bytes so a later
  // soft-invalidation of this block can be undone by revalidation instead of a
  // recompile. [StartAddr, StartAddr+Length) is the frontend's decoded span;
  // HashGuestBlock only hashes the parts of it that lie in CodePages.
  uint64_t GuestHash = 0;
  uint64_t HashedRangeLength = 0;
  if (Config.SMCSoftInvalidate() && FEXCore::SMC::IsHashableBlock(Length, CodePages.size())) {
    HashedRangeLength = Length;
    GuestHash = FEXCore::SMC::HashGuestBlock(CodePages, StartAddr, Length);
  }

  // Insert to lookup cache

  // BlockBegin is shared across all EntryPoints of a single CompiledCode
  // (each block has one begin, potentially multiple entry points).
  const uint64_t BlockBegin = reinterpret_cast<uintptr_t>(CompiledCode.BlockBegin);
  // SMC Idea 4: the block is eligible for semantic patching only if BOTH halves
  // of the metadata survived (a decoded rel32 field to match the guest write
  // against, and a constant exit window to repatch). Either table empty leaves
  // both empty, so the fault handler's "block claims the write but has no exit
  // sites" decline can only mean a genuine multiblock-folded branch.
  if (BranchImmSites.empty() || CompiledCode.ExitRIPSites.empty()) {
    BranchImmSites.clear();
    CompiledCode.ExitRIPSites.clear();
  }
  // The mov-immediate half is independent: a block can be eligible for one
  // shape and not the other. Same all-or-nothing rule per half -- a site table
  // without windows (or windows whose site indices were invalidated by an
  // overflowing site table) must not be consulted at fault time.
  if (MovImmSites.empty() || CompiledCode.MovImmWindows.empty()) {
    MovImmSites.clear();
    CompiledCode.MovImmWindows.clear();
  }

  for (auto [GuestAddr, HostAddr] : CompiledCode.EntryPoints) {
    Thread->LookupCache->AddBlockMapping(Thread, GuestAddr, BlockBegin, CodePages, HostAddr, StartAddr, HashedRangeLength, GuestHash,
                                         BranchImmSites, CompiledCode.ExitRIPSites, MovImmSites, CompiledCode.MovImmWindows);
  }

  if (CodeMapWriter) {
    auto Region = SyscallHandler->LookupExecutableFileSection(Thread, GuestRIP);
    if (Region && Region->FileStartVA != 0) {
      CodeMapWriter->AppendBlock(*Region, GuestRIP);
    }
  }

  return (uintptr_t)CodePtr;
}

// Registers a block installed by the code cache the way CompileBlock registers
// a compiled one: guest code pages (arming SMC protection on new ones), the
// soft-invalidation hash, and the lookup mapping. A cached block carries none
// of the SMC patching metadata; CodeCache refuses to load when those modes are on.
uintptr_t ContextImpl::RegisterCachedBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP, const FEXCore::Context::CodeCache::LoadedBlock& Block) {
  if (Config.BlockJITNaming()) {
    if (auto Section = SyscallHandler->LookupExecutableFileSection(Thread, GuestRIP)) {
      Symbols.Register(Thread->SymbolBuffer.get(), Block.BlockBegin, Block.Size, Section->FileInfo.Filename, GuestRIP - Section->FileStartVA);
    } else {
      Symbols.Register(Thread->SymbolBuffer.get(), Block.BlockBegin, GuestRIP, Block.Size);
    }
  }

  // The decoder's page set for a single block: the page of every instruction.
  fextl::vector<uint64_t> CodePages;
  const uint64_t LastInst = Block.StartAddr + Block.Length - FEXCore::A64::INSTRUCTION_SIZE;
  for (uint64_t Page = Block.StartAddr & FEXCore::Utils::FEX_GUEST_PAGE_MASK; Page <= LastInst; Page += FEXCore::Utils::FEX_GUEST_PAGE_SIZE) {
    CodePages.push_back(Page);
  }
  // The pages were registered (and write-protected) by TryLoadBlock before it
  // checked the guest bytes.

  uint64_t GuestHash = 0;
  uint64_t HashedRangeLength = 0;
  if (Config.SMCSoftInvalidate() && FEXCore::SMC::IsHashableBlock(Block.Length, CodePages.size())) {
    HashedRangeLength = Block.Length;
    GuestHash = FEXCore::SMC::HashGuestBlock(CodePages, Block.StartAddr, Block.Length);
  }

  Thread->LookupCache->AddBlockMapping(Thread, GuestRIP, reinterpret_cast<uintptr_t>(Block.BlockBegin), CodePages, Block.HostCode,
                                       Block.StartAddr, HashedRangeLength, GuestHash);
  return reinterpret_cast<uintptr_t>(Block.HostCode);
}

uintptr_t ContextImpl::CompileSingleStep(FEXCore::Core::CpuStateFrame* Frame, uint64_t GuestRIP) {
  FEXCORE_PROFILE_SCOPED("CompileSingleStep");
  auto Thread = Frame->Thread;

  if (SMCAuditCompileFD() >= 0) {
    dprintf(SMCAuditCompileFD(), "single-step rip=%lx tid=%d\n", GuestRIP, FHU::Syscalls::gettid());
  }

  static_cast<ContextImpl*>(Thread->CTX)->SyscallHandler->PreCompile();

  // Invalidate might take a unique lock on this, to guarantee that during invalidation no code gets compiled
  auto lk = GuardSignalDeferringSection<std::shared_lock>(CodeInvalidationMutex, Thread);

  auto [CompiledCode, DebugData, StartAddr, Length, _, __, ___] = CompileCode(Thread, GuestRIP, 1);
  auto CodePtr = CompiledCode.EntryPoints[GuestRIP];
  if (CodePtr == nullptr) {
    if (SMCAuditCompileFD() >= 0) {
      dprintf(SMCAuditCompileFD(), "single-step rip=%lx FAILED (nullptr)\n", GuestRIP);
    }
    return 0;
  }

  // Clear any relocations that might have been generated
  Thread->CPUBackend->ClearRelocations();

  return (uintptr_t)CodePtr;
}

void ContextImpl::InvalidateCodeBuffersCodeRange(uint64_t Start, uint64_t Length) {
  FEXCORE_PROFILE_SCOPED("InvalidateCodeBuffersCodeRange");

  LOGMAN_THROW_A_FMT(CodeInvalidationMutex.is_write_owned(), "CodeInvalidationMutex needs to be unique_locked here");

  // Every invalidation funnels through here, so this is the one place that
  // sees the per-page churn the cheap compile tier keys off. No-op unless
  // FEX_SMCCHEAPTIER is set.
  RecordCodeRangeInvalidation(Start, Length);
  if (auto* CompileLog = GetCompileLog()) {
    CompileLog->RecordInvalidate(Length);
  }

  std::scoped_lock lk {CodeBufferListLock};
  auto it = CodeBufferList.begin();
  while (it != CodeBufferList.end()) {
    if (auto Strong = it->lock()) {
      Strong->LookupCache->InvalidateRange(Start, Length);
      it++;
    } else {
      it = CodeBufferList.erase(it);
    }
  }
}

uintptr_t ContextImpl::TryRelinkSoftInvalidatedBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP) {
  // Called from CompileBlock, which holds CodeInvalidationMutex shared. That is
  // load-bearing: it excludes every (soft-)invalidation for the duration, so a
  // retained entry taken here cannot be concurrently purged by a munmap.
  auto Retained = Thread->LookupCache->TakeRetainedBlock(GuestRIP);
  if (!Retained) {
    return 0;
  }

  const uint64_t CurrentHash = FEXCore::SMC::HashGuestBlock(Retained->CodePages, Retained->GuestRangeStart, Retained->GuestRangeLength);
  if (CurrentHash != Retained->GuestHash) {
    // Genuinely modified: drop the metadata (the host code stays in the
    // CodeBuffer for any thread still executing it, exactly as legacy
    // invalidation leaves it) and let the caller compile a fresh block.
    //
    // The soft-invalidation path bypasses InvalidateCodeBuffersCodeRange, so
    // the cheap-tier churn counter (FEX_SMCCHEAPTIER) would never see these
    // pages. Count the churn here instead — a hash mismatch is a stronger
    // "this page's code really changed" signal than a raw invalidation, and it
    // deliberately excludes relinks (false sharing / identical rewrites), which
    // are not churn worth demoting a page's compile tier over.
    RecordCodeRangeInvalidation(Retained->GuestRangeStart, Retained->GuestRangeLength);
    if (SMCAuditCompileFD() >= 0) {
      dprintf(SMCAuditCompileFD(), "relink-miss rip=%lx\n", GuestRIP);
    }
    return 0;
  }

  // FEX_SMCGRANULEMIXED: a retained block was compiled without per-instruction
  // guards, and the re-arm below is skipped for a demoted granule, so a
  // retained block on a demoted page has nothing keeping it sound. Refuse the
  // relink; the fresh compile that follows reads the demoted bit and guards.
  if (SyscallHandler) {
    for (auto CodePage : Retained->CodePages) {
      if (SyscallHandler->GuestCodePageValidateOnly(CodePage)) {
        if (SMCAuditCompileFD() >= 0) {
          dprintf(SMCAuditCompileFD(), "relink-refused-demoted rip=%lx page=%lx\n", GuestRIP, CodePage);
        }
        return 0;
      }
    }
  }

  // Unchanged: re-publish. Registering the code pages again re-arms mtrack's
  // write protection through the same path a fresh compile uses, so the page
  // becomes protected exactly when live code reappears on it.
  fextl::set<uint64_t> Entrypoints {GuestRIP};
  for (auto CodePage : Retained->CodePages) {
    // SMC Idea 3: re-set the granule bits the soft-invalidation cleared. The
    // retained entry always carries a non-zero extent (SoftEraseBlock refuses
    // to retain a block without one), so this is granule-precise, and it lands
    // before MarkGuestExecutableRange re-arms the protection.
    const bool NewPage = Thread->LookupCache->AddBlockExecutableRange(
      Thread, Entrypoints, CodePage, FEXCore::Utils::FEX_GUEST_PAGE_SIZE, Retained->GuestRangeStart, Retained->GuestRangeLength);
    if (NewPage) {
      SyscallHandler->MarkGuestExecutableRange(Thread, CodePage, FEXCore::Utils::FEX_GUEST_PAGE_SIZE);
    }
  }

  // SMC soundness -- VERIFY-AFTER-ARM.  The CurrentHash compare at the top of
  // this function ran with the granule still WRITABLE: on the strict/mtrack
  // path the fault handler unprotects the granule to R+W and returns, and the
  // faulting guest store retires only after sigreturn.  A store the handler had
  // already admitted but that had not yet retired was therefore invisible to
  // that first hash -- it matched the stale bytes, and we arrived here about to
  // republish a translation of code that is about to change.  This is the 64K
  // concurrent-self-modification race (CoreCLR tiering / JVM re-JIT patch a call
  // site on a background thread while a mutator relinks the same granule).
  //
  // MarkGuestExecutableRange above re-armed every code page with an
  // mprotect(PROT_READ) -- a full barrier with a TLB shootdown -- so the racing
  // store now either landed BEFORE the arm (and a re-hash sees it) or lands
  // AFTER it (and faults, soft-invalidating this block).  Re-hash under the arm;
  // a mismatch means the bytes moved out from under this translation, so drop it
  // and let the caller compile fresh, exactly as the first-hash miss does.  The
  // re-hash costs one XXH3 over the retained span, only on the relink path.
  // See Interface/Core/SMCSoftInvalidate.h, "VERIFY-AFTER-ARM".
  {
    const uint64_t ArmedHash =
      FEXCore::SMC::HashGuestBlock(Retained->CodePages, Retained->GuestRangeStart, Retained->GuestRangeLength);
    if (ArmedHash != Retained->GuestHash) {
      RecordCodeRangeInvalidation(Retained->GuestRangeStart, Retained->GuestRangeLength);
      if (SMCAuditCompileFD() >= 0) {
        dprintf(SMCAuditCompileFD(), "relink-miss-postarm rip=%lx\n", GuestRIP);
      }
      return 0;
    }
  }

  // SMC Idea 4: carry the semantic-patch metadata across the relink. A relink
  // only happens when the guest bytes hashed identical, and the host code is
  // the same code that was compiled from them -- so every recorded guest field
  // and every recorded host window is still exactly as valid as it was before
  // the soft-invalidation. Dropping it here would silently make every
  // soft-invalidated block ineligible for patching from then on.
  Thread->LookupCache->AddBlockMapping(Thread, GuestRIP, Retained->BlockBegin, Retained->CodePages,
                                       reinterpret_cast<void*>(Retained->HostCode), Retained->GuestRangeStart, Retained->GuestRangeLength,
                                       Retained->GuestHash, Retained->BranchImmSites, Retained->ExitRIPSites, Retained->MovImmSites,
                                       Retained->MovImmWindows);

  if (SMCAuditCompileFD() >= 0) {
    dprintf(SMCAuditCompileFD(), "relink rip=%lx host=%lx pages=%zu\n", GuestRIP, Retained->HostCode, Retained->CodePages.size());
  }

  return Retained->HostCode;
}

void ContextImpl::SoftInvalidateCodeBuffersCodeRange(uint64_t Start, uint64_t Length) {
  FEXCORE_PROFILE_SCOPED("SoftInvalidateCodeBuffersCodeRange");

  LOGMAN_THROW_A_FMT(CodeInvalidationMutex.is_write_owned(), "CodeInvalidationMutex needs to be unique_locked here");
  std::scoped_lock lk {CodeBufferListLock};
  auto it = CodeBufferList.begin();
  while (it != CodeBufferList.end()) {
    if (auto Strong = it->lock()) {
      Strong->LookupCache->SoftInvalidateRange(Start, Length);
      it++;
    } else {
      it = CodeBufferList.erase(it);
    }
  }
}

void ContextImpl::InvalidateThreadCachedCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) {
  LOGMAN_THROW_A_FMT(CodeInvalidationMutex.is_write_owned(), "CodeInvalidationMutex needs to be unique_locked here");

  // Ensures now-modified mappings aren't cached as being in their previous non-executable state.
  // Accessing FrontendDecoder is safe as the thread's code invalidation mutex must be locked here.
  Thread->FrontendDecoder->ResetExecutableRangeCache();

  if (Thread->LookupCache->InvalidateCacheRange(Start, Length)) {
    FEXCORE_PROFILE_SCOPED("InvalidateCallRet");

    // This may cause access violations in the thread on Windows as zeroing is not atomic, this is handled by the frontend
    //
    // One madvise per thread per invalidation, issued under the exclusive
    // CodeInvalidationMutex. The CallRet stack is only ever written when the
    // shadow return stack is armed (FEX_SHADOWRETSTACK); with it off the
    // stack is empty and the syscall is pure critical-section time, so skip it.
    if (Config.ShadowRetStack()) {
      Allocator::VirtualDontNeed(Thread->CallRetStackBase, FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE);
    }
  }
}

void ContextImpl::ScrubThreadLookupCacheForLazySMC(FEXCore::Core::InternalThreadState* Thread) {
  // FEX_SMCLAZYSCRUB. Signal-handler context, faulting thread, own cache.
  // Deliberately does NOT touch L2/L3, CachedCodePages, the frontend's
  // executable-range cache, or the CallRet stack: all of those need a lock this
  // handler must not take, and none of them is reachable without first going
  // through the lookup slow path, which drains.
  //
  // The CallRet (shadow-return) stack is left alone even when the PPC64LE
  // FEX_SHADOWRETSTACK feature is enabled. That is sound because this scrub is
  // only part of the FEX_SMCLAZYINVAL soundness machinery, and the shadow
  // return stack is force-disabled under FEX_SMCLAZYINVAL unless
  // FEX_SMCLAZYLINK is also set (see PPC64JITCore's ShadowRetStack interlock).
  // Under LAZYLINK the real drain — DrainLazySMCInvalidations ->
  // InvalidateThreadCachedCodeRange — zeroes the CallRet stack, and it is
  // reached via the InterruptFaultPage poke at every block entry, including
  // the return-block entry a shadow RET fast path lands on. So no stale host
  // trampoline outlives the scrub's drain window.
  Thread->LookupCache->ScrubForLazySMC();
}

void ContextImpl::ArmLazySMCDrainPending(FEXCore::Core::InternalThreadState* Thread) {
  // FEX_SMCLAZYCROSSPOKE. Unlike ScrubThreadLookupCacheForLazySMC this may be
  // called for a thread other than the caller, so it must not touch anything
  // that thread owns exclusively — only the drain-pending flag, which is a
  // std::atomic<bool> written relaxed.
  Thread->LookupCache->ArmLazySMCDrainPending();
}

void ContextImpl::SettleLazySMCDrainIfPending(FEXCore::Core::InternalThreadState* Thread) {
  // FEX_SMCLAZYLINK. Same consume-then-drain sequence as the copy in
  // PPC64JITCore::ExitFunctionLink, reachable from the frontend's fault-page
  // SIGSEGV branch — the one trap a linked block chain cannot skip. Runs at a
  // block-entry boundary (the poke is the block's first instruction), so guest
  // state is consistent and the drain's lock acquisition is as legal here as
  // it is on the lookup slow path.
  if (Thread->LookupCache->TakeLazySMCDrainPending()) {
    if (auto* LazyCount = SyscallHandler->LazySMCDirtyCount; LazyCount && LazyCount->load(std::memory_order_acquire) != 0) {
      SyscallHandler->DrainLazySMCInvalidations(Thread);
    }
  }
}

void ContextImpl::ThreadRemoveCodeEntryFromJit(FEXCore::Core::CpuStateFrame* Frame, uint64_t GuestRIP) {
  static_cast<ContextImpl*>(Frame->Thread->CTX)->SyscallHandler->InvalidateGuestCodeRange(Frame->Thread, GuestRIP, 1);
}

std::optional<CustomIRResult>
ContextImpl::AddCustomIREntrypoint(uintptr_t Entrypoint, CustomIREntrypointHandler Handler, void* Creator, void* Data) {

  std::unique_lock lk(CustomIRMutex);

  auto InsertedIterator = CustomIRHandlers.emplace(Entrypoint, CustomIRHandlerEntry {Handler, Creator, Data});
  HasCustomIRHandlers = true;

  if (!InsertedIterator.second) {
    const auto& [fn, Creator, Data] = InsertedIterator.first->second;
    return CustomIRResult(Creator, Data);
  }

  return std::nullopt;
}

void ContextImpl::AddThunkTrampolineIRHandler(uintptr_t Entrypoint, uintptr_t GuestThunkEntrypoint) {
  LOGMAN_THROW_A_FMT(Entrypoint, "Tried to link null pointer address to guest function");
  LOGMAN_THROW_A_FMT(GuestThunkEntrypoint, "Tried to link address to null pointer guest function");
  LogMan::Msg::DFmt("Thunks: Adding guest trampoline from address {:#x} to guest function {:#x}", Entrypoint, GuestThunkEntrypoint);

  auto Result = AddCustomIREntrypoint(
    Entrypoint,
    [GuestThunkEntrypoint](uintptr_t Entrypoint, FEXCore::IR::IREmitter* emit) {
      auto IRHeader = emit->_IRHeader(emit->Invalid(), Entrypoint, 0, 0, 0, 0);
      auto Block = emit->CreateCodeNode(true, 0);
      IRHeader.first->Blocks = emit->WrapNode(Block);
      emit->SetCurrentCodeBlock(Block);

      // POWERARM-M0-TODO(thunks): the x86-64 trampoline passed its own address in R11; X16 (IP0) is the AAPCS64 veneer register but the guest-thunk ABI is an M5 decision.
      emit->_StoreContextGPR(IR::OpSize::i64Bit, emit->Constant(Entrypoint), Core::CPUState::GPROffset(16));
      emit->_ExitFunction(IR::OpSize::i64Bit, emit->Constant(GuestThunkEntrypoint), IR::BranchHint::None, emit->Invalid(), emit->Invalid());
    },
    ThunkHandler, (void*)GuestThunkEntrypoint);

  if (Result.has_value()) {
    if (Result->Creator != ThunkHandler) {
      ERROR_AND_DIE_FMT("Input address for AddThunkTrampoline is already linked by another module");
    }
    if (Result->Data != (void*)GuestThunkEntrypoint) {
      // NOTE: This may happen in Vulkan thunks if the Vulkan driver resolves two different symbols
      //       to the same function (e.g. vkGetPhysicalDeviceFeatures2/vkGetPhysicalDeviceFeatures2KHR)
      LogMan::Msg::EFmt("Input address for AddThunkTrampoline is already linked elsewhere");
    }
  }
}

void ContextImpl::AddForceTSOInformation(const IntervalList<uint64_t>& ValidRanges, fextl::set<uint64_t>&& Instructions) {
  std::unique_lock lk(ForceTSOMutex);
  ForceTSOValidRanges.Insert(ValidRanges);
  ForceTSOInstructions.merge(std::move(Instructions));
}

void ContextImpl::RemoveForceTSOInformation(uint64_t Address, uint64_t Size) {
  std::unique_lock lk(ForceTSOMutex);
  ForceTSOValidRanges.Remove({Address, Address + Size});
  ForceTSOInstructions.erase(ForceTSOInstructions.lower_bound(Address), ForceTSOInstructions.upper_bound(Address + Size));
}

void ContextImpl::MarkMonoBackpatcherBlock(uint64_t BlockEntry) {
  MonoBackpatcherBlock.store(BlockEntry, std::memory_order_relaxed);
}

void ContextImpl::RemoveCustomIREntrypoint(FEXCore::Core::InternalThreadState* Thread, uintptr_t Entrypoint) {

  {
    std::scoped_lock lk(CustomIRMutex);
    CustomIRHandlers.erase(Entrypoint);
    HasCustomIRHandlers = !CustomIRHandlers.empty();
  }
  // Outside CustomIRMutex: GenerateIR takes CustomIRMutex shared while
  // holding CodeInvalidationMutex shared, so invalidating (exclusive
  // CodeInvalidationMutex) while still holding CustomIRMutex exclusive
  // inverted that order against every concurrent compile.
  SyscallHandler->InvalidateGuestCodeRange(Thread, Entrypoint, 1);
}

void ContextImpl::MonoBackpatcherWrite(FEXCore::Core::CpuStateFrame* Frame, uint8_t Size, uint64_t Address, uint64_t Value) {
  auto Thread = Frame->Thread;
  auto CTX = static_cast<ContextImpl*>(Thread->CTX);
  {
    // Shared, not exclusive: the store is a single aligned host store and the
    // invalidation below retires whatever any concurrent compile read. The
    // exclusive form (the guard's default template argument) stopped every
    // compile and L1-miss link in the process for one store, and a store
    // that faults into HandleSegfault can only have its lock released by
    // ReleaseAllPendingSharedLocks if it is a tracked shared hold.
    auto lk = GuardSignalDeferringSection<std::shared_lock>(CTX->CodeInvalidationMutex, Thread);

    if (Size == 8) {
      *reinterpret_cast<uint64_t*>(Address) = Value;
    } else if (Size == 4) {
      *reinterpret_cast<uint32_t*>(Address) = Value;
    } else {
      ERROR_AND_DIE_FMT("Unexpected write size for backpatcher: {}", Size);
    }
  }

  CTX->SyscallHandler->InvalidateGuestCodeRange(Thread, Address, Size);
}

void ContextImpl::ConfigureAOTGen(FEXCore::Core::InternalThreadState* Thread, fextl::set<uint64_t>* ExternalBranches, uint64_t SectionMaxAddress) {
  Thread->FrontendDecoder->SetExternalBranches(ExternalBranches);
}
} // namespace FEXCore::Context
