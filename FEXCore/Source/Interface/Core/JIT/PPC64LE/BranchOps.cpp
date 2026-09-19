// SPDX-License-Identifier: MIT
// PPC64LE branch/control-flow operations for FEX JIT backend.
#include "Interface/Context/Context.h"
#include "Interface/Core/LookupCache.h"
#include "Interface/Core/JIT/PPC64LE/JITClass.h"

#include <bit>
#include <cstdlib>

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/MathUtils.h>

namespace FEXCore::CPU {

// ---------------------------------------------------------------------------
// P5.0.2 re-zero policy: EmitExitR0Zero
// ---------------------------------------------------------------------------
// Block exits used to end with an unconditional `li r0, 0`, defending the
// backend's global "r0 == 0 in JIT code" invariant against a clobber the
// exiting block might have left behind. The in-tree comment on the hit leg
// said as much: "Every mflr(r0) in the backend today is paired with a restore,
// so no bug is visible -- but the invariant is currently globally assumed."
//
// It is not merely assumed any more, it is tracked. Emit32 sets an r0-dirty
// flag for the compile unit whenever it emits `li r0,0` (the restore that any
// correct clobber site must emit) or `bctrl` (a host call, after which
// ELFv2-volatile r0 holds the callee's parked return address). See the long
// comment in CodeEmitter/PPC64LE/Emitter.h for why those two words are a
// sound proxy that cannot be forgotten by future code.
//
// A compile unit that emitted neither has no way to reach an exit with r0
// non-zero, so the re-zero is dead code there -- which is most units: the
// clobbering constructs are host calls (x87/transcendental fallbacks, CPUID,
// thunks, syscalls, split-lock atomics) and the rep-string tiers, none of
// which appear in ordinary arithmetic/memory blocks.
//
// RESIDUAL HOLE, stated plainly: emission is single-pass, so an exit emitted
// before a later clobber in the same unit sees a clean flag. That is only
// exploitable by a clobber site that does NOT restore -- and such a site is
// already broken for the remainder of its own block, since the next guest
// load/store would be misaddressed. It is a regression detector, not a live
// risk. FEX_R0TRAP=1 makes it loud anyway (below).
//
// Modes:
//   default        elide when the unit is provably clean
//   FEX_NOR0ELIDE  always emit `li r0,0` (the pre-change behaviour)
//   FEX_R0TRAP     never elide the *slot*: emit `tdnei r0, 0` in place of the
//                  li, which traps if the invariant was ever violated. Same
//                  one-instruction footprint as the code it replaces, so it
//                  is a clean A/B and usable in Release. This is the
//                  FEX_DEADPROLOGUE=trap methodology applied to P5.0.2.
namespace {
enum class R0ZeroModeType { Elide, Always, Trap };
R0ZeroModeType R0ZeroMode() {
  static const R0ZeroModeType Mode = []() {
    if (getenv("FEX_R0TRAP")) {
      return R0ZeroModeType::Trap;
    }
    if (getenv("FEX_NOR0ELIDE")) {
      return R0ZeroModeType::Always;
    }
    return R0ZeroModeType::Elide;
  }();
  return Mode;
}
} // namespace

void PPC64JITCore::EmitExitR0Zero(bool UnitDirty) {
  switch (R0ZeroMode()) {
  case R0ZeroModeType::Always: li(r(0), 0); return;
  case R0ZeroModeType::Trap:
    // TO=24 (LT|GT) == "not equal". Traps iff r0 != 0. Touches no CR field,
    // so the block's packed NZCV in CR0 and the cr7 discipline below are
    // both unaffected.
    tdi(24, r(0), 0);
    return;
  case R0ZeroModeType::Elide:
    if (UnitDirty) {
      li(r(0), 0);
    }
    return;
  }
}

DEF_OP(CallbackReturn) {
  // Spill SRA back to context
  SpillStaticRegs(TMP1);
  ResetStack();

  // Decrement signal handler ref counter
  int32_t ref_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, SignalHandlerRefCounter));
  lwz(TMP2, ref_off, STATE);
  addi(TMP2, TMP2, -1);
  stw(TMP2, ref_off, STATE);

  // Undo the dispatcher's callback entry (PPC64Dispatcher.cpp CallbackPtr):
  // it saved the interrupted thunk crossing's X30 at [SP] and moved SP down
  // 16. The guest callee has returned (to here, through X30) with SP back
  // where it found it, so restore X30 from [SP] and SP += 16. The crossing's
  // refill then reads both from the frame: the callback killed the
  // InSyscallInfo sentinel, so that refill is the full one.
  int32_t sp_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, State.sp));
  int32_t lr_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, State.x) + 30 * sizeof(uint64_t));
  ld(TMP2, sp_off, STATE);
  ld(TMP3, 0, TMP2);
  std(TMP3, lr_off, STATE);
  addi(TMP2, TMP2, 16);
  std(TMP2, sp_off, STATE);

  PopCalleeSavedRegisters();
  blr();
}

// Inlined L1 lookup at every block exit.
//
// Previously this op stored State.rip, ran SpillStaticRegs (62 instructions),
// and jumped to Pointers.DispatcherLoopTop, which then did the ~15-instruction
// L1 probe and bctr'd to the block. On an L1 hit that whole spill was pure
// waste: ppc64le SRA registers are FIXED physical assignments (PPC64Emitter.h
// x64::SRA / x32::SRA), disjoint from the dynamic RA pool, so no SSA value is
// ever allocated to one and guest state is live in registers unconditionally.
// The dispatcher's hit leg branches to CodeData.EntryPoints[GuestEntry], which
// JIT.cpp records AFTER EmitEntryPoint's FillStaticRegs, so the warm target
// never refills — meaning the spill was written and then immediately made
// redundant by registers that were never disturbed.
//
// So: do the probe here and jump straight to the target, and pay the spill
// only on the miss leg.
//
// Register / flag discipline this sequence must honour:
//
//   CR7, never CR0. CR0 carries the block's live packed-NZCV (FillStaticRegs
//   sets it, downstream FromNZCV consumers read it) and SpillStaticRegs on the
//   miss leg packs CR0+XER into flags[RFLAG_NZCV_LOC]. Every instruction below
//   is a no-Rc form and the compare targets cr7, so both CR0 and XER reach the
//   miss-leg spill exactly as the block left them.
//
//   r0 stays 0. Guest loads/stores are X-form with r0 in the rB slot (where
//   PPC does NOT substitute literal zero), so a nonzero r0 silently offsets
//   every memory access in the target block. Nothing here writes r0, and no
//   rB=r0 form is used; the invariant simply carries through, which is why the
//   dispatcher's `li r0, 0` has no counterpart on the inlined hit leg.
//
//   Address dependency on the HostCode load. See the ORDERING NOTE in
//   PPC64Dispatcher.cpp and LookupCacheEntry::Publish: the conditional branch
//   does NOT order load->load on Power, so the GuestCode value is fed into the
//   HostCode load's base register.
//
// BLOCK LINKING (constant-target exits, BlockLinking knob).
//
// When BlockLinkingEnabled and this exit has a constant target RIP —
// Hint == None (plain jumps) or Hint == Call (guest CALL, whose target is a
// constant and whose x86 return address is an EntrypointOffset constant the
// guest pushed to ITS stack, entirely independent of host control flow) —
// the exit is reordered so the WHOLE L1 probe becomes the patch target.
//
// Calls were HISTORICALLY excluded by an objection about patching to `bl`:
// that would push the probe's second instruction onto the hardware link
// stack while the architectural return goes elsewhere, mispredicting every
// call/ret pair. But the linker below never emits `bl` — it patches a plain
// `b` (or `b Thunk`), creating no link-stack entry, and a 2026-08-05 storm
// profile put the unlinked path (ExitFunctionLink->FindBlock) at 4.1% of
// CPU in a call-dense Mono workload, so the exclusion cost real time for a
// hazard the mechanism doesn't have. The backend consults Hint nowhere else
// (verified by audit), and linked targets land on the callee's EntryPoint
// prologue, whose deferred-signal drain is hint-agnostic.
//
// Still excluded: Return (dynamic target — nothing constant to link) and
// CheckTF (must reach the dispatcher's trap check every time). A shadow
// return stack for the Return side is the remaining, genuinely
// design-heavy half.
//
//     li    r0, 0                         hoisted from the hit leg (P5.0.2)
//   PatchSite:                            4-byte aligned by construction
//     InsertExitRIPMove TMP1, NewRIP      (1-5 insns; 5 fixed when
//                                          ExitRIPFixedWidth -- see below)
//     std   TMP1, State.rip(STATE)        sunk from BOTH probe legs
//     <L1 probe, hit leg ends mtctr;bctr, miss leg spills and branches to
//      this exit's jump thunk LinkPath — see CompileCode's thunk emission>
//
// Unlinked, PatchSite holds the RIP move's first instruction and behaviour is
// identical to the non-linking lowering below: the r0 re-zero runs earlier and
// the rip store runs once ahead of the probe instead of once per leg, so both
// legs still see exactly the state they would have — architecturally invisible.
// (For a shadow CALL the pre-probe push sits between PatchSite and the sunk
// region; see the link-stack pairing diagram in DEF_OP(ExitFunction).)
// On link, ExitFunctionLinkWithRecord atomically rewrites
// PatchSite to `b HostCode` (I-form, ±32MiB) or, out of range — the common
// case against a 128MiB code buffer — `b Thunk`, with the thunk's own first
// word patched to the `bcl 20,31,$+4` PC-discovery idiom that loads HostCode
// from the adjacent record.
//
// The ordering is load-bearing:
//   - li r0,0 BEFORE PatchSite: a linked branch skips everything after the
//     patch site, so leaving the re-zero inside the probe's hit leg would let
//     a nonzero r0 reach the target block's X-form rB=r0 addressing — silent
//     guest memory corruption. The invariant must be re-established before the
//     patchable word, not after it. This is not negotiable and does not move.
//   - the RIP constant and `std State.rip` AFTER PatchSite: on a linked exit
//     the destination-RIP register is dead (the branch target is a patched
//     immediate; nothing past the patch site executes), so both are pure cost
//     there. Both probe legs still need them, and both run after the patch
//     site, so the unlinked path is unchanged. This is the sink; see the long
//     comment at the sink decision in DEF_OP(ExitFunction) for the State.rip
//     reader analysis that makes it sound.
//
// FEX_NOSINKEXITRIP restores the hoisted form (RIP constant + `std State.rip`
// above the patch site) for A/B and bisection.
// ---------------------------------------------------------------------------
// A64 guest call/return pairing (P3, OPTIMIZATION-CHECKLIST.md)
// ---------------------------------------------------------------------------
// The x86-shaped pairing further down pushes a host continuation that is a
// block of the SAME compile unit (CallReturnBlock). The A64 frontend compiles
// one guest block per unit and ends it at every branch, so a BL/BLR never has
// its return block in the unit, and that layout would push {0,0}.
//
// Here the continuation is a trampoline in the CALLER's unit: a constant exit
// to the return address R, in link-first form, so once linked it is a single
// `b` to R's translation. The call ends in an LK=1 branch whose next word IS
// that trampoline, and pushes {R, &trampoline} on the per-thread shadow stack:
//
//   BL (constant target T)                BLR (register target)
//     push {R, &Tramp}                      push {R, &Tramp}
//     [li r0,0]                             L1 probe on the target:
//   A: b MissLeg     -> linked: bl T'        hit: mtctr; std rip; [li r0,0]
//   Tramp: exit to R (link-first)                 bctrl
//     ...                                 Tramp: exit to R (link-first)
//   MissLeg: rip = T; b LinkPath            miss: std rip; b spill+dispatch
//
// A guest RET (DEF_OP(ExitFunction), Return hint) pops the top entry and, when
// its guest address equals the RET's actual target, loads the trampoline into
// LR and returns with `blr`, which the hardware link stack predicts because
// the call's `bl`/`bctrl` pushed exactly that word.
//
// CORRECTNESS does not depend on the pairing being right. A trampoline does
// nothing but continue at its R with the guest state as it is, so executing
// any entry whose R equals the RET target is exact, whoever pushed it: a stale
// entry left by longjmp, a tail call or a signal frame either does not match
// (the RET falls back to the L1 probe on its real target, and the pop is
// unconditional so stale entries drain) or matches and still continues at the
// right guest address. What the hardware link stack predicts is only a
// prediction: LR is loaded explicitly, so a desynchronised link stack costs a
// mispredict, never a wrong branch. A call that did not reach its `bl` (first
// execution, before A is linked; the BLR probe's miss leg) pushed an entry
// without a link-stack push: its return mispredicts once and is still exact.
// Host trampolines never outlive their code: every code-buffer rotation and
// every guest code invalidation zeroes the stack. A zero entry matches only a
// RET to guest address 0; its `blr` to host address 0 faults outside the code
// buffer with State.pc already 0, which is the guest SIGSEGV the RET owes
// (the L1 probe has the same property for an empty entry).
//
// Neither the push nor the pop checks bounds (7 and 9 instructions before
// the r0 re-zero and the branch). An overflow or underflow faults on a guard page of the
// call-ret allocation at the first ld/std through the stack pointer register;
// SyscallHandler::HandleSegfault resets that register to the default location
// and retries. Tested by unittests/A64Frontend/callret.c (120000-deep
// recursion, 300000 unpaired RETs). PC discovery for &Tramp is lnia
// (addpcis) on ISA 3.0, which avoids the mflr SPR move; POWER8 uses
// bcl 20,31,$+4 ; mflr.
bool PPC64JITCore::ConstantCallReturnAddress(const IR::OrderedNodeWrapper& WNode, uint64_t* Value) const {
  // After register allocation a GPR argument is either an inline constant op
  // or an encoded register, so the frontend passes the return address as an
  // InlineEntrypointOffset rather than the SSA value it stored to X30.
  return IsInlineConstant(WNode, Value) || IsInlineEntrypointOffset(WNode, Value);
}

void PPC64JITCore::EmitLinkFirstConstExit(uint64_t Target) {
  const int16_t rip_off = static_cast<int16_t>(offsetof(FEXCore::Core::CpuStateFrame, State.pc));
  // The patch site is the first word of the RIP move; the linker rewrites it
  // to `b HostCode` (or `b Thunk`), skipping the rest.
  PendingJumpThunks.push_back({GetCursorAddress<uint64_t>(), Target, {}});
  InsertExitRIPMove(TMP1, Target);
  std(TMP1, rip_off, STATE);
  b(&PendingJumpThunks.back().LinkPath);
}

void PPC64JITCore::EmitA64PairedCall(const IR::IROp_ExitFunction* Op, bool ConstRIP, uint64_t NewRIP, uint64_t ReturnAddress,
                                     bool UnitR0Dirty) {
  const int16_t rip_off = static_cast<int16_t>(offsetof(FEXCore::Core::CpuStateFrame, State.pc));
  const int16_t sp_off = static_cast<int16_t>(offsetof(FEXCore::Core::CpuStateFrame, State.callret_sp));

  // The guest return address for the push is X30, which the frontend stored
  // just before this exit and which lives in its pinned static register at
  // every block exit (the next block reads it there).
  GPR RetReg = TMP1;
  for (size_t i = 0; i < FEXCore::Core::StaticGPRGuestReg.size(); ++i) {
    if (FEXCore::Core::StaticGPRGuestReg[i] == 30) {
      RetReg = a64::SRA[i];
    }
  }
  if (RetReg == TMP1) {
    LoadConstant(TMP1, ReturnAddress);
  }
  GPR TargetReg = ConstRIP ? TMP1 : GetReg(Op->NewRIP);

  // Push {R, &Tramp}. The addi's displacement is patched once Tramp's address
  // is known.
  uint64_t Anchor;
  if (CTX->HostFeatures.SupportsISA30) {
    lnia(TMP2);
    Anchor = GetCursorAddress<uint64_t>();
  } else {
    bcl(20, 31, 4);                     // no link-stack push
    Anchor = GetCursorAddress<uint64_t>();
    mflr(TMP2);
  }
  uint32_t* AddiWord = GetCursorAddress<uint32_t*>();
  addi(TMP2, TMP2, 0);
  // No bounds check: a push below the base faults on the guard page and
  // SyscallHandler::HandleSegfault resets TMP3 (the faulting store's base
  // register) to the default location, then the store retries.
  ld(TMP3, sp_off, STATE);
  addi(TMP3, TMP3, -16);
  std(RetReg, 0, TMP3);
  std(TMP2, 8, TMP3);
  std(TMP3, sp_off, STATE);

  auto PatchTramp = [&]() {
    const int64_t Delta = static_cast<int64_t>(GetCursorAddress<uint64_t>()) - static_cast<int64_t>(Anchor);
    LOGMAN_THROW_A_FMT(Delta > 0 && Delta < 0x7FFF, "A64 call trampoline out of addi reach: {}", Delta);
    *AddiWord = (14u << 26) | (static_cast<uint32_t>(TMP2.idx) << 21) | (static_cast<uint32_t>(TMP2.idx) << 16) |
                (static_cast<uint32_t>(Delta) & 0xFFFFu);
  };

  PPC64Emitter::Label MissLeg {};
  if (ConstRIP) {
    EmitExitR0Zero(UnitR0Dirty);
    // A: the record's caller word, and also its Final word: FinalOffset ==
    // CallerOffset tells the linker to write `bl` here in place.
    const uint64_t A = GetCursorAddress<uint64_t>();
    PendingJumpThunks.push_back({A, NewRIP, {}});
    auto* Record = &PendingJumpThunks.back();
    Record->FinalAddress = A;
    b(&MissLeg);
    PatchTramp();
    EmitLinkFirstConstExit(ReturnAddress);
    Bind(&MissLeg);
    InsertExitRIPMove(TMP1, NewRIP);
    std(TMP1, rip_off, STATE);
    b(&Record->LinkPath);
  } else {
    const int32_t l1_off = static_cast<int32_t>(offsetof(FEXCore::Core::CpuStateFrame, State.L1Pointer));
    const int32_t l1mask_off = static_cast<int32_t>(offsetof(FEXCore::Core::CpuStateFrame, State.L1Mask));
    ld(TMP2, l1_off, STATE);
    if (!FEXCore::Config::Get_DYNAMICL1CACHE()) {
      constexpr uint32_t L1MB = 64 - (std::countr_zero(FEXCore::LookupCache::MAX_L1_ENTRIES) + 4);
      rldic(TMP4, TargetReg, 4, L1MB);
    } else {
      ld(TMP3, l1mask_off, STATE);
      sldi(TMP4, TargetReg, 4);
      and_(TMP4, TMP4, TMP3);
    }
    add(TMP2, TMP2, TMP4);
    ld(TMP4, 8, TMP2);                  // GuestCode, loaded first (see the probe below)
    cmpd(cr(7), TMP4, TargetReg);
    bc({4, 30}, &MissLeg);
    xor_(TMP3, TMP4, TMP4);
    ldx(TMP3, TMP2, TMP3);              // HostCode under the GuestCode address dependency
    mtctr(TMP3);
    std(TargetReg, rip_off, STATE);
    EmitExitR0Zero(UnitR0Dirty);
    bctrl();                            // LK=1: link stack <- &Tramp
    PatchTramp();
    EmitLinkFirstConstExit(ReturnAddress);
    Bind(&MissLeg);
    std(TargetReg, rip_off, STATE);
    SharedSpillExitUsed = true;
    b(&SharedSpillExitLabel);
  }

}

DEF_OP(ExitFunction) {
  auto Op = IROp->C<IR::IROp_ExitFunction>();
  // Snapshot the unit's r0-dirty state BEFORE this handler emits anything:
  // the shadow-RET fast path below emits its own `li r0,0`, which would
  // otherwise make the hoist and hit leg below think the unit was dirty.
  const bool UnitR0Dirty = R0Dirty();
  ResetStack();

  const int32_t rip_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, State.pc));

  // ---------------------------------------------------------------------
  // Materialise the destination RIP into a register.
  //
  // TMP1 is the staging register: the probe below uses only TMP2/TMP3/TMP4,
  // and in the register case RIPReg is an RA- or SRA-allocated GPR (r7-r26,
  // r30, r31 across both guest modes) which is disjoint from TMP1-TMP4, so
  // nothing here can clobber a still-live SSA value. ResetStack may itself
  // use TMP1 for oversized frames, hence this comes after it.
  // ---------------------------------------------------------------------
  GPR RIPReg = TMP1;
  uint64_t NewRIP = 0;
  bool ConstRIP = false;
  if (IsInlineConstant(Op->NewRIP, &NewRIP) ||
      IsInlineEntrypointOffset(Op->NewRIP, &NewRIP)) {
    ConstRIP = true;
  }

  // Emission of the constant destination RIP is a closure because the sink
  // (default; FEX_NOSINKEXITRIP restores the hoist) moves it below the patch
  // site — and, for a shadow CALL, below the pre-probe push as well. See the
  // sink decision further down.
  auto EmitConstRIPIntoTMP1 = [&]() {
    // S3.7-C2: this constant is a guest RIP baked into host instruction bytes;
    // a code cache saved in one ASLR session and loaded in another would jump
    // to a stale address without a RELOC_GUEST_RIP_MOVE. Record placed AFTER
    // the 32-bit mask so the recorded value matches the emitted immediate.
    // SMC Idea 4: this is the ONLY guest-RIP constant the semantic-patch fault
    // handler is allowed to rewrite, so it is the only one recorded.
    //
    // WIDTH: InsertExitRIPMove emits the fixed 20-byte window only when
    // something rewrites it in place — code caching (ApplyCodeRelocations) or
    // FEX_SMCSEMANTICPATCH (the fault handler's SynthesizeRIPWindow). With
    // both off it degrades to a variable-width LoadConstant and records no
    // relocation; see PPC64JITCore::ExitRIPFixedWidth in JIT.cpp. Neither the
    // hoist below nor the linker cares about the width — PatchSite is captured
    // from the cursor after this call, not computed from a fixed offset.
    // (Note BlockLinking is separately interlocked off when
    // FEX_SMCSEMANTICPATCH is enabled; see JIT.cpp BlockLinkingEnabled.)
    InsertExitRIPMove(TMP1, NewRIP);
  };

  // -------------------------------------------------------------------------
  // Shadow-RET / linkability predicates, resolved before anything is emitted
  // because the sink decision below depends on both.
  // -------------------------------------------------------------------------
  // A call whose return continuation is not a block of this compile unit (the
  // A64 frontend: one guest block per unit, so never) pairs only through the
  // A64 layout below, which needs the return address as a compile-time
  // constant and the record linker. Without both it is a plain exit.
  uint64_t A64CallReturn = 0;
  const bool A64CallShape = Op->Hint == IR::BranchHint::Call && Op->CallReturnBlock.IsInvalid();
  const bool A64Call = ShadowRetStackEnabled && A64CallShape && CallLinkingEnabled && !Op->CallReturnAddress.IsInvalid() &&
                       ConstantCallReturnAddress(Op->CallReturnAddress, &A64CallReturn);
  const bool ShadowActive = ShadowRetStackEnabled &&
    (Op->Hint == IR::BranchHint::Return || (Op->Hint == IR::BranchHint::Call && (!A64CallShape || A64Call)));

  // Plain jumps link whenever BlockLinkingEnabled. CALL exits additionally
  // require CallLinkingEnabled (= BlockLinkingEnabled && !LazyLinkArmed):
  // under FEX_SMCLAZYLINK the SMC scrub severs links constantly, and call-
  // dense guests (32-bit Mono/Unity — Dex) then flood ExitFunctionLinkWith
  // Record with relink-and-recompile on every call, a compile storm that
  // throttles guest execution (measured on Dex load 2026-08-05). Under lazy
  // linking, calls take the fast rldic L1 probe instead; the call-linking win
  // is retained only where links actually stick (non-lazy configs).
  const bool Linkable = ConstRIP &&
    ((Op->Hint == IR::BranchHint::None && BlockLinkingEnabled) ||
     (Op->Hint == IR::BranchHint::Call && CallLinkingEnabled));

  // -------------------------------------------------------------------------
  // EXIT-RIP SINK (default ON; FEX_NOSINKEXITRIP restores the hoisted form).
  //
  // On a LINKED constant exit the destination RIP register is dead: the branch
  // target is a patched immediate and nothing past the patch site executes. The
  // only reason the constant and its `std State.rip` ever sat ABOVE the patch
  // site was the P5.0.1 invariant. Sinking both below it makes a linked exit
  //     [ResetStack] ; [li r0,0] ; b HostCode
  // instead of up to eight instructions. Both probe legs still materialise and
  // store it, so the UNLINKED path is byte-identical either way.
  //
  // WHAT P5.0.1 BOUGHT, and why it is no longer needed here.
  //
  // RestoreRIPFromHostPC (Core.cpp) reconstructs a guest RIP from a host PC
  // using the block's vl64pair table, and falls back to Frame->State.rip when
  // it cannot find a block for that PC. SignalDelegator::SpillSRA assigns the
  // result straight into State.rip on every guest signal. So the question is
  // only ever: which host PCs fail to resolve to a block?
  //
  // Before audit P1 the answer included a "target prologue window": the block
  // lookup went through State.InlineJITBlockHeader, which the JIT published
  // from each block's own EntryPoint prologue, so a signal taken in the first
  // few instructions of a freshly-entered block — after the branch, before
  // that block's header store retired — still named the PREVIOUS block and
  // failed the containment test. That is exactly the window a linked exit
  // jumps through, and it is why the sink shipped off.
  //
  // Audit P1 closes it by construction. CodeBuffer::AppendBlock maintains a
  // per-CodeBuffer host-PC -> block index, written at the end of CompileCode
  // under the code-buffer write lock, so a host PC inside ANY block resolves
  // to that block's JITCodeHeader/JITCodeTail and reconstructs from the
  // vl64pair table with the JIT publishing nothing at run time. There is no
  // prologue window any more: the first instruction of the target block is
  // already inside an indexed block.
  //
  // The State.rip fallback therefore only remains for host PCs in NO block:
  //   - SMC auxiliary stubs living in the code buffer's free tail (emitted
  //     outside any block's [BlockBegin, BlockBegin+Tail->Size) span);
  //   - host helpers reached FROM a block — thunks and the x87/transcendental
  //     FABI helpers — whose asynchronous deliveries SignalDelegator.cpp
  //     defers anyway via the kInFABISentinel and in-code-buffer rules, so no
  //     guest signal frame is built from such a PC in the first place;
  //   - syscalls, where the frontend stores State.rip explicitly before the
  //     call (OpcodeDispatcher.cpp:5491) and the value is therefore exact.
  // None of those is reached by falling off the end of a linked exit, so the
  // sunk store's absence is not observable on any of them.
  //
  // Retained deliberately, pending their own proof (each is ONE instruction on
  // a register that is live at that point anyway, so the upside is small and
  // the argument is a different one): the shadow-RET fast path's
  // `std(RIPReg, rip_off)` and the inline-cache LinkedEntry leg's
  // `std(RIPReg, rip_off)` + r0 re-zero. Also unchanged: every miss leg's
  // store, which the dispatcher / ExitFunctionLinker / indirect-record linker
  // genuinely READ as the lookup key.
  //
  // Regression detector if this is ever doubted: FEX_RIPFALLBACKTRAP counts
  // (or aborts on) RestoreRIPFromHostPC fallbacks taken with an in-code-buffer
  // host PC.
  // -------------------------------------------------------------------------
  static const bool SinkExitRIP = getenv("FEX_NOSINKEXITRIP") == nullptr;

  // The sink now covers the constant shadow CALL too. For a Linkable exit with
  // ShadowActive, Op->Hint must be Call (a Return is never Linkable and a None
  // hint is never ShadowActive), i.e. Linkable && ShadowActive == Linkable &&
  // ShadowCall — so the old `&& !ShadowActive` guard was excluding exactly the
  // shadow-call case, and dropping it is the whole extension.
  //
  // Why it is sound there. For a shadow CALL the patch site A is the `bcl` of
  // the pre-probe push (EmitShadowCallPush), and a linked A becomes
  // `b LinkedEntry`; the LinkedEntry leg does its OWN push and ends in
  // `bl HostCode`. Nothing on that leg reads RIPReg: the push stores
  // GetReg(Op->CallReturnAddress) — a separate, still-live register holding
  // the guest return address — and TMP2 (the trampoline address it computes
  // itself), never RIPReg/TMP1. So RIPReg is dead on the linked path exactly
  // as it is for a plain linked jump.
  //
  // Where it goes: NOT immediately after the patch-site registration (that
  // would displace the `bcl`, which must remain the first — and only — word
  // the linker rewrites), but immediately AFTER the pre-probe push returns and
  // BEFORE the L1 probe. Inserting there is free: the push touches only
  // TMP2/TMP3/TMP4 (TMP1, i.e. RIPReg, is untouched) and its one compile-time
  // backpatch, PatchShadowCallAddi, computes its delta from real emitted
  // addresses rather than a fixed instruction count.
  const bool SinkLinkedRIP = SinkExitRIP && Linkable;
  // Emit the sunk region after the shadow push rather than at the patch site.
  // (ShadowCall itself is only declared further down, but by the identity
  // above `SinkLinkedRIP && ShadowActive` names exactly the same exits.)
  const bool SinkAfterShadowPush = SinkLinkedRIP && ShadowActive;

  // ---------------------------------------------------------------------
  // LINK FIRST (POWERARM_NOLINKFIRST=1 restores the probe-first form).
  //
  // An unlinked constant exit used to run the inline L1 probe and reach the
  // record linker only on an L1 *miss*. A target this thread had already
  // dispatched to sits in its L1, so such a site hit the probe on its first
  // execution and was never linked: it paid the probe's dependent chain
  // (ld;rldic;add;ld;cmpd;bne;ldx;mtctr;bctr, ~21 cycles with the FXU stall
  // on mtctr, and a count-cache bctr) for the life of the block. Measured on
  // `cc1 -O2 lvm.c` (2026-09-17 branch census): 162k of 370k constant exit
  // sites stayed unlinked and executed 197M times, 12% of all block exits.
  //
  // So when links stick (CallLinkingEnabled: block linking on, not the
  // lazy-link regime, whose scrub severs links constantly), the unlinked
  // path goes straight to the record linker: PatchSite (the sunk RIP move),
  // std State.rip, b LinkPath. The linker looks up or compiles the target and
  // patches PatchSite, so every later execution is the single linked `b`.
  // A site the linker cannot patch (buffer rotated under it, a lookup race)
  // simply re-enters the linker next time; the only permanent failure,
  // LinkOutcomeUnreachable, needs a compile unit larger than a `b`'s reach.
  // Paired A64 calls use the same link-first form (EmitA64PairedCall); the
  // x86-shaped shadow CALL keeps its own layout (the push precedes the probe).
  // ---------------------------------------------------------------------
  static const bool NoLinkFirst = getenv("POWERARM_NOLINKFIRST") != nullptr;
  const bool LinkFirst = SinkLinkedRIP && !ShadowActive && CallLinkingEnabled && !NoLinkFirst;

  if (A64Call) {
    EmitA64PairedCall(Op, ConstRIP, NewRIP, A64CallReturn, UnitR0Dirty);
    return;
  }

  if (ConstRIP) {
    if (!SinkLinkedRIP) {
      EmitConstRIPIntoTMP1();
    }
    RIPReg = TMP1;
  } else {
    GPR NewRIPReg = GetReg(Op->NewRIP);
    RIPReg = NewRIPReg;
  }

  // ---------------------------------------------------------------------
  // Shadow return stack (FEX_SHADOWRETSTACK). See JITClass.h, the
  // resolution+interlock in JIT.cpp, and the CallReturnEntryLabels bind in
  // CompileCode's block loop.
  //
  // RET pop: peek the top {guest_ret_rip, host_trampoline}; if the recorded
  // guest RIP equals this RET's target, branch straight to the trampoline
  // (which re-enters the return block at its L1-hit landing), skipping the L1
  // probe. The pop is unconditional (mirrors the stack discipline): a mismatch,
  // an empty stack, or any prior invalidation just falls through to the probe,
  // which is the correctness net.
  //
  // CALL push: record {guest_ret_rip, &trampoline} so a matching RET can take
  // the fast path. Emitted BEFORE the existing (possibly linkable) exit to the
  // callee, which is left byte-for-byte unchanged.
  //
  // Discipline: every compare targets cr7 with a no-Rc form, so CR0's packed
  // NZCV and XER reach the target block / the miss-leg spill exactly as the
  // block left them (identical to the L1 probe below). r0 is rewritten (to 0)
  // only on the taken RET fast path, immediately before the bctr, preserving
  // the X-form zero-index invariant. The bounds are read per-op from the
  // frame's callret_base/callret_end mirrors (CoreState.h) — the code buffer
  // is shared across threads so they cannot be immediates, and the mirrors
  // replace the former Frame->Thread->CallRetStackBase two-load pointer chase
  // (+addis for base+SIZE) with one D-form ld off STATE; callret_end shares
  // callret_sp's cache line for the pop. Both mirrors are set at thread
  // creation next to callret_sp and never change (base is immutable until
  // thread teardown). [base, base+SIZE) is exactly the R/W region between the
  // frontend's two guard pages, so the bounds checks never fault. Overflow
  // (push) resets to empty and skips the store; empty (pop) skips the fast
  // path; neither ever corrupts memory. Threads without a frontend-allocated
  // call-ret stack carry zero mirrors: pops read sp(0) >= end(0) -> empty,
  // pushes see new sp(-16) < base(0) -> overflow reset, never storing.
  // ---------------------------------------------------------------------
  PPC64Emitter::Label ShadowRetReprobe{};
  // ---------------------------------------------------------------------
  // Link-stack pairing [2026-09-06]. On the POWER8 this port targets the
  // count cache predicts NOTHING: a bcctr/bcctrl to a target that never
  // changes mispredicts on every execution (native microbench, CTR written
  // once outside the loop: 1.000 pm_br_mpred_ccache per iteration, ~20
  // cycles), while direct branches and the link stack predict perfectly
  // (bl/blr pair: 0.000). So every indirect exit this backend emits costs
  // a mispredict, and the one exit class that CAN be made predictable is
  // the return: if the CALL ends in an LK=1 branch (bctrl, or the linked
  // bl), the hardware pushes the word after it on the link stack, and a
  // RET that restores exactly that word into LR and ends in `blr` is
  // predicted. The shadow stack already carries a host continuation per
  // call; the layout below makes that continuation THE word after the
  // call's final branch, so the entry's pointer and the link stack agree:
  //
  //   [Linkable: hoisted r0 zero]
  //   A:      bcl 20,31,$+4          <- Linkable: the patch site (b LinkedEntry)
  //           mflr TMP2 ; addi TMP2,TMP2,D1    D1 patched = &Tramp1 - &mflr
  //           push {guest_ret_rip, TMP2}
  //   [Linkable: SUNK rip constant + std State.rip -- skipped when linked]
  //           L1 probe ... hit: mtctr TMP3 ; [rip, r0] ; bctrl
  //   Tramp1: b CallReturnEntryLabels[return block]
  //   Miss:   (unchanged miss leg)
  //   [Linkable only, reachable only through a patched A:]
  //   LinkedEntry: bcl ; mflr TMP2 ; addi TMP2,TMP2,D2 ; push {ret_rip, TMP2}
  //   Final:  trap                   <- linker writes bl HostCode / bl ThunkStart
  //   Tramp2: b CallReturnEntryLabels[return block]
  //
  // A linked call pushes on its own leg because its return word differs
  // (Tramp2, after the bl); the pre-probe push is skipped by construction
  // since A itself is the patch site. Final is written while unreachable
  // and A is the only word that ever flips live, so the single-atomic-word
  // contract of the block linker is untouched, and a delink restores A
  // alone (Final goes stale but unreachable). The far form `bl ThunkStart`
  // still works: the thunk's bcl 20,31,$+4 is the no-push form and its
  // bctr does not touch the link stack, so the callee returns to Tramp2.
  // Without a return block in this unit the pushed entry is {0,0}, never
  // matches, and the trampoline word is a `trap` nothing ever reaches
  // architecturally (the link stack may fetch it speculatively).
  //
  // The RET fast path ends in mtlr/blr instead of mtctr/bctr for the same
  // reason. Everything is byte-identical to the pre-existing lowering when
  // FEX_SHADOWRETSTACK is off.
  // ---------------------------------------------------------------------
  // Bisection levers (read once): FEX_NO_INLINECACHE=1 keeps the indirect
  // shadow call on the probe; FEX_NO_LINKSTACKPAIR=1 keeps the shadow
  // layout but ends the call in bctr and the return in mtctr/bctr, i.e.
  // the pre-pairing branch forms with the pairing's push/pop bookkeeping.
  static const bool NoInlineCache = getenv("FEX_NO_INLINECACHE") != nullptr;
  static const bool NoLinkStackPair = getenv("FEX_NO_LINKSTACKPAIR") != nullptr;
  const bool ShadowCall = ShadowActive && Op->Hint == IR::BranchHint::Call;
  const uint32_t CRBID = (ShadowCall && !Op->CallReturnBlock.IsInvalid()) ?
                         IR->GetOp<IR::IROp_CodeBlock>(Op->CallReturnBlock)->ID : 0u;
  const int16_t sp_off = static_cast<int16_t>(offsetof(FEXCore::Core::CpuStateFrame, State.callret_sp));
  const int16_t base_off = static_cast<int16_t>(offsetof(FEXCore::Core::CpuStateFrame, State.callret_base));
  const int16_t end_off = static_cast<int16_t>(offsetof(FEXCore::Core::CpuStateFrame, State.callret_end));

  // Emits bcl/mflr/addi(placeholder) + the push. Returns {&mflr, &addi word}
  // so the caller can patch the addi's SI once the trampoline address is
  // known (compile-time write into the buffer we are emitting into; the
  // whole unit is icache-flushed once at the end of CompileCode).
  auto EmitShadowCallPush = [&]() -> std::pair<uint64_t, uint32_t*> {
    bcl(20, 31, 4);                     // LK form the link stack does not push
    const uint64_t MflrAddr = GetCursorAddress<uint64_t>();
    mflr(TMP2);                         // TMP2 = &mflr
    uint32_t* AddiWord = GetCursorAddress<uint32_t*>();
    addi(TMP2, TMP2, 0);                // SI patched: TMP2 = &trampoline
    ld(TMP3, sp_off, STATE);            // TMP3 = sp
    ld(TMP4, base_off, STATE);          // TMP4 = base (frame mirror)
    addi(TMP3, TMP3, -16);              // TMP3 = new sp
    cmpd(cr(7), TMP3, TMP4);
    PPC64Emitter::Label l_do_push{}, l_push_done{};
    bc({4, 28}, &l_do_push);            // new sp >= base -> room to push
    // Overflow: reset to empty (base+SIZE, read from the end mirror), skip
    // the store, never write below the low guard page.
    ld(TMP4, end_off, STATE);
    std(TMP4, sp_off, STATE);
    b(&l_push_done);
    Bind(&l_do_push);
    if (!Op->CallReturnBlock.IsInvalid()) {
      std(GetReg(Op->CallReturnAddress), 0, TMP3); // slot0 = guest_ret_rip
      std(TMP2, 8, TMP3);                          // slot8 = host trampoline
    } else {
      li(TMP2, 0);                      // no return block: a {0,0} entry never matches
      std(TMP2, 0, TMP3);
      std(TMP2, 8, TMP3);
    }
    std(TMP3, sp_off, STATE);           // callret_sp = new sp
    Bind(&l_push_done);
    return {MflrAddr, AddiWord};
  };
  auto PatchShadowCallAddi = [&](std::pair<uint64_t, uint32_t*> Push, uint64_t TrampAddr) {
    const int64_t Delta = static_cast<int64_t>(TrampAddr) - static_cast<int64_t>(Push.first);
    LOGMAN_THROW_A_FMT(Delta > 0 && Delta < 0x7FFF, "shadow call trampoline out of addi reach: {}", Delta);
    // addi TMP2, TMP2, Delta  (D-form, opcode 14)
    *Push.second = (14u << 26) | (static_cast<uint32_t>(TMP2.idx) << 21) |
                   (static_cast<uint32_t>(TMP2.idx) << 16) | (static_cast<uint32_t>(Delta) & 0xFFFFu);
  };
  // The trampoline word the link stack returns to: a direct branch to the
  // return block's shadow entry label, or a trap when the unit has none.
  auto EmitShadowCallTrampoline = [&]() {
    if (!Op->CallReturnBlock.IsInvalid()) {
      b(&CallReturnEntryLabels[CRBID]);
    } else {
      Emit32(0x7FE00008u);              // trap (tw 31,0,0): never reached architecturally
    }
  };

  if (ShadowActive && Op->Hint == IR::BranchHint::Return) {
    // RET pop: peek {guest_ret_rip, host_trampoline}; on a match restore
    // the trampoline into LR and `blr` -- the link stack, primed by the
    // call's LK=1 branch, predicts this; the count cache would not.
    // No empty check: a pop past the top of the stack faults on the guard
    // page above it, and SyscallHandler::HandleSegfault resets TMP2 to the
    // default location and retries the load (a stale or zero entry follows,
    // which cannot match a live trampoline wrongly; see EmitA64PairedCall).
    ld(TMP2, sp_off, STATE);            // TMP2 = sp
    // Rule 4 (POWER9 pipeline research): move the host trampoline into the
    // branch register as early as its value exists, so the 5-6 cycle SPR move
    // overlaps the guest-address compare instead of preceding the blr. LR/CTR
    // are scratch on the mismatch path (the probe reloads CTR).
    ld(TMP4, 8, TMP2);                  // TMP4 = host trampoline (top slot + 8)
    ld(TMP3, 0, TMP2);                  // TMP3 = guest_ret_rip (top slot)
    if (NoLinkStackPair) {
      mtctr(TMP4);
    } else {
      mtlr(TMP4);
    }
    cmpd(cr(7), TMP3, RIPReg);
    addi(TMP2, TMP2, 16);               // pop (unconditional, mirrors the stack discipline)
    std(TMP2, sp_off, STATE);
    bc({4, 30}, &ShadowRetReprobe);     // guest_ret_rip != target -> probe
    // P5.0.1: store rip before the jump. RETAINED deliberately even though the
    // exit-RIP sink retires the equivalent store on linked constant exits:
    // this is one instruction on a register that is live here anyway, and the
    // sink's soundness argument is about the LINKED path of a patched exit,
    // which this is not. Dropping it wants its own proof.
    std(RIPReg, rip_off, STATE);
    EmitExitR0Zero(UnitR0Dirty);        // P5.0.2: zero-index invariant
    if (NoLinkStackPair) {
      bctr();
    } else {
      blr();
    }
    // Empty / mismatch fall through into the unchanged L1 probe (ShadowRetReprobe).
  }

  // ---------------------------------------------------------------------
  // Block-linking hoist + patch-site registration (see header comment).
  // Only for constant-target exits with the knob on (plain jumps, and guest
  // CALLs when CallLinkingEnabled); every other exit shape keeps the exact
  // non-linking lowering below. Linkable was resolved above, before any
  // emission, because the sink decision depends on it.
  // ---------------------------------------------------------------------
  PPC64Emitter::Label* LinkPathLabel = nullptr;
  if (Linkable) {
    // Hoisted region: the r0 re-zero (always), plus the rip store when the
    // sink is off. Both probe legs below skip their own copies when Linkable.
    //
    // The r0 re-zero cannot move: a linked branch skips everything after the
    // patch site, so a nonzero r0 would reach the target block's X-form rB
    // addressing. The rip store and the constant that feeds it DO move, below
    // the patch site, leaving a linked exit as nothing but the r0 re-zero and
    // the patched branch (FEX_NOSINKEXITRIP restores the hoisted form).
    if (!SinkLinkedRIP) {
      std(RIPReg, rip_off, STATE);
    }
    EmitExitR0Zero(UnitR0Dirty);
    // PatchSite == the first word emitted from here on. For a plain jump that
    // is the sunk RIP move (or, sink off, the probe's ld of L1Pointer); for a
    // shadow CALL it is the pre-probe push's `bcl` and the sunk region goes
    // AFTER that push instead (SinkAfterShadowPush). Either way the linker
    // rewrites exactly this one word. All emitted instructions are 4 bytes and
    // SetBuffer lands on a 16-byte boundary, so this address is always 4-byte
    // aligned for the linker's atomic 4-byte rewrite.
    PendingJumpThunks.push_back({GetCursorAddress<uint64_t>(), NewRIP, {}});
    LinkPathLabel = &PendingJumpThunks.back().LinkPath;
    // Sunk region: everything from here on is skipped by a linked branch. The
    // probe below needs RIPReg regardless, so the constant is not duplicated --
    // it simply moved. State.rip is stored here so both probe legs (hit ->
    // bctr, miss -> LinkPath) still leave it correct, exactly as the hoisted
    // form did; only the linked fast path stops updating it.
    //
    // Overwriting the first sunk instruction is the intended behaviour: the
    // linker only ever patches this word forward, never back. Nothing on this
    // port unlinks -- LookupCache's BlockLinks map is permanently empty here
    // (see SMCSemanticPatch.h) so SeverBlockLinks is a no-op -- so the word is
    // dead the moment it is patched.
    //
    // SinkAfterShadowPush defers this to just past EmitShadowCallPush below:
    // for a shadow CALL the `bcl` of that push must be the FIRST word after
    // the registration above, because it is the word the linker rewrites.
    if (SinkLinkedRIP && !SinkAfterShadowPush) {
      EmitConstRIPIntoTMP1();
      std(RIPReg, rip_off, STATE);
    }
  }

  // ---------------------------------------------------------------------
  // Inline cache for an INDIRECT shadow call [2026-09-06]: on this POWER8
  // the count cache never predicts the probe's bctrl (see the pairing
  // comment above), so a monomorphic `call [reg]` / vtable call pays ~20
  // cycles per execution for nothing. The exit therefore carries a guarded
  // direct call that the block linker fills on the first miss, keyed on the
  // target it observed (record.GuestRIP == 0 marks the record indirect):
  //
  //   A:      b MISS                  <- patch site; linked: nop (fall in);
  //                                      given up: b PROBE
  //           lis  TMP2, 0            \  five words, imm fields written by
  //           ori  TMP2, TMP2, 0       |  the linker while A still skips
  //           sldi TMP2, TMP2, 32      |  them (they are unreachable until
  //           oris TMP2, TMP2, 0       |  A flips), so no half-written
  //           ori  TMP2, TMP2, 0      /   constant is ever compared
  //           cmpd cr7, RIPReg, TMP2
  //           bne  cr7, PROBE         <- polymorphic: today's path
  //   LinkedEntry: push(Tramp2) ; Final: bl HostCode ; Tramp2: b Entry
  //   PROBE:  push(Tramp1) ; L1 probe ; bctrl ; Tramp1
  //   Miss:   std rip ; b LinkPath    <- the record linker, like a const exit
  //
  // Unlinked, A goes straight to the linker rather than to the probe: the
  // linker only ever runs from a miss, and a target this thread already
  // dispatched to sits in its L1, so a site whose first execution hit the
  // probe would never be linked at all (measured: the crossing bench's
  // vtable call stayed unlinked forever). The linker fills the guard on
  // that first execution, or -- when it cannot (suspect target, out of
  // reach) -- points A at PROBE for good. First target wins until the
  // target block is erased (the record is registered under that RIP, so
  // Erase restores `b MISS` and the next execution relinks); a site that
  // turns out polymorphic pays the 7-instruction guard and takes the
  // probe. Gated like constant-call linking (CallLinkingEnabled).
  // ---------------------------------------------------------------------
  const bool InlineCache = ShadowCall && !ConstRIP && CallLinkingEnabled && !NoInlineCache;
  PPC64Emitter::Label InlineCacheProbe{};
  auto MissLabel = PPC64Emitter::Label{};
  if (InlineCache) {
    PendingJumpThunks.push_back({GetCursorAddress<uint64_t>(), 0 /* indirect */, {}});
    LinkPathLabel = &PendingJumpThunks.back().LinkPath;
    b(&MissLabel);                      // A: unlinked -> the linker
    lis(TMP2, 0);
    ori(TMP2, TMP2, 0);
    sldi(TMP2, TMP2, 32);
    oris(TMP2, TMP2, 0);
    ori(TMP2, TMP2, 0);
    cmpd(cr(7), RIPReg, TMP2);
    bc({4, 30}, &InlineCacheProbe);     // bne cr7
    auto& Thunk = PendingJumpThunks.back();
    auto Push2 = EmitShadowCallPush();
    // Every path out of a block stores the destination RIP (P5.0.1) and
    // re-zeroes r0 (P5.0.2) before the branch; a constant exit emits both
    // around its patch site, but this leg has none to share. Leaving r0
    // dirty here silently offset every X-form access in the callee
    // [2026-09-06: nw-cp2077 callback c0000005, nw-witcher3 c000001d and a
    // DXVK spinlock hang on the first build with this leg].
    //
    // The rip store is RETAINED deliberately: the exit-RIP sink applies only
    // to Linkable (constant-target) exits, which this is not (InlineCache
    // requires !ConstRIP), and it is one instruction on a register that is
    // live here anyway. Removing it wants its own proof; the r0 re-zero is
    // never removable, see above.
    std(RIPReg, rip_off, STATE);
    EmitExitR0Zero(UnitR0Dirty);
    Thunk.FinalAddress = GetCursorAddress<uint64_t>();
    Emit32(0x7FE00008u);                // Final: trap until linked
    PatchShadowCallAddi(Push2, GetCursorAddress<uint64_t>());
    EmitShadowCallTrampoline();         // Tramp2
    Bind(&InlineCacheProbe);
    // For an indirect record LinkedEntryOffset carries PROBE: the word the
    // linker points A at when it gives up on this site.
    Thunk.LinkedEntryAddress = GetCursorAddress<uint64_t>();
  }

  // ---------------------------------------------------------------------
  // P2: inline compare cache for a guest BR (POWER9 pipeline research Rule 2,
  // §4.5). The count cache predicts the LAST target of a bctr site, so an
  // interpreter's dispatch `br` mispredicts on every change of opcode, while
  // the direction predictor learns patterned sequences perfectly through a
  // chain of compares (period-8 dispatch: 35.3 cycles through bctr, 16.5
  // through a compare chain). Each slot is the indirect-call inline cache's
  // guarded direct branch, chained:
  //
  //   A0: b MISS0      <- unlinked: to the record linker (sampled, below),
  //                       which fills slot 0 with the target it observed,
  //                       then A0 becomes nop
  //       lis/ori/sldi/oris/ori TMP2 ; cmpd cr7, target, TMP2
  //       bne A1                        (the last slot: bne PROBE)
  //       std rip ; [li r0,0]
  //   F0: trap         <- linker: b HostCode / b Thunk
  //   A1: b MISS1 ...
  //   PROBE: the L1 probe, as before (hit: bctr; miss: dispatcher)
  //   MISSi: 1 in 64: std rip ; b LinkPath_i   otherwise: b PROBE
  //
  // Sampled targets win their slots, a slot whose target block is
  // erased is relinked by the next target to reach it, and a target the
  // linker refuses points its slot's A at PROBE for good. Unfilled slots cost
  // nothing past the first; filled non-matching slots cost two issue slots
  // each for the compare and branch (the constants have no dependency).
  // POWERARM_BRCACHESLOTS=N (0-8, default 8) sets the chain length.
  // ---------------------------------------------------------------------
  static const uint32_t BRCacheSlots = [] {
    const char* Env = getenv("POWERARM_BRCACHESLOTS");
    return Env ? std::min<uint32_t>(static_cast<uint32_t>(strtoul(Env, nullptr, 10)), 8u) : 8u;
  }();
  const bool BRCache = Op->Hint == IR::BranchHint::None && !ConstRIP && CallLinkingEnabled && BRCacheSlots != 0;
  std::array<PPC64Emitter::Label, 8> BRSlotMiss {};
  std::array<PPC64Emitter::Label, 8> BRSlotNext {};
  std::array<PendingJumpThunk*, 8> BRSlotThunk {};
  if (BRCache) {
    for (uint32_t i = 0; i < BRCacheSlots; ++i) {
      if (i != 0) {
        Bind(&BRSlotNext[i - 1]);
      }
      PendingJumpThunks.push_back({GetCursorAddress<uint64_t>(), 0 /* indirect */, {}});
      auto* Slot = &PendingJumpThunks.back();
      BRSlotThunk[i] = Slot;
      b(&BRSlotMiss[i]);                // A_i
      lis(TMP2, 0);
      ori(TMP2, TMP2, 0);
      sldi(TMP2, TMP2, 32);
      oris(TMP2, TMP2, 0);
      ori(TMP2, TMP2, 0);
      cmpd(cr(7), RIPReg, TMP2);
      bc({4, 30}, i + 1 < BRCacheSlots ? &BRSlotNext[i] : &InlineCacheProbe);
      std(RIPReg, rip_off, STATE);
      EmitExitR0Zero(UnitR0Dirty);
      Slot->FinalAddress = GetCursorAddress<uint64_t>();
      Slot->FinalPlainBranch = true;
      Emit32(0x7FE00008u);              // F_i: trap until linked
    }
    Bind(&InlineCacheProbe);
    for (uint32_t i = 0; i < BRCacheSlots; ++i) {
      BRSlotThunk[i]->LinkedEntryAddress = GetCursorAddress<uint64_t>();
    }
  }

  // Shadow CALL push. For a Linkable exit this `bcl` IS the patch site A:
  // the registration above deliberately emits nothing after itself in that
  // case (SinkAfterShadowPush defers the sunk region to just below), so A is
  // the first and only word the linker rewrites. For an indirect (inline
  // cache) exit it is simply the pre-probe push.
  std::pair<uint64_t, uint32_t*> ShadowPush1 {};
  if (ShadowCall) {
    ShadowPush1 = EmitShadowCallPush();
    // Sunk region for a linked constant shadow CALL. Placed here rather than
    // at the patch site because A must stay the push's first word; the push
    // clobbers only TMP2/TMP3/TMP4 and reads GetReg(Op->CallReturnAddress),
    // so RIPReg (TMP1) is untouched and materialising it now is equivalent to
    // materialising it before the push. On the LINKED path A is `b Linked
    // Entry` and everything here is skipped -- sound because that leg pushes
    // GetReg(Op->CallReturnAddress) (a separate live register) and its own
    // computed trampoline address, never RIPReg, and then `bl HostCode`; the
    // destination RIP is a patched immediate there. On the unlinked path the
    // L1 probe just below consumes RIPReg exactly as before.
    if (SinkAfterShadowPush) {
      EmitConstRIPIntoTMP1();
      std(RIPReg, rip_off, STATE);
    }
  }

  // ---------------------------------------------------------------------
  // L1 probe. Mirrors the dispatcher's arithmetic exactly (PPC64Dispatcher.cpp
  // DispatcherLoopTop): L1Mask is pre-scaled by sizeof(LookupCacheEntry)=16,
  // so the index is (RIP << 4) & L1Mask.
  // ---------------------------------------------------------------------
  const int32_t l1_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, State.L1Pointer));
  const int32_t l1mask_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, State.L1Mask));

  // A shadow RET that found an empty stack or a mismatched top re-enters the
  // normal lookup here, so the fast path degrades to exactly the L1-probe
  // behaviour on any miss.
  if (ShadowActive && Op->Hint == IR::BranchHint::Return) {
    Bind(&ShadowRetReprobe);
  }

  // LINK-FIRST: an unlinked constant exit does not probe (see LinkFirst above).
  if (!LinkFirst) {
    ld(TMP2, l1_off, STATE);       // TMP2 = L1Pointer
    if (!FEXCore::Config::Get_DYNAMICL1CACHE()) {
      // Static L1: constant-mask probe, one rldic instead of L1Mask load +
      // sldi + and_. Same derivation as the dispatcher's DispatcherLoopTop.
      static_assert((FEXCore::LookupCache::MAX_L1_ENTRIES & (FEXCore::LookupCache::MAX_L1_ENTRIES - 1)) == 0,
                    "rldic probe requires a power-of-two L1");
      constexpr uint32_t L1MB = 64 - (std::countr_zero(FEXCore::LookupCache::MAX_L1_ENTRIES) + 4);
      rldic(TMP4, RIPReg, 4, L1MB);
    } else {
      ld(TMP3, l1mask_off, STATE);   // TMP3 = L1Mask (pre-scaled)
      sldi(TMP4, RIPReg, 4);         // log2(sizeof(LookupCacheEntry)) == 4
      and_(TMP4, TMP4, TMP3);
    }
    add(TMP2, TMP2, TMP4);         // TMP2 = &L1[hash]

    ld(TMP4, 8, TMP2);             // TMP4 = GuestCode (the "key"), loaded FIRST
    cmpd(cr(7), TMP4, RIPReg);
    // BO=4 (branch if false), BI=30 (CR7.EQ at PPC bit 4*7+2). i.e. bne cr7.
    bc({4, 30}, &MissLabel);

    // Hit. Carry the GuestCode value into the HostCode load's address so the
    // hardware cannot hoist it above the GuestCode load and observe
    // {stale HostCode, new GuestCode} mid-Publish. TMP3 is 0 by construction.
    // Same one-instruction fold as the dispatcher's match_label leg: the data
    // dependency rides ldx's index operand (TMP3 == 0), preserving the
    // load-load ordering the comment above requires.
    xor_(TMP3, TMP4, TMP4);
    ldx(TMP3, TMP2, TMP3);         // TMP3 = HostCode (loaded under address-dep)
    mtctr(TMP3);
    if (!Linkable) {
      // P5.0.1: store the destination RIP into State.rip on the hit leg too.
      // Rationale: RestoreRIPFromHostPC's fallback (Frame->State.rip) is invoked
      // whenever a JIT block has no per-instruction RIP entries or the host PC
      // sits outside a header'd block; without this store the fallback returns
      // whatever the last L1 *miss* stored, which can be arbitrarily stale.
      // Symptom (silent): guest signal frames carry wrong-but-plausible RIPs and
      // sigreturn resumes at the wrong address. 1 instruction on a 14-instruction
      // leg. Reviewed against Power ISA v3.0B — safe placement here (RIPReg is
      // still live; no dependency on TMP1-TMP4 that could be misordered).
      std(RIPReg, rip_off, STATE);
      // P5.0.2: reset r0 to 0 before the bctr. JIT blocks emit X-form indexed
      // memory ops with r0 in the rB slot (ldx/stdx and friends), which read r0
      // as its actual value (not literal zero — the "r0 reads as zero" rule
      // applies only to rA). A nonzero r0 silently offsets every load/store in
      // the target block. Every mflr(r0) in the backend today is paired with a
      // restore, so no bug is visible — but the invariant is currently globally
      // assumed, and the failure mode is silent guest memory corruption. Make
      // the local guarantee explicit for 1 extra instruction on this hot leg.
      //
      // ...and it is no longer merely assumed: EmitExitR0Zero drops the
      // instruction entirely in compile units that provably never clobber r0.
      // See the policy comment at the top of this file.
      EmitExitR0Zero(UnitR0Dirty);
    }
    // Linkable exits emit neither of the above on this leg. The r0 re-zero was
    // hoisted ABOVE the patch site (a linked branch skips everything after it,
    // and P5.0.2's failure mode is silent guest memory corruption); the rip
    // store was SUNK below the patch site — still before this hit leg, so this
    // leg's State.rip is exactly what the hoisted form left. See the sink
    // comment up top.
    if (ShadowCall) {
      if (NoLinkStackPair) {
        bctr();
      } else {
        bctrl();                          // LK=1: link stack <- &Tramp1
      }
      PatchShadowCallAddi(ShadowPush1, GetCursorAddress<uint64_t>());
      EmitShadowCallTrampoline();         // Tramp1
    } else {
      bctr();
    }
  }

  // ---------------------------------------------------------------------
  // Miss. This is where the spill lives now.
  //
  // It is RELOCATED, not deleted. SignalDelegator's SIGNAL_FOR_PAUSE (and
  // Stop) handling gates only on IsAddressInCodeBuffer, which excludes the
  // dispatcher's separate mmap — so a dispatcher PC always takes the
  // "non-jit, SRA is already spilled" branch and reads guest state that only
  // a completed spill makes valid. (The LOGMAN_THROW_A_FMT that would catch a
  // dispatcher PC there is inside #if ASSERTIONS_ENABLED and inert in
  // Release, and the async-signal deferral does not cover it because
  // PauseHandler is a host handler.) Spilling here, inside the code buffer
  // and before the branch out, keeps IsAddressInCodeBuffer a valid proxy and
  // needs no signal-delegator change.
  //
  // Extending the delegator to IsAddressInDispatcher instead would be
  // actively unsafe: ExitFunctionLinker continues past a bctrl, after which
  // r7-r12 (SRA-mapped, ELFv2-volatile) hold garbage.
  //
  // Correspondingly, Pointers.ExitFunctionLinker no longer spills; the
  // dispatcher's own L1 miss branches to a second, SRA-spilling entry.
  // ---------------------------------------------------------------------
  Bind(&MissLabel);
  if (Linkable) {
    // Linkable miss leg: State.rip was already stored by the sunk region
    // (just below the patch site for a plain jump, just past the pre-probe
    // push for a shadow CALL) — or, under FEX_NOSINKEXITRIP, by the hoisted
    // region above the patch site. Either way it is stored before this
    // branch, and the record linker below genuinely reads it.
    // Branch to this exit's jump thunk LinkPath (emitted at the tail of
    // CompileCode), which PC-discovers the adjacent PPC64BlockLinkRecord into
    // TMP2 and tail-branches to the shared SpillStaticRegs stub
    // (SharedSpillLinkLabel — SpillStaticRegs preserves TMP2 through f0, so
    // &record survives it), which then enters the dispatcher's
    // ExitFunctionLinkerWithRecord stub with r4 = &record and SRA spilled.
    // That path compiles/looks up the target AND backpatches the probe above;
    // it dispatches exactly like ExitFunctionLinker otherwise (deferred-signal
    // guard, FillStaticRegs, bctr).
    b(LinkPathLabel);
  } else if (InlineCache) {
    // Indirect shadow call: the record linker reads the target from
    // State.rip (record.GuestRIP is 0) and fills the guard above.
    std(RIPReg, rip_off, STATE);
    b(LinkPathLabel);
  } else {
    std(RIPReg, rip_off, STATE); // BEFORE the shared stub's spill clobbers TMP1-TMP4
    // Shared per-compile-unit spill stub (CompileCode tail): SpillStaticRegs +
    // dispatch to Pointers.ExitFunctionLinker. Replaces ~90 inline cold
    // instructions per exit with this one branch; see SharedSpillExitLabel.
    SharedSpillExitUsed = true;
    b(&SharedSpillExitLabel);
  }
  if (BRCache) {
    // An empty slot is filled by a sampled arrival, not the first one: only
    // when the time base's low 6 bits are zero (about 1 in 64 arrivals) does
    // the miss leg go to the linker; the rest take the probe. A slot then
    // holds a target in proportion to how often it arrives, instead of
    // whichever targets a start-up path happened to dispatch first (vm filled
    // all eight slots with its setup opcodes that way), and a cold site pays
    // the C++ linker on ~2% of its executions rather than on each of its
    // first few. mftb is a user-readable SPR; no CR0 or XER bits are touched.
    for (uint32_t i = 0; i < BRCacheSlots; ++i) {
      Bind(&BRSlotMiss[i]);
      mftb(TMP2);
      rldicl(TMP2, TMP2, 0, 58);
      cmpldi(cr(7), TMP2, 0);
      bc({4, 30}, &InlineCacheProbe);   // bne cr7: not sampled, probe
      std(RIPReg, rip_off, STATE);      // the record linker reads the target here
      b(&BRSlotThunk[i]->LinkPath);
    }
  }

  // Linked shadow call: its own push (return word = Tramp2), the Final word
  // the linker rewrites to `bl`, and Tramp2. Reachable only once the linker
  // patches A to `b LinkedEntry`; see the link-stack pairing comment above.
  if (ShadowCall && Linkable) {
    auto& Thunk = PendingJumpThunks.back();
    Thunk.LinkedEntryAddress = GetCursorAddress<uint64_t>();
    auto Push2 = EmitShadowCallPush();
    Thunk.FinalAddress = GetCursorAddress<uint64_t>();
    Emit32(0x7FE00008u);                // Final: trap until linked
    PatchShadowCallAddi(Push2, GetCursorAddress<uint64_t>());
    EmitShadowCallTrampoline();         // Tramp2
  }
}

DEF_OP(Jump) {
  auto Op = IROp->C<IR::IROp_Jump>();
  auto Target = JumpTarget(Op->TargetBlock);
  // A bound label means the target block was already emitted, i.e. this is a
  // backward edge -- a potential guest loop that must pass a deferred-signal
  // drain point (see EmitSuspendInterruptCheck).
  if (Target->bound) {
    EmitSuspendInterruptCheck();
  }
  // Spin-loop SMT priority hint for this edge, if AnalyzeSpinLoops marked it.
  EmitSpinEdgeHint(Op->TargetBlock);
  // Fallthrough elision: the target is the next emitted block (necessarily
  // forward/unbound, so the suspend poke above did not run and must not).
  // See FallthroughBlockID in JITClass.h; EndBlock after this op is a no-op.
  if (IR->GetOp<IR::IROp_CodeBlock>(Op->TargetBlock)->ID == FallthroughBlockID) {
    return;
  }
  // A forward jump to the next emitted block falls into it (NextBlockID is
  // never an EntryPoint block, whose prologue must not run on this edge).
  // POWERARM_NOSHORTCOND=1 keeps the `b`.
  static const bool NoShortCond = getenv("POWERARM_NOSHORTCOND") != nullptr;
  if (!NoShortCond && !Target->bound && SpinBackedges.empty() && SpinRestoreEdges.empty() &&
      IR->GetOp<IR::IROp_CodeBlock>(Op->TargetBlock)->ID == NextBlockID) {
    return;
  }
  b(Target);
}

DEF_OP(CondJump) {
  auto Op = IROp->C<IR::IROp_CondJump>();

  // PPC `bc` has a signed 14-bit displacement (±32KB). Block-to-block jumps
  // can easily exceed that, so emit the canonical long-conditional idiom:
  // `bc !cond, skip; b TrueBlock; skip: b FalseBlock`. Both `b` insns have
  // a 24-bit displacement (±32MB), well past any single-function code size.
  Cond CC;
  if (Op->VCmpElementSize != IR::OpSize::iInvalid) {
    // Vector-compare branch (glibc vector-scan fusion; see
    // docs/VCMPEQ_FUSION_DESIGN.md and HostFeatures::SupportsVCmpFlagBranch).
    // Cmp1/Cmp2 are FPR-class here, NOT GPRs.
    //
    // The record form of the VMX integer compares writes CR field 6:
    //   CR6 bit 0 (CR bit 24) = every lane compared equal
    //   CR6 bit 2 (CR bit 26) = NO lane compared equal
    // so a single `bc` on CR bit 26 answers "did any lane match" with the lane
    // mask never leaving the vector unit. VTMP1 absorbs the (unused) VRT.
    //
    //   Cond == NEQ -> TrueBlock when ANY lane matched  -> CR6[2] clear -> BO=4
    //   Cond == EQ  -> TrueBlock when NO  lane matched  -> CR6[2] set   -> BO=12
    const auto V1 = GetVReg(Op->Cmp1);
    const auto V2 = GetVReg(Op->Cmp2);
    switch (Op->VCmpElementSize) {
    case IR::OpSize::i8Bit: vcmpequb_(VTMP1, V1, V2); break;
    case IR::OpSize::i16Bit: vcmpequh_(VTMP1, V1, V2); break;
    case IR::OpSize::i32Bit: vcmpequw_(VTMP1, V1, V2); break;
    case IR::OpSize::i64Bit: vcmpequd_(VTMP1, V1, V2); break;
    default: LOGMAN_MSG_A_FMT("CondJump: unhandled VCmpElementSize {}", static_cast<uint32_t>(Op->VCmpElementSize)); break;
    }
    LOGMAN_THROW_A_FMT(Op->Cond == IR::CondClass::NEQ || Op->Cond == IR::CondClass::EQ,
                       "CondJump vector-compare mode only encodes EQ/NEQ over 'any lane matched'");
    // BI 26 = CR6's "none matched" bit (CR field 6 occupies CR bits 24..27).
    CC = (Op->Cond == IR::CondClass::NEQ) ? Cond {4, 26} : Cond {12, 26};
  } else if (Op->FromNZCV) {
    CC = MapNZCVCC(Op->Cond);
  } else if (Op->Cond == IR::CondClass::TSTZ || Op->Cond == IR::CondClass::TSTNZ) {
    // Bit-test branch: Cmp2 is an inline constant giving the bit POSITION
    // (0..63), not a mask. TSTZ jumps if bit clear, TSTNZ if bit set.
    // Extract the bit and compare against zero via cr7, so we don't disturb
    // CR0 — downstream IR ops (e.g. a CondJumpNZCV in the very next block
    // produced by x86 `jp; je`) consume CR0 as the AXFlag/NZCV side-channel,
    // and clobbering CR0 here would silently corrupt them.
    //
    // (This is the same invariant honored by DEF_OP(Parity), which uses
    //  rldicl (no Rc) rather than andi. for exactly this reason — see
    //  ALUOps.cpp::Parity.)
    uint64_t Bit;
    LOGMAN_THROW_A_FMT(IsInlineConstant(Op->Cmp2, &Bit) && Bit < 64,
                       "CondJump TSTZ/TSTNZ: expected inline-constant bit < 64");
    auto Reg = GetReg(Op->Cmp1);
    uint32_t sh = (64u - static_cast<uint32_t>(Bit)) & 63u;
    rldicl(TMP1, Reg, sh, 63);          // TMP1 = (Reg >> Bit) & 1 (no Rc, CR untouched)
    cmpldi(cr(7), TMP1, 0);             // cr7 = (TMP1 == 0)
    // bc BI = cr7*4 + EQ_bit(2) = 30. BO=12 → take when EQ set; BO=4 → when clear.
    CC = (Op->Cond == IR::CondClass::TSTNZ) ? Cond{4, 30} : Cond{12, 30};
  } else if (IsSpinCollapseBranch(Node)) {
    // FEX_SPINCOLLAPSE (contract at kSpinCollapseK, JITClass.h): the spin
    // backedge of a matched counted-decrement pair whose Sub now retires K
    // budget per iteration. Exit exactly when the batched decrement lands on
    // 0, i.e. keep spinning iff old > K. Compare signedness follows the
    // matched idiom: NEQ-1 (`dec; jne`) budgets are canonicalized
    // zero-extended values — unsigned cmpldi, worst case is exiting early;
    // SGT-0 (`test; jg` — CP2077's redDispatcher worker loop) must use the
    // SIGNED cmpdi so a negative value exits like the original branch would,
    // where `>u K` would read it as huge and spin forever. cr7, no Rc.
    // ★ The compare WIDTH must follow CompareSize, exactly as the generic
    // EmitCompare path does. The SGT-0 idiom stages its pre-decrement value
    // through a 32-bit ZERO-EXTEND (Bfe #32,#0 — `mov eax,ecx`), so a
    // negative budget sits in the register as 0x00000000_FFFFFFFF: a 64-bit
    // cmpdi reads that as +4294967295 > K and spins FOREVER, while the guest's
    // own `jg` would have exited. cmpwi compares the low 32 bits signed and
    // exits, matching the guest.
    const bool Is32 = Op->CompareSize <= IR::OpSize::i32Bit;
    if (IsSpinCollapseBranchSigned(Node)) {
      if (Is32) {
        cmpwi(cr(7), GetReg(Op->Cmp1), static_cast<int16_t>(kSpinCollapseK));
      } else {
        cmpdi(cr(7), GetReg(Op->Cmp1), static_cast<int16_t>(kSpinCollapseK));
      }
    } else {
      if (Is32) {
        cmplwi(cr(7), GetReg(Op->Cmp1), kSpinCollapseK);
      } else {
        cmpldi(cr(7), GetReg(Op->Cmp1), kSpinCollapseK);
      }
    }
    // BO=12 (branch if set), BI=29 (cr7.GT): TrueBlock (the backedge) taken
    // while old > K.
    CC = {12, 29};
  } else {
    // Route the compare through cr(7) so we don't disturb CR0 / XER.
    // CR0 carries packed-NZCV N/Z bits filled by FillStaticRegs at block
    // entry (and consumed by downstream FromNZCV CondJump / NZCVSelect
    // ops). Under CONFIG_SMC_FULL the SMC IR pass emits a non-FromNZCV
    // CondJump (on the ValidateCode result) right before the actual x86
    // instruction body — clobbering CR0 here cascades into wrong-direction
    // x86 conditional jumps. (See SelfModifyingCode/Delinking under
    // SMC_FULL: cmp/jg + cmp/je fall the wrong way because CR0 held the
    // validate compare's eq/lt outcome rather than the guest cmp result.)
    EmitCompare(Op->Cond, Op->CompareSize, Op->Cmp1, Op->Cmp2, /*CRField=*/7);
    CC = MapCC(Op->Cond);
    // MapCC returns a Cond with BI numbered against CR0 (BI in 0..3).
    // Shift to the equivalent CR7 bit positions (BI in 28..31).
    CC = {CC.BO, static_cast<uint8_t>(CC.BI + 28)};
  }
  const uint32_t TrueID = IR->GetOp<IR::IROp_CodeBlock>(Op->TrueBlock)->ID;
  const uint32_t FalseID = IR->GetOp<IR::IROp_CodeBlock>(Op->FalseBlock)->ID;

  // P6 (POWER9 pipeline research Rule 6): a conditional guest branch between
  // two block exits. The frontend emits it as this CondJump followed by the
  // taken block and then the not-taken block, each holding only a constant
  // exit, which once linked is a single `b`. The generic shape below reaches
  // them through `bc; b; b` hops: two taken branches on the true path and
  // three on the false one. Branch straight to the false exit instead and
  // fall into the true one: one taken branch on the true path (the linked
  // exit), two on the false path. The bc's reach is the true block alone (a
  // constant exit, at most ~30 instructions). POWERARM_NOEXITSHAPE=1 disables.
  static const bool NoExitShape = getenv("POWERARM_NOEXITSHAPE") != nullptr;
  if (!NoExitShape && TrueID != FalseID && TrueID == NextBlockID && FalseID == NextNextBlockID && TrueID < ConstExitOnlyBlock.size() &&
      FalseID < ConstExitOnlyBlock.size() && ConstExitOnlyBlock[TrueID] && ConstExitOnlyBlock[FalseID] && SpinBackedges.empty() &&
      SpinRestoreEdges.empty()) {
    bc(InvertCond(CC), JumpTarget(Op->FalseBlock));
    return;
  }

  // Branch shape between two blocks of the unit (P6 leftovers). The generic
  // shape below is `bc !cc, skip; b True; skip: b False`: two taken branches
  // on the false path even when a leg is the next block. When the next block
  // is a leg, fall into it and take one `bc` to the other; otherwise
  // `bc cc, True; b False`, one taken branch either way. Forward legs only
  // take the short `bc` (ShortCondLabel, islands keep them in reach); a
  // backward leg keeps its suspend poke and long `b`.
  // POWERARM_NOSHORTCOND=1 restores the generic shape.
  static const bool NoShortCond = getenv("POWERARM_NOSHORTCOND") != nullptr;
  if (!NoShortCond && TrueID != FalseID && SpinBackedges.empty() && SpinRestoreEdges.empty()) {
    auto* TrueTarget = JumpTarget(Op->TrueBlock);
    auto* FalseTarget = JumpTarget(Op->FalseBlock);
    if (!TrueTarget->bound && !FalseTarget->bound) {
      if (TrueID == NextBlockID) {
        bc(InvertCond(CC), ShortCondLabel(FalseID));
      } else if (FalseID == NextBlockID) {
        bc(CC, ShortCondLabel(TrueID));
      } else {
        bc(CC, ShortCondLabel(TrueID));
        b(FalseTarget);
      }
      return;
    }
    if (TrueTarget->bound != FalseTarget->bound) {
      const bool TrueBackward = TrueTarget->bound;
      const uint32_t ForwardID = TrueBackward ? FalseID : TrueID;
      const Cond ToForward = TrueBackward ? InvertCond(CC) : CC;
      PPC64Emitter::Label Skip {};
      bc(ToForward, ForwardID == NextBlockID ? &Skip : ShortCondLabel(ForwardID));
      EmitEdgeSuspendInterruptCheck(TrueBackward ? Op->TrueBlock : Op->FalseBlock);
      b(TrueBackward ? TrueTarget : FalseTarget);
      Bind(&Skip);
      return;
    }
  }

  // Fallthrough elision (see FallthroughBlockID in JITClass.h). A fallthrough
  // target is by construction the next emitted block: forward, unbound, so
  // the backward-edge suspend poke never applies to an elided leg.
  //
  // TrueBlock is the fallthrough: flip the legs. Take the bc on CC (not the
  // inversion) over the false leg, and fall into TrueBlock. The false leg
  // keeps its own poke/hint discipline. (TrueID == FalseID degenerates to
  // the generic shape below; both cannot be elided.)
  if (TrueID == FallthroughBlockID && FalseID != TrueID) {
    Label TakeFall;
    bc(CC, &TakeFall);
    auto FalseTarget = JumpTarget(Op->FalseBlock);
    if (FalseTarget->bound) {
      EmitEdgeSuspendInterruptCheck(Op->FalseBlock);
    }
    EmitSpinEdgeHint(Op->FalseBlock);
    b(FalseTarget);
    Bind(&TakeFall);
    // Hint after the bind so it executes exactly when the true edge is taken.
    EmitSpinEdgeHint(Op->TrueBlock);
    return;
  }

  Label Skip;
  bc(InvertCond(CC), &Skip);
  // Backward edges (bound labels) must pass a deferred-signal drain point;
  // see DEF_OP(Jump). Each leg pokes independently so the forward leg stays
  // poke-free.
  auto TrueTarget = JumpTarget(Op->TrueBlock);
  if (TrueTarget->bound) {
    EmitEdgeSuspendInterruptCheck(Op->TrueBlock);
  }
  // Spin-loop SMT priority hints, per edge (see AnalyzeSpinLoops). Emitted
  // inside each leg so the hint executes exactly when that edge is taken.
  EmitSpinEdgeHint(Op->TrueBlock);
  b(TrueTarget);
  Bind(&Skip);
  // FalseBlock is the fallthrough: the bc's skip label lands directly on the
  // next block's body.
  if (FalseID == FallthroughBlockID) {
    EmitSpinEdgeHint(Op->FalseBlock);
    return;
  }
  auto FalseTarget = JumpTarget(Op->FalseBlock);
  if (FalseTarget->bound) {
    EmitEdgeSuspendInterruptCheck(Op->FalseBlock);
  }
  EmitSpinEdgeHint(Op->FalseBlock);
  b(FalseTarget);
}

DEF_OP(Break) {
  auto Op = IROp->C<IR::IROp_Break>();
  ResetStack();

  // Pack the fault data into a single 64-bit value and store it
  FEXCore::Core::CpuStateFrame::SynchronousFaultDataStruct FaultData = {
    .FaultToTopAndGeneratedException = 1,
    .Signal    = Op->Reason.Signal,
    .TrapNo    = Op->Reason.TrapNumber,
    .si_code   = Op->Reason.si_code,
    .err_code  = Op->Reason.ErrorRegister,
  };
  uint64_t Constant = 0;
  memcpy(&Constant, &FaultData, sizeof(FaultData));

  LoadConstant(TMP2, Constant);
  int32_t fault_off = static_cast<int32_t>(
    offsetof(FEXCore::Core::CpuStateFrame, SynchronousFaultData));
  std(TMP2, fault_off, STATE);

  // Spill SRA before calling signal handler
  SpillStaticRegs(TMP1);

  // Jump to the appropriate guest signal handler pointer
  int32_t sig_off;
  switch (Op->Reason.Signal) {
  case FEXCore::Core::FAULT_SIGILL:
    sig_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, Pointers.GuestSignal_SIGILL));
    break;
  case FEXCore::Core::FAULT_SIGTRAP:
    sig_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, Pointers.GuestSignal_SIGTRAP));
    break;
  case FEXCore::Core::FAULT_SIGSEGV:
    sig_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, Pointers.GuestSignal_SIGSEGV));
    break;
  default:
    sig_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, Pointers.GuestSignal_SIGTRAP));
    break;
  }
  ld(TMP1, sig_off, STATE);
  mtctr(TMP1);
  bctr();
}

// kInSyscallSentinel — the value parked in CpuStateFrame::InSyscallInfo for
// the duration of a JIT host-call crossing — lives in ArchHelpers/
// PPC64Emitter.h with its full bit-layout rationale, now that DEF_OP(Thunk)
// and the FABI bridge stubs (PPC64Dispatcher.cpp GenerateABICall) share it
// with this op via ArmInSyscallSentinel/FillForABICallChecked. This op keeps
// its original inline copy of the check because the elision interleaves with
// the RAX result handling below.

DEF_OP(Syscall) {
  auto Op = IROp->C<IR::IROp_Syscall>();

  // Spill SRA to STATE (physical registers retain their values for arg reads below).
  SpillStaticRegs(TMP1);

  // Mark that we are inside the JIT-emitted Syscall op. The signal handler
  // path in HandleDispatcherGuestSignal reads (InSyscallInfo & 0xFFFF) as the
  // SpillSRA IgnoreMask — bits 0..15 each represent one already-spilled SRA
  // GPR. PPC64LE's x64-mode SRA has 16 GPRs, all spilled by the call above,
  // so we set 0xFFFF. Mirrors ARM64 backend at JIT/BranchOps.cpp:277-278.
  // Without this, an async signal arriving between SpillStaticRegs and
  // FillStaticRegs causes the handler to re-spill from post-bctrl volatile
  // registers, overwriting the freshly-stored gregs[RAX] with junk.
  //
  // Bit 24 on top of that mask is a "nobody has touched the guest state
  // behind our back" tripwire, read back by the fill below. See
  // kInSyscallSentinel.
  {
    const int32_t isi_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, InSyscallInfo));
    LoadConstant(TMP1, kInSyscallSentinel);
    std(TMP1, static_cast<int16_t>(isi_off), STATE);
  }

  // Create a mini-frame for the C call.  Layout (16-byte aligned):
  //   [r1+  0]:                back chain (old r1)
  //   [r1+  8..31]:            ELFv2 linkage area (CR/LR/TOC save for HandleSyscall)
  //   [r1+ 32..95]:            ELFv2 parameter save area (8 doublewords, callee-scratch)
  //   [r1+ 96..151]:           SyscallArguments (7 × 8 = 56 bytes)
  //   [r1+152..159]:           padding
  //   [r1+160..]:              volatile dynamic-FPR save area (see below):
  //                            4 × 16 = 64 bytes in x64 mode  -> frame 224
  //                            12 × 16 = 192 bytes in x32 mode -> frame 352
  //
  // SyscallArguments MUST live above the 96-byte ELFv2 linkage+param block:
  // the parameter save area is defined by ELFv2 §2.2.2 as callee-scratch --
  // HandleSyscall or any of its transitive callees can overwrite [r1+32..95]
  // freely, so putting SyscallArguments there and handing HandleSyscall an
  // r5 pointer into it was a data hazard. Move the SSA-source pack to +96
  // and grow the frame to 160B to keep 16B alignment.
  //
  // ---- Volatile dynamic-FPR save area ----------------------------------
  // This op spills STATIC registers only, and on Linux `syscall` is NOT
  // block-end (X86Tables.h:409-414 adds FLAGS_BLOCK_END on _WIN32 only), so
  // JIT-internal vector SSA values are routinely live across the bctrl. The
  // named-vector-constant cache is per-BLOCK and survives FlushRegisterCache
  // (OpcodeDispatcher.h:2288, sole clear at :167 on block reset), and such
  // values are never rematerialised (RegisterAllocationPass.cpp:126-128 --
  // only OP_CONSTANT is). The allocator hands out the lowest free index
  // (:479), i.e. RAFPR[0] = v16 in x64 and v8 in x32, and both are
  // ELFv2-volatile. So the first FPR value allocated in a block was being
  // silently destroyed by any syscall in that block.
  //
  // Guest XMM state was never at risk -- SRAFPR is spilled by
  // SpillStaticRegs. The exposure is JIT-internal temporaries, which is why
  // it presented as rare non-deterministic wrong results rather than as a
  // reproducible failure.
  //
  // Saved into this op's OWN mini-frame rather than via PushDynamicRegs:
  // that helper would pay a second stdu and a second 96-byte linkage
  // reservation on top of the one already reserved here, and would save the
  // non-volatile half of the pool for nothing. The save/restore pair is the
  // FABI-callsite helper (SaveDynVRsToFrame/RestoreDynVRsFromFrame), which
  // honours DynVRSpillMask — CompileCode sets it to this op's RA live-in ∪
  // dest before dispatch, so only vector SSA values actually live across the
  // bctrl are stored (most syscalls touch none). The save area stays sized
  // for the full volatile set; the helper packs saved regs densely from
  // kFPRSaveOff and both loops iterate the same unchanged mask, so they
  // cannot fall out of lockstep.
  //
  // The dynamic GPR pool needs no equivalent: RA is r24-r26/r30-r31 (x64) and
  // r16-r26/r30-r31 (x32), all ELFv2 callee-saved. Asserted in the header.
  //
  // Signal safety is unchanged: these are IR SSA temporaries, not guest
  // state, so SpillSRA never reads them. On an abandon/restart path the
  // restore below simply does not run, exactly as before.
  static_assert(FEXCore::HLE::SyscallArguments::MAX_ARGS == 7);

  constexpr int kFPRSaveOff = 160;
  const auto RAFPRVolatile = std::span<const VR>(a64::RAFPRVolatile);
  const int16_t FrameSize =
    static_cast<int16_t>(kFPRSaveOff + RAFPRVolatile.size() * 16);

  stdu(r1, static_cast<int16_t>(-FrameSize), r1);

  // Save the live volatile dynamic FPRs (DynVRSpillMask-selected; clobbers
  // TMP3, which nothing here holds live yet). r1 is 16-byte aligned per ELFv2
  // and every offset is a multiple of 16, so stvx's address masking is a
  // no-op.
  SaveDynVRsToFrame(kFPRSaveOff);

  // Fill SyscallArguments from the IR op's source nodes.
  // After SpillStaticRegs the physical SRA registers still hold the live values.
  for (uint32_t i = 0; i < FEXCore::HLE::SyscallArguments::MAX_ARGS; ++i) {
    if (Op->Header.Args[i].IsInvalid()) continue;
    const int16_t slot_off = static_cast<int16_t>(96 + i * 8);
    uint64_t Const;
    if (IsInlineConstant(Op->Header.Args[i], &Const)) {
      LoadConstant(TMP1, Const);
      std(TMP1, slot_off, r1);
    } else {
      std(GetReg(Op->Header.Args[i]), slot_off, r1);
    }
  }

  // Call: SyscallHandler::HandleSyscall(this, Frame*, SyscallArguments*)
  //   r3 = SyscallHandlerObj (this)
  //   r4 = Frame* (CpuStateFrame*)
  //   r5 = SyscallArguments* (at [r1+96])
  //   r12 = callee address (ELFv2 indirect-call requirement)
  {
    const int32_t obj_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, Pointers.SyscallHandlerObj));
    const int32_t fn_off  = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, Pointers.SyscallHandlerFunc));

    ld(r3,    obj_off, STATE);   // r3  = this
    ld(r(12), fn_off,  STATE);   // r12 = fn ptr
    mr(r4, STATE);               // r4  = Frame
    addi(r5, r1, 96);            // r5  = &SyscallArguments
  }

  std(r2, 24, r1);     // save TOC (ELFv2 linkage area, unchanged offset)
  mtctr(r(12));
  bctrl();
  ld(r2, 24, r1);      // restore TOC

  // HandleSyscall returns the new guest RAX value in r3.
  //
  // ...but ONLY for the Linux ABIs. OS_GENERIC ("no JIT-side argument
  // handling, spill/fill all regs") is the ABI a Wine CPU-DLL-shaped embedder
  // selects, and there the handler owns the whole register file: it writes
  // gregs[] in the frame directly and its C return value is meaningless.
  // Upstream FEX expresses this as IR SyscallFlags::NORETURNEDRESULT, which
  // this fork's Syscall IR op does not carry; the OSABI is fixed for the
  // process, so testing it here is equivalent and needs no IR change.
  //
  // Measured before the fix: an OS_GENERIC handler that set gregs[RAX] had its
  // value overwritten by this store on the way out (probe T2c).
  const bool GenericABI = CTX->SyscallHandler && CTX->SyscallHandler->GetOSABI() == FEXCore::HLE::SyscallOSABI::OS_GENERIC;

  if (!GenericABI) {
    const int32_t rax_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame,
               State.x[0]));
    std(r3, rax_off, STATE);
  }

  // Mirror ARM64 (JIT/BranchOps.cpp:319-323): write the syscall result to
  // the IR destination SSA reg so consumers reading the edge directly (not
  // via _LoadRegister(RAX)) get the right value. Must happen BEFORE
  // FillStaticRegs because that helper uses TMP1=r3 as scratch and will
  // clobber the return value. GetReg(Node) is in the dynamic RA pool
  // (r24-r26, r30-r31), all callee-saved per ELFv2, so it survives
  // everything that follows.
  mr(GetReg(Node), r3);

  // Restore the saved dynamic FPRs under the same (unchanged) DynVRSpillMask.
  // Clobbers TMP3 (= r5, dead since the bctrl); kept after both consumers of
  // r3 above anyway so the ordering stays obviously safe.
  RestoreDynVRsFromFrame(kFPRSaveOff);

  // Free the mini-frame, then reload SRA from STATE (picks up the RAX result).
  addi(r1, r1, FrameSize);

  // ---- Fill elision -----------------------------------------------------
  // SRA slots 6..15 map to r14..r23, which ELFv2 preserves across the bctrl
  // above. Nothing between SpillStaticRegs and here touches them: the arg
  // pack and the result move use the dynamic RA pool (r24-r26/r30-r31 in
  // x64), the call sequence uses r3/r4/r5/r12, and TMP1..TMP4 are r3-r6. So
  // in the common case those ten host registers still hold the live guest
  // values and reloading them from the frame is pure overhead.
  //
  // The uncommon case is a signal: HandleDispatcherGuestSignal / the guest
  // handler / RestoreThreadState can rewrite ANY greg in the frame while the
  // host registers keep their pre-signal contents, so those ten loads are
  // mandatory there. kInSyscallSentinel's bit 24 detects exactly that — it is
  // erased by the uint16_t ContextBackup round trip, so "some bit above 15 is
  // still set" is a sound proof that no signal republished the frame.
  //
  // NOT applied to i686 guests: their fill uses lwz for its zero-extension
  // side effect, which a surviving host register does not provide.
  //
  // NOTE this only elides the *fill*. The spill must stay complete: syscalls
  // that snapshot guest state read it straight out of the frame — e.g.
  // Thread.cpp:103 `TM.CreateThread(0, 0, &Frame->State, ...)` hands the
  // parent's whole CPUState to a new guest thread — and a partial spill would
  // hand them a stale RSI/RDI/R8-R15.
  //
  // NOT applied to OS_GENERIC either, and this one is a correctness bound, not
  // a tuning choice. The sentinel proves "no *signal* republished the frame".
  // It says nothing about the handler itself having rewritten gregs[], which
  // for OS_GENERIC is the handler's whole job: BTCpuSetContext, NtContinue,
  // KiUserExceptionDispatcher and APC delivery all resume the guest with a
  // register file the host just wrote. Measured before this fix (probe T2c):
  // of 15 handler-written GPRs, only the five in ELFv2-volatile SRA slots
  // (RAX/RCX/RDX/RBX/RBP) reached the guest; RSI, RDI and R8-R15 were silently
  // dropped. Linux guests are unaffected — their handlers never write gregs[]
  // except RAX, which is what the elision was designed around.
  if (!GenericABI) {
    const int32_t isi_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, InSyscallInfo));
    PPC64Emitter::Label SentinelIntact;
    ld(TMP1, isi_off, STATE);
    // TMP1 = InSyscallInfo >> 16, recording into CR0: EQ iff nothing above
    // bit 15 survived, i.e. iff the frame was republished behind us.
    rldicl_(TMP1, TMP1, 48, 16);
    // BO=4 (branch if false), BI=2 (CR0.EQ) — i.e. bne cr0.
    bc({4, 2}, &SentinelIntact);
    FillStaticRegs(FillMode::NonVolatileGPRsOnly);
    Bind(&SentinelIntact);
    FillStaticRegs(FillMode::SkipNonVolatileGPRs);
  } else {
    FillStaticRegs();
  }
  // HandleSyscall is a host C function; r0 was clobbered. Restore the JIT's
  // r0=0 zero-index invariant before falling back into JIT code that uses
  // ldx/stdx.
  li(r(0), 0);

  // Clear InSyscallInfo. From here onward the JIT-emitted Syscall op is
  // done; any signal arriving treats this code as normal JIT and the full
  // SRA spill path is correct again.
  {
    const int32_t isi_off = static_cast<int32_t>(
      offsetof(FEXCore::Core::CpuStateFrame, InSyscallInfo));
    li(TMP1, 0);
    std(TMP1, static_cast<int16_t>(isi_off), STATE);
  }
}

DEF_OP(Thunk) {
  // ELFv2 ABI: r3 = ArgPtr (void* to guest argument data), thunk function
  // pointer in TMP2=r4. ArgPtr comes from an SRA register (guest GPR) so it
  // survives SpillStaticRegs (which only COPIES SRA regs to ctx; the physical
  // registers retain their values until PushDynamicRegs overwrites them).
  auto Op = IROp->C<IR::IROp_Thunk>();

  // Sentinel-guarded partial SRA GPR refill, ported from DEF_OP(Syscall)'s
  // fill elision above: the ELFv2 callee cannot touch r14-r23, so the ten
  // non-volatile SRA GPR reloads after the bctrl are pure overhead unless
  // something republished the frame during the call. The sentinel proves the
  // negative: a guest-signal delivery truncates it through the uint16_t
  // ContextBackup stash on either HandleDispatcherGuestSignal branch, and a
  // host->guest callback (thunk callees DO re-enter the guest — X11 event
  // handlers etc.) clears it at the dispatcher's CallbackPtr entry before any
  // callback guest block runs. Both the arm and the checked fill live on
  // PPC64EmitterBase (shared with the FABI bridge stubs); see
  // kInSyscallSentinel in ArchHelpers/PPC64Emitter.h for the full contract.
  // 64-bit guests only — the 32-bit lwz zero-extension invariant forbids
  // partial fills (PPC64Emitter.cpp FillStaticRegs) — and 32-bit keeps
  // today's unarmed full-fill path byte for byte. XMM/VR fills are NEVER
  // elidable: v0-v15 are ELFv2-volatile and the frame copy is also what
  // signal delivery reads mid-call.
  static const bool NoPartialFill = getenv("FEX_NO_THUNK_PARTIAL_FILL") != nullptr;
  const bool PartialFill = !NoPartialFill;

  SpillForABICall(TMP1);
  if (PartialFill) {
    // Strictly after the spill completes (the mask claims "already
    // spilled"), strictly before the callee can run. Clobbers TMP1 only —
    // the ArgPtr/relocation setup below uses r3/TMP2 after this point.
    ArmInSyscallSentinel();
  }

  // Set up the single argument: ArgPtr in r3
  mr(r3, GetReg(Op->ArgPtr));

  // Load thunk function address into TMP2; record relocation for cache patching
  InsertNamedThunkRelocation(TMP2, Op->ThunkNameHash);

  // Call: set r12 = callee GEP per ELFv2, then branch via CTR.
  //
  // The thunk callee lives in a *different* DSO (libGL-host.so etc), so it is
  // entered at its global entry point, overwrites r2 with its own TOC, and
  // returns with r2 changed. ELFv2 (OpenPOWER 64-bit ELF V2 ABI 2.2.1.1)
  // makes the *caller* responsible for preserving r2 across a call through a
  // function pointer. Same idiom as DEF_OP(Syscall) above. r1 has already
  // been lowered by SpillForABICall -> PushDynamicRegs, and kDynGPRStart ==
  // kDynLinkArea == 96, so [r1+24] is this frame's own (unused) linkage-area
  // TOC doubleword.
  mr(r(12), TMP2);
  std(r2, 24, r1);     // save TOC (ELFv2 linkage area, unchanged offset)
  mtctr(TMP2);
  // Second C argument: the CpuStateFrame. A Linux-thunk callee is
  // void(void*) and ignores r4, which ELFv2 makes harmless.  TMP2 (== r4) is dead here: the callee address is already in
  // CTR and its GEP copy in r12.
  mr(TMP2, STATE);
  bctrl();
  ld(r2, 24, r1);      // restore TOC

  if (PartialFill) {
    FillForABICallChecked();
  } else {
    FillForABICall();
  }
}

} // namespace FEXCore::CPU
