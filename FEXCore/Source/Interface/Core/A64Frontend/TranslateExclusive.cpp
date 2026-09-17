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
  // the store-then-load reordering; the trailing lwsync/isync is the acquire
  // half. Both halves must use the same convention, so do not "optimise" one
  // of them into a trailing sync without doing the other.
  const uint32_t Size = Bits(Word, 31, 30);
  const bool IsLoad = Bit(Word, 22);
  Ref Address = LoadXSP(Bits(Word, 9, 5));
  _Fence(IR::FenceType::LoadStore);
  LoadStoreSingle(IsLoad, IR::SizeToOpSize(1U << Size), false, Size == 3, Bits(Word, 4, 0), Address);
  if (IsLoad) {
    _Fence(IR::FenceType::Load);
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
// Ordering: A and R (bits 23 and 22) are ignored here because the PPC64
// backend already brackets every atomic with hwsync/isync (AtomicOps.cpp),
// which is at least as strong as any of the four variants asks for. That
// reasoning covers the JIT's atomics ONLY -- LDAR/STLR and DMB are plain
// accesses to the backend and need their barriers emitted explicitly, see
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

  Ref Address = LoadXSP(Rn);
  Ref Value = LoadX(Rs);
  Ref Old {};
  switch (Op) {
  case 0b0000: Old = _AtomicFetchAdd(MemSize, Value, Address); break;
  case 0b0001: Old = _AtomicFetchCLR(MemSize, Value, Address); break;
  case 0b0010: Old = _AtomicFetchXor(MemSize, Value, Address); break;
  case 0b0011: Old = _AtomicFetchOr(MemSize, Value, Address); break;
  case 0b1000: Old = _AtomicSwap(MemSize, Value, Address); break;
  // LDSMAX/LDSMIN/LDUMAX/LDUMIN (opc 010x/011x) need a CAS retry loop and are
  // still unimplemented; the outline-atomics helpers never emit them.
  default: return false;
  }
  StoreReg(Rt, Is64, Old);
  return true;
}

// LDAPRB/LDAPRH/LDAPR: an acquire load of the access width, no monitor.
bool IRBuilder::LDAPR(uint32_t Word) {
  // RCpc acquire, weaker than LDAR: it orders this load against everything
  // after it, but carries no store-then-load guarantee, so it needs the
  // trailing acquire fence only -- no leading hwsync.
  const uint32_t Size = Bits(Word, 31, 30);
  LoadStoreSingle(true, IR::SizeToOpSize(1U << Size), false, Size == 3, Bits(Word, 4, 0), LoadXSP(Bits(Word, 9, 5)));
  _Fence(IR::FenceType::Load);
  return true;
}

// FEAT_LSE CASB/CASH/CAS. CASP is still unimplemented: the pair form needs the
// two-result CASPair op, and no outline-atomics helper emits it.
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

  Ref NZCV = _LoadNZCV();
  Ref Old = _CAS(MemSize, LoadX(Rs), LoadX(Rt), LoadXSP(Rn));
  _StoreNZCV(NZCV);
  StoreReg(Rs, Size == 3, Old);
  return true;
}

} // namespace FEXCore::A64
