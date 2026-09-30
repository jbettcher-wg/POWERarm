// SPDX-License-Identifier: MIT
//
// NZCV exit-deadness: the guest-visible half of the policy.
//
// POWERARM_NZCVEXITDEAD (docs/powerarm/research/power-isa/NZCV-LIVENESS.md §7)
// lets the compiler DROP the instructions that compute NZCV when a compile unit
// leaves by a constant exit whose target, scanned through the guest's own
// instructions, writes every NZCV bit before reading one. The scan is a bounded
// walk over guest words; everything it cannot prove it must refuse.
//
// Every case below sets NZCV to a known value, LEAVES THE COMPILE UNIT by one
// shape of constant exit, and reads the flags on the far side. The Pi answers
// every one from the architecture; POWERarm has to answer the same. What the
// cases exist to catch:
//
//   * a target whose FIRST flag op is a READER -- every reader kind the
//     document's §7.2 table lists that this machine can execute: B.cond, the
//     CSEL family, CCMP, ADC, and MRS NZCV. The scan must say LIVE and the
//     flags must arrive. If it wrongly says dead, `on` delivers whatever the
//     last kept producer left and `canary` delivers a DELIBERATELY WRONG
//     N-C- (0xA), so these lines flip.
//   * a target whose first flag op is a WRITER (CMP, SUBS, ANDS, FCMP, MSR
//     NZCV). The scan says DEAD and the producer goes; the answer must be
//     exactly what the writer computes, whatever the incoming flags were.
//     Under `canary` those incoming flags are wrong on purpose, so a target
//     that reads them before writing shows up here.
//   * the reader at DEPTH, behind 8 / 16 / 17 / 33 neutral instructions, and
//     behind more than the scan's per-path budget (the budget case must come
//     out conservative, i.e. the flags still arrive).
//   * the reader BEHIND something the simple scan cannot follow: a BL at the
//     target (stage 3's territory, not enabled) and an SVC at the target
//     (which the scan DOES walk through, because the kernel preserves NZCV).
//   * the target past 64 KiB, where §7.5's hull bound refuses the verdict
//     because the unit cannot carry that many witness pages for SMC.
//   * the MSR/MRS round trip at a DEAD entry (§7.6): what a target WRITES must
//     be what it READS BACK, even where the incoming value is not preserved.
//
// The exit shapes are B, BL, B.cond (taken leg), CBZ and TBZ. A conditional
// branch's NOT-TAKEN leg is always PC+4 and therefore always inside the same
// compile unit, so it cannot be a constant exit at all -- except under
// POWERARM_MAXINST=1, where every instruction is its own unit and every one of
// these arrivals becomes a constant exit. That is why this test is run in all
// three suite modes and not just the default one.
//
// Flag values: 0x6 is N0 Z1 C1 V0, 0x9 is N1 Z0 C0 V1. The canary is
// N1 Z0 C1 V0 = 0xA, which differs from both in at least two bits, so no
// reader here can be satisfied by it.
//
// Self-checking: one PASS/FAIL line per check. Differential all the same --
// every line must match the Pi byte for byte, and run.sh additionally requires
// the golden to contain no FAIL at all, so a golden taken from hardware that
// was failing cannot pass silently.
#include "common.S"

        // One check: X0 = value, \expected = what the Pi produces.
        .macro CHECK name, expected
        adr     x2, 90f
        ldr     x1, =\expected
        bl      check
        b       91f
90:     .asciz  "\name"
        .balign 4
91:
        .endm

        // ------------------------------------------------------------------
        // Exit shapes. Each sets X21 to the continuation, sets NZCV LAST
        // before the branch (nothing between the MSR and the exit may touch
        // the flags), and leaves for \tgt. Every target returns with `br x21`.
        //
        // A shape whose branch is NOT taken falls into `mov x0, #0xdead`, so a
        // mis-set condition shows up as a FAIL line rather than as a pass by
        // accident.
        // ------------------------------------------------------------------
        .macro GO_B tgt, v
        adr     x21, 71f
        mov     x20, #(\v << 28)
        msr     nzcv, x20
        b       \tgt
71:
        .endm

        .macro GO_BL tgt, v
        adr     x21, 71f
        mov     x20, #(\v << 28)
        msr     nzcv, x20
        bl      \tgt
71:
        .endm

        .macro GO_BCOND cc, tgt, v
        adr     x21, 71f
        mov     x20, #(\v << 28)
        msr     nzcv, x20
        b.\cc   \tgt
        mov     x0, #0xdead
71:
        .endm

        .macro GO_CBZ tgt, v
        adr     x21, 71f
        mov     x22, #0
        mov     x20, #(\v << 28)
        msr     nzcv, x20
        cbz     x22, \tgt
        mov     x0, #0xdead
71:
        .endm

        .macro GO_TBZ tgt, v
        adr     x21, 71f
        mov     x22, #0
        mov     x20, #(\v << 28)
        msr     nzcv, x20
        tbz     x22, #3, \tgt
        mov     x0, #0xdead
71:
        .endm

        test_start

        // ==================================================================
        // 1. Reader kinds at the target, reached by B.
        // ==================================================================
        GO_B    t_mrs, 0x6
        CHECK   mrs-b-zc, 0x6
        GO_B    t_mrs, 0x9
        CHECK   mrs-b-nv, 0x9

        // B.cond at the target, one case per flag so a wrong bit cannot hide:
        // EQ reads Z, MI reads N, CS reads C, VS reads V.
        GO_B    t_beq, 0x6
        CHECK   beq-b-zc, 1
        GO_B    t_beq, 0x9
        CHECK   beq-b-nv, 0
        GO_B    t_bmi, 0x6
        CHECK   bmi-b-zc, 0
        GO_B    t_bmi, 0x9
        CHECK   bmi-b-nv, 1
        GO_B    t_bcs, 0x6
        CHECK   bcs-b-zc, 1
        GO_B    t_bcs, 0x9
        CHECK   bcs-b-nv, 0
        GO_B    t_bvs, 0x6
        CHECK   bvs-b-zc, 0
        GO_B    t_bvs, 0x9
        CHECK   bvs-b-nv, 1

        // The CSEL family, CCMP and ADC.
        GO_B    t_csel, 0x6
        CHECK   csel-b-zc, 11
        GO_B    t_csel, 0x9
        CHECK   csel-b-nv, 22
        GO_B    t_csinc, 0x6
        CHECK   csinc-b-zc, 11
        GO_B    t_csinc, 0x9
        CHECK   csinc-b-nv, 23
        GO_B    t_csinv, 0x6
        CHECK   csinv-b-zc, 11
        GO_B    t_csinv, 0x9
        CHECK   csinv-b-nv, -23
        GO_B    t_csneg, 0x6
        CHECK   csneg-b-zc, 11
        GO_B    t_csneg, 0x9
        CHECK   csneg-b-nv, -22
        GO_B    t_ccmp, 0x6
        CHECK   ccmp-b-zc, 0x8
        GO_B    t_ccmp, 0x9
        CHECK   ccmp-b-nv, 0x0
        GO_B    t_adc, 0x6
        CHECK   adc-b-zc, 1
        GO_B    t_adc, 0x9
        CHECK   adc-b-nv, 0

        // ==================================================================
        // 2. The same reader through every other exit shape.
        // ==================================================================
        GO_BL   t_mrs, 0x6
        CHECK   mrs-bl-zc, 0x6
        GO_BL   t_mrs, 0x9
        CHECK   mrs-bl-nv, 0x9
        // Taken leg of a conditional branch: EQ is true for 0x6, MI for 0x9.
        GO_BCOND eq, t_mrs, 0x6
        CHECK   mrs-bcond-zc, 0x6
        GO_BCOND mi, t_mrs, 0x9
        CHECK   mrs-bcond-nv, 0x9
        GO_CBZ  t_mrs, 0x6
        CHECK   mrs-cbz-zc, 0x6
        GO_CBZ  t_mrs, 0x9
        CHECK   mrs-cbz-nv, 0x9
        GO_TBZ  t_mrs, 0x6
        CHECK   mrs-tbz-zc, 0x6
        GO_TBZ  t_mrs, 0x9
        CHECK   mrs-tbz-nv, 0x9

        // ==================================================================
        // 3. Writers at the target. The scan calls these DEAD, the producer
        //    before the exit goes, and the answer is the writer's own.
        // ==================================================================
        GO_B    t_wr_cmp, 0x6
        CHECK   wr-cmp-zc, 0x6
        GO_B    t_wr_cmp, 0x9
        CHECK   wr-cmp-nv, 0x6
        GO_B    t_wr_subs, 0x6
        CHECK   wr-subs-zc, 0x8
        GO_B    t_wr_subs, 0x9
        CHECK   wr-subs-nv, 0x8
        GO_B    t_wr_ands, 0x6
        CHECK   wr-ands-zc, 0x4
        GO_B    t_wr_ands, 0x9
        CHECK   wr-ands-nv, 0x4
        GO_B    t_wr_fcmp, 0x6
        CHECK   wr-fcmp-zc, 0x6
        GO_B    t_wr_fcmp, 0x9
        CHECK   wr-fcmp-nv, 0x6
        // §7.6's round trip: MSR then MRS at a dead entry.
        GO_B    t_wr_msr, 0x6
        CHECK   wr-msr-zc, 0x3
        GO_B    t_wr_msr, 0x9
        CHECK   wr-msr-nv, 0x3
        // The same, arrived at by a call and by a compare-and-branch leg.
        GO_BL   t_wr_cmp, 0x9
        CHECK   wr-cmp-bl, 0x6
        GO_CBZ  t_wr_cmp, 0x9
        CHECK   wr-cmp-cbz, 0x6

        // A writer, then a branch on it: the direction must be the writer's.
        GO_B    t_wr_then_beq, 0x9
        CHECK   wr-then-beq, 1

        // ==================================================================
        // 4. The reader at depth, and past the scan's budget.
        // ==================================================================
        GO_B    t_d8, 0x6
        CHECK   depth8-zc, 0x6
        GO_B    t_d16, 0x9
        CHECK   depth16-nv, 0x9
        GO_B    t_d17, 0x6
        CHECK   depth17-zc, 0x6
        GO_B    t_d33, 0x9
        CHECK   depth33-nv, 0x9
        GO_B    t_dover, 0x6
        CHECK   depth-over-budget-zc, 0x6
        GO_B    t_dover, 0x9
        CHECK   depth-over-budget-nv, 0x9

        // ==================================================================
        // 5. Behind what the simple scan cannot follow.
        // ==================================================================
        GO_B    t_viabl, 0x6
        CHECK   reader-behind-bl-zc, 0x6
        GO_B    t_viabl, 0x9
        CHECK   reader-behind-bl-nv, 0x9
        GO_B    t_viasvc, 0x6
        CHECK   reader-behind-svc-zc, 0x6
        GO_B    t_viasvc, 0x9
        CHECK   reader-behind-svc-nv, 0x9

        // ==================================================================
        // 6. Past the 64 KiB hull bound (§7.5), reader and writer.
        // ==================================================================
        GO_B    t_far_mrs, 0x6
        CHECK   far-mrs-zc, 0x6
        GO_B    t_far_mrs, 0x9
        CHECK   far-mrs-nv, 0x9
        GO_BL   t_far_wr, 0x9
        CHECK   far-wr-cmp, 0x6

        test_exit

// ======================================================================
// Targets. The gap puts every one of them more than the 128-byte region
// window away from the exits above, so the decoder cannot fold them into the
// exiting unit and each arrival really is a compile-unit entry.
// ======================================================================
        .balign 4
        .space 512

        // Read the incoming NZCV as a 0-15 nibble.
        .macro RD_NZCV
        mrs     x0, nzcv
        lsr     x0, x0, #28
        .endm

t_mrs:  RD_NZCV
        br      x21

t_beq:  mov     x0, #0
        b.eq    1f
        b       2f
1:      mov     x0, #1
2:      br      x21

t_bmi:  mov     x0, #0
        b.mi    1f
        b       2f
1:      mov     x0, #1
2:      br      x21

t_bcs:  mov     x0, #0
        b.cs    1f
        b       2f
1:      mov     x0, #1
2:      br      x21

t_bvs:  mov     x0, #0
        b.vs    1f
        b       2f
1:      mov     x0, #1
2:      br      x21

        // EQ is true for 0x6 and false for 0x9.
t_csel: mov     x3, #11
        mov     x4, #22
        csel    x0, x3, x4, eq
        br      x21

t_csinc: mov    x3, #11
        mov     x4, #22
        csinc   x0, x3, x4, eq
        br      x21

t_csinv: mov    x3, #11
        mov     x4, #22
        csinv   x0, x3, x4, eq
        br      x21

t_csneg: mov    x3, #11
        mov     x4, #22
        csneg   x0, x3, x4, eq
        br      x21

        // CCMP reads the flags to decide, then writes them: if EQ, compare
        // 0 with 1 (N1 Z0 C0 V0 = 0x8); otherwise take the literal 0x0.
t_ccmp: mov     x3, #0
        ccmp    x3, #1, #0x0, eq
        RD_NZCV
        br      x21

        // ADC adds the carry in.
t_adc:  mov     x3, #0
        mov     x4, #0
        adc     x0, x3, x4
        br      x21

        // ---- Writers. Each ends with the flags it computed, read back.
        // cmp x3,x3 on equal operands: N0 Z1 C1 V0 = 0x6.
t_wr_cmp:
        mov     x3, #1
        cmp     x3, x3
        RD_NZCV
        br      x21

        // subs of 0 - 1: the result is -1, so N=1 and Z=0; AArch64 SUBS is
        // an add of the inverted operand plus one, which does not carry out
        // here, so C=0; and -1 is representable, so V=0. 0x8.
t_wr_subs:
        mov     x3, #0
        subs    x4, x3, #1
        RD_NZCV
        br      x21

        // ands x3, #1 with x3 = 0: Z1, and ANDS clears C and V. 0x4.
t_wr_ands:
        mov     x3, #0
        ands    x4, x3, #1
        RD_NZCV
        br      x21

        // fcmp of equal operands: N0 Z1 C1 V0 = 0x6.
t_wr_fcmp:
        fmov    d0, #1.0
        fcmp    d0, d0
        RD_NZCV
        br      x21

        // MSR NZCV then MRS NZCV: the round trip §7.6 keeps exact.
t_wr_msr:
        mov     x3, #(0x3 << 28)
        msr     nzcv, x3
        RD_NZCV
        br      x21

        // A writer, then a reader of it. cmp x3,x3 sets Z, so b.eq is taken.
t_wr_then_beq:
        mov     x3, #1
        cmp     x3, x3
        mov     x0, #0
        b.eq    1f
        b       2f
1:      mov     x0, #1
2:      br      x21

        // ---- The reader at depth. The padding is `orr x5, x5, x5`, which
        // touches no flag and no register this test uses.
        .macro PAD n
        .rept \n
        orr     x5, x5, x5
        .endr
        .endm

t_d8:   PAD     8
        RD_NZCV
        br      x21

t_d16:  PAD     16
        RD_NZCV
        br      x21

t_d17:  PAD     17
        RD_NZCV
        br      x21

t_d33:  PAD     33
        RD_NZCV
        br      x21

        // Past the scan's per-path budget: the verdict must come out
        // UNRESOLVED, which is conservative, so the flags still arrive.
t_dover:
        PAD     200
        RD_NZCV
        br      x21

        // A BL at the target: the simple scan stops there (stage 3 would
        // follow it), so the verdict is UNRESOLVED and the flags arrive.
t_viabl:
        bl      leaf_reader
        br      x21

leaf_reader:
        RD_NZCV
        ret

        // An SVC at the target: the scan walks THROUGH it, because the kernel
        // preserves NZCV across a syscall, and finds the reader after it.
        // 172 is getpid; its result in x0 is immediately overwritten.
t_viasvc:
        mov     x8, #172
        svc     #0
        RD_NZCV
        br      x21

// ======================================================================
// Past 64 KiB. §7.5 refuses a DEAD verdict whose witnesses would push the
// hull of unit and witnesses beyond 64 KiB, because that hull is what the
// unit has to carry as CodePages and as its SMC extent. So these two cases
// must come out conservative -- and correct.
// ======================================================================
        .balign 4
        .space 70000

t_far_mrs:
        RD_NZCV
        br      x21

t_far_wr:
        mov     x3, #1
        cmp     x3, x3
        RD_NZCV
        br      x21
