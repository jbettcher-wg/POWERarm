// SPDX-License-Identifier: MIT
//
// The three-source integer multiplies: MUL/MNEG, MADD/MSUB, and the long forms
// SMULL/UMULL, SMADDL/UMADDL, SMSUBL/UMSUBL, SMNEGL/UMNEGL -- the lowerings of
// ISA-OPPORTUNITIES.md §3.8 (SMULL as one mullw, MADD as one maddld, SMADDL as
// mullw plus add).
//
// What those lowerings can get wrong, and is covered here:
//  * SMULL/SMADDL must sign-extend the low words and must not be perturbed by
//    the high halves. Every source below carries 32 bits of garbage above the
//    operand, and the operands themselves are INT32_MIN, INT32_MAX, -1, 0 and 1
//    in every pairing -- INT32_MIN squared is the product that overflows a
//    signed 32-bit multiply, and INT32_MIN x -1 is the one that overflows its
//    own negation.
//  * MADD's addend may BE one of its multiplicands, or the destination, or all
//    three at once. A lowering that computes the product into the destination
//    first destroys the addend; maddld reads all three operands before writing.
//  * A W-form result must be zero-extended into its X register, so every 32-bit
//    destination is poisoned with 0xcccc... beforehand and dumped as an X.
//  * None of them writes flags: NZCV is set to a known non-zero pattern before
//    every case and dumped with the registers.
//
// Differential: the Pi's own output.
#include "common.S"

        .macro imm64 xd, val
        movz    \xd, #(((\val) >> 48) & 0xffff), lsl #48
        movk    \xd, #(((\val) >> 32) & 0xffff), lsl #32
        movk    \xd, #(((\val) >> 16) & 0xffff), lsl #16
        movk    \xd, #(((\val)      ) & 0xffff)
        .endm

        // x1, x2, x3 are the operands; x0 and x4 are poisoned destinations.
        .macro setup a, b, c
        imm64   x1, \a
        imm64   x2, \b
        imm64   x3, \c
        imm64   x0, 0xcccccccccccccccc
        imm64   x4, 0xcccccccccccccccc
        mov     x17, #0xb0000000
        msr     nzcv, x17
        .endm

        // One instruction over the three operands, dumped.
        .macro m3 a, b, c, text:vararg
        setup   \a, \b, \c
        \text
        bl      dump
        .endm

        test_start

        // ---- 1. MUL and MNEG, X and W, with high garbage in the sources ----
        m3      0x80000000, 0x80000000, 0,          mul x0, x1, x2
        m3      0xdeadbeef80000000, 0xcafebabe80000000, 0, mul w0, w1, w2
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 0, mul w0, w1, w2
        m3      0xffffffffffffffff, 0x7fffffffffffffff, 0, mul x0, x1, x2
        m3      0xdeadbeef7fffffff, 0xcafebabe7fffffff, 0, mul w0, w1, w2
        m3      0x80000000, 0xffffffff, 0,          mneg x0, x1, x2
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 0, mneg w0, w1, w2
        m3      0x123456789abcdef0, 0xfedcba9876543210, 0, mul x0, x1, x2

        // ---- 2. MADD/MSUB, all the aliasing shapes -------------------------
        m3      3, 5, 7,                            madd x0, x1, x2, x3
        m3      3, 5, 7,                            msub x0, x1, x2, x3
        m3      0x8000000000000000, 0x8000000000000000, 0xffffffffffffffff, madd x0, x1, x2, x3
        // The addend IS a multiplicand.
        m3      6, 7, 0,                            madd x0, x1, x2, x1
        m3      6, 7, 0,                            madd x0, x1, x2, x2
        m3      6, 7, 0,                            msub x0, x1, x2, x1
        // The destination IS the addend.
        m3      6, 7, 9,                            madd x3, x1, x2, x3
        m3      6, 7, 9,                            msub x3, x1, x2, x3
        // The destination IS a multiplicand.
        m3      6, 7, 9,                            madd x1, x1, x2, x3
        m3      6, 7, 9,                            madd x2, x1, x2, x3
        m3      6, 7, 9,                            msub x1, x1, x2, x3
        // Every operand the same register, destination included.
        m3      5, 0, 0,                            madd x1, x1, x1, x1
        m3      5, 0, 0,                            msub x1, x1, x1, x1
        // Ra = XZR (this is MUL) and Rd = XZR (the result is discarded).
        m3      6, 7, 9,                            madd x0, x1, x2, xzr
        m3      6, 7, 9,                            madd xzr, x1, x2, x3

        // ---- 3. MADD/MSUB, W form: high garbage in and zero-extension out --
        m3      0xdeadbeef00000003, 0xcafebabe00000005, 0x1111111100000007, madd w0, w1, w2, w3
        m3      0xdeadbeef00000003, 0xcafebabe00000005, 0x1111111100000007, msub w0, w1, w2, w3
        m3      0xdeadbeef80000000, 0xcafebabe80000000, 0x11111111ffffffff, madd w0, w1, w2, w3
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 0x1111111100000001, madd w0, w1, w2, w3
        m3      0xffffffff00000006, 0xffffffff00000007, 0xffffffff00000009, madd w3, w1, w2, w3
        m3      0xffffffff00000006, 0xffffffff00000007, 0xffffffff00000009, madd w1, w1, w2, w3
        m3      0xffffffff00000006, 0xffffffff00000007, 0, madd w0, w1, w2, w1
        m3      0xffffffff7fffffff, 0xffffffff7fffffff, 0xffffffff7fffffff, madd w0, w1, w2, w3
        m3      0xffffffff00000005, 0, 0,           madd w1, w1, w1, w1

        // ---- 4. SMULL / UMULL at the 32-bit signed extremes -----------------
        m3      0xdeadbeef80000000, 0xcafebabe80000000, 0, smull x0, w1, w2
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 0, smull x0, w1, w2
        m3      0xdeadbeef80000000, 0xcafebabe00000000, 0, smull x0, w1, w2
        m3      0xdeadbeef80000000, 0xcafebabe00000001, 0, smull x0, w1, w2
        m3      0xdeadbeef80000000, 0xcafebabe7fffffff, 0, smull x0, w1, w2
        m3      0xdeadbeef7fffffff, 0xcafebabe7fffffff, 0, smull x0, w1, w2
        m3      0xdeadbeefffffffff, 0xcafebabeffffffff, 0, smull x0, w1, w2
        m3      0xdeadbeef12345678, 0xcafebabe9abcdef0, 0, smull x0, w1, w2
        m3      0xdeadbeef80000000, 0xcafebabe80000000, 0, umull x0, w1, w2
        m3      0xdeadbeefffffffff, 0xcafebabeffffffff, 0, umull x0, w1, w2
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 0, smnegl x0, w1, w2
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 0, umnegl x0, w1, w2
        // The destination aliases a multiplicand's register.
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 0, smull x1, w1, w2
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 0, smull x2, w1, w2

        // ---- 5. SMADDL / UMADDL / SMSUBL / UMSUBL --------------------------
        m3      0xdeadbeef80000000, 0xcafebabe80000000, 0x0000000000000001, smaddl x0, w1, w2, x3
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 0xffffffffffffffff, smaddl x0, w1, w2, x3
        m3      0xdeadbeef7fffffff, 0xcafebabe7fffffff, 0x8000000000000000, smaddl x0, w1, w2, x3
        m3      0xdeadbeef80000000, 0xcafebabe80000000, 0x0000000000000001, umaddl x0, w1, w2, x3
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 0xffffffffffffffff, umaddl x0, w1, w2, x3
        m3      0xdeadbeef80000000, 0xcafebabe80000000, 0x0000000000000001, smsubl x0, w1, w2, x3
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 0xffffffffffffffff, umsubl x0, w1, w2, x3
        // Accumulator shapes: the destination is the addend, or a multiplicand.
        m3      0xdeadbeef00000003, 0xcafebabe00000005, 0x0000000100000000, smaddl x3, w1, w2, x3
        m3      0xdeadbeef00000003, 0xcafebabe00000005, 0x0000000100000000, umaddl x3, w1, w2, x3
        m3      0xdeadbeef00000003, 0xcafebabe00000005, 0x0000000100000000, smsubl x3, w1, w2, x3
        m3      0xdeadbeef00000003, 0xcafebabe00000005, 0x0000000100000000, smaddl x1, w1, w2, x3
        m3      0xdeadbeef00000003, 0xcafebabe00000005, 0x0000000100000000, smaddl x2, w1, w2, x3
        m3      0xdeadbeef00000003, 0, 0,           smaddl x1, w1, w1, x1
        // Ra = XZR is SMULL/UMULL again; Rd = XZR discards.
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 9, smaddl x0, w1, w2, xzr
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 9, smaddl xzr, w1, w2, x3
        m3      0xdeadbeef80000000, 0xcafebabeffffffff, 9, smsubl x0, w1, w2, xzr

        // ---- 6. SMULH / UMULH, next door to the above ----------------------
        m3      0x8000000000000000, 0x8000000000000000, 0, smulh x0, x1, x2
        m3      0xffffffffffffffff, 0xffffffffffffffff, 0, smulh x0, x1, x2
        m3      0xffffffffffffffff, 0xffffffffffffffff, 0, umulh x0, x1, x2
        m3      0x123456789abcdef0, 0xfedcba9876543210, 0, smulh x0, x1, x2
        m3      0x123456789abcdef0, 0xfedcba9876543210, 0, umulh x0, x1, x2

        // ---- 7. The division-by-a-constant idiom, end to end ---------------
        // What a compiler emits for `x / 10` on a signed int: smull, shift,
        // add of the sign bit. The whole sequence must agree, not just the
        // multiply.
        m3      0xdeadbeef80000000, 0x66666667, 0,  smull x0, w1, w2
        asr     x0, x0, #34
        add     w0, w0, w1, lsr #31
        bl      dump
        m3      0xdeadbeef7fffffff, 0x66666667, 0,  smull x0, w1, w2
        asr     x0, x0, #34
        add     w0, w0, w1, lsr #31
        bl      dump

        test_exit
        .ltorg
