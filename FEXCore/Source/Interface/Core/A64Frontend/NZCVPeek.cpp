// SPDX-License-Identifier: MIT
// NZCV exit-deadness, stage 1. See NZCVPeek.h for what this is and why.
#include "Interface/Core/A64Frontend/NZCVPeek.h"
#include "Interface/Core/A64Frontend/DecodeTable.h"
#include "Interface/Core/A64Frontend/Decoder.h"
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/JIT/PPC64LE/NZCVExitCensus.h"
#include "Interface/IR/IR.h"
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/IntrusiveIRList.h"
#include "Interface/IR/Passes.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <cstdio>

namespace FEXCore::A64::NZCVPeek {

namespace Census = FEXCore::CPU::NZCVExitCensus;

// ===========================================================================
// §9 stage 1: the table, proven against the frontend.
// ===========================================================================
namespace {

  // What the frontend's translation of one guest word does to NZCV, computed
  // the way §7.2's verdict depends on it: a bit is READ only if nothing earlier
  // in the same translation wrote it (a read-then-write op like CCMP is a
  // reader), and WRITTEN is the union over the translation.
  struct FrontendFlags {
    unsigned Read {};
    unsigned Write {};
    bool Rejected {}; // the handler refused the encoding: the guest takes SIGILL here
  };

  constexpr unsigned kAllFlags = 0xF;

  // The classes the scan WALKS THROUGH. A missed reader in one of these is
  // unsound: the scan carries on (or, for a writer, ends the path DEAD) with a
  // live reader behind it. PARTIAL, CALL, RET and TERM all end the scan
  // UNRESOLVED, so a read the table missed there costs nothing.
  bool ScanPassesThrough(Census::WordClass C) {
    return C == Census::WC_NEUTRAL || C == Census::WC_WRITER || C == Census::WC_BRANCH || C == Census::WC_TWOWAY || C == Census::WC_SVC;
  }

  struct CheckState {
    FEXCore::Core::InternalThreadState* Thread;
    uint64_t ScratchPC;
    uint32_t Unsound;
    uint32_t Conservative;
    uint32_t Checked;
    uint32_t Skipped;
  };

  // Kept across calls so the per-word check does not allocate.
  fextl::vector<uint8_t> Brackets;

  FrontendFlags TranslateOne(FEXCore::Core::InternalThreadState* Thread, uint64_t PC, uint32_t Word, const InstMatcher* Matcher) {
    using namespace FEXCore::IR;
    auto* Builder = Thread->OpDispatcher.get();

    fextl::vector<FEXCore::A64::Decoder::DecodedBlocks> Blocks;
    FEXCore::A64::Decoder::DecodedInst Inst {};
    Inst.PC = PC;
    Inst.Word = Word;
    Inst.Matcher = Matcher;
    auto& B = Blocks.emplace_back();
    B.Entry = PC;
    B.Size = FEXCore::A64::INSTRUCTION_SIZE;
    B.NumInstructions = 1;
    B.DecodedInstructions = &Inst;
    B.BlockStatus = FEXCore::A64::Decoder::DecodedBlockStatus::SUCCESS;
    B.IsEntryPoint = true;

    Builder->ResetWorkingList();
    Builder->BeginFunction(PC, &Blocks, 1);
    Builder->SetNewBlockIfChanged(PC);
    Builder->StartNewBlock();
    Builder->TranslateInstruction(Inst);
    Builder->FinishOp(PC + FEXCore::A64::INSTRUCTION_SIZE, true);
    Builder->Finalize();

    FrontendFlags F {};
    auto IR = Builder->ViewIR();

    // ---- Pass 1: cancel the NZCV PRESERVE BRACKET.
    //
    // The frontend wraps every guest op whose PPC64LE lowering clobbers CR0 --
    // the whole LDXR/STXR/CAS/LD<op> family, whose `stwcx.`/`ldarx` record into
    // CR0 -- in `LoadNZCV ... StoreNZCV`, so the guest's flags come back
    // unchanged (NZCV-LIVENESS.md §11 finding 4 is this exact pairing). In
    // DFCE's model that bracket reads all four bits and writes all four, and
    // from the guest's point of view it does neither. The peek's table is a
    // GUEST-level table and calls STXR neutral, which is correct; a checker
    // comparing against DFCE's model without cancelling the bracket would flag
    // every atomic as unsound and be useless.
    //
    // MarkNZCVPreserveBrackets is DeadFlagCalculationElimination's own, shared
    // so that this check and the pass cannot disagree about what a real guest
    // flag read is.
    MarkNZCVPreserveBrackets(IR, Brackets);

    // ---- Pass 2: the guest-visible classification.
    unsigned Undefined = kAllFlags;
    for (auto [BlockNode, BlockHeader] : IR.GetBlocks()) {
      for (auto [CodeNode, IROp] : IR.GetCode(BlockNode)) {
        if (IROp->Op == OP_BREAK) {
          // The handler refused the encoding, or the word is a real BRK/HLT/UDF.
          F.Rejected = true;
        }
        const uint32_t ID = IR.GetID(CodeNode).Value;
        if (ID < Brackets.size() && Brackets[ID] != 0) {
          continue;
        }
        const unsigned R = IROpNZCVRead(IROp);
        const unsigned W = IROpNZCVWrite(IROp);
        F.Read |= R & Undefined;
        F.Write |= W;
        Undefined &= ~W;
      }
    }
    return F;
  }

  void CheckEntry(void* Opaque, const InstMatcher& M) {
    auto* S = static_cast<CheckState*>(Opaque);
    if (!M.Handler) {
      ++S->Skipped;
      return;
    }

    // Several words per entry: the fixed bits from the table, the free bits
    // from a deterministic sequence (deterministic so a failure reproduces).
    // Two of them are the all-zero and all-one fills, which between them hit
    // the size/condition/opc fields the flag behaviour usually keys off.
    static constexpr uint32_t kFills[] = {0x0000'0000u, 0xFFFF'FFFFu, 0x5555'5555u, 0xAAAA'AAAAu,
                                          0x1234'5678u, 0x9E37'79B9u, 0x0F1E'2D3Cu, 0xC0DE'BA5Eu};
    for (uint32_t Fill : kFills) {
      const uint32_t Word = M.Expect | (Fill & ~M.Mask);

      // Classify the word the way the frontend will see it: whichever table
      // entry actually wins, not necessarily M. That is the comparison that
      // matters -- the peek classifies words and so does the decoder.
      const auto* Winner = DecodeInstruction(Word);
      if (!Winner || !Winner->Handler) {
        ++S->Skipped;
        continue;
      }

      const Census::WordClass C = Census::ClassifyGuestWord(Word);
      const FrontendFlags F = TranslateOne(S->Thread, S->ScratchPC, Word, Winner);
      if (F.Rejected) {
        // The handler refused this encoding (a reserved field value, an
        // unimplemented sysreg) and the frontend raises SIGILL, or the word is
        // a genuine BRK/HLT/UDF. Either way control leaves at this word, which
        // the table already treats as ending the scan or ending the path, and
        // the flag behaviour of an instruction that never completes is not a
        // property the table can be measured against.
        ++S->Skipped;
        continue;
      }
      ++S->Checked;

      bool Unsound = false;
      const char* Why = "";
      if (F.Read && ScanPassesThrough(C)) {
        Unsound = true;
        Why = "table says the scan may walk through this word, but the frontend reads NZCV at it";
      } else if (C == Census::WC_WRITER && (F.Write & kAllFlags) != kAllFlags) {
        Unsound = true;
        Why = "table says this word writes all four NZCV bits, but the frontend does not";
      }

      if (Unsound) {
        ++S->Unsound;
        if (S->Unsound <= 64) {
          fextl::fmt::print(stderr, "NZCV_TABLECHECK UNSOUND word={:#010x} entry={} decoded={} class={} read={:x} write={:x} why=\"{}\"\n",
                            Word, M.Name, Winner->Name, Census::WordClassName(C), F.Read, F.Write, Why);
        }
        continue;
      }

      // Conservative divergences cost value, not correctness. Counted and
      // printed once each so the table can be tightened later, never fatal.
      const char* Loose = nullptr;
      if (C == Census::WC_READER && !F.Read) {
        Loose = "table calls this a reader, the frontend reads nothing";
      } else if (C == Census::WC_PARTIAL && (F.Write & kAllFlags) == kAllFlags) {
        Loose = "table calls this a partial writer, the frontend writes all four";
      }
      if (Loose) {
        ++S->Conservative;
        if (S->Conservative <= 24) {
          fextl::fmt::print(stderr, "NZCV_TABLECHECK conservative word={:#010x} entry={} decoded={} class={} read={:x} write={:x} why=\"{}\"\n",
                            Word, M.Name, Winner->Name, Census::WordClassName(C), F.Read, F.Write, Loose);
        }
      }
    }
  }

} // namespace

uint32_t RunTableCheck(FEXCore::Core::InternalThreadState* Thread) {
  // The scratch PC must be readable memory: IRBuilder::HLT reads 32 bytes at
  // PC+4 when the word is the thunk marker, and every synthesised HLT word
  // reaches it. A static buffer is the cheapest guarantee; nothing executes
  // from it.
  alignas(64) static uint8_t Scratch[64] {};

  CheckState S {};
  S.Thread = Thread;
  S.ScratchPC = reinterpret_cast<uint64_t>(Scratch);

  ForEachTableEntry(&S, &CheckEntry);

  const auto Stats = GetDecodeTableStats();
  fextl::fmt::print(stderr, "NZCV_TABLECHECK checked={} handled_entries={} table_entries={} unsound={} conservative={} skipped={}\n", S.Checked,
                    Stats.HandledEntries, Stats.Entries, S.Unsound, S.Conservative, S.Skipped);
  return S.Unsound;
}

} // namespace FEXCore::A64::NZCVPeek
