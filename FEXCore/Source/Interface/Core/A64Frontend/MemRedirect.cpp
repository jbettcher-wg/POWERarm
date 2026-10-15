// SPDX-License-Identifier: MIT
/*
$info$
tags: frontend|a64|memredirect
desc: The size-gated prologue that hands a guest memset/memcpy/memmove to the host's
$end_info$
*/

// The guest side of the libc memory-routine redirect (docs/powerarm/THUNKS-DESIGN.md
// 4.2; DESIGN.md 6.3 is why a FULL libc thunk is refused instead).
//
// What this emits, ahead of the hooked routine's own translation:
//
//     entry:   if (X2 < Threshold) goto fallthrough
//              State.pc = <routine's guest address>      ; precise PC for faults
//              Thunk(ArgPtr = X0, hash("powerarm:mem..."))
//              ExitFunction(X30, Return)
//     fallthrough:
//              ... the normal decode of the same address, unchanged ...
//
// Three things about that shape are load-bearing.
//
// 1. It is a PREFIX, not a replacement. The routine is still decoded and still
//    registers its guest code ranges, so SMCChecks=mtrack keeps covering the
//    routine's own bytes and a call below the threshold is the guest's own code
//    running. That is why this is NOT built on AddCustomIREntrypoint: a custom IR
//    handler IS the whole block (Core.cpp GenerateIR) and sets
//    NeedsAddGuestCodeRanges=false, both of which are the opposite of what a gate
//    needs. The block structure here is instead the one the full-SMC
//    validate/continue pair already uses, which needs no contract change at all.
//
// 2. The State.pc store is not cosmetic, and the comment on the guest-side thunk
//    marker (TranslateBranchSystem.cpp HLT) says why in one line: "A fault inside
//    the host function is delivered with this PC." A fault taken inside the host
//    routine has a host PC in host libc, so HandleDispatcherGuestSignal takes its
//    !WasInJIT branch and never runs SpillSRA -- which is the only thing that
//    recomputes State.pc from the host PC. Without this store the guest's signal
//    frame would carry whatever block boundary the JIT last wrote, which is what
//    the fatal-fault report means by "block-boundary value, may be stale". With
//    it, the frame's PC is the routine's own entry. si_addr needs no help: the
//    guest and host share one address space, so the kernel's si_addr already IS
//    the guest address, and the delivery path copies the host siginfo verbatim.
//
// 3. The gate reads X2 and X0 as plain guest registers before the Thunk, so the
//    SpillForABICall inside DEF_OP(Thunk) publishes the live guest register file
//    to the context. The host half therefore reads its arguments out of the frame
//    rather than being handed them, which keeps it inside ThunkedFunction's
//    void(void*) shape.

#include "Interface/Context/Context.h"
#include "Interface/Core/A64Frontend/IRBuilder.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/Thunks.h>
#include <FEXCore/IR/IR.h>

namespace FEXCore::A64 {

namespace {
  // sha256 of the thunk name, the same convention the built-in fex: thunks use
  // (Source/Tools/LinuxEmulation/Thunks.cpp). These must stay byte-identical to
  // the table entries there, or the named-thunk relocation resolves to nullptr
  // and the call goes to address zero.
  constexpr FEXCore::IR::SHA256Sum MemsetSum {{
    // sha256(powerarm:memset)
    0xc9, 0xa3, 0xc6, 0xdb, 0xfc, 0x26, 0xf3, 0x09, 0x58, 0x72, 0xb2, 0x5a, 0x5c, 0xa3, 0xd6, 0x7d,
    0xc6, 0xd1, 0x75, 0x82, 0x05, 0x98, 0x21, 0x95, 0x7c, 0x81, 0xa1, 0x7e, 0xf2, 0x95, 0x7d, 0xcf,
  }};
  constexpr FEXCore::IR::SHA256Sum MemcpySum {{
    // sha256(powerarm:memcpy)
    0x02, 0x9a, 0x39, 0xf6, 0x36, 0xed, 0xf1, 0x18, 0x23, 0x33, 0xb9, 0x41, 0xf2, 0xd4, 0xbd, 0xb0,
    0x8d, 0x33, 0xc8, 0xd7, 0x08, 0x7d, 0x22, 0x24, 0x2b, 0x52, 0x03, 0x11, 0x4e, 0xf2, 0x71, 0x97,
  }};
  constexpr FEXCore::IR::SHA256Sum MemmoveSum {{
    // sha256(powerarm:memmove)
    0x18, 0x4e, 0x38, 0x29, 0xd7, 0x10, 0x39, 0xec, 0xba, 0x20, 0xf2, 0x4a, 0x97, 0xa1, 0x5d, 0xfe,
    0xf2, 0xc3, 0x58, 0x0c, 0xe1, 0x11, 0x19, 0x47, 0x69, 0x1b, 0x87, 0x94, 0xe5, 0xc1, 0xb2, 0x3a,
  }};
} // namespace

bool IRBuilder::EmitMemRedirectGate(uint64_t RoutinePC) {
  const auto Kind = CTX->LookupMemRedirect(RoutinePC);
  if (Kind == FEXCore::Context::MemRedirectKind::None) {
    return false;
  }

  const FEXCore::IR::SHA256Sum* Sum {};
  switch (Kind) {
  case FEXCore::Context::MemRedirectKind::Memset: Sum = &MemsetSum; break;
  case FEXCore::Context::MemRedirectKind::Memcpy: Sum = &MemcpySum; break;
  case FEXCore::Context::MemRedirectKind::Memmove: Sum = &MemmoveSum; break;
  case FEXCore::Context::MemRedirectKind::None: return false;
  }

  // The host half must exist before the gate is emitted. A named-thunk
  // relocation that cannot be resolved becomes a call through a null pointer,
  // and the code cache would then persist a block containing it, so refuse the
  // gate instead -- exactly as the guest-side thunk marker refuses an unknown
  // hash (TranslateBranchSystem.cpp HLT).
  if (!CTX->ThunkHandler || !CTX->ThunkHandler->LookupThunk(*Sum)) {
    return false;
  }

  // AAPCS64: X0 = dest, X1 = src (memcpy/memmove) or fill byte (memset),
  // X2 = n, X30 = return address. X2 is the gate's input.
  //
  // Unsigned compare: n is a size_t, so a "negative" count from a caller bug is
  // an enormous unsigned one. Taking the host path for it is the right answer --
  // the host routine then faults the same way the guest's own loop would, and
  // the fault reaches the guest (see 2. in the file header). Refusing it here
  // instead would only hide the bug behind a slower copy that faults anyway.
  const uint64_t Threshold = CTX->Config.MemRedirectThreshold();
  auto Size = LoadX(2);
  auto Gate = _CondJump(Size, _Constant(Threshold), InvalidNode, InvalidNode, FEXCore::IR::CondClass::UGE, OpSize::i64Bit);

  auto FallthroughPred = GetCurrentBlock();
  auto RedirectBlock = CreateNewCodeBlockAtEnd();
  SetTrueJumpTarget(Gate, RedirectBlock);

  SetCurrentCodeBlock(RedirectBlock);
  _StoreContext(OpSize::i64Bit, FEXCore::IR::RegClass::GPR, PCValue(RoutinePC), offsetof(FEXCore::Core::CPUState, pc));
  _Thunk(LoadX(0), *Sum);
  // The routine returns to its caller. BranchHint::Return lets the exit use the
  // shadow return stack, so a redirected call returns as cheaply as the
  // translated one did.
  ExitFunction(LoadX(30), FEXCore::IR::BranchHint::Return);

  // Fall through into the routine's own translation, which the caller continues
  // emitting into this block.
  auto FallthroughBlock = CreateNewCodeBlockAfter(FallthroughPred);
  SetFalseJumpTarget(Gate, FallthroughBlock);
  SetCurrentCodeBlock(FallthroughBlock);
  StartContinuationBlock();
  return true;
}

} // namespace FEXCore::A64
