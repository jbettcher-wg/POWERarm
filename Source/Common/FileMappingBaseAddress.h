// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/Utils/TypeDefines.h>
#include <FEXCore/fextl/vector.h>

#include <cstdint>
#include <span>

#include <elf.h>

namespace FEXCore {

/**
 * Infers the base virtual address from a file mapping (as described by parameters to a single
 * call to mmap()).
 *
 * Usually the base address can uniquely be inferred, but in edge cases multiple possible
 * candidates are returned.
 *
 * The file offset of any given mapping need not match its virtual address offset from the base
 * mapping (file offset = 0). Instead, this function searches the corresponding ELF program headers
 * for an entry that generated the given file mapping.
 */
inline fextl::vector<uint64_t>
InferMappingBaseAddress(std::span<const Elf64_Phdr> ProgramHeaders, uint64_t Addr, uint64_t Size, uint64_t FileOffset, int AccessFlags) {
  fextl::vector<uint64_t> Ret;
  for (auto& phdr : ProgramHeaders) {
    if (phdr.p_type != PT_LOAD) {
      // Skip headers that don't trigger memory mappings
      continue;
    }

    if ((phdr.p_flags & (PF_X | PF_W | PF_R)) != (AccessFlags & (PF_X | PF_W | PF_R))) {
      continue;
    }

    // The mapped file offset must be included at the start of the section header
    auto SegmentStartOffset = phdr.p_offset - (phdr.p_vaddr & 0xfff);
    // GUEST: guest ELF p_offset values are 4K-congruent by the x86 ABI; this compares
    // a guest ELF segment offset against the offset the mapping was made with.
    bool FromSegment = FileOffset >= SegmentStartOffset && FileOffset < SegmentStartOffset + phdr.p_filesz &&
                       (FileOffset & Utils::FEX_GUEST_PAGE_MASK) == (phdr.p_offset & Utils::FEX_GUEST_PAGE_MASK);
    if (!FromSegment && !HostPage::MatchesGuest() && HostPage::IsAligned(FileOffset)) {
      // HOST: on a larger host page a loader maps a host-congruent segment from
      // the host page that holds it -- the guest's ld.so (AT_PAGESZ is the host
      // page) and POWERarm's own ELF loader both do -- and the ELF loader may
      // start past host pages an earlier segment already materialised.
      FromSegment = FileOffset >= HostPage::AlignDown(phdr.p_offset) && FileOffset < phdr.p_offset + phdr.p_filesz;
    }
    if (FromSegment) {
      // Compute VA offset relative to the base mapping
      Ret.push_back(Addr - (phdr.p_vaddr - (phdr.p_offset & 0xfff)) + (ProgramHeaders[0].p_vaddr - (ProgramHeaders[0].p_offset & 0xfff)) -
                    (FileOffset - SegmentStartOffset));
    }
  }

  return Ret;
}
} // namespace FEXCore
