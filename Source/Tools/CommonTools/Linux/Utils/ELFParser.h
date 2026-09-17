// SPDX-License-Identifier: MIT
#pragma once
#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <elf.h>
#include <fcntl.h>
#include <optional>
#include <unistd.h>

#include "Linux/Utils/ELFContainer.h"

/*
  Simpler elf parser, checks for the elf MAGIC COOKIE
  and loads the phdrs
  Also keeps an fd open
*/

struct ELFParser {
  Elf64_Ehdr ehdr;
  fextl::vector<Elf64_Phdr> phdrs;
  std::optional<fextl::vector<Elf64_Shdr>> shdrs;
  ::ELFLoader::ELFContainer::ELFType type {::ELFLoader::ELFContainer::TYPE_NONE};

  fextl::string InterpreterElf;
  int fd {-1};

  bool ReadElf(int NewFD) {
    Closefd();
    static_assert(EI_CLASS == 4);

    fd = NewFD;
    type = ::ELFLoader::ELFContainer::TYPE_NONE;
    shdrs.reset();

    if (fd == -1) {
      // Likely just doesn't exist
      return false;
    }

    // Get file size
    off_t Size = lseek(fd, 0, SEEK_END);

    if (Size < 4) {
      // Likely invalid can't fit header
      return false;
    }

    // Reset to beginning
    if (lseek(fd, 0, SEEK_SET) == -1) {
      return false;
    }

    uint8_t header[EI_NIDENT];
    if (pread(fd, header, sizeof(header), 0) != sizeof(header)) {
      LogMan::Msg::EFmt("Failed to read elf header from '{}'", fd);
      return false;
    }

    if (header[0] != ELFMAG0 || header[1] != ELFMAG1 || header[2] != ELFMAG2 || header[3] != ELFMAG3) {
      LogMan::Msg::EFmt("Elf header from '{}' doesn't match ELF MAGIC", fd);
      return false;
    }

    type = ::ELFLoader::ELFContainer::TYPE_OTHER_ELF;

    if (header[EI_CLASS] != ELFCLASS64) {
      LogMan::Msg::EFmt("Unsupported ELF from '{}': EI_CLASS {} is not ELFCLASS64", fd, header[EI_CLASS]);
      return false;
    }

    if (header[EI_DATA] != ELFDATA2LSB) {
      LogMan::Msg::EFmt("Unsupported ELF from '{}': EI_DATA {} is not ELFDATA2LSB", fd, header[EI_DATA]);
      return false;
    }

    if (pread(fd, &ehdr, sizeof(ehdr), 0) == -1) {
      LogMan::Msg::EFmt("Failed to read Ehdr64 from '{}'", fd);
      return false;
    }

    // do the sizes match up as expected?

    // check elf header
    if (ehdr.e_ehsize != sizeof(ehdr)) {
      LogMan::Msg::EFmt("Invalid e_ehsize64 from '{}'", fd);
      return false;
    }

    // check program header
    if (ehdr.e_phentsize != sizeof(Elf64_Phdr)) {
      LogMan::Msg::EFmt("Invalid e_phentsize64 from '{}'", fd);
      return false;
    }

    // POWERarm runs AArch64 Linux ELFs only: 64-bit, little-endian, EM_AARCH64, executable or shared object.
    if (ehdr.e_machine != EM_AARCH64) {
      LogMan::Msg::EFmt("Unsupported ELF from '{}': e_machine {} is not EM_AARCH64 ({})", fd, ehdr.e_machine, EM_AARCH64);
      return false;
    }

    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
      LogMan::Msg::EFmt("Unsupported ELF from '{}': e_type {} is neither ET_EXEC nor ET_DYN", fd, ehdr.e_type);
      return false;
    }

    type = ::ELFLoader::ELFContainer::TYPE_AARCH64;

    // sanity check program header count
    if (ehdr.e_phnum < 1 || ehdr.e_phnum > 65536 / ehdr.e_phentsize) {
      LogMan::Msg::EFmt("Too many program headers '{}'", fd);
      return false;
    }

    // sanity check program header offset size.
    if (ehdr.e_phoff > Size || (ehdr.e_phentsize * ehdr.e_phnum) > (Size - ehdr.e_phoff)) {
      LogMan::Msg::EFmt("Program headers exceeds size of program");
      return false;
    }

    phdrs.resize(ehdr.e_phnum);

    if (pread(fd, phdrs.data(), sizeof(Elf64_Phdr) * ehdr.e_phnum, ehdr.e_phoff) == -1) {
      LogMan::Msg::EFmt("Failed to read phdr64 from '{}'", fd);
      return false;
    }

    for (const auto& phdr : phdrs) {
      if (phdr.p_type == PT_INTERP) {
        InterpreterElf.resize(phdr.p_filesz);

        if (pread(fd, InterpreterElf.data(), phdr.p_filesz, phdr.p_offset) == -1) {
          LogMan::Msg::EFmt("Failed to read interpreter from '{}'", fd);
          return false;
        }
      }
    }

    return true;
  }

  ptrdiff_t FileToVA(off_t FileOffset) const {
    for (const auto& phdr : phdrs) {
      if (phdr.p_offset <= FileOffset && (phdr.p_offset + phdr.p_filesz) > FileOffset) {
        auto SectionFileOffset = FileOffset - phdr.p_offset;

        if (SectionFileOffset < phdr.p_memsz) {
          return SectionFileOffset + phdr.p_vaddr;
        }
      }
    }

    return {};
  }

  off_t VAToFile(ptrdiff_t VAOffset) const {
    for (const auto& phdr : phdrs) {
      if (phdr.p_vaddr <= VAOffset && (phdr.p_vaddr + phdr.p_memsz) > VAOffset) {
        auto SectionVAOffset = VAOffset - phdr.p_vaddr;

        if (SectionVAOffset < phdr.p_filesz) {
          return SectionVAOffset + phdr.p_offset;
        }
      }
    }

    return {};
  }

  bool ReadElf(const fextl::string& file) {
    int NewFD = ::open(file.c_str(), O_RDONLY);

    return ReadElf(NewFD);
  }

  /**
   * Checks if DT_TEXTREL/DF_TEXTREL exist in the PT_DYNAMIC segment.
   *
   * These indicate that the ELF has relocations that cover to read-only code
   * pages. The dynamic loader will temporarily map these pages as writeable
   * to apply the relocations.
   */
  bool HasCodeRelocations() const {
    if (fd == -1) {
      return false;
    }

    auto phdr_it = std::ranges::find_if(phdrs, [](auto& phdr) { return phdr.p_type == PT_DYNAMIC; });
    if (phdr_it == phdrs.end()) {
      return false;
    }

    return HasCodeRelocations<Elf64_Dyn>(*phdr_it);
  }

  template<typename Elf_Dyn>
  bool HasCodeRelocations(const Elf64_Phdr& phdr) const {
    const size_t EntryCount = phdr.p_filesz / sizeof(Elf_Dyn);
    fextl::vector<Elf_Dyn> Entries(EntryCount);

    if (pread(fd, Entries.data(), phdr.p_filesz, phdr.p_offset) == -1) {
      return false;
    }

    for (auto& Entry : Entries) {
      if (Entry.d_tag == DT_NULL) {
        break;
      }
      if (Entry.d_tag == DT_TEXTREL) {
        return true;
      }
      if (Entry.d_tag == DT_FLAGS && (Entry.d_un.d_val & DF_TEXTREL)) {
        return true;
      }
    }

    return false;
  }

  /**
   * Parses relocation sections (SHT_REL/SHT_RELA) and returns a map of
   * offsets to relocations that FEX's JIT must know about.
   */
  fextl::robin_map<uint32_t, FEXCore::GuestRelocationType> PopulateRelocations() {
    if (fd == -1 || !EnsureSectionHeadersLoaded()) {
      return {};
    }

    fextl::robin_map<uint32_t, FEXCore::GuestRelocationType> Relocations;

    for (const auto& shdr : *shdrs) {
      if (shdr.sh_entsize == 0) {
        continue;
      }

      const size_t EntryCount = shdr.sh_size / shdr.sh_entsize;

      if (shdr.sh_type == SHT_REL) {
        LOGMAN_THROW_A_FMT(false, "Unexpected relocation section type");
      } else if (shdr.sh_type == SHT_RELA) {
        fextl::vector<Elf64_Rela> Entries(EntryCount);
        if (pread(fd, Entries.data(), shdr.sh_size, shdr.sh_offset) == -1) {
          LOGMAN_THROW_A_FMT(false, "Failed to read RELA section");
        }
        for (auto& Entry : Entries) {
          auto RelocType = ClassifyRelocation64(ELF64_R_TYPE(Entry.r_info));
          if (RelocType) {
            Relocations.emplace(static_cast<uint32_t>(Entry.r_offset), *RelocType);
          }
        }
      }
    }

    return Relocations;
  }

  void Closefd() {
    if (fd != -1) {
      close(fd);
      fd = -1;
    }
  }

  ~ELFParser() {
    Closefd();
  }

private:
  /// Returns true if loading section headers succeeded
  bool EnsureSectionHeadersLoaded() {
    if (shdrs.has_value()) {
      return !shdrs->empty();
    }

    if (fd == -1 || ehdr.e_shoff == 0 || ehdr.e_shnum == 0) {
      shdrs.emplace();
      return false;
    }

    shdrs.emplace(ehdr.e_shnum);
    if (pread(fd, shdrs->data(), sizeof(Elf64_Shdr) * ehdr.e_shnum, ehdr.e_shoff) == -1) {
      shdrs->clear();
      return false;
    }

    return !shdrs->empty();
  }

  static std::optional<FEXCore::GuestRelocationType> ClassifyRelocation64(uint32_t Type) {
    // POWERARM-M0-TODO(smc): code-cache relocation classes were chosen for x86-64 data relocations; re-check which AArch64 relocations can land in cached code ranges.
    if (Type == R_AARCH64_RELATIVE || Type == R_AARCH64_ABS64) {
      return FEXCore::GuestRelocationType::Rel64;
    } else if (Type == R_AARCH64_ABS32) {
      return FEXCore::GuestRelocationType::Rel32;
    }
    return std::nullopt;
  }
};
