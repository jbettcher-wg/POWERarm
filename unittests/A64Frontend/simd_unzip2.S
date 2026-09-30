// SPDX-License-Identifier: MIT
//
// UZP2 and the pairwise reductions built on it (ISA-OPPORTUNITIES.md 3.11).
// Differential: compared against the Pi.
//
// The backend lowers VUnZip2 by shifting each operand so the odd-indexed
// elements land on even positions and then packing. The shift used to bring
// in zeros from a zeroed register; it now rotates the operand against itself,
// which is one instruction shorter and is only correct because the pack
// discards exactly the bytes the two forms disagree about (the leading 1, 2 or
// 4 bytes for the byte, halfword and word element sizes). If that reasoning is
// wrong, the wrapped-round bytes appear in the result.
//
// So every source here has sixteen DISTINCT bytes: v1 is 00..0f and v2 is
// 10..1f, ascending by LE byte index. Any byte of the result that comes from
// the wrong place names itself. The same vectors run through UZP1, ZIP1/2 and
// TRN1/2, which share the pack-and-shift machinery, and through
// ADDP/UMAXP/UMINP/SMAXP/SMINP, which the frontend lowers as UZP1 + UZP2 +
// the arithmetic -- those also pin down which operand is the even half and
// which the odd, a thing a byte-identical-lanes test cannot see.
//
// Q=0 and Q=1 both run: the 64-bit forms take a different frontend path (the
// two low halves are concatenated first) onto the same backend op.
#include "common.S"

        .macro imm64 xd, val
        movz    \xd, #(((\val) >> 48) & 0xffff), lsl #48
        movk    \xd, #(((\val) >> 32) & 0xffff), lsl #32
        movk    \xd, #(((\val) >> 16) & 0xffff), lsl #16
        movk    \xd, #(((\val)      ) & 0xffff)
        .endm

        .macro setv reg, lo, hi
        imm64   x9, \lo
        imm64   x10, \hi
        fmov    d\reg\(), x9
        mov     v\reg\().d[1], x10
        .endm

        .macro dirty
        movi    v0.16b, #0xff
        .endm

        .macro report
        fmov    x5, d0
        mov     x6, v0.d[1]
        bl      dump
        .endm

        // Every permute at one arrangement.
        .macro perms t
        .irp o, uzp1, uzp2, zip1, zip2, trn1, trn2
        dirty
        \o      v0.\t, v1.\t, v2.\t
        report
        .endr
        .endm

        // Every pairwise reduction at one arrangement.
        .macro pairs t
        .irp o, addp, umaxp, uminp, smaxp, sminp
        dirty
        \o      v0.\t, v1.\t, v2.\t
        report
        .endr
        .endm

        test_start

// --- sixteen distinct bytes per operand ----------------------------------
        setv    1, 0x0706050403020100, 0x0f0e0d0c0b0a0908
        setv    2, 0x1716151413121110, 0x1f1e1d1c1b1a1918
        perms   16b
        perms   8h
        perms   4s
        perms   2d
        perms   8b
        perms   4h
        perms   2s
        pairs   16b
        pairs   8h
        pairs   4s
        pairs   8b
        pairs   4h
        pairs   2s

// --- the same bytes, operands exchanged ----------------------------------
// Names which operand supplies the low half of the result.
        setv    1, 0x1716151413121110, 0x1f1e1d1c1b1a1918
        setv    2, 0x0706050403020100, 0x0f0e0d0c0b0a0908
        perms   16b
        perms   8h
        perms   4s
        perms   2d
        perms   8b
        perms   4h
        perms   2s
        pairs   16b
        pairs   8h
        pairs   4s

// --- a pattern where only the discarded bytes differ ---------------------
// Both operands agree on every byte the pack keeps for the word form and
// disagree on the four bytes it drops, so a lowering that read the dropped
// bytes would return the other operand's data.
        setv    1, 0xAAAAAAAA00112233, 0xAAAAAAAA44556677
        setv    2, 0x555555558899AABB, 0x55555555CCDDEEFF
        perms   4s
        perms   8h
        perms   16b
        pairs   4s

// --- pairwise arithmetic that has to wrap, saturate or compare signed ----
// Sign matters: 0x80 is the most negative byte and the largest unsigned one.
        setv    1, 0x807F01FF8081027F, 0x00FF7F80FF010080
        setv    2, 0x7F80FF0100FF807F, 0x8001FF7F80FF0100
        pairs   16b
        pairs   8h
        pairs   4s
        pairs   8b
        pairs   4h
        pairs   2s
// Sums that carry out of every element size.
        setv    1, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF80000000
        setv    2, 0x8000000000000001, 0x7FFFFFFF7FFFFFFF
        pairs   16b
        pairs   8h
        pairs   4s

// --- the scalar pairwise form, and the 2D pairwise add -------------------
        setv    1, 0x0123456789ABCDEF, 0xFEDCBA9876543210
        dirty
        addp    d0, v1.2d
        report
        setv    1, 0xFFFFFFFFFFFFFFFF, 0x0000000000000001
        dirty
        addp    d0, v1.2d
        report

        test_exit
