// SPDX-License-Identifier: MIT
#pragma once
// ===========================================================================
// NZCV exit-deadness, stage 1: the guest-word table, proven against the
// frontend.
//
// docs/powerarm/research/power-isa/NZCV-LIVENESS.md §7 is the design and §9 is
// the staging. The policy itself -- seeding NZCV dead at a constant exit whose
// target the scan proves writes every bit before reading one -- is stage 2 and
// is not here. What is here is stage 1's gate, which the document puts first
// precisely because it changes no codegen and its test is exhaustive over the
// decoder:
//
//   A generated test enumerates every a64.inc entry with a handler, synthesises
//   words for it, translates each alone, classifies the resulting IR with
//   DeadFlagCalculationElimination's own ClassifyFast, and asserts the peek's
//   table gives the same class. Any handler the table calls neutral that the
//   frontend translates into a flag reader fails.
//
// There is no host-side harness that can instantiate the A64 frontend -- an
// IRBuilder needs a ContextImpl and a Decoder needs an InternalThreadState --
// so the check runs from inside the emulator, once per process, under
// POWERARM_NZCVTABLECHECK=1, translating through the calling thread's own
// IRBuilder. unittests/A64Frontend/run.sh's `nzcv_table_check` is the gate.
// ===========================================================================

#include <cstdint>

namespace FEXCore::Core {
struct InternalThreadState;
}

namespace FEXCore::A64::NZCVPeek {

// Runs the table check and prints a one-line NZCV_TABLECHECK verdict to stderr
// (not through LogMan: FEX_SILENTLOG defaults to 1 and the interpreter
// uninstalls the message handler, so a logged verdict would be invisible in
// exactly the runs that matter). Returns the number of UNSOUND disagreements.
uint32_t RunTableCheck(FEXCore::Core::InternalThreadState* Thread);

} // namespace FEXCore::A64::NZCVPeek
