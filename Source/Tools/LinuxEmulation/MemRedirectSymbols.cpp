// SPDX-License-Identifier: MIT
/*
$info$
tags: LinuxSyscalls|memredirect
desc: Resolves a mapped ELF's memcpy/memmove/memset for the libc mem-routine redirect
$end_info$
*/

// Symbol resolution for the libc mem-routine redirect (THUNKS-DESIGN.md 4.2).
// This is the "symbol-level redirect" half of 4.2's argument: there is no library
// thunk, no packer and no guest stub here, only the guest ADDRESSES of three
// routines, which the frontend then prefixes with a size gate.
//
// The walker is the one from VDSO_Emulation.cpp's LoadGuestVDSOSymbols -- both
// symbol tables, .dynsym and .symtab -- with two differences that matter:
//
//  * It reads through pread, not through the mapping. Section headers live at
//    FILE offsets and are normally not mapped at all (nothing loads a PT_LOAD
//    covering .shstrtab), so the vDSO's read-it-from-memory approach cannot work
//    for a file on disk. .symtab in particular is never mapped.
//  * It does not skip STV_HIDDEN symbols. The vDSO walker does, but in static
//    glibc every concrete implementation (__memcpy_generic and friends) is
//    GLOBAL HIDDEN, and those are precisely the ones that have to be hooked.
//
// IFUNCs are the reason this file is longer than a name comparison.
//
// In the SHARED ALARM glibc 2.43, memcpy@@GLIBC_2.17, memmove and memset are
// plain FUNC symbols and the exported address IS the routine. In a STATICALLY
// linked binary -- which is every binary in unittests/A64Frontend -- all three
// are instead IFUNC, and the symbol's st_value is the RESOLVER, a short function
// that reads hwcaps and returns a pointer. Hooking that address would hook the
// resolver, so a redirected call would run the gate against the resolver's own
// arguments and then "return" the fill value. The resolver's result lands in a
// GOT slot that is only written at startup, so it cannot be read here either.
//
// What is hooked instead is every concrete implementation: __memcpy_generic,
// __memcpy_sve, __memcpy_a64fx, __memcpy_mops, __memcpy_oryon1, __memset_zva64,
// and the rest of the family. Whichever one the resolver picks is the address the
// IPLT stub actually branches to, so hooking all of them hooks the one that runs
// and leaves the others unreachable. That is also why the _chk variants are
// refused by name: __memcpy_chk takes a fourth argument (the destination's known
// size) and is a different function, so giving it memcpy's three-argument
// reading would silently drop its bounds check.

#include "MemRedirectSymbols.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/vector.h>

#include <array>
#include <cstring>
#include <elf.h>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace FEX::HLE::MemRedirectSymbols {

namespace {
  // A symbol table section and the string table it points at.
  struct TableRef {
    uint64_t Offset {};
    uint64_t Size {};
    uint64_t EntSize {};
    uint64_t StrOffset {};
    uint64_t StrSize {};
  };

  bool ReadExact(int FD, void* Dest, size_t Size, off_t Offset) {
    size_t Done = 0;
    while (Done < Size) {
      const auto Result = pread(FD, static_cast<char*>(Dest) + Done, Size - Done, Offset + static_cast<off_t>(Done));
      if (Result <= 0) {
        return false;
      }
      Done += static_cast<size_t>(Result);
    }
    return true;
  }

  // Only the main executable and libc are worth reading. Scanning every DSO a
  // desktop app maps would be a few hundred stray preads for nothing: these three
  // routines live in libc, and in a static binary in the executable itself.
  bool PathIsInteresting(std::string_view Path, uint16_t ElfType) {
    if (ElfType == ET_EXEC) {
      // The main executable. Statically linked, this is where all three live;
      // dynamically linked, the scan simply finds nothing and costs one read.
      return true;
    }
    const auto Slash = Path.rfind('/');
    const auto Base = Slash == std::string_view::npos ? Path : Path.substr(Slash + 1);
    return Base.starts_with("libc.so") || Base.starts_with("libc-");
  }

  // Which routine a symbol name names, if any.
  //
  // Two shapes are accepted. The exact exported name, which is what the shared
  // libc gives; and the __<name>_<impl> family, which is what an IFUNC's concrete
  // implementations are called. Everything else, including the _chk variants and
  // the __libc_*_ifunc resolvers themselves, is refused.
  FEXCore::Context::MemRedirectKind ClassifySymbol(std::string_view Name) {
    using Kind = FEXCore::Context::MemRedirectKind;
    struct Row {
      std::string_view Exact;
      std::string_view ImplPrefix;
      Kind Which;
    };
    static constexpr std::array<Row, 3> Rows {{
      {"memset", "__memset_", Kind::Memset},
      {"memcpy", "__memcpy_", Kind::Memcpy},
      {"memmove", "__memmove_", Kind::Memmove},
    }};

    for (const auto& Row : Rows) {
      if (Name == Row.Exact) {
        return Row.Which;
      }
      if (Name.starts_with(Row.ImplPrefix)) {
        const auto Suffix = Name.substr(Row.ImplPrefix.size());
        if (Suffix.empty()) {
          continue;
        }
        // __memcpy_chk has a fourth argument; __libc_memcpy_ifunc-shaped names
        // are resolvers. Neither is this routine.
        if (Suffix == "chk" || Suffix == "ifunc") {
          continue;
        }
        return Row.Which;
      }
    }
    return Kind::None;
  }

  void WalkTable(FEXCore::Context::Context* CTX, int FD, uint64_t Bias, const TableRef& Table, std::string_view Path, uint32_t& Hooked) {
    if (!Table.EntSize || Table.EntSize < sizeof(Elf64_Sym) || !Table.Size || !Table.StrSize) {
      return;
    }

    fextl::vector<char> StrTab;
    StrTab.resize(Table.StrSize);
    if (!ReadExact(FD, StrTab.data(), StrTab.size(), static_cast<off_t>(Table.StrOffset))) {
      return;
    }
    // A name read out of here is indexed by st_name, which the file controls, so
    // force a terminator rather than trusting the table to carry one.
    StrTab.back() = '\0';

    const uint64_t Count = Table.Size / Table.EntSize;
    for (uint64_t i = 0; i < Count; ++i) {
      Elf64_Sym Symbol {};
      if (!ReadExact(FD, &Symbol, sizeof(Symbol), static_cast<off_t>(Table.Offset + i * Table.EntSize))) {
        return;
      }

      // An IFUNC's st_value is its RESOLVER, never the routine, so it is refused
      // outright -- see the file header. The concrete implementations carry
      // STT_FUNC and are picked up by name instead.
      if (ELF64_ST_TYPE(Symbol.st_info) != STT_FUNC) {
        continue;
      }
      // st_value/st_shndx only: an UNDEF symbol is an import with no address
      // here, which is what a dynamically linked executable's own .dynsym entry
      // for memcpy looks like. st_size is deliberately NOT required to be
      // non-zero -- glibc's ENTRY/END macros always emit .size, but a libc whose
      // routines are hand-written assembly without one would otherwise have the
      // feature silently do nothing, which is a worse surprise than hooking an
      // alias that points at the same bytes.
      if (Symbol.st_value == 0 || Symbol.st_shndx == SHN_UNDEF) {
        continue;
      }
      if (Symbol.st_name == 0 || Symbol.st_name >= StrTab.size()) {
        continue;
      }

      const std::string_view Name {&StrTab[Symbol.st_name]};
      const auto Kind = ClassifySymbol(Name);
      if (Kind == FEXCore::Context::MemRedirectKind::None) {
        continue;
      }

      const uint64_t GuestAddr = Bias + Symbol.st_value;
      CTX->AddMemRedirect(GuestAddr, Kind);
      ++Hooked;
      LogMan::Msg::DFmt("MemRedirect: hooked {} at {:#x} ({})", Name, GuestAddr, Path);
    }
  }
} // namespace

void ScanMappedELF(FEXCore::Context::Context* CTX, bool Enabled, int FD, uint64_t MapAddr, uint64_t MapOffset, std::string_view Path) {
  if (!Enabled || !CTX || FD < 0) {
    return;
  }

  Elf64_Ehdr Header {};
  if (!ReadExact(FD, &Header, sizeof(Header), 0)) {
    return;
  }
  if (Header.e_ident[EI_MAG0] != ELFMAG0 || Header.e_ident[EI_MAG1] != ELFMAG1 || Header.e_ident[EI_MAG2] != ELFMAG2 ||
      Header.e_ident[EI_MAG3] != ELFMAG3) {
    return;
  }
  // The guest's architecture, not the host's: these are AArch64 routine addresses.
  if (Header.e_ident[EI_CLASS] != ELFCLASS64 || Header.e_machine != EM_AARCH64) {
    return;
  }
  if (Header.e_type != ET_EXEC && Header.e_type != ET_DYN) {
    return;
  }
  if (!PathIsInteresting(Path, Header.e_type)) {
    return;
  }
  if (!Header.e_shoff || !Header.e_shnum || Header.e_shentsize < sizeof(Elf64_Shdr)) {
    // Section headers stripped. .dynsym is still reachable through PT_DYNAMIC,
    // but nothing in the corpus needs that and a half-correct second reader is
    // worse than resolving nothing: refuse and leave the routines translated.
    return;
  }

  // Load bias. st_value is a link-time virtual address, and the guest address is
  // that plus however far this image was displaced.
  //
  // Derived from the program headers rather than assumed, so the same code is
  // right for an ET_EXEC loaded at its own addresses (where it works out to zero)
  // and for an ET_DYN the guest's ld.so put wherever it liked. The segment that
  // contains this mapping's file offset is the one that fixes the relationship.
  uint64_t Bias = 0;
  if (Header.e_phoff && Header.e_phnum && Header.e_phentsize >= sizeof(Elf64_Phdr)) {
    bool Found = false;
    uint64_t LowestVaddr = ~0ull;
    for (uint64_t i = 0; i < Header.e_phnum; ++i) {
      Elf64_Phdr Phdr {};
      if (!ReadExact(FD, &Phdr, sizeof(Phdr), static_cast<off_t>(Header.e_phoff + i * Header.e_phentsize))) {
        return;
      }
      if (Phdr.p_type != PT_LOAD) {
        continue;
      }
      LowestVaddr = std::min(LowestVaddr, Phdr.p_vaddr);
      if (MapOffset >= Phdr.p_offset && MapOffset < Phdr.p_offset + Phdr.p_filesz) {
        Bias = MapAddr - (Phdr.p_vaddr + (MapOffset - Phdr.p_offset));
        Found = true;
        break;
      }
    }
    if (!Found) {
      if (LowestVaddr == ~0ull) {
        return;
      }
      Bias = MapAddr - LowestVaddr;
    }
  }

  // Section headers, then both symbol tables.
  fextl::vector<Elf64_Shdr> Sections;
  Sections.resize(Header.e_shnum);
  {
    // e_shentsize may exceed sizeof(Elf64_Shdr) in principle, so read entry by
    // entry at its own stride rather than slurping the array.
    for (uint64_t i = 0; i < Header.e_shnum; ++i) {
      if (!ReadExact(FD, &Sections[i], sizeof(Elf64_Shdr), static_cast<off_t>(Header.e_shoff + i * Header.e_shentsize))) {
        return;
      }
    }
  }

  uint32_t Hooked = 0;
  for (const auto WantType : {SHT_DYNSYM, SHT_SYMTAB}) {
    for (const auto& Section : Sections) {
      if (Section.sh_type != static_cast<uint32_t>(WantType)) {
        continue;
      }
      if (Section.sh_link >= Sections.size()) {
        continue;
      }
      const auto& StrSection = Sections[Section.sh_link];
      if (StrSection.sh_type != SHT_STRTAB) {
        continue;
      }
      WalkTable(CTX, FD, Bias,
                TableRef {
                  .Offset = Section.sh_offset,
                  .Size = Section.sh_size,
                  .EntSize = Section.sh_entsize,
                  .StrOffset = StrSection.sh_offset,
                  .StrSize = StrSection.sh_size,
                },
                Path, Hooked);
    }
  }

  if (Hooked) {
    LogMan::Msg::DFmt("MemRedirect: {} routine(s) hooked in {} (bias {:#x})", Hooked, Path, Bias);
  }
}

} // namespace FEX::HLE::MemRedirectSymbols
