// SPDX-License-Identifier: MIT
#pragma once

// ---------------------------------------------------------------------------
// File mappings whose alignment the host kernel cannot represent (64K port, S4)
// ---------------------------------------------------------------------------
// x86 toolchains emit p_align = 0x1000 and the x86 mmap2 ABI counts file
// offsets in 4096-byte units, so a guest file mapping is only ever guaranteed
// to be 4K-congruent. A host mmap requires `offset % hostpage == 0` (and, for
// MAP_FIXED, `addr % hostpage == 0`). On a host whose page is larger than the
// guest's, the request has to be emulated: establish the containing host pages
// as private anonymous memory, pread the file bytes into place, then apply the
// requested protection at host granularity.
//
// The cost is that such a mapping is no longer shared with the page cache.
// That is an RSS cost, not a semantics cost (design Part 2 section 3), and it
// only applies on a host whose page size differs from the guest's -- every
// predicate here short-circuits to "no fallback" when they match, so the 4K
// build never reaches any of this.
// ---------------------------------------------------------------------------

#include <FEXCore/Utils/TypeDefines.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <vector>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace FEX::HostPageMapping {

/**
 * @brief Can this mapping request be handed to the host kernel verbatim?
 *
 * Anything anonymous can: the kernel rounds the length up itself and the
 * guest-visible address it returns is host-aligned, which is also 4K-aligned,
 * which is all the guest ABI promises. Only file offsets and MAP_FIXED
 * addresses carry a congruence the host cannot satisfy.
 */
[[nodiscard]]
inline bool RequiresFallback(uint64_t Addr, uint64_t Offset, int Flags, int FD) {
  if (FEXCore::HostPage::MatchesGuest()) {
    // The overwhelmingly common case, and the shipping 4K build's only case.
    return false;
  }

  if ((Flags & MAP_ANONYMOUS) || FD == -1) {
    return false;
  }

  if (!FEXCore::HostPage::IsAligned(Offset)) {
    return true;
  }

  // A non-fixed mapping lets the kernel pick a host-aligned address, so only a
  // caller-dictated address can be unrepresentable.
  if ((Flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && !FEXCore::HostPage::IsAligned(Addr)) {
    return true;
  }

  return false;
}

/**
 * @brief pread a whole range, retrying short reads and EINTR.
 *
 * A short read at EOF is not an error: mmap past the end of a file gives zero
 * bytes, and the destination here is freshly anonymous (or has been explicitly
 * zeroed by the caller), so stopping early reproduces that. Returns false only
 * on a real I/O error.
 */
inline bool ReadFully(int FD, void* Dest, size_t Size, uint64_t Offset) {
  auto* Out = static_cast<uint8_t*>(Dest);
  while (Size) {
    const ssize_t Read = ::pread(FD, Out, Size, static_cast<off_t>(Offset));
    if (Read < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (Read == 0) {
      // EOF; the remainder stays zero, matching mmap-past-EOF.
      return true;
    }
    Out += Read;
    Offset += Read;
    Size -= static_cast<size_t>(Read);
  }
  return true;
}


// ---------------------------------------------------------------------------
// Ranges a fallback turned into anonymous memory
// ---------------------------------------------------------------------------
// A MAP_PRIVATE file mapping keeps its file underneath it: MADV_DONTNEED (and
// MADV_FREE, and MADV_REMOVE) drop the private copy and the next access reads
// the file again. Anonymous memory has no file underneath, so the same advice
// zeroes it for good.
//
// The fallback above hands the guest anonymous memory where it asked for a file
// mapping, which makes those advices destructive where the guest is entitled to
// expect them to be free. Bun's standalone executables do exactly this: they
// serve their embedded bundle out of their own image and MADV_DONTNEED the part
// they have finished with, and 40MB of JavaScript became NUL bytes
// (POWERARM bunbytes). Record what each fallback covers so the advice can put
// the file bytes back, which is what the guest would have seen.
struct FallbackRange {
  uint64_t Start {};
  uint64_t End {}; ///< exclusive, and only as far as the file reaches
  int FD {-1};     ///< our own dup, alive as long as the range is
  uint64_t Offset {};
};

inline std::mutex FallbackRangesLock;
inline std::vector<FallbackRange> FallbackRanges;

/**
 * @brief Record that [Addr, Addr + Length) is anonymous memory standing in for
 * a file mapping. Length is the file-backed part only: anything past the end of
 * the file really is anonymous and a destructive advice may zero it.
 */
inline void RegisterFallbackRange(uint64_t Addr, uint64_t Length, int FD, uint64_t Offset) {
  if (!Length || FD < 0) {
    return;
  }
  const int Dup = ::fcntl(FD, F_DUPFD_CLOEXEC, 0);
  if (Dup < 0) {
    return;
  }
  std::lock_guard Lock {FallbackRangesLock};
  FallbackRanges.push_back(FallbackRange {Addr, Addr + Length, Dup, Offset});
}

/**
 * @brief Drop whatever [Addr, Addr + Length) covers, because the guest has
 * unmapped it or mapped something else over it. A range that is only partly
 * covered keeps the parts that survive.
 */
inline void ForgetFallbackRange(uint64_t Addr, uint64_t Length) {
  if (!Length) {
    return;
  }
  const uint64_t End = Addr + Length;
  std::lock_guard Lock {FallbackRangesLock};
  std::vector<FallbackRange> Split;
  auto It = FallbackRanges.begin();
  while (It != FallbackRanges.end()) {
    if (It->End <= Addr || It->Start >= End) {
      ++It;
      continue;
    }
    if (It->Start < Addr) {
      Split.push_back(FallbackRange {It->Start, Addr, ::fcntl(It->FD, F_DUPFD_CLOEXEC, 0), It->Offset});
    }
    if (It->End > End) {
      Split.push_back(FallbackRange {End, It->End, ::fcntl(It->FD, F_DUPFD_CLOEXEC, 0), It->Offset + (End - It->Start)});
    }
    ::close(It->FD);
    It = FallbackRanges.erase(It);
  }
  for (auto& Entry : Split) {
    if (Entry.FD >= 0) {
      FallbackRanges.push_back(Entry);
    }
  }
}

/**
 * @brief Is this madvise one the guest expects to be free on a file mapping?
 */
[[nodiscard]]
inline bool AdviceDiscardsPrivateCopy(int Advice) {
  return Advice == MADV_DONTNEED || Advice == MADV_FREE || Advice == MADV_REMOVE;
}

/**
 * @brief Put the file bytes back over whatever the advice just threw away.
 *
 * Called after the host madvise has been applied, so the pages are fresh and
 * the pread is the only thing that has written them. A range with nothing
 * registered under it costs one lock and a scan.
 */
inline void RestoreAfterDiscard(uint64_t Addr, uint64_t Length) {
  const uint64_t End = Addr + Length;
  std::lock_guard Lock {FallbackRangesLock};
  for (const auto& Entry : FallbackRanges) {
    const uint64_t From = std::max(Entry.Start, Addr);
    const uint64_t To = std::min(Entry.End, End);
    if (From >= To) {
      continue;
    }
    ReadFully(Entry.FD, reinterpret_cast<void*>(From), To - From, Entry.Offset + (From - Entry.Start));
  }
}

/**
 * @brief Emulate one unrepresentable file mapping.
 *
 * MmapFn:     void*(void* Addr, size_t Length, int Prot, int Flags, int FD, off_t Offset)
 * MprotectFn: uint64_t(void* Addr, size_t Length, int Prot)
 *
 * Both are supplied by the caller so the mapping is made through whatever layer
 * owns the address space (the guest syscall handler, the 32-bit allocator, ...)
 * rather than behind its back.
 *
 * Only valid for MAP_PRIVATE. MAP_SHARED at an unrepresentable alignment cannot
 * be emulated by copying -- the caller must reject it (design Part 2 section 2).
 *
 * Returns the guest-visible address, or a negative errno encoded as a pointer
 * (the convention FEX::HLE::HasSyscallError() decodes).
 */
template<typename MmapFn, typename MprotectFn>
inline void* MapFilePrivate(MmapFn&& Mmap, MprotectFn&& Mprotect, void* Addr, size_t Length, int Prot, int Flags, int FD, uint64_t Offset) {
  const uint64_t Guest = reinterpret_cast<uint64_t>(Addr);
  const bool Fixed = (Flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;

  // Reserve the host pages that back the request. Without a fixed address the
  // kernel picks, and the result is host-aligned by construction, so the guest
  // address is simply the reservation base.
  const uint64_t HostStart = Fixed ? FEXCore::HostPage::AlignDown(Guest) : 0;
  const uint64_t HostLength = Fixed ? FEXCore::HostPage::AlignUp(Guest + Length) - HostStart : FEXCore::HostPage::AlignUp(Length);

  const int AnonFlags = (Flags & ~(MAP_SHARED | MAP_SHARED_VALIDATE | MAP_DENYWRITE)) | MAP_PRIVATE | MAP_ANONYMOUS;

  // Write permission is needed for the pread regardless of what the caller
  // asked for; the requested protection is applied afterwards.
  void* Base = Mmap(reinterpret_cast<void*>(HostStart), HostLength, PROT_READ | PROT_WRITE, AnonFlags, -1, 0);
  if (reinterpret_cast<uint64_t>(Base) >= -4095ULL) {
    return Base;
  }

  const uint64_t Result = Fixed ? Guest : reinterpret_cast<uint64_t>(Base);
  if (!ReadFully(FD, reinterpret_cast<void*>(Result), Length, Offset)) {
    return reinterpret_cast<void*>(static_cast<int64_t>(-errno));
  }
  RegisterFallbackRange(Result, Length, FD, Offset);

  if (Prot != (PROT_READ | PROT_WRITE)) {
    // Host granularity: a protection finer than the host page cannot be
    // installed, so the whole reservation takes the requested one. The
    // reservation is private to this mapping, so nothing else is affected.
    Mprotect(reinterpret_cast<void*>(FEXCore::HostPage::AlignDown(Result)), HostLength, Prot);
  }

  return reinterpret_cast<void*>(Result);
}

} // namespace FEX::HostPageMapping
