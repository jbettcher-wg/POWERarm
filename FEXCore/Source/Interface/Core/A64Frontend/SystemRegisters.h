// SPDX-License-Identifier: MIT
//
// The CPU POWERarm presents to EL0 through MRS (DESIGN.md §4.5, §4.8).
//
// The model is the Raspberry Pi 5's Cortex-A76 r4p1, the differential-test
// reference machine. Values marked "Pi 5" were read there with MRS under
// Linux 6.18 (the kernel's sanitized EL0 view). Feature fields are then cut
// down to the M1 HWCAP profile, fp | asimd | cpuid: every feature register
// field that would imply another HWCAP bit reads as "not implemented".
//
// Change these together with AT_HWCAP (Linux layer) and /proc/cpuinfo: a
// feature must be visible the same way through all three, and only once its
// instructions pass parity against the Pi.
#pragma once

#include <cstdint>

namespace FEXCore::A64::SystemRegisters {

// Cortex-A76 r4p1: implementer 0x41 (Arm), variant 4, architecture 0xF,
// part 0xD0B, revision 1. Pi 5: 0x414FD0B1.
inline constexpr uint64_t MIDR_EL1 = 0x414FD0B1;
// Linux reports SYS_MPIDR_SAFE_VAL (bit 31 set, affinity 0) to EL0.
inline constexpr uint64_t MPIDR_EL1 = 0x80000000;
inline constexpr uint64_t REVIDR_EL1 = 0;

// Pi 5: 0x9444C004. IDC=1, DIC=0, DCacheLine=4 and ICacheLine=4 (64-byte
// lines, log2 of the size in words), ICachePOL=0b11. With DIC=0, code that
// flushes the instruction cache runs IC IVAU, which translates to a NOP.
inline constexpr uint64_t CTR_EL0 = 0x9444C004;
// Pi 5: 0x4. DZP=0, BlockSize=4: DC ZVA zeroes 64 bytes.
inline constexpr uint64_t DCZID_EL0 = 0x4;

// ID_AA64PFR0_EL1. Pi 5: 0x110011 (FP=1 and AdvSIMD=1 advertise half
// precision). M1: EL0 and EL1 AArch64-only (1), FP=0 and AdvSIMD=0
// (implemented, no FP16: fphp/asimdhp stay clear).
inline constexpr uint64_t ID_AA64PFR0_EL1 = 0x0000000000000011;
// Pi 5: 0.
inline constexpr uint64_t ID_AA64PFR1_EL1 = 0;
// Pi 5: 0x6 (DebugVer only).
inline constexpr uint64_t ID_AA64DFR0_EL1 = 0x6;
// Pi 5: 0x0000100010211120 (AES, SHA1, SHA2, CRC32, Atomic, RDM, DP). M1: none.
inline constexpr uint64_t ID_AA64ISAR0_EL1 = 0;
// Pi 5: 0x100001 (DPB, LRCPC). M1: none.
inline constexpr uint64_t ID_AA64ISAR1_EL1 = 0;
// Pi 5: 0x00000111FF000000 (translation granule fields). No HWCAP depends on it.
inline constexpr uint64_t ID_AA64MMFR0_EL1 = 0x00000111FF000000;

// FPCR bits EL0 can set. Pi 5 reads back 0x07C80000 after writing all-ones:
// AHP(26), DN(25), FZ(24), RMode(23:22) and FZ16(19). FZ16 is left out
// because the presented CPU has no FP16 (a deliberate difference from the Pi).
inline constexpr uint64_t FPCR_WRITABLE_MASK = 0x07C00000;
// FPSR bits EL0 can set. Pi 5: 0xF800009F (NZCV, QC, IDC, IXC..IOC).
inline constexpr uint64_t FPSR_WRITABLE_MASK = 0xF800009F;

// Linux's EL0 ID register view: CRm=0 holds MIDR (op2 0), MPIDR (5) and
// REVIDR (6), anything else there is undefined; CRm 2..7 read the sanitized
// value or zero for unallocated registers. Returns false where Linux delivers
// SIGILL. CRm=1 is rejected by the caller.
inline bool ReadIDRegister(uint32_t CRm, uint32_t Op2, uint64_t* Value) {
  if (CRm == 0) {
    switch (Op2) {
    case 0: *Value = MIDR_EL1; return true;
    case 5: *Value = MPIDR_EL1; return true;
    case 6: *Value = REVIDR_EL1; return true;
    default: return false;
    }
  }

  if (CRm < 2 || CRm > 7) {
    return false;
  }

  *Value = 0;
  if (CRm == 4 && Op2 == 0) {
    *Value = ID_AA64PFR0_EL1;
  } else if (CRm == 4 && Op2 == 1) {
    *Value = ID_AA64PFR1_EL1;
  } else if (CRm == 5 && Op2 == 0) {
    *Value = ID_AA64DFR0_EL1;
  } else if (CRm == 6 && Op2 == 0) {
    *Value = ID_AA64ISAR0_EL1;
  } else if (CRm == 6 && Op2 == 1) {
    *Value = ID_AA64ISAR1_EL1;
  } else if (CRm == 7 && Op2 == 0) {
    *Value = ID_AA64MMFR0_EL1;
  }
  return true;
}

} // namespace FEXCore::A64::SystemRegisters
