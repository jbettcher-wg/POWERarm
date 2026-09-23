// SPDX-License-Identifier: MIT
#include <FEXCore/Utils/SpinWaitLock.h>

#include <Interface/Context/Context.h>
#ifndef ARCHITECTURE_ppc64le
#include <Interface/Core/ArchHelpers/Arm64Emitter.h>
#include <Interface/Core/Dispatcher/Dispatcher.h>
#endif
#include <Interface/Core/JIT/DebugData.h>
#include <Interface/Core/JIT/Relocations.h>
#include <Interface/Core/A64Frontend/Decoder.h>
#include <Interface/Core/A64Frontend/IRBuilder.h>
#include <Interface/Core/LookupCache.h>
#include <Interface/IR/PassManager.h>

#include <FEXCore/Core/Thunks.h>
#include <FEXCore/HLE/SourcecodeResolver.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/ArchHelpers/PPC64CacheFlush.h>

#include <FEXHeaderUtils/Filesystem.h>

#include <git_version.h>

#include <cstdlib>
#include <cstring>
// XXH3_state_t on the stack (per-block hashing on the load path).
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

// ComputeCodeMapId streams the mapped file to derive a content-based cache
// identity. close() was already used unguarded in this file, so POSIX is
// assumed here rather than newly introduced.
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <Interface/Core/ArchHelpers/PPC64Emitter.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <link.h>
#include <optional>
#include <csignal>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace FEXCore {

#if __clang_major__ < 16
ExecutableFileInfo::ExecutableFileInfo(fextl::unique_ptr<HLE::SourcecodeMap> Map, uint64_t FileId, fextl::string Filename)
  : SourcecodeMap(std::move(Map))
  , FileId(FileId)
  , Filename(Filename) {}
#endif
ExecutableFileInfo::~ExecutableFileInfo() = default;

fextl::string CodeMap::GetBaseFilename(const ExecutableFileInfo& MainExecutable, bool AddNombSuffix) {
  auto FileId = MainExecutable.FileId;

  std::string_view base_filename = FHU::Filesystem::GetFilename(std::string_view {MainExecutable.Filename});
  if (FileId != 0xffff'ffff'ffff'ffff) {
    return fextl::fmt::format("{}-{:016x}{}", base_filename, MainExecutable.FileId, AddNombSuffix ? "-nomb" : "");
  }

  return "";
}

fextl::map<CodeMapFileId, CodeMap::ParsedContents> CodeMap::ParseCodeMap(std::ifstream& File) {
  fextl::map<CodeMapFileId, CodeMap::ParsedContents> Ret;
  while (true) {
    Entry Entry;
    File.read(reinterpret_cast<char*>(&Entry), sizeof(Entry));
    if (!File) {
      break;
    }

    if (Entry.FileId == LoadExternalLibrary.FileId && Entry.BlockOffset == LoadExternalLibrary.BlockOffset) {
      ExternalLibraryInfo Info;
      File.read(reinterpret_cast<char*>(&Info), sizeof(Info));

      fextl::string Filename;
      std::getline(File, Filename, '\0');

      // Align to 4-byte boundary
      char Null[4];
      File.read(Null, AlignUp(Filename.size() + 1, 4) - Filename.size() - 1);
      if (!File) {
        break;
      }
      Ret[Info.ExternalFileId].Filename = std::move(Filename);
    } else if (Entry.FileId == SetExecutableFileId {}.Marker.FileId && Entry.BlockOffset == SetExecutableFileId {}.Marker.BlockOffset) {
      CodeMapFileId ExecutableFileId;
      File.read(reinterpret_cast<char*>(&ExecutableFileId), sizeof(ExecutableFileId));
      if (!File) {
        break;
      }
      Ret[ExecutableFileId].IsExecutable = true;
    } else {
      if (!Ret.contains(Entry.FileId)) {
        LogMan::Msg::EFmt("Code map referenced unknown file id {:016x}", Entry.FileId);
      } else {
        Ret[Entry.FileId].Blocks.insert(Entry.BlockOffset);
      }
    }

    if (!File) {
      break;
    }
  }
  return Ret;
}

CodeMapWriter::CodeMapWriter(CodeMapOpener& Opener, bool OpenEagerly)
  : Buffer(4096)
  , FileOpener(Opener) {
  if (OpenEagerly) {
    CodeMapFD = FileOpener.OpenCodeMapFile();
  }
}

CodeMapWriter::~CodeMapWriter() {
  if (CodeMapFD.value_or(-1) != -1) {
    Flush(BufferOffset);
    close(*CodeMapFD);
  }
}

bool CodeMapWriter::IsWriteEnabled(const ExecutableFileSectionInfo& Section) {
  if (CodeMapFD == -1) {
    return false;
  }

  // PV libraries can't yet be read by FEXServer, so skip dumping them
  if (Section.FileInfo.Filename.starts_with("/run/pressure-vessel")) {
    return false;
  }

  if (CodeMapFD) {
    return true;
  }

  // Acquire mutex and re-check CodeMapFD to avoid race conditions
  auto lk = std::unique_lock {Mutex};
  if (!CodeMapFD) {
    CodeMapFD = FileOpener.OpenCodeMapFile();
  }

  return CodeMapFD != -1;
}

void CodeMapWriter::Flush(size_t Offset) {
  // Acquire exclusive lock and flush circular buffer
  std::unique_lock Lock {Mutex};
  Flush(Offset, Lock);
}

void CodeMapWriter::Flush(size_t Offset, std::unique_lock<std::shared_mutex>&) {
  write(*CodeMapFD, Buffer.data(), Offset);
  BufferOffset = 0;
}

void CodeMapWriter::AppendBlock(const FEXCore::ExecutableFileSectionInfo& SectionInfo, uint64_t BlockEntry) {
  if (!IsWriteEnabled(SectionInfo)) {
    return;
  }

  // A block outside the 4 GiB window above the section's FileStartVA has no
  // code-map representation. That used to be fatal, which killed every
  // fexproton title on the 64K host: wine maps a PE image in several views
  // and FirstVMA is not always the lowest one, so a block from a view below
  // it went negative here. The code map is an optimisation; skip the block.
  const uint64_t Offset = BlockEntry - SectionInfo.FileStartVA;
  if (Offset > std::numeric_limits<uint32_t>::max()) {
    static std::atomic<bool> Logged {false};
    if (!Logged.exchange(true)) {
      LogMan::Msg::EFmt("Code map: block {:#x} lies outside the 4 GiB window of {} (FileStartVA {:#x}); not recorded (reported once)",
                        BlockEntry, SectionInfo.FileInfo.Filename, SectionInfo.FileStartVA);
    }
    return;
  }
  BlockEntry = Offset;

  // Register new library if not already known
  bool NewLibraryLoad = false;
  {
    // Check prior registration with shared lock
    std::shared_lock Lock {Mutex};
    NewLibraryLoad = !KnownFileIds.contains(SectionInfo.FileInfo.FileId);
  }
  if (NewLibraryLoad) {
    // Register to map with exclusive lock
    std::unique_lock Lock {Mutex};
    NewLibraryLoad &= KnownFileIds.insert(SectionInfo.FileInfo.FileId).second;
  }
  if (NewLibraryLoad) {
    // Add entry to code map
    AppendLibraryLoad(SectionInfo.FileInfo);
  }

  // Register the actual code block
  CodeMap::Entry DataEntry {SectionInfo.FileInfo.FileId, static_cast<uint32_t>(BlockEntry)};
  AppendData(std::as_bytes(std::span {&DataEntry, 1}));
}

void CodeMapWriter::AppendLibraryLoad(const FEXCore::ExecutableFileInfo& FileInfo) {
  // See CodeMap::ExternalLibraryInfo
  auto ExternalFileId = FileInfo.FileId;
  auto TotalSize = AlignUp(sizeof(CodeMap::LoadExternalLibrary) + sizeof(ExternalFileId) + FileInfo.Filename.size() + 1, 4);
  const auto Data = reinterpret_cast<char*>(alloca(TotalSize));
  auto WritePtr = std::copy_n(reinterpret_cast<const char*>(&CodeMap::LoadExternalLibrary), sizeof(CodeMap::LoadExternalLibrary), Data);
  WritePtr = std::copy_n(reinterpret_cast<const char*>(&ExternalFileId), sizeof(ExternalFileId), WritePtr);
  WritePtr = std::copy(FileInfo.Filename.begin(), FileInfo.Filename.end(), WritePtr);
  std::fill(WritePtr, Data + TotalSize, 0);
  AppendData(std::as_bytes(std::span {Data, TotalSize}));
}

void CodeMapWriter::AppendSetMainExecutable(const FEXCore::ExecutableFileInfo& FileInfo) {
  CodeMap::SetExecutableFileId Data {.ExecutableFileId = FileInfo.FileId};
  AppendData(std::span {reinterpret_cast<const std::byte*>(&Data), sizeof(Data)});
}

void CodeMapWriter::AppendData(std::span<const std::byte> Data) {
  std::shared_lock Lock {Mutex};
  auto Offset = BufferOffset.fetch_add(Data.size_bytes());
  if (Offset + Data.size_bytes() > Buffer.size()) {
    // Acquire exclusive lock and flush the buffer.
    // Under heavy pressure, multiple threads may observe an exhausted buffer simultaneously.
    // The thread with the last in-bounds Offset is responsible for flushing the buffer.
    Lock.unlock();
    bool IsResponsibleForFlush = false;
    {
      std::unique_lock ExclusiveLock {Mutex};
      IsResponsibleForFlush = (Offset <= Buffer.size());
      if (IsResponsibleForFlush) {
        Flush(Offset, ExclusiveLock);
      }
    }
    if (!IsResponsibleForFlush) {
      // Wait for the buffer to be flushed on the responsible thread
      Utils::SpinWaitLock::WaitPred<std::less_equal<>, size_t>(reinterpret_cast<size_t*>(&BufferOffset), Buffer.size());
    }
    AppendData(Data);
    return;
  }

  memcpy(&Buffer.at(Offset), Data.data(), Data.size_bytes());
}

namespace {
// Streaming xxhash helper. Length-prefixes strings so that concatenating two
// values can never collide with a single longer one.
struct StreamHasher {
  XXH3_state_t* State;

  void Add(uint64_t Value) {
    XXH3_64bits_update(State, &Value, sizeof(Value));
  }
  void Add(std::string_view Value) {
    Add(Value.size());
    if (!Value.empty()) {
      XXH3_64bits_update(State, Value.data(), Value.size());
    }
  }
};

// Reserved / sentinel FileId values that must never be produced by a hash.
constexpr uint64_t InvalidFileId = 0xffff'ffff'ffff'ffff;

uint64_t SanitizeId(uint64_t Id) {
  // 0 means "unset" to FEXOfflineCompiler's code-map parser and
  // InvalidFileId means "no file"; nudge rather than collide.
  if (Id == 0 || Id == InvalidFileId) {
    return 1;
  }
  return Id;
}
} // namespace

namespace {
  // Detected host features, registered by the context before the id is first
  // computed. See SetCodeCacheHostFeatures.
  std::atomic<const HostFeatures*> CodeCacheHostFeatures {nullptr};

  // NT_GNU_BUILD_ID of the running POWERarm executable. FEXCore is linked into
  // it statically, so this changes with any change to the code generator, even
  // one that leaves GIT_HASH alone (an uncommitted tree).
  fextl::vector<uint8_t> ExecutableBuildId() {
    fextl::vector<uint8_t> Id;
    dl_iterate_phdr(
      [](dl_phdr_info* Info, size_t, void* Data) -> int {
        auto* Out = static_cast<fextl::vector<uint8_t>*>(Data);
        for (ElfW(Half) i = 0; i < Info->dlpi_phnum; ++i) {
          const auto& Phdr = Info->dlpi_phdr[i];
          if (Phdr.p_type != PT_NOTE) {
            continue;
          }
          const auto* Note = reinterpret_cast<const uint8_t*>(Info->dlpi_addr + Phdr.p_vaddr);
          size_t Left = Phdr.p_memsz;
          while (Left >= sizeof(ElfW(Nhdr))) {
            const auto* N = reinterpret_cast<const ElfW(Nhdr)*>(Note);
            const size_t NameSize = AlignUp(N->n_namesz, 4);
            const size_t DescSize = AlignUp(N->n_descsz, 4);
            const size_t Total = sizeof(ElfW(Nhdr)) + NameSize + DescSize;
            if (Total > Left) {
              break;
            }
            if (N->n_type == NT_GNU_BUILD_ID && N->n_namesz == 4 && ::memcmp(Note + sizeof(ElfW(Nhdr)), "GNU", 4) == 0) {
              const auto* Desc = Note + sizeof(ElfW(Nhdr)) + NameSize;
              Out->assign(Desc, Desc + N->n_descsz);
              return 1;
            }
            Note += Total;
            Left -= Total;
          }
        }
        // The first object is the executable; stop either way.
        return 1;
      },
      &Id);
    return Id;
  }

  // 32-bit id of this emulator build: GIT_HASH and the executable's build id.
  uint32_t EmulatorBuildId() {
    static const uint32_t Id = [] {
      const auto BuildId = ExecutableBuildId();
      XXH3_state_t State;
      XXH3_64bits_reset(&State);
      XXH3_64bits_update(&State, GIT_HASH.data(), GIT_HASH.size());
      XXH3_64bits_update(&State, BuildId.data(), BuildId.size());
      return static_cast<uint32_t>(XXH3_64bits_digest(&State));
    }();
    return Id;
  }
} // namespace

void SetCodeCacheHostFeatures(const HostFeatures& Features) {
  static HostFeatures Copy;
  const HostFeatures* Expected = nullptr;
  Copy = Features;
  CodeCacheHostFeatures.compare_exchange_strong(Expected, &Copy);
}

uint64_t ComputeCodeCacheConfigId() {
  // Computed once: config is loaded before any mapping is tracked and does not
  // change afterwards, and every cache filename in the process must agree.
  static const uint64_t Id = []() -> uint64_t {
    XXH3_state_t* State = XXH3_createState();
    if (!State) {
      // Without a hash we cannot distinguish configurations, and silently
      // sharing one cache namespace across configurations is the failure this
      // whole mechanism exists to prevent. Use a value that no real
      // configuration can produce so nothing loads.
      return InvalidFileId;
    }
    XXH3_64bits_reset(State);
    StreamHasher Hasher {State};

    // 1. The FEX build. CodeCacheHeader::FEXVersion carries this too, but
    //    hashing it into the *filename* means caches from different builds
    //    coexist instead of one rejecting and deleting the other's file.
    XXH3_64bits_update(State, GIT_HASH.data(), GIT_HASH.size());
    {
      const auto BuildId = ExecutableBuildId();
      Hasher.Add(std::string_view {reinterpret_cast<const char*>(BuildId.data()), BuildId.size()});
    }

    // Detected host features. SupportsISA30 alone selects between instruction
    // sequences in dozens of lowerings, and FEX_HOSTFEATURES=disableisa30 flips
    // it on the same machine; DCacheLineSize is baked into dcbz loops. Hashed
    // field by field (the struct has padding); the size assert makes a new
    // field fail to build until it is added here.
    const auto* Features = CodeCacheHostFeatures.load(std::memory_order_acquire);
    if (!Features) {
      XXH3_freeState(State);
      return InvalidFileId;
    }
    static_assert(sizeof(HostFeatures) == 72, "HostFeatures changed: hash the new field below");
    {
      const auto& F = *Features;
      for (uint64_t V : {uint64_t {F.DCacheLineSize}, uint64_t {F.ICacheLineSize}}) {
        Hasher.Add(V);
      }
      for (bool B : {F.SupportsCacheMaintenanceOps, F.SupportsAES, F.SupportsCRC, F.SupportsCLZERO, F.SupportsAtomics, F.SupportsRCPC,
                     F.SupportsTSOImm9, F.SupportsTSODisp16, F.SupportsRAND, F.SupportsAVX, F.SupportsAVX2, F.SupportsSVE128, F.SupportsSVE256,
                     F.SupportsSHA, F.SupportsPMULL_128Bit, F.SupportsCSSC, F.SupportsFCMA, F.SupportsFlagM, F.SupportsFlagM2, F.SupportsFCmpX86,
                     F.SupportsRPRES, F.SupportsPreserveAllABI, F.SupportsAES256, F.SupportsSVEBitPerm, F.SupportsCPUIndexInTPIDRRO,
                     F.SupportsFRINTTS, F.SupportsECV, F.SupportsWFXT, F.Supports3DNow, F.SupportsSSE4a, F.SupportsMOPS, F.SupportsISA30,
                     F.SupportsVCmpFlagBranch, F.SupportsFlagTransparentSelect, F.SupportsAFP, F.SupportsFloatExceptions, F.IsInstCountCI}) {
        Hasher.Add(uint64_t {B});
      }
      // The distinct MIDR values, not one per CPU: the count follows the
      // process's CPU affinity, and a `taskset` run must share the cache of an
      // unpinned one. Codegen only reads the values (the LRCPC2 erratum list).
      fextl::vector<uint32_t> MIDRs = F.CPUMIDRs;
      std::ranges::sort(MIDRs);
      MIDRs.erase(std::unique(MIDRs.begin(), MIDRs.end()), MIDRs.end());
      Hasher.Add(uint64_t {MIDRs.size()});
      for (uint32_t MIDR : MIDRs) {
        Hasher.Add(uint64_t {MIDR});
      }
    }

    // 2. Every option that changes emitted host code. When adding a codegen
    //    option, add it here: an option missing from this list means a cache
    //    generated with it set is loaded into a process without it, and the
    //    difference is *executed*.
#define HASH_OPT(NAME) Hasher.Add(static_cast<uint64_t>(FEXCore::Config::Get_##NAME()()))
#define HASH_STR_OPT(NAME) Hasher.Add(std::string_view {FEXCore::Config::Get_##NAME()()})

    // Block shape and mode.
    HASH_OPT(MULTIBLOCK);
    HASH_OPT(MAXINST);
    HASH_OPT(O0);
    HASH_OPT(SINGLESTEP);
    // HOSTFEATURES is load-bearing on ppc64le beyond CPUID: host detection
    // hardcodes SupportsAVX off (Source/Common/HostFeatures.cpp), so
    // FEX_HOSTFEATURES=enableavx is the ONLY way it flips — and it changes
    // emitted block bytes, not just what the guest sees. SpillStaticRegs /
    // FillStaticRegs emit the AVX-high VSX bank sync only when it is set, and
    // the context load/store lowerings redirect avx_high[] accesses into that
    // register bank (JIT/PPC64LE/MemoryOps.cpp). Code cached with it on is
    // unsound in a session with it off, and vice versa.
    HASH_OPT(HOSTFEATURES);
    HASH_OPT(FORCESVEWIDTH);
    HASH_OPT(MONOHACKS);

    // Memory model. These change the instructions emitted for essentially every
    // guest load and store.
    HASH_OPT(TSOENABLED);
    HASH_OPT(LOCKONLYTSO);
    HASH_OPT(VECTORTSOENABLED);
    HASH_OPT(MEMCPYSETTSOENABLED);
    HASH_OPT(HALFBARRIERTSOENABLED);
    // HWTSO compiles with NO TSO barriers at all (hardware SAO pages carry the
    // ordering); a cache built with it on is unsound in any session with it off.
    //
    // Hash the EFFECTIVE state, never HASH_OPT(HWTSO). The requested option and
    // what the JIT actually did diverge on any machine that cannot do SAO, and
    // hashing the request produced the SAME id for two incompatible code images:
    //
    //   Box A, POWER8 + hash MMU: FEX_HWTSO=1, probe succeeds, zero barriers
    //     emitted, cache written under id(HWTSO=1).
    //   Box B, POWER9/POWER10 radix or an LPAR without
    //     CONFIG_PPC_PROT_SAO_LPAR: FEX_HWTSO=1, probe FAILS, full barriers
    //     emitted -- and it computed id(HWTSO=1) too.
    //
    // Same filename, and Box A's barrier-free blocks then execute on Box B over
    // pages with no hardware ordering at all. EffectiveHardwareTSO is what
    // SetHardwareTSOSupport installed, so the two boxes now land in different
    // namespaces and Box B simply misses.
    //
    // Ordering: FEX::Kernel::Init runs SetupTSOEmulation (FEXInterpreter.cpp
    // :403) long before the SyscallHandler member initialiser that forces this
    // function-local static (:614), so the value is already final here.
    //
    // What this CANNOT cover, and must not be mistaken for covering: a mid-run
    // revocation. The id is memoised above; by the time HardwareTSOState goes
    // Active -> Revoked it can no longer change, so this always observes Active
    // in the frontend. That case is handled where it has to be, by the
    // LoadCodeCache/SaveCodeCaches gates in SyscallsSMCTracking.cpp. Revoked is
    // still hashed distinctly rather than folded into Active, so that if this
    // computation is ever moved later the failure mode is a cache MISS (cold,
    // correct) rather than a hit on blocks compiled under the other model.
    Hasher.Add(static_cast<uint64_t>(Context::EffectiveHardwareTSO.load(std::memory_order_acquire)));
    // NONTSORBP drops barriers from RBP-addressed accesses the same way.
    HASH_OPT(NONTSORBP);
    HASH_OPT(STRICTINPROCESSSPLITLOCKS);
    HASH_OPT(SPLITLOCKINLINECONTAINED);
    HASH_OPT(KERNELUNALIGNEDATOMICBACKPATCHING);
    HASH_OPT(VOLATILEMETADATA);
    // The DFCE toggles change emitted flag code wholesale (and DISABLEDFCE
    // also disables the pipeline's only dead-code elimination).
    HASH_OPT(DISABLEDFCE);
    HASH_OPT(DISABLEDFCESTOREELIM);
    HASH_STR_OPT(EXTENDEDVOLATILEMETADATA);

    // SMC. SMCSemanticPatch in particular changes constant materialisation
    // (fixed-width windows instead of the shortest sequence), and the cheap tier
    // changes the block shape outright.
    HASH_OPT(SMCCHECKS);
    HASH_OPT(SMCSOFTINVALIDATE);
    HASH_OPT(SMCFILEIMMUTABLE);
    HASH_OPT(SMCLAZYINVAL);
    HASH_OPT(SMCLAZYSCRUB);
    HASH_OPT(SMCSTOREEMULATION);
    HASH_OPT(SMCSEMANTICPATCH);
    HASH_OPT(SMCSTOREBACKPATCH);
    HASH_OPT(SMCCHEAPTIER);
    HASH_OPT(SMCCHEAPTIERTHRESHOLD);
    HASH_OPT(SMCCHEAPTIERMAXINST);
    HASH_OPT(SMCMPROTECTDEFER);

    // Vector-scan fusion changes block *shape* (it swallows three guest
    // instructions and grows an extra IR block on the match edge), so cached
    // code produced with it on must not be reused with it off, or vice versa.
    HASH_OPT(VCMPFUSION);

    // Lookup-cache shape: the dispatcher's inlined L1 probe is emitted into
    // every block exit on this backend.
    HASH_OPT(DISABLEL2CACHE);
    HASH_OPT(DYNAMICL1CACHE);

    // Exit shape: the shadow call/ret stack adds push/pop sequences to every
    // CALL/RET-hinted exit.
    HASH_OPT(SHADOWRETSTACK);

    // Host page size (4K vs 64K kernel on the same machine). Nothing the JIT
    // emits depends on it today, and the on-disk format does not either (the
    // code buffer is memcpy'd out of the mapped file, see LoadData), so caches
    // are portable in principle. Hashed anyway, as design section 7 asked: the
    // two kernels then never share a cache namespace, and a host-page-dependent
    // emitter change that lands later can only cause a cold miss, not a hit on
    // code compiled for the other granule. Costs one cold run per kernel.
    Hasher.Add(static_cast<uint64_t>(FEXCore::HostPage::Size()));

    // Backend env toggles that change emitted block bytes. Raw getenv switches
    // with no config plumbing, so hash their EFFECTIVE values exactly as the
    // emitters parse them (JIT.cpp Compute32MaskElision / ComputeTSOPairElision
    // / the fallthrough gate; BranchOps.cpp DEF_OP(Thunk)). FALLTHROUGH is
    // presence-enabled and NO_THUNK_PARTIAL_FILL presence-disabled — mirror,
    // don't normalize.
    {
      Hasher.Add(static_cast<uint64_t>(getenv("FEX_NOCONSTCACHE") != nullptr));
      Hasher.Add(static_cast<uint64_t>(getenv("FEX_NOSPLATFUSION") != nullptr));
      const char* ZExtEnv = getenv("FEX_ZEXTOPT");
      Hasher.Add(static_cast<uint64_t>(!(ZExtEnv && ZExtEnv[0] == '0')));
      // Per-pass halves of the same switch. Hashed separately: each one changes
      // emitted code on its own, so a cache built with one off is unsound in a
      // session with it on.
      const char* ZExtConsumerEnv = getenv("FEX_ZEXTOPT_CONSUMER");
      Hasher.Add(static_cast<uint64_t>(!(ZExtConsumerEnv && ZExtConsumerEnv[0] == '0')));
      const char* ZExtProducerEnv = getenv("FEX_ZEXTOPT_PRODUCER");
      Hasher.Add(static_cast<uint64_t>(!(ZExtProducerEnv && ZExtProducerEnv[0] == '0')));
      // FEX_GUESTTRACE / FEX_GUESTSERIALIZE / FEX_GUESTANCHOR family (JIT.cpp
      // instrumentation): the target lists change the emitted prologue of any
      // matched block, so hash the raw strings (FNV-1a) rather than presence.
      // MOREOVER, instrumented prologues bake per-process pointers into host
      // code (the trace ring mmap, the serialize lock word) and, in anchor
      // mode, per-LAUNCH module bases -- a cached instrumented block served in
      // a later process would poke another process's addresses. While any of
      // these is armed, fold in a per-process random salt so the cache never
      // matches across runs; disarming the envs restores normal caching.
      {
        bool Armed = false;
        uint64_t H = 0xcbf29ce484222325ull;
        for (const char* Env : {getenv("FEX_GUESTTRACE"), getenv("FEX_GUESTTRACE_DEREF"), getenv("FEX_GUESTSERIALIZE"),
                                getenv("FEX_GUESTTRACE_RVA"), getenv("FEX_GUESTSERIALIZE_RVA"), getenv("FEX_GUESTANCHOR")}) {
          Armed |= (Env && *Env);
          for (; Env && *Env; ++Env) {
            H = (H ^ static_cast<uint8_t>(*Env)) * 0x100000001b3ull;
          }
          H = (H ^ 0xff) * 0x100000001b3ull; // separator so "a",""/"","a" differ
        }
        if (Armed) {
          static const uint64_t ProcessSalt = (static_cast<uint64_t>(::getpid()) << 32) ^ static_cast<uint64_t>(::time(nullptr));
          H ^= ProcessSalt;
        }
        Hasher.Add(H);
      }
      Hasher.Add(static_cast<uint64_t>(getenv("FEX_FALLTHROUGH") != nullptr));
      const char* PairEnv = getenv("FEX_TSOPAIRELIDE");
      Hasher.Add(static_cast<uint64_t>(!(PairEnv && PairEnv[0] == '0')));
      Hasher.Add(static_cast<uint64_t>(getenv("FEX_NO_THUNK_PARTIAL_FILL") != nullptr));
      // DFCE ReplacementNoWrite arm (RedundantFlagCalculationElimination.cpp):
      // presence-DISABLED, 64-bit-guest-only; rewrites value-dead flag ops to
      // their flags-only forms pre-RA, so it changes emitted block bytes.
      Hasher.Add(static_cast<uint64_t>(getenv("FEX_NO_DFCE_NOWRITE") != nullptr));
      // Linked-exit RIP sink (BranchOps.cpp): now default-ON, with
      // FEX_NOSINKEXITRIP as the kill switch. Presence-DISABLED; the switch
      // moves the destination-RIP constant and its `std State.rip` from below
      // the block-link patch site back to above it, so the emitted exit differs.
      Hasher.Add(static_cast<uint64_t>(getenv("FEX_NOSINKEXITRIP") != nullptr));
      // A64 FP cold blocks (JITClass.h FPColdEnabled, DEF_OP(A64FArith)).
      // Value-DISABLED with "0", mirroring JITClass.h's parse. It is a
      // diagnostic control that deliberately emits WRONG code — the NaN check
      // without the branch to the fix-up — so a cache built under it must never
      // be served to a session without it.
      {
        const char* FPColdEnv = getenv("POWERARM_FPCOLD");
        Hasher.Add(static_cast<uint64_t>(!(FPColdEnv && FPColdEnv[0] == '0')));
      }
      // P5.0.2 re-zero policy at block exits (BranchOps.cpp R0ZeroMode).
      // Three-way: elide (default) / always emit `li r0,0` / emit `tdnei r0,0`.
      // All three differ in emitted bytes, and the trap arm differs in
      // behaviour, so hash the resolved mode rather than either flag alone.
      Hasher.Add(static_cast<uint64_t>(getenv("FEX_R0TRAP")   != nullptr ? 2 :
                                       getenv("FEX_NOR0ELIDE") != nullptr ? 1 : 0));
      // Entry-point prologue shape (JIT.cpp EmitStoreBlockBeginToInlineHeader):
      // presence-DISABLED; picks between the addi/addis delta fold and the
      // legacy LoadImm32+subf, which differ in instruction count. Only has an
      // effect when the store is emitted at all, i.e. under FEX_NOBLOCKHEADER=0
      // (audit P1 elides the whole 5-instruction store by default).
      Hasher.Add(static_cast<uint64_t>(getenv("FEX_NOHDRADDI") != nullptr));
      // Audit P1 InlineJITBlockHeader store (JIT.cpp EmitEntryPoint): resolved
      // bool, true = elided (the default). Unset or "1" elides the whole
      // bcl/mflr/addi/std prologue store; "0" emits it as before. Five
      // instructions at the head of every entry point, so it changes the
      // emitted bytes of essentially every block.
      {
        const char* NoBlockHeaderEnv = getenv("FEX_NOBLOCKHEADER");
        Hasher.Add(static_cast<uint64_t>(!(NoBlockHeaderEnv && NoBlockHeaderEnv[0] == '0')));
      }
      // Shifted-32 rule in LoadImm64 (CodeEmitter/PPC64LE/Emitter.h
      // DisableShiftedImm32): presence-DISABLED; changes how wide the constant
      // load is at every 64-bit guest-RIP materialisation, so it changes
      // emitted block bytes and every downstream branch displacement.
      Hasher.Add(static_cast<uint64_t>(getenv("FEX_NOSHIFTIMM32") != nullptr));
      // XER arithmetic-write kill switch (PPC64Emitter.h XERArithDisabled):
      // presence-DISABLED; flips every CA/OV write helper between the addic/
      // addo arithmetic forms and the legacy mfspr/rlwimi/mtspr RMW shapes.
      Hasher.Add(static_cast<uint64_t>(getenv("FEX_NOXERARITH") != nullptr));
      // Spin-loop SMT priority hints emit `or 31,31,31` / `or 2,2,2` into
      // backedges and region exits, so both the feature switch and the
      // stationary-poll bisect switch change emitted block bytes. Presence-'1'
      // enabled for ANYLOOP, mirroring JIT.cpp's parse.
      HASH_OPT(DISABLESPINLOOPHINT);
      const char* HintAnyEnv = getenv("FEX_SPINHINT_ANYLOOP");
      Hasher.Add(static_cast<uint64_t>(HintAnyEnv && HintAnyEnv[0] == '1'));
      // The last three block-transfer-era switches that change emitted code and
      // were missing from this list (same gap class as the four below).
      // FEX_DEADPROLOGUE is a three-way mode (off / "trap" / "emit"), resolved
      // here exactly as JIT.cpp's DeadPrologueMode resolves it — unknown values
      // fall back to Off there, so they hash as Off here too.
      const char* DeadProEnv = getenv("FEX_DEADPROLOGUE");
      Hasher.Add(static_cast<uint64_t>(!DeadProEnv                          ? 0 :
                                       ::strcmp(DeadProEnv, "trap") == 0 ? 1 :
                                       ::strcmp(DeadProEnv, "emit") == 0 ? 2 :
                                                                           0));
      // FEX_ENTRYWATCH is a guest-RIP range whose VALUE picks which entry
      // points get the watch instrumentation, so the string itself is hashed.
      const char* EntryWatchEnv = getenv("FEX_ENTRYWATCH");
      Hasher.Add(std::string_view {EntryWatchEnv ? EntryWatchEnv : ""});
      // FEX_NO_ABI_LIVEMASK reverts the syscall mini-frame FPR saves to the
      // full set; presence-DISABLED.
      Hasher.Add(static_cast<uint64_t>(getenv("FEX_NO_ABI_LIVEMASK") != nullptr));
      // These four also change emitted code and were simply missing from this
      // list. Unlike BlockLinking below — which is excluded deliberately and
      // says so — nothing documented their absence, so a cache built with any
      // of them flipped would have been reused by a session with it unflipped.
      // Latent rather than live, since EnableCodeCachingWIP is off by default,
      // but it is exactly the shape of bug that costs a week when it does bite.
      HASH_OPT(DISABLECMPBRANCHFUSION);
      HASH_OPT(DISABLESCALARSPLATCHAIN);
      // Aligned 128-bit vector lowering: with it on, an $Align-certified
      // LoadMem/StoreMem is one lvx/stvx; with it off it is the two-instruction
      // lxvd2x+xxpermdi / xxpermdi+stxvd2x pair. Different bytes for the same
      // guest instruction, so the two are not interchangeable in a cache.
      HASH_OPT(DISABLEALIGNEDVECTORLDST);

      // Spin collapse changes the emitted Sub and CondJump inside every matched
      // spin region, so the raw option value is part of the block identity.
      // This used to re-parse getenv here and substitute a sentinel 8 for
      // "enabled but out of range", where the JIT ran K=32 — consistent, but
      // only by accident, and it read like a bug. Now that SpinCollapse is a
      // real config option there is one value and no second parse to drift.
      HASH_OPT(SPINCOLLAPSE);
      // FEX_MEMCPYDCBZ adds a dcbz cache-line tier to the REP MOVSB fast path,
      // so MemCpy blocks differ byte-for-byte with it on. Mirrors JIT.cpp's
      // parse (any non-empty, non-"0" value enables).
      const char* DcbzEnv = getenv("FEX_MEMCPYDCBZ");
      Hasher.Add(static_cast<uint64_t>(DcbzEnv && DcbzEnv[0] != '\0' && DcbzEnv[0] != '0'));
      // FEX_MEMSETDCBZ=0 removes the dcbz block-zero path from every rep-stos
      // block. Only an explicit "0" disables, mirroring JIT.cpp's parse.
      const char* SetDcbzEnv = getenv("FEX_MEMSETDCBZ");
      Hasher.Add(static_cast<uint64_t>(!(SetDcbzEnv && SetDcbzEnv[0] == '0')));
      // FEX_PPCINLINECONST=0 reverts IREmitter's inline-constant predicates to
      // the AArch64 ones (Interface/IR/PPC64Immediates.h). It decides which
      // operands are folded into an instruction and which occupy a register, so
      // it changes the operands, the instruction count AND the allocation of
      // essentially every arithmetic and logical op in a block. Only an
      // explicit "0" disables, mirroring InlineConstEnabled().
      const char* InlineConstEnv = getenv("FEX_PPCINLINECONST");
      Hasher.Add(static_cast<uint64_t>(!(InlineConstEnv && InlineConstEnv[0] == '0')));
      // FEX_PPCLOGICALIMM=0 reverts the logical-immediate lowering in
      // JIT/PPC64LE/ALUOps.cpp — the rotate-and-mask AND forms, andis. for the
      // flag-setting ANDs, and the oris+ori pair in Or/Xor — back to
      // LoadConstant plus a register-form op. Only an explicit "0" disables,
      // mirroring LogicalImmEnabled().
      const char* LogicalImmEnv = getenv("FEX_PPCLOGICALIMM");
      Hasher.Add(static_cast<uint64_t>(!(LogicalImmEnv && LogicalImmEnv[0] == '0')));
      const char* RegionWindowEnv = getenv("POWERARM_REGIONWINDOW");
      Hasher.Add(std::string_view {RegionWindowEnv ? RegionWindowEnv : ""});
      const char* MaxLeadersEnv = getenv("POWERARM_MAXLEADERS");
      Hasher.Add(std::string_view {MaxLeadersEnv ? MaxLeadersEnv : ""});
    }

    // The scope option itself, because it decides whether the process runs as a
    // cache generator (section-bounded decode, relocations retained).
    HASH_STR_OPT(CODECACHESCOPE);

    // Block linking changes the exit shape (link thunks and records), even
    // though cached blocks are always stored unlinked.
    HASH_OPT(BLOCKLINKING);

    // Detected host capabilities are hashed above, next to the build identity.
#undef HASH_OPT
#undef HASH_STR_OPT

    const uint64_t Result = XXH3_64bits_digest(State);
    XXH3_freeState(State);
    return SanitizeId(Result);
  }();

  return Id;
}

} // namespace FEXCore

namespace FEXCore::Context {

// =============================================================================
// On-disk format, version 6.
//
// A cache for one guest file is a set of SEGMENTS: `<Base>`, `<Base>.1`, ...,
// `<Base>.<MaxSegments-1>`, where Base is `<cache dir>/cache/<name>-<FileId>-<ConfigId>`.
// Every segment is self-contained:
//
//   SegmentHeader | SegmentBlock[NumBlocks] (sorted by GuestOffset) | pad
//   | CPU::Relocation[NumRelocs] | pad | code
//
// Each SegmentBlock describes one block exactly as the JIT laid it out
// (JITCodeHeader .. JITCodeTail + RIP entries), stored UNLINKED and with every
// relocation applied for storage (guest RIPs as offsets from the file's load
// base, host symbols zeroed). Relocation offsets are relative to the block.
//
// Loading is lazy and per block: a dispatcher miss that would compile GuestRIP
// first looks the block up here (TryLoadBlock). A block is installed only if
//   - its EntryHash (entry fields + code + relocations) verifies, which catches
//     torn or corrupt files without fsync;
//   - the guest bytes it was decoded from, [GuestRIP, GuestRIP + GuestLength),
//     are executable in this process and hash to GuestHash. This is what makes
//     the cache sound no matter how the file identity was derived: a rebuilt or
//     replaced binary, a guest that patched its own code before the block was
//     first reached, or a different file that happens to share an identity all
//     fail this check and compile instead;
//   - its relocations apply cleanly.
// Everything else a translation depends on (FEX build, codegen options, host
// ISA features, page size) is in ConfigId, and so in the file name.
//
// Writing appends: a process writes only blocks it compiled that no existing
// segment holds, as a new segment created with link(2) (never replaces a
// file). When all MaxSegments names are taken the writer compacts them into
// `<Base>` under an exclusive flock; appenders hold the lock shared. A reader
// needs no lock: it opens `<Base>`, `<Base>.1`, ... until the first missing
// name, and every file it maps stays valid even if a compaction unlinks it.
// =============================================================================
namespace {
  // ::write is allowed to write fewer bytes than requested and to fail with EINTR.
  bool WriteAll(int FD, const void* Data, size_t Size) {
    const auto* Ptr = reinterpret_cast<const uint8_t*>(Data);
    while (Size) {
      const ssize_t Written = ::write(FD, Ptr, Size);
      if (Written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (Written == 0) {
        return false;
      }
      Ptr += Written;
      Size -= static_cast<size_t>(Written);
    }
    return true;
  }

  uint64_t MonotonicMilliseconds() {
    struct timespec TS {};
    if (::clock_gettime(CLOCK_MONOTONIC_COARSE, &TS) != 0) {
      return 0;
    }
    return static_cast<uint64_t>(TS.tv_sec) * 1000 + static_cast<uint64_t>(TS.tv_nsec) / 1000000;
  }

  uint64_t MonotonicNS() {
    struct timespec TS {};
    ::clock_gettime(CLOCK_MONOTONIC, &TS);
    return static_cast<uint64_t>(TS.tv_sec) * 1000000000ULL + static_cast<uint64_t>(TS.tv_nsec);
  }

  // Adds the scope's wall time to a counter.
  struct ScopedNS {
    std::atomic<uint64_t>& Counter;
    uint64_t Start = MonotonicNS();
    ~ScopedNS() {
      Counter.fetch_add(MonotonicNS() - Start, std::memory_order_relaxed);
    }
  };

  // Only POWERARM_CODECACHESTATS=1 prints the timers. The load timer runs once
  // per installed block, and its two clock_gettime calls were 1.2% of a warm
  // `gcc -c empty.c`.
  bool StatsTimersEnabled() {
    static const bool Enabled = [] {
      const char* Env = getenv("FEX_CODECACHESTATS");
      return Env && *Env == '1';
    }();
    return Enabled;
  }

  // Where DumpStats writes: a private copy of the stderr the process started
  // with. The counters are printed as the image ends, and by then the guest may
  // have closed its own stderr (every coreutils program does, in the atexit
  // handler close_stdout), which silently lost the line. Taken in the
  // CodeCache constructor, before any guest code runs.
  struct StatsOutput {
    int FD = -1;
    dev_t Dev {};
    ino_t Ino {};
  };
  const StatsOutput& StatsStream() {
    static const StatsOutput Out = [] {
      StatsOutput O;
      if (!StatsTimersEnabled()) {
        return O;
      }
      struct stat St {};
      const int FD = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
      if (FD >= 0 && ::fstat(FD, &St) == 0) {
        O = {FD, St.st_dev, St.st_ino};
      } else if (FD >= 0) {
        ::close(FD);
      }
      return O;
    }();
    return Out;
  }

  uint64_t MonotonicSeconds() {
    struct timespec TS {};
    if (::clock_gettime(CLOCK_MONOTONIC, &TS) != 0) {
      return 0;
    }
    return static_cast<uint64_t>(TS.tv_sec);
  }

  constexpr std::array<char, 4> SegmentMagic = {'P', 'A', 'C', 'C'};
  constexpr uint32_t SegmentVersion = 6;
  constexpr size_t MaxSegments = 8;
  // A runtime writer skips files with fewer new blocks than this. Stops a
  // process that compiled a handful of rare-path blocks from spending a
  // segment (and, eventually, a compaction) on them.
  constexpr size_t MinNewBlocksPerSegment = 8;
  constexpr uint64_t BlockAlignment = 16;

  // Size sweep (SweepCacheDirectory). A namespace's last use is the newest
  // mtime of its files; a process opening a namespace refreshes it when older
  // than UseStampSeconds.
  constexpr int64_t UseStampSeconds = 600;
  constexpr int64_t SweepIntervalSeconds = 60;
  // Another build's namespace, or a leftover temp file, unused this long is
  // removed whatever the cap.
  constexpr int64_t StaleSeconds = 3600;

  struct SegmentHeader {
    std::array<char, 4> Magic;
    uint32_t Version;
    uint64_t ConfigId;
    uint64_t FileId;
    std::array<uint8_t, 20> BuildHash;
    uint32_t NumBlocks;
    uint32_t NumRelocs;
    // EmulatorBuildId(): lets the size sweep recognise another build's files
    // without the ConfigId inputs.
    uint32_t EmulatorId;
    // /proc/sys/kernel/random/boot_id of the writer. Within that boot the file
    // is exactly what was written (it was complete before link(2) or rename(2)
    // published it), so entry hashes only need checking in another boot, where
    // a crash may have left it torn.
    std::array<uint8_t, 16> BootId;
    uint64_t IndexOffset;
    uint64_t RelocOffset;
    uint64_t CodeOffset;
    uint64_t CodeSize;
    uint64_t HeaderHash; // XXH3 of every byte above
  };
  static_assert(sizeof(SegmentHeader) == 112 && offsetof(SegmentHeader, HeaderHash) == 104, "cache segment header layout");

  // This boot's id, all zero if unreadable (which never matches a writer's, so
  // every entry is then checked).
  const std::array<uint8_t, 16>& CurrentBootId() {
    static const std::array<uint8_t, 16> Id = [] {
      std::array<uint8_t, 16> Out {};
      int FD = ::open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
      if (FD == -1) {
        return Out;
      }
      char Buf[64] {};
      const ssize_t N = ::read(FD, Buf, sizeof(Buf) - 1);
      ::close(FD);
      size_t Nibbles = 0;
      for (ssize_t i = 0; i < N && Nibbles < 32; ++i) {
        const char Ch = Buf[i];
        int V = (Ch >= '0' && Ch <= '9') ? Ch - '0' : (Ch >= 'a' && Ch <= 'f') ? Ch - 'a' + 10 : -1;
        if (V < 0) {
          continue;
        }
        Out[Nibbles / 2] |= static_cast<uint8_t>(V << ((Nibbles % 2) ? 0 : 4));
        ++Nibbles;
      }
      if (Nibbles != 32) {
        Out = {};
      }
      return Out;
    }();
    return Id;
  }

  // POWERARM_CODECACHEVERIFY=1: check every entry hash even for this boot's files.
  bool ForceVerify() {
    static const bool Force = [] {
      const char* Env = getenv("FEX_CODECACHEVERIFY");
      return Env && *Env == '1';
    }();
    return Force;
  }

  struct SegmentBlock {
    uint64_t GuestOffset; // entry PC - file load base
    uint64_t GuestHash;   // XXH3 of the guest bytes [entry, entry + GuestLength)
    uint64_t CodeOffset;  // in the code section
    uint32_t GuestLength;
    uint32_t CodeSize;    // hot size + cold size
    uint32_t EntryOffset; // entry point - JITCodeHeader
    uint32_t RelocBegin;
    uint32_t RelocCount;
    uint32_t ColdSize;    // cold thunk bytes at end of Code (0 if none)
    uint64_t EntryHash;   // XXH3 of the fields above, the code and the relocations
  };
  static_assert(sizeof(SegmentBlock) == 56 && offsetof(SegmentBlock, EntryHash) == 48, "cache segment block layout");

  constexpr size_t RelocSize = sizeof(CPU::Relocation);
  static_assert(RelocSize == 48, "Breaking change in code cache data layout");

  fextl::string SegmentPath(const fextl::string& Base, size_t Index) {
    return Index == 0 ? Base : fextl::fmt::format("{}.{}", Base, Index);
  }

  uint64_t HashHeader(const SegmentHeader& H) {
    return XXH3_64bits(&H, offsetof(SegmentHeader, HeaderHash));
  }

  uint64_t HashBlock(const SegmentBlock& B, const std::byte* Code, const CPU::Relocation* Relocs) {
    XXH3_state_t State;
    XXH3_64bits_reset(&State);
    XXH3_64bits_update(&State, &B, offsetof(SegmentBlock, EntryHash));
    XXH3_64bits_update(&State, Code, B.CodeSize);
    XXH3_64bits_update(&State, Relocs, static_cast<size_t>(B.RelocCount) * RelocSize);
    return XXH3_64bits_digest(&State);
  }

  // Bytes of the code a relocation rewrites.
  uint64_t RelocWidth(const CPU::Relocation& Reloc) {
    switch (Reloc.Header.Type) {
    case CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL:
    case CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL:
    case CPU::RelocationTypes::RELOC_LINK_RECORD: return sizeof(uint64_t);
    case CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE:
      if (Reloc.GuestRIP.Instructions != 0) {
        return uint64_t {Reloc.GuestRIP.Instructions} * 4;
      }
      [[fallthrough]];
    default: return PPC64Emitter::Emitter::LoadConstantFixedBytes;
    }
  }

  // Guest-memory read that cannot fault.
  bool ReadGuest(uint64_t Address, std::byte* Out, size_t Size) {
    const struct iovec Local {.iov_base = Out, .iov_len = Size};
    const struct iovec Remote {.iov_base = reinterpret_cast<void*>(Address), .iov_len = Size};
    return ::process_vm_readv(::getpid(), &Local, 1, &Remote, 1, 0) == static_cast<ssize_t>(Size);
  }

  // Everything one segment holds, before it is written.
  struct SegmentBuilder {
    fextl::vector<SegmentBlock> Blocks;
    fextl::vector<CPU::Relocation> Relocs;
    fextl::vector<std::byte> Code;
  };

  // Small buffered writer: segments are written in one pass.
  struct BufferedWriter {
    int FD;
    fextl::vector<std::byte> Buffer;
    uint64_t Written = 0;
    bool Ok = true;

    explicit BufferedWriter(int FD_)
      : FD {FD_} {
      Buffer.reserve(1 << 20);
    }
    void Put(const void* Data, size_t Size) {
      Written += Size;
      if (!Ok) {
        return;
      }
      if (Buffer.size() + Size > Buffer.capacity()) {
        Flush();
        if (Size >= Buffer.capacity()) {
          Ok = WriteAll(FD, Data, Size);
          return;
        }
      }
      const auto* Bytes = reinterpret_cast<const std::byte*>(Data);
      Buffer.insert(Buffer.end(), Bytes, Bytes + Size);
    }
    void PadTo(uint64_t Alignment) {
      static constexpr std::array<std::byte, 64> Zero {};
      while (Written % Alignment) {
        Put(Zero.data(), std::min<uint64_t>(Alignment - Written % Alignment, Zero.size()));
      }
    }
    bool Flush() {
      if (Ok && !Buffer.empty()) {
        Ok = WriteAll(FD, Buffer.data(), Buffer.size());
      }
      Buffer.clear();
      return Ok;
    }
  };

  // Layout shared by the writer and the compactor: returns the header for a
  // segment with the given counts.
  SegmentHeader MakeHeader(uint64_t ConfigId, uint64_t FileId, uint32_t NumBlocks, uint32_t NumRelocs, uint64_t CodeSize) {
    SegmentHeader H {};
    H.Magic = SegmentMagic;
    H.Version = SegmentVersion;
    H.ConfigId = ConfigId;
    H.FileId = FileId;
    std::ranges::copy(GIT_HASH, H.BuildHash.begin());
    H.EmulatorId = EmulatorBuildId();
    H.BootId = CurrentBootId();
    H.NumBlocks = NumBlocks;
    H.NumRelocs = NumRelocs;
    H.IndexOffset = sizeof(SegmentHeader);
    H.RelocOffset = AlignUp(H.IndexOffset + uint64_t {NumBlocks} * sizeof(SegmentBlock), 8);
    H.CodeOffset = AlignUp(H.RelocOffset + uint64_t {NumRelocs} * RelocSize, BlockAlignment);
    H.CodeSize = CodeSize;
    H.HeaderHash = HashHeader(H);
    return H;
  }

  bool WriteSegment(int FD, const SegmentBuilder& B, uint64_t ConfigId, uint64_t FileId) {
    if (B.Blocks.size() > std::numeric_limits<uint32_t>::max() || B.Relocs.size() > std::numeric_limits<uint32_t>::max()) {
      return false;
    }
    const auto H = MakeHeader(ConfigId, FileId, B.Blocks.size(), B.Relocs.size(), B.Code.size());
    BufferedWriter Out {FD};
    Out.Put(&H, sizeof(H));
    Out.Put(B.Blocks.data(), B.Blocks.size() * sizeof(SegmentBlock));
    Out.PadTo(8);
    Out.Put(B.Relocs.data(), B.Relocs.size() * RelocSize);
    Out.PadTo(BlockAlignment);
    Out.Put(B.Code.data(), B.Code.size());
    return Out.Flush() && Out.Written == H.CodeOffset + H.CodeSize;
  }

  // Writes to a unique temp name next to Base. Returns the temp path, or empty.
  fextl::string WriteTempSegment(const fextl::string& Base, const std::function<bool(int)>& Writer) {
    static std::atomic<uint64_t> Counter {0};
    auto Temp = fextl::fmt::format("{}.tmp.{}.{}", Base, ::getpid(), Counter.fetch_add(1));
    int FD = ::open(Temp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
    if (FD == -1) {
      return {};
    }
    const bool Ok = Writer(FD);
    ::close(FD);
    if (!Ok) {
      ::unlink(Temp.c_str());
      return {};
    }
    return Temp;
  }
} // namespace


// =============================================================================
// Two tiers: a hot copy in RAM over the durable copy on disk
//
// The working cache is the hot tier: every segment a process maps, appends to
// or compacts lives there, so a warm session reads and writes RAM (a tmpfs)
// and touches the NVMe only to seed and to write back. The durable tier is the
// copy that survives a reboot.
//
//   seed        lazily, per namespace, the first time a process opens one: the
//               segments the durable tier has and the hot tier has not are
//               copied up, through a temp file and link(2), so two processes
//               racing cost one wasted copy and never half a file. Nothing is
//               copied at startup, and a namespace no process opens is never
//               copied at all.
//   write-back  after a segment is published or a namespace compacted, in the
//               forked writer -- the guest thread never waits for the disk. A
//               segment is copied only if it validates (magic, version, header
//               hash, and a file long enough for the extent the header
//               declares), and it lands with rename(2). So a torn or truncated
//               hot copy cannot replace a good durable one, and a reader of the
//               durable tier never sees a partial file.
//
// Losing the hot tier -- a reboot, a wipe, its own size sweep -- costs a
// re-seed and nothing else. The tiers are allowed to disagree, the hot one
// being the newer: a block the durable tier is missing is compiled once more in
// the session after the reboot, and every block either tier holds is validated
// on install exactly as before.
//
// POWERARM_CODECACHEHOTTIER=0 turns it off and leaves one tier, the durable
// one, exactly as it was.
// =============================================================================
namespace {
  // Copies Size bytes from In to Out, from wherever both offsets stand.
  bool CopyFileContents(int In, int Out, uint64_t Size) {
    while (Size) {
      const ssize_t Done = ::copy_file_range(In, nullptr, Out, nullptr, Size, 0);
      if (Done > 0) {
        Size -= static_cast<uint64_t>(Done);
        continue;
      }
      if (Done == 0) {
        return false;
      }
      if (errno == EINTR) {
        continue;
      }
      // A kernel or a filesystem pair that refuses it: finish by hand.
      break;
    }
    fextl::vector<std::byte> Buffer;
    while (Size) {
      if (Buffer.empty()) {
        Buffer.resize(1 << 20);
      }
      const ssize_t Got = ::read(In, Buffer.data(), std::min<uint64_t>(Size, Buffer.size()));
      if (Got <= 0) {
        if (Got < 0 && errno == EINTR) {
          continue;
        }
        return false;
      }
      if (!WriteAll(Out, Buffer.data(), static_cast<size_t>(Got))) {
        return false;
      }
      Size -= static_cast<uint64_t>(Got);
    }
    return true;
  }

  // A segment file that is whole: it starts with a header this build wrote in
  // this format and the file covers everything that header describes. What this
  // rejects is a file that was truncated or is still being written -- the state
  // a crashed or killed writer leaves behind. What is inside is not checked
  // here; every block is validated when it is installed.
  bool SegmentLooksComplete(const fextl::string& Path, uint64_t* SizeOut) {
    const int FD = ::open(Path.c_str(), O_RDONLY | O_CLOEXEC);
    if (FD == -1) {
      return false;
    }
    SegmentHeader H {};
    struct stat St {};
    const bool Ok = ::pread(FD, &H, sizeof(H), 0) == static_cast<ssize_t>(sizeof(H)) && ::fstat(FD, &St) == 0 && S_ISREG(St.st_mode) &&
                    H.Magic == SegmentMagic && H.Version == SegmentVersion && H.HeaderHash == HashHeader(H) &&
                    static_cast<uint64_t>(St.st_size) >= H.CodeOffset + H.CodeSize;
    ::close(FD);
    if (Ok && SizeOut) {
      *SizeOut = static_cast<uint64_t>(St.st_size);
    }
    return Ok;
  }

  // The hot cache directory, or empty when there is one tier only. Resolved
  // once, from the durable directory of the first namespace this process names:
  //
  //   POWERARM_CODECACHEHOTLOCATION  where the caller says;
  //   a chosen cache location        `hot/` beside its `cache/`, so a test
  //                                  harness with its own cache directory has
  //                                  its own hot tier too and two runs never
  //                                  share one;
  //   otherwise                      $XDG_RUNTIME_DIR/powerarm/cache, falling
  //                                  back to /tmp/powerarm-<uid>/cache -- both
  //                                  tmpfs on this machine, and both emptied by
  //                                  a reboot, which is all the hot tier needs.
  fextl::string ResolveHotCacheDir(std::string_view DurableDir) {
    if (!FEXCore::Config::Get_CODECACHEHOTTIER()()) {
      return {};
    }
    fextl::string Dir;
    bool Shared = false;
    if (const auto& Configured = FEXCore::Config::Get_CODECACHEHOTLOCATION()(); !Configured.empty()) {
      Dir = Configured;
    } else if (::getenv("POWERARM_APP_CACHE_LOCATION") || ::getenv("FEX_APP_CACHE_LOCATION")) {
      const auto Parent = std::filesystem::path(DurableDir).parent_path().string();
      Dir = fextl::fmt::format("{}/hot", Parent);
    } else if (const char* Runtime = ::getenv("XDG_RUNTIME_DIR"); Runtime && Runtime[0] == '/') {
      Dir = fextl::fmt::format("{}/powerarm/cache", Runtime);
    } else {
      Dir = fextl::fmt::format("/tmp/powerarm-{}/cache", static_cast<unsigned>(::getuid()));
      Shared = true;
    }
    while (!Dir.empty() && Dir.back() == '/') {
      Dir.pop_back();
    }
    if (Dir.empty() || Dir == DurableDir) {
      return {};
    }
    std::error_code EC;
    std::filesystem::create_directories(std::filesystem::path(std::string_view {Dir}), EC);
    if (Shared) {
      // A guessable name under a directory anyone can write. A cache file is
      // only ever as trustworthy as its directory -- every block in it is
      // checked against the guest's bytes, but the host code beside them is
      // executed as written -- so this one is used only while it is ours and
      // nobody else's to write.
      const auto Parent = fextl::string {std::filesystem::path(std::string_view {Dir}).parent_path().string()};
      ::chmod(Parent.c_str(), 0700);
      struct stat St {};
      if (::lstat(Parent.c_str(), &St) != 0 || !S_ISDIR(St.st_mode) || St.st_uid != ::getuid() || (St.st_mode & (S_IWGRP | S_IWOTH))) {
        LogMan::Msg::IFmt("Code cache: no hot tier, {} is not ours alone", Parent);
        return {};
      }
    }
    if (::access(Dir.c_str(), R_OK | W_OK | X_OK) != 0) {
      LogMan::Msg::IFmt("Code cache: no hot tier, {} is not usable", Dir);
      return {};
    }
    return Dir;
  }

  // The hot path of a durable base path, or empty with one tier.
  fextl::string HotBasePath(const fextl::string& DurableBase) {
    const auto Slash = DurableBase.rfind('/');
    if (DurableBase.empty() || Slash == fextl::string::npos) {
      return {};
    }
    static const fextl::string Dir = ResolveHotCacheDir(std::string_view {DurableBase}.substr(0, Slash));
    if (Dir.empty()) {
      return {};
    }
    return fextl::fmt::format("{}/{}", Dir, std::string_view {DurableBase}.substr(Slash + 1));
  }

  // Copies the durable segments of a namespace the hot tier has not got. Stops
  // at the first name neither tier has: the reader stops there too.
  void SeedHotNamespace(const fextl::string& HotBase, const fextl::string& DurableBase) {
    if (HotBase.empty() || DurableBase.empty()) {
      return;
    }
    for (size_t Index = 0; Index < MaxSegments; ++Index) {
      const auto Hot = SegmentPath(HotBase, Index);
      if (::access(Hot.c_str(), F_OK) == 0) {
        continue;
      }
      uint64_t Size = 0;
      if (!SegmentLooksComplete(SegmentPath(DurableBase, Index), &Size)) {
        return;
      }
      const int In = ::open(SegmentPath(DurableBase, Index).c_str(), O_RDONLY | O_CLOEXEC);
      if (In == -1) {
        return;
      }
      std::error_code EC;
      std::filesystem::create_directories(std::filesystem::path(std::string_view {HotBase}).parent_path(), EC);
      auto Temp = WriteTempSegment(HotBase, [&](int FD) { return CopyFileContents(In, FD, Size); });
      ::close(In);
      if (Temp.empty()) {
        return;
      }
      // A sibling that seeded the same name first wins; this copy is dropped.
      const bool Have = ::link(Temp.c_str(), Hot.c_str()) == 0 || errno == EEXIST;
      ::unlink(Temp.c_str());
      if (!Have) {
        return;
      }
    }
  }

  // Mirrors one hot segment into the durable tier, whole or not at all.
  bool WriteBackSegment(const fextl::string& HotBase, const fextl::string& DurableBase, size_t Index) {
    uint64_t Size = 0;
    if (!SegmentLooksComplete(SegmentPath(HotBase, Index), &Size)) {
      return false;
    }
    const int In = ::open(SegmentPath(HotBase, Index).c_str(), O_RDONLY | O_CLOEXEC);
    if (In == -1) {
      return false;
    }
    std::error_code EC;
    std::filesystem::create_directories(std::filesystem::path(std::string_view {DurableBase}).parent_path(), EC);
    auto Temp = WriteTempSegment(DurableBase, [&](int FD) { return CopyFileContents(In, FD, Size); });
    ::close(In);
    if (Temp.empty()) {
      return false;
    }
    // rename(2), not link(2): this replaces whatever the durable tier had under
    // that name, and it replaces it in one step.
    if (::rename(Temp.c_str(), SegmentPath(DurableBase, Index).c_str()) != 0) {
      ::unlink(Temp.c_str());
      return false;
    }
    return true;
  }

  // Mirrors a whole namespace, which is what a compaction leaves to do: the hot
  // tier is one segment again and the durable tier must lose the names it no
  // longer has. The new segment 0 goes first, so a process seeding in between
  // finds a namespace that is complete, if for a moment redundant.
  void WriteBackNamespace(const fextl::string& HotBase, const fextl::string& DurableBase) {
    size_t Present = 0;
    for (; Present < MaxSegments; ++Present) {
      if (::access(SegmentPath(HotBase, Present).c_str(), F_OK) != 0) {
        break;
      }
      WriteBackSegment(HotBase, DurableBase, Present);
    }
    for (size_t Index = MaxSegments; Index-- > Present;) {
      ::unlink(SegmentPath(DurableBase, Index).c_str());
    }
  }
} // namespace

struct CodeCache::CacheSegment {
  void* Map {};
  size_t MapSize {};
  const SegmentHeader* Header {};
  const SegmentBlock* Blocks {};
  const CPU::Relocation* Relocs {};
  const std::byte* Code {};
  // Entry hashes need checking (written in another boot, or forced).
  bool CheckHashes = true;

  CacheSegment() = default;
  CacheSegment(const CacheSegment&) = delete;
  CacheSegment& operator=(const CacheSegment&) = delete;
  ~CacheSegment() {
    if (Map) {
      FEXCore::Allocator::munmap(Map, MapSize);
    }
  }

  const SegmentBlock* Find(uint64_t GuestOffset) const {
    const auto* End = Blocks + Header->NumBlocks;
    const auto* It = std::lower_bound(Blocks, End, GuestOffset, [](const SegmentBlock& B, uint64_t Off) { return B.GuestOffset < Off; });
    return (It != End && It->GuestOffset == GuestOffset) ? It : nullptr;
  }

  // Structural bounds plus the integrity hash. Relocation extents are checked
  // again by ApplyCodeRelocations when they are used.
  bool Validate(const SegmentBlock& B) const {
    if (B.CodeOffset > Header->CodeSize || B.CodeSize > Header->CodeSize - B.CodeOffset || B.CodeSize % BlockAlignment != 0 ||
        B.ColdSize > B.CodeSize || B.ColdSize % 16 != 0 ||
        (B.CodeSize - B.ColdSize) < sizeof(CPU::CPUBackend::JITCodeHeader) + 4 ||
        B.ColdSize < sizeof(CPU::CPUBackend::JITCodeTail) ||
        B.EntryOffset >= (B.CodeSize - B.ColdSize)) {
      return false;
    }
    if (B.RelocBegin > Header->NumRelocs || B.RelocCount > Header->NumRelocs - B.RelocBegin) {
      return false;
    }
    return !CheckHashes || HashBlock(B, Code + B.CodeOffset, Relocs + B.RelocBegin) == B.EntryHash;
  }

  // MarkUsed: record a use for the size sweep's LRU order by refreshing the
  // file's mtime, at most every UseStampSeconds per file.
  static fextl::unique_ptr<CacheSegment> Open(const fextl::string& Path, uint64_t ConfigId, uint64_t FileId, bool MarkUsed = false) {
    int FD = ::open(Path.c_str(), O_RDONLY | O_CLOEXEC);
    if (FD == -1) {
      return nullptr;
    }
    struct stat St {};
    if (::fstat(FD, &St) != 0 || St.st_size < static_cast<off_t>(sizeof(SegmentHeader))) {
      ::close(FD);
      return nullptr;
    }
    if (MarkUsed && St.st_mtime + UseStampSeconds < ::time(nullptr)) {
      // Fails harmlessly on a read-only cache.
      ::utimensat(AT_FDCWD, Path.c_str(), nullptr, 0);
    }
    const size_t Size = static_cast<size_t>(St.st_size);
    void* Map = FEXCore::Allocator::mmap(nullptr, Size, PROT_READ, MAP_PRIVATE, FD, 0);
    ::close(FD);
    if (Map == MAP_FAILED || Map == nullptr) {
      return nullptr;
    }
    auto Seg = fextl::make_unique<CacheSegment>();
    Seg->Map = Map;
    Seg->MapSize = Size;
    const auto* H = reinterpret_cast<const SegmentHeader*>(Map);
    // Every count and offset below comes from a file this process does not
    // control; bound each against the mapping before it is used.
    if (H->Magic != SegmentMagic || H->Version != SegmentVersion || H->ConfigId != ConfigId || H->FileId != FileId ||
        !std::ranges::equal(H->BuildHash, GIT_HASH) || H->HeaderHash != HashHeader(*H) || H->IndexOffset != sizeof(SegmentHeader) ||
        H->NumBlocks > (Size - H->IndexOffset) / sizeof(SegmentBlock) || H->RelocOffset % 8 != 0 || H->RelocOffset > Size ||
        H->NumRelocs > (Size - H->RelocOffset) / RelocSize || H->CodeOffset % BlockAlignment != 0 || H->CodeOffset > Size ||
        H->CodeSize > Size - H->CodeOffset) {
      LogMan::Msg::IFmt("Code cache: ignoring invalid or foreign segment {}", Path);
      return nullptr;
    }
    Seg->Header = H;
    Seg->Blocks = reinterpret_cast<const SegmentBlock*>(static_cast<const std::byte*>(Map) + H->IndexOffset);
    Seg->Relocs = reinterpret_cast<const CPU::Relocation*>(static_cast<const std::byte*>(Map) + H->RelocOffset);
    Seg->Code = static_cast<const std::byte*>(Map) + H->CodeOffset;
    const auto& Boot = CurrentBootId();
    Seg->CheckHashes = ForceVerify() || H->BootId != Boot || Boot == std::array<uint8_t, 16> {};
    return Seg;
  }
};

struct CodeCache::FileCache {
  // The working namespace: the hot tier's when there is one, the durable
  // tier's when there is not.
  fextl::string BasePath;
  // The durable namespace, empty with one tier. Set even when the hot tier is
  // in use: it is where this namespace is seeded from and written back to.
  fextl::string DurableBase;
  // Append-only. Appended under CodeCache::RegistryMutex (unique); read without
  // a lock: a slot is filled before NumSegments counts it.
  std::array<fextl::unique_ptr<CacheSegment>, MaxSegments> Segments;
  std::atomic<size_t> NumSegments {0};

  std::span<const fextl::unique_ptr<CacheSegment>> Loaded() const {
    return {Segments.data(), NumSegments.load(std::memory_order_acquire)};
  }
  std::atomic<uint64_t> LastProbeMS {0};

  bool Contains(uint64_t GuestOffset) const {
    for (const auto& Seg : Loaded()) {
      if (Seg->Find(GuestOffset)) {
        return true;
      }
    }
    return false;
  }

  // Opens segments this process has not seen yet, stopping at the first gap.
  void ProbeNewSegments(uint64_t ConfigId, uint64_t FileId) {
    while (NumSegments.load(std::memory_order_relaxed) < MaxSegments) {
      const size_t Index = NumSegments.load(std::memory_order_relaxed);
      auto Seg = CacheSegment::Open(SegmentPath(BasePath, Index), ConfigId, FileId, Index == 0);
      if (!Seg) {
        break;
      }
      Segments[Index] = std::move(Seg);
      NumSegments.store(Index + 1, std::memory_order_release);
    }
  }
};

CodeCache::CodeCache(ContextImpl& CTX_)
  : CTX(CTX_) {
  // Loading installs blocks through the same registration a compile uses, but
  // not the per-block metadata these SMC modes attach at compile time (semantic
  // patch sites, cheap-tier state, store emulation), so they and the cache are
  // mutually exclusive.
  LoadEnabled = EnableCodeCaching() && !FEXCore::Config::Get_SMCSEMANTICPATCH() && !FEXCore::Config::Get_SMCLAZYINVAL() &&
                !FEXCore::Config::Get_SMCCHEAPTIER() && !FEXCore::Config::Get_SMCSTOREEMULATION() &&
                !FEXCore::Config::Get_SMCSTOREBACKPATCH();
  StatsStream();
}
CodeCache::~CodeCache() = default;

uint64_t CodeCache::ComputeCodeMapId(std::string_view Filename, int FD) {
  if (Filename.empty()) {
    return InvalidFileId;
  }

  // Identity from metadata, not content. This used to stream-hash the whole
  // file on every executable mmap, in every process, whether or not caching
  // was enabled: 2 % of `gcc -c empty.c` (cc1 alone is tens of MB). Soundness
  // does not rest on this id (see the format comment above: every cached block
  // is checked against the guest bytes it was translated from), so a cheap id
  // that changes whenever the file is replaced or rewritten is enough.
  XXH3_state_t State;
  XXH3_64bits_reset(&State);
  struct stat St {};
  if (FD >= 0 && ::fstat(FD, &St) == 0) {
    const uint64_t Fields[] = {
      static_cast<uint64_t>(St.st_dev),        static_cast<uint64_t>(St.st_ino),         static_cast<uint64_t>(St.st_size),
      static_cast<uint64_t>(St.st_mtim.tv_sec), static_cast<uint64_t>(St.st_mtim.tv_nsec), static_cast<uint64_t>(St.st_ctim.tv_sec),
      static_cast<uint64_t>(St.st_ctim.tv_nsec),
    };
    XXH3_64bits_update(&State, Fields, sizeof(Fields));
  } else {
    XXH3_64bits_update(&State, Filename.data(), Filename.size());
  }
  return SanitizeId(XXH3_64bits_digest(&State));
}

// Counters of the segment writers this process forks, in a page it shares
// with them. See "Publishing a pass's segments, and the forked writer".
struct CodeCache::SaveWriterStats {
  std::atomic<uint64_t> SavedBlocks;
  std::atomic<uint64_t> SavedSegments;
  std::atomic<uint64_t> Compactions;
  // Segments a writer could not publish: the namespace lock stayed busy for
  // PublishLockWaitSeconds, or the temp file could not be written.
  std::atomic<uint64_t> LostSegments;
  // Writers running now. A writer killed outright cannot decrement it, so it is
  // only believed for as long as a writer can live.
  std::atomic<int32_t> Active;
};

bool CodeCache::WantsSave(bool IgnoreInterval) {
  // Two independent triggers so neither a burst of compilation nor a long quiet
  // stretch can leave an unbounded amount of work unsaved.
  // Exit and execve save everything anyway; these only bound what a process
  // killed by a signal loses. Each save is a segment, and segments cost a
  // compaction every MaxSegments, so keep them coarse.
  constexpr uint64_t BlocksPerSave = 50000;
  constexpr uint64_t SecondsPerSave = 60;

  if (!IsGeneratingCache) {
    return false;
  }
  const uint64_t Blocks = BlocksSinceSave.load(std::memory_order_relaxed);
  if (Blocks == 0) {
    if (!IgnoreInterval) {
      return false;
    }
    // Blocks an earlier pass kept for later still need the final pass.
    std::lock_guard lk {RelocationSinkMutex};
    return !CompiledBlocks.empty();
  }
  if (IgnoreInterval || Blocks >= BlocksPerSave) {
    return true;
  }

  const uint64_t Now = MonotonicSeconds();
  uint64_t Last = LastSaveTimeSeconds.load(std::memory_order_relaxed);
  if (Last == 0) {
    LastSaveTimeSeconds.compare_exchange_strong(Last, Now, std::memory_order_relaxed);
    return false;
  }
  return Now >= Last + SecondsPerSave;
}

void CodeCache::NotifyCachesSaved() {
  BlocksSinceSave.store(0, std::memory_order_relaxed);
  LastSaveTimeSeconds.store(MonotonicSeconds(), std::memory_order_relaxed);
}

void CodeCache::ResetAfterFork() {
  // Runs in the child with a single thread. Every lock here is only ever taken
  // while holding either CodeInvalidationMutex (shared, on the compile/load path)
  // or the code-cache SaveIOLock (on the save pass); fork holds both exclusively
  // before snapshotting (LockBeforeFork), so none can be held by a thread that
  // did not survive the fork.
  BlocksSinceSave.store(0, std::memory_order_relaxed);
  RanPeriodicPass.store(false, std::memory_order_relaxed);
  // The writers this process forked report to a page it shares with them; the
  // child gets a page of its own, so its stats line describes the child alone
  // (and it cannot decrement an Active count it never incremented).
  if (WriterStats) {
    ::munmap(WriterStats, sizeof(SaveWriterStats));
    WriterStats = nullptr;
  }
  LastWriterForkSeconds = 0;
  {
    std::lock_guard lk {RelocationSinkMutex};
    CompiledBlocks.clear();
    RelocationSink.clear();
    ++SinkGeneration;
  }
  // The child's counters start at zero, so its line describes the child alone.
  for (auto* Counter : {&Stats.Loaded, &Stats.NotInIndex, &Stats.NoFile, &Stats.BadEntry, &Stats.GuestMismatch, &Stats.NotExecutable,
                        &Stats.RelocFailed, &Stats.SavedBlocks, &Stats.SavedSegments, &Stats.Compactions, &Stats.SaveNS, &Stats.LoadNS}) {
    Counter->store(0, std::memory_order_relaxed);
  }
}

void CodeCache::DumpStats() {
  auto L = [](const std::atomic<uint64_t>& A) {
    return A.load(std::memory_order_relaxed);
  };
  // What this process's forked writers have published so far. They report into
  // a shared page, so their work is this process's, wherever it ran; a writer
  // still running when this line is printed is not in it.
  uint64_t WrittenBlocks = 0, WrittenSegments = 0, WriterCompactions = 0, LostSegments = 0;
  if (WriterStats) {
    WrittenBlocks = L(WriterStats->SavedBlocks);
    WrittenSegments = L(WriterStats->SavedSegments);
    WriterCompactions = L(WriterStats->Compactions);
    LostSegments = L(WriterStats->LostSegments);
  }
  // The private copy of stderr, unless the guest has since put something else
  // on that descriptor number.
  const auto& Out = StatsStream();
  int FD = STDERR_FILENO;
  struct stat St {};
  if (Out.FD >= 0 && ::fstat(Out.FD, &St) == 0 && St.st_dev == Out.Dev && St.st_ino == Out.Ino) {
    FD = Out.FD;
  }
  const auto Line = fextl::fmt::format(
    "POWERarm code cache [{}]: loaded {} not-in-index {} no-file {} bad-entry {} guest-mismatch {} not-exec {} "
    "reloc-failed {} saved {} blocks in {} segments, {} compactions, {} lost; save-ms {} lookup-ms {}\n",
    ::getpid(), L(Stats.Loaded), L(Stats.NotInIndex), L(Stats.NoFile), L(Stats.BadEntry), L(Stats.GuestMismatch), L(Stats.NotExecutable),
    L(Stats.RelocFailed), L(Stats.SavedBlocks) + WrittenBlocks, L(Stats.SavedSegments) + WrittenSegments,
    L(Stats.Compactions) + WriterCompactions, LostSegments, L(Stats.SaveNS) / 1000000, L(Stats.LoadNS) / 1000000);
  (void)::write(FD, Line.data(), Line.size());
}

void CodeCache::AbsorbRelocations(Core::InternalThreadState& Thread, uint64_t GuestRIP) {
  // Base 0: the sink keeps absolute guest RIPs and each save rebases its own
  // copy. TakeRelocations rebases in place and is not idempotent.
  auto New = Thread.CPUBackend->TakeRelocations(0);
  std::lock_guard lk {RelocationSinkMutex};
  const uint64_t Begin = RelocationSink.size();
  RelocationSink.insert(RelocationSink.end(), New.begin(), New.end());
  CompiledBlocks.push_back({GuestRIP, Begin, RelocationSink.size()});
}

void CodeCache::ResetRelocations() {
  std::lock_guard lk {RelocationSinkMutex};
  RelocationSink.clear();
  CompiledBlocks.clear();
  ++SinkGeneration;
}

CodeCache::FileCache* CodeCache::GetFileCache(const ExecutableFileInfo& FileInfo) {
  // FileCaches are never freed, so a per-thread memo of the last one needs no
  // lock. Consecutive misses are almost always in the same file.
  thread_local const CodeCache* MemoCache = nullptr;
  thread_local uint64_t MemoFileId = 0;
  thread_local FileCache* MemoFile = nullptr;
  if (MemoCache == this && MemoFileId == FileInfo.FileId && MemoFile) {
    return MemoFile;
  }
  auto Remember = [&](FileCache* File) {
    MemoCache = this;
    MemoFileId = FileInfo.FileId;
    MemoFile = File;
    return File;
  };
  {
    std::shared_lock lk {RegistryMutex};
    auto It = Registry.find(FileInfo.FileId);
    if (It != Registry.end()) {
      return Remember(It->second.get());
    }
  }
  // First sight of this file in this process: resolve its scope and path once.
  // An out-of-scope file is remembered with an empty BasePath.
  auto DurableBase = CTX.SyscallHandler->CodeCacheBasePath(FileInfo);
  auto BasePath = HotBasePath(DurableBase);
  // Seed before the registry lock: it is one copy up from disk, once per
  // namespace per process, and every other thread's first touch of every other
  // file waits behind that lock. Two processes seeding the same namespace at
  // once is handled by the link(2) in there, not by a lock.
  if (BasePath.empty()) {
    BasePath = std::move(DurableBase);
    DurableBase.clear();
  } else {
    SeedHotNamespace(BasePath, DurableBase);
  }
  std::unique_lock lk {RegistryMutex};
  auto& Slot = Registry[FileInfo.FileId];
  if (!Slot) {
    Slot = fextl::make_unique<FileCache>();
    Slot->BasePath = std::move(BasePath);
    Slot->DurableBase = std::move(DurableBase);
    if (!Slot->BasePath.empty()) {
      Slot->ProbeNewSegments(ComputeCodeCacheConfigId(), FileInfo.FileId);
    }
  }
  return Remember(Slot.get());
}

std::optional<CodeCache::LoadedBlock> CodeCache::TryLoadBlock(Core::InternalThreadState* Thread, uint64_t GuestRIP) {
  auto* SyscallHandler = CTX.SyscallHandler;
  if (!LoadEnabled || !SyscallHandler || !Thread) {
    return std::nullopt;
  }
  const auto Section = SyscallHandler->LookupExecutableFileSection(Thread, GuestRIP);
  if (!Section || GuestRIP < Section->FileStartVA) {
    return std::nullopt;
  }
  auto* File = GetFileCache(Section->FileInfo);
  if (File->BasePath.empty()) {
    return std::nullopt;
  }
  std::optional<ScopedNS> Timer;
  if (StatsTimersEnabled()) [[unlikely]] {
    Timer.emplace(Stats.LoadNS);
  }


  const uint64_t GuestOffset = GuestRIP - Section->FileStartVA;
  const CacheSegment* Seg = nullptr;
  const SegmentBlock* Block = nullptr;
  bool Found = false;
  auto Lookup = [&]() {
    for (const auto& Candidate : File->Loaded()) {
      if (auto* Entry = Candidate->Find(GuestOffset)) {
        Found = true;
        if (Candidate->Validate(*Entry)) {
          Seg = Candidate.get();
          Block = Entry;
          return;
        }
      }
    }
  };
  Lookup();
  if (!Block && !Found && File->NumSegments.load(std::memory_order_relaxed) < MaxSegments) {
    // Another process (often a sibling from the same parent, which inherited
    // this registry) may have written the block since this file was probed.
    // Looking for a new segment costs one failed open(2); rate-limit it.
    const uint64_t Now = MonotonicMilliseconds();
    if (Now - File->LastProbeMS.load(std::memory_order_relaxed) >= 100) {
      std::unique_lock lk {RegistryMutex};
      File->LastProbeMS.store(Now, std::memory_order_relaxed);
      const size_t Before = File->NumSegments.load(std::memory_order_relaxed);
      File->ProbeNewSegments(ComputeCodeCacheConfigId(), Section->FileInfo.FileId);
      if (File->NumSegments.load(std::memory_order_relaxed) != Before) {
        Lookup();
      }
    }
  }
  if (!Block) {
    (Found ? Stats.BadEntry : File->NumSegments.load(std::memory_order_relaxed) == 0 ? Stats.NoFile : Stats.NotInIndex).fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
  }

  // The guest bytes. A64 instructions are 4 bytes, and the decoder bounds a
  // block to MaxInst of them.
  const uint64_t Length = Block->GuestLength;
  if (Length == 0 || Length % 4 != 0 || Length > uint64_t {FEXCore::A64::DEFAULT_MAX_INSTRUCTIONS} * 4 ||
      GuestRIP + Length < GuestRIP) {
    return std::nullopt;
  }
  for (uint64_t Address = GuestRIP; Address < GuestRIP + Length;) {
    // Executable in this process now, exactly as the decoder requires.
    const auto Range = SyscallHandler->QueryGuestExecutableRange(Thread, Address);
    if (Range.Size == 0 || Address < Range.Base || Address - Range.Base >= Range.Size) {
      Stats.NotExecutable.fetch_add(1, std::memory_order_relaxed);
      return std::nullopt;
    }
    Address = Range.Base + Range.Size;
  }
  for (uint64_t Page = GuestRIP & FEXCore::Utils::FEX_GUEST_PAGE_MASK; Page < GuestRIP + Length; Page += FEXCore::Utils::FEX_GUEST_PAGE_SIZE) {
    // A demoted mixed code/data granule needs per-instruction guards that a
    // cached block does not carry.
    if (SyscallHandler->GuestCodePageValidateOnly(Page)) {
      Stats.NotExecutable.fetch_add(1, std::memory_order_relaxed);
      return std::nullopt;
    }
  }
  // Arm SMC write protection on the block's pages BEFORE hashing the guest
  // bytes. The caller holds CodeInvalidationMutex shared, so a write that lands
  // after this point faults and waits for the invalidation until the block is
  // registered; a write that landed before it is seen by the hash. Hashing
  // first would leave a window in which a write the hash never saw is not
  // tracked either. (This is registration CompileBlock does after compiling;
  // for a block that then fails to load it only costs a spurious SMC fault.)
  {
    const fextl::set<uint64_t> EntryPoints {GuestRIP};
    for (uint64_t Page = GuestRIP & FEXCore::Utils::FEX_GUEST_PAGE_MASK; Page < GuestRIP + Length; Page += FEXCore::Utils::FEX_GUEST_PAGE_SIZE) {
      if (Thread->LookupCache->AddBlockExecutableRange(Thread, EntryPoints, Page, FEXCore::Utils::FEX_GUEST_PAGE_SIZE, GuestRIP, Length)) {
        SyscallHandler->MarkGuestExecutableRange(Thread, Page, FEXCore::Utils::FEX_GUEST_PAGE_SIZE);
      }
    }
  }
  if (XXH3_64bits(reinterpret_cast<const void*>(GuestRIP), Length) != Block->GuestHash) {
    Stats.GuestMismatch.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
  }

  auto Lock = std::unique_lock {CTX.CodeBufferWriteMutex};
  if (auto Prev = Thread->CPUBackend->CheckCodeBufferUpdate()) {
    Allocator::VirtualDontNeed(Thread->CallRetStackBase, FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE);
    auto lk = Thread->LookupCache->AcquireWriteLock();
    Thread->LookupCache->ChangeGuestToHostMapping(*Prev, *CTX.GetLatest()->LookupCache, lk);
  }
  auto CodeBuffer = CTX.GetLatest();
  const uint32_t ColdSize = Block->ColdSize;
  const uint32_t HotSize = Block->CodeSize - ColdSize;

  if (!CTX.EnsureHeadroom(HotSize + ColdSize + BlockAlignment)) {
    // Leave rotation to the compiler.
    return std::nullopt;
  }
  const uint64_t HotOffset = AlignUp(CTX.LatestOffset, BlockAlignment);
  if (HotOffset > CTX.ColdOffset || HotSize > CTX.ColdOffset - HotOffset) {
    return std::nullopt;
  }

  auto HotDest = std::as_writable_bytes(std::span {CodeBuffer->Ptr, CodeBuffer->UsableSize()}).subspan(HotOffset, HotSize);
  ::memcpy(HotDest.data(), Seg->Code + Block->CodeOffset, HotSize);

  std::span<std::byte> ColdDest {};
  if (ColdSize > 0) {
    const uint64_t ColdOffset = CTX.AllocateColdThunkBytes(ColdSize);
    ColdDest = std::as_writable_bytes(std::span {CodeBuffer->Ptr, CodeBuffer->UsableSize()}).subspan(ColdOffset, ColdSize);
    ::memcpy(ColdDest.data(), Seg->Code + Block->CodeOffset + HotSize, ColdSize);
  }

  const std::span<const CPU::Relocation> Relocs {Seg->Relocs + Block->RelocBegin, Block->RelocCount};
  if (!ApplyCodeRelocationsSplit(Section->FileStartVA, HotDest, ColdDest, Relocs)) {
    if (ColdSize > 0) {
      CTX.ColdOffset += ColdSize;
    }
    Stats.RelocFailed.fetch_add(1, std::memory_order_relaxed);
    // Nothing references these bytes; LatestOffset is unchanged, so the next
    // compile overwrites them.
    return std::nullopt;
  }

  if (ColdSize < sizeof(CPU::CPUBackend::JITCodeTail)) {
    CTX.ColdOffset += ColdSize;
    return std::nullopt;
  }
  const auto* Tail = reinterpret_cast<const CPU::CPUBackend::JITCodeTail*>(ColdDest.data());
  if (Tail->RIP != GuestRIP || Tail->GuestSize != Length || Tail->Size != HotSize || Tail->ColdSize != ColdSize) {
    CTX.ColdOffset += ColdSize;
    Stats.RelocFailed.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
  }

  FEXCore::ArchHelpers::PPC64::FlushICacheRange(HotDest.data(), HotDest.size_bytes());
  if (ColdSize > 0) {
    FEXCore::ArchHelpers::PPC64::FlushICacheRange(ColdDest.data(), ColdDest.size_bytes());
  }
  CTX.LatestOffset = HotOffset + HotSize;
  // Host-PC -> block index, so signals inside the block resolve like a compile's.
  CodeBuffer->AppendBlock(static_cast<uint32_t>(HotOffset));

  Stats.Loaded.fetch_add(1, std::memory_order_relaxed);
  auto* Begin = reinterpret_cast<uint8_t*>(HotDest.data());
  return LoadedBlock {
    .BlockBegin = Begin,
    .HostCode = Begin + Block->EntryOffset,
    .Size = HotSize,
    .StartAddr = GuestRIP,
    .Length = Length,
  };
}

// Reads guest code pages through process_vm_readv, one page at a time with a
// one-page memo: blocks are serialized in address order, so this is about one
// syscall per code page instead of one per block.
namespace {
  struct GuestPageReader {
    uint64_t CachedPage = ~0ULL;
    bool CachedValid = false;
    std::array<std::byte, 4096> Page;

    bool Read(uint64_t Address, std::byte* Out, size_t Size) {
      while (Size) {
        const uint64_t PageBase = Address & ~uint64_t {4095};
        if (PageBase != CachedPage) {
          CachedPage = PageBase;
          CachedValid = ReadGuest(PageBase, Page.data(), Page.size());
        }
        if (!CachedValid) {
          return false;
        }
        const size_t Off = Address - PageBase;
        const size_t Chunk = std::min<size_t>(Size, Page.size() - Off);
        ::memcpy(Out, Page.data() + Off, Chunk);
        Out += Chunk;
        Address += Chunk;
        Size -= Chunk;
      }
      return true;
    }
  };
} // namespace

// Serializes the live blocks at the given guest entries (sorted, unique) of one
// file. RelocsFor returns the relocations recorded for a block, as
// buffer-relative offsets and absolute guest RIPs; they must all lie inside the
// block's extent or the block is skipped. Blocks already on disk are skipped.
static void CollectLiveBlocks(CodeCache& Cache, ContextImpl& CTX, const ExecutableFileSectionInfo& Section,
                              std::span<const uint64_t> Candidates, const std::function<bool(uint64_t)>& AlreadyCached,
                              const std::function<std::span<const CPU::Relocation>(uint64_t Guest)>& RelocsFor, SegmentBuilder& Out) {
  auto CodeBufferLock = std::unique_lock {CTX.CodeBufferWriteMutex};
  auto CodeBuffer = CTX.GetLatest();
  auto& LookupCache = *CodeBuffer->LookupCache;
  auto ReadLock = LookupCache.AcquireReadLock();
  const uint64_t BufferBase = reinterpret_cast<uintptr_t>(CodeBuffer->Ptr);
  const uint64_t Used = CTX.LatestOffset;

  GuestPageReader Reader;
  fextl::vector<std::byte> GuestBytes;
  for (uint64_t Guest : Candidates) {
    if (Guest < Section.FileStartVA) {
      continue;
    }
    const uint64_t GuestOffset = Guest - Section.FileStartVA;
    if (AlreadyCached(GuestOffset)) {
      continue;
    }
    auto It = LookupCache.BlockList.find(Guest);
    if (It == LookupCache.BlockList.end()) {
      // Invalidated since it was compiled.
      continue;
    }
    const auto& Entry = It->second;
    if (Entry.BlockBegin < BufferBase || Entry.BlockBegin - BufferBase >= Used) {
      continue;
    }
    const uint64_t Begin = Entry.BlockBegin - BufferBase;
    if (Used - Begin < sizeof(CPU::CPUBackend::JITCodeHeader)) {
      continue;
    }
    const auto* Header = reinterpret_cast<const CPU::CPUBackend::JITCodeHeader*>(BufferBase + Begin);
    const uint64_t ColdBaseOffset = Begin + Header->OffsetToBlockTail;
    if (ColdBaseOffset + sizeof(CPU::CPUBackend::JITCodeTail) > CodeBuffer->UsableSize()) {
      continue;
    }
    const auto* Tail = reinterpret_cast<const CPU::CPUBackend::JITCodeTail*>(BufferBase + ColdBaseOffset);
    const uint64_t Size = Tail->Size;
    const uint64_t ColdSize = Tail->ColdSize;
    const uint64_t Length = Tail->GuestSize;
    if (Tail->RIP != Guest || Size > Used - Begin || Size % BlockAlignment != 0 ||
        ColdSize < sizeof(CPU::CPUBackend::JITCodeTail) || ColdSize % 16 != 0 ||
        ColdBaseOffset + ColdSize > CodeBuffer->UsableSize() ||
        Entry.HostCode < Entry.BlockBegin || Entry.HostCode - Entry.BlockBegin >= Size || Length == 0 || Length % 4 != 0 ||
        Length > uint64_t {FEXCore::A64::DEFAULT_MAX_INSTRUCTIONS} * 4 || Size > std::numeric_limits<uint32_t>::max()) {
      continue;
    }

    // Every relocation must be inside this block (hot or cold). A block holding a thunk
    // relocation is not cacheable: the thunk may not be registered yet when a
    // later process loads it.
    const auto Relocs = RelocsFor(Guest);
    bool Cacheable = true;

    for (const auto& R : Relocs) {
      if (R.Header.Type == CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE) {
        Cacheable = false;
        break;
      }
      const uint64_t Width = RelocWidth(R);
      const bool IsHot = (R.Header.Offset >= Begin && R.Header.Offset + Width <= Begin + Size);
      const bool IsCold = (R.Header.Offset >= ColdBaseOffset && R.Header.Offset + Width <= ColdBaseOffset + ColdSize);
      if (!IsHot && !IsCold) {
        Cacheable = false;
        break;
      }
      if (R.Header.Type == CPU::RelocationTypes::RELOC_LINK_RECORD) {
        const int64_t LiveRecord = static_cast<int64_t>(R.Header.Offset);
        const int64_t LiveCaller = LiveRecord + R.LinkRecord.CallerDelta;
        const int64_t LiveLinkBranch = LiveRecord + R.LinkRecord.LinkBranchDelta;
        if (LiveCaller < static_cast<int64_t>(Begin) || LiveCaller + 4 > static_cast<int64_t>(Begin + Size) ||
            LiveLinkBranch < static_cast<int64_t>(Begin) || LiveLinkBranch + 4 > static_cast<int64_t>(Begin + Size)) {
          Cacheable = false;
          break;
        }
        if (R.LinkRecord.LinkedEntryDelta != 0) {
          const int64_t LiveLinkedEntry = LiveRecord + R.LinkRecord.LinkedEntryDelta;
          if (LiveLinkedEntry < static_cast<int64_t>(Begin) || LiveLinkedEntry >= static_cast<int64_t>(Begin + Size)) {
            Cacheable = false;
            break;
          }
        }
        if (R.LinkRecord.FinalDelta != 0) {
          const int64_t LiveFinal = LiveRecord + R.LinkRecord.FinalDelta;
          if (LiveFinal < static_cast<int64_t>(Begin) || LiveFinal + 4 > static_cast<int64_t>(Begin + Size)) {
            Cacheable = false;
            break;
          }
        }
      }
    }
    if (!Cacheable) {
      continue;
    }

    GuestBytes.resize(Length);
    if (!Reader.Read(Guest, GuestBytes.data(), Length)) {
      continue;
    }

    SegmentBlock B {};
    B.GuestOffset = GuestOffset;
    B.GuestHash = XXH3_64bits(GuestBytes.data(), Length);
    B.GuestLength = static_cast<uint32_t>(Length);
    B.CodeSize = static_cast<uint32_t>(Size + ColdSize);
    B.ColdSize = static_cast<uint32_t>(ColdSize);
    B.EntryOffset = static_cast<uint32_t>(Entry.HostCode - Entry.BlockBegin);
    B.RelocBegin = static_cast<uint32_t>(Out.Relocs.size());
    for (auto Copy : Relocs) {
      if (Copy.Header.Offset >= Begin && Copy.Header.Offset < Begin + Size) {
        Copy.Header.Offset -= Begin;
      } else {
        const uint64_t ColdRelocOff = Copy.Header.Offset - ColdBaseOffset;
        const uint64_t StoredRecordOff = Size + ColdRelocOff;
        if (Copy.Header.Type == CPU::RelocationTypes::RELOC_LINK_RECORD) {
          const int64_t LiveRecord = static_cast<int64_t>(ColdBaseOffset + ColdRelocOff);
          const int64_t LiveCaller = LiveRecord + Copy.LinkRecord.CallerDelta;
          const int64_t LiveLinkBranch = LiveRecord + Copy.LinkRecord.LinkBranchDelta;
          const int64_t LiveLinkedEntry = Copy.LinkRecord.LinkedEntryDelta ? LiveRecord + Copy.LinkRecord.LinkedEntryDelta : 0;
          const int64_t LiveFinal = Copy.LinkRecord.FinalDelta ? LiveRecord + Copy.LinkRecord.FinalDelta : 0;

          const int64_t HotCallerOffset = LiveCaller - static_cast<int64_t>(Begin);
          const int64_t HotLinkBranchOffset = LiveLinkBranch - static_cast<int64_t>(Begin);
          const int64_t HotLinkedEntryOffset = LiveLinkedEntry ? LiveLinkedEntry - static_cast<int64_t>(Begin) : 0;
          const int64_t HotFinalOffset = LiveFinal ? LiveFinal - static_cast<int64_t>(Begin) : 0;

          Copy.LinkRecord.CallerDelta = static_cast<int32_t>(HotCallerOffset - static_cast<int64_t>(StoredRecordOff));
          Copy.LinkRecord.LinkBranchDelta = static_cast<int32_t>(HotLinkBranchOffset - static_cast<int64_t>(StoredRecordOff));
          Copy.LinkRecord.LinkedEntryDelta = HotLinkedEntryOffset ? static_cast<int32_t>(HotLinkedEntryOffset - static_cast<int64_t>(StoredRecordOff)) : 0;
          Copy.LinkRecord.FinalDelta = HotFinalOffset ? static_cast<int32_t>(HotFinalOffset - static_cast<int64_t>(StoredRecordOff)) : 0;
        }
        Copy.Header.Offset = StoredRecordOff;
      }
      if (Copy.Header.Type == CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL || Copy.Header.Type == CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE) {
        Copy.GuestRIP.GuestRIP -= Section.FileStartVA;
      }
      Out.Relocs.push_back(Copy);
    }
    B.RelocCount = static_cast<uint32_t>(Out.Relocs.size() - B.RelocBegin);

    // Code, then return the copy to its storage form: unlinked, host symbols
    // zeroed, guest RIPs relative to the file base, no live futex state.
    const size_t CodeStart = AlignUp(Out.Code.size(), BlockAlignment);
    Out.Code.resize(CodeStart);
    const auto* SrcHot = reinterpret_cast<const std::byte*>(BufferBase + Begin);
    Out.Code.insert(Out.Code.end(), SrcHot, SrcHot + Size);
    const auto* SrcCold = reinterpret_cast<const std::byte*>(BufferBase + ColdBaseOffset);
    Out.Code.insert(Out.Code.end(), SrcCold, SrcCold + ColdSize);
    B.CodeOffset = CodeStart;
    std::span<std::byte> Copy {Out.Code.data() + CodeStart, Size + ColdSize};
    const std::span<const CPU::Relocation> BlockRelocs {Out.Relocs.data() + B.RelocBegin, B.RelocCount};
    if (!Cache.ApplyCodeRelocations(0, Copy, BlockRelocs, true)) {
      Out.Code.resize(CodeStart);
      Out.Relocs.resize(B.RelocBegin);
      continue;
    }
    auto* StoredHeader = reinterpret_cast<CPU::CPUBackend::JITCodeHeader*>(Copy.data());
    StoredHeader->OffsetToBlockTail = static_cast<uint32_t>(Size);

    const uint32_t ZeroFutex = 0;
    ::memcpy(Copy.data() + Size + offsetof(CPU::CPUBackend::JITCodeTail, SpinLockFutex), &ZeroFutex, sizeof(ZeroFutex));

    B.EntryHash = HashBlock(B, Copy.data(), Out.Relocs.data() + B.RelocBegin);
    Out.Blocks.push_back(B);
  }
}

bool CodeCache::SaveData(Core::InternalThreadState&, int FD, const ExecutableFileSectionInfo& Section, uint64_t SerializedBaseAddress,
                         std::span<const GuestAddressRange> GuestRanges) {
  if (SerializedBaseAddress != 0) {
    return false;
  }
  fextl::vector<CompiledRecord> Records;
  fextl::vector<CPU::Relocation> Sink;
  {
    std::lock_guard lk {RelocationSinkMutex};
    Records = CompiledBlocks;
    if (Records.empty()) {
      return false;
    }
    Sink.assign(RelocationSink.begin(), RelocationSink.begin() + Records.back().RelocEnd);
  }
  std::ranges::stable_sort(Records, {}, &CompiledRecord::GuestRIP);
  fextl::vector<CompiledRecord> Latest;
  for (size_t i = 0; i < Records.size(); ++i) {
    if (i + 1 == Records.size() || Records[i + 1].GuestRIP != Records[i].GuestRIP) {
      Latest.push_back(Records[i]);
    }
  }
  fextl::unordered_map<uint64_t, size_t> ByGuest;
  for (size_t i = 0; i < Latest.size(); ++i) {
    ByGuest[Latest[i].GuestRIP] = i;
  }

  fextl::vector<uint64_t> Candidates;
  {
    auto CodeBufferLock = std::unique_lock {CTX.CodeBufferWriteMutex};
    auto CodeBuffer = CTX.GetLatest();
    auto ReadLock = CodeBuffer->LookupCache->AcquireReadLock();
    const uint64_t BufferBase = reinterpret_cast<uintptr_t>(CodeBuffer->Ptr);
    for (const auto& [Guest, Entry] : CodeBuffer->LookupCache->BlockList) {
      bool InRange = GuestRanges.empty();
      for (const auto& [Begin, End] : GuestRanges) {
        InRange |= Guest >= Begin && Guest < End;
      }
      if (!InRange || Entry.BlockBegin < BufferBase) {
        continue;
      }
      if (ByGuest.contains(Guest)) {
        Candidates.push_back(Guest);
      }
    }
  }
  std::ranges::sort(Candidates);

  SegmentBuilder Builder;
  CollectLiveBlocks(
    *this, CTX, Section, Candidates, [](uint64_t) { return false; },
    [&](uint64_t Guest) -> std::span<const CPU::Relocation> {
      const auto& Record = Latest[ByGuest[Guest]];
      return {Sink.data() + Record.RelocBegin, static_cast<size_t>(Record.RelocEnd - Record.RelocBegin)};
    },
    Builder);
  if (Builder.Blocks.empty()) {
    return false;
  }
  return WriteSegment(FD, Builder, ComputeCodeCacheConfigId(), Section.FileInfo.FileId);
}

static bool CompactSegments(const fextl::string& Base, const fextl::string& Extra, uint64_t ConfigId, uint64_t FileId) {
  fextl::vector<fextl::unique_ptr<CodeCache::CacheSegment>> Inputs;
  size_t NumNamed = 0;
  for (; NumNamed < MaxSegments; ++NumNamed) {
    auto Seg = CodeCache::CacheSegment::Open(SegmentPath(Base, NumNamed), ConfigId, FileId);
    if (!Seg) {
      break;
    }
    Inputs.push_back(std::move(Seg));
  }
  if (!Extra.empty()) {
    if (auto Seg = CodeCache::CacheSegment::Open(Extra, ConfigId, FileId)) {
      Inputs.push_back(std::move(Seg));
    }
  }
  if (Inputs.empty()) {
    return false;
  }

  // Select one valid entry per guest offset, earliest segment first.
  struct Pick {
    const CodeCache::CacheSegment* Seg;
    const SegmentBlock* Block;
  };
  fextl::vector<Pick> Picks;
  for (const auto& Seg : Inputs) {
    for (uint32_t i = 0; i < Seg->Header->NumBlocks; ++i) {
      if (Seg->Validate(Seg->Blocks[i])) {
        Picks.push_back({Seg.get(), &Seg->Blocks[i]});
      }
    }
  }
  std::ranges::stable_sort(Picks, {}, [](const Pick& P) { return P.Block->GuestOffset; });
  auto [First, Last] = std::ranges::unique(Picks, {}, [](const Pick& P) { return P.Block->GuestOffset; });
  Picks.erase(First, Last);

  uint64_t NumRelocs = 0;
  uint64_t CodeSize = 0;
  fextl::vector<SegmentBlock> Index;
  Index.reserve(Picks.size());
  for (const auto& P : Picks) {
    SegmentBlock B = *P.Block;
    B.RelocBegin = static_cast<uint32_t>(NumRelocs);
    B.CodeOffset = CodeSize;
    NumRelocs += B.RelocCount;
    CodeSize = AlignUp(CodeSize + B.CodeSize, BlockAlignment);
    B.EntryHash = HashBlock(B, P.Seg->Code + P.Block->CodeOffset, P.Seg->Relocs + P.Block->RelocBegin);
    Index.push_back(B);
  }
  if (Index.size() > std::numeric_limits<uint32_t>::max() || NumRelocs > std::numeric_limits<uint32_t>::max()) {
    return false;
  }

  auto Temp = WriteTempSegment(Base, [&](int FD) {
    const auto H = MakeHeader(ConfigId, FileId, Index.size(), NumRelocs, CodeSize);
    BufferedWriter Out {FD};
    Out.Put(&H, sizeof(H));
    Out.Put(Index.data(), Index.size() * sizeof(SegmentBlock));
    Out.PadTo(8);
    for (const auto& P : Picks) {
      Out.Put(P.Seg->Relocs + P.Block->RelocBegin, uint64_t {P.Block->RelocCount} * RelocSize);
    }
    Out.PadTo(BlockAlignment);
    const uint64_t CodeStart = Out.Written;
    for (const auto& P : Picks) {
      Out.Put(P.Seg->Code + P.Block->CodeOffset, P.Block->CodeSize);
      Out.PadTo(BlockAlignment);
    }
    return Out.Flush() && Out.Written == CodeStart + CodeSize && CodeStart == H.CodeOffset;
  });
  if (Temp.empty()) {
    return false;
  }
  if (::rename(Temp.c_str(), Base.c_str()) != 0) {
    ::unlink(Temp.c_str());
    return false;
  }
  for (size_t i = NumNamed; i-- > 1;) {
    ::unlink(SegmentPath(Base, i).c_str());
  }
  LogMan::Msg::IFmt("Code cache: compacted {} segments into {} ({} blocks)", Inputs.size(), Base, Index.size());
  return true;
}


// =============================================================================
// Size cap and eviction
//
// The cache directory holds one namespace per (guest file, ConfigId):
// `<name>-<FileId>-<ConfigId>` plus `.1`..`.7`, `.lock` and `.tmp.*` files.
// After a process publishes a segment it sweeps the directory, at most once per
// SweepIntervalSeconds across all processes (the mtime of `.sweep`, claimed
// under `.sweep.lock` with LOCK_NB):
//   1. namespaces written by another emulator build (or an older format),
//      and temp files, unused for StaleSeconds are removed;
//   2. if the rest exceeds CodeCacheMaxSize, whole namespaces are removed until
//      they fit in 90% of it: another build's first (they are dead weight the
//      moment its last process exits, and no process of this build can read
//      them), then this build's own, each in least-recently-used order.
// A namespace is removed under an exclusive LOCK_NB flock of its `.lock`, so
// never while a writer appends or compacts it; a busy one is skipped. Segments
// are unlinked from the highest index down, then the lock file. A process that
// has a segment mapped keeps valid data. A writer that raced the lock file's
// removal can at worst lose its own segment to a concurrent compaction, which
// costs recompiles, never wrong code (every block is checked on install).
// =============================================================================
namespace {
  struct NamespaceName {
    std::string_view Base;
    // -1: `.lock`, -2: `.tmp.*`, else the segment index.
    int Kind;
  };

  bool IsHex16(std::string_view S) {
    return S.size() == 16 && std::ranges::all_of(S, [](char C) { return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'); });
  }

  // Splits a cache directory entry into its namespace and kind.
  std::optional<NamespaceName> ParseCacheFileName(std::string_view Name) {
    // The namespace ends with `-<16 hex>-<16 hex>`; basenames may contain dots
    // and dashes, so search every dash for that shape.
    for (size_t Pos = Name.find('-'); Pos != std::string_view::npos; Pos = Name.find('-', Pos + 1)) {
      const size_t End = Pos + 34;
      if (End > Name.size() || !IsHex16(Name.substr(Pos + 1, 16)) || Name[Pos + 17] != '-' || !IsHex16(Name.substr(Pos + 18, 16))) {
        continue;
      }
      const auto Base = Name.substr(0, End);
      const auto Suffix = Name.substr(End);
      if (Suffix.empty()) {
        return NamespaceName {Base, 0};
      }
      if (Suffix == ".lock") {
        return NamespaceName {Base, -1};
      }
      if (Suffix.starts_with(".tmp.")) {
        return NamespaceName {Base, -2};
      }
      if (Suffix.size() == 2 && Suffix[0] == '.' && Suffix[1] >= '1' && Suffix[1] < '0' + static_cast<char>(MaxSegments)) {
        return NamespaceName {Base, Suffix[1] - '0'};
      }
    }
    return std::nullopt;
  }

  struct NamespaceInfo {
    uint64_t Bytes = 0;
    int64_t LastUse = 0;
    uint32_t SegmentMask = 0;
    bool HasLock = false;
    // Written by the build running this sweep (IsOwnBuild of its first segment).
    bool OwnBuild = false;
    fextl::vector<fextl::string> TempFiles;
  };

  // True if the segment file was written by this build in this format.
  bool IsOwnBuild(const fextl::string& Path) {
    int FD = ::open(Path.c_str(), O_RDONLY | O_CLOEXEC);
    if (FD == -1) {
      return false;
    }
    SegmentHeader H {};
    const bool Read = ::pread(FD, &H, sizeof(H), 0) == static_cast<ssize_t>(sizeof(H));
    ::close(FD);
    return Read && H.Magic == SegmentMagic && H.Version == SegmentVersion && H.HeaderHash == HashHeader(H) &&
           std::ranges::equal(H.BuildHash, GIT_HASH) && H.EmulatorId == EmulatorBuildId();
  }

  // Removes a namespace's files under its lock. False if the lock is busy.
  bool RemoveNamespace(const fextl::string& Dir, std::string_view Base, const NamespaceInfo& Info) {
    const fextl::string BasePath = fextl::fmt::format("{}/{}", Dir, Base);
    const fextl::string LockPath = BasePath + ".lock";
    int LockFD = -1;
    if (Info.HasLock) {
      LockFD = ::open(LockPath.c_str(), O_RDWR | O_CLOEXEC);
      if (LockFD != -1 && ::flock(LockFD, LOCK_EX | LOCK_NB) != 0) {
        ::close(LockFD);
        return false;
      }
    }
    for (size_t i = MaxSegments; i-- > 0;) {
      if (Info.SegmentMask & (1u << i)) {
        ::unlink(SegmentPath(BasePath, i).c_str());
      }
    }
    for (const auto& Temp : Info.TempFiles) {
      ::unlink(Temp.c_str());
    }
    if (LockFD != -1) {
      ::unlink(LockPath.c_str());
      ::close(LockFD);
    }
    return true;
  }

  void SweepCacheDirectory(const fextl::string& Dir, uint64_t CapBytes) {
    const int64_t Now = ::time(nullptr);
    const fextl::string StampPath = Dir + "/.sweep";
    struct stat St {};
    if (::stat(StampPath.c_str(), &St) == 0 && St.st_mtime + SweepIntervalSeconds > Now) {
      return;
    }
    const fextl::string SweepLock = Dir + "/.sweep.lock";
    int LockFD = ::open(SweepLock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (LockFD == -1) {
      return;
    }
    if (::flock(LockFD, LOCK_EX | LOCK_NB) != 0) {
      ::close(LockFD);
      return;
    }
    // Claim this interval: re-check under the lock, then stamp before the scan.
    if (::stat(StampPath.c_str(), &St) == 0 && St.st_mtime + SweepIntervalSeconds > Now) {
      ::close(LockFD);
      return;
    }
    if (int StampFD = ::open(StampPath.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644); StampFD != -1) {
      ::futimens(StampFD, nullptr);
      ::close(StampFD);
    }

    fextl::map<fextl::string, NamespaceInfo> Namespaces;
    if (DIR* D = ::opendir(Dir.c_str())) {
      const int DirFD = ::dirfd(D);
      while (const auto* Entry = ::readdir(D)) {
        const auto Parsed = ParseCacheFileName(Entry->d_name);
        if (!Parsed || ::fstatat(DirFD, Entry->d_name, &St, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(St.st_mode)) {
          continue;
        }
        auto& Info = Namespaces[fextl::string {Parsed->Base}];
        Info.Bytes += static_cast<uint64_t>(St.st_blocks) * 512;
        // Newest mtime of any file. Opening the lock does not change its mtime,
        // so it matters only for a namespace that never got a segment.
        Info.LastUse = std::max<int64_t>(Info.LastUse, St.st_mtime);
        if (Parsed->Kind == -1) {
          Info.HasLock = true;
        } else if (Parsed->Kind == -2) {
          if (St.st_mtime + StaleSeconds < Now) {
            Info.TempFiles.push_back(fextl::fmt::format("{}/{}", Dir, Entry->d_name));
          }
        } else {
          Info.SegmentMask |= 1u << Parsed->Kind;
        }
      }
      ::closedir(D);
    }

    struct Candidate {
      const fextl::string* Base;
      NamespaceInfo* Info;
    };
    fextl::vector<Candidate> Live;
    uint64_t Total = 0;
    for (auto& [Base, Info] : Namespaces) {
      const bool Stale = Info.LastUse + StaleSeconds < Now;
      bool Own = false;
      if (Info.SegmentMask) {
        const int Lowest = std::countr_zero(Info.SegmentMask);
        Own = IsOwnBuild(SegmentPath(fextl::fmt::format("{}/{}", Dir, Base), Lowest));
      }
      Info.OwnBuild = Own;
      if (!Own && Stale && RemoveNamespace(Dir, Base, Info)) {
        LogMan::Msg::IFmt("Code cache: removed unused namespace {} of another build", Base);
        continue;
      }
      if (!Info.TempFiles.empty()) {
        // Leftovers of a crashed writer; the namespace itself stays.
        for (const auto& Temp : Info.TempFiles) {
          ::unlink(Temp.c_str());
        }
      }
      Total += Info.Bytes;
      Live.push_back({&Base, &Info});
    }

    if (CapBytes != 0 && Total > CapBytes) {
      const uint64_t Target = CapBytes / 10 * 9;
      // Another build's namespaces first, whatever their mtime, then this
      // build's in least-recently-used order. A promote gives every guest file
      // a new ConfigId while the processes started before it keep writing the
      // old one (HANDOVER "Traps"), so the dead build's namespaces are as
      // recently written as the live build's and a pure LRU evicted apps that
      // are in use -- which is a whole cold session for that app -- to keep
      // caches nothing can ever read again. They cannot be removed outright
      // (their writers are still running); they are just worth least.
      std::ranges::sort(Live, {}, [](const Candidate& C) { return std::pair {C.Info->OwnBuild, C.Info->LastUse}; });
      for (const auto& C : Live) {
        if (Total <= Target) {
          break;
        }
        if (RemoveNamespace(Dir, *C.Base, *C.Info)) {
          Total -= std::min(Total, C.Info->Bytes);
          LogMan::Msg::IFmt("Code cache: evicted {} ({} KiB)", *C.Base, C.Info->Bytes >> 10);
        }
      }
    }
    ::close(LockFD);
  }
} // namespace

// =============================================================================
// Publishing a pass's segments, and the forked writer
//
// A save pass has two halves. Collecting the blocks needs this process:
// CollectLiveBlocks walks the code buffer under CodeBufferWriteMutex and reads
// the guest's own bytes. Publishing them does not: it writes a temp file, takes
// the namespace flock, links the temp into a free segment name (or, when all
// MaxSegments names are taken, folds the whole namespace and it into one) and
// sweeps the directory. That half is file I/O over a finished snapshot, and on
// a namespace at MaxSegments it rewrites the namespace whole -- 726 MB for VS
// Code's -- on whichever guest thread happened to reach the save trigger inside
// an mmap, munmap or mprotect, while it holds SaveIOLock and, inside the walk,
// CodeBufferWriteMutex. So the pass forks and the child publishes. Cold G4
// already proved the tool on the exit save (a forked writer outlives its
// parent); this is the same writer on the periodic and unmap passes, which are
// the ones a long-lived app pays over and over.
//
// The child is a fork of a live multi-threaded guest, so it only ever writes:
//   * no FEX lock is taken, no guest memory is read, no guest state is touched,
//     and it never returns into emulation -- it _exit()s;
//   * it comes from ::fork(), whose pthread_atfork handlers leave the allocator
//     consistent in the child (jemalloc registers them; the raw clone(2) of
//     ForkGuest does not run them). The built segments are inherited
//     copy-on-write, so the fork copies page tables, not data;
//   * the parent forks twice and reaps the intermediate, so the writer is
//     init's child and not the guest's: a guest calling wait(2) can neither see
//     it nor reap it, and no zombie waits for a parent that never waits;
//   * it takes the namespace lock with a bounded wait instead of dropping the
//     segment the moment the lock is busy, which is what lost the blocks of
//     every pass that raced a sibling (COLD-ROUND2 1.2(a)); and an alarm bounds
//     its whole life, so a lock a crashed sibling still holds costs one
//     segment, never a stray process.
//
// Nothing waits for the writer. What it could not publish is counted, not
// recovered: keeping the records for a child that is about to write them is how
// the relocation sink grows without bound in a long-lived process. The pass
// that publishes on the guest thread does hand its records back (see
// SaveNewBlocks), because there nothing else will write them.
// =============================================================================
namespace {
  // How long a writer child waits for a busy namespace lock before giving its
  // segment up, and the hard bound on the child's whole life.
  constexpr uint64_t PublishLockWaitSeconds = 20;
  constexpr unsigned WriterLifetimeSeconds = 60;
  // Writers of this process that may still be running before a pass publishes
  // its segments itself instead. A lock held longer than one writer's wait is a
  // sibling compacting a large namespace; queueing more writers behind it only
  // spends forks.
  constexpr int32_t MaxConcurrentWriters = 2;

  // flock(LOCK_NB) with a bounded wait. A blocking flock(2) cannot be used even
  // in a child: a crashed sibling's lock survives until its core dump drains,
  // and a writer parked on that forever is exactly the stray process a forked
  // writer must not become.
  bool LockWithDeadline(int FD, int Operation, uint64_t WaitSeconds) {
    const uint64_t Deadline = MonotonicSeconds() + WaitSeconds;
    for (;;) {
      if (::flock(FD, Operation | LOCK_NB) == 0) {
        return true;
      }
      if ((errno != EWOULDBLOCK && errno != EINTR) || MonotonicSeconds() >= Deadline) {
        return false;
      }
      const struct timespec Pause {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
      ::nanosleep(&Pause, nullptr);
    }
  }

  // What a publish did, so the durable mirror knows what changed.
  struct PublishResult {
    bool Written = false;
    size_t Index = 0;
    bool Compacted = false;
  };

  // Links Temp into a free segment name of Base, or folds every segment plus
  // Temp into one when all MaxSegments names are taken. Temp is unlinked either
  // way; the blocks are in the namespace only if this returns Written.
  PublishResult PublishTempSegment(const fextl::string& Base, const fextl::string& Temp, uint64_t ConfigId, uint64_t FileId, uint64_t WaitSeconds) {
    PublishResult Result;
    const auto LockPath = Base + ".lock";
    const int LockFD = ::open(LockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (LockFD != -1) {
      if (LockWithDeadline(LockFD, LOCK_SH, WaitSeconds)) {
        for (size_t i = 0; i < MaxSegments && !Result.Written; ++i) {
          if (::link(Temp.c_str(), SegmentPath(Base, i).c_str()) == 0) {
            Result.Written = true;
            Result.Index = i;
          } else if (errno != EEXIST) {
            break;
          }
        }
        ::flock(LockFD, LOCK_UN);
      }
      if (!Result.Written && LockWithDeadline(LockFD, LOCK_EX, WaitSeconds)) {
        // Every segment name is taken: fold them, and this segment, into one.
        Result.Written = CompactSegments(Base, Temp, ConfigId, FileId);
        Result.Compacted = Result.Written;
        ::flock(LockFD, LOCK_UN);
      }
      ::close(LockFD);
    }
    ::unlink(Temp.c_str());
    return Result;
  }
} // namespace

struct CodeCache::SweepPlan {
  // The working tier's directory and its cap, then the durable tier's. With
  // one tier only the first pair is set.
  fextl::string Dir;
  uint64_t CapBytes {};
  fextl::string DurableDir;
  uint64_t DurableCapBytes {};
};

// One target's blocks, built and waiting for a segment name.
struct CodeCache::PendingSegment {
  fextl::string Base;
  // The durable namespace to mirror this into, empty with one tier.
  fextl::string DurableBase;
  fextl::string Filename;
  uint64_t FileId;
  SegmentBuilder Builder;
  // Indices into the pass's KeepRecord, so a segment that could not be written
  // can hand its records back to a later pass.
  fextl::vector<uint32_t> Records;
  bool Written = false;
};

CodeCache::SaveWriterStats* CodeCache::GetSaveWriterStats() {
  if (!WriterStats) {
    // MAP_SHARED: the writers and this process must see one page, not a copy
    // each, or a writer's counters die with it and the stats line understates
    // every save this process made.
    void* Page = ::mmap(nullptr, sizeof(SaveWriterStats), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (Page == MAP_FAILED) {
      return nullptr;
    }
    WriterStats = new (Page) SaveWriterStats {};
  }
  return WriterStats;
}

size_t CodeCache::PublishSegments(std::span<PendingSegment> Pending, uint64_t ConfigId, uint64_t WaitSeconds, SaveWriterStats* Shared) {
  size_t Written = 0;
  for (auto& P : Pending) {
    std::error_code EC;
    std::filesystem::create_directories(std::filesystem::path(std::string_view {P.Base}).parent_path(), EC);
    auto Temp = WriteTempSegment(P.Base, [&](int FD) { return WriteSegment(FD, P.Builder, ConfigId, P.FileId); });
    const auto Published = Temp.empty() ? PublishResult {} : PublishTempSegment(P.Base, Temp, ConfigId, P.FileId, WaitSeconds);
    const uint64_t Compactions = Published.Compacted ? 1 : 0;
    P.Written = Published.Written;
    if (P.Written && !P.DurableBase.empty()) {
      // Mirror what changed, no more: one segment for an append, the whole
      // namespace after a compaction, which is where the names the durable tier
      // must lose are decided.
      if (Published.Compacted) {
        WriteBackNamespace(P.Base, P.DurableBase);
      } else {
        WriteBackSegment(P.Base, P.DurableBase, Published.Index);
      }
    }
    if (Shared) {
      // A writer says nothing: LogMan's handler can take a lock (stdio's) that
      // another thread held at the fork, and the counters carry this anyway.
      Shared->Compactions.fetch_add(Compactions, std::memory_order_relaxed);
      if (P.Written) {
        Shared->SavedBlocks.fetch_add(P.Builder.Blocks.size(), std::memory_order_relaxed);
        Shared->SavedSegments.fetch_add(1, std::memory_order_relaxed);
      } else {
        Shared->LostSegments.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      Stats.Compactions.fetch_add(Compactions, std::memory_order_relaxed);
      if (P.Written) {
        Stats.SavedBlocks.fetch_add(P.Builder.Blocks.size(), std::memory_order_relaxed);
        Stats.SavedSegments.fetch_add(1, std::memory_order_relaxed);
        LogMan::Msg::IFmt("Code cache: wrote {} blocks for {}", P.Builder.Blocks.size(), P.Filename);
      } else if (Temp.empty()) {
        LogMan::Msg::EFmt("Code cache: cannot write a segment for {}", P.Base);
      }
    }
    Written += P.Written ? 1 : 0;
  }
  return Written;
}

void CodeCache::RunSweeps(const SweepPlan& Sweeps) {
  if (!Sweeps.Dir.empty()) {
    SweepCacheDirectory(Sweeps.Dir, Sweeps.CapBytes);
  }
  if (!Sweeps.DurableDir.empty()) {
    SweepCacheDirectory(Sweeps.DurableDir, Sweeps.DurableCapBytes);
  }
}

bool CodeCache::ForkSegmentWriter(std::span<PendingSegment> Pending, uint64_t ConfigId, const SweepPlan& Sweeps) {
  auto* Shared = GetSaveWriterStats();
  if (!Shared) {
    return false;
  }
  const uint64_t Now = MonotonicSeconds();
  if (Shared->Active.load(std::memory_order_relaxed) >= MaxConcurrentWriters && Now < LastWriterForkSeconds + WriterLifetimeSeconds) {
    return false;
  }

  const pid_t Middle = ::fork();
  if (Middle < 0) {
    return false;
  }
  if (Middle == 0) {
    // The intermediate exists only so that the writer is not this guest's child.
    if (::fork() == 0) {
      Shared->Active.fetch_add(1, std::memory_order_relaxed);
      // Every descriptor the guest had open is inherited, and holding one open
      // makes this process something the world waits for: a shell's $(guest)
      // reads the guest's stdout until EVERY writer closes it, so a writer
      // parked on a namespace lock hung the command substitution for its whole
      // wait. The writer needs none of them -- it opens the files it writes by
      // name -- so it takes /dev/null for the three standard ones and drops the
      // rest. A fatal fault in a writer is then a core, not a message.
      if (const int Null = ::open("/dev/null", O_RDWR); Null != -1) {
        ::dup2(Null, STDIN_FILENO);
        ::dup2(Null, STDOUT_FILENO);
        ::dup2(Null, STDERR_FILENO);
        if (Null > STDERR_FILENO) {
          ::close(Null);
        }
      }
#ifdef SYS_close_range
      ::syscall(SYS_close_range, 3, ~0U, 0);
#endif
      // Its own alarm, on a clean handler: whatever happens below -- a lock
      // nobody releases, a host lock inherited mid-use -- this process is gone
      // within the minute. Timers do not cross a fork, so this one is its own.
      ::signal(SIGALRM, SIG_DFL);
      ::alarm(WriterLifetimeSeconds);
      const size_t Count = PublishSegments(Pending, ConfigId, PublishLockWaitSeconds, Shared);
      if (Count != 0) {
        RunSweeps(Sweeps);
      }
      Shared->Active.fetch_sub(1, std::memory_order_relaxed);
      ::_exit(0);
    }
    ::_exit(0);
  }
  LastWriterForkSeconds = Now;
  // The intermediate only forks and exits; reaping it here is what keeps the
  // writer out of the guest's wait(2) and out of the process table.
  int Status = 0;
  while (::waitpid(Middle, &Status, 0) < 0 && errno == EINTR) { }
  return true;
}


bool CodeCache::CompactAllSegments(const fextl::string& DurableBase, uint64_t FileId) {
  if (DurableBase.empty()) {
    return false;
  }
  // The caller names the durable namespace; the working one is the hot tier's
  // when there is one, and the result is mirrored back.
  const auto Hot = HotBasePath(DurableBase);
  const auto& Base = Hot.empty() ? DurableBase : Hot;
  SeedHotNamespace(Hot, DurableBase);
  const auto LockPath = Base + ".lock";
  int LockFD = ::open(LockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (LockFD == -1) {
    return false;
  }
  bool Done = false;
  bool Compacted = false;
  if (::flock(LockFD, LOCK_EX | LOCK_NB) == 0) {
    Compacted = ::access(SegmentPath(Base, 1).c_str(), F_OK) == 0 && CompactSegments(Base, {}, ComputeCodeCacheConfigId(), FileId);
    Done = Compacted || ::access(SegmentPath(Base, 1).c_str(), F_OK) != 0;
    Stats.Compactions.fetch_add(Done ? 1 : 0, std::memory_order_relaxed);
    ::flock(LockFD, LOCK_UN);
  }
  ::close(LockFD);
  if (Compacted && !Hot.empty()) {
    WriteBackNamespace(Hot, DurableBase);
  }
  return Done;
}

size_t CodeCache::SaveNewBlocks(Core::InternalThreadState&, std::span<const CodeCacheSaveTarget> Targets, CodeCacheSaveKind Kind) {
  if (!IsGeneratingCache || Targets.empty()) {
    return 0;
  }
  if (Kind == CodeCacheSaveKind::Periodic) {
    RanPeriodicPass.store(true, std::memory_order_relaxed);
  }
  ScopedNS Timer {Stats.SaveNS};
  const uint64_t ConfigId = ComputeCodeCacheConfigId();

  // Snapshot what has been compiled so far. Records appended while this pass
  // runs belong to the next one.
  fextl::vector<CompiledRecord> Records;
  fextl::vector<CPU::Relocation> Sink;
  uint64_t Generation {};
  {
    std::lock_guard lk {RelocationSinkMutex};
    if (Kind == CodeCacheSaveKind::Unmap) {
      // Most unmapped files have nothing compiled since the last pass.
      const bool AnyInTargets = std::ranges::any_of(CompiledBlocks, [&](const CompiledRecord& Record) {
        return std::ranges::any_of(Targets, [&](const CodeCacheSaveTarget& Target) {
          return std::ranges::any_of(Target.GuestRanges, [&](const auto& Range) {
            const auto& [RangeBegin, RangeEnd] = Range;
            return Record.GuestRIP >= RangeBegin && Record.GuestRIP < RangeEnd;
          });
        });
      });
      if (!AnyInTargets) {
        return 0;
      }
    }
    Records = CompiledBlocks;
    if (Records.empty()) {
      return 0;
    }
    Sink.assign(RelocationSink.begin(), RelocationSink.begin() + Records.back().RelocEnd);
    Generation = SinkGeneration;
  }
  const size_t SnapshotSize = Records.size();
  // Latest record per guest entry: a block recompiled after an invalidation is
  // live at its newest translation, whose relocations are the newest record's.
  std::ranges::stable_sort(Records, {}, &CompiledRecord::GuestRIP);
  fextl::vector<CompiledRecord> Latest;
  for (size_t i = 0; i < Records.size(); ++i) {
    if (i + 1 == Records.size() || Records[i + 1].GuestRIP != Records[i].GuestRIP) {
      Latest.push_back(Records[i]);
    }
  }

  // What happens to each latest record after this pass. A periodic or final
  // pass targets every mapped file, so a record outside every target belongs
  // to a file that is gone and is dropped. An unmap pass targets only the
  // files being unmapped and leaves every other record for a later pass.
  fextl::vector<bool> KeepRecord(Latest.size(), Kind == CodeCacheSaveKind::Unmap);
  // The last pass that can save a target's blocks, so the per-file minimum
  // cannot defer them.
  const bool LastChance = Kind != CodeCacheSaveKind::Periodic;

  // Built here, published below -- in a forked writer where one is possible.
  fextl::vector<PendingSegment> Pending;
  for (const auto& Target : Targets) {
    const auto& Section = Target.Section;
    // The target names the durable namespace; a pass writes the hot one and
    // mirrors it (see the two-tier block).
    const auto Hot = HotBasePath(Target.BasePath);
    const auto& Base = Hot.empty() ? Target.BasePath : Hot;
    const uint64_t FileId = Section.FileInfo.FileId;
    if (Base.empty()) {
      continue;
    }

    fextl::vector<uint64_t> Candidates;
    fextl::unordered_map<uint64_t, size_t> ByGuest;
    for (size_t i = 0; i < Latest.size(); ++i) {
      const auto& Record = Latest[i];
      for (const auto& [RangeBegin, RangeEnd] : Target.GuestRanges) {
        if (Record.GuestRIP >= RangeBegin && Record.GuestRIP < RangeEnd && Record.GuestRIP >= Section.FileStartVA) {
          Candidates.push_back(Record.GuestRIP);
          ByGuest[Record.GuestRIP] = i;
          KeepRecord[i] = false;
          break;
        }
      }
    }
    if (Candidates.empty()) {
      continue;
    }
    // Too few new blocks for a segment: a periodic pass keeps them, so a file
    // whose code is reached a few blocks at a time (a library's constructors
    // at startup and its destructors at exit) still gets saved. Dropping them
    // here left such files uncached for good.
    auto Defer = [&] {
      if (!LastChance) {
        for (uint64_t Guest : Candidates) {
          KeepRecord[ByGuest[Guest]] = true;
        }
      }
    };
    if (!LastChance && Candidates.size() < MinNewBlocksPerSegment) {
      Defer();
      continue;
    }

    // What is on disk now, including segments other processes wrote since this
    // process first looked.
    auto* File = GetFileCache(Section.FileInfo);
    if (File->BasePath != Base) {
      continue;
    }
    SegmentBuilder Builder;
    size_t MinBlocks = MinNewBlocksPerSegment;
    {
      std::unique_lock lk {RegistryMutex};
      File->ProbeNewSegments(ConfigId, FileId);
      // The minimum keeps the many short processes of a build from each
      // spending a segment (and, every MaxSegments, a compaction) on a few
      // rare-path blocks. On a file's last chance it is waived where that
      // cannot happen: for a file with no cache yet, whose first segment costs
      // no compaction (a small library's handful of constructor blocks), and
      // in a process that has run long enough for a periodic pass, which
      // exits too rarely for its segments to add up. Otherwise the blocks a
      // long-running process reaches only at exit, its libraries'
      // destructors, were recompiled by every run.
      if (LastChance && (File->NumSegments.load(std::memory_order_relaxed) == 0 || RanPeriodicPass.load(std::memory_order_relaxed))) {
        MinBlocks = 1;
      }
      if (Candidates.size() >= MinBlocks) {
        CollectLiveBlocks(
          *this, CTX, Section, Candidates, [File](uint64_t Off) { return File->Contains(Off); },
          [&](uint64_t Guest) -> std::span<const CPU::Relocation> {
            const auto& Record = Latest[ByGuest[Guest]];
            return {Sink.data() + Record.RelocBegin, static_cast<size_t>(Record.RelocEnd - Record.RelocBegin)};
          },
          Builder);
      }
    }
    if (Builder.Blocks.size() < MinBlocks) {
      Defer();
      continue;
    }

    PendingSegment P;
    P.Base = Base;
    P.DurableBase = Hot.empty() ? fextl::string {} : Target.BasePath;
    P.Filename = Section.FileInfo.Filename;
    P.FileId = FileId;
    P.Builder = std::move(Builder);
    P.Records.reserve(Candidates.size());
    for (uint64_t Guest : Candidates) {
      P.Records.push_back(static_cast<uint32_t>(ByGuest[Guest]));
    }
    Pending.push_back(std::move(P));
  }

  const bool OneShot = (Kind == CodeCacheSaveKind::Final) && !RanPeriodicPass.load(std::memory_order_relaxed);
  // Every base path is in the one cache directory, and with two tiers both are
  // swept: the durable one against the disk cap, the hot one against its own,
  // because that one is RAM.
  SweepPlan Sweeps;
  if (!OneShot) {
    for (const auto& Target : Targets) {
      if (Target.BasePath.empty()) {
        continue;
      }
      const auto MiBToBytes = [](int64_t MiB) {
        return MiB > 0 ? static_cast<uint64_t>(MiB) << 20 : 0;
      };
      const auto DurableDir = fextl::string {std::filesystem::path(std::string_view {Target.BasePath}).parent_path().string()};
      const auto Hot = HotBasePath(Target.BasePath);
      if (Hot.empty()) {
        Sweeps.Dir = DurableDir;
        Sweeps.CapBytes = MiBToBytes(FEXCore::Config::Get_CODECACHEMAXSIZE());
      } else {
        Sweeps.Dir = fextl::string {std::filesystem::path(std::string_view {Hot}).parent_path().string()};
        Sweeps.CapBytes = MiBToBytes(FEXCore::Config::Get_CODECACHEHOTMAXSIZE());
        Sweeps.DurableDir = DurableDir;
        Sweeps.DurableCapBytes = MiBToBytes(FEXCore::Config::Get_CODECACHEMAXSIZE());
      }
      break;
    }
  }

  size_t SegmentsWritten = 0;
  if (!Pending.empty()) {
    // The final pass already runs in a forked writer of its own, with no parent
    // left to stall (cold G4); every other pass is on a guest thread and hands
    // the writing to one.
    const bool Forked = Kind != CodeCacheSaveKind::Final && ForkWriter() && ForkSegmentWriter(Pending, ConfigId, Sweeps);
    if (Forked) {
      SegmentsWritten = Pending.size();
    } else {
      SegmentsWritten = PublishSegments(Pending, ConfigId, 0, nullptr);
      if (Kind != CodeCacheSaveKind::Final) {
        for (const auto& P : Pending) {
          if (!P.Written) {
            // The namespace lock was busy (a sibling appending or compacting),
            // or the segment could not be written. Hand the records back: this
            // pass published nothing for them and no writer will. Dropping them
            // here is what lost the blocks of every process that raced a
            // sibling -- with MaxSegments names taken every periodic pass wants
            // the exclusive lock, so two processes saving in the same minute
            // cost one of them its blocks for good.
            for (uint32_t Index : P.Records) {
              KeepRecord[Index] = true;
            }
          }
        }
      }
      if (SegmentsWritten != 0) {
        RunSweeps(Sweeps);
      }
    }
  }

  // Replace the snapshot's records with the ones kept above, and the
  // relocations only they reference. The rest has had its chance: written,
  // already on disk, not cacheable, or of a file no longer mapped. Records
  // appended during the pass stay, after the kept ones (newest last).
  // On Final save the image is terminating; nothing reads these again.
  if (Kind != CodeCacheSaveKind::Final) {
    std::lock_guard lk {RelocationSinkMutex};
    const uint64_t SinkPrefix = Sink.size();
    if (SinkGeneration == Generation && CompiledBlocks.size() >= SnapshotSize && RelocationSink.size() >= SinkPrefix) {
      fextl::vector<CompiledRecord> Blocks;
      fextl::vector<CPU::Relocation> Relocs;
      for (size_t i = 0; i < Latest.size(); ++i) {
        if (KeepRecord[i]) {
          const auto& Record = Latest[i];
          const uint64_t Begin = Relocs.size();
          Relocs.insert(Relocs.end(), Sink.begin() + Record.RelocBegin, Sink.begin() + Record.RelocEnd);
          Blocks.push_back({Record.GuestRIP, Begin, Relocs.size()});
        }
      }
      const uint64_t KeptRelocs = Relocs.size();
      Relocs.insert(Relocs.end(), RelocationSink.begin() + SinkPrefix, RelocationSink.end());
      for (size_t i = SnapshotSize; i < CompiledBlocks.size(); ++i) {
        auto Record = CompiledBlocks[i];
        Record.RelocBegin = Record.RelocBegin - SinkPrefix + KeptRelocs;
        Record.RelocEnd = Record.RelocEnd - SinkPrefix + KeptRelocs;
        Blocks.push_back(Record);
      }
      CompiledBlocks = std::move(Blocks);
      RelocationSink = std::move(Relocs);
    }
  }
  return SegmentsWritten;
}

bool CodeCache::ApplyCodeRelocations(uint64_t GuestEntry, std::span<std::byte> Code, std::span<const CPU::Relocation> EntryRelocations,
                                     bool ForStorage) {
  if (!ForStorage) {
    return ApplyCodeRelocationsSplit(GuestEntry, Code, {}, EntryRelocations);
  }

  // Link records are handled around the other relocations, because a link
  // thunk's caller word can be the first word of a guest RIP window (a
  // link-first constant exit): the unlinked word is whatever the RIP
  // relocation leaves there, not the word emitted in the generating process.
  //   1. storage only: undo links (caller and thunk words, HostCode);
  //   2. every other relocation;
  //   3. store the resulting unlinked words in the record (the delinker
  //      restores from them), and on install check the record is unlinked.
  auto* Base = reinterpret_cast<uint8_t*>(Code.data());
  const int64_t Size = static_cast<int64_t>(Code.size());
  constexpr int64_t RecordOrigWordsOffset = 24; // PPC64BlockLinkRecord::OrigCallerWord, then OrigThunkWord
  auto LinkSites = [&](const CPU::Relocation& Reloc, int64_t& Record, int64_t& Caller, int64_t& Thunk) {
    Record = static_cast<int64_t>(Reloc.Header.Offset);
    Caller = Record + Reloc.LinkRecord.CallerDelta;
    Thunk = Record + Reloc.LinkRecord.ThunkDelta;
    return Record >= 0 && Record <= Size - RecordOrigWordsOffset - 8 && Caller >= 0 && Caller <= Size - 4 && Thunk >= 0 && Thunk <= Size - 4;
  };

  for (const auto& Reloc : EntryRelocations) {
    if (Reloc.Header.Type != CPU::RelocationTypes::RELOC_LINK_RECORD) {
      continue;
    }
    int64_t Record, Caller, Thunk;
    if (!LinkSites(Reloc, Record, Caller, Thunk)) {
      return false;
    }
    if (ForStorage) {
      const uint64_t Zero = 0;
      memcpy(Base + Caller, &Reloc.LinkRecord.OrigCallerWord, 4);
      memcpy(Base + Thunk, &Reloc.LinkRecord.OrigThunkWord, 4);
      memcpy(Base + Record, &Zero, sizeof(Zero));
      if (Reloc.LinkRecord.FinalDelta != 0 && Reloc.LinkRecord.FinalDelta != Reloc.LinkRecord.CallerDelta) {
        const int64_t Final = Record + Reloc.LinkRecord.FinalDelta;
        if (Final >= 0 && Final <= Size - 4) {
          const uint32_t Trap = 0x7FE00008u;
          memcpy(Base + Final, &Trap, 4);
        }
      }
    }
  }

  for (const auto& Reloc : EntryRelocations) {
    const uint64_t Width = RelocWidth(Reloc);
    if (Reloc.Header.Offset > Code.size() || Width > Code.size() - Reloc.Header.Offset) {
      LogMan::Msg::EFmt("Code cache relocation at {:#x} overruns its {:#x} byte block", Reloc.Header.Offset, Code.size());
      return false;
    }
    auto* Ptr = reinterpret_cast<uint8_t*>(Code.data()) + Reloc.Header.Offset;
    const size_t Remaining = Code.size() - Reloc.Header.Offset;

    switch (Reloc.Header.Type) {
    case CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL: {
      uint64_t Pointer = ForStorage ? 0 : CPU::GetNamedSymbolLiteral(CTX, Reloc.NamedSymbolLiteral.Symbol);
      memcpy(Ptr, &Pointer, sizeof(Pointer));
      break;
    }
    case CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE: {
      // Never stored (see CollectLiveBlocks). Fail closed on an unresolved thunk:
      // patching in a null target would call address 0.
      uint64_t Pointer = ForStorage ? 0 : reinterpret_cast<uint64_t>(CTX.ThunkHandler->LookupThunk(Reloc.NamedThunkMove.Symbol));
      if (!ForStorage && (Pointer == 0 || Pointer == ~0ULL)) {
        return false;
      }
      FEXCore::CPU::PPC64EmitterBase PatchEmitter(&CTX, Ptr, Remaining);
      PatchEmitter.LoadConstantFixed(PPC64Emitter::r(Reloc.NamedThunkMove.RegisterIndex), Pointer);
      break;
    }
    case CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL: {
      uint64_t Val = GuestEntry + Reloc.GuestRIP.GuestRIP;
      memcpy(Ptr, &Val, sizeof(Val));
      break;
    }
    case CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE: {
      uint64_t Pointer = Reloc.GuestRIP.GuestRIP + GuestEntry;
      if (Reloc.GuestRIP.Instructions == 0) {
        FEXCore::CPU::PPC64EmitterBase PatchEmitter(&CTX, Ptr, Remaining);
        PatchEmitter.LoadConstantFixed(PPC64Emitter::r(Reloc.GuestRIP.RegisterIndex), Pointer);
        break;
      }
      // Variable width: stored as nops, so the file does not depend on the
      // writer's load base. On install, the rebased value must fit the
      // instructions the writer emitted; the block is compiled otherwise.
      // Emitted into a scratch window first: LoadConstant may need more room.
      uint8_t Window[PPC64Emitter::Emitter::LoadConstantFixedBytes + 4];
      FEXCore::CPU::PPC64EmitterBase PatchEmitter(&CTX, Window, sizeof(Window));
      if (!ForStorage) {
        PatchEmitter.LoadConstant(PPC64Emitter::r(Reloc.GuestRIP.RegisterIndex), Pointer);
      }
      if (PatchEmitter.GetOffset() > Width || Width > sizeof(Window)) {
        return false;
      }
      while (PatchEmitter.GetOffset() < Width) {
        PatchEmitter.nop();
      }
      memcpy(Ptr, Window, Width);
      break;
    }
    case CPU::RelocationTypes::RELOC_LINK_RECORD: break;
    default: LogMan::Msg::EFmt("Unknown code cache relocation type {}", ToUnderlying(Reloc.Header.Type)); return false;
    }
  }

  for (const auto& Reloc : EntryRelocations) {
    if (Reloc.Header.Type != CPU::RelocationTypes::RELOC_LINK_RECORD) {
      continue;
    }
    int64_t Record, Caller, Thunk;
    if (!LinkSites(Reloc, Record, Caller, Thunk)) {
      return false;
    }
    uint32_t CallerWord, ThunkWord;
    uint64_t HostCode;
    memcpy(&CallerWord, Base + Caller, 4);
    memcpy(&ThunkWord, Base + Thunk, 4);
    memcpy(&HostCode, Base + Record, sizeof(HostCode));
    if (!ForStorage && (HostCode != 0 || ThunkWord != Reloc.LinkRecord.OrigThunkWord)) {
      return false;
    }
    memcpy(Base + Record + RecordOrigWordsOffset, &CallerWord, 4);
    memcpy(Base + Record + RecordOrigWordsOffset + 4, &ThunkWord, 4);
  }
  return true;
}

bool CodeCache::ApplyCodeRelocationsSplit(uint64_t GuestEntry, std::span<std::byte> HotCode, std::span<std::byte> ColdCode,
                                          std::span<const CPU::Relocation> EntryRelocations) {
  const uint64_t HotSize = HotCode.size();
  const uint64_t ColdSize = ColdCode.size();

  if (HotSize >= sizeof(CPU::CPUBackend::JITCodeHeader)) {
    auto* Header = reinterpret_cast<CPU::CPUBackend::JITCodeHeader*>(HotCode.data());
    Header->OffsetToBlockTail = static_cast<uint32_t>(reinterpret_cast<intptr_t>(ColdCode.data()) - reinterpret_cast<intptr_t>(HotCode.data()));
  }

  for (const auto& Reloc : EntryRelocations) {
    if (Reloc.Header.Type != CPU::RelocationTypes::RELOC_LINK_RECORD) {
      continue;
    }
    if (Reloc.Header.Offset < HotSize || Reloc.Header.Offset >= HotSize + ColdSize) {
      return false;
    }
    const uint64_t ColdRecordOff = Reloc.Header.Offset - HotSize;
    if (ColdRecordOff + sizeof(CPU::PPC64BlockLinkRecord) > ColdSize) {
      return false;
    }
    auto* Record = reinterpret_cast<CPU::PPC64BlockLinkRecord*>(ColdCode.data() + ColdRecordOff);

    const int64_t ColdThunkOff = static_cast<int64_t>(ColdRecordOff) + Reloc.LinkRecord.ThunkDelta;
    if (ColdThunkOff < 0 || ColdThunkOff + 112 > static_cast<int64_t>(ColdSize)) {
      return false;
    }
    uint8_t* ThunkPtr = reinterpret_cast<uint8_t*>(ColdCode.data()) + ColdThunkOff;

    const int64_t HotCallerOff = static_cast<int64_t>(Reloc.Header.Offset) + Reloc.LinkRecord.CallerDelta;
    if (HotCallerOff < 0 || HotCallerOff + 4 > static_cast<int64_t>(HotSize)) {
      return false;
    }
    uint8_t* CallerPtr = reinterpret_cast<uint8_t*>(HotCode.data()) + HotCallerOff;

    const int64_t HotLinkBranchOff = static_cast<int64_t>(Reloc.Header.Offset) + Reloc.LinkRecord.LinkBranchDelta;
    if (HotLinkBranchOff < 0 || HotLinkBranchOff + 4 > static_cast<int64_t>(HotSize)) {
      return false;
    }
    uint8_t* LinkBranchPtr = reinterpret_cast<uint8_t*>(HotCode.data()) + HotLinkBranchOff;

    uint32_t ThunkWord = 0;
    memcpy(&ThunkWord, ThunkPtr, 4);
    if (Record->HostCode != 0 || ThunkWord != Reloc.LinkRecord.OrigThunkWord) {
      return false;
    }

    uint8_t* LinkPath = ThunkPtr + 0x14;
    const int64_t BranchDelta = reinterpret_cast<intptr_t>(LinkPath) - reinterpret_cast<intptr_t>(LinkBranchPtr);
    if (!CPU::PPC64BranchDisplacementInRange(BranchDelta)) {
      return false;
    }

    *reinterpret_cast<uint32_t*>(LinkBranchPtr) = CPU::PPC64EncodeBranch(BranchDelta);

    Record->CallerOffset = reinterpret_cast<intptr_t>(CallerPtr) - reinterpret_cast<intptr_t>(Record);
    Record->OrigCallerWord = (CallerPtr == LinkBranchPtr) ? *reinterpret_cast<uint32_t*>(CallerPtr) : Reloc.LinkRecord.OrigCallerWord;
    Record->OrigThunkWord = Reloc.LinkRecord.OrigThunkWord;
    Record->StubAddr = CTX.Dispatcher->GetExitFunctionLinkerWithRecordAddress();

    if (Reloc.LinkRecord.LinkedEntryDelta != 0) {
      const int64_t HotLinkedEntryOff = static_cast<int64_t>(Reloc.Header.Offset) + Reloc.LinkRecord.LinkedEntryDelta;
      if (HotLinkedEntryOff < 0 || HotLinkedEntryOff >= static_cast<int64_t>(HotSize)) {
        return false;
      }
      uint8_t* LinkedEntryPtr = reinterpret_cast<uint8_t*>(HotCode.data()) + HotLinkedEntryOff;
      Record->LinkedEntryOffset = reinterpret_cast<intptr_t>(LinkedEntryPtr) - reinterpret_cast<intptr_t>(Record);
    } else {
      Record->LinkedEntryOffset = 0;
    }

    if (Reloc.LinkRecord.FinalDelta != 0) {
      const int64_t HotFinalOff = static_cast<int64_t>(Reloc.Header.Offset) + Reloc.LinkRecord.FinalDelta;
      if (HotFinalOff < 0 || HotFinalOff >= static_cast<int64_t>(HotSize)) {
        return false;
      }
      uint8_t* FinalPtr = reinterpret_cast<uint8_t*>(HotCode.data()) + HotFinalOff;
      const int64_t FinalOffset = reinterpret_cast<intptr_t>(FinalPtr) - reinterpret_cast<intptr_t>(Record);
      Record->FinalOffset = FinalOffset | (Reloc.LinkRecord.FinalPlainBranch ? 1 : 0);
    } else {
      Record->FinalOffset = 0;
    }
  }

  for (const auto& Reloc : EntryRelocations) {
    if (Reloc.Header.Type == CPU::RelocationTypes::RELOC_LINK_RECORD) {
      continue;
    }
    const uint64_t Width = RelocWidth(Reloc);
    uint8_t* Ptr = nullptr;
    size_t Remaining = 0;
    if (Reloc.Header.Offset < HotSize) {
      if (Width > HotSize - Reloc.Header.Offset) {
        return false;
      }
      Ptr = reinterpret_cast<uint8_t*>(HotCode.data()) + Reloc.Header.Offset;
      Remaining = HotSize - Reloc.Header.Offset;
    } else {
      const uint64_t ColdOff = Reloc.Header.Offset - HotSize;
      if (ColdOff >= ColdSize || Width > ColdSize - ColdOff) {
        return false;
      }
      Ptr = reinterpret_cast<uint8_t*>(ColdCode.data()) + ColdOff;
      Remaining = ColdSize - ColdOff;
    }

    switch (Reloc.Header.Type) {
    case CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL: {
      uint64_t Pointer = CPU::GetNamedSymbolLiteral(CTX, Reloc.NamedSymbolLiteral.Symbol);
      memcpy(Ptr, &Pointer, sizeof(Pointer));
      break;
    }
    case CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE: {
      uint64_t Pointer = reinterpret_cast<uint64_t>(CTX.ThunkHandler->LookupThunk(Reloc.NamedThunkMove.Symbol));
      if (Pointer == 0 || Pointer == ~0ULL) {
        return false;
      }
      FEXCore::CPU::PPC64EmitterBase PatchEmitter(&CTX, Ptr, Remaining);
      PatchEmitter.LoadConstantFixed(PPC64Emitter::r(Reloc.NamedThunkMove.RegisterIndex), Pointer);
      break;
    }
    case CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL: {
      uint64_t Val = GuestEntry + Reloc.GuestRIP.GuestRIP;
      memcpy(Ptr, &Val, sizeof(Val));
      break;
    }
    case CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE: {
      uint64_t Pointer = Reloc.GuestRIP.GuestRIP + GuestEntry;
      if (Reloc.GuestRIP.Instructions == 0) {
        FEXCore::CPU::PPC64EmitterBase PatchEmitter(&CTX, Ptr, Remaining);
        PatchEmitter.LoadConstantFixed(PPC64Emitter::r(Reloc.GuestRIP.RegisterIndex), Pointer);
        break;
      }
      uint8_t Window[PPC64Emitter::Emitter::LoadConstantFixedBytes + 4];
      FEXCore::CPU::PPC64EmitterBase PatchEmitter(&CTX, Window, sizeof(Window));
      PatchEmitter.LoadConstant(PPC64Emitter::r(Reloc.GuestRIP.RegisterIndex), Pointer);
      if (PatchEmitter.GetOffset() > Width || Width > sizeof(Window)) {
        return false;
      }
      while (PatchEmitter.GetOffset() < Width) {
        PatchEmitter.nop();
      }
      memcpy(Ptr, Window, Width);
      break;
    }
    default:
      LogMan::Msg::EFmt("Unknown code cache relocation type {}", ToUnderlying(Reloc.Header.Type));
      return false;
    }
  }

  return true;
}

} // namespace FEXCore::Context
