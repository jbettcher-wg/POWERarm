// SPDX-License-Identifier: MIT
#include "../FEXInterpreter/ELFCodeLoader.h"
#include <DummyHandlers.h>
#include <PortabilityInfo.h>
#include <Thunks.h>

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/HostFeatures.h>

#include "Common/HostPageGate.h"
#include <Common/ArgumentLoader.h>
#include <Common/Config.h>
#include <Common/FEXServerClient.h>
#include <Common/HostFeatures.h>

#include <OptionParser.h>

#include <fmt/printf.h>

#include <fstream>
#include <optional>

class AOTSyscallHandler : public FEXCore::HLE::SyscallHandler, public FEX::HLE::SyscallMmapInterface {
public:
  AOTSyscallHandler(FEXCore::HLE::SyscallOSABI SyscallOSABI) {
    OSABI = SyscallOSABI;
  }

  uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* Frame, FEXCore::HLE::SyscallArguments* Args) override {
    // Don't do anything
    return 0;
  }

  FEXCore::ExecutableFileInfo FileInfo;
  std::map<uint64_t, uint64_t> FileRanges;

  uintptr_t VAFileStart = 0;

  // These are no-ops implementations of the SyscallHandler API
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t Address) override {
    auto It = FileRanges.upper_bound(Address - VAFileStart);
    LOGMAN_THROW_A_FMT(It != FileRanges.begin(), "Could not find associated file mapping");
    --It;
    LOGMAN_THROW_A_FMT(VAFileStart + It->first + It->second > Address, "Could not find associated file mapping for {:#x}", Address);
    return FEXCore::ExecutableFileSectionInfo {FileInfo, VAFileStart, VAFileStart + It->first, VAFileStart + It->first + It->second};
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Address) override {
    return {0, UINT64_MAX, true};
  }

  uint64_t GuestMprotect(FEXCore::Core::InternalThreadState*, void* addr, size_t len, int prot) override {
    // The offline compiler forces every mapping writeable so it can apply
    // relocations; honouring a protection change would undo that.
    return 0;
  }

  void* GuestMmap(FEXCore::Core::InternalThreadState*, void* addr, size_t Size, int prot, int Flags, int fd, off_t offset) override {
    // Force writeable to allow applying relocations
    auto Ret = mmap(addr, Size, prot | PROT_WRITE, Flags, fd, offset);
    if (Ret != MAP_FAILED && VAFileStart == 0) {
      VAFileStart = reinterpret_cast<uintptr_t>(Ret);
    }
    FileRanges[reinterpret_cast<uintptr_t>(Ret) - VAFileStart] = Size;
    return Ret;
  }

  uint64_t GuestMunmap(FEXCore::Core::InternalThreadState*, void* addr, uint64_t length) override {
    return munmap(addr, length);
  }
};

static void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fmt::print("[{}] {}\n", LogMan::DebugLevelStr(Level), Message);
}

static void AssertHandler(const char* Message) {
  fmt::print("[A] {}\n", Message);
}

namespace FEXCore {
inline bool operator<(const ExecutableFileInfo& a, const ExecutableFileInfo& b) noexcept {
  return a.FileId < b.FileId;
}
} // namespace FEXCore

template<>
struct std::hash<FEXCore::ExecutableFileInfo> {
  std::size_t operator()(const FEXCore::ExecutableFileInfo& Val) const noexcept {
    return Val.FileId;
  }
};

static FEXCore::Core::InternalThreadState* SetupCompileThread(FEXCore::Context::Context& CTX) {
  return CTX.CreateThread(0, 0);
}

// Returns filename of generated cache on success
static std::optional<std::string> GenerateSingleCache(FEXCore::ExecutableFileInfo& Binary, fextl::set<uintptr_t> BlockList, std::string_view OutDir) {
  // Must match what the runtime computes (FEX::HLE::SyscallHandler), or the
  // cache this writes is named something no runtime will ever look for.
  const uint64_t CodeCacheConfigId = FEXCore::ComputeCodeCacheConfigId();

  ELFCodeLoader Loader(Binary.Filename.c_str(), -1, "", fextl::vector<fextl::string> {Binary.Filename.c_str()},
                       fextl::vector<fextl::string> {}, nullptr, nullptr, true /* skip interpreter */);
  if (!Loader.ELFWasLoaded()) {
    fmt::print("Invalid or unsupported ELF file.\n");
    return std::nullopt;
  }

  auto SyscallHandler = std::make_unique<AOTSyscallHandler>(FEXCore::HLE::SyscallOSABI::OS_LINUX64);

  // Populate relocations from ELF file
  {
    ELFParser RelocParser;
    RelocParser.ReadElf(Binary.Filename);
    Binary.Relocations = RelocParser.PopulateRelocations();
    SyscallHandler->FileInfo.Relocations = Binary.Relocations;
  }

  // Load HostFeatures
  auto HostFeatures = FEX::FetchHostFeatures();

  if (!std::filesystem::exists(Binary.Filename)) {
    fmt::print("File {} does not exist\n", Binary.Filename);
    // TODO: Pressure vessel hits this
    return /*EXIT_FAILURE*/ std::nullopt;
  }

  auto CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);

  Loader.CalculateHWCaps(CTX.get());

  auto SignalDelegation = std::make_unique<FEX::DummyHandlers::DummySignalDelegator>();
  CTX->SetSignalDelegator(SignalDelegation.get());
  CTX->SetSyscallHandler(SyscallHandler.get());
  auto ThunkHandler = FEX::HLE::CreateThunkHandler();
  CTX->SetThunkHandler(ThunkHandler.get());

  if (!CTX->InitCore()) {
    return std::nullopt;
  }

  auto Thread = SetupCompileThread(*CTX);

  {
    auto ElfBase = Loader.LoadMainElfFile(nullptr, SyscallHandler.get(), Thread);
    if (!ElfBase.has_value()) {
      ERROR_AND_DIE_FMT("Failed to load ELF file {} ({})", Binary.Filename, Binary.FileId);
    }

    // POWERARM-M0-TODO(smc): the offline compiler only countered 32-bit x86 relocations here; decide whether AArch64 RELATIVE/ABS64 data needs the same treatment.
  }

  CTX->GetCodeCache().InitiateCacheGeneration();

  {
    std::vector<std::unique_ptr<ELFCodeLoader>> LoaderMem;

    // Refuse to continue if the block list contains any out-of-bounds blocks.
    // This often indicates a corrupted code map.
    {
      auto [min_val, max_val] = std::ranges::minmax_element(BlockList, std::less {});
      auto MinBound = SyscallHandler->LookupExecutableFileSection(Thread, *min_val + SyscallHandler->VAFileStart);
      auto MaxBound = SyscallHandler->LookupExecutableFileSection(Thread, *max_val + SyscallHandler->VAFileStart);
      LOGMAN_THROW_A_FMT(MinBound && MaxBound, "Cached blocks offsets {:#x}-{:#x} out of bounds for library {} ({:016x} @ {:#x})!",
                         *min_val, *max_val, Binary.Filename, Binary.FileId, SyscallHandler->VAFileStart);
    }

    fmt::print(stderr, "Compiling code...\n");

    FEX_CONFIG_OPT(MaxInst, MAXINST);
    for (auto Addr : BlockList) {
      if (!CTX->CheckIfBlockIsCacheable(*Thread, Addr + SyscallHandler->VAFileStart, MaxInst)) {
        continue;
      }

      CTX->CompileRIP(Thread, Addr + SyscallHandler->VAFileStart);
    }

    auto Filename = fmt::format("{}{}-{:016x}", OutDir, FEXCore::CodeMap::GetBaseFilename(Binary, false), CodeCacheConfigId);
    auto FilenameNew = Filename + ".new";
    // O_TRUNC: a leftover .new from a killed run must not be appended to.
    int fd = open(FilenameNew.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    bool Ok = false;
    {
      auto Entry = SyscallHandler->LookupExecutableFileSection(Thread, SyscallHandler->VAFileStart).value();
      // No GuestRanges filter: this process compiled exactly one binary, so
      // every block in the buffer belongs to it.
      Ok = CTX->GetCodeCache().SaveData(*Thread, fd, Entry, 0 /* TODO: Use static base address information if available */);
    }
    // Close (flushing) and only then publish. Renaming a file descriptor's worth
    // of unwritten data over a good cache is how a crashed generator used to
    // destroy a working one.
    if (Ok) {
      Ok = fsync(fd) == 0;
    }
    close(fd);
    if (!Ok) {
      fmt::print(stderr, "Failed to write cache data for {}\n", Binary.Filename);
      std::filesystem::remove(FilenameNew.c_str());
      return std::nullopt;
    }
    std::filesystem::rename(FilenameNew.c_str(), Filename.c_str());
    return Filename;
  }
}

// Command handler that parses the given code map and generates a code cache for the selected guest binary.
// If no binary is selected explicitly, it is inferred from the code map ExecutableFileId block.
static int GenerateCache(int argc, const char** argv) {
  optparse::OptionParser Parser {};
  Parser.add_option("--outdir").set_default(FEX::Config::GetCacheDirectory() + "cache").help("Output directory for generated cache files");
  Parser.add_option("--fileid").help("Select binary to generate cache for");

  optparse::Values Options = Parser.parse_args(argc, argv);
  if (Parser.args().size() != 1) {
    Parser.print_usage();
    return 1;
  }
  const fextl::string CodeMapPath = Parser.args()[0];

  std::ifstream Codemap(CodeMapPath.c_str(), std::ios_base::binary);
  if (!Codemap) {
    fmt::print("Could not open {}\n", CodeMapPath);
    return 1;
  }

  FEXCore::ExecutableFileInfo ProgramName;
  std::map<FEXCore::ExecutableFileInfo, fextl::set<uintptr_t>> Data;
  {
    auto Parsed = FEXCore::CodeMap::ParseCodeMap(Codemap);

    // If an explicit file id is selected, use it.
    // Otherwise, fall back to an IsExecutable marker (or pick the first entry if there's only one)
    auto ExplicitFileId = strtoull(((fextl::string)Options.get("fileid")).data(), nullptr, 16);
    if (ExplicitFileId) {
      ProgramName.FileId = ExplicitFileId;
      ProgramName.Filename = Parsed.at(ExplicitFileId).Filename;
    }

    for (auto& [FileId, Contents] : Parsed) {
      if (!ExplicitFileId && (Contents.IsExecutable || Parsed.size() == 1)) {
        ProgramName.FileId = FileId;
        ProgramName.Filename = Contents.Filename;
      }
      Data.emplace(std::piecewise_construct, std::forward_as_tuple(nullptr, FileId, std::move(Contents.Filename)),
                   std::forward_as_tuple(std::move(Contents.Blocks)));
    }
  }
  if (!ProgramName.FileId) {
    fmt::print("Cannot generate cache from unsanitized code map {}", CodeMapPath);
    return 1;
  }

  for (auto& [File, Blocks] : Data) {
    if (!Blocks.empty()) {
      fmt::print("Parsed {} codemap entries for {} ({:016x})\n", Blocks.size(), File.Filename, File.FileId);
    } else {
      fmt::print("Found dependency {} ({:016x})\n", File.Filename, File.FileId);
    }
  }

  if (!Data.contains(ProgramName)) {
    throw std::runtime_error(fmt::format("Input code map {} did not contain {} ({:016x})", CodeMapPath, ProgramName.Filename, ProgramName.FileId));
  }

  fextl::string OutDir(Options.get("outdir"));
  if (!OutDir.ends_with('/')) {
    OutDir.push_back('/');
  }
  std::filesystem::create_directories(OutDir);

  const auto PortableInfo = FEX::ReadPortabilityInformation();
  char* envp[] = {nullptr};
  FEX::Config::LoadConfig("", envp, PortableInfo);

  // Host page size gate (64K port). Config is up by this point, so HostPageMode and the
  // degrade-mode SMCChecks forcing both work. No guest is loaded here, so auto and
  // native have nothing further to decide.
  (void)FEX::HostPageGate::CheckHostPageSize(true);

  auto NumBlocks = Data.at(ProgramName).size();
  auto GeneratedCache = GenerateSingleCache(ProgramName, Data.at(ProgramName), OutDir);
  if (GeneratedCache) {
    fmt::print("Successfully populated cache {} ({} blocks) via {}\n\n", GeneratedCache.value(), NumBlocks,
               std::filesystem::path {CodeMapPath}.filename().string());
  }
  return GeneratedCache ? 0 : 1;
}

int main(int argc, char** argv) {
  // Host page size is a runtime quantity (64K port). Latch it before anything maps
  // memory; every accessor self-initialises too, so a missed call cannot return 0.
  FEXCore::HostPage::Initialize();
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);

  std::vector<const char*> Args {argv + 1, argv + argc};
  auto CommandName = std::string {basename(argv[0])} + " " + (argc > 1 ? argv[1] : "");
  Args[0] = CommandName.c_str();

  if (argc >= 2 && argv[1] == std::string_view {"generate"}) {
    return GenerateCache(argc - 1, Args.data());
  } else {
    fmt::print("Usage: {} <command>\n\n", basename(argv[0]));
    fmt::print("Commands:\n");
    fmt::print("  generate\tTrigger cache generation from combined code map\n");
    return EXIT_FAILURE;
  }
}
