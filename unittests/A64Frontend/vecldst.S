// SPDX-License-Identifier: MIT
//
// Guest 128-bit vector loads and stores, for the lowering that puts the
// addressing mode on the access instruction instead of collapsing it into one
// register first: the X-form's index on every ISA level, and ISA 3.0's DQ-form
// displacement (lxv/stxv) when the displacement is a multiple of 16.
//
// What that partition can get wrong, and is checked here for each shape:
//  * The DQ field holds EA bits 4:15, so a displacement that is NOT a multiple
//    of 16 must not take it. Every LDUR/STUR offset 0..15 is exercised, and
//    the same addresses are reached a second time through a different shape so
//    a dropped low nibble shows up as two different answers for one address.
//  * The signed 12-bit DQ field tops out at 32752. Offsets of 32768 and above
//    must fall out to a materialized index, so vbuf is big enough to address
//    past it.
//  * `lvx`/`stvx` MASK the effective address to a 16-byte boundary; `lxv`,
//    `lxvx` and the pre-ISA-3.0 `lxvd2x` do not. AArch64's LDR q has no
//    alignment requirement, so every unaligned offset below would silently
//    read or write the containing aligned quadword if an EA-masking form were
//    ever selected for it.
//  * Guest V16-V31 live in host vs16-vs31, the FPR-aliased VSX half that
//    VMX-form instructions cannot name. Every shape is run into a low-bank
//    register as well as a VMX-half one, and vdump prints all 32.
//  * Half-width forms (LDR b/h/s/d) must clear the rest of the V register, so
//    every destination is poisoned with 0xff bytes first.
//  * Element order: a stored register is read back through differently-sized
//    loads, which pins which byte of memory holds which byte of the register.
//  * A load or store spanning a page boundary must still move 16 bytes.
//
// Differential: the Pi's own output. buf is the 256 bytes common.S fills;
// vbuf is this test's own page-aligned buffer, filled below.
#include "common.S"

        .macro imm64 xd, val
        movz    \xd, #(((\val) >> 48) & 0xffff), lsl #48
        movk    \xd, #(((\val) >> 32) & 0xffff), lsl #32
        movk    \xd, #(((\val) >> 16) & 0xffff), lsl #16
        movk    \xd, #(((\val)      ) & 0xffff)
        .endm

        // Poison every V register with 0xff bytes, so a half-width load that
        // fails to clear the upper lanes shows it, and set a known NZCV:
        // address arithmetic must not touch the flags.
        .macro vpoison
        movi    v0.16b,  #0xff
        movi    v1.16b,  #0xff
        movi    v2.16b,  #0xff
        movi    v3.16b,  #0xff
        movi    v16.16b, #0xff
        movi    v17.16b, #0xff
        movi    v23.16b, #0xff
        movi    v31.16b, #0xff
        mov     x17, #0xb0000000
        msr     nzcv, x17
        .endm

        // Report V<vn>'s two doublewords compactly: x1 = d[0], x3 = d[1].
        .macro vrep vn
        mov     x1, \vn\().d[0]
        mov     x3, \vn\().d[1]
        bl      dump
        .endm

        // One vector load, poisoned destination, reported as two doublewords.
        // `vn` is the whole V register `dst` writes into: a half-width load
        // names e.g. d16, and the report has to read all 128 bits of v16 to
        // show that the rest of it was cleared.
        .macro vld ins, dst, vn, mem:vararg
        vpoison
        \ins    \dst, \mem
        vrep    \vn
        .endm

        // One vector store into buf, read back through the whole buffer.
        .macro vst ins, src, mem:vararg
        \ins    \src, \mem
        bl      dumpbuf
        .endm

        test_start
        // x21 = buf, x22 = buf+128 (so negative offsets stay inside),
        // x23 = vbuf, x24 = vbuf+4096 (the page boundary), x25 = vbuf+4080.
        adrp    x21, buf
        add     x21, x21, :lo12:buf
        add     x22, x21, #128
        adrp    x23, vbuf
        add     x23, x23, :lo12:vbuf
        add     x24, x23, #4096
        sub     x25, x24, #16

        // Fill vbuf's first 12 pages with (i * 101 + 7) & 0xff, a different
        // pattern from buf's so a confused base shows up.
        mov     x10, #0
        mov     w11, #7
1:      strb    w11, [x23, x10]
        add     w11, w11, #101
        add     x10, x10, #1
        cmp     x10, #(12 * 4096)
        b.ne    1b

        // ---- 1. Q loads, unsigned scaled offset (every one a multiple of 16)
        // Into the VMX half and into the low bank, same addresses, so the two
        // must agree byte for byte.
        vld     ldr, q0, v0, [x21]
        vld     ldr, q16, v16, [x21]
        vld     ldr, q0, v0, [x21, #16]
        vld     ldr, q16, v16, [x21, #16]
        vld     ldr, q1, v1, [x21, #48]
        vld     ldr, q17, v17, [x21, #48]
        vld     ldr, q2, v2, [x21, #240]
        vld     ldr, q31, v31, [x21, #240]
        // Past the DQ field: 32752 is the last displacement it holds, 32768
        // and 49136 must fall out to a materialized index.
        vld     ldr, q3, v3, [x23, #32752]
        vld     ldr, q23, v23, [x23, #32752]
        vld     ldr, q3, v3, [x23, #32768]
        vld     ldr, q23, v23, [x23, #32768]
        vld     ldr, q3, v3, [x23, #49136]
        vld     ldr, q23, v23, [x23, #49136]

        // ---- 2. Q loads, unscaled simm9: every offset mod 16 -------------
        // Offsets 0..15 off buf, so each lands at a different alignment, and
        // 16..31 so each aligned/unaligned pair can be compared.
        vld     ldur, q0, v0, [x21, #0]
        vld     ldur, q16, v16, [x21, #1]
        vld     ldur, q0, v0, [x21, #2]
        vld     ldur, q16, v16, [x21, #3]
        vld     ldur, q0, v0, [x21, #4]
        vld     ldur, q16, v16, [x21, #5]
        vld     ldur, q0, v0, [x21, #6]
        vld     ldur, q16, v16, [x21, #7]
        vld     ldur, q0, v0, [x21, #8]
        vld     ldur, q16, v16, [x21, #9]
        vld     ldur, q0, v0, [x21, #10]
        vld     ldur, q16, v16, [x21, #11]
        vld     ldur, q0, v0, [x21, #12]
        vld     ldur, q16, v16, [x21, #13]
        vld     ldur, q0, v0, [x21, #14]
        vld     ldur, q16, v16, [x21, #15]
        vld     ldur, q17, v17, [x21, #16]
        vld     ldur, q17, v17, [x21, #17]
        // Negative, off the middle of buf.
        vld     ldur, q0, v0, [x22, #-16]
        vld     ldur, q16, v16, [x22, #-16]
        vld     ldur, q0, v0, [x22, #-1]
        vld     ldur, q16, v16, [x22, #-1]
        vld     ldur, q0, v0, [x22, #-15]
        vld     ldur, q16, v16, [x22, #-15]

        // ---- 3. Q loads, register offset (the X-form's index) -------------
        imm64   x2, 32
        vld     ldr, q0, v0, [x21, x2]
        vld     ldr, q16, v16, [x21, x2]
        imm64   x2, 3
        vld     ldr, q0, v0, [x21, x2, lsl #4]
        vld     ldr, q16, v16, [x21, x2, lsl #4]
        // The index must be extended from the low word only: both registers
        // carry garbage in the high half.
        imm64   x2, 0xdeadbeef00000020
        vld     ldr, q0, v0, [x21, w2, uxtw]
        vld     ldr, q16, v16, [x21, w2, uxtw]
        imm64   x2, 0xdeadbeef00000002
        vld     ldr, q0, v0, [x21, w2, uxtw #4]
        vld     ldr, q16, v16, [x21, w2, uxtw #4]
        imm64   x2, 0x12345678fffffff0
        vld     ldr, q0, v0, [x22, w2, sxtw]
        vld     ldr, q16, v16, [x22, w2, sxtw]
        imm64   x2, 0x12345678ffffffff
        vld     ldr, q0, v0, [x22, w2, sxtw #4]
        vld     ldr, q16, v16, [x22, w2, sxtw #4]
        // An unaligned register offset: the index must not be masked either.
        imm64   x2, 7
        vld     ldr, q0, v0, [x21, x2]
        vld     ldr, q16, v16, [x21, x2]
        // Rm = XZR is the bare base.
        vld     ldr, q0, v0, [x21, xzr]
        vld     ldr, q16, v16, [x21, xzr]

        // ---- 4. Pairs: LDP/STP q, offset / pre-index / post-index ---------
        vpoison
        ldp     q0, q1, [x21]
        vrep    v0
        vrep    v1
        vpoison
        ldp     q16, q17, [x21]
        vrep    v16
        vrep    v17
        vpoison
        ldp     q0, q1, [x21, #32]
        vrep    v0
        vrep    v1
        vpoison
        ldp     q16, q17, [x21, #32]
        vrep    v16
        vrep    v17
        vpoison
        ldp     q0, q1, [x22, #-32]
        vrep    v0
        vrep    v1
        vpoison
        ldp     q16, q17, [x22, #-32]
        vrep    v16
        vrep    v17
        // Writeback forms: Rn must end up Base+Offset, and the access must be
        // at the right end of it. mov the base into a scratch each time so the
        // pinned bases survive.
        vpoison
        mov     x5, x21
        ldp     q0, q1, [x5, #48]!
        mov     x1, x5
        mov     x3, xzr
        bl      dump
        vrep    v0
        vrep    v1
        vpoison
        mov     x5, x21
        ldp     q16, q17, [x5, #48]!
        mov     x1, x5
        mov     x3, xzr
        bl      dump
        vrep    v16
        vrep    v17
        vpoison
        mov     x5, x21
        ldp     q0, q1, [x5], #64
        mov     x1, x5
        mov     x3, xzr
        bl      dump
        vrep    v0
        vrep    v1
        vpoison
        mov     x5, x21
        ldp     q16, q17, [x5], #64
        mov     x1, x5
        mov     x3, xzr
        bl      dump
        vrep    v16
        vrep    v17
        // LDNP, the no-allocate pair.
        vpoison
        ldnp    q0, q1, [x21, #16]
        vrep    v0
        vrep    v1
        vpoison
        ldnp    q16, q17, [x21, #16]
        vrep    v16
        vrep    v17

        // ---- 5. Half-width loads must clear the rest of the register ------
        // Every element size, into both halves of the register file, at an
        // offset the size does not align.
        vld     ldr, d0, v0, [x21, #8]
        vld     ldr, d16, v16, [x21, #8]
        vld     ldr, s0, v0, [x21, #4]
        vld     ldr, s16, v16, [x21, #4]
        vld     ldr, h0, v0, [x21, #2]
        vld     ldr, h16, v16, [x21, #2]
        vld     ldr, b0, v0, [x21, #1]
        vld     ldr, b16, v16, [x21, #1]
        vld     ldur, d0, v0, [x21, #3]
        vld     ldur, d16, v16, [x21, #3]
        vld     ldur, s0, v0, [x21, #5]
        vld     ldur, s16, v16, [x21, #5]
        vld     ldur, h0, v0, [x21, #7]
        vld     ldur, h16, v16, [x21, #7]
        vld     ldur, b0, v0, [x21, #9]
        vld     ldur, b16, v16, [x21, #9]

        // ---- 6. Q stores at every offset mod 16 --------------------------
        // The whole 256-byte buffer is dumped after each, so a store that
        // wrote the containing aligned quadword instead of the asked-for
        // bytes, or wrote more or fewer than 16, is visible.
        bl      reset_state
        adrp    x21, buf
        add     x21, x21, :lo12:buf
        imm64   x4, 0x1122334455667788
        imm64   x6, 0x99aabbccddeeff00
        mov     v0.d[0], x4
        mov     v0.d[1], x6
        imm64   x4, 0x0f1e2d3c4b5a6978
        imm64   x6, 0x8796a5b4c3d2e1f0
        mov     v16.d[0], x4
        mov     v16.d[1], x6
        vst     str,  q0,  [x21]
        vst     str,  q16, [x21, #16]
        vst     stur, q0,  [x21, #33]
        vst     stur, q16, [x21, #50]
        vst     stur, q0,  [x21, #67]
        vst     stur, q16, [x21, #84]
        vst     stur, q0,  [x21, #101]
        vst     stur, q16, [x21, #118]
        vst     stur, q0,  [x21, #135]
        vst     stur, q16, [x21, #152]
        vst     stur, q0,  [x21, #169]
        vst     stur, q16, [x21, #186]
        vst     stur, q0,  [x21, #203]
        vst     stur, q16, [x21, #220]
        vst     str,  q0,  [x21, #224]
        vst     str,  q16, [x21, #240]
        // Register-offset stores.
        bl      reset_state
        adrp    x21, buf
        add     x21, x21, :lo12:buf
        imm64   x4, 0x1122334455667788
        imm64   x6, 0x99aabbccddeeff00
        mov     v0.d[0], x4
        mov     v0.d[1], x6
        mov     v16.d[0], x6
        mov     v16.d[1], x4
        imm64   x2, 5
        vst     str, q0, [x21, x2]
        imm64   x2, 0xdeadbeef00000045
        vst     str, q16, [x21, w2, uxtw]
        imm64   x2, 6
        vst     str, q0, [x21, x2, lsl #4]
        // Pair stores, including the writeback forms.
        bl      reset_state
        adrp    x21, buf
        add     x21, x21, :lo12:buf
        imm64   x4, 0x0102030405060708
        imm64   x6, 0x1112131415161718
        mov     v0.d[0], x4
        mov     v0.d[1], x6
        imm64   x4, 0x2122232425262728
        imm64   x6, 0x3132333435363738
        mov     v1.d[0], x4
        mov     v1.d[1], x6
        imm64   x4, 0x4142434445464748
        imm64   x6, 0x5152535455565758
        mov     v16.d[0], x4
        mov     v16.d[1], x6
        imm64   x4, 0x6162636465666768
        imm64   x6, 0x7172737475767778
        mov     v17.d[0], x4
        mov     v17.d[1], x6
        vst     stp, q0,  q1,  [x21]
        vst     stp, q16, q17, [x21, #32]
        vst     stnp, q0, q1,  [x21, #64]
        mov     x5, x21
        stp     q16, q17, [x5, #96]!
        mov     x1, x5
        mov     x3, xzr
        bl      dump
        bl      dumpbuf
        mov     x5, x21
        add     x5, x5, #160
        stp     q0, q1, [x5], #32
        mov     x1, x5
        mov     x3, xzr
        bl      dump
        bl      dumpbuf

        // ---- 7. Element order: store once, read back at every width -------
        bl      reset_state
        adrp    x21, buf
        add     x21, x21, :lo12:buf
        imm64   x4, 0x0807060504030201
        imm64   x6, 0x100f0e0d0c0b0a09
        mov     v0.d[0], x4
        mov     v0.d[1], x6
        str     q0, [x21, #32]
        // Doublewords, words, halfwords and bytes out of the stored image.
        ldr     x1, [x21, #32]
        ldr     x3, [x21, #40]
        bl      dump
        ldr     w1, [x21, #32]
        ldr     w3, [x21, #44]
        bl      dump
        ldrh    w1, [x21, #32]
        ldrh    w3, [x21, #46]
        bl      dump
        ldrb    w1, [x21, #32]
        ldrb    w3, [x21, #47]
        bl      dump
        // And back through the vector forms, including the half widths, which
        // must take the LOW bytes of the stored image.
        vld     ldr, d1, v1, [x21, #32]
        vld     ldr, d17, v17, [x21, #32]
        vld     ldr, s1, v1, [x21, #32]
        vld     ldr, s17, v17, [x21, #32]
        vld     ldr, h1, v1, [x21, #32]
        vld     ldr, h17, v17, [x21, #32]
        vld     ldr, b1, v1, [x21, #32]
        vld     ldr, b17, v17, [x21, #32]
        // The mirror: build the image with GPR stores, read it as a vector.
        imm64   x4, 0xf0e1d2c3b4a59687
        str     x4, [x21, #64]
        imm64   x4, 0x78695a4b3c2d1e0f
        str     x4, [x21, #72]
        vld     ldr, q1, v1, [x21, #64]
        vld     ldr, q17, v17, [x21, #64]

        // ---- 8. Page crossing --------------------------------------------
        // vbuf is page-aligned; x24 is the boundary and x25 is 16 bytes below
        // it. Every access here spans the two pages. The bases are re-derived
        // because the reset_state calls above zero X0-X29.
        adrp    x23, vbuf
        add     x23, x23, :lo12:vbuf
        add     x24, x23, #4096
        sub     x25, x24, #16
        vld     ldr, q0, v0, [x25]
        vld     ldur, q0, v0, [x24, #-15]
        vld     ldur, q16, v16, [x24, #-15]
        vld     ldur, q0, v0, [x24, #-8]
        vld     ldur, q16, v16, [x24, #-8]
        vld     ldur, q0, v0, [x24, #-1]
        vld     ldur, q16, v16, [x24, #-1]
        imm64   x2, 4089
        vld     ldr, q0, v0, [x23, x2]
        vld     ldr, q16, v16, [x23, x2]
        vpoison
        ldp     q0, q1, [x24, #-16]
        vrep    v0
        vrep    v1
        vpoison
        ldp     q16, q17, [x24, #-16]
        vrep    v16
        vrep    v17
        // Stores across the boundary, read back as doublewords either side.
        imm64   x4, 0xaabbccddeeff0011
        imm64   x6, 0x2233445566778899
        mov     v0.d[0], x4
        mov     v0.d[1], x6
        stur    q0, [x24, #-9]
        ldr     x1, [x24, #-16]
        ldr     x3, [x24, #-8]
        bl      dump
        ldr     x1, [x24]
        ldr     x3, [x24, #8]
        bl      dump
        mov     v16.d[0], x6
        mov     v16.d[1], x4
        stur    q16, [x24, #-3]
        ldr     x1, [x24, #-8]
        ldr     x3, [x24]
        bl      dump
        ldr     x1, [x24, #8]
        ldr     x3, [x24, #16]
        bl      dump

        // ---- 9. LD1/ST1 multiple structures, every element size -----------
        bl      reset_state
        adrp    x21, buf
        add     x21, x21, :lo12:buf
        vpoison
        ld1     {v0.16b}, [x21]
        vrep    v0
        vpoison
        ld1     {v16.16b}, [x21]
        vrep    v16
        vpoison
        ld1     {v0.8h, v1.8h}, [x21]
        vrep    v0
        vrep    v1
        vpoison
        ld1     {v16.8h, v17.8h}, [x21]
        vrep    v16
        vrep    v17
        vpoison
        ld1     {v0.4s, v1.4s, v2.4s}, [x21]
        vrep    v0
        vrep    v1
        vrep    v2
        vpoison
        ld1     {v0.2d, v1.2d, v2.2d, v3.2d}, [x21]
        vrep    v0
        vrep    v1
        vrep    v2
        vrep    v3
        vpoison
        ld1     {v0.8b, v1.8b}, [x21]
        vrep    v0
        vrep    v1
        vpoison
        ld1     {v16.4h, v17.4h}, [x21]
        vrep    v16
        vrep    v17
        vpoison
        ld1     {v0.2s}, [x21]
        vrep    v0
        vpoison
        ld1     {v16.1d}, [x21]
        vrep    v16
        // Interleaved: LD2/LD3/LD4.
        vpoison
        ld2     {v0.16b, v1.16b}, [x21]
        vrep    v0
        vrep    v1
        vpoison
        ld2     {v16.4s, v17.4s}, [x21]
        vrep    v16
        vrep    v17
        vpoison
        ld3     {v0.8h, v1.8h, v2.8h}, [x21]
        vrep    v0
        vrep    v1
        vrep    v2
        vpoison
        ld4     {v0.4s, v1.4s, v2.4s, v3.4s}, [x21]
        vrep    v0
        vrep    v1
        vrep    v2
        vrep    v3
        // The store side, through the buffer.
        bl      reset_state
        adrp    x21, buf
        add     x21, x21, :lo12:buf
        imm64   x4, 0x0102030405060708
        imm64   x6, 0x1112131415161718
        mov     v0.d[0], x4
        mov     v0.d[1], x6
        imm64   x4, 0x2122232425262728
        imm64   x6, 0x3132333435363738
        mov     v1.d[0], x4
        mov     v1.d[1], x6
        imm64   x4, 0x4142434445464748
        imm64   x6, 0x5152535455565758
        mov     v2.d[0], x4
        mov     v2.d[1], x6
        imm64   x4, 0x6162636465666768
        imm64   x6, 0x7172737475767778
        mov     v3.d[0], x4
        mov     v3.d[1], x6
        mov     v16.16b, v0.16b
        mov     v17.16b, v1.16b
        vst     st1, {v0.16b}, [x21]
        vst     st1, {v16.16b, v17.16b}, [x21]
        vst     st1, {v0.4s, v1.4s, v2.4s}, [x21]
        vst     st1, {v0.2d, v1.2d, v2.2d, v3.2d}, [x21]
        vst     st1, {v16.8b, v17.8b}, [x21]
        vst     st2, {v0.16b, v1.16b}, [x21]
        vst     st2, {v16.4s, v17.4s}, [x21]
        vst     st3, {v0.8h, v1.8h, v2.8h}, [x21]
        vst     st4, {v0.4s, v1.4s, v2.4s, v3.4s}, [x21]

        // ---- 10. The whole register file, both halves, in one line --------
        // Everything above reports two doublewords at a time; this proves the
        // low bank and the VMX half hold what they should simultaneously, and
        // that nothing above leaked between them.
        bl      reset_state
        adrp    x21, buf
        add     x21, x21, :lo12:buf
        ldr     q0,  [x21]
        ldr     q1,  [x21, #16]
        ldr     q2,  [x21, #32]
        ldr     q3,  [x21, #48]
        ldr     q4,  [x21, #64]
        ldr     q5,  [x21, #80]
        ldr     q6,  [x21, #96]
        ldr     q7,  [x21, #112]
        ldr     q8,  [x21, #128]
        ldr     q9,  [x21, #144]
        ldr     q10, [x21, #160]
        ldr     q11, [x21, #176]
        ldr     q12, [x21, #192]
        ldr     q13, [x21, #208]
        ldr     q14, [x21, #224]
        ldr     q15, [x21, #240]
        ldur    q16, [x21, #1]
        ldur    q17, [x21, #17]
        ldur    q18, [x21, #33]
        ldur    q19, [x21, #49]
        ldur    q20, [x21, #65]
        ldur    q21, [x21, #81]
        ldur    q22, [x21, #97]
        ldur    q23, [x21, #113]
        ldur    q24, [x21, #129]
        ldur    q25, [x21, #145]
        ldur    q26, [x21, #161]
        ldur    q27, [x21, #177]
        ldur    q28, [x21, #193]
        ldur    q29, [x21, #209]
        ldur    q30, [x21, #225]
        ldur    q31, [x21, #239]
        bl      vdump
        // And the same image written straight back out, 16 registers at a
        // time, so every one of the 32 has been both a load destination and a
        // store source in its own half of the file.
        bl      reset_state
        adrp    x21, buf
        add     x21, x21, :lo12:buf
        str     q16, [x21]
        str     q17, [x21, #16]
        str     q18, [x21, #32]
        str     q19, [x21, #48]
        str     q20, [x21, #64]
        str     q21, [x21, #80]
        str     q22, [x21, #96]
        str     q23, [x21, #112]
        str     q24, [x21, #128]
        str     q25, [x21, #144]
        str     q26, [x21, #160]
        str     q27, [x21, #176]
        str     q28, [x21, #192]
        str     q29, [x21, #208]
        str     q30, [x21, #224]
        str     q31, [x21, #240]
        bl      dumpbuf
        bl      vdump

        test_exit

        .bss
        .balign 4096
        .global vbuf
vbuf:   .skip   (13 * 4096)
