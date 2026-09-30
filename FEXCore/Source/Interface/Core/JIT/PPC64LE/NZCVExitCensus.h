// SPDX-License-Identifier: MIT
#pragma once
// ===========================================================================
// NZCV exit-site census (stage 0 of docs/powerarm/research/power-isa/
// NZCV-LIVENESS.md; its §10 first bullet and §9 "Stage 0" are what this
// closes).
//
// WHAT IT ANSWERS. The document's §5 counts, per *translated* site, why every
// surviving NZCV producer is kept, and §5.2 runs a bounded guest-code scan
// ("the peek") on the constant exit targets those producers reach. Every one of
// those numbers is weighted once per compiled site. Nothing said how often the
// sites actually run, and the previous attempt at this optimisation (2f8013325)
// estimated 6-10% off static shares and delivered 3.0% — so the missing weight
// is the whole difference between a bracket and a number.
//
// WHAT IT IS. Two tables over the same slot space:
//
//   * a STATIC table, filled at compile time, one entry per surviving producer
//     and one per instrumented exit site. It reproduces the census's rows using
//     the census's own terminology, from inside the emulator, so it also works
//     on workloads whose IR dump and disassembly are too large to feed the
//     Python script (libxul);
//   * a DYNAMIC table, one 64-bit counter per slot per guest thread, bumped by
//     three JIT-emitted instructions at every traversal of an instrumented
//     exit site.
//
// The join key is the slot, so "36.7% of translated sites" and "N% of executed
// traversals" are the same row of the same table.
//
// HOW A PRODUCER IS PRICED. A compare kept only by its exits executes once per
// execution of the block it sits in, and on any one execution exactly one of
// the exits its flags reach is traversed. So summing the traversals of the
// exits a producer reaches *is* the number of times that producer executed,
// which is what the saving would scale with. Each exit site therefore bumps one
// counter per distinct (class, verdict) pair that reaches it, plus one counter
// for its own exit kind (the exit-traversal denominator).
//
// OFF-PATH GUARANTEE. Everything here is reached only from
// `if (NZCVExitCensusEnabled)`, a single bool read once per CompileCode from
// the config getter. With the option off (the default) no analysis runs, no
// guest word is read and — the claim that matters — `DEF_OP(ExitFunction)`
// emits exactly the bytes it emitted before this file existed.
// ===========================================================================

#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/string.h>

#include <array>
#include <cstdint>

namespace FEXCore::IR {
class IRListView;
}

namespace FEXCore::CPU::NZCVExitCensus {

// ---------------------------------------------------------------------------
// The census's classification of why a surviving producer's flags are live.
// Names track NZCV-LIVENESS.md §5.1 / nzcv_census.py's `cls` keys one for one;
// ClassName() below prints the census's string so the two tables join on text.
// ---------------------------------------------------------------------------
enum KeptClass : uint8_t {
  CLS_CONST_ONLY = 0,   // "exit: constant only"
  CLS_CONST_AND_RET,    // "exit: constant and indirect-Return"
  CLS_CONST_AND_ICALL,  // "exit: constant and indirect-Call"
  CLS_CONST_AND_OTHER,  // "exit: constant and <other>"
  CLS_RET_ONLY,         // "exit: indirect-Return"
  CLS_ICALL_ONLY,       // "exit: indirect-Call"
  CLS_EXIT_OTHER,       // "exit: <no constant leg>"
  CLS_INUNIT_CCMP,      // "in-unit: unfusable reader (CondSubNZCV)"
  CLS_INUNIT_UNFUSABLE, // "in-unit: unfusable reader (other)"
  CLS_INUNIT_FUSABLE,   // "in-unit: only fusable readers visible"
  CLS_INUNIT_NOREADER,  // "in-unit: no visible reader"
  CLS_MAX
};

// The peek verdict, computed only for CLS_CONST_ONLY producers — exactly the
// population §5.2 scans. DROP_* fold in the census's "readers fusable ->
// droppable" test, because a producer whose targets are all DEAD but which
// still has an unfusable in-unit reader cannot be dropped.
enum Verdict : uint8_t {
  V_NA = 0,         // class is not const-exit-only; no scan was run
  V_DROP_SIMPLE,    // every target DEAD under the simple scan, readers fusable
  V_DROP_FOLLOWBL,  // only DEAD once the scan follows one level of BL
  V_DEAD_UNFUSABLE, // targets DEAD but an unfusable in-unit reader keeps it
  V_LIVE,           // some target reads NZCV before writing it
  V_UNRESOLVED,     // a path ended in BL/BLR/BR/RET/BRK/budget/no-code
  V_MAX
};

// Exit kinds, as the census names them in "## exit kinds reached with live
// flags". Every instrumented exit site bumps exactly one of these on every
// traversal, so their sum is the executed exit-traversal total.
enum ExitKindBucket : uint8_t {
  EXK_CONST_NONE = 0,
  EXK_CONST_CALL,
  EXK_CONST_RETURN,
  EXK_INDIRECT_NONE,
  EXK_INDIRECT_CALL,
  EXK_INDIRECT_RETURN,
  EXK_OTHER,
  EXK_MAX
};

// Slot space: the class x verdict grid first, then the exit-kind buckets.
static constexpr uint32_t kClassVerdictSlots = static_cast<uint32_t>(CLS_MAX) * static_cast<uint32_t>(V_MAX);
static constexpr uint32_t kSlotCount = kClassVerdictSlots + static_cast<uint32_t>(EXK_MAX);

static constexpr uint32_t SlotOf(KeptClass C, Verdict V) {
  return static_cast<uint32_t>(C) * static_cast<uint32_t>(V_MAX) + static_cast<uint32_t>(V);
}
static constexpr uint32_t SlotOf(ExitKindBucket K) {
  return kClassVerdictSlots + static_cast<uint32_t>(K);
}

// The whole per-thread counter array must sit within a d-form displacement of
// its base register, which is what keeps the bump to three instructions.
static_assert(kSlotCount * 8 <= 32760, "counter array must be d-form reachable from its base");

const char* ClassName(KeptClass C);
const char* VerdictName(Verdict V);
const char* ExitKindName(ExitKindBucket K);

// ---------------------------------------------------------------------------
// Scan parameters. The document's §5.2 measured that 16 instructions per path
// captures 91-93% of the value and 32 captures 99%; the census script's
// defaults (48 per path, 96 visited words) are used here so the in-emulator
// numbers are comparable with census/cc1-nzcv.census rather than with a
// tighter bound the implementation might later pick.
// ---------------------------------------------------------------------------
static constexpr uint32_t kMaxInsnsPerPath = 48;
static constexpr uint32_t kMaxVisitedWords = 96;

// How many (class, verdict) counters one exit site may bump. An exit reached by
// producers of more than this many distinct classes has the surplus dropped and
// is counted in StaticSiteSlotOverflow, so the over/under-count is reported
// rather than assumed away. Four covers every site seen on cc1 and libxul.
static constexpr uint32_t kMaxClassSlotsPerExit = 4;

// What one instrumented exit site bumps: its exit-kind bucket, then one slot
// per distinct (class, verdict) pair whose producer's flags reach it.
struct ExitSiteSlots {
  uint8_t Count {};
  uint16_t Slots[1 + kMaxClassSlotsPerExit] {};
};

// ---------------------------------------------------------------------------
// Verdict of the bounded guest-code scan of §7.2, as a pure function of guest
// words. ReadWord returns false for anything not provably executable.
// ---------------------------------------------------------------------------
enum ScanVerdict : uint8_t { SCAN_DEAD = 0, SCAN_LIVE, SCAN_UNRESOLVED };

using ReadWordFn = bool (*)(void* Opaque, uint64_t Address, uint32_t* Out);

ScanVerdict Scan(uint64_t Target, bool FollowBL, ReadWordFn Read, void* Opaque);

// ---------------------------------------------------------------------------
// Compile-time analysis. Walks the unit's IR the way nzcv_census.py's
// forward_reach does, scans the constant exit targets it reaches, accumulates
// the static tables, and fills Out with the slots each ExitFunction node's
// lowering should bump. Returns the number of instrumented exit sites.
// ---------------------------------------------------------------------------
using SlotSink = void (*)(void* Opaque, uint32_t ExitNodeID, const ExitSiteSlots& Slots);

uint32_t Analyse(const FEXCore::IR::IRListView* IR, uint64_t Entry, ReadWordFn Read, void* ReadOpaque, SlotSink Sink, void* SinkOpaque);

// ---------------------------------------------------------------------------
// Dynamic counters. One array per guest thread so the bump needs no atomic —
// an atomic increment on ppc64le is an lwarx/stwcx. loop, and stwcx. records
// into CR0, which is where two of the four guest flags live. A shared array
// with plain stores would also lose counts exactly where they are hottest,
// which is the number this whole exercise reports.
// ---------------------------------------------------------------------------
uint64_t* AllocateThreadCounters();

// Rewrite the dump file (atomic replace, like the op-size profiler) so the
// table survives the SIGKILL that ends a browser session. Called every
// kDumpIntervalCompiles compiled units and once at Dump().
void NoteCompiledUnit();
void Dump();

} // namespace FEXCore::CPU::NZCVExitCensus
