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
// assumption, on all three SMC models, and it is also what makes the code cache
// sound with no format change: the cache hashes [entry, entry + GuestSize) and
// GuestSize is now the hull, so a cached block whose witnesses moved fails its
// guest hash and is recompiled. See §7.5 and the note in NZCVPeek.cpp about how
// that differs from the document's NZCVDeadExits bitmask.
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
// 4 KiB for 68% of simple DEAD verdicts and within 64 KiB for 81%, so the bound
// costs about a fifth of the scan's value and keeps a single extent.
inline constexpr uint64_t kHullBound = 64 * 1024;

// §7.2's per-path budget. The census uses the Python script's 48/96 so its rows
// stay comparable with census/cc1-nzcv.census; the policy uses the document's
// own recommendation, which §5.2 measured as capturing 91-93% of the value.
// Scan parameters are part of the emitted code, so they are hashed into the
// code cache config id along with the mode.
inline constexpr uint32_t kPolicyMaxInsnsPerPath = 16;

// Runs the peek over every ExitFunction of the freshly built unit, sets
// IROp_ExitFunction::NZCVDeadAtTarget where it proves deadness, and records the
// witness hull on the thread's decoder. No-op when M == Off.
void Apply(FEXCore::Core::InternalThreadState* Thread, FEXCore::IR::IREmitter* IREmit, uint64_t Entry, Mode M);

// §7.7, the tripwire, moved from the linker to the compiler -- see the long
// comment at the definition. Called once per compiled unit with the verdict the
// frontend's own translation reached about that unit's entry. Counts
// contradictions always; aborts under Strict.
void NoteUnitCompiled(FEXCore::Core::InternalThreadState* Thread, uint64_t Entry, bool EntryNZCVLiveIn, Mode M);

// Printed with the link outcomes.
uint64_t ContradictionCount();

// §9 stage 1: the table proven against the frontend. Enumerates every a64.inc
// entry with a handler, synthesises words for it, translates each alone through
// this thread's IRBuilder, classifies the resulting IR with DFCE's own
// ClassifyFast, and requires the peek's table to agree. Runs once per process
// under POWERARM_NZCVTABLECHECK=1 and prints a one-line verdict; returns the
// number of mismatches.
uint32_t RunTableCheck(FEXCore::Core::InternalThreadState* Thread);

} // namespace FEXCore::A64::NZCVPeek
