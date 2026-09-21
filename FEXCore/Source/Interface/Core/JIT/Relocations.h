// SPDX-License-Identifier: MIT
#pragma once
#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/CompilerDefs.h>

namespace FEXCore::Context {
class ContextImpl;
}

namespace FEXCore::CPU {
enum class RelocationTypes : uint32_t {
  // 8 byte literal in memory for symbol
  // Aligned to struct RelocNamedSymbolLiteral
  RELOC_NAMED_SYMBOL_LITERAL,

  // Fixed size named thunk move
  // 4 instruction constant generation
  // Aligned to struct RelocNamedThunkMove
  RELOC_NAMED_THUNK_MOVE,

  // 8 byte literal (relative to binary base address)
  RELOC_GUEST_RIP_LITERAL,

  // Guest RIP move: fixed 5-instruction window, or the recorded width
  // Aligned to struct RelocGuestRIP
  RELOC_GUEST_RIP_MOVE,

  // A block-link record (PPC64BlockLinkRecord) and the two instruction words
  // its linker patches. Nothing is written on load: the stored bytes are the
  // unlinked form. On storage the copy is returned to that form, because the
  // live buffer being serialized may already be linked (the patched words are
  // PC-relative branches into other blocks of this process).
  RELOC_LINK_RECORD,
};

struct FEX_PACKED RelocationHeader final {
  // Offset to the relocated host code data
  uint64_t Offset {};

  RelocationTypes Type;
};

struct RelocNamedSymbolLiteral final {
  enum class NamedSymbol : uint32_t {
    ///< Thread specific relocations
    // JIT Literal pointers
    SYMBOL_LITERAL_EXITFUNCTION_LINKER,
    // The dispatcher stub a link thunk's unlinked leg reaches through its
    // record (PPC64BlockLinkRecord::StubAddr).
    SYMBOL_LITERAL_EXITFUNCTION_LINKER_WITH_RECORD,
  };

  RelocationHeader Header {};

  NamedSymbol Symbol;

  uint32_t Pad[8];
};

struct RelocNamedThunkMove final {
  RelocationHeader Header {};

  // GPR index the constant is being moved to
  uint32_t RegisterIndex;

  // The thunk SHA256 hash
  IR::SHA256Sum Symbol;
};

struct RelocGuestRIP final {
  RelocationHeader Header {};

  // GPR index the constant is being moved to (for non-literal relocations)
  uint8_t RegisterIndex;

  // RELOC_GUEST_RIP_MOVE: instructions emitted for the load. 0 means the fixed
  // 5-instruction LoadConstantFixed window; otherwise the loader re-emits a
  // variable-width LoadConstant into this many instructions, padded with nops.
  uint8_t Instructions;

  char Pad[2];

  // The base RIP (to be moved by the register for non-literal relocations).
  // In a serialized code cache, this is relative to the binary base address.
  uint64_t GuestRIP;

  uint32_t pad2[6] {};
};

struct RelocLinkRecord final {
  RelocationHeader Header {};

  // Offsets of the patched words from the record (Header.Offset), and the
  // words they hold while unlinked.
  int32_t CallerDelta;
  int32_t ThunkDelta;
  uint32_t OrigCallerWord;
  uint32_t OrigThunkWord;

  int32_t LinkBranchDelta;
  int32_t LinkedEntryDelta;
  int32_t FinalDelta;
  uint32_t FinalPlainBranch;
  uint32_t Pad[1] {};
};
static_assert(sizeof(RelocLinkRecord) == 48, "RelocLinkRecord size contract");


union Relocation {
  // Clang 16 Can't default-initialize this union
  static Relocation Default() {
#if __clang_major__ < 17
    Relocation Ret {.Header {}};
    memset(&Ret, 0, sizeof(Ret));
    return Ret;
#else
    return {};
#endif
  }

  RelocationHeader Header {};

  RelocNamedSymbolLiteral NamedSymbolLiteral;
  // This makes our union of relocations at least 48 bytes
  // It might be more efficient to not use a union
  RelocNamedThunkMove NamedThunkMove;

  RelocGuestRIP GuestRIP;
  RelocLinkRecord LinkRecord;
};

uint64_t GetNamedSymbolLiteral(FEXCore::Context::ContextImpl&, RelocNamedSymbolLiteral::NamedSymbol);

} // namespace FEXCore::CPU
