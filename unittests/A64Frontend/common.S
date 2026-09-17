// SPDX-License-Identifier: MIT
//
// Runtime shared by the A64Frontend test programs. Built with
// `gcc -nostdlib -static`, so the binary is loaded at the same address on the
// Pi and under POWERarm and every static address (buffers, the private stack,
// ADR/ADRP results) prints identically on both.
//
//   test_start   Macro: switches SP to a private static stack, zeroes
//                X0-X29 and NZCV, fills `buf` with a byte pattern.
//   dump         Prints "#<n> x0..x29 sp nzcv" as one hex line and returns
//                with every register, SP and NZCV exactly as on entry
//                (X30 excepted: the caller's BL wrote it).
//   dumpbuf      Prints the 256 bytes of `buf` as hex; preserves state like dump.
//   vdump        Prints "v" then V0..V31 (each as 32 hex digits, high half
//                first), FPCR and FPSR with the cumulative exception bits
//                (IOC, DZC, OFC, UFC, IXC, IDC) masked out; preserves state
//                like dump.
//   test_exit    Macro: exit(0).
//   check        Self-check: X0 = value, X1 = expected, X2 = pointer to a
//                NUL-terminated name. Prints "PASS name" or "FAIL name got
//                <hex> want <hex>". Clobbers X0-X17 and NZCV.

        .macro test_start
        .global _start
        .text
_start:
        adrp    x9, stack_top
        add     x9, x9, :lo12:stack_top
        mov     sp, x9
        bl      reset_state
        .endm

        .macro test_exit
        mov     x0, #0
        mov     x8, #93
        svc     #0
        .endm

        .text
reset_state:
        // Fill buf with (i * 37 + 11) & 0xff.
        adrp    x9, buf
        add     x9, x9, :lo12:buf
        mov     x10, #0
        mov     x11, #11
1:      strb    w11, [x9, x10]
        add     w11, w11, #37
        add     x10, x10, #1
        cmp     x10, #256
        b.ne    1b
        mov     x0, #0
        mov     x1, #0
        mov     x2, #0
        mov     x3, #0
        mov     x4, #0
        mov     x5, #0
        mov     x6, #0
        mov     x7, #0
        mov     x8, #0
        mov     x9, #0
        mov     x10, #0
        mov     x11, #0
        mov     x12, #0
        mov     x13, #0
        mov     x14, #0
        mov     x15, #0
        mov     x16, #0
        mov     x17, #0
        mov     x18, #0
        mov     x19, #0
        mov     x20, #0
        mov     x21, #0
        mov     x22, #0
        mov     x23, #0
        mov     x24, #0
        mov     x25, #0
        mov     x26, #0
        mov     x27, #0
        mov     x28, #0
        mov     x29, #0
        msr     nzcv, x0
        ret

// hex64: writes 16 lowercase hex digits of X12 at X9, advancing X9. Clobbers X13, X14, NZCV.
hex64:
        mov     x13, #60
1:      lsr     x14, x12, x13
        and     x14, x14, #0xf
        cmp     x14, #10
        b.lo    2f
        add     x14, x14, #('a' - 10)
        b       3f
2:      add     x14, x14, #'0'
3:      strb    w14, [x9], #1
        subs    x13, x13, #4
        b.ge    1b
        ret

// Saves X0-X29, SP-on-entry, NZCV and X30 in a 288-byte frame: X0..X29 at
// [sp, #0..#232], entry SP at #240, NZCV at #248, X30 at #256.
        .macro save_state
        sub     sp, sp, #288
        stp     x0, x1, [sp, #0]
        stp     x2, x3, [sp, #16]
        stp     x4, x5, [sp, #32]
        stp     x6, x7, [sp, #48]
        stp     x8, x9, [sp, #64]
        stp     x10, x11, [sp, #80]
        stp     x12, x13, [sp, #96]
        stp     x14, x15, [sp, #112]
        stp     x16, x17, [sp, #128]
        stp     x18, x19, [sp, #144]
        stp     x20, x21, [sp, #160]
        stp     x22, x23, [sp, #176]
        stp     x24, x25, [sp, #192]
        stp     x26, x27, [sp, #208]
        stp     x28, x29, [sp, #224]
        add     x0, sp, #288
        str     x0, [sp, #240]
        mrs     x0, nzcv
        str     x0, [sp, #248]
        str     x30, [sp, #256]
        .endm

        .macro restore_state
        ldr     x0, [sp, #248]
        msr     nzcv, x0
        ldr     x30, [sp, #256]
        ldp     x0, x1, [sp, #0]
        ldp     x2, x3, [sp, #16]
        ldp     x4, x5, [sp, #32]
        ldp     x6, x7, [sp, #48]
        ldp     x8, x9, [sp, #64]
        ldp     x10, x11, [sp, #80]
        ldp     x12, x13, [sp, #96]
        ldp     x14, x15, [sp, #112]
        ldp     x16, x17, [sp, #128]
        ldp     x18, x19, [sp, #144]
        ldp     x20, x21, [sp, #160]
        ldp     x22, x23, [sp, #176]
        ldp     x24, x25, [sp, #192]
        ldp     x26, x27, [sp, #208]
        ldp     x28, x29, [sp, #224]
        add     sp, sp, #288
        .endm

// write(1, outbuf, X9 - outbuf). Clobbers X0-X2, X8.
flush_line:
        adrp    x1, outbuf
        add     x1, x1, :lo12:outbuf
        sub     x2, x9, x1
        mov     x0, #1
        mov     x8, #64
        svc     #0
        ret

        .global dump
dump:
        save_state
        adrp    x9, outbuf
        add     x9, x9, :lo12:outbuf
        mov     w10, #'#'
        strb    w10, [x9], #1
        adrp    x10, dump_counter
        ldr     x12, [x10, :lo12:dump_counter]
        add     x12, x12, #1
        str     x12, [x10, :lo12:dump_counter]
        bl      hex64
        mov     x15, #0
1:      mov     w10, #' '
        strb    w10, [x9], #1
        ldr     x12, [sp, x15, lsl #3]
        bl      hex64
        add     x15, x15, #1
        cmp     x15, #32
        b.ne    1b
        mov     w10, #'\n'
        strb    w10, [x9], #1
        bl      flush_line
        restore_state
        ret

        .global dumpbuf
dumpbuf:
        save_state
        adrp    x9, outbuf
        add     x9, x9, :lo12:outbuf
        adrp    x15, buf
        add     x15, x15, :lo12:buf
        mov     x16, #0
1:      ldrb    w12, [x15, x16]
        lsr     w13, w12, #4
        and     w14, w12, #0xf
        cmp     w13, #10
        add     w13, w13, #'0'
        b.lo    2f
        add     w13, w13, #('a' - '0' - 10)
2:      strb    w13, [x9], #1
        cmp     w14, #10
        add     w14, w14, #'0'
        b.lo    3f
        add     w14, w14, #('a' - '0' - 10)
3:      strb    w14, [x9], #1
        add     x16, x16, #1
        tst     x16, #31
        b.ne    4f
        mov     w10, #'\n'
        strb    w10, [x9], #1
4:      cmp     x16, #256
        b.ne    1b
        bl      flush_line
        restore_state
        ret

        .global vdump
vdump:
        save_state
        sub     sp, sp, #512
        str     q0, [sp, #0]
        str     q1, [sp, #16]
        str     q2, [sp, #32]
        str     q3, [sp, #48]
        str     q4, [sp, #64]
        str     q5, [sp, #80]
        str     q6, [sp, #96]
        str     q7, [sp, #112]
        str     q8, [sp, #128]
        str     q9, [sp, #144]
        str     q10, [sp, #160]
        str     q11, [sp, #176]
        str     q12, [sp, #192]
        str     q13, [sp, #208]
        str     q14, [sp, #224]
        str     q15, [sp, #240]
        str     q16, [sp, #256]
        str     q17, [sp, #272]
        str     q18, [sp, #288]
        str     q19, [sp, #304]
        str     q20, [sp, #320]
        str     q21, [sp, #336]
        str     q22, [sp, #352]
        str     q23, [sp, #368]
        str     q24, [sp, #384]
        str     q25, [sp, #400]
        str     q26, [sp, #416]
        str     q27, [sp, #432]
        str     q28, [sp, #448]
        str     q29, [sp, #464]
        str     q30, [sp, #480]
        str     q31, [sp, #496]
        adrp    x9, outbuf
        add     x9, x9, :lo12:outbuf
        mov     w10, #'v'
        strb    w10, [x9], #1
        mov     x15, #0
1:      mov     w10, #' '
        strb    w10, [x9], #1
        lsl     x16, x15, #4
        add     x16, x16, #8
        ldr     x12, [sp, x16]
        bl      hex64
        lsl     x16, x15, #4
        ldr     x12, [sp, x16]
        bl      hex64
        add     x15, x15, #1
        cmp     x15, #32
        b.ne    1b
        mov     w10, #' '
        strb    w10, [x9], #1
        mrs     x12, fpcr
        bl      hex64
        mov     w10, #' '
        strb    w10, [x9], #1
        mrs     x12, fpsr
        mov     x14, #0x9f
        bic     x12, x12, x14
        bl      hex64
        mov     w10, #'\n'
        strb    w10, [x9], #1
        bl      flush_line
        add     sp, sp, #512
        restore_state
        ret

        .global check
check:
        mov     x16, x30
        mov     x10, x0
        mov     x11, x1
        adrp    x9, outbuf
        add     x9, x9, :lo12:outbuf
        cmp     x10, x11
        b.ne    1f
        mov     w12, #'P'
        strb    w12, [x9], #1
        mov     w12, #'A'
        strb    w12, [x9], #1
        mov     w12, #'S'
        strb    w12, [x9], #1
        strb    w12, [x9], #1
        b       2f
1:      mov     w12, #'F'
        strb    w12, [x9], #1
        mov     w12, #'A'
        strb    w12, [x9], #1
        mov     w12, #'I'
        strb    w12, [x9], #1
        mov     w12, #'L'
        strb    w12, [x9], #1
2:      mov     w12, #' '
        strb    w12, [x9], #1
3:      ldrb    w12, [x2], #1
        cbz     w12, 4f
        strb    w12, [x9], #1
        b       3b
4:      cmp     x10, x11
        b.eq    5f
        mov     w12, #' '
        strb    w12, [x9], #1
        mov     x12, x10
        bl      hex64
        mov     w12, #' '
        strb    w12, [x9], #1
        mov     x12, x11
        bl      hex64
5:      mov     w12, #'\n'
        strb    w12, [x9], #1
        bl      flush_line
        mov     x30, x16
        ret

        // A non-empty .data keeps p_filesz of the RW segment above zero. With
        // p_filesz == 0 and a 4K-aligned p_vaddr, the ELF loader on a 64K host
        // leaves the first host page of .bss unmapped (see README.md).
        .data
data_marker:
        .quad   0x5a5a5a5a5a5a5a5a

        .bss
        .balign 16
        .global buf
buf:    .skip   256
outbuf: .skip   2048
dump_counter: .skip 8
        .skip   65536
        .balign 16
stack_top:
