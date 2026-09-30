// SPDX-License-Identifier: MIT
//
// The vector integer<->float conversions, which no longer go lane by lane
// through the scalar path but straight onto the VSX vector converts
// (ISA-OPPORTUNITIES.md 3.3). Differential: compared against the Pi.
//
// What the lowering can get wrong, and what is here for it:
//
//  * NaN. POWER's signed converts return the most negative integer for a NaN
//    lane and A64 wants 0, so the lowering masks the signed result with an
//    ordered compare of the source. The unsigned converts are used bare on the
//    claim that they already give 0 for a NaN; every FCVT*U line here is that
//    claim's only check. Both quiet and signalling NaNs, in every lane
//    position, so a mask built for the wrong lane order shows up.
//  * Saturation. A64 saturates out-of-range float->int to the destination's
//    range; the x86-shaped Vector_FToS in the same backend substitutes INT_MIN
//    for positive overflow instead, so a lowering that reached for it would
//    fail here. +-2^31, 2^32 and +-max-float at .4s; +-2^63, 2^64 at .2d.
//  * Negative zero and denormals: both convert to an integer 0, and -0 must
//    not come out as -1 through a floor-then-truncate path.
//  * Ties. FCVTN* is round-to-nearest-EVEN whatever FPCR.RMode says, FCVTA* is
//    ties-away, and neither may be double-rounded by the round-to-integral
//    step that precedes POWER's truncating convert. x.5 and -x.5 at both even
//    and odd magnitudes, under all four rounding modes.
//  * The rounding mode that does apply: SCVTF/UCVTF round with FPCR.RMode.
//    Integers that are not exactly representable (2^24+1 and 2^24+3 in f32,
//    2^53+1 and 2^53+3 in f64, and the signed/unsigned extremes) under all
//    four modes; a lowering that ignored FPSCR.RN would pass only in RN.
//  * Signedness. The same lane bits through the signed and the unsigned
//    convert, including 0x80000000 / 0x8000000000000000.
//  * Width. Every case runs at .4s and .2s (and .2d), with the destination
//    pre-filled with ones, so the half-width forms prove they clear the upper
//    64 bits rather than leaving the lanes the convert also computed.
#include "common.S"

        .macro imm64 xd, val
        movz    \xd, #(((\val) >> 48) & 0xffff), lsl #48
        movk    \xd, #(((\val) >> 32) & 0xffff), lsl #32
        movk    \xd, #(((\val) >> 16) & 0xffff), lsl #16
        movk    \xd, #(((\val)      ) & 0xffff)
        .endm

        // v1 <- four 32-bit lanes, low lane first.
        .macro set4s l0, l1, l2, l3
        imm64   x9, ((((\l1) & 0xffffffff) << 32) | ((\l0) & 0xffffffff))
        imm64   x10, ((((\l3) & 0xffffffff) << 32) | ((\l2) & 0xffffffff))
        fmov    d1, x9
        mov     v1.d[1], x10
        .endm

        // v1 <- two 64-bit lanes, low lane first.
        .macro set2d l0, l1
        imm64   x9, \l0
        imm64   x10, \l1
        fmov    d1, x9
        mov     v1.d[1], x10
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

        // A destination the op must overwrite completely.
        .macro dirty
        movi    v0.16b, #0xff
        .endm

        .macro report
        fmov    x5, d0
        mov     x6, v0.d[1]
        bl      dump
        .endm

        // Every float->int form at 4S and 2S on the current v1.
        .macro fcvt4s
        .irp o, fcvtzs, fcvtzu, fcvtns, fcvtnu, fcvtps, fcvtpu, fcvtms, fcvtmu, fcvtas, fcvtau
        dirty
        \o      v0.4s, v1.4s
        report
        dirty
        \o      v0.2s, v1.2s
        report
        .endr
        .endm

        // Every float->int form at 2D on the current v1.
        .macro fcvt2d
        .irp o, fcvtzs, fcvtzu, fcvtns, fcvtnu, fcvtps, fcvtpu, fcvtms, fcvtmu, fcvtas, fcvtau
        dirty
        \o      v0.2d, v1.2d
        report
        .endr
        .endm

        // Both int->float forms at 4S and 2S on the current v1.
        .macro icvt4s
        .irp o, scvtf, ucvtf
        dirty
        \o      v0.4s, v1.4s
        report
        dirty
        \o      v0.2s, v1.2s
        report
        .endr
        .endm

        .macro icvt2d
        .irp o, scvtf, ucvtf
        dirty
        \o      v0.2d, v1.2d
        report
        .endr
        .endm

        test_start

        .irp m, 0, 1, 2, 3
        rmode   \m

// --- float -> int, single precision -------------------------------------
// zeros and NaNs: +0, -0, a quiet NaN, a signalling NaN.
        set4s   0x00000000, 0x80000000, 0x7FC00000, 0x7F800001
        fcvt4s
// the same NaNs in the other lane positions.
        set4s   0x7FC00123, 0x7FA00ABC, 0xFFC00000, 0xFF800042
        fcvt4s
// infinities and denormals.
        set4s   0x7F800000, 0xFF800000, 0x00000001, 0x807FFFFF
        fcvt4s
// the signed word boundary: 2^31, -2^31, 2^31-128, -2^31-256.
        set4s   0x4F000000, 0xCF000000, 0x4EFFFFFF, 0xCF000001
        fcvt4s
// the unsigned word boundary: 2^32-256, 2^32, 2^33, -1.0.
        set4s   0x4F7FFFFF, 0x4F800000, 0x50000000, 0xBF800000
        fcvt4s
// +-max float and +-min normal: everything saturates but the denormals.
        set4s   0x7F7FFFFF, 0xFF7FFFFF, 0x00800000, 0x80800000
        fcvt4s
// ties at even and odd magnitudes: 0.5, 1.5, 2.5, 3.5.
        set4s   0x3F000000, 0x3FC00000, 0x40200000, 0x40600000
        fcvt4s
// the same ties negated.
        set4s   0xBF000000, 0xBFC00000, 0xC0200000, 0xC0600000
        fcvt4s
// just below and just above a tie: 0.49999997, 0.50000006, 2.4999998, 2.5000005.
        set4s   0x3EFFFFFF, 0x3F000001, 0x401FFFFF, 0x40200001
        fcvt4s
// large odd ties, where the round-to-integral step is the identity:
// 2^23+0.5 is not representable, so these are exact integers already.
        set4s   0x4B000001, 0xCB000001, 0x3F800000, 0xC0000000
        fcvt4s

// --- float -> int, double precision -------------------------------------
        set2d   0x0000000000000000, 0x8000000000000000
        fcvt2d
        set2d   0x7FF8000000000000, 0x7FF0000000000001
        fcvt2d
        set2d   0x7FF8000000000123, 0xFFF4000000000ABC
        fcvt2d
        set2d   0x7FF0000000000000, 0xFFF0000000000000
        fcvt2d
        set2d   0x0000000000000001, 0x800FFFFFFFFFFFFF
        fcvt2d
// the signed doubleword boundary: 2^63, -2^63.
        set2d   0x43E0000000000000, 0xC3E0000000000000
        fcvt2d
// 2^63-1024 and -2^63-2048 (the first value below/above the boundary).
        set2d   0x43DFFFFFFFFFFFFF, 0xC3E0000000000001
        fcvt2d
// the unsigned doubleword boundary: 2^64-2048, 2^64.
        set2d   0x43EFFFFFFFFFFFFF, 0x43F0000000000000
        fcvt2d
// +-max double.
        set2d   0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF
        fcvt2d
// ties: 0.5, 1.5.
        set2d   0x3FE0000000000000, 0x3FF8000000000000
        fcvt2d
// ties: 2.5, 3.5.
        set2d   0x4004000000000000, 0x400C000000000000
        fcvt2d
// ties negated: -0.5, -2.5.
        set2d   0xBFE0000000000000, 0xC004000000000000
        fcvt2d
// either side of a tie, and the word boundary seen from a double.
        set2d   0x3FDFFFFFFFFFFFFF, 0x41E0000000000000
        fcvt2d

// --- int -> float, word lanes -------------------------------------------
        set4s   0x00000000, 0xFFFFFFFF, 0x00000001, 0x00000002
        icvt4s
// the signed word extremes, read both ways.
        set4s   0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE
        icvt4s
// not representable in f32: 2^24+1, 2^24+3, and two values whose rounding
// direction differs between the four modes.
        set4s   0x01000001, 0x01000003, 0x7FFFFFBF, 0xFFFFFF7F
        icvt4s
// exactly representable neighbours of the above, as the control.
        set4s   0x01000000, 0x01000002, 0x00FFFFFF, 0x02000000
        icvt4s

// --- int -> float, doubleword lanes --------------------------------------
        set2d   0x0000000000000000, 0xFFFFFFFFFFFFFFFF
        icvt2d
        set2d   0x7FFFFFFFFFFFFFFF, 0x8000000000000000
        icvt2d
// not representable in f64: 2^53+1, 2^53+3.
        set2d   0x0020000000000001, 0x0020000000000003
        icvt2d
// 2^53 and 2^53+2, exactly representable, as the control.
        set2d   0x0020000000000000, 0x0020000000000002
        icvt2d
// the largest magnitudes that still round.
        set2d   0x7FFFFFFFFFFFFC01, 0x8000000000000401
        icvt2d

// --- fixed point, vector forms -------------------------------------------
// int -> float with a scale: the convert rounds once, the scale is exact.
        set4s   0x01000001, 0x80000000, 0x7FFFFFFF, 0xFFFFFFFF
        .irp f, 1, 8, 31, 32
        .irp o, scvtf, ucvtf
        dirty
        \o      v0.4s, v1.4s, #\f
        report
        dirty
        \o      v0.2s, v1.2s, #\f
        report
        .endr
        .endr
// float -> int with a scale: the product is exact or overflows to an
// infinity, which saturates like the exact product would.
        set4s   0x3FC00000, 0xBFC00000, 0x7F800000, 0x7FC00000
        .irp f, 1, 8, 31, 32
        .irp o, fcvtzs, fcvtzu
        dirty
        \o      v0.4s, v1.4s, #\f
        report
        dirty
        \o      v0.2s, v1.2s, #\f
        report
        .endr
        .endr
        set2d   0x0020000000000001, 0x8000000000000000
        .irp f, 1, 32, 63, 64
        .irp o, scvtf, ucvtf
        dirty
        \o      v0.2d, v1.2d, #\f
        report
        .endr
        .endr
        set2d   0x3FF8000000000000, 0xFFF8000000000000
        .irp f, 1, 32, 63, 64
        .irp o, fcvtzs, fcvtzu
        dirty
        \o      v0.2d, v1.2d, #\f
        report
        .endr
        .endr

        .endr
        rmode   0

// --- the scalar SIMD forms, which keep the per-lane path -----------------
// They share the frontend helpers the vector forms were moved out of, so a
// mistake in the split shows here rather than above.
        .irp m, 0, 1, 2, 3
        rmode   \m
        set4s   0x7FC00000, 0x4F000000, 0x3FC00000, 0xBF000000
        .irp o, fcvtzs, fcvtzu, fcvtns, fcvtnu, fcvtps, fcvtpu, fcvtms, fcvtmu, fcvtas, fcvtau
        dirty
        \o      s0, s1
        report
        .endr
        .irp f, 1, 8, 31
        .irp o, fcvtzs, fcvtzu
        dirty
        \o      s0, s1, #\f
        report
        .endr
        .endr
        set2d   0x43E0000000000000, 0x7FF8000000000000
        .irp o, fcvtzs, fcvtzu, fcvtns, fcvtnu, fcvtps, fcvtpu, fcvtms, fcvtmu, fcvtas, fcvtau
        dirty
        \o      d0, d1
        report
        .endr
        set4s   0x01000001, 0x80000000, 0, 0
        .irp o, scvtf, ucvtf
        dirty
        \o      s0, s1
        report
        .irp f, 1, 8, 31
        dirty
        \o      s0, s1, #\f
        report
        .endr
        .endr
        set2d   0x0020000000000001, 0x8000000000000000
        .irp o, scvtf, ucvtf
        dirty
        \o      d0, d1
        report
        .irp f, 1, 32, 63
        dirty
        \o      d0, d1, #\f
        report
        .endr
        .endr
        .endr
        rmode   0

        test_exit
