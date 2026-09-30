// SPDX-License-Identifier: MIT
//
// Register-offset loads and stores -- `ldr x0, [x1, w2, sxtw #2]` and the rest
// of that family -- for the lowering that puts the extend, the scale and the add
// on the memory instruction itself (ISA-OPPORTUNITIES.md §3.6). Every case here
// exercises a path that had no guest caller before: the backend's UXTW and SXTW
// offset arms existed but nothing emitted them, because x86 only ever asks for
// SXTX.
//
// What the shapes can get wrong, and is checked for each of them:
//  * The extend must consume ONLY the low word. Every index register is loaded
//    with 32 bits of garbage in its high half, so a missing zero- or
//    sign-extension reads from a wild address instead of the right one.
//  * SXTW must sign-extend: the negative indices below address BELOW the base.
//  * Both ends of the scale: shift 0 and the largest the encoding allows for the
//    access size (3 for a doubleword, 2 for a word, 1 for a halfword, 0 for a
//    byte), since ISA 3.0's extswsli is selected only when the shift is non-zero.
//  * A W destination must be zero-extended into its X register, so every
//    destination is poisoned with 0xcccc... before the load.
//  * UXTX/LSL and SXTX, which are the same 64-bit index under two encodings.
//  * Rm = XZR, which is the bare base address.
//
// x21 holds buf and x22 holds buf+128; every address stays inside buf's 256
// bytes, whose contents reset_state fixed. Differential: the Pi's own output.
#include "common.S"

        .macro imm64 xd, val
        movz    \xd, #(((\val) >> 48) & 0xffff), lsl #48
        movk    \xd, #(((\val) >> 32) & 0xffff), lsl #32
        movk    \xd, #(((\val) >> 16) & 0xffff), lsl #16
        movk    \xd, #(((\val)      ) & 0xffff)
        .endm

        // Poison the destinations and set a known NZCV: address arithmetic must
        // not touch flags.
        .macro poison
        imm64   x1, 0xcccccccccccccccc
        imm64   x3, 0xdddddddddddddddd
        mov     x17, #0xb0000000
        msr     nzcv, x17
        .endm

        // One register-offset load, with the index register pre-loaded.
        .macro ldrr ins, dst, base, idxr, ext, sh, idxval
        poison
        imm64   x2, \idxval
        \ins    \dst, [\base, \idxr, \ext #\sh]
        bl      dump
        .endm

        // One register-offset store of x4, dumped as the whole buffer.
        .macro strr ins, src, base, idxr, ext, sh, idxval
        imm64   x2, \idxval
        imm64   x4, 0x1122334455667788
        \ins    \src, [\base, \idxr, \ext #\sh]
        bl      dumpbuf
        .endm

        // A vector register-offset load, reported as its two doublewords so the
        // upper half of a sub-128-bit load is checked too.
        .macro ldrrv ins, dst, base, idxr, ext, sh, idxval
        poison
        imm64   x2, \idxval
        movi    v0.16b, #0xff
        \ins    \dst, [\base, \idxr, \ext #\sh]
        mov     x1, v0.d[0]
        mov     x3, v0.d[1]
        bl      dump
        .endm

        test_start
        adrp    x21, buf
        add     x21, x21, :lo12:buf
        add     x22, x21, #128

        // ---- 1. 64-bit loads, every offset shape ---------------------------
        ldrr    ldr, x1, x21, x2,  lsl,  0, 24
        ldrr    ldr, x1, x21, x2,  lsl,  3, 3
        ldrr    ldr, x1, x21, x2,  sxtx, 0, 24
        ldrr    ldr, x1, x22, x2,  sxtx, 0, 0xfffffffffffffff0
        ldrr    ldr, x1, x22, x2,  sxtx, 3, 0xfffffffffffffff8
        ldrr    ldr, x1, x21, w2,  uxtw, 0, 0xffffffff00000018
        ldrr    ldr, x1, x21, w2,  uxtw, 3, 0xdeadbeef00000003
        ldrr    ldr, x1, x21, w2,  sxtw, 0, 0x1234567800000018
        ldrr    ldr, x1, x22, w2,  sxtw, 0, 0x12345678fffffff0
        ldrr    ldr, x1, x22, w2,  sxtw, 3, 0xcafebabefffffff8
        ldrr    ldr, x1, x21, w2,  sxtw, 3, 0x000000010000000f
        ldrr    ldr, x1, x21, xzr, lsl,  0, 0

        // ---- 2. 32-bit loads: the X register must come back zero-extended --
        ldrr    ldr, w1, x21, x2,  lsl,  0, 20
        ldrr    ldr, w1, x21, x2,  lsl,  2, 5
        ldrr    ldr, w1, x21, w2,  uxtw, 0, 0xffffffff00000014
        ldrr    ldr, w1, x21, w2,  uxtw, 2, 0xffffffff00000005
        ldrr    ldr, w1, x22, w2,  sxtw, 0, 0x99999999fffffff0
        ldrr    ldr, w1, x22, w2,  sxtw, 2, 0x99999999fffffffc
        ldrr    ldr, w1, x21, xzr, lsl,  0, 0

        // ---- 3. Narrow loads, zero- and sign-extending ---------------------
        ldrr    ldrb,  w1, x21, w2, uxtw, 0, 0xffffffff00000007
        ldrr    ldrb,  w1, x22, w2, sxtw, 0, 0x11111111ffffffff
        ldrr    ldrsb, w1, x21, w2, uxtw, 0, 0xffffffff00000007
        ldrr    ldrsb, x1, x21, w2, sxtw, 0, 0x2222222200000007
        ldrr    ldrsb, x1, x22, x2, lsl,  0, 0xfffffffffffffffe
        ldrr    ldrh,  w1, x21, w2, uxtw, 0, 0xffffffff00000006
        ldrr    ldrh,  w1, x21, w2, uxtw, 1, 0xffffffff00000003
        ldrr    ldrh,  w1, x22, w2, sxtw, 1, 0x33333333fffffffe
        ldrr    ldrsh, w1, x21, w2, uxtw, 1, 0xffffffff00000003
        ldrr    ldrsh, x1, x21, w2, sxtw, 1, 0x4444444400000003
        ldrr    ldrsw, x1, x21, w2, uxtw, 2, 0xffffffff00000002
        ldrr    ldrsw, x1, x22, w2, sxtw, 2, 0x55555555fffffffe
        ldrr    ldrsw, x1, x21, x2, lsl,  0, 12

        // ---- 4. Stores, read back through the buffer dump ------------------
        strr    str,  x4, x21, x2, lsl,  3, 1
        strr    str,  x4, x21, w2, uxtw, 3, 0xffffffff00000002
        strr    str,  x4, x22, w2, sxtw, 3, 0x66666666fffffff8
        strr    str,  w4, x21, w2, sxtw, 2, 0x7777777700000009
        strr    strh, w4, x21, w2, uxtw, 1, 0xffffffff00000021
        strr    strb, w4, x21, w2, sxtw, 0, 0x8888888800000045
        strr    str,  x4, x21, xzr, lsl, 0, 0

        // ---- 5. Vector register-offset loads and stores --------------------
        ldrrv   ldr, q0, x21, x2, lsl,  4, 2
        ldrrv   ldr, q0, x21, w2, uxtw, 4, 0xffffffff00000003
        ldrrv   ldr, q0, x22, w2, sxtw, 4, 0x99999999ffffffff
        ldrrv   ldr, d0, x21, w2, uxtw, 3, 0xffffffff00000005
        ldrrv   ldr, d0, x22, w2, sxtw, 3, 0xaaaaaaaafffffffe
        ldrrv   ldr, s0, x21, w2, sxtw, 2, 0xbbbbbbbb00000009
        ldrrv   ldr, h0, x21, w2, uxtw, 1, 0xffffffff00000011
        ldrrv   ldr, b0, x22, x2, lsl,  0, 0xfffffffffffffffd
        ldrrv   ldr, d0, x21, x2, lsl,  0, 40

        poison
        movi    v1.16b, #0x5a
        mov     w2, #0x30
        str     d1, [x21, w2, uxtw #0]
        bl      dumpbuf

        // ---- 6. A base that is SP, stored to and read straight back --------
        // Both indices stay inside the 64 bytes reserved here, and the frame is
        // released before `bl dump` writes one of its own.
        poison
        sub     sp, sp, #64
        imm64   x4, 0x0f1e2d3c4b5a6978
        imm64   x2, 0xffffffff00000002
        str     x4, [sp, w2, uxtw #3]
        ldr     x1, [sp, w2, uxtw #3]
        imm64   x2, 0x1234567800000003
        str     x4, [sp, w2, sxtw #3]
        ldr     x3, [sp, w2, sxtw #3]
        add     sp, sp, #64
        bl      dump

        // ---- 7. PRFM (register offset) is architecturally a hint -----------
        poison
        mov     x2, #8
        prfm    pldl1keep, [x21, x2, lsl #3]
        prfm    pldl1keep, [x21, w2, sxtw #3]
        bl      dump

        test_exit
        .ltorg
