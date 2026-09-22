// SPDX-License-Identifier: MIT
//
// A64 cryptographic extension and CRC32: AESE/AESD/AESMC/AESIMC,
// PMULL/PMULL2 (8- and 64-bit), SHA1C/SHA1M/SHA1P/SHA1H/SHA1SU0/SHA1SU1,
// SHA256H/SHA256H2/SHA256SU0/SHA256SU1, CRC32B-X and CRC32CB-CX.
//
// AES: lowered directly via dedicated vector crypto IR operations:
//   AESE(s, k)  = ShiftRows(SubBytes(s ^ k))       -> _VAESE
//   AESD(s, k)  = InvShiftRows(InvSubBytes(s ^ k)) -> _VAESD
//   AESMC(s)    = MixColumns(s)                    -> _VAESMC
//   AESIMC(s)   = InvMixColumns(s)                 -> _VAESImc
//
// SHA-512 (SHA512H/H2/SU0/SU1) and SHA-3 (EOR3/BCAX/RAX1/XAR) are not
// translated: the reference machine (Cortex-A76) lacks them, so they are
// neither advertised nor testable, and they raise SIGILL.
#include "Interface/Core/A64Frontend/IRBuilder.h"
#include "Interface/Core/A64Frontend/TranslateCommon.h"

namespace FEXCore::A64 {
using namespace FEXCore::IR;

// ---------------------------------------------------------------------------
// AES
// ---------------------------------------------------------------------------

bool IRBuilder::AESE(uint32_t Word) {
  const uint32_t Rd = Bits(Word, 4, 0);
  StoreV(Rd, _VAESE(LoadV(Rd), LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::AESD(uint32_t Word) {
  const uint32_t Rd = Bits(Word, 4, 0);
  StoreV(Rd, _VAESD(LoadV(Rd), LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::AESMC(uint32_t Word) {
  StoreV(Bits(Word, 4, 0), _VAESMC(LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::AESIMC(uint32_t Word) {
  StoreV(Bits(Word, 4, 0), _VAESImc(LoadV(Bits(Word, 9, 5))));
  return true;
}

// ---------------------------------------------------------------------------
// PMULL
// ---------------------------------------------------------------------------

bool IRBuilder::PMULL(uint32_t Word) {
  const bool Q = Bit(Word, 30);
  const uint32_t Size = Bits(Word, 23, 22);
  const auto RS = OpSize::i128Bit;
  Ref A = LoadV(Bits(Word, 9, 5));
  Ref B = LoadV(Bits(Word, 20, 16));
  Ref Result {};
  if (Size == 0) {
    Result = _VPMullB(RS, A, B, Q);
  } else if (Size == 3) {
    // PCLMUL selector bit 0 picks Src1's quadword, bit 4 Src2's.
    Result = _PCLMUL(RS, A, B, Q ? 0x11 : 0x00);
  } else {
    return false;
  }
  StoreV(Bits(Word, 4, 0), Result);
  return true;
}

// ---------------------------------------------------------------------------
// SHA-1 and SHA-256
// ---------------------------------------------------------------------------

bool IRBuilder::SHA1C(uint32_t Word) {
  const uint32_t Rd = Bits(Word, 4, 0);
  StoreV(Rd, _VSha1C(LoadV(Rd), LoadV(Bits(Word, 9, 5)), LoadV(Bits(Word, 20, 16))));
  return true;
}

bool IRBuilder::SHA1M(uint32_t Word) {
  const uint32_t Rd = Bits(Word, 4, 0);
  StoreV(Rd, _VSha1M(LoadV(Rd), LoadV(Bits(Word, 9, 5)), LoadV(Bits(Word, 20, 16))));
  return true;
}

bool IRBuilder::SHA1P(uint32_t Word) {
  const uint32_t Rd = Bits(Word, 4, 0);
  StoreV(Rd, _VSha1P(LoadV(Rd), LoadV(Bits(Word, 9, 5)), LoadV(Bits(Word, 20, 16))));
  return true;
}

bool IRBuilder::SHA1H(uint32_t Word) {
  StoreV(Bits(Word, 4, 0), _VSha1H(LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::SHA1SU0(uint32_t Word) {
  // W = Vd (w0-w3), Vn (w4-w7), Vm (w8-w11): lane i = w[i] ^ w[i+2] ^ w[i+8],
  // i.e. low half d.lo ^ d.hi ^ m.lo, high half d.hi ^ n.lo ^ m.hi.
  const auto RS = OpSize::i128Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref D = LoadV(Rd);
  Ref N = LoadV(Bits(Word, 9, 5));
  Ref M = LoadV(Bits(Word, 20, 16));
  // Shifted = [d.hi : n.lo] as a 128-bit value (low half d.hi, high half n.lo).
  Ref Shifted = _VInsElement(RS, OpSize::i64Bit, 1, 0, _VDupElement(RS, OpSize::i64Bit, D, 1), N);
  StoreV(Rd, _VXor(RS, RS, _VXor(RS, RS, D, Shifted), M));
  return true;
}

bool IRBuilder::SHA1SU1(uint32_t Word) {
  const uint32_t Rd = Bits(Word, 4, 0);
  StoreV(Rd, _VSha1SU1(LoadV(Rd), LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::SHA256H(uint32_t Word) {
  const uint32_t Rd = Bits(Word, 4, 0);
  StoreV(Rd, _VSha256H(LoadV(Rd), LoadV(Bits(Word, 9, 5)), LoadV(Bits(Word, 20, 16))));
  return true;
}

bool IRBuilder::SHA256H2(uint32_t Word) {
  const uint32_t Rd = Bits(Word, 4, 0);
  StoreV(Rd, _VSha256H2(LoadV(Rd), LoadV(Bits(Word, 9, 5)), LoadV(Bits(Word, 20, 16))));
  return true;
}

bool IRBuilder::SHA256SU0(uint32_t Word) {
  const uint32_t Rd = Bits(Word, 4, 0);
  StoreV(Rd, _VSha256U0(LoadV(Rd), LoadV(Bits(Word, 9, 5))));
  return true;
}

bool IRBuilder::SHA256SU1(uint32_t Word) {
  // A64: d[0] += s1(m[2]) + n[1]; d[1] += s1(m[3]) + n[2];
  //      d[2] += s1(d'[0]) + n[3]; d[3] += s1(d'[1]) + m[0]   (d' the updated lanes)
  // VSha256U1(X, Y) = [X1 + s1(Y2), X2 + s1(Y3), X3 + s1(r0), Y0 + s1(r1)], so
  // with D' = d + [n1, n2, n3, m0], X = [-, D'0, D'1, D'2] and Y = [D'3, -, m2, m3].
  const auto RS = OpSize::i128Bit;
  const auto ES = OpSize::i32Bit;
  const uint32_t Rd = Bits(Word, 4, 0);
  Ref D = LoadV(Rd);
  Ref N = LoadV(Bits(Word, 9, 5));
  Ref M = LoadV(Bits(Word, 20, 16));
  Ref Addend = _VInsElement(RS, ES, 3, 0, _VExtr(RS, OpSize::i8Bit, N, N, 4), M);
  Ref DPrime = _VAdd(RS, ES, D, Addend);
  Ref X = _VExtr(RS, OpSize::i8Bit, DPrime, DPrime, 12);
  Ref Y = _VInsElement(RS, ES, 0, 3, M, DPrime);
  StoreV(Rd, _VSha256U1(X, Y));
  return true;
}

// ---------------------------------------------------------------------------
// CRC32
// ---------------------------------------------------------------------------

bool IRBuilder::CRC32Common(uint32_t Word, bool Castagnoli) {
  const bool Sf = Bit(Word, 31);
  const uint32_t Sz = Bits(Word, 11, 10);
  if (Sf != (Sz == 3)) {
    return false;
  }
  const auto SrcSize = IR::SizeToOpSize(1U << Sz);
  Ref Value = Sf ? LoadX(Bits(Word, 20, 16)) : ZeroExtend32(LoadX(Bits(Word, 20, 16)));
  Ref Result = _CRC32(LoadX(Bits(Word, 9, 5)), Value, SrcSize, Castagnoli);
  StoreW(Bits(Word, 4, 0), Result);
  return true;
}

bool IRBuilder::CRC32(uint32_t Word) { return CRC32Common(Word, false); }
bool IRBuilder::CRC32C(uint32_t Word) { return CRC32Common(Word, true); }

} // namespace FEXCore::A64
