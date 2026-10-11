// SPDX-License-Identifier: MIT
//
// Flag-liveness permissions: the guest-visible half of ISA-OPPORTUNITIES.md
// §3.5.
//
// On this backend CR0.LT/CR0.EQ and XER.CA/XER.OV ARE the guest's N/Z/C/V
// between a flag producer and its consumer (§2), so the POWER shift-algebraic
// instructions -- which write XER.CA -- and the record forms -- which write
// CR0 -- destroy guest flags and were refused outright. They are now allowed
// at a site where DeadFlagCalculationElimination has proved that nothing can
// observe the bit being destroyed (AnnotateFlagLiveness; the permission rides
// on IROp_Ashr::CarryDead, IROp_Sbfe::CarryDead and IROp_CondJump::NZCVDead).
// So each lowering now has TWO forms, and which one runs depends on a dataflow
// judgement that is invisible in the answer when it is right and silent when
// it is wrong -- a wrong C surfaces only in bignum, checked-arithmetic and
// range-check code, which is why it gets a test of its own.
//
// Every case here is one of two shapes:
//
//   GRANTED  the op is followed by a full NZCV writer that cannot be dropped
//            (a CMP whose flags an MRS reads, so no pass can fuse or remove
//            it), so every flag is dead at the op and the permission is
//            given. What is checked is the RESULT: that sradi/srad/srawi/sraw
//            compute the A64 shift exactly -- including the count taken
//            MODULO the width, where PPC saturates instead and A64 wraps --
//            and that the TBZ record form branches on the right bit in the
//            right direction.
//
//   REFUSED  a reader of the flag sits after the op, so the permission must be
//            withheld and the OLD sequence must run. What is checked is the
//            FLAG: C through ADC, and all four through MRS NZCV. These are the
//            cases with teeth. Each one is chosen so that the forbidden
//            instruction would produce a DIFFERENT answer rather than the same
//            one by luck:
//              * asr of 0x...F0 by 4 shifts out only zeros, so sradi would
//                CLEAR CA where the guest's C is 1;
//              * asr of 0x...F1 by 4 shifts out a one, so sradi would SET CA
//                where the guest's C is 0;
//              * sbfx of 0x...F1 stages a nonzero low half under sradi, which
//                would SET CA where the guest's C is 0;
//              * rldicl. of a SET bit leaves CR0 GT (N=0, Z=0) and of a CLEAR
//                bit leaves CR0 EQ (N=0, Z=1), against a forced N=1 Z=0, so
//                either polarity of a wrongly granted TBZ moves at least one
//                bit of what MRS reads back.
//
// Both shapes must give the same answers in every suite mode. Under
// POWERARM_MAXINST=1 each guest instruction is a compile unit of its own, so
// every leg of a GRANTED case becomes a constant exit, every exit is seeded
// with all four flags live, and every permission is refused: the GRANTED cases
// then run the conservative lowering and must still print PASS. That is the
// mode in which this file tests the OTHER half of each branch.
//
// Self-checking, with no reference implementation: every answer below is
// forced by the A64 definitions (shift counts are modulo the width, SBFX is a
// field extract then a sign extend from the field's top bit, a W-size result
// zero-extends into X), and the expected values were computed independently
// with the host compiler rather than read off this emulator. The last check is
// a deliberately wrong one: run.sh requires exactly one FAIL line and requires
// it to be that one, so a run in which nothing printed, or in which the
// comparator stopped reporting failures, cannot pass.
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

        // Close the flag live range: a full NZCV writer whose flags are then
        // really read, so it survives every pass and the op ABOVE it sees all
        // four flags dead. X25 == X26, so the forced NZCV is N0 Z1 C1 V0.
        .macro KILLFLAGS
        cmp     x25, x26
        mrs     x22, nzcv
        .endm

        test_start

        // Operands. X19 = -16 (zeros only in the low nibble), X20 has its ones
        // at bits 0-3 and 32-35, X21 only at bits 31 and 63, X23 = -15 (a one
        // in the low nibble, so an arithmetic shift by 4 shifts it out).
        ldr     x19, =0xFFFFFFFFFFFFFFF0
        ldr     x20, =0x0000000F0000000F
        ldr     x21, =0x8000000080000000
        ldr     x23, =0xFFFFFFFFFFFFFFF1
        mov     x25, #0x1234
        mov     x26, #0x1234

        // ==================================================================
        // GRANTED: ASR by an immediate. X size.
        // ==================================================================
        asr     x18, x19, #4
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_x_imm4, 0xFFFFFFFFFFFFFFFF

        asr     x18, x19, #63
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_x_imm63, 0xFFFFFFFFFFFFFFFF

        asr     x18, x20, #4
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_x_imm4_pos, 0x00000000F0000000

        asr     x18, x21, #1
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_x_imm1_neg, 0xC000000040000000

        // Shift of zero: the one count for which the lowering must NOT reach
        // sradi at all, because sradi writes CA even when it shifts nothing.
        asr     x18, x19, #0
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_x_imm0, 0xFFFFFFFFFFFFFFF0

        // ==================================================================
        // GRANTED: ASR by an immediate. W size -- the result zero-extends.
        // ==================================================================
        asr     w18, w19, #4
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_w_imm4, 0x00000000FFFFFFFF

        asr     w18, w21, #4
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_w_imm4_neg, 0x00000000F8000000

        asr     w18, w19, #31
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_w_imm31, 0x00000000FFFFFFFF

        asr     w18, w19, #0
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_w_imm0, 0x00000000FFFFFFF0

        // ==================================================================
        // GRANTED: ASR by a register. The count is taken MODULO the width,
        // which `srad`/`sraw` do NOT do on their own -- they treat a count at
        // or above the width as "shift everything out" and return the sign.
        // These are the cases that fail if the pre-mask is dropped.
        // ==================================================================
        mov     x24, #4
        asr     x18, x19, x24
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_x_reg4, 0xFFFFFFFFFFFFFFFF

        mov     x24, #64
        asr     x18, x19, x24
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_x_reg64_is_0, 0xFFFFFFFFFFFFFFF0

        mov     x24, #65
        asr     x18, x19, x24
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_x_reg65_is_1, 0xFFFFFFFFFFFFFFF8

        mov     x24, #63
        asr     x18, x21, x24
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_x_reg63, 0xFFFFFFFFFFFFFFFF

        // A count with dirty high bits: only Rm[5:0] is architectural.
        ldr     x24, =0xDEADBEEF00000044
        asr     x18, x19, x24
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_x_reg_dirty, 0xFFFFFFFFFFFFFFFF

        mov     x24, #32
        asr     w18, w19, w24
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_w_reg32_is_0, 0x00000000FFFFFFF0

        mov     x24, #33
        asr     w18, w19, w24
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_w_reg33_is_1, 0x00000000FFFFFFF8

        mov     x24, #4
        asr     w18, w21, w24
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_w_reg4_neg, 0x00000000F8000000

        ldr     x24, =0xDEADBEEF00000024
        asr     w18, w19, w24
        KILLFLAGS
        mov     x0, x18
        CHECK   asr_w_reg_dirty, 0x00000000FFFFFFFF

        // ==================================================================
        // GRANTED: SBFX. The sldi+sradi form has to place the field's top bit
        // at bit 63 and shift back by 64-width, for every lsb and width.
        // ==================================================================
        sbfx    x18, x19, #4, #8
        KILLFLAGS
        mov     x0, x18
        CHECK   sbfx_x_4_8_neg, 0xFFFFFFFFFFFFFFFF

        sbfx    x18, x20, #4, #8
        KILLFLAGS
        mov     x0, x18
        CHECK   sbfx_x_4_8_zero, 0

        sbfx    x18, x21, #60, #4
        KILLFLAGS
        mov     x0, x18
        CHECK   sbfx_x_60_4, 0xFFFFFFFFFFFFFFF8

        sbfx    x18, x20, #0, #33
        KILLFLAGS
        mov     x0, x18
        CHECK   sbfx_x_0_33, 0xFFFFFFFF0000000F

        sbfx    x18, x19, #1, #62
        KILLFLAGS
        mov     x0, x18
        CHECK   sbfx_x_1_62, 0xFFFFFFFFFFFFFFF8

        sbfx    x18, x21, #31, #2
        KILLFLAGS
        mov     x0, x18
        CHECK   sbfx_x_31_2, 1

        sbfx    w18, w19, #4, #8
        KILLFLAGS
        mov     x0, x18
        CHECK   sbfx_w_4_8, 0x00000000FFFFFFFF

        sbfx    w18, w21, #1, #7
        KILLFLAGS
        mov     x0, x18
        CHECK   sbfx_w_1_7, 0

        sbfx    w18, w19, #12, #17
        KILLFLAGS
        mov     x0, x18
        CHECK   sbfx_w_12_17, 0x00000000FFFFFFFF

        // The KILLFLAGS compare's own flags, which every case above depends on
        // reaching its MRS: X25 == X26 forces N0 Z1 C1 V0.
        mov     x0, x22
        CHECK   killflags_nzcv, 0x60000000

        // ==================================================================
        // GRANTED: the TBZ/TBNZ record form. Both legs reach the KILLFLAGS
        // compare before anything reads a flag, so all four are dead at the
        // branch and `rldicl.` may set CR0. What is checked is the DIRECTION.
        //
        // X18 = 1 on the fall-through, 2 on the branch.
        // ==================================================================
        // X19 bit 4 is SET: tbz does not branch, tbnz does.
        tbz     x19, #4, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbz_bit_set, 1

        tbnz    x19, #4, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbnz_bit_set, 2

        // X19 bit 0 is CLEAR: tbz branches, tbnz does not.
        tbz     x19, #0, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbz_bit_clear, 2

        tbnz    x19, #0, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbnz_bit_clear, 1

        // Bit 3 clear, bit 15 set -- either side of the 16-bit boundary where
        // an `andi.`-shaped extract would change form.
        tbz     x19, #3, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbz_bit3_clear, 2

        tbz     x19, #15, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbz_bit15_set, 1

        tbz     x19, #16, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbz_bit16_set, 1

        // The word boundary and the top bit, on a value that is zero there.
        tbz     x20, #31, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbz_bit31_clear, 2

        tbz     x20, #32, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbz_bit32_set, 1

        tbz     x20, #35, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbz_bit35_set, 1

        tbz     x20, #63, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbz_bit63_clear, 2

        // And the top bit set, where the extract's rotate amount is 1.
        tbnz    x21, #63, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbnz_bit63_set, 2

        tbz     x21, #31, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      KILLFLAGS
        mov     x0, x18
        CHECK   tbz_bit31_set, 1

        // ==================================================================
        // REFUSED: C is read after the shift, so the shift must not write it.
        //
        // 0xFFFFFFFFFFFFFFFF + 1 carries out: C = 1. An `asr` of X19 by 4
        // shifts out four zeros, so a wrongly granted sradi would CLEAR CA and
        // this would read 0.
        // ==================================================================
        mov     x27, #-1
        mov     x28, #1
        adds    x29, x27, x28
        asr     x18, x19, #4
        adc     x0, xzr, xzr
        CHECK   asr_x_imm_keeps_c1, 1

        // 1 + 1 does not carry: C = 0. An `asr` of X23 (= -15) by 4 shifts out
        // a one, so a wrongly granted sradi would SET CA and this would read 1.
        adds    x29, x28, x28
        asr     x18, x23, #4
        adc     x0, xzr, xzr
        CHECK   asr_x_imm_keeps_c0, 0

        // The same two through the register form (srad), the W forms (sraw)
        // and a second consumer shape (B.CS rather than ADC).
        mov     x24, #4
        adds    x29, x27, x28
        asr     x18, x19, x24
        adc     x0, xzr, xzr
        CHECK   asr_x_reg_keeps_c1, 1

        adds    x29, x28, x28
        asr     x18, x23, x24
        adc     x0, xzr, xzr
        CHECK   asr_x_reg_keeps_c0, 0

        adds    x29, x27, x28
        asr     w18, w19, #4
        adc     x0, xzr, xzr
        CHECK   asr_w_imm_keeps_c1, 1

        adds    x29, x28, x28
        asr     w18, w23, #4
        adc     x0, xzr, xzr
        CHECK   asr_w_imm_keeps_c0, 0

        adds    x29, x27, x28
        asr     w18, w19, w24
        adc     x0, xzr, xzr
        CHECK   asr_w_reg_keeps_c1, 1

        adds    x29, x27, x28
        asr     x18, x19, #4
        mov     x0, #0
        b.cc    3f
        mov     x0, #1
3:      CHECK   asr_keeps_c_for_bcs, 1

        // SBFX, same question. The staged value under sldi has a nonzero low
        // half here, so a wrongly granted sradi would SET CA against a real
        // C of 0.
        adds    x29, x28, x28
        sbfx    x18, x23, #4, #8
        adc     x0, xzr, xzr
        CHECK   sbfx_x_keeps_c0, 0

        adds    x29, x27, x28
        sbfx    x18, x19, #4, #8
        adc     x0, xzr, xzr
        CHECK   sbfx_x_keeps_c1, 1

        adds    x29, x28, x28
        sbfx    w18, w23, #4, #8
        adc     x0, xzr, xzr
        CHECK   sbfx_w_keeps_c0, 0

        // And the extracted value is still right when the permission is
        // refused: the two lowerings must agree.
        mov     x0, x18
        CHECK   sbfx_w_refused_value, 0x00000000FFFFFFFF

        // ==================================================================
        // REFUSED: all four flags are read after the TBZ, so it must not
        // write CR0. CMP X19, X20 forces N1 Z0 C1 V0 = 0xA0000000; a wrongly
        // granted `rldicl.` would leave CR0 from the extracted bit instead --
        // GT for a set bit (N0 Z0) and EQ for a clear one (N0 Z1) -- so either
        // polarity moves at least one bit of what MRS reads back.
        // ==================================================================
        cmp     x19, x20
        tbz     x19, #4, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      mrs     x0, nzcv
        CHECK   tbz_set_keeps_nzcv, 0xA0000000

        cmp     x19, x20
        tbz     x19, #0, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      mrs     x0, nzcv
        CHECK   tbz_clear_keeps_nzcv, 0xA0000000

        // The direction is still right on the refused path.
        mov     x0, x18
        CHECK   tbz_refused_direction, 2

        // A conditional consumer rather than MRS, so the refusal is pinned by
        // a branch as well as by a read-back.
        cmp     x19, x20
        tbz     x19, #4, 1f
        mov     x18, #1
        b       2f
1:      mov     x18, #2
2:      mov     x0, #0
        b.pl    3f
        mov     x0, #1
3:      CHECK   tbz_keeps_n_for_bmi, 1

        // ==================================================================
        // The positive control. run.sh requires exactly one FAIL line and
        // requires it to be this one: a run that printed nothing, or a
        // comparator that stopped reporting mismatches, must not pass.
        // ==================================================================
        mov     x0, #0
        CHECK   control_deliberately_wrong, 1

        test_exit
