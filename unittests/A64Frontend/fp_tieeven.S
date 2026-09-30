// SPDX-License-Identifier: MIT
//
// Round-to-nearest-even where the guest's rounding mode is something else:
// FCVTNS/FCVTNU and FRINTN, which are ties-to-even whatever FPCR.RMode says.
// Differential: compared against the Pi.
//
// POWER has no fixed ties-to-even round for doubles -- xsrdpic and xvrdpic
// round in FPSCR.RN -- so the backend used to force RN to nearest around every
// one of them and put it back afterwards. It now reads the guest's FPCR out of
// the context first and skips that bracket when the mode is already nearest
// (SCALAR-FP-LOWERING.md 5.4, checklist F6). Three things can go wrong with
// that and each has cases here:
//
//  * The wrong branch. If the shadow read or its mask is wrong, the bracket is
//    skipped in RP/RM/RZ and every tie rounds the wrong way. Every corpus
//    value runs in all four modes, and the corpus is ties at even and odd
//    magnitudes, on both signs, plus the values either side of a tie.
//  * A leaked rounding mode. If the slow path forgets to restore RN, or the
//    fast path is reached with RN not nearest, the NEXT floating-point
//    instruction rounds wrong rather than this one. So every case is followed
//    by 1.0/3.0, which has a different answer in each of the four modes, and
//    by a read-back of FPCR, and both are reported alongside the result.
//  * A stale shadow. MSR FPCR writes the context word and syncs FPSCR; the
//    convert reads the context word back. The two must not be reordered or
//    cached across the write, so there are runs that change FPCR between
//    converts inside one straight-line block, and a loop that changes it every
//    iteration.
//
// FCVTAS/FCVTPS/FCVTMS/FCVTZS run on the same corpus as controls: they use
// fixed-mode host instructions and never touch FPSCR, so they must be
// identical in all four modes. FPCR values with FZ, DN, AHP and FZ16 set are
// run too: those bits are outside RMode and must not push the convert onto
// either path by mistake.
#include "common.S"

        .macro imm64 xd, val
        movz    \xd, #(((\val) >> 48) & 0xffff), lsl #48
        movk    \xd, #(((\val) >> 32) & 0xffff), lsl #32
        movk    \xd, #(((\val) >> 16) & 0xffff), lsl #16
        movk    \xd, #(((\val)      ) & 0xffff)
        .endm

        // FPCR <- exactly this value (RMode in bits 23:22).
        .macro setfpcr val
        imm64   x9, \val
        msr     fpcr, x9
        .endm

        .macro rmode m
        mrs     x9, fpcr
        and     x9, x9, #~(3 << 22)
        .if (\m) != 0
        orr     x9, x9, #((\m) << 22)
        .endif
        msr     fpcr, x9
        .endm

        // x7 <- 1.0/3.0 rounded in the mode that is live right now, x6 <- FPCR.
        // Both go out with every result: they are how a leaked or unrestored
        // FPSCR.RN shows up.
        .macro after
        imm64   x9, 0x3FF0000000000000
        fmov    d21, x9
        imm64   x9, 0x4008000000000000
        fmov    d22, x9
        fdiv    d20, d21, d22
        fmov    x7, d20
        mrs     x6, fpcr
        bl      dump
        .endm

        // d1 <- bits.
        .macro setd bits
        imm64   x9, \bits
        fmov    d1, x9
        .endm

        // s1 <- bits.
        .macro sets bits
        mov     w9, #((\bits) & 0xffff)
        movk    w9, #(((\bits) >> 16) & 0xffff), lsl #16
        fmov    s1, w9
        .endm

        // Every scalar float-to-int form on the current d1.
        .macro d_scalar
        .irp o, fcvtns, fcvtnu, fcvtas, fcvtau, fcvtps, fcvtpu, fcvtms, fcvtmu, fcvtzs, fcvtzu
        \o      x5, d1
        after
        \o      w5, d1
        after
        .endr
        frintn  d0, d1
        fmov    x5, d0
        after
        frinta  d0, d1
        fmov    x5, d0
        after
        frinti  d0, d1
        fmov    x5, d0
        after
        .endm

        // Every scalar float-to-int form on the current s1.
        .macro s_scalar
        .irp o, fcvtns, fcvtnu, fcvtas, fcvtau, fcvtps, fcvtpu, fcvtms, fcvtmu, fcvtzs, fcvtzu
        \o      x5, s1
        after
        \o      w5, s1
        after
        .endr
        frintn  s0, s1
        fmov    w5, s0
        after
        frinta  s0, s1
        fmov    w5, s0
        after
        frinti  s0, s1
        fmov    w5, s0
        after
        .endm

        // v1 <- two doubles / four floats.
        .macro setv2d l0, l1
        imm64   x9, \l0
        imm64   x10, \l1
        fmov    d1, x9
        mov     v1.d[1], x10
        .endm

        .macro setv4s l0, l1, l2, l3
        imm64   x9, ((((\l1) & 0xffffffff) << 32) | ((\l0) & 0xffffffff))
        imm64   x10, ((((\l3) & 0xffffffff) << 32) | ((\l2) & 0xffffffff))
        fmov    d1, x9
        mov     v1.d[1], x10
        .endm

        .macro vec t
        .irp o, fcvtns, fcvtnu, fcvtas, fcvtau, fcvtzs, fcvtzu, frintn, frinta, frinti
        movi    v0.16b, #0xff
        \o      v0.\t, v1.\t
        fmov    x5, d0
        mov     x8, v0.d[1]
        after
        .endr
        .endm

        test_start

// --- scalar doubles, every mode ------------------------------------------
        .irp m, 0, 1, 2, 3
        rmode   \m
        .irp b, 0x3FE0000000000000, 0x3FF8000000000000, 0x4004000000000000, 0x400C000000000000
        setd    \b
        d_scalar
        .endr
        .irp b, 0xBFE0000000000000, 0xBFF8000000000000, 0xC004000000000000, 0x4014000000000000
        setd    \b
        d_scalar
        .endr
        // Either side of a tie, then the special values.
        .irp b, 0x3FDFFFFFFFFFFFFF, 0x3FE0000000000001, 0x4003FFFFFFFFFFFF, 0x4004000000000001
        setd    \b
        d_scalar
        .endr
        .irp b, 0x0000000000000000, 0x8000000000000000, 0x7FF8000000000000, 0x7FF0000000000001
        setd    \b
        d_scalar
        .endr
        .irp b, 0x7FF0000000000000, 0xFFF0000000000000, 0x0000000000000001, 0x800FFFFFFFFFFFFF
        setd    \b
        d_scalar
        .endr
        .irp b, 0x41E0000000000000, 0xC1E0000000000000, 0x43E0000000000000, 0xC3E0000000000000
        setd    \b
        d_scalar
        .endr
        .endr

// --- scalar singles, every mode ------------------------------------------
        .irp m, 0, 1, 2, 3
        rmode   \m
        .irp b, 0x3F000000, 0x3FC00000, 0x40200000, 0x40600000
        sets    \b
        s_scalar
        .endr
        .irp b, 0xBF000000, 0xBFC00000, 0xC0200000, 0x40A00000
        sets    \b
        s_scalar
        .endr
        .irp b, 0x3EFFFFFF, 0x3F000001, 0x401FFFFF, 0x40200001
        sets    \b
        s_scalar
        .endr
        .irp b, 0x00000000, 0x80000000, 0x7FC00000, 0x7F800001
        sets    \b
        s_scalar
        .endr
        .irp b, 0x7F800000, 0xFF800000, 0x00000001, 0x807FFFFF
        sets    \b
        s_scalar
        .endr
        .irp b, 0x4F000000, 0xCF000000, 0x5F000000, 0x4B000001
        sets    \b
        s_scalar
        .endr
        .endr

// --- vectors, every mode -------------------------------------------------
        .irp m, 0, 1, 2, 3
        rmode   \m
        setv2d  0x3FE0000000000000, 0x3FF8000000000000
        vec     2d
        setv2d  0x4004000000000000, 0xC004000000000000
        vec     2d
        setv2d  0x7FF8000000000000, 0x8000000000000000
        vec     2d
        setv2d  0x43E0000000000000, 0xC3E0000000000000
        vec     2d
        setv4s  0x3F000000, 0x3FC00000, 0x40200000, 0x40600000
        vec     4s
        vec     2s
        setv4s  0xBF000000, 0x7FC00000, 0x7F800000, 0x00000001
        vec     4s
        vec     2s
        .endr

// --- FPCR bits outside RMode -------------------------------------------
// AHP (26), DN (25), FZ (24) and FZ16 (19) must not change which path the
// convert takes: only bits 23:22 may.
        .irp f, 0x07080000, 0x07480000, 0x07880000, 0x07C80000
        setfpcr \f
        setd    0x3FE0000000000000
        d_scalar
        setd    0x4004000000000000
        d_scalar
        sets    0x3F000000
        s_scalar
        setv2d  0x3FE0000000000000, 0x4004000000000000
        vec     2d
        .endr
        setfpcr 0

// --- FPCR changed between converts inside one block ---------------------
// The shadow the convert reads is the context word MSR FPCR just wrote; they
// must not be reordered or cached across the write.
        setd    0x3FE0000000000000
        rmode   0
        fcvtns  x5, d1
        after
        rmode   1
        fcvtns  x5, d1
        after
        rmode   0
        fcvtns  x5, d1
        after
        rmode   2
        fcvtns  x5, d1
        after
        rmode   3
        fcvtns  x5, d1
        after
        rmode   0
        fcvtns  x5, d1
        after
        setd    0x4004000000000000
        rmode   3
        frintn  d0, d1
        fmov    x5, d0
        rmode   0
        frintn  d2, d1
        fmov    x8, d2
        after

// --- the same, in a loop, so the block is compiled once and run four times
        mov     x19, #0
1:      and     x9, x19, #3
        lsl     x9, x9, #22
        msr     fpcr, x9
        setd    0x3FE0000000000000
        fcvtns  x5, d1
        setd    0x4004000000000000
        fcvtnu  x8, d1
        setv2d  0x3FF8000000000000, 0xC004000000000000
        fcvtns  v0.2d, v1.2d
        fmov    x10, d0
        fmov    x11, v0.d[1]
        after
        add     x19, x19, #1
        cmp     x19, #8
        b.ne    1b
        rmode   0

        test_exit
