// SPDX-License-Identifier: MIT
        .section .data.blob, "aw", @progbits
        .balign 64
        .globl  BigBlob
BigBlob:
        .incbin "madvfile_blob.bin"
        .globl  BigBlobEnd
BigBlobEnd:
