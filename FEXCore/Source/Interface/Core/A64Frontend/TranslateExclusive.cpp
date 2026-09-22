// SPDX-License-Identifier: MIT
//
// A64 exclusive loads and stores (LDXR/LDAXR, STXR/STLXR) through the
// software exclusive monitor in CPUState (DESIGN.md §4.3), and the plain
// atomic-width loads and stores LDAR/LDLAR/STLR/STLLR.
//
// LDXR records the address, size and loaded value and marks the monitor
// valid. STXR succeeds when the monitor is valid for the same address and
// size and the memory still holds the recorded value; the store itself is a
// compare-and-swap against that value, so a concurrent writer that changed
// the value makes it fail. A writer that restores the same value between the
// two is not detected (the ABA case the design accepts for the software
// path). Either way the monitor is cleared.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

#include <FEXCore/Core/CoreState.h>

namespace FEXCore::A64 {
using namespace FEXCore::IR;

bool IRBuilder::LoadExclusive(uint32_t Word) {
  const uint32_t Size = Bits(Word, 31, 30);
  const auto MemSize = IR::SizeToOpSize(1U << Size);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  Ref Address = LoadXSP(Rn);
  Ref Value = _LoadMem(RegClass::GPR, MemSize, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
  _StoreContext(OpSize::i64Bit, RegClass::GPR, Address, offsetof(FEXCore::Core::CPUState, excl_addr));
  _StoreContext(OpSize::i64Bit, RegClass::GPR, Value, offsetof(FEXCore::Core::CPUState, excl_value));
  _StoreContext(OpSize::i8Bit, RegClass::GPR, Constant(1U << Size), offsetof(FEXCore::Core::CPUState, excl_size));
  _StoreContext(OpSize::i8Bit, RegClass::GPR, Constant(1), offsetof(FEXCore::Core::CPUState, excl_valid));
  StoreReg(Rt, Size == 3, Value);
  if (Bit(Word, 15)) {
    _Fence(IR::FenceType::Acquire);
  }
  return true;
}

bool IRBuilder::LoadExclusivePair(uint32_t Word) {
  const bool Is64 = Bit(Word, 30);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);
  const uint32_t Rt2 = Bits(Word, 14, 10);

  Ref Address = LoadXSP(Rn);
  if (Is64) {
    Ref Val1 = _LoadMem(RegClass::GPR, OpSize::i64Bit, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    Ref Val2 = _LoadMem(RegClass::GPR, OpSize::i64Bit, Address, Constant(8), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    _StoreContext(OpSize::i64Bit, RegClass::GPR, Address, offsetof(FEXCore::Core::CPUState, excl_addr));
    _StoreContext(OpSize::i64Bit, RegClass::GPR, Val1, offsetof(FEXCore::Core::CPUState, excl_value));
    _StoreContext(OpSize::i64Bit, RegClass::GPR, Val2, offsetof(FEXCore::Core::CPUState, excl_value_hi));
    _StoreContext(OpSize::i8Bit, RegClass::GPR, Constant(16), offsetof(FEXCore::Core::CPUState, excl_size));
    _StoreContext(OpSize::i8Bit, RegClass::GPR, Constant(1), offsetof(FEXCore::Core::CPUState, excl_valid));
    StoreX(Rt, Val1);
    StoreX(Rt2, Val2);
  } else {
    Ref Pair64 = _LoadMem(RegClass::GPR, OpSize::i64Bit, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
    Ref Val1 = _Bfe(OpSize::i64Bit, 32, 0, Pair64);
    Ref Val2 = _Lshr(OpSize::i64Bit, Pair64, Constant(32));
    _StoreContext(OpSize::i64Bit, RegClass::GPR, Address, offsetof(FEXCore::Core::CPUState, excl_addr));
    _StoreContext(OpSize::i64Bit, RegClass::GPR, Pair64, offsetof(FEXCore::Core::CPUState, excl_value));
    _StoreContext(OpSize::i8Bit, RegClass::GPR, Constant(8), offsetof(FEXCore::Core::CPUState, excl_size));
    _StoreContext(OpSize::i8Bit, RegClass::GPR, Constant(1), offsetof(FEXCore::Core::CPUState, excl_valid));
    StoreW(Rt, Val1);
    StoreW(Rt2, Val2);
  }
  if (Bit(Word, 15)) {
    _Fence(IR::FenceType::Acquire);
  }
  return true;
}

bool IRBuilder::StoreExclusive(uint32_t Word) {
  const uint32_t Size = Bits(Word, 31, 30);
  const auto MemSize = IR::SizeToOpSize(1U << Size);
  const uint32_t Rs = Bits(Word, 20, 16);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  // Match = valid && excl_addr == Rn && excl_size == size.
  Ref Valid = _LoadContext(OpSize::i8Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, excl_valid));
  Ref ExclSize = _LoadContext(OpSize::i8Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, excl_size));
  Ref ExclAddr = _LoadContext(OpSize::i64Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, excl_addr));
  Ref AddrMatch = _Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::EQ, ExclAddr, LoadXSP(Rn), Constant(1), Constant(0));
  Ref SizeMatch = _Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::EQ, ExclSize, Constant(1U << Size), Valid, Constant(0));
  Ref Match = _And(OpSize::i64Bit, AddrMatch, SizeMatch);
  _StoreContext(OpSize::i8Bit, RegClass::GPR, Constant(0), offsetof(FEXCore::Core::CPUState, excl_valid));
  auto Branch = _CondJump(Match, Constant(0), InvalidNode, InvalidNode, CondClass::NEQ, OpSize::i64Bit);

  // SSA values are block local, so each arm reloads what it needs.
  Ref Current = GetCurrentBlock();
  Ref TryBlock = CreateNewCodeBlockAfter(Current);
  SetTrueJumpTarget(Branch, TryBlock);
  SetCurrentCodeBlock(TryBlock);
  {
    // The CAS lowering clobbers the host flags that hold NZCV.
    Ref NZCV = _LoadNZCV();
    Ref Expected = _LoadContext(OpSize::i64Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, excl_value));
    Ref Old = _CAS(MemSize, Expected, LoadX(Rt), LoadXSP(Rn));
    Ref Status = _Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::EQ, Old, Expected, Constant(0), Constant(1));
    _StoreNZCV(NZCV);
    StoreW(Rs, Status);
  }
  auto TryDone = _Jump();

  Ref FailBlock = CreateNewCodeBlockAfter(TryBlock);
  SetFalseJumpTarget(Branch, FailBlock);
  SetCurrentCodeBlock(FailBlock);
  StoreW(Rs, Constant(1));
  auto FailDone = _Jump();

  Ref Join = CreateNewCodeBlockAfter(FailBlock);
  SetJumpTarget(TryDone, Join);
  SetJumpTarget(FailDone, Join);
  SetCurrentCodeBlock(Join);
  return true;
}

bool IRBuilder::StoreExclusivePair(uint32_t Word) {
  const bool Is64 = Bit(Word, 30);
  const uint32_t Rs = Bits(Word, 20, 16);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);
  const uint32_t Rt2 = Bits(Word, 14, 10);

  // Match = valid && excl_addr == Rn && excl_size == (Is64 ? 16 : 8).
  Ref Valid = _LoadContext(OpSize::i8Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, excl_valid));
  Ref ExclSize = _LoadContext(OpSize::i8Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, excl_size));
  Ref ExclAddr = _LoadContext(OpSize::i64Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, excl_addr));
  Ref AddrMatch = _Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::EQ, ExclAddr, LoadXSP(Rn), Constant(1), Constant(0));
  Ref SizeMatch = _Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::EQ, ExclSize, Constant(Is64 ? 16 : 8), Valid, Constant(0));
  Ref Match = _And(OpSize::i64Bit, AddrMatch, SizeMatch);
  _StoreContext(OpSize::i8Bit, RegClass::GPR, Constant(0), offsetof(FEXCore::Core::CPUState, excl_valid));
  auto Branch = _CondJump(Match, Constant(0), InvalidNode, InvalidNode, CondClass::NEQ, OpSize::i64Bit);

  // SSA values are block local, so each arm reloads what it needs.
  Ref Current = GetCurrentBlock();
  Ref TryBlock = CreateNewCodeBlockAfter(Current);
  SetTrueJumpTarget(Branch, TryBlock);
  SetCurrentCodeBlock(TryBlock);
  {
    Ref NZCV = _LoadNZCV();
    Ref Status {};
    if (Is64) {
      Ref ExpLo = _LoadContext(OpSize::i64Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, excl_value));
      Ref ExpHi = _LoadContext(OpSize::i64Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, excl_value_hi));
      Ref DesLo = LoadX(Rt);
      Ref DesHi = LoadX(Rt2);
      Ref Address = LoadXSP(Rn);
      Ref OutLo = _Copy(ExpLo);
      Ref OutHi = _Copy(ExpHi);
      _CASPair(OpSize::i64Bit, ExpLo, ExpHi, DesLo, DesHi, Address, OutLo, OutHi);
      Ref LoMatch = _Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::EQ, OutLo, ExpLo, Constant(1), Constant(0));
      Ref HiMatch = _Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::EQ, OutHi, ExpHi, Constant(1), Constant(0));
      Ref BothMatch = _And(OpSize::i64Bit, LoMatch, HiMatch);
      Status = _Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::EQ, BothMatch, Constant(1), Constant(0), Constant(1));
    } else {
      Ref Expected = _LoadContext(OpSize::i64Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, excl_value));
      Ref Lo = _Bfe(OpSize::i64Bit, 32, 0, LoadX(Rt));
      Ref Hi = _Lshl(OpSize::i64Bit, LoadX(Rt2), Constant(32));
      Ref Desired = _Or(OpSize::i64Bit, Hi, Lo);
      Ref Old = _CAS(OpSize::i64Bit, Expected, Desired, LoadXSP(Rn));
      Status = _Select(OpSize::i64Bit, OpSize::i64Bit, CondClass::EQ, Old, Expected, Constant(0), Constant(1));
    }
    _StoreNZCV(NZCV);
    StoreW(Rs, Status);
  }
  auto TryDone = _Jump();

  Ref FailBlock = CreateNewCodeBlockAfter(TryBlock);
  SetFalseJumpTarget(Branch, FailBlock);
  SetCurrentCodeBlock(FailBlock);
  StoreW(Rs, Constant(1));
  auto FailDone = _Jump();

  Ref Join = CreateNewCodeBlockAfter(FailBlock);
  SetJumpTarget(TryDone, Join);
  SetJumpTarget(FailDone, Join);
  SetCurrentCodeBlock(Join);
  return true;
}

bool IRBuilder::LoadStoreAtomicWidth(uint32_t Word) {
  // LDAR/LDLAR and STLR/STLLR: one load or store of the register width at
  // [Rn], no offset, no monitor. Bit 22 selects the load.
  //
  // The ordering is the whole point of these encodings and cannot be dropped.
  // LDAR/STLR are RCsc, not merely acquire/release: an STLR followed by an
  // LDAR to a DIFFERENT location may not be reordered, which is what makes a
  // Dekker-shaped handoff ("publish, then check whether the peer parked" on
  // one side, "park, then check whether the peer published" on the other)
  // safe on AArch64. Every lock-free wakeup in glibc, JSC's ParkingLot and
  // Bun's thread pool is that shape, and LLVM emits LDAR/STLR for exactly it.
  // PPC64 does not order store-then-load on its own, so with plain loads and
  // stores here both sides read stale, nobody issues the FUTEX_WAKE, and the
  // guest deadlocks with every thread parked on a word that never changes.
  //
  // Use the standard leading-sync mapping: `hwsync` before the access, plus an
  // acquire fence after the load. Leading sync on both sides is what forbids
  // the store-then-load reordering; the trailing lwsync (FenceType::Acquire) is
  // the acquire half. It used to be lwsync; isync, whose isync is the x86
  // LFENCE speculation-barrier half and bought AArch64 acquire nothing. Both halves must use the same convention, so do not "optimise" one
  // of them into a trailing sync without doing the other.
  const uint32_t Size = Bits(Word, 31, 30);
  const bool IsLoad = Bit(Word, 22);
  Ref Address = LoadXSP(Bits(Word, 9, 5));
  _Fence(IR::FenceType::LoadStore);
  LoadStoreSingle(IsLoad, IR::SizeToOpSize(1U << Size), false, Size == 3, Bits(Word, 4, 0), Address);
  if (IsLoad) {
    _Fence(IR::FenceType::Acquire);
  }
  return true;
}


// FEAT_LSE atomic memory operations: LDADD/LDCLR/LDEOR/LDSET and SWP, in all
// four widths and all four ordering variants. The guest picks these over the
// LDXR/STXR loop through libgcc/compiler-rt's outline-atomics flag
// (__aarch64_have_lse_atomics), and some builds - the Bun-based Claude Code
// executable among them - set that flag unconditionally instead of reading
// AT_HWCAP, so leaving these unimplemented is a SIGILL in ordinary programs
// however the emulator advertises itself.
//
// Ordering: A and R (bits 23 and 22). The PPC64 backend brackets each atomic
// with hwsync/isync (AtomicOps.cpp), which is at least as strong as any of the
// four variants asks for, and every variant with A or R set still gets exactly
// that. Only the fully relaxed form -- neither bit set -- passes Relaxed and
// drops the bracket (checklist P7, first step). That is safe under either fence
// convention because a relaxed RMW takes no part in acquire/release ordering at
// all: AArch64 promises it only atomicity and coherence, which the larx/stcx.
// loop provides alone, and any ordering the program wants around it comes from
// a DMB, which keeps its own hwsync. unittests/A64Frontend/litmus.c gates
// exactly that composition (sb/mp+ldadd.relaxed+dmb).
//
// None of this covers LDAR/STLR and DMB, which are plain accesses to the
// backend and need their barriers emitted explicitly -- see
// LoadStoreAtomicWidth above and Barrier() in TranslateBranchSystem.cpp.
//
// Rs == 31 is the zero register, so LDADD with Rs == 31 is a plain load.
// Rt == 31 is the ST<op> alias: StoreReg drops the write.
bool IRBuilder::AtomicMemOp(uint32_t Word) {
  const uint32_t Size = Bits(Word, 31, 30);
  const auto MemSize = IR::SizeToOpSize(1U << Size);
  const bool Is64 = Size == 3;
  const uint32_t Op = Bits(Word, 15, 12); // o3:opc
  const uint32_t Rs = Bits(Word, 20, 16);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);
  const bool Relaxed = !Bit(Word, 23) && !Bit(Word, 22);

  Ref Address = LoadXSP(Rn);
  Ref Value = LoadX(Rs);
  Ref Old {};
  switch (Op) {
  case 0b0000: Old = _AtomicFetchAdd(MemSize, Value, Address, Relaxed); break;
  case 0b0001: Old = _AtomicFetchCLR(MemSize, Value, Address, Relaxed); break;
  case 0b0010: Old = _AtomicFetchXor(MemSize, Value, Address, Relaxed); break;
  case 0b0011: Old = _AtomicFetchOr(MemSize, Value, Address, Relaxed); break;
  case 0b1000: Old = _AtomicSwap(MemSize, Value, Address, Relaxed); break;
  // LDSMAX/LDSMIN/LDUMAX/LDUMIN (opc 010x/011x) go to AtomicMinMax.
  default: return false;
  }
  StoreReg(Rt, Is64, Old);
  return true;
}

// FEAT_LSE LDSMAX/LDSMIN/LDUMAX/LDUMIN, all four widths and all four ordering
// variants. POWER has no atomic min or max, and the IR has no AtomicFetch op
// for one, so these are the one LSE family that has to be spelled out as a
// load / compare / CAS retry loop.
//
// A new IR op per form with an LL/SC lowering would be one memory op per
// iteration instead of two, but it would also need the four misaligned
// fallbacks every other AtomicFetch* op carries (EmitInlineContainedRMW, a
// SplitLockOp enum entry and an ApplyRmwOp case that knows the operand width
// for the signed compare) -- and all of that is dead weight for these
// encodings, because a misaligned LSE atomic faults on real hardware rather
// than being emulated. The loop reuses _CAS, which already has those paths
// right. Min/max is also the case where a CAS loop is at its best: the ABA
// window a value-compare CAS leaves open is harmless here, because if memory
// goes A -> B -> A our stored max(A, operand) is still correct for the A the
// CAS matched.
//
// The loop carries nothing across its own back edge: the address and the
// operand are reloaded from Rn and Rs each time round, and the loaded value
// goes to CPUState::atomic_scratch, which the block after the loop reads.
// See the comment on that field for why neither an SSA value nor a guest
// register can hold it.
bool IRBuilder::AtomicMinMax(uint32_t Word) {
  const uint32_t Size = Bits(Word, 31, 30);
  const auto MemSize = IR::SizeToOpSize(1U << Size);
  const bool Is64 = Size == 3;
  const uint32_t Op = Bits(Word, 15, 12); // o3:opc
  const uint32_t Rs = Bits(Word, 20, 16);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  // opc 0100 LDSMAX, 0101 LDSMIN, 0110 LDUMAX, 0111 LDUMIN.
  const bool Unsigned = (Op & 0b0010) != 0;
  const bool IsMin = (Op & 0b0001) != 0;
  const auto Cond = Unsigned ? (IsMin ? CondClass::ULT : CondClass::UGT) : (IsMin ? CondClass::SLT : CondClass::SGT);
  // Relaxed (neither A nor R) lets the loop's CAS drop its fence bracket, as
  // AtomicMemOp explains; atomicity comes from the CAS either way.
  const bool Relaxed = !Bit(Word, 23) && !Bit(Word, 22);

  // The loop body needs a block of its own to branch back to; the current one
  // holds the guest instructions before this one. A forward Jump to the next
  // block emits no host instruction (P6), so entering costs nothing.
  Ref Head = CreateNewCodeBlockAfter(GetCurrentBlock());
  auto Enter = _Jump();
  SetJumpTarget(Enter, Head);
  SetCurrentCodeBlock(Head);

  Ref Address = LoadXSP(Rn);
  Ref Value = LoadX(Rs);

  // _Select and _CAS both clobber the host flags that hold NZCV, and these
  // instructions leave PSTATE.NZCV alone. Save it across the whole body.
  Ref NZCV = _LoadNZCV();
  Ref Old = _LoadMem(RegClass::GPR, MemSize, Address, Invalid(), OpSize::i8Bit, MemOffsetType::SXTX, 1);
  _StoreContext(OpSize::i64Bit, RegClass::GPR, Old, offsetof(FEXCore::Core::CPUState, atomic_scratch));

  // Compare at the access width. Select only validates a 32- or 64-bit
  // CompareSize, so the byte and halfword forms are widened here rather than
  // compared narrow: Old arrives zero-extended from _LoadMem, so an unsigned
  // compare needs only the operand masked, while a signed one needs both sides
  // sign-extended out of the access width. The value that goes to memory is
  // the unwidened Old or Value -- _CAS stores the access width and drops the
  // rest.
  const uint8_t WidthBits = 8U << Size;
  Ref OldCmp = Old;
  Ref ValueCmp = Value;
  if (!Is64) {
    if (Unsigned) {
      ValueCmp = _Bfe(OpSize::i64Bit, WidthBits, 0, Value);
    } else {
      OldCmp = _Sbfe(OpSize::i64Bit, WidthBits, 0, Old);
      ValueCmp = _Sbfe(OpSize::i64Bit, WidthBits, 0, Value);
    }
  }
  Ref New = _Select(OpSize::i64Bit, OpSize::i64Bit, Cond, OldCmp, ValueCmp, Old, Value);

  // _CAS returns the value it observed, zero-extended at the access width, as
  // does _LoadMem -- so a mismatch is a plain 64-bit compare either way.
  Ref Seen = _CAS(MemSize, Old, New, Address, Relaxed);
  _StoreNZCV(NZCV);
  auto Retry = _CondJump(Seen, Old, InvalidNode, InvalidNode, CondClass::NEQ, OpSize::i64Bit);
  SetTrueJumpTarget(Retry, Head);

  Ref Done = CreateNewCodeBlockAfter(Head);
  SetFalseJumpTarget(Retry, Done);
  SetCurrentCodeBlock(Done);
  StoreReg(Rt, Is64, _LoadContext(OpSize::i64Bit, RegClass::GPR, offsetof(FEXCore::Core::CPUState, atomic_scratch)));
  return true;
}

// LDAPRB/LDAPRH/LDAPR: an acquire load of the access width, no monitor.
bool IRBuilder::LDAPR(uint32_t Word) {
  // RCpc acquire, weaker than LDAR: it orders this load against everything
  // after it, but carries no store-then-load guarantee, so it needs the
  // trailing acquire fence only -- no leading hwsync.
  const uint32_t Size = Bits(Word, 31, 30);
  LoadStoreSingle(true, IR::SizeToOpSize(1U << Size), false, Size == 3, Bits(Word, 4, 0), LoadXSP(Bits(Word, 9, 5)));
  _Fence(IR::FenceType::Acquire);
  return true;
}

// FEAT_LSE CASB/CASH/CAS.
//
// CAS compares [Xn] against Rs and stores Rt on a match; Rs is overwritten with
// the value that was in memory either way. The CAS lowering clobbers the host
// flags that hold NZCV, as it does in StoreExclusive.
bool IRBuilder::CompareAndSwap(uint32_t Word) {
  const uint32_t Size = Bits(Word, 31, 30);
  const auto MemSize = IR::SizeToOpSize(1U << Size);
  const uint32_t Rs = Bits(Word, 20, 16);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  // Acquire is bit 22 (L) and release is bit 15 (o0) in this encoding, not
  // the 23/22 of the LDADD family. Plain CAS -- neither -- is relaxed and
  // drops the fence bracket, as AtomicMemOp explains.
  const bool Relaxed = !Bit(Word, 22) && !Bit(Word, 15);
  Ref NZCV = _LoadNZCV();
  Ref Old = _CAS(MemSize, LoadX(Rs), LoadX(Rt), LoadXSP(Rn), Relaxed);
  _StoreNZCV(NZCV);
  StoreReg(Rs, Size == 3, Old);
  return true;
}

// FEAT_LSE CASP: the paired compare-and-swap. Rs and Rt each name the first of
// an even/odd register pair, and the memory operand is twice the register
// width -- 8 bytes for the 32-bit form, 16 for the 64-bit one. Bit 30 selects.
//
// The pair {Rs, Rs+1} is compared against memory and {Rt, Rt+1} stored on a
// match; Rs:Rs+1 receive what was in memory either way. In the little-endian
// image Rs holds the low half, at [Xn], and Rs+1 the half above it. That is
// the same shape as x86's CMPXCHG8B/CMPXCHG16B writing back into EDX:EAX,
// which is what the IR's CASPair op and its PPC64 lowering were written for:
// Size i32Bit is the 8-byte-memory form and i64Bit the 16-byte one, matching
// CASP's two variants exactly.
//
// Worth knowing when reading a bug here: nothing in this tree used CASPair
// before this, so its lowering had never executed. There is no x86 frontend
// here to have exercised it, and the A64 frontend reached the plain _CAS only.
//
// CASPair is a multi-destination op, which the IR generator implements by
// appending the destinations as ordinary `Out` SSA sources ("Named
// destinations require side effects because they break SSA hard") -- the
// backend writes into whatever registers those operands were allocated, and
// the reads below then see the written values. They need to be register
// resident and distinct from the Expected operands the lowering still needs
// while it works, so they are fresh _Copy nodes rather than the Expected
// values themselves.
//
// Unlike _CAS, CASPair is not declared ImplicitFlagClobber: its lowering saves
// CR0 to the red zone on entry and restores it on exit, so no NZCV dance is
// needed around it here.
bool IRBuilder::CompareAndSwapPair(uint32_t Word) {
  const bool Is64 = Bit(Word, 30);
  const auto MemSize = Is64 ? OpSize::i64Bit : OpSize::i32Bit;
  const uint32_t Rs = Bits(Word, 20, 16);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  // An odd Rs or Rt is UNDEFINED, so decode it as unallocated rather than
  // guessing a pair: returning false raises the SIGILL the architecture asks
  // for. Rs or Rt == 30 is legal, and makes the odd half of the pair register
  // 31, which LoadX reads as XZR and StoreX discards -- the architectural
  // meaning, and what these helpers already do for that number.
  if ((Rs & 1) || (Rt & 1)) {
    return false;
  }

  Ref ExpLo = LoadX(Rs);
  Ref ExpHi = LoadX(Rs + 1);
  Ref DesLo = LoadX(Rt);
  Ref DesHi = LoadX(Rt + 1);
  Ref Address = LoadXSP(Rn);

  Ref OutLo = _Copy(ExpLo);
  Ref OutHi = _Copy(ExpHi);
  _CASPair(MemSize, ExpLo, ExpHi, DesLo, DesHi, Address, OutLo, OutHi);

  StoreReg(Rs, Is64, OutLo);
  StoreReg(Rs + 1, Is64, OutHi);
  return true;
}

} // namespace FEXCore::A64
