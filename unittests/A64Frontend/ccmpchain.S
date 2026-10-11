// SPDX-License-Identifier: MIT
//
// Conditional-compare chains: the guest-visible half of
// ISA-OPPORTUNITIES.md §3.2.
//
// `cmp a,b ; ccmp c,d,#K,cond1 ; b.cond2 L` used to lower to a packed-NZCV
// compare, a DATA-DEPENDENT HOST BRANCH over either the second compare or a
// 7-9 instruction write of the constant K into CR0 and XER, and then the
// consumer's projection of XER back out. It now collapses to two compares in
// two condition-register fields, one CR logical op and the guest's own branch
// (ChainCompare + a FromChainCR CondJump), with no packed NZCV anywhere.
//
// That replaces a four-flag computation with a boolean identity:
//
//     taken = cond1(first compare) ? cond2(second compare) : cond2(K)
//
// and cond2(K) is folded at compile time. The identity is only as good as two
// tables agreeing -- the direct-compare decode of each condition against the
// packed-NZCV decode of the same condition (the table at the top of
// CompareBranchFusion.cpp), the polarity of each CR bit, and the evaluation of
// cond2 against a 4-bit literal. There are 14 conditions in each position and
// two polarities per CR bit, so this is a place where one wrong table entry is
// a wrong branch on one condition pair and nothing else: invisible to any test
// that does not enumerate the pairs.
//
// So this file enumerates them, and does it DIFFERENTIALLY AGAINST THE PATH
// THAT ALREADY SHIPPED rather than against a table written here. Each case
// runs the same guest arithmetic twice:
//
//   shape A   cmp ; ccmp ; b.cond2        -- collapsed into the CR-logic form
//   shape B   cmp ; ccmp ; mrs nzcv ; b.cond2
//                                         -- NOT collapsed: the MRS is an NZCV
//                                            reader between the conditional
//                                            compare and the branch, which the
//                                            chain walker refuses, so this is
//                                            the old packed-NZCV lowering that
//                                            the differential tests here (and
//                                            flagsweep's hardware golden in
//                                            particular) already pin
//
// and requires the two to take the same branch. 896 pairs over all 14x14
// conditions, both reachable values of the gating result, the register and
// immediate conditional-compare forms, CCMN as well as CCMP, and W as well as
// X. One accumulator per gating condition, so a FAIL line names it.
//
// A pure self-differential could in principle be satisfied by both paths being
// wrong together, so the ANCHORS section pins eleven outcomes computed by hand
// from the A64 definitions -- the cases that discriminate the interesting
// structure: the gating condition true and false, cond2(K) true and false
// (which is what picks AND vs OR among the CR logical ops), a cond2 that reads
// C, one that reads Z and one that reads both, and the immediate and CCMN
// forms.
//
// The last check is deliberately wrong: run.sh requires exactly one FAIL line
// and requires it to be that one.
//
// NO-GOLDEN
#include "common.S"

        // One check: X0 = value, \expected = the architecturally forced answer.
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
        // One differential case. X19/X20 feed the first compare, X21/X23 the
        // conditional compare. X24 is shape A's answer and X25 shape B's: 0
        // when the branch was taken, 1 when it fell through. X28 accumulates
        // any disagreement.
        //
        // Nothing sits between the conditional compare and the branch in
        // shape A, which is what makes it collapsible; the MRS is what makes
        // shape B not.
        // ------------------------------------------------------------------
        .macro CASE_XR cc1, cc2, k
        mov     x24, #0
        cmp     x19, x20
        ccmp    x21, x23, #\k, \cc1
        b.\cc2  1f
        mov     x24, #1
1:      mov     x25, #0
        cmp     x19, x20
        ccmp    x21, x23, #\k, \cc1
        mrs     x26, nzcv
        b.\cc2  2f
        mov     x25, #1
2:      cmp     x24, x25
        cset    w27, ne
        orr     x28, x28, x27
        .endm

        // W size: the compare boundary is bit 31, so the backend compares with
        // cmpw/cmplw rather than cmpd/cmpld and the operands carry dirty upper
        // halves from the X loads.
        .macro CASE_WR cc1, cc2, k
        mov     x24, #0
        cmp     w19, w20
        ccmp    w21, w23, #\k, \cc1
        b.\cc2  1f
        mov     x24, #1
1:      mov     x25, #0
        cmp     w19, w20
        ccmp    w21, w23, #\k, \cc1
        mrs     x26, nzcv
        b.\cc2  2f
        mov     x25, #1
2:      cmp     x24, x25
        cset    w27, ne
        orr     x28, x28, x27
        .endm

        // The immediate conditional-compare form: a 5-bit unsigned operand,
        // which the backend takes as an inline constant and compares with
        // cmpdi/cmpldi.
        .macro CASE_XI cc1, cc2, k
        mov     x24, #0
        cmp     x19, x20
        ccmp    x21, #9, #\k, \cc1
        b.\cc2  1f
        mov     x24, #1
1:      mov     x25, #0
        cmp     x19, x20
        ccmp    x21, #9, #\k, \cc1
        mrs     x26, nzcv
        b.\cc2  2f
        mov     x25, #1
2:      cmp     x24, x25
        cset    w27, ne
        orr     x28, x28, x27
        .endm

        // CCMN: the add sense. Against a compare this is a comparison with the
        // NEGATED addend, which is an identity for every condition except the
        // unsigned ones at an addend of zero and the signed ones at the sign
        // bit -- the two exclusions the chain walker inherits from compare
        // fusion. Both are reachable here: #0 is one of the two K sweeps'
        // operands and the immediate is 5 bits, so the sign-bit case is not,
        // exactly as for CMN.
        .macro CASE_XN cc1, cc2, k
        mov     x24, #0
        cmp     x19, x20
        ccmn    x21, #9, #\k, \cc1
        b.\cc2  1f
        mov     x24, #1
1:      mov     x25, #0
        cmp     x19, x20
        ccmn    x21, #9, #\k, \cc1
        mrs     x26, nzcv
        b.\cc2  2f
        mov     x25, #1
2:      cmp     x24, x25
        cset    w27, ne
        orr     x28, x28, x27
        .endm

        // Every cond2 for one gating condition and one K, in one form.
        .macro SWEEP form, cc1, k
        .irp CC2, eq, ne, cs, cc, mi, pl, vs, vc, hi, ls, ge, lt, gt, le
        CASE_\form \cc1, \CC2, \k
        .endr
        .endm

        // Both K values and all four forms for one gating condition.
        .macro SWEEP_ALL cc1
        SWEEP XR, \cc1, 0
        SWEEP XR, \cc1, 15
        SWEEP WR, \cc1, 0
        SWEEP WR, \cc1, 15
        SWEEP XI, \cc1, 6
        SWEEP XN, \cc1, 9
        .endm

        test_start

        // Operands chosen so the first compare is a strict unsigned and signed
        // ordering with no overflow (N0 Z0 C1 V0), and the conditional
        // compare's own operands are equal at X size and ordered at W size --
        // so the two sizes do not take the same leg of every condition.
        ldr     x19, =0x0000000500000005
        ldr     x20, =0x0000000300000009
        ldr     x21, =0x0000000700000007
        ldr     x23, =0x0000000700000003
        mov     x28, #0

        SWEEP_ALL eq
        mov     x0, x28
        CHECK   chain_gate_eq, 0

        mov     x28, #0
        SWEEP_ALL ne
        mov     x0, x28
        CHECK   chain_gate_ne, 0

        mov     x28, #0
        SWEEP_ALL cs
        mov     x0, x28
        CHECK   chain_gate_cs, 0

        mov     x28, #0
        SWEEP_ALL cc
        mov     x0, x28
        CHECK   chain_gate_cc, 0

        mov     x28, #0
        SWEEP_ALL mi
        mov     x0, x28
        CHECK   chain_gate_mi, 0

        mov     x28, #0
        SWEEP_ALL pl
        mov     x0, x28
        CHECK   chain_gate_pl, 0

        mov     x28, #0
        SWEEP_ALL vs
        mov     x0, x28
        CHECK   chain_gate_vs, 0

        mov     x28, #0
        SWEEP_ALL vc
        mov     x0, x28
        CHECK   chain_gate_vc, 0

        mov     x28, #0
        SWEEP_ALL hi
        mov     x0, x28
        CHECK   chain_gate_hi, 0

        mov     x28, #0
        SWEEP_ALL ls
        mov     x0, x28
        CHECK   chain_gate_ls, 0

        mov     x28, #0
        SWEEP_ALL ge
        mov     x0, x28
        CHECK   chain_gate_ge, 0

        mov     x28, #0
        SWEEP_ALL lt
        mov     x0, x28
        CHECK   chain_gate_lt, 0

        mov     x28, #0
        SWEEP_ALL gt
        mov     x0, x28
        CHECK   chain_gate_gt, 0

        mov     x28, #0
        SWEEP_ALL le
        mov     x0, x28
        CHECK   chain_gate_le, 0

        // The same sweep with the first compare EQUAL, so every gating
        // condition takes the other leg from the one it took above.
        ldr     x20, =0x0000000500000005
        mov     x28, #0
        SWEEP_ALL cs
        SWEEP_ALL eq
        SWEEP_ALL hi
        SWEEP_ALL lt
        mov     x0, x28
        CHECK   chain_gate_equal_operands, 0

        // And with the first compare a strict unsigned ordering the other way,
        // which makes C clear and N set.
        ldr     x19, =0x0000000300000003
        ldr     x20, =0x0000000500000005
        mov     x28, #0
        SWEEP_ALL cs
        SWEEP_ALL cc
        SWEEP_ALL mi
        SWEEP_ALL ge
        mov     x0, x28
        CHECK   chain_gate_reversed_operands, 0

        // ==================================================================
        // ANCHORS. Computed by hand from the A64 definitions, so the pairs
        // above cannot be satisfied by both paths being wrong together.
        //
        // X19 = 5, X20 = 3: `cmp x19, x20` computes 5 - 3 = 2, so
        // N0 Z0 C1 V0 -- CS/HI/NE/GE/GT hold, CC/EQ/MI/LS/LT/LE do not.
        // X21 = 7, X23 = 7: `ccmp x21, x23` computes 7 - 7 = 0, so on the
        // taken leg the flags are N0 Z1 C1 V0.
        //
        // The `cmp x28, x28 ; mrs x18, nzcv` after each branch is load-bearing
        // and not scaffolding. Without it the next thing in the block is the
        // CHECK's `bl`, every unit exit is seeded with all four flags live, the
        // chain is refused, and the anchor would silently measure the OLD
        // lowering -- which is exactly the trap this section exists to avoid.
        // The pair closes the live range with a writer whose flags are really
        // read, so it cannot be dropped: nothing can observe the conditional
        // compare's flags, and the chain collapses. run.sh counts the
        // collapsed chains to keep that true.
        // ==================================================================
        mov     x19, #5
        mov     x20, #3
        mov     x21, #7
        mov     x23, #7

        // Gate CS holds, so the flags are the second compare's: Z1 -> EQ.
        mov     x0, #0
        cmp     x19, x20
        ccmp    x21, x23, #0, cs
        b.eq    11f
        mov     x0, #1
11:     cmp     x28, x28
        mrs     x18, nzcv
        CHECK   anchor_gate_true_eq_taken, 0

        // Gate CC fails, so the flags are K = 0: Z0 -> EQ does not hold.
        mov     x0, #0
        cmp     x19, x20
        ccmp    x21, x23, #0, cc
        b.eq    12f
        mov     x0, #1
12:     cmp     x28, x28
        mrs     x18, nzcv
        CHECK   anchor_gate_false_k0_eq_not_taken, 1

        // Gate CC fails and K = 4 (N0 Z1 C0 V0): EQ now holds on the false
        // leg, which is the case that makes the predicate an OR rather than an
        // AND.
        mov     x0, #0
        cmp     x19, x20
        ccmp    x21, x23, #4, cc
        b.eq    13f
        mov     x0, #1
13:     cmp     x28, x28
        mrs     x18, nzcv
        CHECK   anchor_gate_false_k4_eq_taken, 0

        // Gate CS holds: flags C1 Z1, so HI (C and not Z) does NOT hold...
        mov     x0, #0
        cmp     x19, x20
        ccmp    x21, x23, #0, cs
        b.hi    14f
        mov     x0, #1
14:     cmp     x28, x28
        mrs     x18, nzcv
        CHECK   anchor_gate_true_hi_not_taken, 1

        // ...and LS does.
        mov     x0, #0
        cmp     x19, x20
        ccmp    x21, x23, #0, cs
        b.ls    15f
        mov     x0, #1
15:     cmp     x28, x28
        mrs     x18, nzcv
        CHECK   anchor_gate_true_ls_taken, 0

        // Gate CS holds, flags C1: a cond2 that reads C alone.
        mov     x0, #0
        cmp     x19, x20
        ccmp    x21, x23, #0, cs
        b.cs    16f
        mov     x0, #1
16:     cmp     x28, x28
        mrs     x18, nzcv
        CHECK   anchor_gate_true_cs_taken, 0

        // Gate CC fails, K = 2 (C set, Z clear): cond2 CS holds on the false
        // leg, cond2 EQ does not. Both legs of one K.
        mov     x0, #0
        cmp     x19, x20
        ccmp    x21, x23, #2, cc
        b.cs    17f
        mov     x0, #1
17:     cmp     x28, x28
        mrs     x18, nzcv
        CHECK   anchor_gate_false_k2_cs_taken, 0

        mov     x0, #0
        cmp     x19, x20
        ccmp    x21, x23, #2, cc
        b.eq    18f
        mov     x0, #1
18:     cmp     x28, x28
        mrs     x18, nzcv
        CHECK   anchor_gate_false_k2_eq_not_taken, 1

        // The immediate form, gate true: 7 - 9 = -2, so N1 Z0 C0 V0 and LT
        // (N != V) holds while GE does not.
        mov     x0, #0
        cmp     x19, x20
        ccmp    x21, #9, #0, cs
        b.lt    19f
        mov     x0, #1
19:     cmp     x28, x28
        mrs     x18, nzcv
        CHECK   anchor_imm_gate_true_lt_taken, 0

        mov     x0, #0
        cmp     x19, x20
        ccmp    x21, #9, #0, cs
        b.ge    20f
        mov     x0, #1
20:     cmp     x28, x28
        mrs     x18, nzcv
        CHECK   anchor_imm_gate_true_ge_not_taken, 1

        // CCMN, gate true: 7 + 9 = 16, so N0 Z0 C0 V0 and NE holds. Against a
        // compare this is 7 vs -9, which is the negated-addend identity the
        // chain relies on.
        mov     x0, #0
        cmp     x19, x20
        ccmn    x21, #9, #0, cs
        b.ne    21f
        mov     x0, #1
21:     cmp     x28, x28
        mrs     x18, nzcv
        CHECK   anchor_ccmn_gate_true_ne_taken, 0

        // ==================================================================
        // The positive control.
        // ==================================================================
        mov     x0, #0
        CHECK   control_deliberately_wrong, 1

        test_exit
