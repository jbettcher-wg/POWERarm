// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|common
desc: Emulated /proc/cpuinfo, version, osrelease, etc
$end_info$
*/

#include "CodeLoader.h"

#include "Common/CPUInfo.h"
#include "Common/FDUtils.h"
#include "LinuxSyscalls/GranuleMemory.h"
#include "LinuxSyscalls/Syscalls.h"
#include "LinuxSyscalls/EmulatedFiles/EmulatedFiles.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/sstream.h>
#include <FEXHeaderUtils/Filesystem.h>

#include <git_version.h>

#include <climits>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <ostream>
#include <stdio.h>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace FEX::EmulatedFile {
/**
 * @brief Generates a temporary file using raw FDs
 *
 * Since we are hooking syscalls that are expecting to use raw FDs, we need to make sure to also use raw FDs.
 * The guest application can leave these FDs dangling.
 *
 * Using glibc tmpfile creates a FILE which glibc tracks and will try cleaning up on application exit.
 * If we are running a 32-bit application then this dangling FILE will be allocated using the FEX allcator
 * Which will have already been cleaned up on shutdown.
 *
 * Dangling raw FD is safe since if the guest doesn't close them, then the kernel cleans them up on application close.
 *
 * @return A temporary file that we can use
 */
static int GenTmpFD(const char* pathname, int flags) {
  uint32_t memfd_flags {MFD_ALLOW_SEALING};
  if (flags & O_CLOEXEC) {
    memfd_flags |= MFD_CLOEXEC;
  }

  return memfd_create(pathname, memfd_flags);
}

// Seal the tmpfd features by sealing them all.
// Makes the tmpfd read-only.
static void SealTmpFD(int fd) {
  int ret = fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE);
  if (ret == -1) [[unlikely]] {
    // This shouldn't ever happen, but also isn't fatal.
    LogMan::Msg::EFmt("Couldn't seal tmpfd! {}", errno);
  }
}

fextl::string GenerateCPUInfo(FEXCore::Context::Context* ctx, uint32_t CPUCores) {
  // POWERARM-M0-TODO(cpustate): arm64 /proc/cpuinfo layout with a placeholder profile; the Features line, implementer and part must come from the hwcap/ID-register profile (DESIGN.md §4.5, §4.8).
  fextl::ostringstream cpu_stream {};
  for (uint32_t i = 0; i < CPUCores; ++i) {
    cpu_stream << "processor\t: " << i << std::endl;
    cpu_stream << "BogoMIPS\t: 100.00" << std::endl;
    // Linux prints the hwcaps in this order (arch/arm64/kernel/cpuinfo.c).
    cpu_stream << "Features\t: fp asimd aes pmull sha1 sha2 crc32 fphp asimdhp cpuid" << std::endl;
    cpu_stream << "CPU implementer\t: 0x00" << std::endl;
    cpu_stream << "CPU architecture: 8" << std::endl;
    cpu_stream << "CPU variant\t: 0x0" << std::endl;
    cpu_stream << "CPU part\t: 0x000" << std::endl;
    cpu_stream << "CPU revision\t: 0" << std::endl;
    cpu_stream << std::endl;
  }
  return cpu_stream.str();
}

EmulatedFDManager::EmulatedFDManager(FEXCore::Context::Context* ctx)
  : CTX {ctx}
  , ThreadsConfig {FEX::CPUInfo::CalculateNumberOfCPUs()} {
  FDReadCreators["/proc/cpuinfo"] = [&](FEXCore::Context::Context* ctx, int32_t fd, const char* pathname, int32_t flags, mode_t mode) -> int32_t {
    // Only allow a single thread to initialize the cpu_info.
    // Jit in-case multiple threads try to initialize at once.
    // Check if deferred cpuinfo initialization has occured.
    std::call_once(cpu_info_initialized, [&]() { cpu_info = GenerateCPUInfo(ctx, ThreadsConfig); });

    int FD = GenTmpFD(pathname, flags);
    write(FD, cpu_info.data(), cpu_info.size());
    lseek(FD, 0, SEEK_SET);
    SealTmpFD(FD);
    return FD;
  };

  FDReadCreators["/proc/sys/kernel/osrelease"] = [&](FEXCore::Context::Context* ctx, int32_t fd, const char* pathname, int32_t flags,
                                                     mode_t mode) -> int32_t {
    int FD = GenTmpFD(pathname, flags);
    uint32_t GuestVersion = FEX::HLE::_SyscallHandler->GetGuestKernelVersion();
    char Tmp[64] {};
    snprintf(Tmp, sizeof(Tmp), "%d.%d.%d\n", FEX::HLE::SyscallHandler::KernelMajor(GuestVersion),
             FEX::HLE::SyscallHandler::KernelMinor(GuestVersion), FEX::HLE::SyscallHandler::KernelPatch(GuestVersion));
    // + 1 to ensure null at the end
    write(FD, Tmp, strlen(Tmp) + 1);
    lseek(FD, 0, SEEK_SET);
    SealTmpFD(FD);
    return FD;
  };

  FDReadCreators["/proc/version"] = [&](FEXCore::Context::Context* ctx, int32_t fd, const char* pathname, int32_t flags, mode_t mode) -> int32_t {
    int FD = GenTmpFD(pathname, flags);
    // UTS version NEEDS to be in a format that can pass to `date -d`
    // Format of this is Linux version <Release> (<Compile By>@<Compile Host>) (<Linux Compiler>) #<version> {SMP, PREEMPT, PREEMPT_RT} <UTS version>\n"
    const char kernel_version[] = "Linux version %d.%d.%d (POWERarm@POWERarm) (clang) #" GIT_DESCRIBE_STRING " SMP " __DATE__ " " __TIME__ "\n";
    uint32_t GuestVersion = FEX::HLE::_SyscallHandler->GetGuestKernelVersion();
    char Tmp[sizeof(kernel_version) + 64] {};
    snprintf(Tmp, sizeof(Tmp), kernel_version, FEX::HLE::SyscallHandler::KernelMajor(GuestVersion),
             FEX::HLE::SyscallHandler::KernelMinor(GuestVersion), FEX::HLE::SyscallHandler::KernelPatch(GuestVersion));
    // + 1 to ensure null at the end
    write(FD, Tmp, strlen(Tmp) + 1);
    lseek(FD, 0, SEEK_SET);
    SealTmpFD(FD);
    return FD;
  };

  // Wine reads this to ensure TSC is trusted by the kernel. Otherwise it falls back to maximum clock speed of the CPU cores.
  // Without this, games like Horizon Zero Dawn would run their physics in slow-motion.
  FDReadCreators["/sys/devices/system/clocksource/clocksource0/current_clocksource"] =
    [&](FEXCore::Context::Context* ctx, int32_t fd, const char* pathname, int32_t flags, mode_t mode) -> int32_t {
    int FD = GenTmpFD(pathname, flags);
    const char source[] = "tsc\n";
    // + 1 to ensure null at the end
    write(FD, source, strlen(source) + 1);
    lseek(FD, 0, SEEK_SET);
    SealTmpFD(FD);
    return FD;
  };

  auto NumCPUCores = [&](FEXCore::Context::Context* ctx, int32_t fd, const char* pathname, int32_t flags, mode_t mode) -> int32_t {
    int FD = GenTmpFD(pathname, flags);
    write(FD, cpus_online.data(), cpus_online.size());
    lseek(FD, 0, SEEK_SET);
    SealTmpFD(FD);
    return FD;
  };

  FDReadCreators["/sys/devices/system/cpu/online"] = NumCPUCores;
  FDReadCreators["/sys/devices/system/cpu/present"] = NumCPUCores;
  // glibc 2.34+ derives _SC_NPROCESSORS_CONF from "possible"; without this
  // the guest sees every configured host CPU (160 on SMT-off POWER8) no
  // matter what online/present say.
  FDReadCreators["/sys/devices/system/cpu/possible"] = NumCPUCores;

  // /proc/self/maps and /proc/self/smaps, synthesised from VMATracking and the
  // granule table. PAGE_SIZE_64K_PLAN section 7: every granularity the guest can
  // observe must come from the fiction, not the host. A sub-granule hole left by
  // a partial munmap, and a 4K guard page inside a live granule, exist ONLY in
  // the granule table -- the host's own file cannot show either, because the
  // granule stays mapped for its live siblings. Wine's PE loader and glibc both
  // read maps at start-up.
  //
  // Granule::GenerateMaps returns an empty string on a 4K host and before any
  // guest mapping is tracked, and an empty string here means "fall through to
  // the real file". The shipping 4K build therefore never takes this path.
  auto ProcMaps = [](bool Smaps) {
    return [Smaps](FEXCore::Context::Context* ctx, int32_t fd, const char* pathname, int32_t flags, mode_t mode) -> int32_t {
      const auto Content = FEX::HLE::Granule::GenerateMaps(Smaps);
      if (Content.empty()) {
        return -1;
      }
      int FD = GenTmpFD(pathname, flags);
      write(FD, Content.data(), Content.size());
      lseek(FD, 0, SEEK_SET);
      SealTmpFD(FD);
      return FD;
    };
  };

  FDReadCreators["/proc/self/maps"] = ProcMaps(false);
  FDReadCreators["/proc/thread-self/maps"] = ProcMaps(false);
  FDReadCreators[fextl::fmt::format("/proc/{}/maps", getpid())] = ProcMaps(false);
  FDReadCreators["/proc/self/smaps"] = ProcMaps(true);
  FDReadCreators["/proc/thread-self/smaps"] = ProcMaps(true);
  FDReadCreators[fextl::fmt::format("/proc/{}/smaps", getpid())] = ProcMaps(true);

  fextl::string procAuxv = fextl::fmt::format("/proc/{}/auxv", getpid());

  FDReadCreators[procAuxv] = &EmulatedFDManager::ProcAuxv;
  FDReadCreators["/proc/self/auxv"] = &EmulatedFDManager::ProcAuxv;

  // /proc/self/cmdline shows the emulator's own command line (its path, the
  // guest's resolved path, then the guest arguments) unless the loader could
  // point the kernel at the guest's argument strings, which needs
  // CONFIG_CHECKPOINT_RESTORE. Without it, serve the guest's strings, read
  // live so later changes to argv[0] show as they would natively.
  FDReadCreators["/proc/self/cmdline"] = &EmulatedFDManager::ProcCmdline;
  FDReadCreators["/proc/thread-self/cmdline"] = &EmulatedFDManager::ProcCmdline;

  if (ThreadsConfig > 1) {
    cpus_online = fextl::fmt::format("0-{}", ThreadsConfig - 1);
  } else {
    cpus_online = "0";
  }
}

EmulatedFDManager::~EmulatedFDManager() {}

// "/proc/<our pid>/<rest>" as "/proc/self/<rest>" in Buffer, or nullptr. An
// opened /proc/self file reads back as /proc/<pid>/..., and after a fork the
// pid differs from the one the table was built with.
static const char* ProcSelfPath(const char* pathname, char* Buffer, size_t BufferSize) {
  constexpr std::string_view Proc {"/proc/"};
  std::string_view Path {pathname};
  if (!Path.starts_with(Proc)) {
    return nullptr;
  }
  Path.remove_prefix(Proc.size());
  const auto Slash = Path.find('/');
  if (Slash == std::string_view::npos || Slash == 0) {
    return nullptr;
  }
  const auto Pid = Path.substr(0, Slash);
  if (Pid.find_first_not_of("0123456789") != std::string_view::npos || Pid != fextl::fmt::format("{}", ::getpid())) {
    return nullptr;
  }
  const auto Rest = Path.substr(Slash);
  const int Len = snprintf(Buffer, BufferSize, "/proc/self%.*s", static_cast<int>(Rest.size()), Rest.data());
  return Len > 0 && static_cast<size_t>(Len) < BufferSize ? Buffer : nullptr;
}

int32_t EmulatedFDManager::Open(const char* pathname, int flags, uint32_t mode) {
  auto Creator = FDReadCreators.end();
  if (pathname) {
    Creator = FDReadCreators.find(pathname);
    if (Creator == FDReadCreators.end()) {
      char Buffer[PATH_MAX];
      if (const char* SelfPath = ProcSelfPath(pathname, Buffer, sizeof(Buffer))) {
        Creator = FDReadCreators.find(SelfPath);
      }
    }
  }

  if (Creator == FDReadCreators.end()) {
    return -1;
  }

  return Creator->second(CTX, AT_FDCWD, pathname, flags, mode);
}

int32_t EmulatedFDManager::ProcCmdline(FEXCore::Context::Context* ctx, int32_t fd, const char* pathname, int32_t flags, mode_t mode) {
  const auto Args = FEX::HLE::_SyscallHandler->GetCodeLoader()->GetArgumentData();
  if (Args.KernelRemapped || Args.address == 0) {
    // The kernel's own file is right.
    return -1;
  }

  int FD = GenTmpFD(pathname, flags);
  write(FD, reinterpret_cast<void*>(Args.address), Args.size);
  lseek(FD, 0, SEEK_SET);
  SealTmpFD(FD);
  return FD;
}

int32_t EmulatedFDManager::ProcAuxv(FEXCore::Context::Context* ctx, int32_t fd, const char* pathname, int32_t flags, mode_t mode) {
  const auto [auxvBase, auxvSize] = FEX::HLE::_SyscallHandler->GetCodeLoader()->GetAuxv();
  if (auxvBase == 0) {
    LogMan::Msg::DFmt("Failed to get Auxv stack address");
    return -1;
  }

  int FD = GenTmpFD(pathname, flags);
  write(FD, (void*)auxvBase, auxvSize);
  lseek(FD, 0, SEEK_SET);
  SealTmpFD(FD);
  return FD;
}
} // namespace FEX::EmulatedFile
