// SPDX-License-Identifier: MIT
//
// ELF loader: an RW PT_LOAD with p_filesz == 0 and a 4K-aligned p_vaddr in a
// host page of its own. The loader used to start the anonymous .bss map at
// the host page above p_vaddr, leaving the first host page of .bss unmapped
// on a 64K host.
        // The first .bss input decides the segment's p_vaddr: page-align it.
        .bss
        .balign 0x1000
#include "common.S"

        .global _start
        .text
_start:
        adrp    x9, stack_top
        add     x9, x9, :lo12:stack_top
        mov     sp, x9

        // All of buf, outbuf, dump_counter and the 64K skip after them.
        adrp    x3, buf
        add     x3, x3, :lo12:buf
        mov     x4, #0
        mov     x5, #0
        mov     x7, #0x10000
1:      ldrb    w6, [x3, x5]
        orr     x4, x4, x6
        add     x5, x5, #1
        cmp     x5, x7
        b.ne    1b
        mov     x0, x4
        mov     x1, #0
        adr     x2, name_zero
        bl      check

        // The first byte of .bss, and one in every 4K page of the scanned range.
        adrp    x3, buf
        add     x3, x3, :lo12:buf
        mov     x4, #0x3a
        mov     x5, #0
        mov     x0, #0
2:      strb    w4, [x3, x5]
        ldrb    w6, [x3, x5]
        add     x0, x0, x6
        add     x5, x5, #0x1000
        cmp     x5, x7
        b.ne    2b
        mov     x1, #(0x3a * 16)
        adr     x2, name_write
        bl      check

        test_exit
        .ltorg

name_zero:  .asciz "bss-reads-zero"
name_write: .asciz "bss-writable"
