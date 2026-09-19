// SPDX-License-Identifier: MIT
//
// AArch64 guest signal-frame types, as the arm64 Linux kernel lays them out
// (arch/arm64/include/uapi/asm/sigcontext.h, ucontext.h; asm-generic signal.h).
// These are what a guest handler sees through its ucontext_t* argument.
//
// SetupFrame_Arm64 puts an fpsimd_context record in __reserved[] and
// RestoreFrame_Arm64 reads it back.
// POWERARM-M0-TODO(signals): no esr_context record yet (the kernel adds one for faults with an ESR, e.g. data aborts).
#pragma once

#include <FEXCore/Utils/CompilerDefs.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <signal.h>

namespace FEXCore::arm64 {
// sigaltstack(2) stack_t: LP64, same layout as the host's.
struct stack_t {
  void* ss_sp;
  int32_t ss_flags;
  size_t ss_size;
};
static_assert(sizeof(stack_t) == 24, "arm64 stack_t is 24 bytes");
static_assert(offsetof(stack_t, ss_size) == 16, "arm64 stack_t layout");

// Header of every record in sigcontext::__reserved[].
struct _aarch64_ctx {
  uint32_t magic;
  uint32_t size;
};
static_assert(sizeof(_aarch64_ctx) == 8);

constexpr uint32_t FPSIMD_MAGIC = 0x46508001;
constexpr uint32_t ESR_MAGIC = 0x45535201;
constexpr uint32_t EXTRA_MAGIC = 0x45585401;

struct fpsimd_context {
  _aarch64_ctx head;
  uint32_t fpsr;
  uint32_t fpcr;
  __uint128_t vregs[32];
};
static_assert(sizeof(fpsimd_context) == 0x210, "arm64 fpsimd_context is 0x210 bytes");

struct esr_context {
  _aarch64_ctx head;
  uint64_t esr;
};
static_assert(sizeof(esr_context) == 0x10);

struct sigcontext {
  uint64_t fault_address;
  uint64_t regs[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
  alignas(16) uint8_t __reserved[4096];
};
static_assert(offsetof(sigcontext, pstate) == 272, "arm64 sigcontext layout");
static_assert(offsetof(sigcontext, __reserved) == 288, "arm64 sigcontext __reserved is 16-byte aligned");
static_assert(sizeof(sigcontext) == 4384, "arm64 sigcontext is 4384 bytes");

// The fpsimd_context record in a frame's __reserved[] list, or nullptr when the
// list has none or is malformed. The list is the guest's memory: every record
// must fit, and a record's size is a multiple of 16, as the kernel's
// parse_user_sigframe requires. The walk stops at the terminating null record.
inline const fpsimd_context* FindFPSIMDContext(const sigcontext& sc) {
  constexpr size_t Limit = sizeof(sigcontext::__reserved);
  size_t Offset = 0;
  while (Offset + sizeof(_aarch64_ctx) <= Limit) {
    _aarch64_ctx Head;
    memcpy(&Head, sc.__reserved + Offset, sizeof(Head));
    if (Head.magic == 0 || Head.size < sizeof(Head) || Head.size % 16 != 0 || Head.size > Limit - Offset) {
      return nullptr;
    }
    if (Head.magic == FPSIMD_MAGIC) {
      return Head.size == sizeof(fpsimd_context) ? reinterpret_cast<const fpsimd_context*>(sc.__reserved + Offset) : nullptr;
    }
    Offset += Head.size;
  }
  return nullptr;
}

struct ucontext_t {
  uint64_t uc_flags;
  ucontext_t* uc_link;
  stack_t uc_stack;
  uint64_t uc_sigmask; // kernel sigset_t, 64 signals
  uint8_t __unused[1024 / 8 - sizeof(uint64_t)];
  sigcontext uc_mcontext;
};
static_assert(offsetof(ucontext_t, uc_sigmask) == 40, "arm64 ucontext_t layout");
static_assert(offsetof(ucontext_t, uc_mcontext) == 176, "arm64 ucontext_t layout");
static_assert(sizeof(ucontext_t) == 4560, "arm64 ucontext_t is 4560 bytes");

// siginfo_t is LP64 and 128 bytes on both arm64 and powerpc64, with the same
// union layout for every si_code the kernel generates, so the host type is used
// directly.
static_assert(sizeof(::siginfo_t) == 128, "siginfo_t is 128 bytes");

// rt_sigframe as arch/arm64/kernel/signal.c builds it: siginfo, then ucontext,
// followed by the frame record (fp, lr).
struct rt_sigframe {
  ::siginfo_t info;
  ucontext_t uc;
};

struct frame_record {
  uint64_t fp;
  uint64_t lr;
};
} // namespace FEXCore::arm64
