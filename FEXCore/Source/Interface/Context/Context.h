// SPDX-License-Identifier: MIT
#pragma once

#include "Common/JitSymbols.h"
#include "Interface/Core/CPUBackend.h"
#ifdef ARCHITECTURE_ppc64le
#include "Interface/Core/JIT/PPC64LE/PPC64Dispatcher.h"
#include "Interface/Core/TranslateAhead.h"
#endif
#include <Interface/IR/IntrusiveIRList.h>
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/CompilerDefs.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/fextl/map.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/set.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_map.h>
#include <FEXCore/fextl/vector.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace FEXCore {
class SignalDelegator;
class ThunkHandler;
struct LookupCacheWriteLockToken;

namespace Core {
  struct DebugData;
  struct InternalThreadState;
} // namespace Core

namespace CPU {
#ifndef ARCHITECTURE_ppc64le
  class Dispatcher;
#endif
} // namespace CPU

namespace HLE {
  class SourcecodeResolver;
  class SyscallHandler;
} // namespace HLE
} // namespace FEXCore

namespace FEXCore::Context {
struct FEX_PACKED ExitFunctionLinkData {
  uint64_t HostCode;
  uint64_t GuestRIP;
  int64_t CallerOffset;
};

struct CustomIRResult {
  void* Creator;
  void* Data;

  CustomIRResult(void* Creator, void* Data)
    : Creator(Creator)
    , Data(Data) {}
};

using BlockDelinkerFunc = void (*)(FEXCore::Context::ExitFunctionLinkData* Record);
constexpr uint32_t TSC_SCALE_MAXIMUM = 1'000'000'000; ///< 1Ghz

// EFFECTIVE hardware-TSO state for this process, as distinct from the FEX_HWTSO
// option the user asked for. The two differ, and the difference is what a guest
// executes:
//
//   FEX_HWTSO=1 on POWER8 with a hash MMU -> the startup PROT_SAO probe
//   succeeds, SetHardwareTSOSupport(true) is called, and the JIT emits NO TSO
//   barriers at all because the page attribute carries the ordering.
//
//   FEX_HWTSO=1 on POWER9/POWER10 radix, or an LPAR built without
//   CONFIG_PPC_PROT_SAO_LPAR -> the same probe fails, SetHardwareTSOSupport is
//   never called with true, and the JIT emits full barriers.
//
// Same requested option, opposite code. Anything that has to distinguish the
// two -- the code-cache config id above all, see ComputeCodeCacheConfigId --
// must read this and not FEXCore::Config::Get_HWTSO().
//
// It is a process-wide variable rather than ContextImpl state because
// ComputeCodeCacheConfigId is a free function that names no context, and
// because the id it derives is a property of the process's on-disk cache
// namespace rather than of any one context. The only writer is
// ContextImpl::SetHardwareTSOSupport, whose callers are FEX::Kernel::Init's TSO
// setup (once, at startup, before any thread exists) and
// FEX::HLE::SyscallHandler::RevokeHardwareTSO (at most once, under the
// exclusive CodeInvalidationMutex).
//
// Revoked is deliberately a third state and not just Off: a process that gave
// hardware TSO up mid-run has a code buffer holding BOTH barrier-free blocks
// compiled before the downgrade and barrier-carrying ones compiled after, which
// is a thing neither Off nor Active describes.
enum class HardwareTSOState : uint8_t {
  Off = 0,     ///< Never enabled, or the startup probe refused. JIT emits barriers.
  Active = 1,  ///< Enabled and still in force. JIT emits none; PROT_SAO carries ordering.
  Revoked = 2, ///< Was Active, given up mid-run after a PROT_SAO refusal on ordinary memory.
};
inline std::atomic<HardwareTSOState> EffectiveHardwareTSO {HardwareTSOState::Off};

class CodeCache : public AbstractCodeCache {
public:
  CodeCache(ContextImpl&);
  ~CodeCache();

  ContextImpl& CTX;
  bool IsGeneratingCache = false;

  FEX_CONFIG_OPT(EnableCodeCaching, ENABLECODECACHINGWIP);

  // Held across a SaveNewBlocks pass in place of the shared CodeInvalidationMutex.
  // The save does unbounded, cross-process I/O (a blocking flock on the cache .lock,
  // and compaction that reads/hashes/writes hundreds of MiB); holding the shared
  // invalidation lock across that starved an in-process SMC invalidation past the
  // 4 s stall detector (issue #1). This lock gives the same fork guarantee -- fork's
  // LockBeforeFork takes it exclusively, so it never snapshots a thread holding an
  // internal cache lock -- without serializing invalidation behind cache I/O.
  FEXCore::ForkableUniqueMutex SaveIOLock;

  FEXCore::ForkableUniqueMutex& GetSaveIOLock() final { return SaveIOLock; }

  uint64_t ComputeCodeMapId(std::string_view Filename, int FD) override;
  bool SaveData(Core::InternalThreadState&, int TargetFD, const ExecutableFileSectionInfo&, uint64_t SerializedBaseAddress,
                std::span<const GuestAddressRange> GuestRanges = {}) override;
  size_t SaveNewBlocks(Core::InternalThreadState&, std::span<const CodeCacheSaveTarget> Targets, CodeCacheSaveKind Kind) override;
  bool CompactAllSegments(const fextl::string& BasePath, uint64_t FileId) override;

  void InitiateCacheGeneration() override {
    IsGeneratingCache = true;
  }

  // A block installed from an on-disk cache, laid out exactly like a fresh
  // compile of GuestRIP.
  struct LoadedBlock {
    uint8_t* BlockBegin;
    uint8_t* HostCode;
    size_t Size;
    // Guest span the block was decoded from ([StartAddr, StartAddr + Length)).
    uint64_t StartAddr;
    uint64_t Length;
  };

  /**
   * Looks GuestRIP up in the on-disk cache of the file it lies in and, if a
   * block is present, still matches the guest's instruction bytes, and passes
   * its integrity hash, copies it into the code buffer and relocates it.
   * The caller registers it exactly as it would a compiled block.
   *
   * Must be called with CodeInvalidationMutex held shared (CompileBlock).
   */
  std::optional<LoadedBlock> TryLoadBlock(Core::InternalThreadState* Thread, uint64_t GuestRIP);

  // True when TryLoadBlock can ever succeed in this process.
  bool CanLoad() const {
    return LoadEnabled;
  }

  /**
   * Moves the relocations the given thread's backend has accumulated into the
   * context-wide sink, and records GuestRIP as compiled by this process.
   *
   * Relocations are recorded per thread backend but describe offsets into the
   * shared code buffer, and a save runs on whichever thread reaches a safe point.
   * GuestRIP relocations are kept absolute; each save rebases its own copy.
   */
  void AbsorbRelocations(Core::InternalThreadState& Thread, uint64_t GuestRIP);

  // Drops the sink and the compiled-block list. Must be called whenever the
  // code buffer rotates: every offset in them refers to a buffer that is gone.
  void ResetRelocations();

  bool WantsSave(bool IgnoreInterval) override;
  void NotifyCachesSaved() override;
  void ResetAfterFork() override;
  void DumpStats() override;

  // Counters for DumpStats. Relaxed atomics: diagnostic only.
  struct {
    std::atomic<uint64_t> Loaded, NotInIndex, NoFile, BadEntry, GuestMismatch, NotExecutable, RelocFailed, SavedBlocks, SavedSegments,
      Compactions, SaveNS, LoadNS;
  } Stats {};

  // Number of blocks compiled since the last save pass; also drives WantsSave.
  std::atomic<uint64_t> BlocksSinceSave {0};
  // This process has run a periodic save pass: it is long-running, not one of
  // a build's short processes (see the per-file minimum in SaveNewBlocks).
  std::atomic<bool> RanPeriodicPass {false};

  /**
   * Applies a set of relocations to the given code.
   *
   * @param GuestEntry Guest base added to RIP-relative data
   * @param ForStorage True when serializing (deterministic output, links undone);
   *                   false when installing (dynamic symbols resolved)
   */
  [[nodiscard]]
  bool ApplyCodeRelocations(uint64_t GuestEntry, std::span<std::byte> Code, std::span<const CPU::Relocation> Relocations, bool ForStorage);
  [[nodiscard]]
  bool ApplyCodeRelocationsSplit(uint64_t GuestEntry, std::span<std::byte> HotCode, std::span<std::byte> ColdCode,
                                 std::span<const CPU::Relocation> Relocations);

  struct CacheSegment;
  struct FileCache;

private:
  FileCache* GetFileCache(const ExecutableFileInfo& FileInfo);

  bool LoadEnabled = false;

  std::mutex RelocationSinkMutex;
  // GuestRIP entries hold absolute guest addresses; Header.Offset is relative to
  // the code buffer base.
  fextl::vector<CPU::Relocation> RelocationSink;
  // Every block this process compiled (not loaded) since the buffer was
  // created or the process forked, and that no save pass has consumed yet,
  // with the range of RelocationSink its compile appended. Oldest first.
  struct CompiledRecord {
    uint64_t GuestRIP;
    uint64_t RelocBegin;
    uint64_t RelocEnd;
  };
  fextl::vector<CompiledRecord> CompiledBlocks;
  // Bumped whenever the sink and CompiledBlocks are dropped wholesale, so a
  // save pass does not trim records that arrived after its snapshot.
  uint64_t SinkGeneration = 0;


  std::shared_mutex RegistryMutex;
  fextl::map<uint64_t, fextl::unique_ptr<FileCache>> Registry;

  std::atomic<uint64_t> LastSaveTimeSeconds {0};
};

class ContextImpl final : public FEXCore::Context::Context, public CPU::CodeBufferManager {
public:
  // Context base class implementation.
  bool InitCore() override;

  void ExecuteThread(FEXCore::Core::InternalThreadState* Thread) override;

  bool CheckIfBlockIsCacheable(FEXCore::Core::InternalThreadState&, uint64_t GuestRIP, uint64_t MaxInst) override;
  uintptr_t RegisterCachedBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP, const CodeCache::LoadedBlock& Block);
  void CompileRIP(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP) override;
  void CompileRIPCount(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP, uint64_t MaxInst) override;

  void HandleCallback(FEXCore::Core::InternalThreadState* Thread, uint64_t RIP) override;

  bool IsAddressInCurrentBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC, uint64_t Address, uint64_t Size) override;
  bool IsCurrentBlockSingleInst(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC) override;
  uint64_t GetGuestBlockEntry(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC) override;

  uint64_t RestoreRIPFromHostPC(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC) override;

  /**
   * @brief Used to create FEX thread objects in preparation for creating a true OS thread. Does set a TID or PID.
   *
   * @param InitialRIP The starting RIP of this thread
   * @param StackPointer The starting RSP of this thread
   * @param NewThreadState The initial thread state to setup for our state, if inheriting.
   *
   * @return The InternalThreadState object that tracks all of the emulated thread's state
   *
   * Usecases:
   *  Parent thread Creation:
   *    - Thread = CreateThread(InitialRIP, InitialStack, nullptr, 0);
   *    - CTX->ExecuteThread(Thread);
   *  OS thread Creation:
   *    - Thread = CreateThread(0, 0, NewState, PPID);
   *    - Thread->ExecutionThread = FEXCore::Threads::Thread::Create(ThreadHandler, Arg);
   *    - ThreadHandler calls `CTX->ExecuteThread(Thread)`
   *  OS fork (New thread created with a clone of thread state):
   *    - clone{2, 3}
   *    - Thread = CreateThread(0, 0, CopyOfThreadState, PPID);
   *    - ExecuteThread(Thread); // Starts executing without creating another host thread
   *  Thunk callback executing guest code from native host thread
   *    - Thread = CreateThread(0, 0, NewState, PPID);
   *    - HandleCallback(Thread, RIP);
   */

  FEXCore::Core::InternalThreadState* CreateThread(uint64_t InitialRIP, uint64_t StackPointer, const FEXCore::Core::CPUState* NewThreadState) override;

  /**
   * @brief Destroys this FEX thread object and stops tracking it internally
   *
   * @param Thread The internal FEX thread state object
   */
  void DestroyThread(FEXCore::Core::InternalThreadState* Thread) override;

#ifndef _WIN32
  void LockBeforeFork(FEXCore::Core::InternalThreadState* Thread) override;
  void UnlockAfterFork(FEXCore::Core::InternalThreadState* Thread, bool Child) override;
#endif
  void SetSignalDelegator(FEXCore::SignalDelegator* SignalDelegation) override;
  void SetSyscallHandler(FEXCore::HLE::SyscallHandler* Handler) override;
  void SetThunkHandler(FEXCore::ThunkHandler* Handler) override;

  CodeCache& GetCodeCache() override {
    return CodeCache;
  }

  void SetCodeMapWriter(fextl::unique_ptr<CodeMapWriter> Writer) override {
    CodeMapWriter = std::move(Writer);
  }

  void FlushAndCloseCodeMap() override {
    if (CodeMapWriter) {
      CodeMapWriter.reset();
    }
  }

  void OnCodeBufferAllocated(const std::shared_ptr<CPU::CodeBuffer>&) override;
  void ClearCodeCache(FEXCore::Core::InternalThreadState* Thread, bool NewCodeBuffer = true) override;
  void InvalidateCodeBuffersCodeRange(uint64_t Start, uint64_t Length) override;
  void SoftInvalidateCodeBuffersCodeRange(uint64_t Start, uint64_t Length) override;
  void InvalidateThreadCachedCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override;
  // FEX_SMCLAZYSCRUB; see the declaration in FEXCore/Core/Context.h.
  void ScrubThreadLookupCacheForLazySMC(FEXCore::Core::InternalThreadState* Thread) override;
  // FEX_SMCLAZYCROSSPOKE; see the declaration in FEXCore/Core/Context.h.
  void ArmLazySMCDrainPending(FEXCore::Core::InternalThreadState* Thread) override;
  void SettleLazySMCDrainIfPending(FEXCore::Core::InternalThreadState* Thread) override;

  // SMC v3: attempts to revalidate and re-publish a soft-invalidated block for
  // this guest RIP. Returns its host code pointer on success, 0 if there is no
  // retained block or its guest bytes changed. Must be called with
  // CodeInvalidationMutex held (shared is enough; CompileBlock's lock).
  uintptr_t TryRelinkSoftInvalidatedBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP);
  FEXCore::Utils::WritePriorityMutex::Mutex& GetCodeInvalidationMutex() override {
    return CodeInvalidationMutex;
  }

  void ConfigureAOTGen(FEXCore::Core::InternalThreadState* Thread, fextl::set<uint64_t>* ExternalBranches, uint64_t SectionMaxAddress) override;

  bool IsAddressInCodeBuffer(FEXCore::Core::InternalThreadState* Thread, uintptr_t Address) const override;
  FEXCore::Context::JITAuxAllocation AllocateJITAuxMemory(FEXCore::Core::InternalThreadState* Thread, size_t Bytes, size_t Alignment,
                                                          uint64_t NearHostPC, uint64_t MaxDelta) override;
  uint64_t GetJITCodeBufferGeneration() const override;
  bool GuestRangeOverlapsCompiledCode(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override;
  bool TrySemanticPatchCodeRange(uint64_t Start, uint64_t Length, const void* NewBytes, const char** Reason) override;

  ///// Cheap compile tier for churn arenas (FEX_SMCCHEAPTIER) /////
  //
  // A runtime code arena (CP2077's scripting VM, mono, CoreCLR) overwrites the
  // same guest pages over and over -- the CP2077 SMC audit histogram showed
  // ~90 invalidations per page across thousands of pages.  Almost all of the
  // compile work spent on such a page is thrown away before it executes enough
  // to pay for itself, and a big multiblock compile is the most expensive kind
  // of work to throw away.  So: count invalidations per guest page, and once a
  // page is clearly churning, compile its blocks disposably -- a small
  // instruction cap and no multiblock formation.  Correctness is unaffected;
  // this only changes how much code a compilation is allowed to cover.
  //
  // The counters are a fixed-size direct-mapped table rather than a growable
  // map: it must be safe to bump from the invalidation path (which runs under
  // the code-invalidation write lock, but also from mmap/munmap/mprotect and
  // the SIGSEGV handler) with no allocation and no lock, and the arenas that
  // motivate this span thousands of pages, so an unbounded map would be pure
  // growth.  Two pages colliding on a slot simply steal it from each other and
  // lose their counts, which costs a heuristic miss and nothing else.
  static constexpr size_t SMCPageCounterSlots = 4096; // power of two
  struct SMCPageCounter {
    std::atomic<uint64_t> Page {0};
    std::atomic<uint32_t> Count {0};
  };
  std::array<SMCPageCounter, SMCPageCounterSlots> SMCPageCounters {};

  void RecordCodeRangeInvalidation(uint64_t Start, uint64_t Length);
  bool ShouldUseCheapTier(uint64_t GuestRIP);

  // returns false if a handler was already registered
  std::optional<CustomIRResult>
  AddCustomIREntrypoint(uintptr_t Entrypoint, CustomIREntrypointHandler Handler, void* Creator = nullptr, void* Data = nullptr);

  void AddThunkTrampolineIRHandler(uintptr_t Entrypoint, uintptr_t GuestThunkEntrypoint) override;

  void AddForceTSOInformation(const IntervalList<uint64_t>& ValidRanges, fextl::set<uint64_t>&& Instructions) override;

  void RemoveForceTSOInformation(uint64_t Address, uint64_t Size) override;

  // POWERARM-M0-TODO(other): MonoHacks are x86 Unity/Mono JIT workarounds; the A64 frontend has no consumer. Remove together with the Linux-layer Mono detection.
  void MarkMonoDetected() override {
    MonoDetected = true;
  }

  void MarkMonoBackpatcherBlock(uint64_t BlockEntry) override;

public:
  struct {
    uint64_t VirtualMemSize {1ULL << 36};
    uint64_t TSCScale = 0;

    // Used if the JIT needs to have its interrupt fault code emitted.
    bool NeedsPendingInterruptFaultCheck {false};

    FEX_CONFIG_OPT(Multiblock, MULTIBLOCK);
    FEX_CONFIG_OPT(SingleStepConfig, SINGLESTEP);
    FEX_CONFIG_OPT(GdbServer, GDBSERVER);
    FEX_CONFIG_OPT(TSOEnabled, TSOENABLED);
    FEX_CONFIG_OPT(LockOnlyTSO, LOCKONLYTSO);
    FEX_CONFIG_OPT(NonTSORBP, NONTSORBP);
    FEX_CONFIG_OPT(VectorTSOEnabled, VECTORTSOENABLED);
    FEX_CONFIG_OPT(MemcpySetTSOEnabled, MEMCPYSETTSOENABLED);
    FEX_CONFIG_OPT(SMCChecks, SMCCHECKS);
    FEX_CONFIG_OPT(SMCCheapTier, SMCCHEAPTIER);
    FEX_CONFIG_OPT(SMCCheapTierThreshold, SMCCHEAPTIERTHRESHOLD);
    FEX_CONFIG_OPT(SMCCheapTierMaxInst, SMCCHEAPTIERMAXINST);
    FEX_CONFIG_OPT(TranslateAhead, TRANSLATEAHEAD);
    FEX_CONFIG_OPT(TranslateAheadDepth, TRANSLATEAHEADDEPTH);
    FEX_CONFIG_OPT(SMCSoftInvalidate, SMCSOFTINVALIDATE);
    FEX_CONFIG_OPT(SMCSemanticPatch, SMCSEMANTICPATCH);
    // SMC Idea 3 uses these two only to decide whether to build the code-granule
    // bitmap. Their policy still lives entirely in the Linux frontend
    // (LinuxSyscalls/Syscalls.h); this is a read of the same options, not a new
    // flag. See Interface/Core/SMCCodeGranules.h.
    FEX_CONFIG_OPT(SMCStoreEmulation, SMCSTOREEMULATION);
    FEX_CONFIG_OPT(SMCStoreBackpatch, SMCSTOREBACKPATCH);
    FEX_CONFIG_OPT(MaxInstPerBlock, MAXINST);
    FEX_CONFIG_OPT(RootFSPath, ROOTFS);
    FEX_CONFIG_OPT(GlobalJITNaming, GLOBALJITNAMING);
    FEX_CONFIG_OPT(LibraryJITNaming, LIBRARYJITNAMING);
    FEX_CONFIG_OPT(BlockJITNaming, BLOCKJITNAMING);
    FEX_CONFIG_OPT(GDBSymbols, GDBSYMBOLS);
    FEX_CONFIG_OPT(JITOpSizeProfile, JITOPSIZEPROFILE);
    FEX_CONFIG_OPT(DisableTelemetry, DISABLETELEMETRY);
    FEX_CONFIG_OPT(DisableVixlIndirectCalls, DISABLE_VIXL_INDIRECT_RUNTIME_CALLS);
    FEX_CONFIG_OPT(SmallTSCScale, SMALLTSCSCALE);
    FEX_CONFIG_OPT(StrictInProcessSplitLocks, STRICTINPROCESSSPLITLOCKS);
    FEX_CONFIG_OPT(SplitLockInlineContained, SPLITLOCKINLINECONTAINED);
    FEX_CONFIG_OPT(BlockLinking, BLOCKLINKING);
    FEX_CONFIG_OPT(ShadowRetStack, SHADOWRETSTACK);
    FEX_CONFIG_OPT(MonoHacks, MONOHACKS);
    FEX_CONFIG_OPT(VCmpFusion, VCMPFUSION);
  } Config;

  FEXCore::Utils::WritePriorityMutex::Mutex CodeInvalidationMutex {};

  uint32_t StrictSplitLockMutex {};

  // Host timebase frequency in Hz, as __ppc_get_timebase_freq() reports it and
  // therefore the rate `mftb` actually counts at. Read by the A64 frontend for
  // MRS CNTFRQ_EL0, which must agree with the raw CycleCounter the guest reads
  // through CNTVCT_EL0. Deliberately the UNSCALED value: the x86 TSC path below
  // may shift its own copy up by Config.TSCScale to meet a minimum rate, and
  // applying that here would make CNTFRQ disagree with the counter. Zero if the
  // host cannot report one, in which case CNTFRQ_EL0 stays unimplemented rather
  // than handing the guest a zero to divide by.
  uint64_t CycleCounterFrequency {};

  FEXCore::HostFeatures HostFeatures;
  FEXCore::HLE::SyscallHandler* SyscallHandler {};
  FEXCore::HLE::SourcecodeResolver* SourcecodeResolver {};
  FEXCore::ThunkHandler* ThunkHandler {};
#ifdef ARCHITECTURE_ppc64le
  fextl::unique_ptr<FEXCore::CPU::PPC64Dispatcher> Dispatcher;
#else
  fextl::unique_ptr<FEXCore::CPU::Dispatcher> Dispatcher;
#endif
  CodeCache CodeCache;
  fextl::unique_ptr<CodeMapWriter> CodeMapWriter;

  // Cold G2: the translate-ahead helper. Idle (and threadless) until a
  // compiled unit hands it a constant exit target; see TranslateAhead.h.
  TranslateAheadService TranslateAheadHelper {this};

  // Stops and joins the translate-ahead helper. Must run before anything
  // fork()s for the code-cache writer (that fork is a raw fork(), not the
  // LockBeforeFork/UnlockAfterFork pair, so a helper holding any internal lock
  // would strand it in the child) and before the context tears down.
  void StopBackgroundTranslation() override {
    TranslateAheadHelper.Shutdown();
  }

  SignalDelegator* SignalDelegation {};

  ContextImpl(const FEXCore::HostFeatures& Features);

  static void ThreadRemoveCodeEntryFromJit(FEXCore::Core::CpuStateFrame* Frame, uint64_t GuestRIP);

  // This is used as a replacement for the SMC writes in the mono callsite backpatcher that avoids atomic operations
  // (safe as the invalidation mutex is locked) and manually invalidates the modified range. Allowing SMC to be detected
  // even if faulting is disabled.
  static void MonoBackpatcherWrite(FEXCore::Core::CpuStateFrame* Frame, uint8_t Size, uint64_t Address, uint64_t Value);

  void RemoveCustomIREntrypoint(FEXCore::Core::InternalThreadState* Thread, uintptr_t Entrypoint);

  struct GenerateIRResult {
    std::optional<IR::IRListView> IRView;
    uint64_t TotalInstructions;
    uint64_t TotalInstructionsLength;
    uint64_t StartAddr;
    uint64_t Length;
    bool NeedsAddGuestCodeRanges;
    // SMC Idea 4 (FEX_SMCSEMANTICPATCH): rel32 fields of the direct branches
    // the frontend decoded into this block, and the immediate fields of its
    // mov-immediates. Both empty unless the flag is on.
    FEXCore::SMC::BranchImmSites BranchImmSites;
    FEXCore::SMC::MovImmSites MovImmSites;
  };
  [[nodiscard]]
  GenerateIRResult GenerateIR(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP, bool ExtendedDebugInfo, uint64_t MaxInst);

  struct CompileCodeResult {
    CPU::CPUBackend::CompiledCode CompiledCode;
    fextl::unique_ptr<FEXCore::Core::DebugData> DebugData;
    uint64_t StartAddr;
    uint64_t Length;
    bool NeedsAddGuestCodeRanges;
    FEXCore::SMC::BranchImmSites BranchImmSites;
    FEXCore::SMC::MovImmSites MovImmSites;
  };
  [[nodiscard]]
  CompileCodeResult CompileCode(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestRIP, uint64_t MaxInst = 0);
  uintptr_t CompileBlock(FEXCore::Core::CpuStateFrame* Frame, uint64_t GuestRIP, uint64_t MaxInst = 0);
  uintptr_t CompileSingleStep(FEXCore::Core::CpuStateFrame* Frame, uint64_t GuestRIP);

  FEXCore::JITSymbols Symbols;

  FEXCore::Utils::PooledAllocatorVirtual OpDispatcherAllocator {"POWERarmMem_OpDispatcher"};
  FEXCore::Utils::PooledAllocatorVirtual FrontendAllocator {"POWERarmMem_Frontend"};
  FEXCore::Utils::PooledAllocatorVirtualWithGuard CPUBackendAllocator {"POWERarmMem_CPUBackend"};

  // THE FOUR TSO FLAGS BELOW ARE NOT WRITE-ONCE.
  //
  // They used to be plain bools set only during startup, which is why they were
  // read without any synchronisation. FEX_HWTSO revocation broke that: when the
  // kernel refuses PROT_SAO for a range of ordinary guest memory, the frontend
  // calls SetHardwareTSOSupport(false) from a running guest thread and every
  // subsequent compile has to emit barriers (see
  // FEX::HLE::SyscallHandler::RevokeHardwareTSO).
  //
  // That write is published from inside the EXCLUSIVE CodeInvalidationMutex, so
  // it is properly ordered against every compile-time reader -- CompileBlock and
  // ExitFunctionLink hold that mutex shared. The one reader outside it is
  // CPUID.cpp's SupportsEnhancedREPMOVS, which runs from JIT'd code when the
  // guest executes CPUID, and would otherwise be a plain data race. Relaxed
  // atomics cost nothing on ppc64le (a relaxed load is the same `ld` a plain
  // bool read compiles to) and make that race well-defined; the mutex, not the
  // memory order here, is what provides the happens-before that matters.
  //
  // Relaxed is sufficient for that CPUID reader specifically, and the reason is
  // worth writing down because "relaxed" usually is not. The read produces one
  // guest-visible feature bit (ERMS, CPUID.07h:EBX[9]) and nothing else; the
  // flip publishes no other state that this reader then consumes, so there is no
  // acquire/release pairing to get wrong. A guest racing the downgrade sees
  // either answer, and both are self-consistent: ERMS is advisory, and a guest
  // that chooses `rep movsb` on the strength of it executes a block compiled
  // AFTER the flip, hence with barriers.
  //
  // If a fifth flag is ever added here it must be atomic for the same reason.

  // If Atomic-based TSO emulation is enabled or not.
  bool IsAtomicTSOEnabled() const {
    return AtomicTSOEmulationEnabled.load(std::memory_order_relaxed);
  }

  // If atomic-based TSO emulation is enabled for vector operations.
  bool IsVectorAtomicTSOEnabled() const {
    return VectorAtomicTSOEmulationEnabled.load(std::memory_order_relaxed);
  }

  // If atomic-based TSO emulation is enabled for memcpy operations.
  bool IsMemcpyAtomicTSOEnabled() const {
    return MemcpyAtomicTSOEmulationEnabled.load(std::memory_order_relaxed);
  }

  // Whether ordering is being carried by the HARDWARE (ppc64le FEX_HWTSO =
  // PROT_SAO pages) rather than by emitted barriers. Distinct from
  // !IsMemcpyAtomicTSOEnabled(), which is also false when TSO is simply off.
  // Backends need this directly because SAO changes the COST of instructions,
  // not just which ones are needed -- see the dcbz gate in PPC64LE MemoryOps.
  bool IsHardwareTSOSupported() const {
    return SupportsHardwareTSO.load(std::memory_order_relaxed);
  }

  void SetHardwareTSOSupport(bool HardwareTSOSupported) override {
    SupportsHardwareTSO.store(HardwareTSOSupported, std::memory_order_relaxed);

    // Keep the process-wide effective state in step; this is its only writer.
    // See the HardwareTSOState comment near the top of this header.
    if (HardwareTSOSupported) {
      // Startup only, single-threaded, before any compilation.
      EffectiveHardwareTSO.store(HardwareTSOState::Active, std::memory_order_release);
    } else {
      // Only Active -> Revoked means anything. A false here with the state
      // still Off is the ordinary "the probe refused, emit barriers" path and
      // must NOT be recorded as a revocation, or every non-SAO machine would
      // claim to have given up something it never had. compare_exchange rather
      // than load-then-store so the one-way property is structural, not just a
      // consequence of the caller happening to serialise.
      auto Expected = HardwareTSOState::Active;
      EffectiveHardwareTSO.compare_exchange_strong(Expected, HardwareTSOState::Revoked, std::memory_order_release,
                                                   std::memory_order_relaxed);
    }

    UpdateAtomicTSOEmulationConfig();
  }

  void EnableExitOnHLT() override {
    ExitOnHLT = true;
  }

  bool ExitOnHLTEnabled() const {
    return ExitOnHLT;
  }

  bool AreMonoHacksActive() const {
    return Config.MonoHacks && MonoDetected;
  }

protected:
  void UpdateAtomicTSOEmulationConfig() {
    // POWERARM-M0-TODO(tso): TSO emulation is force-disabled. The barrier IR ops, backend lowering, PROT_SAO path and config are x86-TSO machinery wired through the whole JIT, so they stay in-tree but inert until the AArch64 guest memory-ordering story is decided.
    constexpr bool ForceDisableTSO = true;
    if (ForceDisableTSO || SupportsHardwareTSO.load(std::memory_order_relaxed)) {
      // If the hardware supports TSO then we don't need to emulate it through atomics.
      AtomicTSOEmulationEnabled.store(false, std::memory_order_relaxed);
      VectorAtomicTSOEmulationEnabled.store(false, std::memory_order_relaxed);
      MemcpyAtomicTSOEmulationEnabled.store(false, std::memory_order_relaxed);
    } else {
      AtomicTSOEmulationEnabled.store(Config.TSOEnabled, std::memory_order_relaxed);
      VectorAtomicTSOEmulationEnabled.store(Config.TSOEnabled && Config.VectorTSOEnabled, std::memory_order_relaxed);
      MemcpyAtomicTSOEmulationEnabled.store(Config.TSOEnabled && Config.MemcpySetTSOEnabled, std::memory_order_relaxed);
    }
  }

private:
  /**
   * @brief Initializes the JIT compilers for the thread
   *
   * @param State The internal FEX thread state object
   *
   * InitializeCompiler is called inside of CreateThread, so you likely don't need this
   */
  void InitializeCompiler(FEXCore::Core::InternalThreadState* Thread);

  // See the comment above IsAtomicTSOEnabled for why these are atomic.
  std::atomic<bool> SupportsHardwareTSO {false};
  std::atomic<bool> AtomicTSOEmulationEnabled {true};
  std::atomic<bool> VectorAtomicTSOEmulationEnabled {false};
  std::atomic<bool> MemcpyAtomicTSOEmulationEnabled {false};

  bool ExitOnHLT = false;
  FEX_CONFIG_OPT(AppFilename, APP_FILENAME);

  std::shared_mutex CustomIRMutex;
  std::atomic<bool> HasCustomIRHandlers {};
  struct CustomIRHandlerEntry final {
    CustomIREntrypointHandler Handler;
    void* Creator;
    void* Data;
  };
  fextl::unordered_map<uint64_t, CustomIRHandlerEntry> CustomIRHandlers;
  // ForceTSO metadata has its own lock. Readers are CompileBlock's per-block
  // lookups (shared, for the length of the block loop); writers are the
  // Add/RemoveForceTSOInformation calls from guest mmap/munmap. These used to
  // ride on the exclusive CodeInvalidationMutex, which made every mmap and
  // munmap of a volatile-metadata image a stop-the-world for all compiles and
  // L1-miss links in the process. Writers still hold CodeInvalidationMutex
  // SHARED while they hold this exclusively: that is what keeps fork (which
  // takes CodeInvalidationMutex exclusively) from snapshotting this mutex in
  // the locked state into the child.
  std::shared_mutex ForceTSOMutex;
  IntervalList<uint64_t> ForceTSOValidRanges; // The ranges for which ForceTSOInstructions has populated data
  fextl::set<uint64_t> ForceTSOInstructions;

  bool MonoDetected = false;

  std::atomic<uint64_t> MonoBackpatcherBlock;

  std::mutex CodeBufferListLock;
  fextl::vector<std::weak_ptr<CPU::CodeBuffer>> CodeBufferList;
};
} // namespace FEXCore::Context
