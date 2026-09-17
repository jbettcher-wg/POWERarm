// SPDX-License-Identifier: MIT
//
// Cases that discriminate the three backend bugs of the scalar-FP research
// (SCALAR-FP-LOWERING.md §10). The random corpus in gen_simd.py does not
// reach any of them, so they are enumerated here. Differential: compared
// against the Pi.
//
//  B1  FNMADD/FNMSUB (and the vector FMLA/FMLS forms behind VFNMLA/VFNMLS)
//      must negate the multiplicand *before* the single rounding. Lowering
//      them to xvnmadda*/xvnmsuba*, which negate the rounded result, differs
//      in the sign of an exact zero, in the magnitudes of the directed
//      rounding modes and in the sign of a NaN.
//  B2  SCVTF s,x rounds i64 -> f32 exactly once (fcfids); fcfid+frsp rounds
//      twice and lands on a different float for 0x8000004000000001.
//  B3  ADDV .4S wraps modulo 2^32; vsumsws saturates (and sets VSCR.SAT).
#include "common.S"

        // Load a 64-bit immediate into Xd.
        .macro imm64 xd, val
        movz    \xd, #(((\val) >> 48) & 0xffff), lsl #48
        movk    \xd, #(((\val) >> 32) & 0xffff), lsl #32
        movk    \xd, #(((\val) >> 16) & 0xffff), lsl #16
        movk    \xd, #(((\val)      ) & 0xffff)
        .endm

        // d\n <- bits, d\m <- bits2, d\a <- bits3, run \op, report d0 bits.
        .macro fma3 op, nbits, mbits, abits
        imm64   x9, \nbits
        fmov    d1, x9
        imm64   x9, \mbits
        fmov    d2, x9
        imm64   x9, \abits
        fmov    d3, x9
        \op     d0, d1, d2, d3
        fmov    x5, d0
        bl      dump
        .endm

        .macro fma3s op, nbits, mbits, abits
        mov     w9, #((\nbits) & 0xffff)
        movk    w9, #(((\nbits) >> 16) & 0xffff), lsl #16
        fmov    s1, w9
        mov     w9, #((\mbits) & 0xffff)
        movk    w9, #(((\mbits) >> 16) & 0xffff), lsl #16
        fmov    s2, w9
        mov     w9, #((\abits) & 0xffff)
        movk    w9, #(((\abits) >> 16) & 0xffff), lsl #16
        fmov    s3, w9
        \op     s0, s1, s2, s3
        fmov    w5, s0
        bl      dump
        .endm

        // Set FPCR.RMode (bits 23:22): 0 RN, 1 RP, 2 RM, 3 RZ.
        .macro rmode m
        mrs     x9, fpcr
        and     x9, x9, #~(3 << 22)
        .if (\m) != 0
        orr     x9, x9, #((\m) << 22)
        .endif
        msr     fpcr, x9
        .endm

        test_start

// --- B1: sign of an exact zero, all four FMA forms, both widths -----------
// -(0 * 0) +- 0 with every sign combination. A negate-after-rounding
// lowering gets the sign of the zero wrong in half of these.
        .irp o, fmadd, fmsub, fnmadd, fnmsub
        fma3    \o, 0x0000000000000000, 0x0000000000000000, 0x0000000000000000
        fma3    \o, 0x8000000000000000, 0x0000000000000000, 0x0000000000000000
        fma3    \o, 0x0000000000000000, 0x8000000000000000, 0x0000000000000000
        fma3    \o, 0x0000000000000000, 0x0000000000000000, 0x8000000000000000
        fma3    \o, 0x8000000000000000, 0x8000000000000000, 0x8000000000000000
        fma3s   \o, 0x00000000, 0x00000000, 0x00000000
        fma3s   \o, 0x80000000, 0x00000000, 0x00000000
        fma3s   \o, 0x00000000, 0x00000000, 0x80000000
        .endr

// --- B1: directed rounding, where -round_up(x) != round_up(-x) ------------
// 1/3-ish products that land between two representables, in RP/RM/RZ.
        .irp m, 1, 2, 3
        rmode   \m
        .irp o, fmadd, fmsub, fnmadd, fnmsub
        fma3    \o, 0x3FD5555555555555, 0x3FD5555555555555, 0x3FF0000000000001
        fma3    \o, 0xBFD5555555555555, 0x3FD5555555555555, 0x3FF0000000000001
        fma3    \o, 0x3FF0000000000001, 0x3FF0000000000001, 0xBFF0000000000000
        fma3s   \o, 0x3EAAAAAB, 0x3EAAAAAB, 0x3F800001
        fma3s   \o, 0xBEAAAAAB, 0x3EAAAAAB, 0x3F800001
        .endr
        .endr
        rmode   0

// --- B1: NaN sign and payload through the negated forms -------------------
        .irp o, fmadd, fmsub, fnmadd, fnmsub
        fma3    \o, 0x7FF8000000000001, 0x3FF0000000000000, 0x3FF0000000000000
        fma3    \o, 0xFFF8000000000001, 0x3FF0000000000000, 0x3FF0000000000000
        fma3    \o, 0x7FF4000000000001, 0x3FF0000000000000, 0x3FF0000000000000
        fma3    \o, 0x3FF0000000000000, 0x3FF0000000000000, 0xFFF8000000000001
        fma3    \o, 0x0000000000000000, 0x7FF0000000000000, 0x7FF8000000000001
        .endr

// --- B1: the vector forms (FMLA/FMLS .2d/.4s) ----------------------------
// FMLS Vd, Vn, Vm is Vd - Vn*Vm, the negated-product shape the backend
// reaches through VFNMLA/VFNMLS.
        imm64   x9, 0x8000000000000000
        fmov    d1, x9
        mov     v1.d[1], x9
        movi    v2.2d, #0
        movi    v3.2d, #0
        mov     v0.16b, v3.16b
        fmls    v0.2d, v1.2d, v2.2d
        fmov    x5, d0
        mov     x6, v0.d[1]
        bl      dump
        mov     v0.16b, v3.16b
        fmla    v0.2d, v1.2d, v2.2d
        fmov    x5, d0
        mov     x6, v0.d[1]
        bl      dump
        movi    v1.4s, #0x80, lsl #24
        movi    v2.4s, #0
        movi    v0.4s, #0
        fmls    v0.4s, v1.4s, v2.4s
        fmov    x5, d0
        mov     x6, v0.d[1]
        bl      dump

// --- B2: SCVTF s,x rounds once -------------------------------------------
        .irp v, 0x8000004000000001, 0x7FFFFFBFFFFFFFFF, 0x0000000000FFFFFF, \
                0x0000000001000001, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, \
                0x0000004000000001, 0xFFFFFF7FFFFFFFFF
        imm64   x9, \v
        scvtf   s0, x9
        fmov    w5, s0
        ucvtf   s1, x9
        fmov    w6, s1
        scvtf   d2, x9
        fmov    x7, d2
        bl      dump
        .endr
        // The same under every rounding mode.
        .irp m, 1, 2, 3
        rmode   \m
        imm64   x9, 0x8000004000000001
        scvtf   s0, x9
        fmov    w5, s0
        imm64   x9, 0x0000000001000001
        scvtf   s1, x9
        fmov    w6, s1
        bl      dump
        .endr
        rmode   0

// --- B3: ADDV .4S wraps -------------------------------------------------
        adrp    x9, addv_vecs
        add     x9, x9, :lo12:addv_vecs
        mov     x10, #0
1:      ldr     q1, [x9, x10]
        addv    s0, v1.4s
        fmov    w5, s0
        addv    h2, v1.8h
        umov    w6, v2.h[0]
        addv    b3, v1.16b
        umov    w7, v3.b[0]
        uaddlv  d4, v1.4s
        fmov    x8, d4
        bl      dump
        add     x10, x10, #16
        cmp     x10, #(16 * 8)
        b.ne    1b

        test_exit

        .section .rodata
        .balign 16
addv_vecs:
        // Word sums that overflow a signed 32-bit accumulator in both
        // directions, plus the exact saturation boundaries.
        .word 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF
        .word 0x80000000, 0x80000000, 0x80000000, 0x80000000
        .word 0x7FFFFFFF, 0x00000001, 0x00000000, 0x00000000
        .word 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
        .word 0x40000000, 0x40000000, 0x40000000, 0x40000000
        .word 0xC0000000, 0xC0000000, 0x00000000, 0x00000000
        .word 0x12345678, 0x9ABCDEF0, 0x0F0F0F0F, 0xF0F0F0F0
        .word 0x00000001, 0x00000002, 0x00000003, 0x00000004
