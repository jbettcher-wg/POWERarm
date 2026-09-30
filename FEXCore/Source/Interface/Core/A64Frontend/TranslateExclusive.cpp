// SPDX-License-Identifier: MIT
//
// A64 exclusive loads and stores (LDXR/LDAXR, STXR/STLXR) and the plain
// atomic-width loads and stores LDAR/LDLAR/STLR/STLLR.
//
// There are two lowerings for the exclusives. TryFuseExclusiveLoop at the
// bottom of this file recognises the complete guest retry loop and emits
// POWER's native lwarx/stwcx. reservation for it; everything else goes through
// the software exclusive monitor in CPUState (DESIGN.md §4.3) described next.
// The two agree by construction: the fused path clears excl_valid, so a
// software STXR that is not paired with a software LDXR always fails.
//
// LDXR records the address, size and loaded value and marks the monitor
// valid. STXR succeeds when the monitor is valid for the same address and
// size and the memory still holds the recorded value; the store itself is a
// compare-and-swap against that value, so a concurrent writer that changed
// the value makes it fail. A writer that restores the same value between the
// two is not detected (the ABA case the design accepts for the software
// path). Either way the monitor is cleared.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/DecodeTable.h"
#include "Interface/Core/A64Frontend/Decoder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Utils/LogManager.h>

#include <string_view>

namespace FEXCore::A64 {
using namespace FEXCore::IR;

bool IRBuilder::LoadExclusive(uint32_t Word) {
  const uint32_t Size = Bits(Word, 31, 30);
  const auto MemSize = IR::SizeToOpSize(1U << Size);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rt = Bits(Word, 4, 0);

  Ref Address = LoadXSP(Rn);
  // LDAXR is RCsc, exactly as LDAR is: a preceding STLR and this load may not
  // be reordered. That is a leading hwsync, and the trailing acquire fence
  // below is only the other half -- see LoadStoreAtomicWidth for the whole
  // argument and for why both halves have to use the same convention.
  if (Bit(Word, 15)) {
    _Fence(IR::FenceType::LoadStore);
  }
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
  // LDAXP is RCsc; see LoadExclusive.
  if (Bit(Word, 15)) {
    _Fence(IR::FenceType::LoadStore);
  }
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
  // Bit 15 is o0, the release bit: STLXR has it, STXR does not. A plain STXR
  // asks for atomicity and nothing else, so its CAS drops the hwsync/isync
  // bracket the same way a fully relaxed LSE RMW does (AtomicMemOp explains
  // why that is safe). Until this was passed, an exclusive pair with neither
  // acquire nor release still paid two heavyweight syncs.
  const bool Relaxed = !Bit(Word, 15);

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
    Ref Old = _CAS(MemSize, Expected, LoadX(Rt), LoadXSP(Rn), Relaxed);
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
  // As in StoreExclusive: bit 15 is o0. Only the 32-bit form can use it --
  // _CASPair has no Relaxed flag and keeps its unconditional bracket.
  const bool Relaxed = !Bit(Word, 15);

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
      Ref Old = _CAS(OpSize::i64Bit, Expected, Desired, LoadXSP(Rn), Relaxed);
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

// ---------------------------------------------------------------------------
// Native lwarx/stwcx. for the complete LDXR..STXR retry loop
// ---------------------------------------------------------------------------
//
// POWER's reservation is weaker than AArch64's monitor in the one way that
// matters: anything between the l*arx and the st*cx. can take it away. A store
// to the granule from another processor, another l*arx, a st*cx., a context
// switch. AArch64 lets STXR fail spuriously, so losing it is always
// *architecturally* legal -- but it is not automatically *progress*. If the
// emulator does real work between the guest's LDXR and its STXR (a block
// boundary, a dispatcher round trip, a lookup miss, a compile, a helper call
// that itself takes a reservation) the st*cx. can fail every single time and
// the guest spins forever.
//
// So the fusion condition is not "the pair is in one block", it is stronger:
// **the guest's entire retry loop is the block**, exactly
//
//     L: LDXR|LDAXR  Wt, [Xn]
//        <body: register-to-register instructions only>
//        STXR|STLXR  Ws, Wv, [Xn]
//        CBNZ        Ws, L
//
// with the CBNZ branching back to the LDXR itself. Three things follow, and
// together they are the forward-progress argument:
//
//   (a) The emitted region is straight-line host code with one backward branch
//       at its end. The decoder cannot have put a branch target inside it (a
//       leader ends a block), the body's whitelist contains nothing that
//       touches memory, calls a helper, raises a signal or emits another
//       reservation instruction, and no IR block boundary falls inside it, so
//       there is no dispatcher exit, no block link, no drain-point branch and
//       no second l*arx in the window. Nothing the *emulator* does can lose
//       the reservation. Only real contention and real preemption can.
//   (b) A retry is therefore progress-equivalent to the guest's own retry. The
//       host loop re-executes precisely the guest instructions the guest's
//       CBNZ would have re-executed, in the same order, on the same registers
//       -- because the fused region *is* the guest's loop body. The guest
//       cannot tell the two apart, which is also why the body may freely
//       overwrite its own inputs: the guest's loop has the same property.
//   (c) Under contention the loop is the ordinary POWER LL/SC loop that every
//       native atomic on this machine already is: lock-free, and some CPU
//       always completes. Preemption between the l*arx and the st*cx. costs
//       one extra pass, not a livelock, since the reservation is re-armed by
//       the retry.
//
// A guest signal cannot be delivered inside the region -- deferred signals are
// drained at the poke the backend emits at entry points and backward IR edges,
// and there is neither inside a fused loop -- so the window is a handful of
// instructions during which signals stay pending, exactly as they already do
// inside the CAS lowering's own l*arx/st*cx. loop. A *synchronous* fault is
// possible (the l*arx or st*cx. can SIGSEGV, or SIGBUS on an unaligned
// address, which is what AArch64 LDXR does too); the region emits a RIP-table
// marker per fused guest instruction, so the fault is attributed to the right
// guest PC, and resuming there re-enters through the software monitor, whose
// excl_valid this region clears -- so the STXR reports failure and the guest
// loops round. Terminating, and architecturally permitted.
//
// Everything that is not this shape keeps the software monitor. The compare-
// exchange loop (`ldaxr; cmp; b.ne out; stlxr; cbnz`) is the notable one: the
// b.ne ends the decoded block, so the pair straddles two blocks and cannot
// carry a progress guarantee. It falls back, and the fallback is now cheaper
// too (see StoreExclusive's Relaxed flag).
//
// LDXP/STXP stay on the software monitor unconditionally. lqarx/stqcx. need an
// even register pair and 16-byte alignment, and there is no ldxp/stxp retry
// loop in the reference binaries worth the second lowering; the progress
// argument would have to be made again for a pair whose granule handling
// differs. Said once here so the next reader does not have to re-derive it.

// The body instructions a fused window may contain: AArch64 data-processing
// forms that write exactly one GPR (Rd, bits 4:0), read only GPRs, touch no
// memory, set no flags, raise no signal, and whose ppc64le lowerings emit
// neither a helper call nor a reservation instruction. Deliberately narrow:
// this covers what compilers actually put in an exclusive loop (the
// __atomic_fetch_* bodies, the exchange with no body at all) and nothing whose
// lowering would have to be re-audited.
static bool ExclusiveBodyWrites(const InstMatcher* Matcher, uint32_t Word, uint32_t& Rd) {
  if (!Matcher || !Matcher->Handler) {
    return false;
  }
  static constexpr std::string_view Allowed[] = {
    "ADD_imm", "SUB_imm", "AND_imm", "ORR_imm", "EOR_imm",
    "ADD_shift", "SUB_shift", "AND_shift", "ORR_shift", "EOR_shift", "BIC_shift", "ORN_shift", "EON",
    "ADD_ext", "SUB_ext", "MOVZ", "MOVN", "MOVK", "UBFM", "SBFM",
  };
  const std::string_view Name {Matcher->Name};
  for (const auto& A : Allowed) {
    if (Name == A) {
      Rd = Bits(Word, 4, 0);
      return true;
    }
  }
  return false;
}

size_t IRBuilder::TryFuseExclusiveLoop(const Decoder::DecodedBlocks& Block, size_t Index) {
  // At most this many register-only guest instructions between the pair. Every
  // real body is 0-2 (an add, a bic+orr); the bound only keeps a pathological
  // decode from growing the reservation window without a reason.
  constexpr size_t MaxBody = 8;

  // The LDXR must be the block's first instruction. That is not an extra
  // requirement: the CBNZ below has to target it, so its PC is a leader, and
  // the decoder splits a block at every leader.
  if (Index != 0 || Block.NumInstructions < 3 || Block.NumInstructions > MaxBody + 3) {
    return 0;
  }
  const size_t LastIdx = Block.NumInstructions - 1;
  const size_t StoIdx = LastIdx - 1;

  const auto& Ld = Block.DecodedInstructions[Index];
  const auto& Sto = Block.DecodedInstructions[StoIdx];
  const auto& Br = Block.DecodedInstructions[LastIdx];
  if (!Ld.Matcher || !Sto.Matcher || !Br.Matcher) {
    return 0;
  }

  const std::string_view LdName {Ld.Matcher->Name};
  const std::string_view StoName {Sto.Matcher->Name};
  if ((LdName != "LDXR" && LdName != "LDAXR") || (StoName != "STXR" && StoName != "STLXR") ||
      std::string_view {Br.Matcher->Name} != "CBNZ") {
    return 0;
  }

  // CBZ is not this idiom (it branches on success, not on failure), and is
  // rejected above by name. The CBNZ must close the loop at the LDXR.
  const uint64_t TargetPC = Br.PC + SignExtend(Bits(Br.Word, 23, 5), 19) * 4;
  if (TargetPC != Ld.PC) {
    return 0;
  }

  const uint32_t Size = Bits(Ld.Word, 31, 30);
  const uint32_t Rn = Bits(Ld.Word, 9, 5);
  const uint32_t RtLd = Bits(Ld.Word, 4, 0);
  const uint32_t Rs = Bits(Sto.Word, 20, 16);
  const uint32_t RtSt = Bits(Sto.Word, 4, 0);

  // Same access width and same base register, or the pair is not a pair.
  if (Bits(Sto.Word, 31, 30) != Size || Bits(Sto.Word, 9, 5) != Rn) {
    return 0;
  }
  // The CBNZ must test the status the STXR wrote, and nothing else.
  if (Bits(Br.Word, 4, 0) != Rs) {
    return 0;
  }
  // STXR with s == t or s == n is CONSTRAINED UNPREDICTABLE; leave those to the
  // software monitor rather than pick a behaviour here. Rs == 31 discards the
  // status, which makes the CBNZ a `cbnz wzr` that never retries -- not this
  // idiom either.
  if (Rs == 31 || Rs == Rn || Rs == RtSt) {
    return 0;
  }
  // The loaded value must not land in the base register: the second pass round
  // the loop would then address somewhere else, and the address SSA value this
  // region hands both halves of the pair would no longer be Xn.
  if (RtLd == Rn) {
    return 0;
  }

  // Body: whitelisted, and it must leave the base register and the status
  // register alone. It may do anything it likes to the loaded value.
  for (size_t i = Index + 1; i < StoIdx; ++i) {
    uint32_t Rd = 0;
    if (!ExclusiveBodyWrites(Block.DecodedInstructions[i].Matcher, Block.DecodedInstructions[i].Word, Rd)) {
      return 0;
    }
    if (Rd == Rn || Rd == Rs) {
      return 0;
    }
  }

  const auto MemSize = IR::SizeToOpSize(1U << Size);
  const bool Is64 = Size == 3;
  const bool Acquire = Bit(Ld.Word, 15);  // LDAXR
  const bool Release = Bit(Sto.Word, 15); // STLXR

  // One address value for both halves of the pair. Same SSA node, so the
  // register allocator keeps it in one register across the whole region and the
  // st*cx. cannot address anywhere other than the l*arx did.
  Ref Address = LoadXSP(Rn);

  // The software monitor must not survive a fused loop: a later unpaired
  // software STXR has to fail, and a stale excl_valid from an earlier software
  // LDXR would let it succeed. Reporting failure there is exactly what the
  // architecture permits. One byte store, outside the loop, and a store by this
  // processor never clears a POWER reservation.
  _StoreContext(OpSize::i8Bit, RegClass::GPR, Constant(0), offsetof(FEXCore::Core::CPUState, excl_valid));

  // Leading hwsync, once, before the loop. LDAXR and STLXR are RCsc, the same
  // as LDAR and STLR, so both need the leading sync that forbids store-then-load
  // reordering -- see LoadStoreAtomicWidth for why that half cannot be moved to
  // the trailing side. Hoisting it above the l*arx rather than emitting one per
  // annotation is what collapses two syncs into one; it is at least as strong as
  // a per-instruction placement, because everything it orders it orders against
  // both halves of the pair, and the l*arx/st*cx. pair is ordered internally by
  // the reservation itself. unittests/MemoryModel/check.sh gates this.
  if (Acquire || Release) {
    _Fence(IR::FenceType::LoadStore);
  }

  Ref Value = _LoadReserved(MemSize, Address);
  StoreReg(RtLd, Is64, Value);
  if (Acquire) {
    // The acquire half, inside the loop: it has to order the value this pass
    // loaded against everything after it.
    _Fence(IR::FenceType::Acquire);
  }

  // The body, translated exactly as it would be outside a loop.
  for (size_t i = Index + 1; i < StoIdx; ++i) {
    const auto& Inst = Block.DecodedInstructions[i];
    _GuestOpcode(Inst.PC - Entry);
    if (!TranslateInstruction(Inst)) {
      // Unreachable: every whitelisted handler returns true for a matched word.
      // If one ever does not, the region is already half emitted, so fail loudly
      // rather than leave a l*arx with no st*cx.
      ERROR_AND_DIE_FMT("exclusive-loop body instruction {:#x} at {:#x} did not translate", Inst.Word, Inst.PC);
    }
  }

  _GuestOpcode(Sto.PC - Entry);
  _StoreConditional(MemSize, LoadX(RtSt), Address);
  // The st*cx. only falls through once it stored, so the architectural status is
  // always success and the guest's CBNZ is never taken. One `li Ws,0`; dropped
  // entirely when Ws is the zero register, which the Rs == 31 reject above means
  // cannot happen here.
  _GuestOpcode(Br.PC - Entry);
  StoreW(Rs, Constant(0));
  ExitToPC(Br.PC + INSTRUCTION_SIZE);
  return Block.NumInstructions - Index;
}

} // namespace FEXCore::A64
