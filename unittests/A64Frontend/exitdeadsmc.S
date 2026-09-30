// SPDX-License-Identifier: MIT
//
// NZCV exit-deadness when the guest REWRITES the code the verdict was taken
// from (POWERARM_NZCVEXITDEAD, NZCV-LIVENESS.md 7.5).
//
// The peek reads guest words OUTSIDE the compile unit and keeps a DEAD verdict
// that is only as good as those words. 7.5's answer is that every page the scan
// read joins the unit's SMC footprint and the decoded extent widens to the hull
// of unit and witnesses, so a guest write to a witness costs a recompile rather
// than a stale assumption. Nothing tested that. exitdead.S never writes a byte
// of code, and a headless screenshot of a static page never does either --
// which is why this class of bug survived both. A JavaScript JIT does it
// thousands of times a second.
//
// So this is a JIT in miniature: a stub and its branch target are BUILT AT
// RUNTIME into one RWX region, called, then rewritten in place and called
// again. The rewrite turns the target's first NZCV operation from a WRITER
// (which makes the exit's flags dead, and lets the producer before the exit go)
// into a READER of exactly those flags. Whatever the emulator concluded from
// the old bytes, the answer after the rewrite is the architecture's.
//
// Four shapes, because the machinery that has to notice the write is different
// for each:
//   * the target's FIRST word rewritten, target ABOVE the stub;
//   * the same with the target BELOW the stub -- the witness is below the
//     unit's entry, where a footprint recorded as [entry, entry+size) does not
//     reach;
//   * a word ONE IN from the target, so the byte that changed is a witness the
//     scan walked through rather than the target itself;
//   * the rewrite done after the stub has run enough times to be linked, so the
//     invalidation has to reach a unit that is already wired to its successor.
//
// Every check is architectural: cmp x3,x3 leaves N0 Z1 C1 V0 = 0x6 whatever the
// incoming flags were, and the reader form returns the nibble the stub set. The
// canary mode makes a stale assumption unmistakable -- it writes N1 Z0 C1 V0 =
// 0xA at a dead exit, and 0xA is not a value any check here expects.
#include "common.S"

        .set    STUB_FWD,  0x0000       // stub, branches forward to TGT_HI
        .set    TGT_LO,    0x0400       // target below STUB_BWD
        .set    TGT_HI,    0x0800       // target above STUB_FWD
        .set    STUB_BWD,  0x1000       // stub, branches backward to TGT_LO

        // One check: X0 = value, \expected = what the hardware produces.
        .macro CHECK name, expected
        adr     x2, 90f
        ldr     x1, =\expected
        bl      check
        b       91f
90:     .asciz  "\name"
        .balign 4
91:
        .endm

        // Copy \n words from \src into smcbuf + \off and make them executable.
        .macro EMIT off, src, n
        adrp    x0, smcbuf
        add     x0, x0, :lo12:smcbuf
        add     x0, x0, #\off
        adr     x1, \src
        mov     x2, #\n
        bl      emit_words
        .endm

        // Call the stub at \off with the flag nibble \v; result in X0.
        .macro CALL off, v
        adrp    x9, smcbuf
        add     x9, x9, :lo12:smcbuf
        add     x9, x9, #\off
        mov     x0, #\v
        blr     x9
        .endm

        test_start

        // The region is .bss, so make it executable before anything is built.
        adrp    x0, smcbuf
        add     x0, x0, :lo12:smcbuf
        mov     x1, #65536
        mov     x2, #7                  // PROT_READ|PROT_WRITE|PROT_EXEC
        mov     x8, #226                // mprotect
        svc     #0
        cmp     x0, #0
        b.eq    1f
        // Without an executable region there is nothing to test; say so once
        // rather than reporting passes that never ran.
        mov     x0, #0
        mov     x1, #1
        adr     x2, mprotect_failed
        bl      check
        test_exit
mprotect_failed:
        .asciz  "smc-mprotect"
        .balign 4
1:

        // ==================================================================
        // 1. The target's first word, rewritten from a writer to a reader.
        //    The target is ABOVE the stub.
        // ==================================================================
        EMIT    STUB_FWD, tpl_stub_fwd, 4
        EMIT    TGT_HI, tpl_wr, 4
        CALL    STUB_FWD, 0x9
        CHECK   smc-hi-writer, 0x6
        EMIT    TGT_HI, tpl_rd, 4
        CALL    STUB_FWD, 0x9
        CHECK   smc-hi-rewritten-to-reader, 0x9
        EMIT    TGT_HI, tpl_wr, 4
        CALL    STUB_FWD, 0x5
        CHECK   smc-hi-back-to-writer, 0x6
        EMIT    TGT_HI, tpl_rd, 4
        CALL    STUB_FWD, 0x5
        CHECK   smc-hi-reader-again, 0x5

        // ==================================================================
        // 2. The same, with the target BELOW the stub: the witness sits under
        //    the unit's entry.
        // ==================================================================
        EMIT    STUB_BWD, tpl_stub_bwd, 4
        EMIT    TGT_LO, tpl_wr, 4
        CALL    STUB_BWD, 0x9
        CHECK   smc-lo-writer, 0x6
        EMIT    TGT_LO, tpl_rd, 4
        CALL    STUB_BWD, 0x9
        CHECK   smc-lo-rewritten-to-reader, 0x9
        EMIT    TGT_LO, tpl_wr, 4
        CALL    STUB_BWD, 0x5
        CHECK   smc-lo-back-to-writer, 0x6
        EMIT    TGT_LO, tpl_rd, 4
        CALL    STUB_BWD, 0x5
        CHECK   smc-lo-reader-again, 0x5

        // ==================================================================
        // 3. A word ONE IN from the target: the scan walked through a NOP to
        //    reach the writer, and the NOP's successor is what changes.
        // ==================================================================
        EMIT    TGT_HI, tpl_nop_wr, 5
        CALL    STUB_FWD, 0x9
        CHECK   smc-inner-writer, 0x6
        EMIT    TGT_HI, tpl_nop_rd, 5
        CALL    STUB_FWD, 0x9
        CHECK   smc-inner-rewritten-to-reader, 0x9

        // ==================================================================
        // 4. The rewrite after the stub is hot: run it enough times that the
        //    exit is linked to its successor, then change the successor.
        // ==================================================================
        EMIT    TGT_HI, tpl_wr, 4
        mov     x23, #200
2:      CALL    STUB_FWD, 0x9
        subs    x23, x23, #1
        b.ne    2b
        CHECK   smc-hot-writer, 0x6
        EMIT    TGT_HI, tpl_rd, 4
        CALL    STUB_FWD, 0x9
        CHECK   smc-hot-rewritten-to-reader, 0x9

        test_exit

// ======================================================================
// emit_words(X0 = dst, X1 = src, X2 = word count)
//
// Writes the words, then announces them the way the architecture requires:
// clean to the point of unification, barrier, invalidate the instruction
// cache, barrier, ISB. Exactly __builtin___clear_cache's sequence.
// ======================================================================
emit_words:
        mov     x10, x0
        mov     x11, x2
1:      ldr     w12, [x1], #4
        str     w12, [x0], #4
        subs    x11, x11, #1
        b.ne    1b
        mov     x11, x2
        mov     x13, x10
2:      dc      cvau, x13
        add     x13, x13, #4
        subs    x11, x11, #1
        b.ne    2b
        dsb     ish
        mov     x11, x2
        mov     x13, x10
3:      ic      ivau, x13
        add     x13, x13, #4
        subs    x11, x11, #1
        b.ne    3b
        dsb     ish
        isb
        ret

// ======================================================================
// The templates. The stubs' final word is a `b` whose displacement is a
// compile-time constant, because both ends live at fixed offsets in the
// region -- the same thing a JIT does when it emits a jump between two
// blocks it has just placed.
// ======================================================================
        .balign 4
tpl_stub_fwd:
        mov     x21, x30
        lsl     x20, x0, #28
        msr     nzcv, x20
        .word   0x14000000 | (((TGT_HI - (STUB_FWD + 12)) / 4) & 0x3FFFFFF)

tpl_stub_bwd:
        mov     x21, x30
        lsl     x20, x0, #28
        msr     nzcv, x20
        .word   0x14000000 | (((TGT_LO - (STUB_BWD + 12)) / 4) & 0x3FFFFFF)

        // A full NZCV writer first: the scan calls the target dead, and the
        // answer is the writer's own whatever the incoming flags were.
tpl_wr:
        cmp     x3, x3
        mrs     x0, nzcv
        lsr     x0, x0, #28
        br      x21

        // A reader first: the flags the stub set have to arrive.
tpl_rd:
        mrs     x0, nzcv
        lsr     x0, x0, #28
        br      x21
        nop

tpl_nop_wr:
        nop
        cmp     x3, x3
        mrs     x0, nzcv
        lsr     x0, x0, #28
        br      x21

tpl_nop_rd:
        nop
        nop
        mrs     x0, nzcv
        lsr     x0, x0, #28
        br      x21

        .bss
        .balign 65536
smcbuf: .skip   65536
