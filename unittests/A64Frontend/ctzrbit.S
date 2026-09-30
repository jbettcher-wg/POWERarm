// SPDX-License-Identifier: MIT
//
// RBIT and CLZ, for the lowering that makes `rbit ; clz` one CTZ instruction
// (ISA-OPPORTUNITIES.md §3.4). Three things are under test and the generated
// `bits` sweep reaches none of them, because it emits one instruction per case:
//
//  1. The FUSED pair `rbit Rd,Rn ; clz Rd,Rd`, which is what GCC and clang emit
//     for __builtin_ctz and what glibc's string functions end in. Zero is the
//     case that discriminates: rbit(0) = 0 and clz(0) = width, so CTZ of zero
//     must be 64 for X and 32 for W, and cnttzd/cnttzw give exactly that.
//  2. The shapes that must NOT fuse -- a CLZ that writes a different register,
//     one that reads a different register, a mismatched operand width, an
//     instruction in between, and a CLZ that is a branch target (so it begins a
//     decoded block of its own and the RBIT's write is observable).
//  3. The STANDALONE bit reverse, which no longer goes through the stack: the
//     ISA 3.0 path is vgbbd/xxbrd in the vector unit, and 0x8000000000000000
//     (the single top bit, which must land in bit 0) and 0 are the two values a
//     wrong byte- or bit-order composition gets wrong quietly.
//
// NZCV is set to a known non-zero pattern before every case and dumped with the
// registers: neither instruction writes flags, and the record forms the host has
// for these counts (cnttzd.) must not be the ones emitted.
//
// Differential: every line is the Pi's own output.
#include "common.S"

        .macro imm64 xd, val
        movz    \xd, #(((\val) >> 48) & 0xffff), lsl #48
        movk    \xd, #(((\val) >> 32) & 0xffff), lsl #32
        movk    \xd, #(((\val) >> 16) & 0xffff), lsl #16
        movk    \xd, #(((\val)      ) & 0xffff)
        .endm

        .macro flags4 v
        mov     x17, #(\v << 28)
        msr     nzcv, x17
        .endm

        // The fused pair, 64-bit: CTZ of \val into x1.
        .macro ctzx val
        imm64   x0, \val
        imm64   x1, 0xcccccccccccccccc
        flags4  0xb
        rbit    x1, x0
        clz     x1, x1
        bl      dump
        .endm

        // The fused pair, 32-bit. x0 carries high garbage that the W form must
        // ignore, and the X view of x1 must come back zero-extended.
        .macro ctzw val
        imm64   x0, \val
        imm64   x1, 0xcccccccccccccccc
        flags4  0x6
        rbit    w1, w0
        clz     w1, w1
        bl      dump
        .endm

        // Standalone bit reverse, no CLZ consumer (the mov keeps x1 live).
        .macro rbitx val
        imm64   x0, \val
        imm64   x1, 0xcccccccccccccccc
        flags4  0x9
        rbit    x1, x0
        mov     x2, x1
        bl      dump
        .endm

        .macro rbitw val
        imm64   x0, \val
        imm64   x1, 0xcccccccccccccccc
        flags4  0x3
        rbit    w1, w0
        mov     x2, x1
        bl      dump
        .endm

        // Same source and destination: `rbit x0,x0 ; clz x0,x0`.
        .macro ctzx_inplace val
        imm64   x0, \val
        flags4  0xf
        rbit    x0, x0
        clz     x0, x0
        bl      dump
        .endm

        test_start

        // ---- 1. The fused 64-bit pair --------------------------------------
        ctzx    0
        ctzx    1
        ctzx    2
        ctzx    0x80
        ctzx    0x100
        ctzx    0x8000000000000000
        ctzx    0x8000000000000001
        ctzx    0xffffffffffffffff
        ctzx    0xfffffffffffffffe
        ctzx    0x100000000
        ctzx    0xffffffff00000000
        ctzx    0x00000000ffffffff
        ctzx    0x123456789abcdef0
        ctzx    0xfedcba9876543210
        ctzx    0x0010000000000000

        // ---- 2. The fused 32-bit pair, with high garbage in the source -----
        ctzw    0
        ctzw    1
        ctzw    2
        ctzw    0x80
        ctzw    0x80000000
        ctzw    0x80000001
        ctzw    0xffffffff
        ctzw    0x00010000
        ctzw    0xdeadbeef00000000      // w0 == 0: the W answer is 32, not 64
        ctzw    0xffffffff00000001
        ctzw    0x1234567800000000      // w0 == 0 again, different garbage
        ctzw    0xcafebabe9abcdef0

        // ---- 3. Same register for source and destination -------------------
        ctzx_inplace 0
        ctzx_inplace 0x8000000000000000
        ctzx_inplace 0x00f0000000000000

        // ---- 4. Standalone RBIT, 64-bit -----------------------------------
        rbitx   0
        rbitx   1
        rbitx   2
        rbitx   0x8000000000000000
        rbitx   0x4000000000000000
        rbitx   0x0000000000000001
        rbitx   0xffffffffffffffff
        rbitx   0x123456789abcdef0
        rbitx   0xfedcba9876543210
        rbitx   0xff00ff00ff00ff00
        rbitx   0x0102040810204080
        rbitx   0x00000000ffffffff
        rbitx   0xffffffff00000000

        // ---- 5. Standalone RBIT, 32-bit: the X register must zero-extend ---
        rbitw   0
        rbitw   1
        rbitw   0x80000000
        rbitw   0xffffffff
        rbitw   0x12345678
        rbitw   0xffffffff80000000
        rbitw   0xdeadbeef00000001

        // ---- 6. Shapes that must NOT fuse ---------------------------------
        // (a) The CLZ writes a different register, so the RBIT's value stays
        //     live and both results are dumped.
        imm64   x0, 0x0000000000001000
        flags4  0xb
        rbit    x1, x0
        clz     x2, x1
        bl      dump

        // (b) The CLZ reads a different register.
        imm64   x0, 0x0000000000001000
        imm64   x3, 0x0000000100000000
        flags4  0x6
        rbit    x1, x0
        clz     x1, x3
        bl      dump

        // (c) Mismatched operand widths, both ways round.
        imm64   x0, 0xffffffff00010000
        flags4  0x9
        rbit    w1, w0
        clz     x1, x1
        bl      dump

        imm64   x0, 0xffffffff00010000
        flags4  0x9
        rbit    x1, x0
        clz     w1, w1
        bl      dump

        // (d) An instruction in between.
        imm64   x0, 0x0000000000000400
        flags4  0x3
        rbit    x1, x0
        add     x4, x4, #1
        clz     x1, x1
        bl      dump

        // (e) The CLZ is a branch target. Taken (the RBIT is skipped entirely)
        //     and not taken (the RBIT runs, and its write is visible to the
        //     separately entered CLZ).
        imm64   x0, 0x0000000000000200
        imm64   x1, 0x00000000000000f0
        mov     x2, #0
        flags4  0xb
        cbz     x2, 1f
        rbit    x1, x0
1:      clz     x1, x1
        bl      dump

        imm64   x0, 0x0000000000000200
        imm64   x1, 0x00000000000000f0
        mov     x2, #1
        flags4  0xb
        cbz     x2, 2f
        rbit    x1, x0
2:      clz     x1, x1
        bl      dump

        // (f) The pair is the last thing in a block: a taken branch follows it,
        //     and a backward one, which is where an asynchronous signal would
        //     be drained.
        imm64   x0, 0x0000000000000040
        mov     x5, #3
3:      rbit    x1, x0
        clz     x1, x1
        sub     x5, x5, #1
        cbnz    x5, 3b
        bl      dump

        // (g) RBIT into XZR, then a CLZ of XZR: nothing is written, and the
        //     fusion must not claim the pair.
        imm64   x0, 0x0000000000000080
        imm64   x1, 0x1111111111111111
        flags4  0xf
        rbit    xzr, x0
        clz     xzr, xzr
        bl      dump

        // ---- 7. CLZ and CLS on their own, for the same values -------------
        imm64   x0, 0
        flags4  0x6
        clz     x1, x0
        clz     w2, w0
        cls     x3, x0
        bl      dump

        imm64   x0, 0xffffffffffffffff
        flags4  0x6
        clz     x1, x0
        clz     w2, w0
        cls     x3, x0
        bl      dump

        test_exit
        .ltorg
