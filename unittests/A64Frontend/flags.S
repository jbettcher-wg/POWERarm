// SPDX-License-Identifier: MIT
//
// NZCV must survive block exits, block links, a syscall and a taken
// conditional branch. Differential: compared against the Pi.
#include "common.S"

        .macro record
        // Materialise all four flags into X5 without disturbing them.
        cset    x10, mi
        cset    x11, eq
        cset    x12, cs
        cset    x13, vs
        lsl     x10, x10, #3
        orr     x5, x10, x11, lsl #2
        orr     x5, x5, x12, lsl #1
        orr     x5, x5, x13
        bl      dump
        .endm

        test_start

        // Flags from SUBS, consumed after an unconditional branch.
        ldr     x0, =0x8000000000000000
        subs    x1, x0, #1
        b       1f
1:      record

        // Flags from ADDS (carry + overflow), consumed after a syscall.
        ldr     x0, =0x8000000000000000
        adds    x1, x0, x0
        mov     x19, x1
        mov     x0, #1
        adr     x1, 9f
        mov     x2, #0
        mov     x8, #64
        adds    x3, x19, x19
        svc     #0
        record

        // Flags from W CMP, consumed after a register branch.
        mov     w0, #5
        cmp     w0, #6
        adr     x7, 2f
        br      x7
2:      record

        // Flags from ANDS, consumed after a taken B.cond and a BL.
        ldr     x0, =0xf000000000000000
        ands    x1, x0, x0
        b.mi    3f
        mov     x6, #0xbad
3:      bl      4f
        record
        b       5f
4:      ret
5:
        // Flags survive a loop of many blocks.
        mov     x0, #100
        cmp     x0, x0
6:      mov     x1, #0
        cbz     x1, 7f
7:      sub     x0, x0, #1
        cbnz    x0, 6b
        record

        test_exit
9:      .ascii  "."
        .ltorg
