// SPDX-License-Identifier: MIT
//
// Self-checking: MRS/MSR against the CPU profile POWERarm presents
// (FEXCore/Source/Interface/Core/A64Frontend/SystemRegisters.h). Prints one
// PASS/FAIL line per check. Under POWERarm every check passes except
// `control-deliberately-wrong`, the positive control, which must FAIL.
// On the Pi the feature registers and the FPCR mask differ by design.
#include "common.S"

        .macro CHECK name, expected
        adr     x2, 90f
        ldr     x1, =\expected
        bl      check
        b       91f
90:     .asciz  "\name"
        .balign 4
91:
        .endm

        test_start

        // NZCV round trip for every flag combination; the low bits are RES0.
        mov     x19, #0
        mov     x22, #0
1:      lsl     x20, x19, #28
        orr     x21, x20, #0x0fffffff
        msr     nzcv, x21
        mrs     x0, nzcv
        eor     x0, x0, x20
        orr     x22, x22, x0
        add     x19, x19, #1
        cmp     x19, #16
        b.ne    1b
        mov     x0, x22
        CHECK   nzcv-roundtrip, 0

        // FPCR/FPSR writable masks.
        mrs     x19, fpcr
        mov     x0, #-1
        msr     fpcr, x0
        mrs     x0, fpcr
        CHECK   fpcr-writable-mask, 0x07c00000
        msr     fpcr, x19
        mrs     x0, fpcr
        CHECK   fpcr-restore, 0
        mov     x0, #-1
        msr     fpsr, x0
        mrs     x0, fpsr
        CHECK   fpsr-writable-mask, 0xf800009f
        msr     fpsr, xzr
        mrs     x0, fpsr
        CHECK   fpsr-clear, 0

        // TLS registers.
        ldr     x0, =0x1122334455667788
        msr     tpidr_el0, x0
        mov     x0, #0
        mrs     x0, tpidr_el0
        CHECK   tpidr_el0-roundtrip, 0x1122334455667788
        mrs     x0, tpidrro_el0
        CHECK   tpidrro_el0-initial, 0

        // Identification.
        mrs     x0, ctr_el0
        CHECK   ctr_el0, 0x9444c004
        mrs     x0, dczid_el0
        CHECK   dczid_el0, 0x4
        mrs     x0, midr_el1
        CHECK   midr_el1, 0x414fd0b1
        mrs     x0, mpidr_el1
        CHECK   mpidr_el1, 0x80000000
        mrs     x0, revidr_el1
        CHECK   revidr_el1, 0
        mrs     x0, id_aa64pfr0_el1
        CHECK   id_aa64pfr0_el1, 0x11
        mrs     x0, id_aa64pfr1_el1
        CHECK   id_aa64pfr1_el1, 0
        mrs     x0, id_aa64dfr0_el1
        CHECK   id_aa64dfr0_el1, 0x6
        mrs     x0, id_aa64isar0_el1
        CHECK   id_aa64isar0_el1, 0
        mrs     x0, id_aa64isar1_el1
        CHECK   id_aa64isar1_el1, 0
        mrs     x0, id_aa64mmfr0_el1
        CHECK   id_aa64mmfr0_el1, 0x00000111ff000000
        mrs     x0, S3_0_C0_C7_7
        CHECK   id-unallocated-reads-zero, 0
        mov     x9, #0x5a5a
        mrs     x9, S3_0_C0_C4_4
        mov     x0, x9
        CHECK   id_aa64zfr0_el1-zero, 0

        // Values reach context-backed registers too.
        mrs     x12, midr_el1
        mov     x0, x12
        CHECK   midr-into-x12, 0x414fd0b1
        mrs     x26, tpidr_el0
        mov     x0, x26
        CHECK   tpidr-into-x26, 0x1122334455667788

        // Positive control: a deliberately wrong expected value.
        mrs     x0, midr_el1
        CHECK   control-deliberately-wrong, 0x414fd0b2

        test_exit
        .ltorg
