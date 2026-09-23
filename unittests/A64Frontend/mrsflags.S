// SPDX-License-Identifier: MIT
//
// Reading a system register must not disturb NZCV.
//
// On the ppc64le backend the guest's packed N and Z live in CR0 (N = CR0.LT,
// Z = CR0.EQ). DEF_OP(LoadFPSR) used to isolate the host VSCR saturation bit
// with `andi.`, and PowerPC's `andi.` has no non-record form -- it always
// writes CR0. So every `MRS Xt, FPSR` overwrote the guest's N and Z, and a
// sequence like `cmp x0, #1 / mrs x1, fpsr / b.eq` branched on flags the
// emulator had rewritten underneath it.
//
// Both saturation states have to be covered, because the clobber differs:
// with FPSR.QC clear the AND result was zero, so the guest saw Z=1, N=0; with
// QC set it was non-zero, so Z=0, N=0. Each branch leg therefore appears
// twice, once per QC state, and each appears in both its taken and its
// not-taken form.
//
// Self-checking: one PASS/FAIL line per check. Differential all the same --
// every line must match the Pi byte for byte.
#include "common.S"

        // One check: X0 = value, \expected = what the Pi produces.
        .macro CHECK name, expected
        adr     x2, 90f
        ldr     x1, =\expected
        bl      check
        b       91f
90:     .asciz  "\name"
        .balign 4
91:
        .endm

        // With NZCV already set, read FPSR and branch on \cc in the same
        // block. X0 = 1 if the branch was taken, 0 if it was not.
        .macro LEG cc
        mrs     x4, fpsr
        mov     x0, #0
        b.\cc   80f
        b       81f
80:     mov     x0, #1
81:
        .endm

        test_start

        // ------------------------------------------------------------------
        // FPSR.QC clear. The old clobber forced Z=1, N=0 here.
        // ------------------------------------------------------------------
        msr     fpsr, xzr

        // Z=0 (2 != 1): b.eq must NOT be taken. The bug took it.
        mov     x3, #2
        cmp     x3, #1
        LEG     eq
        CHECK   qc0-ne-beq-not-taken, 0

        // Z=1 (1 == 1): b.eq must be taken.
        mov     x3, #1
        cmp     x3, #1
        LEG     eq
        CHECK   qc0-eq-beq-taken, 1

        // N=1 (0 - 1 is negative): b.mi must be taken. The bug cleared N.
        mov     x3, #0
        cmp     x3, #1
        LEG     mi
        CHECK   qc0-neg-bmi-taken, 1

        // N=0: b.mi must not be taken.
        mov     x3, #2
        cmp     x3, #1
        LEG     mi
        CHECK   qc0-pos-bmi-not-taken, 0

        // The whole NZCV word across the read, not just what one branch sees.
        mov     x3, #0
        cmp     x3, #1                  // N=1 Z=0 C=0 V=0
        mrs     x4, fpsr
        mrs     x0, nzcv
        CHECK   qc0-nzcv-N, 0x80000000

        mov     x3, #1
        cmp     x3, #1                  // N=0 Z=1 C=1 V=0
        mrs     x4, fpsr
        mrs     x0, nzcv
        CHECK   qc0-nzcv-ZC, 0x60000000

        // C and V live in XER on this backend rather than CR0; they have to
        // cross the read too.
        ldr     x3, =0x8000000000000000
        adds    x5, x3, x3              // N=0 Z=1 C=1 V=1
        mrs     x4, fpsr
        mrs     x0, nzcv
        CHECK   qc0-nzcv-ZCV, 0x70000000

        // And the read still returns what it is for.
        mrs     x0, fpsr
        CHECK   qc0-fpsr-value, 0

        // Flags set, FPSR read, then a block boundary before the branch --
        // the path where NZCV goes out to memory and comes back.
        mov     x3, #2
        cmp     x3, #1                  // Z=0
        mrs     x4, fpsr
        adr     x7, 70f
        br      x7
70:     mov     x0, #0
        b.eq    71f
        b       72f
71:     mov     x0, #1
72:     CHECK   qc0-crossblock-beq-not-taken, 0

        // ------------------------------------------------------------------
        // FPSR.QC set. A saturating SIMD op raises bit 27, which is the bit
        // DEF_OP(LoadFPSR) merges in from the host VSCR. The old clobber
        // forced Z=0, N=0 here, so the opposite legs are the telling ones.
        // ------------------------------------------------------------------
        movi    v0.16b, #0x7f
        sqadd   v0.16b, v0.16b, v0.16b  // 0x7f + 0x7f saturates: QC <- 1
        mrs     x0, fpsr
        CHECK   qc1-fpsr-value, 0x08000000

        // Z=1: b.eq must be taken. The bug cleared Z and skipped it.
        mov     x3, #1
        cmp     x3, #1
        LEG     eq
        CHECK   qc1-eq-beq-taken, 1

        // Z=0: b.eq must still not be taken.
        mov     x3, #2
        cmp     x3, #1
        LEG     eq
        CHECK   qc1-ne-beq-not-taken, 0

        // N=1: b.mi must be taken. The bug cleared N.
        mov     x3, #0
        cmp     x3, #1
        LEG     mi
        CHECK   qc1-neg-bmi-taken, 1

        mov     x3, #1
        cmp     x3, #1                  // N=0 Z=1 C=1 V=0
        mrs     x4, fpsr
        mrs     x0, nzcv
        CHECK   qc1-nzcv-ZC, 0x60000000

        // Writing FPSR clears QC again, and the write is as constrained as
        // the read: StoreFPSR must use the Rc=0 spellings throughout.
        mov     x3, #1
        cmp     x3, #1                  // N=0 Z=1 C=1 V=0
        msr     fpsr, xzr
        mrs     x0, nzcv
        CHECK   qc-clear-nzcv, 0x60000000
        mrs     x0, fpsr
        CHECK   qc-cleared, 0

        // ------------------------------------------------------------------
        // The other system registers EL0 can read must be as harmless. None
        // of their lowerings may record either.
        // ------------------------------------------------------------------
        mov     x3, #2
        cmp     x3, #1                  // N=0 Z=0 C=1 V=0
        mrs     x4, fpcr
        mrs     x5, tpidr_el0
        mrs     x6, ctr_el0
        mrs     x7, dczid_el0
        mrs     x9, cntvct_el0
        mrs     x10, midr_el1
        mrs     x0, nzcv
        CHECK   other-sysregs-nzcv, 0x20000000

        mov     x3, #0
        cmp     x3, #1                  // N=1 Z=0 C=0 V=0
        mrs     x4, fpcr
        mrs     x5, tpidr_el0
        mrs     x6, ctr_el0
        mrs     x0, nzcv
        CHECK   other-sysregs-nzcv-N, 0x80000000

        test_exit
        .ltorg
