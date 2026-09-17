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
#include <cerrno>
#include <elf.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <link.h>
#include <optional>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
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
      Hasher.Add(uint64_t {F.CPUMIDRs.size()});
      for (uint32_t MIDR : F.CPUMIDRs) {
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
// On-disk format, version 4.
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

  uint64_t MonotonicSeconds() {
    struct timespec TS {};
    if (::clock_gettime(CLOCK_MONOTONIC, &TS) != 0) {
      return 0;
    }
    return static_cast<uint64_t>(TS.tv_sec);
  }

  constexpr std::array<char, 4> SegmentMagic = {'P', 'A', 'C', 'C'};
  constexpr uint32_t SegmentVersion = 5;
  constexpr size_t MaxSegments = 8;
  // A runtime writer skips files with fewer new blocks than this. Stops a
  // process that compiled a handful of rare-path blocks from spending a
  // segment (and, eventually, a compaction) on them.
  constexpr size_t MinNewBlocksPerSegment = 8;
  constexpr uint64_t BlockAlignment = 16;

  struct SegmentHeader {
    std::array<char, 4> Magic;
    uint32_t Version;
    uint64_t ConfigId;
    uint64_t FileId;
    std::array<uint8_t, 20> BuildHash;
    uint32_t NumBlocks;
    uint32_t NumRelocs;
    uint32_t Pad;
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
    uint32_t CodeSize;    // JITCodeTail::Size
    uint32_t EntryOffset; // entry point - JITCodeHeader
    uint32_t RelocBegin;
    uint32_t RelocCount;
    uint32_t Pad;
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
        B.CodeSize < sizeof(CPU::CPUBackend::JITCodeHeader) + sizeof(CPU::CPUBackend::JITCodeTail) || B.EntryOffset >= B.CodeSize) {
      return false;
    }
    if (B.RelocBegin > Header->NumRelocs || B.RelocCount > Header->NumRelocs - B.RelocBegin) {
      return false;
    }
    return !CheckHashes || HashBlock(B, Code + B.CodeOffset, Relocs + B.RelocBegin) == B.EntryHash;
  }

  static fextl::unique_ptr<CacheSegment> Open(const fextl::string& Path, uint64_t ConfigId, uint64_t FileId) {
    int FD = ::open(Path.c_str(), O_RDONLY | O_CLOEXEC);
    if (FD == -1) {
      return nullptr;
    }
    struct stat St {};
    if (::fstat(FD, &St) != 0 || St.st_size < static_cast<off_t>(sizeof(SegmentHeader))) {
      ::close(FD);
      return nullptr;
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
  fextl::string BasePath;
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
      auto Seg = CacheSegment::Open(SegmentPath(BasePath, Index), ConfigId, FileId);
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
    return false;
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
  // under CodeInvalidationMutex (shared), which fork holds exclusively, so none
  // can be held by a thread that did not survive the fork.
  BlocksSinceSave.store(0, std::memory_order_relaxed);
  {
    std::lock_guard lk {RelocationSinkMutex};
    CompiledBlocks.clear();
    RelocationSink.clear();
  }
}

void CodeCache::DumpStats() {
  auto L = [](const std::atomic<uint64_t>& A) {
    return A.load(std::memory_order_relaxed);
  };
  const auto Line = fextl::fmt::format("POWERarm code cache [{}]: loaded {} not-in-index {} no-file {} bad-entry {} guest-mismatch {} not-exec {} "
                                       "reloc-failed {} saved {} blocks in {} segments, {} compactions; save-ms {} lookup-ms {}\n",
                                       ::getpid(), L(Stats.Loaded), L(Stats.NotInIndex), L(Stats.NoFile), L(Stats.BadEntry),
                                       L(Stats.GuestMismatch), L(Stats.NotExecutable), L(Stats.RelocFailed), L(Stats.SavedBlocks),
                                       L(Stats.SavedSegments), L(Stats.Compactions), L(Stats.SaveNS) / 1000000, L(Stats.LoadNS) / 1000000);
  (void)::write(STDERR_FILENO, Line.data(), Line.size());
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
  auto BasePath = CTX.SyscallHandler->CodeCacheBasePath(FileInfo);
  std::unique_lock lk {RegistryMutex};
  auto& Slot = Registry[FileInfo.FileId];
  if (!Slot) {
    Slot = fextl::make_unique<FileCache>();
    Slot->BasePath = std::move(BasePath);
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
  ScopedNS Timer {Stats.LoadNS};


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
  const uint64_t Offset = AlignUp(CTX.LatestOffset, BlockAlignment);
  if (Offset > CodeBuffer->UsableSize() || Block->CodeSize > CodeBuffer->UsableSize() - Offset) {
    // Leave rotation to the compiler.
    return std::nullopt;
  }

  auto Dest = std::as_writable_bytes(std::span {CodeBuffer->Ptr, CodeBuffer->UsableSize()}).subspan(Offset, Block->CodeSize);
  ::memcpy(Dest.data(), Seg->Code + Block->CodeOffset, Block->CodeSize);
  const std::span<const CPU::Relocation> Relocs {Seg->Relocs + Block->RelocBegin, Block->RelocCount};
  if (!ApplyCodeRelocations(Section->FileStartVA, Dest, Relocs, false)) {
    Stats.RelocFailed.fetch_add(1, std::memory_order_relaxed);
    // Nothing references these bytes; LatestOffset is unchanged, so the next
    // compile overwrites them.
    return std::nullopt;
  }

  // The block must describe itself as a compile of GuestRIP would.
  const auto* Header = reinterpret_cast<const CPU::CPUBackend::JITCodeHeader*>(Dest.data());
  if (Header->OffsetToBlockTail > Block->CodeSize - sizeof(CPU::CPUBackend::JITCodeTail)) {
    return std::nullopt;
  }
  const auto* Tail = reinterpret_cast<const CPU::CPUBackend::JITCodeTail*>(Dest.data() + Header->OffsetToBlockTail);
  if (Tail->RIP != GuestRIP || Tail->GuestSize != Length || Tail->Size != Block->CodeSize) {
    Stats.RelocFailed.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
  }

  FEXCore::ArchHelpers::PPC64::FlushICacheRange(Dest.data(), Dest.size_bytes());
  CTX.LatestOffset = Offset + Block->CodeSize;
  // Host-PC -> block index, so signals inside the block resolve like a compile's.
  CodeBuffer->AppendBlock(static_cast<uint32_t>(Offset));

  Stats.Loaded.fetch_add(1, std::memory_order_relaxed);
  auto* Begin = reinterpret_cast<uint8_t*>(Dest.data());
  return LoadedBlock {
    .BlockBegin = Begin,
    .HostCode = Begin + Block->EntryOffset,
    .Size = Block->CodeSize,
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
    if (Used - Begin < sizeof(CPU::CPUBackend::JITCodeHeader) + sizeof(CPU::CPUBackend::JITCodeTail)) {
      continue;
    }
    const auto* Header = reinterpret_cast<const CPU::CPUBackend::JITCodeHeader*>(BufferBase + Begin);
    if (Header->OffsetToBlockTail > Used - Begin - sizeof(CPU::CPUBackend::JITCodeTail)) {
      continue;
    }
    const auto* Tail = reinterpret_cast<const CPU::CPUBackend::JITCodeTail*>(BufferBase + Begin + Header->OffsetToBlockTail);
    const uint64_t Size = Tail->Size;
    const uint64_t Length = Tail->GuestSize;
    if (Tail->RIP != Guest || Size > Used - Begin || Size % BlockAlignment != 0 || Header->OffsetToBlockTail + sizeof(*Tail) > Size ||
        Entry.HostCode < Entry.BlockBegin || Entry.HostCode - Entry.BlockBegin >= Size || Length == 0 || Length % 4 != 0 ||
        Length > uint64_t {FEXCore::A64::DEFAULT_MAX_INSTRUCTIONS} * 4 || Size > std::numeric_limits<uint32_t>::max()) {
      continue;
    }

    // Every relocation must be inside this block. A block holding a thunk
    // relocation is not cacheable: the thunk may not be registered yet when a
    // later process loads it.
    const auto Relocs = RelocsFor(Guest);
    bool Cacheable = true;
    for (const auto& R : Relocs) {
      if (R.Header.Type == CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE || R.Header.Offset < Begin ||
          R.Header.Offset + RelocWidth(R) > Begin + Size) {
        Cacheable = false;
        break;
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
    B.CodeSize = static_cast<uint32_t>(Size);
    B.EntryOffset = static_cast<uint32_t>(Entry.HostCode - Entry.BlockBegin);
    B.RelocBegin = static_cast<uint32_t>(Out.Relocs.size());
    for (auto Copy : Relocs) {
      Copy.Header.Offset -= Begin;
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
    const auto* Src = reinterpret_cast<const std::byte*>(BufferBase + Begin);
    Out.Code.insert(Out.Code.end(), Src, Src + Size);
    B.CodeOffset = CodeStart;
    std::span<std::byte> Copy {Out.Code.data() + CodeStart, Size};
    const std::span<const CPU::Relocation> BlockRelocs {Out.Relocs.data() + B.RelocBegin, B.RelocCount};
    if (!Cache.ApplyCodeRelocations(0, Copy, BlockRelocs, true)) {
      Out.Code.resize(CodeStart);
      Out.Relocs.resize(B.RelocBegin);
      continue;
    }
    const uint32_t ZeroFutex = 0;
    ::memcpy(Copy.data() + Header->OffsetToBlockTail + offsetof(CPU::CPUBackend::JITCodeTail, SpinLockFutex), &ZeroFutex, sizeof(ZeroFutex));

    B.EntryHash = HashBlock(B, Copy.data(), Out.Relocs.data() + B.RelocBegin);
    Out.Blocks.push_back(B);
  }
}

bool CodeCache::SaveData(Core::InternalThreadState&, int FD, const ExecutableFileSectionInfo& Section, uint64_t SerializedBaseAddress,
                         std::span<const GuestAddressRange> GuestRanges) {
  if (SerializedBaseAddress != 0) {
    return false;
  }
  // Every live block in the ranges, with the whole sink sorted by offset.
  fextl::vector<CPU::Relocation> Sink;
  {
    std::lock_guard lk {RelocationSinkMutex};
    Sink = RelocationSink;
  }
  std::ranges::sort(Sink, {}, [](const CPU::Relocation& R) { return R.Header.Offset; });

  fextl::vector<uint64_t> Candidates;
  fextl::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> Extents;
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
      const uint64_t Begin = Entry.BlockBegin - BufferBase;
      const auto* Header = reinterpret_cast<const CPU::CPUBackend::JITCodeHeader*>(Entry.BlockBegin);
      const auto* Tail = reinterpret_cast<const CPU::CPUBackend::JITCodeTail*>(Entry.BlockBegin + Header->OffsetToBlockTail);
      Candidates.push_back(Guest);
      Extents[Guest] = {Begin, Begin + Tail->Size};
    }
  }
  std::ranges::sort(Candidates);

  SegmentBuilder Builder;
  CollectLiveBlocks(
    *this, CTX, Section, Candidates, [](uint64_t) { return false; },
    [&](uint64_t Guest) -> std::span<const CPU::Relocation> {
      const auto [Begin, End] = Extents[Guest];
      auto First = std::ranges::lower_bound(Sink, Begin, {}, [](const CPU::Relocation& R) { return R.Header.Offset; });
      auto Last = std::ranges::lower_bound(First, Sink.end(), End, {}, [](const CPU::Relocation& R) { return R.Header.Offset; });
      return {Sink.data() + (First - Sink.begin()), static_cast<size_t>(Last - First)};
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

size_t CodeCache::SaveNewBlocks(Core::InternalThreadState&, std::span<const CodeCacheSaveTarget> Targets) {
  if (!IsGeneratingCache || Targets.empty()) {
    return 0;
  }
  ScopedNS Timer {Stats.SaveNS};
  const uint64_t ConfigId = ComputeCodeCacheConfigId();

  // Snapshot what has been compiled so far. Records appended while this pass
  // runs belong to the next one.
  fextl::vector<CompiledRecord> Records;
  fextl::vector<CPU::Relocation> Sink;
  {
    std::lock_guard lk {RelocationSinkMutex};
    Records = CompiledBlocks;
    if (Records.empty()) {
      return 0;
    }
    Sink.assign(RelocationSink.begin(), RelocationSink.begin() + Records.back().RelocEnd);
  }
  // Latest record per guest entry: a block recompiled after an invalidation is
  // live at its newest translation, whose relocations are the newest record's.
  std::ranges::stable_sort(Records, {}, &CompiledRecord::GuestRIP);
  fextl::vector<CompiledRecord> Latest;
  for (size_t i = 0; i < Records.size(); ++i) {
    if (i + 1 == Records.size() || Records[i + 1].GuestRIP != Records[i].GuestRIP) {
      Latest.push_back(Records[i]);
    }
  }

  size_t SegmentsWritten = 0;
  for (const auto& Target : Targets) {
    const auto& Section = Target.Section;
    const auto& Base = Target.BasePath;
    const uint64_t FileId = Section.FileInfo.FileId;
    if (Base.empty()) {
      continue;
    }

    fextl::vector<uint64_t> Candidates;
    fextl::unordered_map<uint64_t, const CompiledRecord*> ByGuest;
    for (const auto& Record : Latest) {
      for (const auto& [RangeBegin, RangeEnd] : Target.GuestRanges) {
        if (Record.GuestRIP >= RangeBegin && Record.GuestRIP < RangeEnd && Record.GuestRIP >= Section.FileStartVA) {
          Candidates.push_back(Record.GuestRIP);
          ByGuest[Record.GuestRIP] = &Record;
          break;
        }
      }
    }
    if (Candidates.size() < MinNewBlocksPerSegment) {
      continue;
    }

    // What is on disk now, including segments other processes wrote since this
    // process first looked.
    auto* File = GetFileCache(Section.FileInfo);
    if (File->BasePath != Base) {
      continue;
    }
    SegmentBuilder Builder;
    {
      std::unique_lock lk {RegistryMutex};
      File->ProbeNewSegments(ConfigId, FileId);
      CollectLiveBlocks(
        *this, CTX, Section, Candidates, [File](uint64_t Off) { return File->Contains(Off); },
        [&](uint64_t Guest) -> std::span<const CPU::Relocation> {
          const auto* Record = ByGuest[Guest];
          return {Sink.data() + Record->RelocBegin, static_cast<size_t>(Record->RelocEnd - Record->RelocBegin)};
        },
        Builder);
    }
    if (Builder.Blocks.size() < MinNewBlocksPerSegment) {
      continue;
    }

    std::error_code EC;
    std::filesystem::create_directories(std::filesystem::path(std::string_view {Base}).parent_path(), EC);
    auto Temp = WriteTempSegment(Base, [&](int FD) { return WriteSegment(FD, Builder, ConfigId, FileId); });
    if (Temp.empty()) {
      LogMan::Msg::EFmt("Code cache: cannot write a segment for {}", Base);
      continue;
    }

    const auto LockPath = Base + ".lock";
    int LockFD = ::open(LockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    bool Written = false;
    if (LockFD != -1) {
      if (::flock(LockFD, LOCK_SH) == 0) {
        for (size_t i = 0; i < MaxSegments && !Written; ++i) {
          if (::link(Temp.c_str(), SegmentPath(Base, i).c_str()) == 0) {
            Written = true;
          } else if (errno != EEXIST) {
            break;
          }
        }
        ::flock(LockFD, LOCK_UN);
      }
      if (!Written && ::flock(LockFD, LOCK_EX | LOCK_NB) == 0) {
        // Every segment name is taken: fold them, and this segment, into one.
        Written = CompactSegments(Base, Temp, ConfigId, FileId);
        Stats.Compactions.fetch_add(Written ? 1 : 0, std::memory_order_relaxed);
        ::flock(LockFD, LOCK_UN);
      }
      ::close(LockFD);
    }
    ::unlink(Temp.c_str());

    if (Written) {
      ++SegmentsWritten;
      Stats.SavedBlocks.fetch_add(Builder.Blocks.size(), std::memory_order_relaxed);
      Stats.SavedSegments.fetch_add(1, std::memory_order_relaxed);
      LogMan::Msg::IFmt("Code cache: wrote {} blocks for {}", Builder.Blocks.size(), Section.FileInfo.Filename);
    }
  }

  // Everything in the snapshot has had its chance: written, already on disk,
  // not cacheable, below the per-file minimum, or outside every target. Drop
  // it, and the relocations only it referenced.
  {
    std::lock_guard lk {RelocationSinkMutex};
    const size_t N = Records.size();
    const uint64_t SinkPrefix = Sink.size();
    if (CompiledBlocks.size() >= N && RelocationSink.size() >= SinkPrefix) {
      CompiledBlocks.erase(CompiledBlocks.begin(), CompiledBlocks.begin() + N);
      RelocationSink.erase(RelocationSink.begin(), RelocationSink.begin() + SinkPrefix);
      for (auto& Record : CompiledBlocks) {
        Record.RelocBegin -= SinkPrefix;
        Record.RelocEnd -= SinkPrefix;
      }
    }
  }
  return SegmentsWritten;
}

bool CodeCache::ApplyCodeRelocations(uint64_t GuestEntry, std::span<std::byte> Code, std::span<const CPU::Relocation> EntryRelocations,
                                     bool ForStorage) {
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

} // namespace FEXCore::Context
