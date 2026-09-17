// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|syscalls-arm64
desc: The guest user address-space limit
$end_info$
*/

// DESIGN.md §4.9: the guest sees the host's natural user window. That is 47
// bits on a 64K ppc64 kernel (where the kernel would honour hints up to 2^52)
// and 46 bits on a 4K one, i.e. the same layout as an arm64 VA_BITS=47 kernel,
// capped by what the host can map. The mmap family enforces it the way an
// arm64 kernel with that TASK_SIZE does: a hint above the limit is ignored, a
// fixed mapping above it fails with ENOMEM, munmap/mremap above it with EINVAL.
#pragma once

#include <FEXCore/Utils/Allocator.h>

#include <algorithm>
#include <cstdint>

namespace FEX::HLE::Arm64::GuestVA {
inline constexpr uint64_t MaxBits = 47;

inline uint64_t Bits() {
  static const uint64_t Value = std::min<uint64_t>(FEXCore::Allocator::DetermineVASize(), MaxBits);
  return Value;
}

inline uint64_t Limit() {
  return 1ULL << Bits();
}

// [Addr, Addr + Length) lies entirely below the guest limit.
inline bool RangeFits(uint64_t Addr, uint64_t Length) {
  const uint64_t Top = Limit();
  return Length <= Top && Addr <= Top - Length;
}
} // namespace FEX::HLE::Arm64::GuestVA
