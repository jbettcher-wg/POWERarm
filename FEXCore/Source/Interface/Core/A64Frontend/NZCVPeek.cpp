// SPDX-License-Identifier: MIT
// NZCV exit-deadness. See NZCVPeek.h for what this is and why it lives here.
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
#include <FEXCore/Utils/TypeDefines.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <cstdio>

#include <atomic>
#include <cstring>
#include <mutex>

namespace FEXCore::A64::NZCVPeek {

namespace Census = FEXCore::CPU::NZCVExitCensus;

namespace {

  // -------------------------------------------------------------------------
  // Guest word reads for the scan.
  //
  // Every read goes through the thread decoder's own CheckRangeExecutable, the
  // rule the translate-ahead helper uses, so the peek cannot fault where the
  // decoder would not have read. A64 instructions are 4-aligned; an unaligned
  // target is not code and is refused here rather than half-read.
  // -------------------------------------------------------------------------
  struct ReadContext {
    FEXCore::A64::Decoder* Dec;
  };

  bool ReadWord(void* Opaque, uint64_t Address, uint32_t* Out) {
    auto* Ctx = static_cast<ReadContext*>(Opaque);
    if (Address & 3) {
      return false;
    }
    if (!Ctx->Dec->PeekRangeExecutable(Address, FEXCore::A64::INSTRUCTION_SIZE)) {
      return false;
    }
    std::memcpy(Out, reinterpret_cast<const void*>(Address), sizeof(*Out));
    return true;
  }

  // -------------------------------------------------------------------------
  // §7.7's tripwire, moved from the linker to the compiler.
  //
  // The document put it on the direct/thunk link: a link from a unit with
  // ExitsAssumeNZCVDead into a target with EntryNZCVLiveIn is a contradiction
  // between the scan and the frontend's own translation of the target. What
  // survived contact with the code is the *meaning*, not the site. A link
  // record is reached by the FIRST traversal only, a linked exit never reaches
  // it again, call exits under SMCLAZYLINK and every exit under NOLINKFIRST
  // bypass it, and a shadow-RET arrival never sees it at all -- the same list
  // of holes §6 gives for putting the policy itself at link time. And a link
  // only happens when the successor has been compiled AND linking is on, so the
  // detector would be dark in exactly the configurations (MAXINST=1, linking
  // interlocked off) where the suite exercises the policy hardest.
  //
  // Compile time has none of those holes. Every DEAD verdict is recorded
  // against its target here, every compiled unit's entry is checked against the
  // record, and the two meet whatever the linker does -- including when the
  // target is never linked, or is reached only through the dispatcher, or is
  // compiled BEFORE the unit that assumes it. The record is bounded and
  // direct-mapped: a collision loses a detection, never invents one.
  //
  // It catches only targets that read NZCV within their own compile unit, so it
  // is a detector for table bugs, not a proof. The canary is the proof.
  // -------------------------------------------------------------------------
  constexpr uint32_t kTripwireSlots = 1u << 14;

  struct TripwireEntry {
    uint64_t Address {};
    uint8_t Flags {}; // bit0: some unit assumed NZCV dead here. bit1: this unit reads NZCV from entry.
  };

  std::mutex TripwireMutex;
  TripwireEntry TripwireTable[kTripwireSlots] {};
  std::atomic<uint64_t> Contradictions {};

  // The words at a contradicting target, and how the peek classified each. A
  // tripwire that only says "here" costs an hour per firing; one that prints
  // the eight words and their classes is read once. Best effort: a word that
  // cannot be read ends the list.
  fextl::string DescribeTarget(FEXCore::A64::Decoder* Dec, uint64_t Address) {
    fextl::string Out;
    ReadContext RC {Dec};
    for (uint32_t i = 0; i < 8; ++i) {
      uint32_t Insn = 0;
      if (!ReadWord(&RC, Address + i * 4, &Insn)) {
        Out += " <unreadable>";
        break;
      }
      Out += fextl::fmt::format(" {:08x}/{}", Insn, Census::WordClassName(Census::ClassifyGuestWord(Insn)));
    }
    return Out;
  }

  void TripwireNote(FEXCore::A64::Decoder* Dec, uint64_t Address, uint8_t Bit, Mode M, const char* What) {
    bool Contradiction = false;
    {
      std::lock_guard Guard {TripwireMutex};
      auto& E = TripwireTable[(Address >> 2) & (kTripwireSlots - 1)];
      if (E.Address != Address) {
        E.Address = Address;
        E.Flags = 0;
      }
      E.Flags |= Bit;
      Contradiction = (E.Flags & 3) == 3;
    }
    if (!Contradiction) {
      return;
    }
    Contradictions.fetch_add(1, std::memory_order_relaxed);
    // Straight to stderr, not through LogMan: FEX_SILENTLOG defaults to 1 and
    // FEXInterpreter uninstalls the message handler on that path, so a tripwire
    // that logged would be invisible in exactly the runs that matter. Same
    // reason the LockOnlyTSO warning in Core.cpp writes here.
    fextl::fmt::print(stderr,
                      "POWERarm: NZCV exit-deadness contradiction at {:#x} ({}): the guest-code peek called this "
                      "target dead, and the frontend translates it into a unit that reads NZCV from entry. "
                      "Words at the target:{}. See NZCV-LIVENESS.md 7.7.\n",
                      Address, What, Dec ? DescribeTarget(Dec, Address) : fextl::string {" <no decoder>"});
    if (M == Mode::Strict) {
      ERROR_AND_DIE_FMT("POWERARM_NZCVEXITDEAD=strict: NZCV exit-deadness contradiction at {:#x}", Address);
    }
  }

} // namespace

uint64_t ContradictionCount() {
  return Contradictions.load(std::memory_order_relaxed);
}

void NoteUnitCompiled(FEXCore::Core::InternalThreadState* Thread, uint64_t Entry, bool EntryNZCVLiveIn, Mode M) {
  if (M == Mode::Off || !EntryNZCVLiveIn) {
    return;
  }
  TripwireNote(Thread->FrontendDecoder.get(), Entry, 2, M, "unit compiled");
}

// ===========================================================================
// The policy
// ===========================================================================
void Apply(FEXCore::Core::InternalThreadState* Thread, FEXCore::IR::IREmitter* IREmit, uint64_t Entry, Mode M) {
  using namespace FEXCore::IR;

  if (M == Mode::Off) {
    return;
  }

  auto* Dec = Thread->FrontendDecoder.get();
  auto CurrentIR = IREmit->ViewIR();
  const uint32_t NumBlocks = CurrentIR.GetHeader()->BlockCount;
  if (!NumBlocks) {
    return;
  }

  ReadContext RC {Dec};

  // The hull starts as the unit's own decoded extent, because that is what the
  // widened extent has to cover too. Both are already final here: the decoder
  // ran before the frontend and nothing after this point re-decodes.
  const uint64_t UnitLow = Dec->DecodedMinAddress;
  const uint64_t UnitHigh = Dec->DecodedMaxAddress;
  Census::Witness Hull;
  if (UnitLow < UnitHigh) {
    Hull.Low = UnitLow;
    Hull.High = UnitHigh;
  }

  const auto& ISBTargets = Thread->OpDispatcher->GetISBExitTargets();
  const auto IsISBExit = [&ISBTargets](uint64_t Target) {
    for (auto T : ISBTargets) {
      if (T == Target) {
        return true;
      }
    }
    return false;
  };

  bool AnyDead = false;

  for (auto [BlockNode, BlockHeader] : CurrentIR.GetBlocks()) {
    // The terminator, the way DFCE finds it: the last op before EndBlock.
    auto BlockIROp = CurrentIR.GetOp<IROp_CodeBlock>(BlockNode);
    auto CodeLast = CurrentIR.at(BlockIROp->Last);
    --CodeLast;
    auto [ExitNode, ExitOp] = CodeLast();
    if (ExitOp->Op != OP_EXITFUNCTION) {
      continue;
    }
    auto* Op = ExitOp->CW<IROp_ExitFunction>();

    // §7.3. Return exits stay conservative (that is §7.4's separate switch, not
    // this one), and so does everything whose destination is not a compile-time
    // constant: indirect jumps, the thunk's `ExitFunction(LoadX(30), Return)`,
    // the callback return's ExitFunction through the context.
    if (Op->Hint == BranchHint::Return) {
      continue;
    }
    if (Op->NewRIP.IsImmediate()) {
      continue;
    }
    auto* Src = CurrentIR.GetOp<IROp_Header>(Op->NewRIP);
    uint64_t Target = 0;
    if (Src->Op == OP_INLINEENTRYPOINTOFFSET) {
      auto EOp = Src->C<IROp_InlineEntrypointOffset>();
      const uint64_t Mask = Src->Size == OpSize::i32Bit ? 0xFFFF'FFFFull : ~0ULL;
      Target = (Entry + EOp->Offset) & Mask;
    } else if (Src->Op == OP_INLINECONSTANT) {
      Target = static_cast<uint64_t>(Src->C<IROp_InlineConstant>()->Constant);
    } else {
      continue;
    }

    if (Target & 3) {
      continue;
    }
    if (IsISBExit(Target)) {
      continue;
    }

    Census::Witness W;
    // FollowBL = false: the simple scan. Stage 3 is the callee walk and is
    // deliberately not enabled -- ScanWitnessed already implements it and
    // already threads the witness through the recursion, so stage 3 is this
    // argument plus a second extent (or a per-page witness index) for far
    // callees, which is its whole design cost (§7.5).
    const Census::ScanVerdict V = Census::ScanWitnessed(Target, false, &ReadWord, &RC, &W);
    if (V != Census::SCAN_DEAD || W.Empty()) {
      continue;
    }

    // §7.5's hull bound. Accepting this exit means the unit carries every page
    // the scan read, so the candidate hull is tested BEFORE the verdict is
    // taken and the verdict is refused if it would not fit. Exits are visited
    // in block order, so which exit of a unit loses when several compete is a
    // function of the guest bytes alone -- the property the code cache needs.
    Census::Witness Candidate = Hull;
    Candidate.Merge(W);
    if (Candidate.Empty() || (Candidate.High - Candidate.Low) > kHullBound) {
      continue;
    }

    Hull = Candidate;
    Op->NZCVDeadAtTarget = true;
    AnyDead = true;
    TripwireNote(Dec, Target, 1, M, "peek said dead");
  }

  if (!AnyDead) {
    return;
  }

  // The witnesses join the unit's SMC footprint. Everything downstream follows
  // from this one call: CompileBlock's CodePages loop registers the pages and
  // arms write tracking on them, AddBlockMapping's extent covers them so the
  // overlap filter and the granule bitmap do not discard a write to them,
  // SMCChecks=icache filters IC IVAU against the same extent, and the code
  // cache's guest hash -- taken over [entry, entry + GuestSize) with GuestSize
  // now the hull -- refuses a cached block whose witnesses have changed.
  //
  // That last one is where this departs from §7.5, which proposed a
  // NZCVDeadExits bitmask in JITCodeTail plus a re-scan at load. The re-scan
  // needs the exit targets back, and §7.5 expected the relocation records to
  // carry them -- but RELOC_GUEST_RIP_MOVE is also what every guest CALL's
  // return address is recorded as (InsertEntrypointRIPMove), so the records are
  // not in one-to-one correspondence with constant exits and an index-based
  // bitmask cannot be matched to them. Widening the hashed extent is both
  // simpler and strictly stronger: it refuses the block when ANY witness byte
  // changed, not only when the change happens to flip a verdict, and it needs
  // no tail field and no loader code at all.
  Dec->AddPeekWitnessRange(Hull.Low, Hull.High);
}

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
