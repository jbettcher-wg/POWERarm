// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/fextl/string.h>

#include <cstdint>

namespace FEX::RootFSCheck {
// e_machine values that show up in a rootfs mix-up.  EM_AARCH64 is the only one
// POWERarm can run as a guest.
constexpr uint16_t EM_NONE_ = 0;
constexpr uint16_t EM_386_ = 3;
constexpr uint16_t EM_PPC64_ = 21;
constexpr uint16_t EM_ARM_ = 40;
constexpr uint16_t EM_X86_64_ = 62;
constexpr uint16_t EM_AARCH64_ = 183;
constexpr uint16_t EM_RISCV_ = 243;

enum class Verdict {
  // The tree is there and its own userspace is AArch64.
  OK,
  // The tree is there and is not obviously wrong, but no probe binary could be
  // read, so the guest architecture was never established.  Not fatal: an
  // unusual-but-valid tree must still run.
  UNVERIFIED,
  // Nothing at that path.
  MISSING,
  // A directory with no entries in it.
  EMPTY,
  // A probe binary was read and it is not AArch64.  This is the x86 rootfs case.
  WRONG_MACHINE,
  // A file (an unmounted squashfs/erofs image) or something that is not a directory.
  NOT_A_DIRECTORY,
};

struct Result {
  Verdict Verdict {Verdict::UNVERIFIED};
  // e_machine that was read, 0 when none could be.
  uint16_t Machine {EM_NONE_};
  // Host path of the file `Machine` came from, empty when none.
  fextl::string Probe {};
  // One line of human-readable diagnosis.  Always set when Verdict != OK.
  fextl::string Reason {};
};

// A verdict that must stop the process rather than let guest paths resolve
// against the host filesystem.
bool IsFatal(enum Verdict V);

// "AArch64", "x86-64", ... or "e_machine <n>" for anything unnamed.
const char* MachineName(uint16_t Machine);

// Walks `Relative` inside `Base` the way the guest kernel would: an absolute symlink
// target is re-rooted at `Base` instead of at the host's /, so a rootfs whose
// /bin/sh -> /usr/bin/bash resolves to the guest's bash and never to the host's.
// Returns an empty string when it does not resolve inside the tree.
fextl::string ResolveInsideRootFS(const fextl::string& Base, const fextl::string& Relative);

// Reads the ELF machine type of a tree's own userspace (/usr/bin/env, /bin/sh, ...).
// Returns 0 when it cannot be established cheaply.  `ProbeOut`, when given, receives
// the host path the answer came from.
uint16_t DetectMachine(const fextl::string& RootFS, fextl::string* ProbeOut = nullptr);

// The whole check: exists, is a non-empty directory, and its userspace is AArch64.
Result Check(const fextl::string& RootFS);
} // namespace FEX::RootFSCheck
