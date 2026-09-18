// SPDX-License-Identifier: MIT
//
// The CPU POWERarm presents to EL0 through MRS (DESIGN.md §4.5, §4.8).
//
// The model is the Raspberry Pi 5's Cortex-A76 r4p1, the differential-test
// reference machine. Values marked "Pi 5" were read there with MRS under
// Linux 6.18 (the kernel's sanitized EL0 view). Feature fields are then cut
// down to the M1 HWCAP profile, fp | asimd | cpuid: every feature register
// field that would imply another HWCAP bit reads as "not implemented". The
// profile has since grown by half precision and aes/pmull/sha1/sha2/crc32.
//
// Change these together with AT_HWCAP (Linux layer) and /proc/cpuinfo: a
// feature must be visible the same way through all three, and only once its
// instructions pass parity against the Pi.
//
// ONE DELIBERATE EXCEPTION to modelling the Pi: CNTFRQ_EL0, handled in
// TranslateBranchSystem.cpp rather than here, reports the real host timebase
// (512 MHz on POWER8+) and not the Pi's 54000000. CNTFRQ is a board property,
// not a CPU feature -- real arm64 hardware runs anywhere from 24 MHz to 1 GHz
// and every correct guest divides by whatever it reads -- so the value must
// describe the counter the guest actually gets from CNTVCT_EL0, which is
// `mftb`. Faking the Pi's number would force a multiply-shift on every counter
// read purely to keep the two self-consistent. A test that compares the two
// machines must compare elapsed time, never raw counter values or CNTFRQ.
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

// ID_AA64PFR0_EL1. Pi 5: 0x110011. EL0 and EL1 AArch64-only (1), FP=1 and
// AdvSIMD=1: implemented with half precision (fphp/asimdhp), as on the Pi.
inline constexpr uint64_t ID_AA64PFR0_EL1 = 0x0000000000110011;
// Pi 5: 0.
inline constexpr uint64_t ID_AA64PFR1_EL1 = 0;
// Pi 5: 0x6 (DebugVer only).
inline constexpr uint64_t ID_AA64DFR0_EL1 = 0x6;
// Pi 5: 0x0000100010211120 (AES, SHA1, SHA2, CRC32, Atomic, RDM, DP).
// Presented: AES=2 (AES and PMULL), SHA1=1, SHA2=1 (SHA-256 only), CRC32=1,
// Atomic=2.
//
// Atomic=2 is the only value the field takes besides 0, and it claims the
// whole of FEAT_LSE: LDADD/LDCLR/LDEOR/LDSET, LDSMAX/LDSMIN/LDUMAX/LDUMIN and
// SWP at all four widths, CASB/CASH/CAS, and CASP in both widths. All of those
// pass Pi parity (lse, lseminmax, lsecasp), which is what makes the claim
// honest -- do not set this field while any member of that set is missing,
// because a guest reading it is entitled to emit any of them.
inline constexpr uint64_t ID_AA64ISAR0_EL1 = 0x0000000000211120;
// Pi 5: 0x100001 (DPB, LRCPC). M1: none.
inline constexpr uint64_t ID_AA64ISAR1_EL1 = 0;
// Pi 5: 0x00000111FF000000 (translation granule fields). No HWCAP depends on it.
inline constexpr uint64_t ID_AA64MMFR0_EL1 = 0x00000111FF000000;

// FPCR bits EL0 can set. Pi 5 reads back 0x07C80000 after writing all-ones:
// AHP(26), DN(25), FZ(24), RMode(23:22) and FZ16(19), the same as here.
inline constexpr uint64_t FPCR_WRITABLE_MASK = 0x07C80000;
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
