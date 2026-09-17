// SPDX-License-Identifier: MIT
//
// Exhaustive 8-bit operand sweep of the flag-setting arithmetic, placed at the
// top of the 32-bit and 64-bit operand so carry, overflow, sign and zero all
// sit on the real boundary. Each (a, b) pair runs every operation; result and
// NZCV are folded into a hash printed per operation family. Differential:
// compared against the Pi.
//
// Low operand bits are a fixed filler so Z depends on more than the top byte.
#include "common.S"

        // HASH value, flags: x28 = rotl(x28 ^ value, 7) + flags
        .macro HASH value
        mrs     x17, nzcv
        eor     x28, x28, \value
        ror     x28, x28, #57
        add     x28, x28, x17
        .endm

        // SWEEP body: runs \body for a (x19) and b (x20) in 0..255, with
        // x21/x22 = a, b at the 64-bit top byte and w23/w24 at the 32-bit top byte.
        .macro SWEEP_BEGIN
        mov     x28, #0
        mov     x19, #0
10:     mov     x20, #0
11:     lsl     x21, x19, #56
        lsl     x22, x20, #56
        lsl     w23, w19, #24
        lsl     w24, w20, #24
        // Filler: the same low bits for both operands in half the cases.
        tst     x19, #1
        b.eq    12f
        orr     x21, x21, #0x00ff000000000000
        orr     x22, x22, #0x00ff000000000000
        orr     w23, w23, #0x00800000
        orr     w24, w24, #0x00800000
12:
        .endm

        .macro SWEEP_END
        add     x20, x20, #1
        cmp     x20, #256
        b.ne    11b
        add     x19, x19, #1
        cmp     x19, #256
        b.ne    10b
        mov     x0, x28
        bl      dump
        .endm

        test_start

        SWEEP_BEGIN
        adds    x0, x21, x22
        HASH    x0
        adds    w0, w23, w24
        HASH    x0
        SWEEP_END

        SWEEP_BEGIN
        subs    x0, x21, x22
        HASH    x0
        subs    w0, w23, w24
        HASH    x0
        cmp     x21, x22
        HASH    xzr
        cmn     w23, w24
        HASH    xzr
        SWEEP_END

        SWEEP_BEGIN
        // Carry in 0 then 1.
        msr     nzcv, xzr
        adcs    x0, x21, x22
        HASH    x0
        mov     x1, #0x20000000
        msr     nzcv, x1
        adcs    x0, x21, x22
        HASH    x0
        msr     nzcv, xzr
        adcs    w0, w23, w24
        HASH    x0
        mov     x1, #0x20000000
        msr     nzcv, x1
        adcs    w0, w23, w24
        HASH    x0
        SWEEP_END

        SWEEP_BEGIN
        msr     nzcv, xzr
        sbcs    x0, x21, x22
        HASH    x0
        mov     x1, #0x20000000
        msr     nzcv, x1
        sbcs    x0, x21, x22
        HASH    x0
        msr     nzcv, xzr
        sbcs    w0, w23, w24
        HASH    x0
        mov     x1, #0x20000000
        msr     nzcv, x1
        sbcs    w0, w23, w24
        HASH    x0
        SWEEP_END

        SWEEP_BEGIN
        ands    x0, x21, x22
        HASH    x0
        bics    w0, w23, w24
        HASH    x0
        // Conditional compares on the flags of the previous op.
        ccmp    x21, x22, #0xa, mi
        HASH    xzr
        ccmn    w23, w24, #0x5, ne
        HASH    xzr
        subs    x0, x21, x22
        ccmp    w23, #17, #0x3, cs
        HASH    xzr
        adds    x0, x21, x21
        ccmn    x22, #31, #0xc, vs
        HASH    xzr
        SWEEP_END

        SWEEP_BEGIN
        // Every condition consumer after a SUBS.
        subs    x0, x21, x22
        cset    x1, eq
        cset    x2, cs
        cset    x3, mi
        cset    x4, vs
        cset    x5, hi
        cset    x6, ge
        cset    x7, gt
        cset    x8, ls
        lsl     x2, x2, #1
        lsl     x3, x3, #2
        lsl     x4, x4, #3
        lsl     x5, x5, #4
        lsl     x6, x6, #5
        lsl     x7, x7, #6
        lsl     x8, x8, #7
        orr     x1, x1, x2
        orr     x1, x1, x3
        orr     x1, x1, x4
        orr     x1, x1, x5
        orr     x1, x1, x6
        orr     x1, x1, x7
        orr     x1, x1, x8
        HASH    x1
        subs    w0, w23, w24
        csinc   w1, w23, w24, lt
        HASH    x1
        csneg   x1, x21, x22, le
        HASH    x1
        SWEEP_END

        test_exit
        .ltorg
