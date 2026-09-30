// SPDX-License-Identifier: MIT
//
// NZCV live in at a compile unit's entry: three units that differ in it, and
// the guest-visible half of the property they are about.
//
// DeadFlagCalculationElimination records one bit per compile unit,
// EntryNZCVLiveIn, meaning "some path from the entry block reads an NZCV bit
// before writing it". It has no guest-visible effect today -- the pass seeds
// FLAG_ALL at every unit exit, so the flags are always delivered whatever the
// bit says -- and the only thing that reads it is the JIT's refusal to
// direct-link into such a unit. It was computed from the ENTRY BLOCK ALONE,
// which gets the first of these three wrong: 17 of cc1's 110,706 units at -O2
// are that shape (docs/powerarm/research/power-isa/NZCV-LIVENESS.md 4.2).
//
//   nzcv_late    The entry block ends in `cbz`, which reads no flag at all. The
//                `b.ne` that reads Z is in a LATER block of the same unit, so
//                an entry-block-only rule says "does not read flags on entry"
//                and is wrong. This is the case under test.
//   nzcv_early   `b.ne` is the entry block's first instruction. Reads on entry,
//                and the entry-block-only rule already saw it -- the control
//                that proves the check is not vacuous.
//   nzcv_writer  The entry block's `cmp` writes every NZCV bit before the
//                `b.ne` reads one, so nothing is read from entry. The control
//                that proves the forward reach is not simply always true.
//
// Each is reached by `bl`, which is what makes it a unit entry rather than more
// blocks of the caller's unit.
//
// Two things are checked, in two places:
//
//  1. Here, guest-visibly: the flags set before each `bl` must reach the reader
//     inside the callee, for both values of Z. That is what the FLAG_ALL exit
//     seeding buys, and it must keep holding whatever the bit says.
//  2. In run.sh (nzcv_entry_livein), which runs this program again with the IR
//     dump on and requires the three units' EntryNZCVLiveIn to be 1, 1 and 0.
//     The first line of output is the three entry addresses, because that is
//     how run.sh finds the units in the dump; `-nostdlib -static` puts them at
//     the same address on the Pi and under POWERarm, like every other static
//     address this suite prints.
//
// Differential: every line is the Pi's own output.
#include "common.S"

        // NZCV = \v << 28 (bit 30 is Z), then call \fn with x0 = \x0.
        .macro callz fn, v, x0
        mov     x1, #0
        mov     x0, #(\v << 28)
        msr     nzcv, x0
        mov     x0, #\x0
        bl      \fn
        .endm

        test_start

        // The three entry addresses, for run.sh. `A` so the line is findable.
        adr     x0, nzcv_late
        adr     x1, nzcv_early
        adr     x2, nzcv_writer
        bl      putaddrs

        // nzcv_late with Z clear (NE) takes the b.ne, with Z set (EQ) falls
        // through. Both answers come from flags set before the bl.
        callz   nzcv_late, 0x0, 0
        adr     x2, s_late_ne
        mov     x0, x1
        mov     x1, #0x33
        bl      check

        callz   nzcv_late, 0x4, 0
        adr     x2, s_late_eq
        mov     x0, x1
        mov     x1, #0x22
        bl      check

        // x0 non-zero: the cbz is not taken and the later block is never
        // reached, so the flags are not read at all. The unit is the same one.
        callz   nzcv_late, 0x0, 1
        adr     x2, s_late_nottaken
        mov     x0, x1
        mov     x1, #0x11
        bl      check

        callz   nzcv_early, 0x0, 0
        adr     x2, s_early_ne
        mov     x0, x1
        mov     x1, #0x55
        bl      check

        callz   nzcv_early, 0x4, 0
        adr     x2, s_early_eq
        mov     x0, x1
        mov     x1, #0x44
        bl      check

        // The writer's own cmp decides, so Z on entry must NOT matter: both
        // calls pass x0 = 0 and differ only in the incoming Z.
        callz   nzcv_writer, 0x0, 0
        adr     x2, s_writer_zclear
        mov     x0, x1
        mov     x1, #0x66
        bl      check

        callz   nzcv_writer, 0x4, 0
        adr     x2, s_writer_zset
        mov     x0, x1
        mov     x1, #0x66
        bl      check

        callz   nzcv_writer, 0x0, 1
        adr     x2, s_writer_ne
        mov     x0, x1
        mov     x1, #0x77
        bl      check

        test_exit

// ---------------------------------------------------------------------------
// The three units. Nothing here may write an NZCV bit before the read, and the
// `ret`s keep each leg a block of its own.
// ---------------------------------------------------------------------------

        .balign 4
        .global nzcv_late
nzcv_late:
        cbz     x0, 1f                  // entry block: reads no flag
        mov     x1, #0x11
        ret
1:      b.ne    2f                      // later block: reads Z from the caller
        mov     x1, #0x22
        ret
2:      mov     x1, #0x33
        ret

        .balign 4
        .global nzcv_early
nzcv_early:
        b.ne    1f                      // entry block reads Z immediately
        mov     x1, #0x44
        ret
1:      mov     x1, #0x55
        ret

        .balign 4
        .global nzcv_writer
nzcv_writer:
        cmp     x0, #0                  // writes N, Z, C and V
        b.ne    1f
        mov     x1, #0x66
        ret
1:      mov     x1, #0x77
        ret

// putaddrs: prints "A <x0> <x1> <x2>", each 16 hex digits. hex64 and
// flush_line come from common.S; hex64 walks X12 and clobbers X13/X14/NZCV,
// flush_line clobbers X0-X2 and X8, so the three addresses are parked first.
putaddrs:
        mov     x16, x30
        mov     x17, x0
        mov     x19, x1
        mov     x20, x2
        adrp    x9, outbuf
        add     x9, x9, :lo12:outbuf
        mov     w10, #'A'
        strb    w10, [x9], #1
        mov     w10, #' '
        strb    w10, [x9], #1
        mov     x12, x17
        bl      hex64
        mov     w10, #' '
        strb    w10, [x9], #1
        mov     x12, x19
        bl      hex64
        mov     w10, #' '
        strb    w10, [x9], #1
        mov     x12, x20
        bl      hex64
        mov     w10, #'\n'
        strb    w10, [x9], #1
        bl      flush_line
        mov     x30, x16
        ret

        .section .rodata
s_late_ne:        .asciz "late-ne"
s_late_eq:        .asciz "late-eq"
s_late_nottaken:  .asciz "late-cbz-not-taken"
s_early_ne:       .asciz "early-ne"
s_early_eq:       .asciz "early-eq"
s_writer_zclear:  .asciz "writer-z-clear"
s_writer_zset:    .asciz "writer-z-set"
s_writer_ne:      .asciz "writer-ne"
