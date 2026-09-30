// SPDX-License-Identifier: MIT
// NZCV exit-site census. See NZCVExitCensus.h for what it is and why.
#include "Interface/Core/JIT/PPC64LE/NZCVExitCensus.h"
#include "Interface/IR/IR.h"
#include "Interface/IR/IntrusiveIRList.h"
#include "Interface/IR/Passes.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace FEXCore::CPU::NZCVExitCensus {

namespace {
  // The census works in the same four bits DFCE does (see the rmif
  // static_asserts in RedundantFlagCalculationElimination.cpp): N=8 Z=4 C=2 V=1.
  constexpr unsigned kAllFlags = 0xF;
} // namespace

const char* ClassName(KeptClass C) {
  switch (C) {
  case CLS_CONST_ONLY: return "exit: constant only";
  case CLS_CONST_AND_RET: return "exit: constant and indirect-Return";
  case CLS_CONST_AND_ICALL: return "exit: constant and indirect-Call";
  case CLS_CONST_AND_OTHER: return "exit: constant and other";
  case CLS_RET_ONLY: return "exit: indirect-Return";
  case CLS_ICALL_ONLY: return "exit: indirect-Call";
  case CLS_EXIT_OTHER: return "exit: other";
  case CLS_INUNIT_CCMP: return "in-unit: unfusable reader (CondSubNZCV)";
  case CLS_INUNIT_UNFUSABLE: return "in-unit: unfusable reader (other)";
  case CLS_INUNIT_FUSABLE: return "in-unit: only fusable readers visible";
  case CLS_INUNIT_NOREADER: return "in-unit: no visible reader";
  default: return "?";
  }
}

const char* VerdictName(Verdict V) {
  switch (V) {
  case V_NA: return "n/a";
  case V_DROP_SIMPLE: return "droppable-simple";
  case V_DROP_FOLLOWBL: return "droppable-follow-bl";
  case V_DEAD_UNFUSABLE: return "dead-but-unfusable-reader";
  case V_LIVE: return "live";
  case V_UNRESOLVED: return "unresolved";
  default: return "?";
  }
}

const char* ExitKindName(ExitKindBucket K) {
  switch (K) {
  case EXK_CONST_NONE: return "const-None";
  case EXK_CONST_CALL: return "const-Call";
  case EXK_CONST_RETURN: return "const-Return";
  case EXK_INDIRECT_NONE: return "indirect-None";
  case EXK_INDIRECT_CALL: return "indirect-Call";
  case EXK_INDIRECT_RETURN: return "indirect-Return";
  case EXK_OTHER: return "other";
  default: return "?";
  }
}

// ===========================================================================
// The guest-code scan of NZCV-LIVENESS.md §7.2, as a word classifier plus a
// bounded depth-first walk.
//
// The classifier decides, per A64 word: does it read NZCV (LIVE), write all
// four bits (this path is DEAD), write some of them (UNRESOLVED), transfer
// control somewhere the scan can follow, or transfer control somewhere it
// cannot. Everything else is neutral. The document's table is the
// specification; the encodings are from the Arm ARM's instruction-group
// decode, matched on the group bits so that the whole group is covered rather
// than a list of mnemonics.
//
// nzcv_census.py answers the same question from an llvm-objdump listing. That
// makes the two implementations independent, and cc1's DEAD share is the
// cross-check: §5.2 says 36.7% simple / 55.5% follow-bl on cc1's
// constant-exit-only producers, and this classifier has to reproduce it.
// ===========================================================================
namespace {

  // Local aliases for the public WordClass names (NZCVExitCensus.h). The walk
  // below was written against these and reads better with them; they are the
  // same values, not a second table.
  constexpr WordClass WK_NEUTRAL = WC_NEUTRAL;
  constexpr WordClass WK_READER = WC_READER;
  constexpr WordClass WK_WRITER = WC_WRITER;
  constexpr WordClass WK_PARTIAL = WC_PARTIAL;
  constexpr WordClass WK_B = WC_BRANCH;
  constexpr WordClass WK_TWOWAY = WC_TWOWAY;
  constexpr WordClass WK_BL = WC_CALL;
  constexpr WordClass WK_SVC = WC_SVC;
  constexpr WordClass WK_RET = WC_RET;
  constexpr WordClass WK_TERM = WC_TERM;

  struct Word {
    WordClass Kind;
    int64_t Offset; // branch displacement in bytes, for WK_B/WK_TWOWAY/WK_BL
  };

  constexpr int64_t SignExtend(uint32_t Value, uint32_t Bits) {
    const uint32_t Sign = 1u << (Bits - 1);
    return static_cast<int64_t>(static_cast<int32_t>((Value ^ Sign) - Sign));
  }

  Word ClassifyWordImpl(uint32_t I) {
    // ---- Branches, exception generation and system registers first: their
    // group bits overlap nothing below. ----

    // Unconditional branch (immediate): B / BL.
    if ((I >> 26) == 0b000101) {
      return {WK_B, SignExtend(I & 0x3FF'FFFF, 26) * 4};
    }
    if ((I >> 26) == 0b100101) {
      return {WK_BL, SignExtend(I & 0x3FF'FFFF, 26) * 4};
    }

    // Conditional branch (immediate): B.cond / BC.cond. A reader, and the
    // document is explicit that a reader ends the scan even though it is also
    // a branch.
    if (((I >> 24) & 0xFF) == 0x54) {
      return {WK_READER, 0};
    }

    // Compare and branch / test and branch (immediate): both legs are live.
    const uint32_t Hi7 = (I >> 24) & 0x7F;
    if (Hi7 == 0x34 || Hi7 == 0x35) { // CBZ / CBNZ
      return {WK_TWOWAY, SignExtend((I >> 5) & 0x7FFFF, 19) * 4};
    }
    if (Hi7 == 0x36 || Hi7 == 0x37) { // TBZ / TBNZ
      return {WK_TWOWAY, SignExtend((I >> 5) & 0x3FFF, 14) * 4};
    }

    // Unconditional branch (register): BR/BLR/RET/ERET/DRPS and every
    // pointer-auth form. RET is separated because stage 3 resumes after a
    // followed call when the callee returns with the flags untouched, and
    // because §7.4's AAPCS assumption (not implemented, deliberately) would
    // key off exactly this row.
    if (((I >> 25) & 0x7F) == 0b1101011) {
      const uint32_t Opc = (I >> 21) & 0xF;
      if (Opc == 0b0010 || Opc == 0b0100 || Opc == 0b0101) {
        // RET, RETAA/RETAB share opc=0010 with different op3/op4.
        return {WK_RET, 0};
      }
      return {WK_TERM, 0};
    }

    // Exception generation: SVC continues, everything else stops the path.
    if (((I >> 24) & 0xFF) == 0xD4) {
      if ((I & 0xFFE0'001F) == 0xD400'0001) {
        return {WK_SVC, 0};
      }
      return {WK_TERM, 0};
    }

    // MRS/MSR of NZCV (system register move), and the three PSTATE-field
    // writers the document classes as partial writers.
    if ((I & 0xFFFF'FFE0) == 0xD53B'4200) {
      return {WK_READER, 0}; // MRS Xt, NZCV
    }
    if ((I & 0xFFFF'FFE0) == 0xD51B'4200) {
      return {WK_WRITER, 0}; // MSR NZCV, Xt
    }
    if (I == 0xD500'401F || I == 0xD500'403F || I == 0xD500'405F) {
      return {WK_PARTIAL, 0}; // CFINV / XAFLAG / AXFLAG
    }

    // ---- Data processing: register ----
    // Add/subtract (with carry): ADC/ADCS/SBC/SBCS/NGC/NGCS all read C.
    if (((I >> 21) & 0xFF) == 0b1101'0000) {
      return {WK_READER, 0};
    }
    // Conditional compare (register and immediate): CCMP/CCMN read then write.
    // Readers first, per §7.2.
    if (((I >> 21) & 0xFF) == 0b1101'0010) {
      return {WK_READER, 0};
    }
    // Conditional select: CSEL/CSINC/CSINV/CSNEG and their aliases.
    if (((I >> 21) & 0xFF) == 0b1101'0100) {
      return {WK_READER, 0};
    }
    // Add/subtract (shifted or extended register): S bit makes it ADDS/SUBS,
    // i.e. CMP/CMN/NEGS too.
    if (((I >> 24) & 0x1F) == 0b01011) {
      return {((I >> 29) & 1) ? Word {WK_WRITER, 0} : Word {WK_NEUTRAL, 0}};
    }
    // Logical (shifted register): opc==11 is ANDS/BICS, i.e. TST.
    if (((I >> 24) & 0x1F) == 0b01010) {
      return {(((I >> 29) & 3) == 3) ? Word {WK_WRITER, 0} : Word {WK_NEUTRAL, 0}};
    }
    // Rotate right into flags / evaluate into flags: partial writers.
    if ((I >> 21) == 0b1011'1010'000 && ((I >> 10) & 0x1F) == 1 && !((I >> 4) & 1)) {
      return {WK_PARTIAL, 0}; // RMIF
    }
    if ((I & 0xFFFF'FC1F) == 0x3A00'080D || (I & 0xFFFF'FC1F) == 0x3A00'480D) {
      return {WK_PARTIAL, 0}; // SETF8 / SETF16
    }

    // ---- Data processing: immediate ----
    // Add/subtract (immediate): ADDS/SUBS, i.e. CMP/CMN immediate.
    if (((I >> 23) & 0x3F) == 0b100010) {
      return {((I >> 29) & 1) ? Word {WK_WRITER, 0} : Word {WK_NEUTRAL, 0}};
    }
    // Logical (immediate): ANDS/TST immediate.
    if (((I >> 23) & 0x3F) == 0b100100) {
      return {(((I >> 29) & 3) == 3) ? Word {WK_WRITER, 0} : Word {WK_NEUTRAL, 0}};
    }

    // ---- Scalar floating point ----
    // The scalar FP data-processing group with bit21 set splits on bits[11:10]:
    // 00 is compare (when bits[15:10]==001000) or a one-source/conversion form,
    // 01 is FCCMP, 11 is FCSEL, 10 is a two-source arithmetic op.
    //
    // bits[31:30] MUST be tested, not just bits[28:24]. The group is
    // M(31)=0 0(30) S(29) 11110(28:24); matching bits[28:24] alone also admits
    // 0x5E/0x7E, which is "Advanced SIMD scalar three same" -- FMULX, FCMEQ,
    // FRECPS, FRSQRTS and the rest -- whose bits[11:10] of 01 and 11 were being
    // read as FCCMP and FCSEL and ending scans LIVE at instructions that touch
    // no flag at all. Found by §9 stage 1's table check (POWERARM_NZCVTABLECHECK),
    // which translates every a64.inc entry and compares: 177 of 4,846 synthesised
    // words classified reader against a frontend translation that reads nothing.
    // Conservative, never unsound -- a scan that stops early only keeps a
    // producer it could have dropped -- so the census rows in §5.2/§13 stand,
    // slightly understating DEAD.
    if ((I >> 30) == 0 && ((I >> 24) & 0x1F) == 0b11110 && ((I >> 21) & 1)) {
      switch ((I >> 10) & 3) {
      case 0b00:
        if (((I >> 10) & 0x3F) == 0b001000) {
          return {WK_WRITER, 0}; // FCMP / FCMPE
        }
        break;
      case 0b01: return {WK_READER, 0}; // FCCMP / FCCMPE
      case 0b11: return {WK_READER, 0}; // FCSEL
      default: break;
      }
    }

    return {WK_NEUTRAL, 0};
  }

  Word ClassifyWord(uint32_t I) {
    int64_t Disp = 0;
    const WordClass C = ClassifyGuestWord(I, &Disp);
    return {C, Disp};
  }

  // A direct-mapped memo of scan verdicts, per JIT thread. The same constant
  // target is reached from many exits (cc1: 370k constant exit sites over far
  // fewer distinct targets), and a memo turns the census's compile-time cost from
  // noticeable into negligible. A stale entry after self-modifying code would
  // misclassify a site in a diagnostic table; it cannot affect emitted semantics,
  // because the census changes no codegen decision.
  struct MemoEntry {
    uint64_t Target;
    uint8_t Simple;
    uint8_t FollowBL;
    bool Valid;
  };
  constexpr uint32_t kMemoSize = 1u << 16;
  thread_local fextl::vector<MemoEntry> Memo;

} // namespace

WordClass ClassifyGuestWord(uint32_t Insn, int64_t* Displacement) {
  const Word W = ClassifyWordImpl(Insn);
  if (Displacement) {
    *Displacement = W.Offset;
  }
  return W.Kind;
}

const char* WordClassName(WordClass C) {
  switch (C) {
  case WC_NEUTRAL: return "neutral";
  case WC_READER: return "reader";
  case WC_WRITER: return "writer";
  case WC_PARTIAL: return "partial";
  case WC_BRANCH: return "branch";
  case WC_TWOWAY: return "twoway";
  case WC_CALL: return "call";
  case WC_SVC: return "svc";
  case WC_RET: return "ret";
  case WC_TERM: return "term";
  default: return "?";
  }
}

ScanVerdict Scan(uint64_t Target, bool FollowBL, ReadWordFn Read, void* Opaque) {
  // Depth-first over paths, each path bounded at kMaxInsnsPerPath and the whole
  // scan at kMaxVisitedWords, exactly as nzcv_census.py's Dis.scan does. The
  // verdict is DEAD only if every path ended DEAD.
  struct Frame {
    uint64_t PC;
    uint32_t Depth;
  };
  fextl::vector<Frame> Stack;
  fextl::vector<uint64_t> Seen;
  Stack.push_back({Target, 0});
  uint32_t Nodes = 0;
  ScanVerdict Worst = SCAN_DEAD;

  const auto WasSeen = [&Seen](uint64_t PC) {
    for (auto S : Seen) {
      if (S == PC) {
        return true;
      }
    }
    return false;
  };

  while (!Stack.empty()) {
    auto F = Stack.back();
    Stack.pop_back();
    uint64_t PC = F.PC;
    uint32_t Depth = F.Depth;
    uint32_t Path = 0;
    while (true) {
      if (WasSeen(PC)) {
        break;
      }
      Seen.push_back(PC);
      ++Nodes;
      if (Nodes > kMaxVisitedWords || Path > kMaxInsnsPerPath) {
        return SCAN_UNRESOLVED;
      }
      uint32_t Insn = 0;
      if (!Read(Opaque, PC, &Insn)) {
        return SCAN_UNRESOLVED; // not provably executable: the census's "no-code"
      }
      ++Depth;
      ++Path;
      const Word W = ClassifyWord(Insn);
      if (W.Kind == WK_READER) {
        return SCAN_LIVE;
      }
      if (W.Kind == WK_WRITER) {
        break; // this path is dead; try the next one
      }
      if (W.Kind == WK_PARTIAL || W.Kind == WK_TERM || W.Kind == WK_RET) {
        Worst = SCAN_UNRESOLVED;
        break;
      }
      if (W.Kind == WK_SVC) {
        PC += 4;
        continue;
      }
      if (W.Kind == WK_BL) {
        if (!FollowBL) {
          Worst = SCAN_UNRESOLVED;
          break;
        }
        // Stage 3's shape: scan the callee. DEAD ends this path, LIVE ends the
        // scan. A callee that is UNRESOLVED (including one that returns with
        // the flags still live) leaves the caller unresolved; resuming after
        // the call would need the ret-with-live-flags distinction that §7.4
        // keeps behind its own switch, and this census does not turn it on.
        const ScanVerdict Callee = Scan(PC + W.Offset, false, Read, Opaque);
        if (Callee == SCAN_DEAD) {
          break;
        }
        if (Callee == SCAN_LIVE) {
          return SCAN_LIVE;
        }
        Worst = SCAN_UNRESOLVED;
        break;
      }
      if (W.Kind == WK_B) {
        PC = static_cast<uint64_t>(static_cast<int64_t>(PC) + W.Offset);
        continue;
      }
      if (W.Kind == WK_TWOWAY) {
        Stack.push_back({static_cast<uint64_t>(static_cast<int64_t>(PC) + W.Offset), Depth});
        PC += 4;
        continue;
      }
      PC += 4;
    }
  }
  return Worst;
}

namespace {
  // Memoised two-mode scan: the simple verdict first, and the follow-BL scan only
  // where the simple one was UNRESOLVED (a simple DEAD stays DEAD when more paths
  // are resolvable, and a simple LIVE found its reader without following a call).
  void ScanBoth(uint64_t Target, ReadWordFn Read, void* Opaque, ScanVerdict* Simple, ScanVerdict* FollowBL) {
    if (Memo.empty()) {
      Memo.resize(kMemoSize);
    }
    auto& E = Memo[(Target >> 2) & (kMemoSize - 1)];
    if (E.Valid && E.Target == Target) {
      *Simple = static_cast<ScanVerdict>(E.Simple);
      *FollowBL = static_cast<ScanVerdict>(E.FollowBL);
      return;
    }
    *Simple = Scan(Target, false, Read, Opaque);
    *FollowBL = (*Simple == SCAN_UNRESOLVED) ? Scan(Target, true, Read, Opaque) : *Simple;
    E = {Target, static_cast<uint8_t>(*Simple), static_cast<uint8_t>(*FollowBL), true};
  }
} // namespace

// ===========================================================================
// Static tables and the dump
// ===========================================================================
namespace {

  std::atomic<uint64_t> StaticProducers[kSlotCount] {};
  std::atomic<uint64_t> StaticSites[kSlotCount] {};
  std::atomic<uint64_t> StaticUnits {};
  std::atomic<uint64_t> StaticExitSites {};
  std::atomic<uint64_t> StaticProducersTotal {};
  std::atomic<uint64_t> StaticSiteSlotOverflow {};
  std::atomic<uint64_t> CompiledSinceDump {};

  std::mutex CountersMutex;
  fextl::vector<uint64_t*> ThreadCounters;

  constexpr uint64_t kDumpIntervalCompiles = 512;

  fextl::string DumpPath() {
    // $TMPDIR when set, /tmp otherwise. The op-size profiler hard-codes /tmp;
    // this one has to be redirectable because a census is run per workload and
    // two concurrent runs (a suite in one mode while another measures) would
    // otherwise write into one directory and be summed together by mistake.
    const char* Dir = ::getenv("TMPDIR");
    if (!Dir || !Dir[0]) {
      Dir = "/tmp";
    }
    return fextl::fmt::format("{}/powerarm-nzcv-exits-{}.txt", Dir, ::getpid());
  }

  void WriteDump() {
    // The join is done by a script, so every row is one line with a stable
    // `KEY field=value` shape. Names come from ClassName/VerdictName so a row
    // here and a row in census/cc1-nzcv.census carry the same text.
    uint64_t Dyn[kSlotCount] {};
    {
      std::lock_guard Guard {CountersMutex};
      for (auto* C : ThreadCounters) {
        for (uint32_t i = 0; i < kSlotCount; ++i) {
          Dyn[i] += __atomic_load_n(&C[i], __ATOMIC_RELAXED);
        }
      }
    }

    fextl::string Out;
    Out += fextl::fmt::format("# POWERarm NZCV exit-site census (stage 0 of docs/powerarm/research/power-isa/NZCV-LIVENESS.md)\n");
    Out += fextl::fmt::format("NZCV_META pid={} units={} exit_sites={} producers={} site_slot_overflow={}\n", ::getpid(),
                              StaticUnits.load(std::memory_order_relaxed), StaticExitSites.load(std::memory_order_relaxed),
                              StaticProducersTotal.load(std::memory_order_relaxed), StaticSiteSlotOverflow.load(std::memory_order_relaxed));
    Out += fextl::fmt::format("NZCV_META scan_max_insns_per_path={} scan_max_visited_words={} max_class_slots_per_exit={}\n",
                              kMaxInsnsPerPath, kMaxVisitedWords, kMaxClassSlotsPerExit);

    uint64_t TraversalTotal = 0;
    for (uint32_t k = 0; k < static_cast<uint32_t>(EXK_MAX); ++k) {
      TraversalTotal += Dyn[SlotOf(static_cast<ExitKindBucket>(k))];
    }
    Out += fextl::fmt::format("NZCV_TOTAL exit_traversals={}\n", TraversalTotal);

    for (uint32_t k = 0; k < static_cast<uint32_t>(EXK_MAX); ++k) {
      const uint32_t S = SlotOf(static_cast<ExitKindBucket>(k));
      Out += fextl::fmt::format("NZCV_EXITKIND kind={} sites={} traversals={}\n", ExitKindName(static_cast<ExitKindBucket>(k)),
                                StaticSites[S].load(std::memory_order_relaxed), Dyn[S]);
    }

    for (uint32_t c = 0; c < static_cast<uint32_t>(CLS_MAX); ++c) {
      for (uint32_t v = 0; v < static_cast<uint32_t>(V_MAX); ++v) {
        const uint32_t S = SlotOf(static_cast<KeptClass>(c), static_cast<Verdict>(v));
        const uint64_t P = StaticProducers[S].load(std::memory_order_relaxed);
        const uint64_t Si = StaticSites[S].load(std::memory_order_relaxed);
        if (!P && !Si && !Dyn[S]) {
          continue;
        }
        Out += fextl::fmt::format("NZCV_ROW class=\"{}\" verdict={} producers={} sites={} traversals={}\n",
                                  ClassName(static_cast<KeptClass>(c)), VerdictName(static_cast<Verdict>(v)), P, Si, Dyn[S]);
      }
    }

    const auto Path = DumpPath();
    const auto TmpPath = Path + ".tmp";
    const int FD = ::open(TmpPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (FD == -1) {
      return;
    }
    size_t Written = 0;
    while (Written < Out.size()) {
      const ssize_t R = ::write(FD, Out.data() + Written, Out.size() - Written);
      if (R <= 0) {
        break;
      }
      Written += static_cast<size_t>(R);
    }
    ::close(FD);
    ::rename(TmpPath.c_str(), Path.c_str());
  }

} // namespace

uint64_t* AllocateThreadCounters() {
  // Never freed: a thread that exits keeps its counts, which is what a census
  // over a whole session wants, and the dump may run from any thread at any
  // time (including from a signal-killed process's last compile).
  auto* C = new uint64_t[kSlotCount] {};
  std::lock_guard Guard {CountersMutex};
  ThreadCounters.push_back(C);
  // A clean exit is not enough on its own: guest thread objects are
  // deliberately leaked (see the threads mitigation in HANDOVER), so the JIT
  // destructor is not a reliable last chance. atexit covers a clean exit, the
  // periodic rewrite in NoteCompiledUnit covers a SIGKILL, and between them the
  // file on disk is never more than kDumpIntervalCompiles compiles stale.
  static bool Registered = false;
  if (!Registered) {
    Registered = true;
    ::atexit([]() { WriteDump(); });
  }
  return C;
}

void NoteCompiledUnit() {
  if (CompiledSinceDump.fetch_add(1, std::memory_order_relaxed) + 1 >= kDumpIntervalCompiles) {
    CompiledSinceDump.store(0, std::memory_order_relaxed);
    WriteDump();
  }
}

void Dump() {
  WriteDump();
}

// ===========================================================================
// Compile-time analysis
// ===========================================================================
namespace {

  struct BlockRec {
    FEXCore::IR::Ref Node {};
    uint32_t Succ[2] {};
    uint8_t NumSucc {};
    ExitKindBucket Kind {EXK_OTHER};
    bool IsExitFunction {};
    bool ConstTarget {};
    uint64_t Target {};
    uint32_t ExitNodeID {};
  };

  // Which in-unit readers compare fusion can absorb. nzcv_census.py's
  // FUSABLE_READERS, one for one.
  bool IsFusableReader(FEXCore::IR::IROps Op) {
    return Op == FEXCore::IR::OP_CONDJUMP || Op == FEXCore::IR::OP_NZCVSELECT;
  }

  bool IsPlainProducer(FEXCore::IR::IROps Op) {
    using namespace FEXCore::IR;
    return Op == OP_SUBNZCV || Op == OP_ADDNZCV || Op == OP_SUBWITHFLAGS || Op == OP_ADDWITHFLAGS || Op == OP_TESTNZ || Op == OP_ANDWITHFLAGS;
  }

  bool IsCondProducer(FEXCore::IR::IROps Op) {
    return Op == FEXCore::IR::OP_CONDSUBNZCV || Op == FEXCore::IR::OP_CONDADDNZCV;
  }

} // namespace

uint32_t Analyse(const FEXCore::IR::IRListView* IR, uint64_t Entry, ReadWordFn Read, void* ReadOpaque, SlotSink Sink, void* SinkOpaque) {
  using namespace FEXCore::IR;

  const uint32_t NumBlocks = IR->GetHeader()->BlockCount;
  if (!NumBlocks) {
    return 0;
  }

  fextl::vector<BlockRec> Blocks;
  Blocks.resize(NumBlocks);
  fextl::vector<uint32_t> Order;
  Order.reserve(NumBlocks);

  // ---- Pass 1: block shape. Successors from the terminator, and the exit
  // descriptor for a block that has none. Mirrors nzcv_census.py's finish().
  for (auto [BlockNode, BlockHeader] : IR->GetBlocks()) {
    const uint32_t BID = IR->GetOp<IROp_CodeBlock>(BlockNode)->ID;
    if (BID >= NumBlocks) {
      return 0;
    }
    Order.push_back(BID);
    auto& B = Blocks[BID];
    B.Node = BlockNode;

    Ref TermNode = nullptr;
    const IROp_Header* Term = nullptr;
    for (auto [CodeNode, IROp] : IR->GetCode(BlockNode)) {
      switch (IROp->Op) {
      case OP_CONDJUMP:
      case OP_JUMP:
      case OP_EXITFUNCTION:
      case OP_BREAK:
        TermNode = CodeNode;
        Term = IROp;
        break;
      default: break;
      }
    }
    if (!Term) {
      B.Kind = EXK_OTHER;
      continue;
    }

    switch (Term->Op) {
    case OP_CONDJUMP: {
      auto Op = Term->C<IROp_CondJump>();
      B.Succ[0] = IR->GetOp<IROp_CodeBlock>(Op->TrueBlock)->ID;
      B.Succ[1] = IR->GetOp<IROp_CodeBlock>(Op->FalseBlock)->ID;
      B.NumSucc = 2;
      break;
    }
    case OP_JUMP: {
      auto Op = Term->C<IROp_Jump>();
      B.Succ[0] = IR->GetOp<IROp_CodeBlock>(Op->TargetBlock)->ID;
      B.NumSucc = 1;
      break;
    }
    case OP_EXITFUNCTION: {
      auto Op = Term->C<IROp_ExitFunction>();
      B.IsExitFunction = true;
      B.ExitNodeID = IR->GetID(TermNode).Value;
      // A constant destination is an InlineEntrypointOffset (unit-relative) or
      // an InlineConstant; both are what BranchOps.cpp's ExitFunction lowering
      // treats as linkable.
      if (!Op->NewRIP.IsImmediate()) {
        auto* Src = IR->GetOp<IROp_Header>(Op->NewRIP);
        if (Src->Op == OP_INLINEENTRYPOINTOFFSET) {
          auto EOp = Src->C<IROp_InlineEntrypointOffset>();
          const uint64_t Mask = Src->Size == OpSize::i32Bit ? 0xFFFF'FFFFull : ~0ULL;
          B.ConstTarget = true;
          B.Target = (Entry + EOp->Offset) & Mask;
        } else if (Src->Op == OP_INLINECONSTANT) {
          auto COp = Src->C<IROp_InlineConstant>();
          B.ConstTarget = true;
          B.Target = COp->Constant;
        }
      }
      if (B.ConstTarget) {
        B.Kind = Op->Hint == BranchHint::Call ? EXK_CONST_CALL : (Op->Hint == BranchHint::Return ? EXK_CONST_RETURN : EXK_CONST_NONE);
      } else {
        B.Kind = Op->Hint == BranchHint::Call ? EXK_INDIRECT_CALL : (Op->Hint == BranchHint::Return ? EXK_INDIRECT_RETURN : EXK_INDIRECT_NONE);
      }
      break;
    }
    default: B.Kind = EXK_OTHER; break;
    }
  }

  // ---- Pass 2: per producer, walk forward until every bit it wrote is
  // overwritten, and record which readers and which exits its flags reach.
  // This is nzcv_census.py's forward_reach, over the same DFCE classification
  // (IROpNZCVRead/Write are derived from DFCE's own table, so the census cannot
  // disagree with the pass whose seed the policy would change).
  //
  // Slots per exit site accumulate here and are handed to the Sink once the
  // whole unit has been walked.
  fextl::vector<uint32_t> SiteSlotsFlat; // parallel arrays keyed by block id
  fextl::vector<uint8_t> SiteSlotCount;
  SiteSlotsFlat.resize(static_cast<size_t>(NumBlocks) * kMaxClassSlotsPerExit, 0);
  SiteSlotCount.resize(NumBlocks, 0);
  uint64_t Overflow = 0;

  struct WalkFrame {
    uint32_t Block;
    uint32_t OpIndex;
    unsigned Mask;
  };
  fextl::vector<WalkFrame> Walk;
  fextl::vector<uint64_t> WalkSeen;
  fextl::vector<uint32_t> ReachedExitBlocks;

  // Cache the ops of each block once: the walk indexes into them repeatedly.
  fextl::vector<fextl::vector<const IROp_Header*>> BlockOps;
  BlockOps.resize(NumBlocks);
  for (uint32_t BID : Order) {
    for (auto [CodeNode, IROp] : IR->GetCode(Blocks[BID].Node)) {
      BlockOps[BID].push_back(IROp);
    }
  }

  uint64_t LocalProducers = 0;

  for (uint32_t BID : Order) {
    auto& Ops = BlockOps[BID];
    for (uint32_t Idx = 0; Idx < Ops.size(); ++Idx) {
      const IROp_Header* Prod = Ops[Idx];
      const bool Plain = IsPlainProducer(Prod->Op);
      const bool Cond = IsCondProducer(Prod->Op);
      if (!Plain && !Cond) {
        continue;
      }
      ++LocalProducers;

      bool HasReader = false;
      bool HasUnfusable = false;
      bool UnfusableOnlyCondSub = true;
      ReachedExitBlocks.clear();
      Walk.clear();
      WalkSeen.clear();
      Walk.push_back({BID, Idx + 1, kAllFlags});

      while (!Walk.empty()) {
        auto F = Walk.back();
        Walk.pop_back();
        const uint64_t Key = (static_cast<uint64_t>(F.Block) << 40) | (static_cast<uint64_t>(F.OpIndex) << 8) | F.Mask;
        bool Dup = false;
        for (auto K : WalkSeen) {
          if (K == Key) {
            Dup = true;
            break;
          }
        }
        if (Dup) {
          continue;
        }
        WalkSeen.push_back(Key);

        unsigned M = F.Mask;
        auto& BOps = BlockOps[F.Block];
        uint32_t i = F.OpIndex;
        for (; i < BOps.size(); ++i) {
          auto* H = const_cast<IROp_Header*>(BOps[i]);
          const unsigned R = IROpNZCVRead(H);
          const unsigned W = IROpNZCVWrite(H);
          if (R & M) {
            HasReader = true;
            if (!IsFusableReader(H->Op)) {
              HasUnfusable = true;
              if (H->Op != OP_CONDSUBNZCV) {
                UnfusableOnlyCondSub = false;
              }
            }
          }
          M &= ~W;
          if (!M) {
            break;
          }
        }
        if (!M) {
          continue;
        }
        auto& FB = Blocks[F.Block];
        if (FB.NumSucc) {
          for (uint8_t s = 0; s < FB.NumSucc; ++s) {
            Walk.push_back({FB.Succ[s], 0, M});
          }
        } else {
          bool Dup2 = false;
          for (auto E : ReachedExitBlocks) {
            if (E == F.Block) {
              Dup2 = true;
              break;
            }
          }
          if (!Dup2) {
            ReachedExitBlocks.push_back(F.Block);
          }
        }
      }

      // ---- Classify, the census's way. Exits reached decide the class when
      // there are any; otherwise the in-unit readers do.
      bool AnyConst = false, AnyRet = false, AnyICall = false, AnyOther = false;
      for (auto E : ReachedExitBlocks) {
        switch (Blocks[E].Kind) {
        case EXK_CONST_NONE:
        case EXK_CONST_CALL: AnyConst = true; break;
        case EXK_CONST_RETURN:
          AnyConst = true;
          AnyRet = true;
          break;
        case EXK_INDIRECT_RETURN: AnyRet = true; break;
        case EXK_INDIRECT_CALL: AnyICall = true; break;
        default: AnyOther = true; break;
        }
      }

      KeptClass C = CLS_INUNIT_NOREADER;
      if (ReachedExitBlocks.empty()) {
        if (!HasReader) {
          C = CLS_INUNIT_NOREADER;
        } else if (HasUnfusable) {
          C = UnfusableOnlyCondSub ? CLS_INUNIT_CCMP : CLS_INUNIT_UNFUSABLE;
        } else {
          C = CLS_INUNIT_FUSABLE;
        }
      } else if (AnyConst && !AnyRet && !AnyICall && !AnyOther) {
        C = CLS_CONST_ONLY;
      } else if (AnyConst && AnyRet && !AnyICall && !AnyOther) {
        C = CLS_CONST_AND_RET;
      } else if (AnyConst && AnyICall && !AnyRet && !AnyOther) {
        C = CLS_CONST_AND_ICALL;
      } else if (AnyConst) {
        C = CLS_CONST_AND_OTHER;
      } else if (AnyRet && !AnyICall && !AnyOther) {
        C = CLS_RET_ONLY;
      } else if (AnyICall && !AnyRet && !AnyOther) {
        C = CLS_ICALL_ONLY;
      } else {
        C = CLS_EXIT_OTHER;
      }

      // ---- The peek, for the const-exit-only population §5.2 scans.
      Verdict V = V_NA;
      if (C == CLS_CONST_ONLY) {
        bool AllDeadSimple = true, AllDeadFollow = true, AnyLive = false;
        for (auto E : ReachedExitBlocks) {
          ScanVerdict S = SCAN_UNRESOLVED, FB2 = SCAN_UNRESOLVED;
          if (Blocks[E].ConstTarget) {
            ScanBoth(Blocks[E].Target, Read, ReadOpaque, &S, &FB2);
          }
          if (S == SCAN_LIVE || FB2 == SCAN_LIVE) {
            AnyLive = true;
          }
          if (S != SCAN_DEAD) {
            AllDeadSimple = false;
          }
          if (FB2 != SCAN_DEAD) {
            AllDeadFollow = false;
          }
        }
        if (AllDeadSimple || AllDeadFollow) {
          if (HasUnfusable) {
            V = V_DEAD_UNFUSABLE;
          } else {
            V = AllDeadSimple ? V_DROP_SIMPLE : V_DROP_FOLLOWBL;
          }
        } else if (AnyLive) {
          V = V_LIVE;
        } else {
          V = V_UNRESOLVED;
        }
      }

      const uint32_t Slot = SlotOf(C, V);
      StaticProducers[Slot].fetch_add(1, std::memory_order_relaxed);

      // Attribute this producer's (class, verdict) to every exit site its
      // flags reach. One traversal of such a site is one execution of this
      // producer, because the legs are alternatives.
      for (auto E : ReachedExitBlocks) {
        if (!Blocks[E].IsExitFunction) {
          continue;
        }
        auto* Dst = &SiteSlotsFlat[static_cast<size_t>(E) * kMaxClassSlotsPerExit];
        bool Present = false;
        for (uint8_t k = 0; k < SiteSlotCount[E]; ++k) {
          if (Dst[k] == Slot) {
            Present = true;
            break;
          }
        }
        if (Present) {
          continue;
        }
        if (SiteSlotCount[E] >= kMaxClassSlotsPerExit) {
          ++Overflow;
          continue;
        }
        Dst[SiteSlotCount[E]++] = Slot;
        StaticSites[Slot].fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  // ---- Publish: one ExitSiteSlots per instrumented exit site.
  uint32_t Instrumented = 0;
  for (uint32_t BID : Order) {
    auto& B = Blocks[BID];
    if (!B.IsExitFunction) {
      continue;
    }
    ExitSiteSlots S {};
    S.Slots[S.Count++] = static_cast<uint16_t>(SlotOf(B.Kind));
    StaticSites[SlotOf(B.Kind)].fetch_add(1, std::memory_order_relaxed);
    auto* Src = &SiteSlotsFlat[static_cast<size_t>(BID) * kMaxClassSlotsPerExit];
    for (uint8_t k = 0; k < SiteSlotCount[BID]; ++k) {
      S.Slots[S.Count++] = static_cast<uint16_t>(Src[k]);
    }
    Sink(SinkOpaque, B.ExitNodeID, S);
    ++Instrumented;
  }

  StaticUnits.fetch_add(1, std::memory_order_relaxed);
  StaticExitSites.fetch_add(Instrumented, std::memory_order_relaxed);
  StaticProducersTotal.fetch_add(LocalProducers, std::memory_order_relaxed);
  if (Overflow) {
    StaticSiteSlotOverflow.fetch_add(Overflow, std::memory_order_relaxed);
  }
  NoteCompiledUnit();
  return Instrumented;
}

} // namespace FEXCore::CPU::NZCVExitCensus
