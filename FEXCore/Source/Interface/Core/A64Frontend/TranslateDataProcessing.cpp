// SPDX-License-Identifier: MIT
//
// A64 integer data processing: PC-relative addressing, add/sub, logical,
// move wide, bitfield, extract, shifts, bit counting and reversal,
// conditional compare and select, multiply and divide.
//
// Semantics follow the Arm A-profile pseudocode, cross-checked against
// dynarmic's translators (0BSD).
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

namespace FEXCore::A64 {
using namespace FEXCore::IR;

// ---------------------------------------------------------------------------
// PC-relative addressing
// ---------------------------------------------------------------------------

bool IRBuilder::ADR(uint32_t Word) {
  const int64_t Imm = SignExtend((Bits(Word, 23, 5) << 2) | Bits(Word, 30, 29), 21);
  StoreX(Bits(Word, 4, 0), PCValue(CurrentPC + Imm));
  return true;
}

bool IRBuilder::ADRP(uint32_t Word) {
  const int64_t Imm = SignExtend((Bits(Word, 23, 5) << 2) | Bits(Word, 30, 29), 21) * 4096;
  StoreX(Bits(Word, 4, 0), PCValue((CurrentPC & ~0xFFFULL) + Imm));
  return true;
}

// ---------------------------------------------------------------------------
// Add/sub (immediate)
// ---------------------------------------------------------------------------

bool IRBuilder::AddSubImmediate(uint32_t Word, bool IsSub, bool SetFlags) {
  const bool Is64 = Bit(Word, 31);
  const uint32_t Shift = Bits(Word, 23, 22);
  if (Shift > 1) {
    return false;
  }
  const uint64_t Imm = Bits(Word, 21, 10) << (Shift * 12);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rd = Bits(Word, 4, 0);
  const auto Size = SizeFor(Is64);

  Ref Src = LoadXSP(Rn);
  Ref ImmRef = Constant(Imm);

  if (!SetFlags) {
    StoreRegSP(Rd, Is64, IsSub ? _Sub(Size, Src, ImmRef) : _Add(Size, Src, ImmRef));
    return true;
  }

  // CMP/CMN: flags only.
  if (Rd == 31) {
    IsSub ? _SubNZCV(Size, Src, ImmRef) : _AddNZCV(Size, Src, ImmRef);
    return true;
  }

  StoreReg(Rd, Is64, IsSub ? _SubWithFlags(Size, Src, ImmRef) : _AddWithFlags(Size, Src, ImmRef));
  return true;
}

bool IRBuilder::ADD_imm(uint32_t Word) {
  return AddSubImmediate(Word, false, false);
}
bool IRBuilder::ADDS_imm(uint32_t Word) {
  return AddSubImmediate(Word, false, true);
}
bool IRBuilder::SUB_imm(uint32_t Word) {
  return AddSubImmediate(Word, true, false);
}
bool IRBuilder::SUBS_imm(uint32_t Word) {
  return AddSubImmediate(Word, true, true);
}

// ---------------------------------------------------------------------------
// Logical (immediate)
// ---------------------------------------------------------------------------

bool IRBuilder::LogicalImmediate(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const uint32_t Opc = Bits(Word, 30, 29);
  const bool N = Bit(Word, 22);
  if (!Is64 && N) {
    return false;
  }

  uint64_t Imm {};
  if (!DecodeBitMasks(N, Bits(Word, 15, 10), Bits(Word, 21, 16), true, Is64 ? 64 : 32, &Imm, nullptr)) {
    return false;
  }

  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rd = Bits(Word, 4, 0);
  const auto Size = SizeFor(Is64);
  Ref Src = LoadX(Rn);
  Ref ImmRef = Constant(Imm);

  switch (Opc) {
  case 0b00: StoreRegSP(Rd, Is64, _And(Size, Src, ImmRef)); break;
  case 0b01: StoreRegSP(Rd, Is64, _Or(Size, Src, ImmRef)); break;
  case 0b10: StoreRegSP(Rd, Is64, _Xor(Size, Src, ImmRef)); break;
  default: StoreReg(Rd, Is64, _AndWithFlags(Size, Src, ImmRef)); break;
  }
  return true;
}

bool IRBuilder::AND_imm(uint32_t Word) {
  return LogicalImmediate(Word);
}
bool IRBuilder::ORR_imm(uint32_t Word) {
  return LogicalImmediate(Word);
}
bool IRBuilder::EOR_imm(uint32_t Word) {
  return LogicalImmediate(Word);
}
bool IRBuilder::ANDS_imm(uint32_t Word) {
  return LogicalImmediate(Word);
}

// ---------------------------------------------------------------------------
// Move wide
// ---------------------------------------------------------------------------

bool IRBuilder::MoveWide(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const uint32_t Opc = Bits(Word, 30, 29);
  const uint32_t Hw = Bits(Word, 22, 21);
  if (!Is64 && Hw > 1) {
    return false;
  }
  const uint32_t Shift = Hw * 16;
  const uint64_t Imm = Bits(Word, 20, 5) << Shift;
  const uint32_t Rd = Bits(Word, 4, 0);

  switch (Opc) {
  case 0b00: {
    // MOVN
    uint64_t Value = ~Imm;
    if (!Is64) {
      Value &= 0xFFFF'FFFFULL;
    }
    StoreX(Rd, Constant(Value));
    return true;
  }
  case 0b10:
    // MOVZ
    StoreX(Rd, Constant(Imm));
    return true;
  case 0b11: {
    // MOVK
    const auto Size = SizeFor(Is64);
    Ref Cleared = _And(Size, LoadX(Rd), Constant(~(0xFFFFULL << Shift)));
    StoreReg(Rd, Is64, _Or(Size, Cleared, Constant(Imm)));
    return true;
  }
  default: return false;
  }
}

bool IRBuilder::MOVN(uint32_t Word) {
  return MoveWide(Word);
}
bool IRBuilder::MOVZ(uint32_t Word) {
  return MoveWide(Word);
}
bool IRBuilder::MOVK(uint32_t Word) {
  return MoveWide(Word);
}

// ---------------------------------------------------------------------------
// Bitfield and extract
// ---------------------------------------------------------------------------

bool IRBuilder::Bitfield(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const uint32_t Opc = Bits(Word, 30, 29);
  const bool N = Bit(Word, 22);
  const uint32_t R = Bits(Word, 21, 16);
  const uint32_t S = Bits(Word, 15, 10);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rd = Bits(Word, 4, 0);
  const uint32_t DataSize = Is64 ? 64 : 32;

  if (Opc == 0b11 || N != Is64 || (!Is64 && (R >= 32 || S >= 32))) {
    return false;
  }

  // All three forms are expressed at 64 bits; StoreReg truncates the W forms.
  Ref Src = LoadX(Rn);
  Ref Result {};

  switch (Opc) {
  case 0b00: // SBFM
    if (S >= R) {
      Result = _Sbfe(OpSize::i64Bit, S - R + 1, R, Src);
    } else {
      Result = _Lshl(OpSize::i64Bit, _Sbfe(OpSize::i64Bit, S + 1, 0, Src), Constant(DataSize - R));
    }
    break;
  case 0b01: // BFM
    if (S >= R) {
      Result = _Bfxil(OpSize::i64Bit, S - R + 1, R, LoadX(Rd), Src);
    } else {
      Result = _Bfi(OpSize::i64Bit, S + 1, DataSize - R, LoadX(Rd), Src);
    }
    break;
  default: // UBFM
    if (S >= R) {
      Result = _Bfe(OpSize::i64Bit, S - R + 1, R, Src);
    } else {
      Result = _Lshl(OpSize::i64Bit, _Bfe(OpSize::i64Bit, S + 1, 0, Src), Constant(DataSize - R));
    }
    break;
  }

  StoreReg(Rd, Is64, Result);
  return true;
}

bool IRBuilder::SBFM(uint32_t Word) {
  return Bitfield(Word);
}
bool IRBuilder::BFM(uint32_t Word) {
  return Bitfield(Word);
}
bool IRBuilder::UBFM(uint32_t Word) {
  return Bitfield(Word);
}

bool IRBuilder::EXTR(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const bool N = Bit(Word, 22);
  const uint32_t Rm = Bits(Word, 20, 16);
  const uint32_t Lsb = Bits(Word, 15, 10);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rd = Bits(Word, 4, 0);
  if (N != Is64 || (!Is64 && Lsb >= 32)) {
    return false;
  }

  StoreReg(Rd, Is64, _Extr(SizeFor(Is64), LoadX(Rn), LoadX(Rm), Lsb));
  return true;
}

// ---------------------------------------------------------------------------
// Data processing (2 source): divide and variable shifts
// ---------------------------------------------------------------------------

bool IRBuilder::UDIV(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const auto Size = SizeFor(Is64);
  Ref Dividend = LoadX(Bits(Word, 9, 5));
  Ref Divisor = LoadX(Bits(Word, 20, 16));

  // ARM defines x / 0 = 0. POWER leaves divdu/divwu by zero undefined, so the
  // host divide only ever sees a non-zero divisor and the zero case is selected.
  Ref Zero = Constant(0);
  Ref SafeDivisor = _Select(Size, Size, CondClass::EQ, Divisor, Zero, Constant(1), Divisor);
  Ref Quotient = _AllocateGPR(false);
  Ref Remainder = _AllocateGPR(false);
  _UDiv(Size, Dividend, Invalid(), SafeDivisor, Quotient, Remainder);
  StoreReg(Bits(Word, 4, 0), Is64, _Select(Size, Size, CondClass::EQ, Divisor, Zero, Zero, Quotient));
  return true;
}

bool IRBuilder::SDIV(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const auto Size = SizeFor(Is64);
  Ref Dividend = LoadX(Bits(Word, 9, 5));
  Ref Divisor = LoadX(Bits(Word, 20, 16));

  // ARM defines x / 0 = 0 and INT_MIN / -1 = INT_MIN. POWER leaves divd/divw
  // undefined for both a zero divisor and the overflowing INT_MIN / -1, so the
  // host divide is only fed divisors outside {0, -1}: (Divisor + 1) <=u 1
  // exactly for those two, and they are replaced by 1. The results for 0 and
  // -1 are then selected: 0, and the wrapping negation (-INT_MIN == INT_MIN).
  Ref Zero = Constant(0);
  Ref One = Constant(1);
  Ref MinusOne = Constant(-1);
  Ref DivisorPlusOne = _Add(Size, Divisor, One);
  Ref SafeDivisor = _Select(Size, Size, CondClass::ULE, DivisorPlusOne, One, One, Divisor);
  Ref Quotient = _AllocateGPR(false);
  Ref Remainder = _AllocateGPR(false);
  _Div(Size, Dividend, Invalid(), SafeDivisor, Quotient, Remainder);
  Ref Negated = _Neg(Size, Dividend);
  Ref NonZero = _Select(Size, Size, CondClass::EQ, Divisor, MinusOne, Negated, Quotient);
  StoreReg(Bits(Word, 4, 0), Is64, _Select(Size, Size, CondClass::EQ, Divisor, Zero, Zero, NonZero));
  return true;
}

bool IRBuilder::ShiftVariable(uint32_t Word, IROps Op) {
  const bool Is64 = Bit(Word, 31);
  const auto Size = SizeFor(Is64);
  Ref Src = LoadX(Bits(Word, 9, 5));
  // The shift is Rm MOD datasize; the IR shift ops mask the count to 5 or 6 bits.
  Ref Amount = LoadX(Bits(Word, 20, 16));
  Ref Result {};
  switch (Op) {
  case OP_LSHL: Result = _Lshl(Size, Src, Amount); break;
  case OP_LSHR: Result = _Lshr(Size, Src, Amount); break;
  case OP_ASHR: Result = _Ashr(Size, Src, Amount); break;
  default: Result = _Ror(Size, Src, Amount); break;
  }
  StoreReg(Bits(Word, 4, 0), Is64, Result);
  return true;
}

bool IRBuilder::LSLV(uint32_t Word) {
  return ShiftVariable(Word, OP_LSHL);
}
bool IRBuilder::LSRV(uint32_t Word) {
  return ShiftVariable(Word, OP_LSHR);
}
bool IRBuilder::ASRV(uint32_t Word) {
  return ShiftVariable(Word, OP_ASHR);
}
bool IRBuilder::RORV(uint32_t Word) {
  return ShiftVariable(Word, OP_ROR);
}

// ---------------------------------------------------------------------------
// Data processing (1 source)
// ---------------------------------------------------------------------------

bool IRBuilder::RBIT_int(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  StoreReg(Bits(Word, 4, 0), Is64, _Rbit(SizeFor(Is64), LoadX(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::REV16_int(uint32_t Word) {
  // Swap the bytes of every 16-bit word: ((x & 0x00ff..) << 8) | ((x >> 8) & 0x00ff..).
  const bool Is64 = Bit(Word, 31);
  const auto Size = SizeFor(Is64);
  Ref Src = LoadX(Bits(Word, 9, 5));
  Ref Mask = Constant(Is64 ? 0x00FF'00FF'00FF'00FFULL : 0x00FF'00FFULL);
  Ref Low = _Lshl(Size, _And(Size, Src, Mask), Constant(8));
  Ref High = _And(Size, _Lshr(Size, Src, Constant(8)), Mask);
  StoreReg(Bits(Word, 4, 0), Is64, _Or(Size, Low, High));
  return true;
}

bool IRBuilder::REV(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const bool Opc0 = Bit(Word, 10);
  // sf=1 opc=11 is REV (64-bit), sf=0 opc=10 is REV (32-bit). sf=1 opc=10 is
  // REV32 (its own, more specific entry); sf=0 opc=11 is unallocated.
  if (Is64 != Opc0) {
    return false;
  }
  StoreReg(Bits(Word, 4, 0), Is64, _Rev(SizeFor(Is64), LoadX(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::REV32_int(uint32_t Word) {
  // Byte-reverse each 32-bit half: bswap64 then rotate by 32.
  Ref Swapped = _Rev(OpSize::i64Bit, LoadX(Bits(Word, 9, 5)));
  StoreX(Bits(Word, 4, 0), _Ror(OpSize::i64Bit, Swapped, Constant(32)));
  return true;
}

bool IRBuilder::CLZ_int(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  StoreReg(Bits(Word, 4, 0), Is64, _CountLeadingZeroes(SizeFor(Is64), LoadX(Bits(Word, 9, 5))));
  return true;
}

// `rbit Rd,Rn ; clz Rd,Rd` is CTZ, and it is what GCC and clang emit for
// __builtin_ctz on every AArch64 revision before FEAT_CSSC. Lowered as two
// independent ops it costs the whole bit-reversal (a SWAR chain, or six vector
// instructions) plus a cntlz; as one FindTrailingZeroes it is a single cnttzd /
// cnttzw on ISA 3.0 and four XER-safe instructions without it. glibc's AArch64
// strlen, strnlen, memchr, strchr, strchrnul, strcpy and strrchr all end in
// this pair, so every call to one of them pays it once.
//
// WHY ELIDING THE RBIT'S REGISTER WRITE IS SOUND. The match requires the CLZ to
// both read and write the very register the RBIT wrote, so the RBIT's value is
// dead the instant the CLZ retires, and the two are adjacent in one decoded
// block. The only way a guest could observe the intermediate is at an exception
// or signal taken between them, and there is none to be had: neither
// instruction can fault, asynchronous signals are deferred to a drain point (a
// unit entry or a branch's backward leg -- see sigpreempt.c), and an in-unit
// branch target starts a decoded block of its own, so a CLZ that anything
// jumps to is never this function's Index+1.
size_t IRBuilder::TryFuseRbitClz(const Decoder::DecodedBlocks& Block, size_t Index) {
  if (Index + 2 > Block.NumInstructions) {
    return 0;
  }
  const auto& Rbit = Block.DecodedInstructions[Index];
  const auto& Clz = Block.DecodedInstructions[Index + 1];
  if (!Rbit.Matcher || !Clz.Matcher || strcmp(Rbit.Matcher->Name, "RBIT_int") != 0 ||
      strcmp(Clz.Matcher->Name, "CLZ_int") != 0) {
    return 0;
  }
  // Same operand width, or the two do not compose into one CTZ at all.
  const bool Is64 = Bit(Rbit.Word, 31);
  if (Bit(Clz.Word, 31) != Is64) {
    return 0;
  }
  // Rd of the RBIT must be both operands of the CLZ. That is what makes the
  // RBIT's write dead. Rd = 31 is the discarding form and not worth a case.
  const uint32_t Rd = Bits(Rbit.Word, 4, 0);
  if (Rd == 31 || Bits(Clz.Word, 9, 5) != Rd || Bits(Clz.Word, 4, 0) != Rd) {
    return 0;
  }

  // Core.cpp emitted the marker for the RBIT; the CLZ needs its own so a signal
  // resuming at either PC still finds an instruction boundary there.
  _GuestOpcode(Clz.PC - Entry);
  // LoadGPRSlot's value cache is keyed on CurrentPC, which the caller has not
  // advanced yet (it advances in TranslateInstruction, which is not running).
  CurrentPC = Rbit.PC;
  StoreReg(Rd, Is64, _FindTrailingZeroes(SizeFor(Is64), LoadX(Bits(Rbit.Word, 9, 5))));
  return 2;
}

bool IRBuilder::CLS_int(uint32_t Word) {
  // CLS(x) = CLZ(x ^ (x ASR 1)) - 1: bit i of the xor is x[i] ^ x[i+1], and
  // its top bit is always clear.
  const bool Is64 = Bit(Word, 31);
  const auto Size = SizeFor(Is64);
  Ref Src = LoadX(Bits(Word, 9, 5));
  Ref Xored = _Xor(Size, Src, _Ashr(Size, Src, Constant(1)));
  StoreReg(Bits(Word, 4, 0), Is64, _Sub(Size, _CountLeadingZeroes(Size, Xored), Constant(1)));
  return true;
}

// ---------------------------------------------------------------------------
// Logical and add/sub (shifted and extended register), with carry
// ---------------------------------------------------------------------------

bool IRBuilder::LogicalShifted(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const uint32_t Opc = Bits(Word, 30, 29);
  const uint32_t ShiftType = Bits(Word, 23, 22);
  const bool Invert = Bit(Word, 21);
  const uint32_t Amount = Bits(Word, 15, 10);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rd = Bits(Word, 4, 0);
  if (!Is64 && Amount >= 32) {
    return false;
  }
  const auto Size = SizeFor(Is64);

  Ref Operand = ShiftReg(LoadX(Bits(Word, 20, 16)), ShiftType, Amount, Is64);
  if (Invert) {
    Operand = _Not(OpSize::i64Bit, Operand);
  }
  Ref Src = LoadX(Rn);

  switch (Opc) {
  case 0b00: StoreReg(Rd, Is64, _And(Size, Src, Operand)); break;
  case 0b01: StoreReg(Rd, Is64, _Or(Size, Src, Operand)); break;
  case 0b10: StoreReg(Rd, Is64, _Xor(Size, Src, Operand)); break;
  default: StoreReg(Rd, Is64, _AndWithFlags(Size, Src, Operand)); break;
  }
  return true;
}

bool IRBuilder::AddSubShifted(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const bool IsSub = Bit(Word, 30);
  const bool SetFlags = Bit(Word, 29);
  const uint32_t ShiftType = Bits(Word, 23, 22);
  const uint32_t Amount = Bits(Word, 15, 10);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rd = Bits(Word, 4, 0);
  if (ShiftType == 0b11 || (!Is64 && Amount >= 32)) {
    return false;
  }
  const auto Size = SizeFor(Is64);

  Ref Operand = ShiftReg(LoadX(Bits(Word, 20, 16)), ShiftType, Amount, Is64);
  Ref Src = LoadX(Rn);

  if (!SetFlags) {
    StoreReg(Rd, Is64, IsSub ? _Sub(Size, Src, Operand) : _Add(Size, Src, Operand));
  } else if (Rd == 31) {
    IsSub ? _SubNZCV(Size, Src, Operand) : _AddNZCV(Size, Src, Operand);
  } else {
    StoreReg(Rd, Is64, IsSub ? _SubWithFlags(Size, Src, Operand) : _AddWithFlags(Size, Src, Operand));
  }
  return true;
}

bool IRBuilder::AddSubExtended(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const bool IsSub = Bit(Word, 30);
  const bool SetFlags = Bit(Word, 29);
  const uint32_t Option = Bits(Word, 15, 13);
  const uint32_t Shift = Bits(Word, 12, 10);
  const uint32_t Rn = Bits(Word, 9, 5);
  const uint32_t Rd = Bits(Word, 4, 0);
  if (Shift > 4 || Bits(Word, 23, 22) != 0) {
    return false;
  }
  const auto Size = SizeFor(Is64);

  Ref Operand = ExtendReg(LoadX(Bits(Word, 20, 16)), Option, Shift);
  Ref Src = LoadXSP(Rn);

  if (!SetFlags) {
    StoreRegSP(Rd, Is64, IsSub ? _Sub(Size, Src, Operand) : _Add(Size, Src, Operand));
  } else if (Rd == 31) {
    IsSub ? _SubNZCV(Size, Src, Operand) : _AddNZCV(Size, Src, Operand);
  } else {
    StoreReg(Rd, Is64, IsSub ? _SubWithFlags(Size, Src, Operand) : _AddWithFlags(Size, Src, Operand));
  }
  return true;
}

bool IRBuilder::AddSubCarry(uint32_t Word, bool IsSub, bool SetFlags) {
  // The IR carry ops use ARM carry polarity: Adc adds C, Sbb computes
  // Src1 + ~Src2 + C (Src1 - Src2 - !C), and both flag forms produce ARM C.
  const bool Is64 = Bit(Word, 31);
  const auto Size = SizeFor(Is64);
  Ref Src1 = LoadX(Bits(Word, 9, 5));
  Ref Src2 = LoadX(Bits(Word, 20, 16));
  const uint32_t Rd = Bits(Word, 4, 0);

  Ref Result {};
  if (SetFlags) {
    Result = IsSub ? _SbbWithFlags(Size, Src1, Src2) : _AdcWithFlags(Size, Src1, Src2);
  } else {
    // Adc and Sbb are value-only: they read C and leave NZCV alone.
    Result = IsSub ? _Sbb(Size, Src1, Src2) : _Adc(Size, Src1, Src2);
  }
  StoreReg(Rd, Is64, Result);
  return true;
}

bool IRBuilder::ADC(uint32_t Word) {
  return AddSubCarry(Word, false, false);
}
bool IRBuilder::ADCS(uint32_t Word) {
  return AddSubCarry(Word, false, true);
}
bool IRBuilder::SBC(uint32_t Word) {
  return AddSubCarry(Word, true, false);
}
bool IRBuilder::SBCS(uint32_t Word) {
  return AddSubCarry(Word, true, true);
}

// ---------------------------------------------------------------------------
// Conditional compare and select
// ---------------------------------------------------------------------------

bool IRBuilder::CondCompare(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const bool IsSub = Bit(Word, 30);
  const bool IsImm = Bit(Word, 11);
  const uint32_t Cond = Bits(Word, 15, 12);
  const uint8_t FalseNZCV = Bits(Word, 3, 0);
  const auto Size = SizeFor(Is64);

  if (Bit(Word, 10) || Bit(Word, 4)) {
    return false;
  }

  Ref Src1 = LoadX(Bits(Word, 9, 5));
  Ref Src2 {};
  if (IsImm) {
    Src2 = Constant(Bits(Word, 20, 16));
  } else {
    Src2 = LoadX(Bits(Word, 20, 16));
  }

  // AL and NV always compare. The NZCV condition mapping has no AL entry.
  if (Cond >= 0b1110) {
    IsSub ? _SubNZCV(Size, Src1, Src2) : _AddNZCV(Size, Src1, Src2);
    return true;
  }

  const auto CC = MapCondition(Cond);
  IsSub ? _CondSubNZCV(Size, Src1, Src2, CC, FalseNZCV) : _CondAddNZCV(Size, Src1, Src2, CC, FalseNZCV);
  return true;
}

bool IRBuilder::CondSelect(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const bool Op = Bit(Word, 30);
  const uint32_t Cond = Bits(Word, 15, 12);
  const uint32_t Op2 = Bits(Word, 11, 10);
  const uint32_t Rd = Bits(Word, 4, 0);
  const auto Size = SizeFor(Is64);

  if (Bit(Word, 29) || Op2 > 1) {
    return false;
  }

  Ref TrueVal = LoadX(Bits(Word, 9, 5));
  Ref FalseVal = LoadX(Bits(Word, 20, 16));

  if (Cond >= 0b1110) {
    StoreReg(Rd, Is64, TrueVal);
    return true;
  }

  if (!Op && Op2 == 1) {
    FalseVal = _Add(Size, FalseVal, Constant(1)); // CSINC
  } else if (Op && Op2 == 0) {
    FalseVal = _Not(Size, FalseVal); // CSINV
  } else if (Op && Op2 == 1) {
    FalseVal = _Neg(Size, FalseVal); // CSNEG
  }

  StoreReg(Rd, Is64, _NZCVSelect(Size, MapCondition(Cond), TrueVal, FalseVal));
  return true;
}

// ---------------------------------------------------------------------------
// Data processing (3 source)
// ---------------------------------------------------------------------------

bool IRBuilder::MADD(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const auto Size = SizeFor(Is64);
  Ref Src1 = LoadX(Bits(Word, 9, 5));
  Ref Src2 = LoadX(Bits(Word, 20, 16));
  const uint32_t Ra = Bits(Word, 14, 10);
  // MUL is MADD with Ra = XZR and is the commoner of the two in every
  // reference binary, so keep it a plain multiply rather than a multiply-add
  // against a zero the backend would then have to fold back out.
  if (Ra == 31) {
    StoreReg(Bits(Word, 4, 0), Is64, _Mul(Size, Src1, Src2));
    return true;
  }
  StoreReg(Bits(Word, 4, 0), Is64, _MulAdd(Size, Src1, Src2, LoadX(Ra)));
  return true;
}

bool IRBuilder::MSUB(uint32_t Word) {
  const bool Is64 = Bit(Word, 31);
  const auto Size = SizeFor(Is64);
  Ref Product = _Mul(Size, LoadX(Bits(Word, 9, 5)), LoadX(Bits(Word, 20, 16)));
  StoreReg(Bits(Word, 4, 0), Is64, _Sub(Size, LoadX(Bits(Word, 14, 10)), Product));
  return true;
}

bool IRBuilder::MultiplyAddSubLong(uint32_t Word, bool IsSigned, bool IsSub) {
  Ref Src1 = LoadX(Bits(Word, 9, 5));
  Ref Src2 = LoadX(Bits(Word, 20, 16));
  Ref Product {};
  if (IsSigned) {
    // mullw IS the signed 32x32->64 multiply, so hand the backend SMull and let
    // DEF_OP(SMull) drop both sign-extensions. The unsigned direction has no
    // such instruction, and its Bfe pair is worth keeping as separate ops: the
    // high-zero elision passes can remove a Bfe(#32,#0) whose source is already
    // known zero-extended, which an opaque UMull could not.
    Product = _SMull(Src1, Src2);
  } else {
    Product = _Mul(OpSize::i64Bit, _Bfe(OpSize::i64Bit, 32, 0, Src1), _Bfe(OpSize::i64Bit, 32, 0, Src2));
  }
  const uint32_t Ra = Bits(Word, 14, 10);
  // SMULL/UMULL are these instructions with Ra = XZR: no addend, so no add.
  // (SMNEGL/UMNEGL keep the Sub against the zero, which lowers to a neg.)
  if (Ra == 31 && !IsSub) {
    StoreX(Bits(Word, 4, 0), Product);
    return true;
  }
  Ref Addend = LoadX(Ra);
  StoreX(Bits(Word, 4, 0), IsSub ? _Sub(OpSize::i64Bit, Addend, Product) : _Add(OpSize::i64Bit, Addend, Product));
  return true;
}

bool IRBuilder::SMADDL(uint32_t Word) {
  return MultiplyAddSubLong(Word, true, false);
}
bool IRBuilder::SMSUBL(uint32_t Word) {
  return MultiplyAddSubLong(Word, true, true);
}
bool IRBuilder::UMADDL(uint32_t Word) {
  return MultiplyAddSubLong(Word, false, false);
}
bool IRBuilder::UMSUBL(uint32_t Word) {
  return MultiplyAddSubLong(Word, false, true);
}

bool IRBuilder::SMULH(uint32_t Word) {
  StoreX(Bits(Word, 4, 0), _MulH(OpSize::i64Bit, LoadX(Bits(Word, 9, 5)), LoadX(Bits(Word, 20, 16))));
  return true;
}

bool IRBuilder::UMULH(uint32_t Word) {
  StoreX(Bits(Word, 4, 0), _UMulH(OpSize::i64Bit, LoadX(Bits(Word, 9, 5)), LoadX(Bits(Word, 20, 16))));
  return true;
}

} // namespace FEXCore::A64
