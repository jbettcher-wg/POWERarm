// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/Config/Config.h>
#include <FEXCore/Utils/TypeDefines.h>
#include <FEXCore/Utils/CompilerDefs.h>
#include <FEXCore/fextl/fmt.h>

#include <cstdlib>
#include <string_view>
#include <unistd.h>

namespace FEX::HostPageGate {
/**
 * @brief The host-page-size startup gate.
 *
 * An AArch64 guest is told AT_PAGESZ = the host page size. arm64 Linux software
 * already runs on 4K, 16K and 64K kernels, and GNU ld's aarch64 default
 * max-page-size is 64K, so a binary whose PT_LOADs are aligned to the host page
 * needs nothing beyond host-granular mmap. A binary linked for 4K pages
 * (p_align 0x1000) cannot be mapped that way on a 64K host: its segments share
 * host pages with different protections. For those, the 4K granule emulation
 * inherited from fastppcx86 is kept as a per-process fallback
 * (docs/powerarm/DESIGN.md §4.9, docs/PAGE_SIZE_64K_EXECUTION.md).
 *
 * POWERARM_HOSTPAGEMODE selects what happens on a host page larger than 4K:
 *   auto    (default) - look at the PT_LOADs of the program and its
 *                       interpreter. If every p_align is at least the host
 *                       page, run natively with no granule emulation.
 *                       Otherwise use the granule emulation (as `force`) and
 *                       say why in one line.
 *   native            - no granule emulation, whatever the p_align.
 *   force             - granule emulation with the configured SMCChecks.
 *   degrade           - granule emulation, and force SMCChecks=full, which is
 *                       the one SMC mode whose correctness does not depend on
 *                       host-page protection granularity.
 *   abort             - explain the granule emulation's limits and refuse to
 *                       start (fastppcx86's default for x86 guests).
 *
 * FEX_HOSTPAGEMODE is read as a fallback, and FEX_ALLOW_UNSUPPORTED_PAGE_SIZE=1
 * is kept as an alias for `force`. None of this does anything on a 4K host.
 *
 * Written straight to stderr rather than through LogMan on purpose. Silent
 * logging defaults to on and every LogMan path is swallowed when it is; the
 * gate's messages must not be silenceable by the default logging config.
 */
enum class Mode {
  Auto,
  Native,
  Abort,
  Degrade,
  Force,
};

inline Mode ParseMode(std::string_view Value) {
  if (Value == "auto") {
    return Mode::Auto;
  }
  if (Value == "native") {
    return Mode::Native;
  }
  if (Value == "degrade") {
    return Mode::Degrade;
  }
  if (Value == "force") {
    return Mode::Force;
  }
  // "abort", and anything unrecognised: refusing with an explanation is the
  // safe reading of a typo.
  return Mode::Abort;
}

// Explicit reports whether the mode came from config or the environment (true) or
// from the caller-supplied default (false).
inline Mode GetMode(bool ConfigAvailable, Mode DefaultMode, bool* Explicit) {
  *Explicit = true;
  // The config layer is only consulted when the caller says it exists: FEXCore::Config's
  // meta layer is a null global until Initialize() runs, and this gate is deliberately
  // callable from before that point.
  if (ConfigAvailable) {
    if (auto Value = FEXCore::Config::Get(FEXCore::Config::CONFIG_HOSTPAGEMODE); Value && *Value && !(*Value)->empty()) {
      return ParseMode(std::string_view {(*Value)->c_str()});
    }
  }

  if (const char* Env = ::getenv("FEX_HOSTPAGEMODE")) {
    return ParseMode(std::string_view {Env});
  }

  if (const char* Legacy = ::getenv("FEX_ALLOW_UNSUPPORTED_PAGE_SIZE"); Legacy && Legacy[0] == '1') {
    return Mode::Force;
  }

  *Explicit = false;
  return DefaultMode;
}

/**
 * @brief Call first thing in every tool that hosts guest code.
 *
 * Handles every mode that can be decided before the guest ELF is read, and
 * returns the selected mode. On a 4K host it returns Native: there is nothing
 * to emulate. A caller that gets Auto back must call ResolveAuto() once it
 * knows the guest's PT_LOAD alignment, before the first guest mapping. A
 * caller that gets Native must turn the granule emulation off
 * (VMATracking::GranuleTable::DisableEmulation()).
 */
inline Mode CheckHostPageSize(bool ConfigAvailable = false, Mode DefaultMode = Mode::Auto) {
  const long HostPageSize = ::sysconf(_SC_PAGESIZE);
  if (HostPageSize <= 0 || static_cast<uint64_t>(HostPageSize) == FEXCore::Utils::FEX_GUEST_PAGE_SIZE) {
    // Either a 4K host, or sysconf failed and there is nothing meaningful to
    // say -- the rest of the tree already treats a failed _SC_PAGESIZE as
    // "assume 4K", so do not invent a second policy here.
    return Mode::Native;
  }

  bool Explicit = false;
  const Mode SelectedMode = GetMode(ConfigAvailable, DefaultMode, &Explicit);

  if (SelectedMode == Mode::Auto || SelectedMode == Mode::Native) {
    return SelectedMode;
  }

  if (!Explicit && SelectedMode == Mode::Force) {
    // The caller vouched for this lane: one line, not the bring-up banner.
    fextl::fmt::print(stderr, "POWERarm: host page size is {}; this lane is host-granular, continuing.\n", HostPageSize);
    return SelectedMode;
  }

  fextl::fmt::print(stderr,
                    "POWERarm: {}: host page size is {}, and HostPageMode={} selects the {}-byte granule\n"
                    "emulation on top of the larger host page (AT_PAGESZ still reports the host page;\n"
                    "docs/PAGE_SIZE_64K_EXECUTION.md):\n"
                    "  * loader, brk, ASLR and the allocators are host-granular (S2/S4a),\n"
                    "  * guest mmap/mprotect/munmap below the host page go through the granule table\n"
                    "    in the permissive tier: a granule is mapped/protected as the union of its\n"
                    "    live guest pages, so a guest cannot rely on a fault inside a shared granule (S4b),\n"
                    "  * SMC tracking arms nothing under the default SMCChecks=icache (the guest's own\n"
                    "    IC IVAU is the invalidation), and under SMCChecks=mtrack arms whole host pages\n"
                    "    and re-arms after a write to a shared granule (S4c).\n"
                    "\n"
                    "A guest that depends on sub-granule faults (GC write barriers, guard pages) may misbehave.\n"
                    "\n"
                    "POWERARM_HOSTPAGEMODE=auto (default) runs natively when every PT_LOAD of the program\n"
                    "and its interpreter has p_align >= the host page and emulates otherwise; =native never\n"
                    "emulates; =force emulates with the configured SMCChecks (icache or mtrack);\n"
                    "=degrade emulates and forces SMCChecks=full, which is several times slower;\n"
                    "=abort refuses to start.\n",
                    SelectedMode == Mode::Abort ? "FATAL" : "WARNING", HostPageSize,
                    SelectedMode == Mode::Abort ? "abort" : (SelectedMode == Mode::Degrade ? "degrade" : "force"),
                    FEXCore::Utils::FEX_GUEST_PAGE_SIZE);

  switch (SelectedMode) {
  case Mode::Degrade:
    // SMCChecks=full validates guest code before every run and never installs a host
    // write-protection, so it is the one SMC mode whose correctness does not depend on
    // the host protection granularity matching the guest's.
    if (ConfigAvailable) {
      FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "2" /* CONFIG_SMC_FULL */);
    }
    fextl::fmt::print(stderr, "POWERarm: HostPageMode=degrade -- continuing with SMCChecks forced to full.\n"
                              "Relaxed-correctness contract, stated once: guest protections finer than the host\n"
                              "page are tracked but not enforced, sub-page guard pages do not fault, and freed\n"
                              "sub-page memory stays resident. Do not report performance numbers from this mode.\n");
    return SelectedMode;
  case Mode::Force:
    fextl::fmt::print(stderr, "POWERarm: HostPageMode=force -- continuing with the granule emulation, forcing nothing.\n");
    return SelectedMode;
  case Mode::Abort:
  default: break;
  }

  fextl::fmt::print(stderr, "\nSet POWERARM_HOSTPAGEMODE=auto (or =force, =degrade) to continue.\n");
  FEX_TRAP_EXECUTION;
}

/**
 * @brief Decide Auto once the guest ELF has been read.
 *
 * SmallestAlign is the smallest PT_LOAD p_align of the program and its
 * interpreter, and File the path it came from. Returns true when the granule
 * emulation is needed. The native case prints nothing; the emulated case prints
 * one line saying why.
 */
inline bool ResolveAuto(uint64_t SmallestAlign, std::string_view File) {
  const uint64_t HostPageSize = FEXCore::HostPage::Size();
  if (SmallestAlign >= HostPageSize) {
    return false;
  }

  fextl::fmt::print(stderr,
                    "POWERarm: {} has a PT_LOAD with p_align {:#x}, below the {:#x} host page: emulating {}-byte pages "
                    "for this process (POWERARM_HOSTPAGEMODE=auto)\n",
                    File, SmallestAlign, HostPageSize, FEXCore::Utils::FEX_GUEST_PAGE_SIZE);
  return true;
}
} // namespace FEX::HostPageGate
