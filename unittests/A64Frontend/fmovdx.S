// SPDX-License-Identifier: MIT
//
// FMOV into a vector register from a GPR, FMOV of an immediate, and the
// 64-bit-result ops whose whole tail is "and clear the upper half of the Q
// register" -- the two lowerings of ISA-OPPORTUNITIES.md §3.7 (VCastFromGPR
// through mtvsrdd, and VMov(i64) against the pinned zero register).
//
// Every case re-poisons its destination with all-ones first, so a result whose
// upper lanes were left alone instead of cleared shows up as 0xffff... in the
// vdump rather than as a subtle value difference. The GPR sources carry garbage
// above the moved width for the same reason: FMOV s,w and FMOV h,w must take
// only the low 32 or low 16 bits.
//
// vdump prints V0..V31, FPCR and FPSR (with the cumulative exception bits
// masked), so both halves of every register are compared. Differential: the
// Pi's own output.
#include "common.S"

        .macro imm64 xd, val
        movz    \xd, #(((\val) >> 48) & 0xffff), lsl #48
        movk    \xd, #(((\val) >> 32) & 0xffff), lsl #32
        movk    \xd, #(((\val) >> 16) & 0xffff), lsl #16
        movk    \xd, #(((\val)      ) & 0xffff)
        .endm


        // FMOV d0,x0 / s0,w0 / h0,w0 with \val in the source GPR.
        .macro fmovgpr dst, src, val
        imm64   x0, \val
        movi    v0.16b, #0xff
        fmov    \dst, \src
        bl      vdump
        .endm

        // A 64-bit-result vector or scalar op into v0, from v1/v2/v3.
        .macro op64 text:vararg
        movi    v0.16b, #0xff
        \text
        bl      vdump
        .endm

        test_start
        // All-ones in every vector register: the starting state that makes an
        // uncleared upper half visible in the dumps below.
        movi    v0.16b, #0xff
        movi    v1.16b, #0xff
        movi    v2.16b, #0xff
        movi    v3.16b, #0xff
        movi    v4.16b, #0xff
        movi    v5.16b, #0xff
        movi    v6.16b, #0xff
        movi    v7.16b, #0xff
        movi    v8.16b, #0xff
        movi    v9.16b, #0xff
        movi    v10.16b, #0xff
        movi    v11.16b, #0xff
        movi    v12.16b, #0xff
        movi    v13.16b, #0xff
        movi    v14.16b, #0xff
        movi    v15.16b, #0xff
        movi    v16.16b, #0xff
        movi    v17.16b, #0xff
        movi    v18.16b, #0xff
        movi    v19.16b, #0xff
        movi    v20.16b, #0xff
        movi    v21.16b, #0xff
        movi    v22.16b, #0xff
        movi    v23.16b, #0xff
        movi    v24.16b, #0xff
        movi    v25.16b, #0xff
        movi    v26.16b, #0xff
        movi    v27.16b, #0xff
        movi    v28.16b, #0xff
        movi    v29.16b, #0xff
        movi    v30.16b, #0xff
        movi    v31.16b, #0xff

        // Known operands for the arithmetic below, out of buf.
        adrp    x21, buf
        add     x21, x21, :lo12:buf
        ldr     q1, [x21]
        ldr     q2, [x21, #16]
        ldr     q3, [x21, #32]

        // ---- 1. FMOV d, x --------------------------------------------------
        fmovgpr d0, x0, 0
        fmovgpr d0, x0, 1
        fmovgpr d0, x0, 0x8000000000000000
        fmovgpr d0, x0, 0xffffffffffffffff
        fmovgpr d0, x0, 0x3ff0000000000000
        fmovgpr d0, x0, 0x7ff8000000000001
        fmovgpr d0, x0, 0x123456789abcdef0

        // ---- 2. FMOV s, w: bits 32:63 of the source must not appear --------
        fmovgpr s0, w0, 0
        fmovgpr s0, w0, 0x3f800000
        fmovgpr s0, w0, 0x80000000
        fmovgpr s0, w0, 0xffffffff00000001
        fmovgpr s0, w0, 0xdeadbeefffffffff
        fmovgpr s0, w0, 0x123456789abcdef0

        // ---- 3. FMOV h, w (FEAT_FP16): only bits 0:15 move -----------------
        fmovgpr h0, w0, 0
        fmovgpr h0, w0, 0x3c00
        fmovgpr h0, w0, 0x8000
        fmovgpr h0, w0, 0xffffffffffffffff
        fmovgpr h0, w0, 0xcafebabe12345678

        // ---- 4. FMOV of an immediate --------------------------------------
        op64    fmov d0, #1.0
        op64    fmov d0, #-2.0
        op64    fmov d0, #0.125
        op64    fmov s0, #1.0
        op64    fmov s0, #-31.0
        op64    fmov h0, #1.0
        op64    fmov h0, #-0.5

        // ---- 5. FMOV out of a vector register, and into its upper half -----
        imm64   x0, 0x0f1e2d3c4b5a6978
        movi    v0.16b, #0xff
        fmov    d0, x0
        fmov    x1, d0
        fmov    w2, s0
        fmov    x3, v0.d[1]
        bl      dump
        bl      vdump

        imm64   x0, 0x00ff00ff00ff00ff
        movi    v0.16b, #0xff
        fmov    d0, xzr
        fmov    v0.d[1], x0
        bl      vdump

        // ---- 6. FMOV register to register (the plain StoreVSized tail) -----
        op64    fmov d0, d1
        op64    fmov s0, s1
        op64    fmov h0, h1

        // ---- 7. 64-bit vector results: the upper half must be cleared ------
        op64    add v0.2s, v1.2s, v2.2s
        op64    add v0.8b, v1.8b, v2.8b
        op64    sub v0.4h, v1.4h, v2.4h
        op64    mul v0.2s, v1.2s, v2.2s
        op64    and v0.8b, v1.8b, v2.8b
        op64    orr v0.8b, v1.8b, v2.8b
        op64    eor v0.8b, v1.8b, v2.8b
        op64    neg v0.2s, v1.2s
        op64    cmeq v0.2s, v1.2s, v2.2s
        op64    ushl v0.4h, v1.4h, v2.4h
        op64    addp v0.2s, v1.2s, v2.2s
        op64    zip1 v0.2s, v1.2s, v2.2s
        op64    fadd v0.2s, v1.2s, v2.2s
        op64    fmul v0.2s, v1.2s, v2.2s
        op64    dup v0.2s, v1.s[1]
        op64    ext v0.8b, v1.8b, v2.8b, #3

        // ---- 8. Scalar FP results, which end in the same clear -------------
        op64    fadd d0, d1, d2
        op64    fsub d0, d1, d2
        op64    fmul s0, s1, s2
        op64    fdiv d0, d1, d2
        op64    fabs d0, d1
        op64    fneg s0, s1
        op64    fsqrt d0, d1
        op64    fmadd d0, d1, d2, d3
        op64    fcvt s0, d1
        op64    fcvt d0, s1
        op64    scvtf d0, x21
        op64    fmov d0, d1

        // ---- 9. DUP and INS from a GPR, which share the cast path ----------
        imm64   x0, 0xfedcba9876543210
        op64    dup v0.2s, w0
        op64    dup v0.2d, x0
        op64    ins v0.s[1], w0
        op64    ins v0.d[1], x0

        test_exit
        .ltorg
