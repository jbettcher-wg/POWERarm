// SPDX-License-Identifier: MIT
//
// ELF loader: .bss that begins inside a host page the file mapping already
// covers. `tdata` is the only .data and is aligned to 64K, so the RW PT_LOAD
// has a host-aligned p_vaddr and p_offset on a 64K host and is mapped straight
// from the file. The file continues past p_filesz with `.loaderpad`, 36K of
// 0xa5 bytes, so every byte of the rest of that host page is non-zero in the
// file. The kernel zeroes everything after p_filesz; the 60K bytes after
// `tdata` (the rest of the host page, then common.S's .bss) must read as zero
// and be writable.
#include "common.S"

        .global _start
        .text
_start:
        adrp    x9, stack_top
        add     x9, x9, :lo12:stack_top
        mov     sp, x9

        adrp    x3, tdata_end
        add     x3, x3, :lo12:tdata_end
        mov     x4, #0
        mov     x5, #0
1:      ldrb    w6, [x3, x5]
        orr     x4, x4, x6
        add     x5, x5, #1
        cmp     x5, #0xf000
        b.ne    1b
        mov     x0, x4
        mov     x1, #0
        adr     x2, name_zero
        bl      check

        adrp    x3, tdata_end
        add     x3, x3, :lo12:tdata_end
        mov     x4, #0x5c
        strb    w4, [x3]
        strb    w4, [x3, #0xfff]
        mov     x5, #0xefff
        strb    w4, [x3, x5]
        ldrb    w0, [x3]
        ldrb    w6, [x3, #0xfff]
        add     x0, x0, x6
        ldrb    w6, [x3, x5]
        add     x0, x0, x6
        mov     x1, #(0x5c * 3)
        adr     x2, name_write
        bl      check

        adrp    x3, tdata
        ldr     x0, [x3, :lo12:tdata]
        ldr     x1, =0x1122334455667788
        adr     x2, name_data
        bl      check

        test_exit
        .ltorg

name_zero:  .asciz "bss-reads-zero"
name_write: .asciz "bss-writable"
name_data:  .asciz "data-intact"

        .data
        .balign 0x10000
tdata:  .quad   0x1122334455667788
tdata_end:

        .section .loaderpad, "", %progbits
        .fill   0x9000, 1, 0xa5
