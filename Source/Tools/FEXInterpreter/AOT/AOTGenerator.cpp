// SPDX-License-Identifier: MIT
/*
 * Ahead-of-time translation into the code cache (POWERARM_AOTTRANSLATE).
 *
 * The process runs the normal startup (config, host features, syscall handler,
 * ELF loader) for the file named on the command line, so the cache file name
 * (FileId from the mapped file, ConfigId from this very executable and its
 * options) is exactly the one a later run of the same POWERarm computes. It
 * then compiles every statically discovered entry instead of executing the
 * guest, and writes the blocks through the normal runtime writer
 * (SaveCodeCaches: append-only segments, locks, size cap).
 *
 * Nothing about correctness changes: stored blocks are validated on install
 * (entry hash, guest bytes, relocations) like any other cached block. A seed
 * that is not really code only costs cache space.
 *
 * Seeds, all restricted to executable PT_LOAD ranges and 4-aligned:
 *  - the ELF entry point;
 *  - .eh_frame_hdr FDE starts (every function with unwind info; present in
 *    stripped binaries too);
 *  - STT_FUNC / STT_GNU_IFUNC symbols from .symtab and .dynsym;
 *  - unless the mode is "entries", the return point after every BL whose
 *    target is a known function start, and after every BLR. Blocks end at
 *    calls, so a return point is its own compile unit at run time;
 *  - in mode "all" (the default), also the targets of direct branches (B,
 *    B.cond, CB[N]Z, TB[N]Z) farther than the decoder's region window.
 * Jump-table targets and other indirect-only entries are not found; they are
 * compiled at run time as before.
 *
 * Libraries are loaded as the main ELF here, at a different base than ld.so
 * would choose. Cached guest addresses are relative to the file's load base,
 * so this only matters when a rebased address needs a wider LoadConstant; the
 * install then rejects the block (reloc-failed) and it is compiled normally.
 */
#include "AOT/AOTGenerator.h"

#include "LinuxSyscalls/Syscalls.h"

#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/vector.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <elf.h>
#include <optional>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace FEX::AOT {
namespace {
  // DWARF pointer encodings (.eh_frame_hdr).
  constexpr uint8_t DW_EH_PE_udata4 = 0x03;
  constexpr uint8_t DW_EH_PE_udata8 = 0x04;
  constexpr uint8_t DW_EH_PE_sdata4 = 0x0b;
  constexpr uint8_t DW_EH_PE_sdata8 = 0x0c;
  constexpr uint8_t DW_EH_PE_datarel = 0x30;

  struct LoadRange {
    uint64_t VAddr;
    uint64_t FileSize;
    uint64_t Offset;
    bool Exec;
  };

  uint64_t NowNS() {
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
  }

  class Image {
  public:
    explicit Image(fextl::vector<uint8_t> Bytes)
      : Data {std::move(Bytes)} {}

    bool Parse() {
      if (Data.size() < sizeof(Elf64_Ehdr)) {
        return false;
      }
      memcpy(&Ehdr, Data.data(), sizeof(Ehdr));
      if (memcmp(Ehdr.e_ident, ELFMAG, SELFMAG) != 0 || Ehdr.e_ident[EI_CLASS] != ELFCLASS64 || Ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
          Ehdr.e_machine != EM_AARCH64 || Ehdr.e_phentsize != sizeof(Elf64_Phdr)) {
        return false;
      }
      for (uint32_t i = 0; i < Ehdr.e_phnum; ++i) {
        Elf64_Phdr P;
        if (!Read(Ehdr.e_phoff + uint64_t(i) * sizeof(P), &P, sizeof(P))) {
          return false;
        }
        if (P.p_type == PT_LOAD) {
          Loads.push_back({P.p_vaddr, P.p_filesz, P.p_offset, (P.p_flags & PF_X) != 0});
        } else if (P.p_type == PT_GNU_EH_FRAME) {
          EHFrameHdr = P;
          HasEHFrameHdr = true;
        }
      }
      return true;
    }

    bool Read(uint64_t Offset, void* Out, size_t Size) const {
      if (Offset > Data.size() || Size > Data.size() - Offset) {
        return false;
      }
      memcpy(Out, Data.data() + Offset, Size);
      return true;
    }

    bool InExec(uint64_t VAddr) const {
      for (const auto& L : Loads) {
        if (L.Exec && VAddr >= L.VAddr && VAddr + 4 <= L.VAddr + L.FileSize) {
          return true;
        }
      }
      return false;
    }

    void AddFunc(uint64_t VAddr) {
      if ((VAddr & 3) == 0 && InExec(VAddr)) {
        Funcs.push_back(VAddr);
      }
    }

    // Only the encodings binutils/lld emit for AArch64 are handled.
    std::optional<int64_t> ReadEncoded(uint64_t& Offset, uint8_t Enc) const {
      switch (Enc & 0x0f) {
      case DW_EH_PE_udata4: {
        uint32_t V;
        if (!Read(Offset, &V, 4)) {
          return std::nullopt;
        }
        Offset += 4;
        return int64_t(V);
      }
      case DW_EH_PE_sdata4: {
        int32_t V;
        if (!Read(Offset, &V, 4)) {
          return std::nullopt;
        }
        Offset += 4;
        return int64_t(V);
      }
      case DW_EH_PE_udata8:
      case DW_EH_PE_sdata8: {
        int64_t V;
        if (!Read(Offset, &V, 8)) {
          return std::nullopt;
        }
        Offset += 8;
        return V;
      }
      default: return std::nullopt;
      }
    }

    void CollectEHFrameHdr() {
      if (!HasEHFrameHdr) {
        return;
      }
      uint8_t H[4];
      uint64_t Off = EHFrameHdr.p_offset;
      if (!Read(Off, H, 4) || H[0] != 1) {
        return;
      }
      Off += 4;
      if (!ReadEncoded(Off, H[1])) { // eh_frame_ptr
        return;
      }
      auto Count = ReadEncoded(Off, H[2]);
      // Table: pairs of (initial location, FDE address), data-relative to the
      // start of .eh_frame_hdr.
      if (!Count || H[3] != (DW_EH_PE_datarel | DW_EH_PE_sdata4)) {
        return;
      }
      for (int64_t i = 0; i < *Count; ++i) {
        int32_t Entry[2];
        if (!Read(Off, Entry, sizeof(Entry))) {
          return;
        }
        Off += sizeof(Entry);
        AddFunc(EHFrameHdr.p_vaddr + int64_t(Entry[0]));
      }
    }

    void CollectSymbols() {
      if (Ehdr.e_shentsize != sizeof(Elf64_Shdr)) {
        return;
      }
      for (uint32_t i = 0; i < Ehdr.e_shnum; ++i) {
        Elf64_Shdr S;
        if (!Read(Ehdr.e_shoff + uint64_t(i) * sizeof(S), &S, sizeof(S))) {
          return;
        }
        if ((S.sh_type != SHT_SYMTAB && S.sh_type != SHT_DYNSYM) || S.sh_entsize != sizeof(Elf64_Sym)) {
          continue;
        }
        for (uint64_t Off = S.sh_offset; Off + sizeof(Elf64_Sym) <= S.sh_offset + S.sh_size; Off += sizeof(Elf64_Sym)) {
          Elf64_Sym Sym;
          if (!Read(Off, &Sym, sizeof(Sym))) {
            break;
          }
          const auto Type = ELF64_ST_TYPE(Sym.st_info);
          if ((Type == STT_FUNC || Type == STT_GNU_IFUNC) && Sym.st_shndx != SHN_UNDEF && Sym.st_value != 0) {
            AddFunc(Sym.st_value);
          }
        }
      }
    }

    // Return points after calls, and with Branches the far targets of direct
    // branches: the decoder follows targets within 128 bytes of the branch
    // (Decoder.cpp RegionWindow) into the same unit, anything farther is an
    // exit and its own compile unit at run time.
    fextl::vector<uint64_t> CollectReturnPoints(bool Branches) const {
      fextl::vector<uint64_t> Out;
      for (const auto& L : Loads) {
        if (!L.Exec || L.Offset > Data.size()) {
          continue;
        }
        const uint64_t Size = std::min<uint64_t>(L.FileSize, Data.size() - L.Offset) & ~uint64_t {3};
        for (uint64_t i = 0; i + 4 < Size; i += 4) {
          uint32_t W;
          memcpy(&W, Data.data() + L.Offset + i, 4);
          const uint64_t PC = L.VAddr + i;
          bool Call = false;
          if ((W & 0xFC000000) == 0x94000000) {
            // BL: only trust it when it lands on a known function start.
            int64_t Imm = int64_t(uint64_t(W & 0x03FFFFFF) << 38) >> 36;
            Call = std::binary_search(Funcs.begin(), Funcs.end(), PC + Imm);
          } else if ((W & 0xFFFFFC1F) == 0xD63F0000) {
            // BLR: only inside the span covered by known functions.
            Call = !Funcs.empty() && PC > Funcs.front();
          }
          if (Call) {
            Out.push_back(PC + 4);
            continue;
          }
          if (!Branches || Funcs.empty() || PC < Funcs.front()) {
            continue;
          }
          int64_t Imm = 0;
          if ((W & 0xFC000000) == 0x14000000) {
            // B imm26
            Imm = int64_t(uint64_t(W & 0x03FFFFFF) << 38) >> 36;
          } else if ((W & 0xFF000010) == 0x54000000 || (W & 0x7E000000) == 0x34000000) {
            // B.cond, CBZ/CBNZ: imm19 at bit 5
            Imm = int64_t(uint64_t((W >> 5) & 0x7FFFF) << 45) >> 43;
          } else if ((W & 0x7E000000) == 0x36000000) {
            // TBZ/TBNZ: imm14 at bit 5
            Imm = int64_t(uint64_t((W >> 5) & 0x3FFF) << 50) >> 48;
          } else {
            continue;
          }
          if ((Imm > 128 || Imm < -128) && InExec(PC + Imm)) {
            Out.push_back(PC + Imm);
          }
        }
      }
      return Out;
    }

    Elf64_Ehdr Ehdr {};
    fextl::vector<LoadRange> Loads;
    fextl::vector<uint64_t> Funcs;

  private:
    fextl::vector<uint8_t> Data;
    Elf64_Phdr EHFrameHdr {};
    bool HasEHFrameHdr = false;
  };
} // namespace

int TranslateMainElf(FEXCore::Context::Context* CTX, FEX::HLE::SyscallHandler* Handler, FEXCore::Core::InternalThreadState* Thread,
                     int ElfFD, uint64_t LoadBias, std::string_view Mode) {
  const uint64_t StartNS = NowNS();
  // Background work: stay out of the way of anything interactive.
  setpriority(PRIO_PROCESS, 0, 19);

  char Path[PATH_MAX];
  auto PathLen = readlink(fextl::fmt::format("/proc/self/fd/{}", ElfFD).c_str(), Path, sizeof(Path) - 1);
  if (PathLen < 0) {
    fextl::fmt::print(stderr, "POWERarm AOT: cannot resolve the main ELF\n");
    return 1;
  }
  Path[PathLen] = 0;

  if (!Handler->CodeCacheWriteEnabled() || !Handler->IsPathInCodeCacheScope(Path)) {
    fextl::fmt::print(stderr, "POWERarm AOT: {}: not in code cache scope (is the cache enabled and the file under the RootFS?)\n", Path);
    return 1;
  }

  fextl::vector<uint8_t> Bytes;
  {
    struct stat St {};
    if (fstat(ElfFD, &St) != 0) {
      return 1;
    }
    Bytes.resize(St.st_size);
    size_t Done = 0;
    while (Done < Bytes.size()) {
      auto R = pread(ElfFD, Bytes.data() + Done, Bytes.size() - Done, Done);
      if (R <= 0) {
        fextl::fmt::print(stderr, "POWERarm AOT: {}: read failed\n", Path);
        return 1;
      }
      Done += R;
    }
  }

  Image Elf {std::move(Bytes)};
  if (!Elf.Parse()) {
    fextl::fmt::print(stderr, "POWERarm AOT: {}: not an AArch64 ELF64\n", Path);
    return 1;
  }
  Elf.AddFunc(Elf.Ehdr.e_entry);
  Elf.CollectEHFrameHdr();
  Elf.CollectSymbols();
  std::sort(Elf.Funcs.begin(), Elf.Funcs.end());
  Elf.Funcs.erase(std::unique(Elf.Funcs.begin(), Elf.Funcs.end()), Elf.Funcs.end());

  fextl::vector<uint64_t> Seeds = Elf.Funcs;
  const size_t NumFuncs = Seeds.size();
  if (Mode != "entries") {
    auto Returns = Elf.CollectReturnPoints(Mode != "calls");
    Seeds.insert(Seeds.end(), Returns.begin(), Returns.end());
    std::sort(Seeds.begin(), Seeds.end());
    Seeds.erase(std::unique(Seeds.begin(), Seeds.end()), Seeds.end());
  }

  size_t Done = 0;
  for (uint64_t Seed : Seeds) {
    CTX->CompileRIP(Thread, LoadBias + Seed);
    if ((++Done & 0xfff) == 0) {
      // Writes a segment every 50000 new blocks, like a long-running guest.
      Handler->MaybeSaveCodeCaches(Thread);
    }
  }
  Handler->SaveCodeCaches(Thread, true);

  // The periodic saves above leave up to eight segments. Fold them into one so
  // the first runtime append does not have to compact this namespace.
  if (!Seeds.empty()) {
    if (auto Section = Handler->LookupExecutableFileSection(Thread, LoadBias + Seeds.front())) {
      CTX->GetCodeCache().CompactAllSegments(Handler->CodeCacheBasePath(Section->FileInfo), Section->FileInfo.FileId);
    }
  }

  fextl::fmt::print(stderr, "POWERarm AOT: {}: {} function entries, {} seeds, {:.2f} s\n", Path, NumFuncs, Seeds.size(),
                    double(NowNS() - StartNS) / 1e9);
  return 0;
}
} // namespace FEX::AOT
