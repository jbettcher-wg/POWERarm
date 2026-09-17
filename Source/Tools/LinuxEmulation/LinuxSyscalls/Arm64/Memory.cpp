// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|syscalls-arm64
desc: mmap family for the arm64 guest: flag translation and the guest VA limit
$end_info$
*/

#include "LinuxSyscalls/GranuleMemory.h"
#include "LinuxSyscalls/Syscalls.h"
#include "LinuxSyscalls/Arm64/ABITranslation.h"
#include "LinuxSyscalls/Arm64/GuestVA.h"
#include "LinuxSyscalls/Arm64/Syscalls.h"

#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace FEX::HLE::Arm64 {
using namespace FEX::HLE::Arm64::ABI;

namespace {
  // MAP_HUGE_* sizes live in the top bits and use the same encoding on both.
  const uint64_t GuestHugeBits = GUEST_MAP_HUGE_MASK << GUEST_MAP_HUGE_SHIFT;

  // prot bits an arm64 kernel accepts from mprotect for the presented CPU (no BTI, no MTE).
  const uint64_t GuestMprotectValid = GUEST_PROT_READ | GUEST_PROT_WRITE | GUEST_PROT_EXEC | GUEST_PROT_SEM;
  const uint64_t GuestProtGrows = GUEST_PROT_GROWSDOWN | GUEST_PROT_GROWSUP;
} // namespace

void RegisterMemory(FEX::HLE::SyscallHandler* Handler) {
  REGISTER_SYSCALL_IMPL(
    mmap, [](FEXCore::Core::CpuStateFrame* Frame, void* addr, size_t length, int prot, int flags, int fd, off_t offset) -> uint64_t {
      // arm64 mmap ignores prot bits the CPU does not support (PROT_BTI and
      // PROT_MTE here); powerpc would reject them, and PROT_BTI has the value
      // of powerpc's PROT_SAO, so they are dropped.
      const int HostProt = static_cast<int>(FlagsToHost(ProtFlags, static_cast<uint32_t>(prot)));

      uint64_t Unknown {};
      const uint64_t GuestFlags = static_cast<uint32_t>(flags);
      int HostFlags = static_cast<int>(FlagsToHost(MapFlags, GuestFlags & ~GuestHugeBits, &Unknown) | (GuestFlags & GuestHugeBits));
      if (Unknown && (GuestFlags & GUEST_MAP_TYPE) == GUEST_MAP_SHARED_VALIDATE) {
        return -EOPNOTSUPP;
      }

      // Guest VA limit. The kernel checks the length first, then a fixed
      // address; a hint that does not fit is simply not used.
      if (length > GuestVA::Limit()) {
        return -ENOMEM;
      }
      if (HostFlags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if (!GuestVA::RangeFits(reinterpret_cast<uint64_t>(addr), FEXCore::HostPage::AlignUp(length))) {
          return -ENOMEM;
        }
      } else if (addr && !GuestVA::RangeFits(reinterpret_cast<uint64_t>(addr), FEXCore::HostPage::AlignUp(length))) {
        addr = nullptr;
      }

      uint64_t Emulated {};
      if (FEX::HLE::Granule::Mmap(Frame->Thread, true, addr, length, HostProt, HostFlags, fd, offset, &Emulated)) {
        return Emulated;
      }
      return reinterpret_cast<uint64_t>(FEX::HLE::_SyscallHandler->GuestMmap(Frame->Thread, addr, length, HostProt, HostFlags, fd, offset));
    });

  REGISTER_SYSCALL_IMPL(munmap, [](FEXCore::Core::CpuStateFrame* Frame, void* addr, size_t length) -> uint64_t {
    if (!GuestVA::RangeFits(reinterpret_cast<uint64_t>(addr), length)) {
      return -EINVAL;
    }
    uint64_t Emulated {};
    if (FEX::HLE::Granule::Munmap(Frame->Thread, addr, length, &Emulated)) {
      return Emulated;
    }
    return FEX::HLE::_SyscallHandler->GuestMunmap(Frame->Thread, addr, length);
  });

  REGISTER_SYSCALL_IMPL(mprotect, [](FEXCore::Core::CpuStateFrame* Frame, void* addr, size_t len, int prot) -> uint64_t {
    // Same order as do_mprotect_pkey: both GROWS bits, then a zero length,
    // then the arch prot check, then the VMA lookup (ENOMEM above the limit).
    const uint64_t GuestProt = static_cast<uint32_t>(prot);
    if ((GuestProt & GuestProtGrows) == GuestProtGrows) {
      return -EINVAL;
    }
    if (len != 0) {
      if (GuestProt & ~(GuestMprotectValid | GuestProtGrows)) {
        return -EINVAL;
      }
      if (!GuestVA::RangeFits(reinterpret_cast<uint64_t>(addr), len)) {
        return -ENOMEM;
      }
    }
    const int HostProt = static_cast<int>(FlagsToHost(ProtFlags, GuestProt));

    uint64_t Emulated {};
    if (FEX::HLE::Granule::Mprotect(Frame->Thread, addr, len, HostProt, &Emulated)) {
      return Emulated;
    }
    return FEX::HLE::_SyscallHandler->GuestMprotect(Frame->Thread, addr, len, HostProt);
  });

  REGISTER_SYSCALL_IMPL(
    mremap, [](FEXCore::Core::CpuStateFrame* Frame, void* old_address, size_t old_size, size_t new_size, int flags, void* new_address) -> uint64_t {
      // MREMAP_* are identical on both (checked by the generator).
      if ((flags & MREMAP_FIXED) && !GuestVA::RangeFits(reinterpret_cast<uint64_t>(new_address), new_size)) {
        return -EINVAL;
      }
      if (!GuestVA::RangeFits(reinterpret_cast<uint64_t>(old_address), old_size)) {
        // No guest VMA can exist there.
        return -EFAULT;
      }
      // POWERARM-M1-TODO(syscalls): an in-place grow of a mapping that ends just below the guest limit is left to the host, which on a 64K kernel can extend it past 2^47 where an arm64 kernel would move it (MREMAP_MAYMOVE) or fail with ENOMEM.
      uint64_t Emulated {};
      if (FEX::HLE::Granule::Mremap(Frame->Thread, true, old_address, old_size, new_size, flags, new_address, &Emulated)) {
        return Emulated;
      }
      return FEX::HLE::_SyscallHandler->GuestMremap(true, Frame->Thread, old_address, old_size, new_size, flags, new_address);
    });

  REGISTER_SYSCALL_IMPL(mlockall, [](FEXCore::Core::CpuStateFrame* Frame, int flags) -> uint64_t {
    uint64_t Unknown {};
    const uint64_t HostFlags = FlagsToHost(MclFlags, static_cast<uint32_t>(flags), &Unknown);
    if (Unknown) {
      return -EINVAL;
    }
    uint64_t Result = ::syscall(SYSCALL_DEF(mlockall), HostFlags);
    SYSCALL_ERRNO();
  });
}
} // namespace FEX::HLE::Arm64
