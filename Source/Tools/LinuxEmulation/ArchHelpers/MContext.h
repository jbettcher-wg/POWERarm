// SPDX-License-Identifier: MIT
// Signal context helpers — architecture-neutral dispatcher.
//
// Pulls in the per-arch implementation based on the compile-time architecture.
// Each arch file defines:
//   - ContextBackup struct and `using ContextBackup = ...`
//   - GetSp/SetSp, GetPc/SetPc, GetState/SetState
//   - GetArmReg/SetArmReg, GetArmGPRs, GetArmFPR, GetArmPState (or stubs)
//   - GetProtectFlags
//   - BackupContext<T> / RestoreContext<T>
#pragma once

#include "UContext.h"

#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Core/CoreState.h>

#include <signal.h>
#include <string.h>
#ifndef _WIN32
#include <ucontext.h>
#endif
#include <stdint.h>
#include <type_traits>

namespace FEX::ArchHelpers::Context {
#ifndef _WIN32

enum ContextFlags : uint32_t {
  CONTEXT_FLAG_INJIT = (1U << 0),
};

// Fault classification GetProtectFlags reports (the bit values of the x86 page
// fault error code this layer was written against).
// POWERARM-M0-TODO(signals): AArch64 describes the fault through ESR_EL1 (esr_context); map DSISR to an ESR data-abort syndrome instead.
enum ProtectFlagBits : uint32_t {
  PROTECT_FLAG_WRITE = (1U << 1),
  PROTECT_FLAG_USER = (1U << 2),
};

#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
constexpr uint64_t STACK_COOKIE_MAGIC = 0x4142434445464748ULL;
#endif

// ---------------------------------------------------------------------------
// Common helpers available on all architectures
// ---------------------------------------------------------------------------

static inline ucontext_t* GetUContext(void* ucontext) {
  return static_cast<ucontext_t*>(ucontext);
}

static inline mcontext_t* GetMContext(void* ucontext) {
  return &static_cast<ucontext_t*>(ucontext)->uc_mcontext;
}

// ---------------------------------------------------------------------------
// Per-architecture implementations
// ---------------------------------------------------------------------------
#if defined(ARCHITECTURE_ppc64le)
#  include "MContext_ppc64le.h"
#elif defined(ARCHITECTURE_x86_64)
#  include "MContext_x86_64.h"
#endif

#else // _WIN32

#endif // _WIN32
} // namespace FEX::ArchHelpers::Context
