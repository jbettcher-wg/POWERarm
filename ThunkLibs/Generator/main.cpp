#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/CompilationDatabase.h"

#include "llvm/Support/Signals.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "interface.h"

using namespace clang::tooling;

void print_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name << " <filename> <libname> <-host|-guest> <output_filename> <guest_rootfs|/> [guest_triple] -- <clang_flags>\n";
}

int main(int argc, char* const argv[]) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  if (argc < 6) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  // Parse compile flags after "--" (this updates argc to the index of the "--" separator)
  std::string error;
  auto compile_db = FixedCompilationDatabase::loadFromCommandLine(argc, argv, error);
  if (!compile_db) {
    print_usage(argv[0]);
    std::cerr << "\nError: " << error << "\n";
    return EXIT_FAILURE;
  }

  // Process arguments before the "--" separator
  if (argc != 6 && argc != 7) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  char* const* arg = argv + 1;
  const auto filename = *arg++;
  const std::string libname = *arg++;
  const std::string target_abi = *arg++;
  const std::string output_filename = *arg++;
  // Sysroot holding the guest's libc and libstdc++ headers, normally the guest
  // rootfs itself. "/" means none: only a Debian-style multilib
  // /usr/<triple>/include/ is searched in addition to the flags given.
  const std::string guest_rootfs = *arg++;

  // The guest is AArch64: AAPCS64, LP64, unsigned char, IEEE binary128 long
  // double. Every guest data layout the generated code encodes comes from a
  // parse with this target against the guest's own headers.
  //
  // An explicit triple overrides it. The host halves (ThunkLibs/HostLibs) pass
  // x86_64-linux-gnu: they still model the inherited x86-64 guest until they
  // are regenerated against the AArch64 rootfs and their layouts checked.
  const std::string guest_triple = argc == 7 ? std::string {*arg++} : std::string {"aarch64-linux-gnu"};

  OutputFilenames output_filenames;
  if (target_abi == "-host") {
    output_filenames.host = std::move(output_filename);
  } else if (target_abi == "-guest") {
    output_filenames.guest = std::move(output_filename);
  } else {
    std::cerr << "Unrecognized generator target ABI \"" << target_abi << "\"\n";
    return EXIT_FAILURE;
  }

  ClangTool Tool(*compile_db, {filename});
  if (CLANG_RESOURCE_DIR[0] != 0) {
    auto set_resource_directory = [](const clang::tooling::CommandLineArguments& Args, clang::StringRef) {
      clang::tooling::CommandLineArguments AdjustedArgs = Args;
      AdjustedArgs.push_back(std::string {"-resource-dir="} + CLANG_RESOURCE_DIR);
      return AdjustedArgs;
    };
    Tool.appendArgumentsAdjuster(set_resource_directory);
  }

  ClangTool GuestTool = Tool;

  auto append_guest_rootfs_includes = [&guest_rootfs](clang::tooling::CommandLineArguments& Args) {
    if (guest_rootfs == "/") {
      return;
    }

    // clang finds the guest gcc installation, and with it libstdc++, inside the
    // sysroot (lib/gcc/aarch64-unknown-linux-gnu/<version> in an Arch Linux ARM
    // rootfs).
    Args.push_back("--sysroot");
    Args.push_back(guest_rootfs);

    // A cross-toolchain keeps its libstdc++ headers beside the compiler rather than inside the
    // sysroot, so --sysroot alone resolves libc but not <type_traits>. For that case, set
    // FEX_THUNKGEN_GCC_TOOLCHAIN to the toolchain root (the directory containing
    // lib/gcc/<triple>/<version>) and clang locates both. A guest rootfs needs none of this.
    if (const char* gcc_toolchain = getenv("FEX_THUNKGEN_GCC_TOOLCHAIN"); gcc_toolchain && *gcc_toolchain) {
      Args.push_back(std::string {"--gcc-toolchain="} + gcc_toolchain);
    }

    // Library headers the guest rootfs lacks are architecture-neutral in
    // practice; fall back to the host's, behind everything else.
    Args.push_back("-idirafter");
    Args.push_back("/usr/include/");
  };

  // Analyse data layout for guest ABI
  GuestTool.appendArgumentsAdjuster([&](const clang::tooling::CommandLineArguments& Args, clang::StringRef) {
    clang::tooling::CommandLineArguments AdjustedArgs = Args;
    AdjustedArgs.push_back("-DGUEST_THUNK_LIBRARY");
    AdjustedArgs.push_back(std::string {"--target="} + guest_triple);
    AdjustedArgs.push_back("-isystem");
    AdjustedArgs.push_back(std::string {"/usr/"} + guest_triple + "/include/");

    append_guest_rootfs_includes(AdjustedArgs);

    return AdjustedArgs;
  });
  auto data_layout_analysis_factory = std::make_unique<AnalyzeDataLayoutActionFactory>();
  GuestTool.run(data_layout_analysis_factory.get());
  auto& data_layout = data_layout_analysis_factory->GetDataLayout();

  // Run generator for target ABI
  Tool.appendArgumentsAdjuster([&](const clang::tooling::CommandLineArguments& Args, clang::StringRef) {
    clang::tooling::CommandLineArguments AdjustedArgs = Args;
    AdjustedArgs.push_back("-DIS_HOST_THUNKGEN_PASS");
    if (target_abi == "-guest") {
      AdjustedArgs.push_back(std::string {"--target="} + guest_triple);
      append_guest_rootfs_includes(AdjustedArgs);
    }

    return AdjustedArgs;
  });
  return Tool.run(std::make_unique<GenerateThunkLibsActionFactory>(std::move(libname), std::move(output_filenames), data_layout).get());
}
