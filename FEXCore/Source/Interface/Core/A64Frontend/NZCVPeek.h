// SPDX-License-Identifier: MIT
#pragma once
// ===========================================================================
// NZCV exit-deadness: the compile-time peek at constant exit targets.
//
// docs/powerarm/research/power-isa/NZCV-LIVENESS.md §7 is the design and §9
// stages 1-2 are what this implements. In one paragraph:
//
//   When a compile unit leaves by a CONSTANT exit, the guest's NZCV is usually
//   dead at the target -- 36-39% of the constant-exit-only producers on cc1 --
//   and today we compute and keep it anyway. The verdict is decided by scanning
//   forward through the GUEST'S OWN instructions from the exit target until the
//   first NZCV reader or full writer (§7.2), not by inspecting the successor's
//   IR, which cannot answer the question three quarters of the time (§5.3).
//
// The scan itself is NOT here. It is NZCVExitCensus::ScanWitnessed, which has
// done exactly this walk and this classification since stage 0 and has been
// validated against an independent Python implementation to within 0.1% on
// every row (§5.4, §13). This file is the policy around it: where the verdict
// is taken, what it is allowed to conclude, what it must refuse, and what the
// verdict costs the unit's SMC footprint.
//
// WHERE IT RUNS. GenerateIR, after the frontend has emitted the whole unit and
// BEFORE the pass manager runs -- so DeadFlagCalculationElimination sees the
// verdict as an input, and the decoder's extent can still be widened before
// Core.cpp reads it. A pass would have been the obvious home, but a Pass has no
// route to the thread's decoder or to guest memory, and the widening has to
// happen on the decoder.
//
// WHAT IT REFUSES (§7.3). Anything that is not an ExitFunction with a constant
// destination and hint None or Call; any target that is not 4-aligned or not
// provably executable (the peek reads through the decoder's own
// CheckRangeExecutable, so it cannot fault where the decoder would not);
// the ISB exit in icache mode; and -- the bound that is not obvious -- any
// verdict whose witnesses would push the hull of unit-plus-witnesses past
// 64 KiB (§7.5), because that hull is what the unit has to carry for SMC.
//
// WHAT A DEAD VERDICT COSTS. The scan reads bytes outside the unit. A DEAD
// verdict is only as good as those bytes, so every page they touch joins the
// unit's CodePages and the decoded extent widens to the hull. That is what
// makes a guest write to a witness cost a recompile rather than a stale
// assumption, on all three SMC models. See §7.5 and the note in NZCVPeek.cpp
// about how that differs from the document's NZCVDeadExits bitmask.
//
// THE CODE CACHE NEEDS ONE CONDITION MORE THAN §7.5 THOUGHT. The cache stores
// {Tail->RIP = the unit's ENTRY, Tail->GuestSize = DecodedMax - DecodedMin} and
// validates a loaded block by hashing [RIP, RIP + GuestSize) -- a window
// anchored at the ENTRY, not at DecodedMin. A unit that followed a backward
// branch already has DecodedMin < Entry (LookupCache.h's BlockEntry::ExtentStart
// note says exactly this), and a witness BELOW the entry then falls outside the
// hashed window: the guest could rewrite it and the cached block would still
// load, DEAD verdict intact and wrong, with the pages below the entry not even
// registered for SMC (CodeCache.cpp's load path arms
// [GuestRIP, GuestRIP + Length)). Widening the extent is what §7.5 relied on
// and it is not enough by itself. So a verdict is refused unless every witness
// lies at or above the unit's entry, which is what makes
// [Entry, Entry + GuestSize) a superset of the witnesses again.
// ===========================================================================

#include <cstdint>

namespace FEXCore::Core {
struct InternalThreadState;
}

namespace FEXCore::IR {
class IREmitter;
}

namespace FEXCore::A64::NZCVPeek {

// POWERARM_NZCVEXITDEAD. Mirrors FEXCore::Config::ConfigNZCVExitDead.
enum class Mode : uint8_t {
  Off = 0,
  On,
  Canary,
  Strict,
};

// §7.5: the hull of unit and witnesses must stay inside this, or the DEAD
// verdict is refused for that exit. On cc1 the branch-target scans are within
// 4 KiB for 68% of simple DEAD verdicts and within 64 KiB for 81%.
//
// The number is not a free choice, which is why it is no longer §7.5's 64 KiB.
// The hull becomes the unit's recorded guest length, and CodeCache refuses both
// to store and to load any block whose GuestSize exceeds
// A64::DEFAULT_MAX_INSTRUCTIONS * 4 -- the `Length >` guards on both sides of
// CodeCache.cpp. A 64 KiB bound therefore made every DEAD verdict with a
// witness more than 4 KiB from its unit silently cost that unit its cached
// translation: the policy bought one dropped compare and paid a whole
// recompile for it, on workloads whose startup the cache dominates. §5.2's own
// numbers put that at a fifth of the verdicts at most, so matching the cache's
// limit costs little and keeps the unit cacheable. Kept as a literal rather
// than the constant so this header does not pull in the decoder; the .cpp
// static_asserts the two agree.
inline constexpr uint64_t kHullBound = 4 * 1024;

// §7.2's per-path budget is not a policy constant. The policy calls
// Census::ScanWitnessed, which uses the census's own kMaxInsnsPerPath /
// kMaxVisitedWords (48/96) so its rows stay comparable with
// census/cc1-nzcv.census. §5.2's 16 is a TIGHTER bound than the one actually in
// force, so the implementation is looser than the document rather than tighter,
// and the constant that used to sit here claiming the opposite
// (kPolicyMaxInsnsPerPath) was read by nothing. Scan parameters are part of the
// emitted code and are hashed into the code cache config id with the mode.

// Runs the peek over every ExitFunction of the freshly built unit, sets
// IROp_ExitFunction::NZCVDeadAtTarget where it proves deadness, and records the
// witness hull on the thread's decoder. No-op when M == Off.
void Apply(FEXCore::Core::InternalThreadState* Thread, FEXCore::IR::IREmitter* IREmit, uint64_t Entry, Mode M);

// §7.7, the tripwire -- see the long comment at the definition. Called once per
// compiled unit with the verdict the frontend's own translation reached about
// that unit's entry; where that verdict is "NZCV is live in", it runs the
// peek's scan on the same address immediately, so the two answers are about the
// same guest words at the same instant. Counts contradictions always; aborts
// under Strict.
void NoteUnitCompiled(FEXCore::Core::InternalThreadState* Thread, uint64_t Entry, bool EntryNZCVLiveIn, Mode M);

// §7.7's running total, reported through GetStats below.
uint64_t ContradictionCount();

// Stage 2's own accounting. A running-total line to stderr per compiled unit
// when POWERARM_NZCVEXITDEADSTATS=1 -- per unit rather than at exit because a
// guest's exit_group is forwarded and neither atexit nor a destructor runs
// reliably, which is why the census next door rewrites every 512 compiles.
// unittests/A64Frontend/run.sh's nzcv_exit_dead_window check reads the last
// line; nothing else reads it, and with the variable unset nothing is printed.
//
// It exists because `canary` is the mode a suite run is supposed to prove
// something in, and until now the only aggregate -- the contradiction count --
// had no caller at all (this header claimed it was "printed with the link
// outcomes"; it was not), so a canary run that fired the tripwire fifty times
// and one that never fired it looked identical to the suite.
struct Stats {
  uint64_t ExitsSeen {};         // constant-destination ExitFunctions examined
  uint64_t Dead {};              // verdicts taken: NZCVDeadAtTarget set
  uint64_t RefusedScan {};       // the scan did not return DEAD, or read nothing
  uint64_t RefusedHull {};       // DEAD, but the hull would exceed kHullBound
  uint64_t RefusedBelowEntry {}; // DEAD, but a witness sat below the unit entry
  uint64_t Contradictions {};    // §7.7 tripwire firings
};
Stats GetStats();

// §9 stage 1: the table proven against the frontend. Enumerates every a64.inc
// entry with a handler, synthesises words for it, translates each alone through
// this thread's IRBuilder, classifies the resulting IR with DFCE's own
// ClassifyFast, and requires the peek's table to agree. Runs once per process
// under POWERARM_NZCVTABLECHECK=1 and prints a one-line verdict; returns the
// number of mismatches.
uint32_t RunTableCheck(FEXCore::Core::InternalThreadState* Thread);

} // namespace FEXCore::A64::NZCVPeek
