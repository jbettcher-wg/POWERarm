// SPDX-License-Identifier: MIT
//
// LRCPC2 unscaled loads and stores: STLURB/STLURH/STLUR and LDAPURB/LDAPURH/
// LDAPUR with the sign-extending LDAPURS* forms, over negative, zero and
// positive offsets and a static and a context-backed base register. The
// Pi 5's Cortex-A76 has LRCPC but not LRCPC2, so the instructions are given
// as .inst words and this test states its golden: each case checks the
// load against the pattern stored with STUR, or reads a store back with LDUR.
// Generated once; the expected values are computed from the pattern.
// EXPECT: PASS stlurb_m128_x25
// EXPECT: PASS stlurb_m1_x3
// EXPECT: PASS stlurb_0_x3
// EXPECT: PASS stlurb_1_x25
// EXPECT: PASS stlurb_7_x3
// EXPECT: PASS stlurb_119_x25
// EXPECT: PASS stlurb_m34_x3
// EXPECT: PASS stlurh_m128_x25
// EXPECT: PASS stlurh_m1_x25
// EXPECT: PASS stlurh_0_x3
// EXPECT: PASS stlurh_1_x25
// EXPECT: PASS stlurh_7_x3
// EXPECT: PASS stlurh_119_x3
// EXPECT: PASS stlurh_m2_x3
// EXPECT: PASS stlur_w_m128_x3
// EXPECT: PASS stlur_w_m1_x3
// EXPECT: PASS stlur_w_0_x25
// EXPECT: PASS stlur_w_1_x3
// EXPECT: PASS stlur_w_7_x25
// EXPECT: PASS stlur_w_119_x25
// EXPECT: PASS stlur_w_87_x3
// EXPECT: PASS stlur_x_m128_x3
// EXPECT: PASS stlur_x_m1_x25
// EXPECT: PASS stlur_x_0_x25
// EXPECT: PASS stlur_x_1_x3
// EXPECT: PASS stlur_x_7_x25
// EXPECT: PASS stlur_x_119_x25
// EXPECT: PASS stlur_x_m34_x3
// EXPECT: PASS ldapurb_m128_x25
// EXPECT: PASS ldapurb_m1_x25
// EXPECT: PASS ldapurb_0_x25
// EXPECT: PASS ldapurb_1_x25
// EXPECT: PASS ldapurb_7_x3
// EXPECT: PASS ldapurb_119_x3
// EXPECT: PASS ldapurb_m55_x25
// EXPECT: PASS ldapurh_m128_x25
// EXPECT: PASS ldapurh_m1_x3
// EXPECT: PASS ldapurh_0_x3
// EXPECT: PASS ldapurh_1_x25
// EXPECT: PASS ldapurh_7_x25
// EXPECT: PASS ldapurh_119_x25
// EXPECT: PASS ldapurh_m109_x3
// EXPECT: PASS ldapur_w_m128_x3
// EXPECT: PASS ldapur_w_m1_x3
// EXPECT: PASS ldapur_w_0_x25
// EXPECT: PASS ldapur_w_1_x3
// EXPECT: PASS ldapur_w_7_x3
// EXPECT: PASS ldapur_w_119_x3
// EXPECT: PASS ldapur_w_m29_x3
// EXPECT: PASS ldapur_x_m128_x3
// EXPECT: PASS ldapur_x_m1_x3
// EXPECT: PASS ldapur_x_0_x25
// EXPECT: PASS ldapur_x_1_x3
// EXPECT: PASS ldapur_x_7_x3
// EXPECT: PASS ldapur_x_119_x25
// EXPECT: PASS ldapur_x_4_x3
// EXPECT: PASS ldapursb_x_m128_x25
// EXPECT: PASS ldapursb_x_m1_x25
// EXPECT: PASS ldapursb_x_0_x3
// EXPECT: PASS ldapursb_x_1_x3
// EXPECT: PASS ldapursb_x_7_x3
// EXPECT: PASS ldapursb_x_119_x25
// EXPECT: PASS ldapursb_x_48_x25
// EXPECT: PASS ldapursb_w_m128_x25
// EXPECT: PASS ldapursb_w_m1_x3
// EXPECT: PASS ldapursb_w_0_x25
// EXPECT: PASS ldapursb_w_1_x3
// EXPECT: PASS ldapursb_w_7_x25
// EXPECT: PASS ldapursb_w_119_x25
// EXPECT: PASS ldapursb_w_2_x25
// EXPECT: PASS ldapursh_x_m128_x3
// EXPECT: PASS ldapursh_x_m1_x25
// EXPECT: PASS ldapursh_x_0_x3
// EXPECT: PASS ldapursh_x_1_x3
// EXPECT: PASS ldapursh_x_7_x25
// EXPECT: PASS ldapursh_x_119_x3
// EXPECT: PASS ldapursh_x_m34_x25
// EXPECT: PASS ldapursh_w_m128_x3
// EXPECT: PASS ldapursh_w_m1_x25
// EXPECT: PASS ldapursh_w_0_x3
// EXPECT: PASS ldapursh_w_1_x3
// EXPECT: PASS ldapursh_w_7_x3
// EXPECT: PASS ldapursh_w_119_x25
// EXPECT: PASS ldapursh_w_m120_x25
// EXPECT: PASS ldapursw_m128_x25
// EXPECT: PASS ldapursw_m1_x25
// EXPECT: PASS ldapursw_0_x3
// EXPECT: PASS ldapursw_1_x25
// EXPECT: PASS ldapursw_7_x25
// EXPECT: PASS ldapursw_119_x25
// EXPECT: PASS ldapursw_102_x3
// EXPECT-RC: 0
#include "common.S"

        test_start
        adrp    x3, buf
        add     x3, x3, :lo12:buf
        add     x3, x3, #128
        // stlurb x0, [x25, #-128]
        ldr     x5, =0xe3da41fa61b911c9
        stur    x5, [x3, #-128]
        mov     x25, x3
        ldr     x0, =0x6c6c7f4d2ab4385e
        .inst   0x19180320
        ldur    x0, [x3, #-128]
        ldr     x1, =0xe3da41fa61b9115e
        adr     x2, name_1
        bl      check
        b       1f
name_1: .asciz "stlurb_m128_x25"
        .balign 4
1:
        // stlurb x0, [x3, #-1]
        ldr     x5, =0xe738e863acc80d97
        stur    x5, [x3, #-1]
        ldr     x0, =0xb549945945e1c4e6
        .inst   0x191ff060
        ldur    x0, [x3, #-1]
        ldr     x1, =0xe738e863acc80de6
        adr     x2, name_2
        bl      check
        b       1f
name_2: .asciz "stlurb_m1_x3"
        .balign 4
1:
        // stlurb x0, [x3, #0]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #0]
        ldr     x0, =0x2547e11e8c5542ea
        .inst   0x19000060
        ldur    x0, [x3, #0]
        ldr     x1, =0x71727374757677ea
        adr     x2, name_3
        bl      check
        b       1f
name_3: .asciz "stlurb_0_x3"
        .balign 4
1:
        // stlurb x0, [x25, #1]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #1]
        mov     x25, x3
        ldr     x0, =0x906105f9da2f76a5
        .inst   0x19001320
        ldur    x0, [x3, #1]
        ldr     x1, =0x71727374757677a5
        adr     x2, name_4
        bl      check
        b       1f
name_4: .asciz "stlurb_1_x25"
        .balign 4
1:
        // stlurb x0, [x3, #7]
        ldr     x5, =0xbffebf80e206017b
        stur    x5, [x3, #7]
        ldr     x0, =0x860378eb78b7f59f
        .inst   0x19007060
        ldur    x0, [x3, #7]
        ldr     x1, =0xbffebf80e206019f
        adr     x2, name_5
        bl      check
        b       1f
name_5: .asciz "stlurb_7_x3"
        .balign 4
1:
        // stlurb x0, [x25, #119]
        ldr     x5, =0xa2ffbda12007acf3
        stur    x5, [x3, #119]
        mov     x25, x3
        ldr     x0, =0xe870cf9305c448df
        .inst   0x19077320
        ldur    x0, [x3, #119]
        ldr     x1, =0xa2ffbda12007acdf
        adr     x2, name_6
        bl      check
        b       1f
name_6: .asciz "stlurb_119_x25"
        .balign 4
1:
        // stlurb x0, [x3, #-34]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-34]
        ldr     x0, =0x2d4d09bbf73d0be5
        .inst   0x191de060
        ldur    x0, [x3, #-34]
        ldr     x1, =0x71727374757677e5
        adr     x2, name_7
        bl      check
        b       1f
name_7: .asciz "stlurb_m34_x3"
        .balign 4
1:
        // stlurh x0, [x25, #-128]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #-128]
        mov     x25, x3
        ldr     x0, =0x45914432f578e577
        .inst   0x59180320
        ldur    x0, [x3, #-128]
        ldr     x1, =0x818283848586e577
        adr     x2, name_8
        bl      check
        b       1f
name_8: .asciz "stlurh_m128_x25"
        .balign 4
1:
        // stlurh x0, [x25, #-1]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-1]
        mov     x25, x3
        ldr     x0, =0x35907a3482494a09
        .inst   0x591ff320
        ldur    x0, [x3, #-1]
        ldr     x1, =0x7172737475764a09
        adr     x2, name_9
        bl      check
        b       1f
name_9: .asciz "stlurh_m1_x25"
        .balign 4
1:
        // stlurh x0, [x3, #0]
        ldr     x5, =0xfe6b29c3e011d678
        stur    x5, [x3, #0]
        ldr     x0, =0x9c437ff3bb82c6f9
        .inst   0x59000060
        ldur    x0, [x3, #0]
        ldr     x1, =0xfe6b29c3e011c6f9
        adr     x2, name_10
        bl      check
        b       1f
name_10: .asciz "stlurh_0_x3"
        .balign 4
1:
        // stlurh x0, [x25, #1]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #1]
        mov     x25, x3
        ldr     x0, =0x6be3048c55c6489c
        .inst   0x59001320
        ldur    x0, [x3, #1]
        ldr     x1, =0x818283848586489c
        adr     x2, name_11
        bl      check
        b       1f
name_11: .asciz "stlurh_1_x25"
        .balign 4
1:
        // stlurh x0, [x3, #7]
        ldr     x5, =0xd7d1a0294faeb6fa
        stur    x5, [x3, #7]
        ldr     x0, =0x688082b368627003
        .inst   0x59007060
        ldur    x0, [x3, #7]
        ldr     x1, =0xd7d1a0294fae7003
        adr     x2, name_12
        bl      check
        b       1f
name_12: .asciz "stlurh_7_x3"
        .balign 4
1:
        // stlurh x0, [x3, #119]
        ldr     x5, =0xa4ab50e643dfd9ea
        stur    x5, [x3, #119]
        ldr     x0, =0x95061c51530f69fd
        .inst   0x59077060
        ldur    x0, [x3, #119]
        ldr     x1, =0xa4ab50e643df69fd
        adr     x2, name_13
        bl      check
        b       1f
name_13: .asciz "stlurh_119_x3"
        .balign 4
1:
        // stlurh x0, [x3, #-2]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-2]
        ldr     x0, =0x3a0014dfefecf33b
        .inst   0x591fe060
        ldur    x0, [x3, #-2]
        ldr     x1, =0x717273747576f33b
        adr     x2, name_14
        bl      check
        b       1f
name_14: .asciz "stlurh_m2_x3"
        .balign 4
1:
        // stlur_w x0, [x3, #-128]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-128]
        ldr     x0, =0xc1456200a499fabe
        .inst   0x99180060
        ldur    x0, [x3, #-128]
        ldr     x1, =0x71727374a499fabe
        adr     x2, name_15
        bl      check
        b       1f
name_15: .asciz "stlur_w_m128_x3"
        .balign 4
1:
        // stlur_w x0, [x3, #-1]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-1]
        ldr     x0, =0xdd7e77125a1ac4ec
        .inst   0x991ff060
        ldur    x0, [x3, #-1]
        ldr     x1, =0x717273745a1ac4ec
        adr     x2, name_16
        bl      check
        b       1f
name_16: .asciz "stlur_w_m1_x3"
        .balign 4
1:
        b       2f
        .ltorg
2:
        // stlur_w x0, [x25, #0]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #0]
        mov     x25, x3
        ldr     x0, =0x499ab310af745187
        .inst   0x99000320
        ldur    x0, [x3, #0]
        ldr     x1, =0x81828384af745187
        adr     x2, name_17
        bl      check
        b       1f
name_17: .asciz "stlur_w_0_x25"
        .balign 4
1:
        // stlur_w x0, [x3, #1]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #1]
        ldr     x0, =0x8871dbf952fd211b
        .inst   0x99001060
        ldur    x0, [x3, #1]
        ldr     x1, =0x8182838452fd211b
        adr     x2, name_18
        bl      check
        b       1f
name_18: .asciz "stlur_w_1_x3"
        .balign 4
1:
        // stlur_w x0, [x25, #7]
        ldr     x5, =0xbf38faa7ad2189cd
        stur    x5, [x3, #7]
        mov     x25, x3
        ldr     x0, =0x148e4c70f06e63c3
        .inst   0x99007320
        ldur    x0, [x3, #7]
        ldr     x1, =0xbf38faa7f06e63c3
        adr     x2, name_19
        bl      check
        b       1f
name_19: .asciz "stlur_w_7_x25"
        .balign 4
1:
        // stlur_w x0, [x25, #119]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #119]
        mov     x25, x3
        ldr     x0, =0x1000fb7aeade7d64
        .inst   0x99077320
        ldur    x0, [x3, #119]
        ldr     x1, =0x81828384eade7d64
        adr     x2, name_20
        bl      check
        b       1f
name_20: .asciz "stlur_w_119_x25"
        .balign 4
1:
        // stlur_w x0, [x3, #87]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #87]
        ldr     x0, =0xf83f1d9d3d5f4509
        .inst   0x99057060
        ldur    x0, [x3, #87]
        ldr     x1, =0x717273743d5f4509
        adr     x2, name_21
        bl      check
        b       1f
name_21: .asciz "stlur_w_87_x3"
        .balign 4
1:
        // stlur_x x0, [x3, #-128]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-128]
        ldr     x0, =0x621c4794575b2bbe
        .inst   0xd9180060
        ldur    x0, [x3, #-128]
        ldr     x1, =0x621c4794575b2bbe
        adr     x2, name_22
        bl      check
        b       1f
name_22: .asciz "stlur_x_m128_x3"
        .balign 4
1:
        // stlur_x x0, [x25, #-1]
        ldr     x5, =0xc077c6fc55562128
        stur    x5, [x3, #-1]
        mov     x25, x3
        ldr     x0, =0xe7ef24c4bd8c3632
        .inst   0xd91ff320
        ldur    x0, [x3, #-1]
        ldr     x1, =0xe7ef24c4bd8c3632
        adr     x2, name_23
        bl      check
        b       1f
name_23: .asciz "stlur_x_m1_x25"
        .balign 4
1:
        // stlur_x x0, [x25, #0]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #0]
        mov     x25, x3
        ldr     x0, =0x67445418da5b41d4
        .inst   0xd9000320
        ldur    x0, [x3, #0]
        ldr     x1, =0x67445418da5b41d4
        adr     x2, name_24
        bl      check
        b       1f
name_24: .asciz "stlur_x_0_x25"
        .balign 4
1:
        // stlur_x x0, [x3, #1]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #1]
        ldr     x0, =0xb6d4501ef3d812cc
        .inst   0xd9001060
        ldur    x0, [x3, #1]
        ldr     x1, =0xb6d4501ef3d812cc
        adr     x2, name_25
        bl      check
        b       1f
name_25: .asciz "stlur_x_1_x3"
        .balign 4
1:
        // stlur_x x0, [x25, #7]
        ldr     x5, =0xd6c988865fe9d4c2
        stur    x5, [x3, #7]
        mov     x25, x3
        ldr     x0, =0xc64d6e5016cc3df7
        .inst   0xd9007320
        ldur    x0, [x3, #7]
        ldr     x1, =0xc64d6e5016cc3df7
        adr     x2, name_26
        bl      check
        b       1f
name_26: .asciz "stlur_x_7_x25"
        .balign 4
1:
        // stlur_x x0, [x25, #119]
        ldr     x5, =0xdf9755387c5053de
        stur    x5, [x3, #119]
        mov     x25, x3
        ldr     x0, =0xf8e8afd5b095534c
        .inst   0xd9077320
        ldur    x0, [x3, #119]
        ldr     x1, =0xf8e8afd5b095534c
        adr     x2, name_27
        bl      check
        b       1f
name_27: .asciz "stlur_x_119_x25"
        .balign 4
1:
        // stlur_x x0, [x3, #-34]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-34]
        ldr     x0, =0xa028135c31996c2f
        .inst   0xd91de060
        ldur    x0, [x3, #-34]
        ldr     x1, =0xa028135c31996c2f
        adr     x2, name_28
        bl      check
        b       1f
name_28: .asciz "stlur_x_m34_x3"
        .balign 4
1:
        // ldapurb x0, [x25, #-128]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #-128]
        mov     x25, x3
        ldr     x0, =0x2874561b26d96e2a
        .inst   0x19580320
        ldr     x1, =0x88
        adr     x2, name_29
        bl      check
        b       1f
name_29: .asciz "ldapurb_m128_x25"
        .balign 4
1:
        // ldapurb x0, [x25, #-1]
        ldr     x5, =0xfe620e9b6b2e6223
        stur    x5, [x3, #-1]
        mov     x25, x3
        ldr     x0, =0x3a60976249c02a72
        .inst   0x195ff320
        ldr     x1, =0x23
        adr     x2, name_30
        bl      check
        b       1f
name_30: .asciz "ldapurb_m1_x25"
        .balign 4
1:
        // ldapurb x0, [x25, #0]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #0]
        mov     x25, x3
        ldr     x0, =0x59bd87262e7ae8d5
        .inst   0x19400320
        ldr     x1, =0x88
        adr     x2, name_31
        bl      check
        b       1f
name_31: .asciz "ldapurb_0_x25"
        .balign 4
1:
        // ldapurb x0, [x25, #1]
        ldr     x5, =0xd7a80a5ab7af5be8
        stur    x5, [x3, #1]
        mov     x25, x3
        ldr     x0, =0xb7b69973006aceac
        .inst   0x19401320
        ldr     x1, =0xe8
        adr     x2, name_32
        bl      check
        b       1f
name_32: .asciz "ldapurb_1_x25"
        .balign 4
1:
        b       2f
        .ltorg
2:
        // ldapurb x0, [x3, #7]
        ldr     x5, =0xa8cd0f6fd12c189f
        stur    x5, [x3, #7]
        ldr     x0, =0x27581f52897c0932
        .inst   0x19407060
        ldr     x1, =0x9f
        adr     x2, name_33
        bl      check
        b       1f
name_33: .asciz "ldapurb_7_x3"
        .balign 4
1:
        // ldapurb x0, [x3, #119]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #119]
        ldr     x0, =0x58828b50b1bd5657
        .inst   0x19477060
        ldr     x1, =0x78
        adr     x2, name_34
        bl      check
        b       1f
name_34: .asciz "ldapurb_119_x3"
        .balign 4
1:
        // ldapurb x0, [x25, #-55]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #-55]
        mov     x25, x3
        ldr     x0, =0x7210b20aab9aac22
        .inst   0x195c9320
        ldr     x1, =0x88
        adr     x2, name_35
        bl      check
        b       1f
name_35: .asciz "ldapurb_m55_x25"
        .balign 4
1:
        // ldapurh x0, [x25, #-128]
        ldr     x5, =0xe21f1bccea156c87
        stur    x5, [x3, #-128]
        mov     x25, x3
        ldr     x0, =0x6b9a36774575349b
        .inst   0x59580320
        ldr     x1, =0x6c87
        adr     x2, name_36
        bl      check
        b       1f
name_36: .asciz "ldapurh_m128_x25"
        .balign 4
1:
        // ldapurh x0, [x3, #-1]
        ldr     x5, =0xc526656442c8ebfe
        stur    x5, [x3, #-1]
        ldr     x0, =0xfb909893d6d1949a
        .inst   0x595ff060
        ldr     x1, =0xebfe
        adr     x2, name_37
        bl      check
        b       1f
name_37: .asciz "ldapurh_m1_x3"
        .balign 4
1:
        // ldapurh x0, [x3, #0]
        ldr     x5, =0xfcfbd9ceb6bd1289
        stur    x5, [x3, #0]
        ldr     x0, =0xc83641f1abdcb9d1
        .inst   0x59400060
        ldr     x1, =0x1289
        adr     x2, name_38
        bl      check
        b       1f
name_38: .asciz "ldapurh_0_x3"
        .balign 4
1:
        // ldapurh x0, [x25, #1]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #1]
        mov     x25, x3
        ldr     x0, =0x8920f5bde0c236e
        .inst   0x59401320
        ldr     x1, =0x7778
        adr     x2, name_39
        bl      check
        b       1f
name_39: .asciz "ldapurh_1_x25"
        .balign 4
1:
        // ldapurh x0, [x25, #7]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #7]
        mov     x25, x3
        ldr     x0, =0x73f737df810b0e5f
        .inst   0x59407320
        ldr     x1, =0x8788
        adr     x2, name_40
        bl      check
        b       1f
name_40: .asciz "ldapurh_7_x25"
        .balign 4
1:
        // ldapurh x0, [x25, #119]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #119]
        mov     x25, x3
        ldr     x0, =0x8ec2cf9f4913b6c2
        .inst   0x59477320
        ldr     x1, =0x7778
        adr     x2, name_41
        bl      check
        b       1f
name_41: .asciz "ldapurh_119_x25"
        .balign 4
1:
        // ldapurh x0, [x3, #-109]
        ldr     x5, =0xee557da15015e118
        stur    x5, [x3, #-109]
        ldr     x0, =0x404212be8802d491
        .inst   0x59593060
        ldr     x1, =0xe118
        adr     x2, name_42
        bl      check
        b       1f
name_42: .asciz "ldapurh_m109_x3"
        .balign 4
1:
        // ldapur_w x0, [x3, #-128]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-128]
        ldr     x0, =0x45fe92b8b0ba802a
        .inst   0x99580060
        ldr     x1, =0x75767778
        adr     x2, name_43
        bl      check
        b       1f
name_43: .asciz "ldapur_w_m128_x3"
        .balign 4
1:
        // ldapur_w x0, [x3, #-1]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-1]
        ldr     x0, =0x8eb5c56788b788d8
        .inst   0x995ff060
        ldr     x1, =0x75767778
        adr     x2, name_44
        bl      check
        b       1f
name_44: .asciz "ldapur_w_m1_x3"
        .balign 4
1:
        // ldapur_w x0, [x25, #0]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #0]
        mov     x25, x3
        ldr     x0, =0x9bb167850ad7a32e
        .inst   0x99400320
        ldr     x1, =0x75767778
        adr     x2, name_45
        bl      check
        b       1f
name_45: .asciz "ldapur_w_0_x25"
        .balign 4
1:
        // ldapur_w x0, [x3, #1]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #1]
        ldr     x0, =0x36184ba57d75223
        .inst   0x99401060
        ldr     x1, =0x85868788
        adr     x2, name_46
        bl      check
        b       1f
name_46: .asciz "ldapur_w_1_x3"
        .balign 4
1:
        // ldapur_w x0, [x3, #7]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #7]
        ldr     x0, =0x4b63f3944124ea41
        .inst   0x99407060
        ldr     x1, =0x85868788
        adr     x2, name_47
        bl      check
        b       1f
name_47: .asciz "ldapur_w_7_x3"
        .balign 4
1:
        // ldapur_w x0, [x3, #119]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #119]
        ldr     x0, =0xd6be65be48789553
        .inst   0x99477060
        ldr     x1, =0x75767778
        adr     x2, name_48
        bl      check
        b       1f
name_48: .asciz "ldapur_w_119_x3"
        .balign 4
1:
        b       2f
        .ltorg
2:
        // ldapur_w x0, [x3, #-29]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-29]
        ldr     x0, =0xb0f34ff86e45ba9d
        .inst   0x995e3060
        ldr     x1, =0x75767778
        adr     x2, name_49
        bl      check
        b       1f
name_49: .asciz "ldapur_w_m29_x3"
        .balign 4
1:
        // ldapur_x x0, [x3, #-128]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-128]
        ldr     x0, =0x9e351f2049c50e23
        .inst   0xd9580060
        ldr     x1, =0x7172737475767778
        adr     x2, name_50
        bl      check
        b       1f
name_50: .asciz "ldapur_x_m128_x3"
        .balign 4
1:
        // ldapur_x x0, [x3, #-1]
        ldr     x5, =0xe529c8c8d0cc27d2
        stur    x5, [x3, #-1]
        ldr     x0, =0x10bf571850c453ce
        .inst   0xd95ff060
        ldr     x1, =0xe529c8c8d0cc27d2
        adr     x2, name_51
        bl      check
        b       1f
name_51: .asciz "ldapur_x_m1_x3"
        .balign 4
1:
        // ldapur_x x0, [x25, #0]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #0]
        mov     x25, x3
        ldr     x0, =0xb4a8a098297ac34f
        .inst   0xd9400320
        ldr     x1, =0x7172737475767778
        adr     x2, name_52
        bl      check
        b       1f
name_52: .asciz "ldapur_x_0_x25"
        .balign 4
1:
        // ldapur_x x0, [x3, #1]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #1]
        ldr     x0, =0x8ff570b89c6bcb8b
        .inst   0xd9401060
        ldr     x1, =0x7172737475767778
        adr     x2, name_53
        bl      check
        b       1f
name_53: .asciz "ldapur_x_1_x3"
        .balign 4
1:
        // ldapur_x x0, [x3, #7]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #7]
        ldr     x0, =0xa6196a6354ea4890
        .inst   0xd9407060
        ldr     x1, =0x7172737475767778
        adr     x2, name_54
        bl      check
        b       1f
name_54: .asciz "ldapur_x_7_x3"
        .balign 4
1:
        // ldapur_x x0, [x25, #119]
        ldr     x5, =0xa99ad8f7f5c639ea
        stur    x5, [x3, #119]
        mov     x25, x3
        ldr     x0, =0x67c2b89b412b5605
        .inst   0xd9477320
        ldr     x1, =0xa99ad8f7f5c639ea
        adr     x2, name_55
        bl      check
        b       1f
name_55: .asciz "ldapur_x_119_x25"
        .balign 4
1:
        // ldapur_x x0, [x3, #4]
        ldr     x5, =0x9fc37dc018bf3b8b
        stur    x5, [x3, #4]
        ldr     x0, =0x84c24128670ccb1d
        .inst   0xd9404060
        ldr     x1, =0x9fc37dc018bf3b8b
        adr     x2, name_56
        bl      check
        b       1f
name_56: .asciz "ldapur_x_4_x3"
        .balign 4
1:
        // ldapursb_x x0, [x25, #-128]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #-128]
        mov     x25, x3
        ldr     x0, =0x79c31596d975f89f
        .inst   0x19980320
        ldr     x1, =0xffffffffffffff88
        adr     x2, name_57
        bl      check
        b       1f
name_57: .asciz "ldapursb_x_m128_x25"
        .balign 4
1:
        // ldapursb_x x0, [x25, #-1]
        ldr     x5, =0xf5950306090abf6c
        stur    x5, [x3, #-1]
        mov     x25, x3
        ldr     x0, =0x6fc17805ab0a095c
        .inst   0x199ff320
        ldr     x1, =0x6c
        adr     x2, name_58
        bl      check
        b       1f
name_58: .asciz "ldapursb_x_m1_x25"
        .balign 4
1:
        // ldapursb_x x0, [x3, #0]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #0]
        ldr     x0, =0xaee406cde89aaa1
        .inst   0x19800060
        ldr     x1, =0xffffffffffffff88
        adr     x2, name_59
        bl      check
        b       1f
name_59: .asciz "ldapursb_x_0_x3"
        .balign 4
1:
        // ldapursb_x x0, [x3, #1]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #1]
        ldr     x0, =0xbf7ae080bca98cfa
        .inst   0x19801060
        ldr     x1, =0x78
        adr     x2, name_60
        bl      check
        b       1f
name_60: .asciz "ldapursb_x_1_x3"
        .balign 4
1:
        // ldapursb_x x0, [x3, #7]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #7]
        ldr     x0, =0xcb07ae6084f686c4
        .inst   0x19807060
        ldr     x1, =0xffffffffffffff88
        adr     x2, name_61
        bl      check
        b       1f
name_61: .asciz "ldapursb_x_7_x3"
        .balign 4
1:
        // ldapursb_x x0, [x25, #119]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #119]
        mov     x25, x3
        ldr     x0, =0xc952d02dace4a2ea
        .inst   0x19877320
        ldr     x1, =0x78
        adr     x2, name_62
        bl      check
        b       1f
name_62: .asciz "ldapursb_x_119_x25"
        .balign 4
1:
        // ldapursb_x x0, [x25, #48]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #48]
        mov     x25, x3
        ldr     x0, =0x8b8226084fcdcb82
        .inst   0x19830320
        ldr     x1, =0xffffffffffffff88
        adr     x2, name_63
        bl      check
        b       1f
name_63: .asciz "ldapursb_x_48_x25"
        .balign 4
1:
        // ldapursb_w x0, [x25, #-128]
        ldr     x5, =0xa658c729930bd410
        stur    x5, [x3, #-128]
        mov     x25, x3
        ldr     x0, =0x24a6e2b1673371de
        .inst   0x19d80320
        ldr     x1, =0x10
        adr     x2, name_64
        bl      check
        b       1f
name_64: .asciz "ldapursb_w_m128_x25"
        .balign 4
1:
        b       2f
        .ltorg
2:
        // ldapursb_w x0, [x3, #-1]
        ldr     x5, =0x9d0b3ce4b6aada00
        stur    x5, [x3, #-1]
        ldr     x0, =0xd7d78b3a83bead69
        .inst   0x19dff060
        ldr     x1, =0x0
        adr     x2, name_65
        bl      check
        b       1f
name_65: .asciz "ldapursb_w_m1_x3"
        .balign 4
1:
        // ldapursb_w x0, [x25, #0]
        ldr     x5, =0xef93b835dea89773
        stur    x5, [x3, #0]
        mov     x25, x3
        ldr     x0, =0xdf55aa515eda0fab
        .inst   0x19c00320
        ldr     x1, =0x73
        adr     x2, name_66
        bl      check
        b       1f
name_66: .asciz "ldapursb_w_0_x25"
        .balign 4
1:
        // ldapursb_w x0, [x3, #1]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #1]
        ldr     x0, =0x9477c792143c1103
        .inst   0x19c01060
        ldr     x1, =0x78
        adr     x2, name_67
        bl      check
        b       1f
name_67: .asciz "ldapursb_w_1_x3"
        .balign 4
1:
        // ldapursb_w x0, [x25, #7]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #7]
        mov     x25, x3
        ldr     x0, =0x8e1d13d5244018db
        .inst   0x19c07320
        ldr     x1, =0xffffff88
        adr     x2, name_68
        bl      check
        b       1f
name_68: .asciz "ldapursb_w_7_x25"
        .balign 4
1:
        // ldapursb_w x0, [x25, #119]
        ldr     x5, =0x89553fc5c99402a0
        stur    x5, [x3, #119]
        mov     x25, x3
        ldr     x0, =0x61fedbb3788d4f86
        .inst   0x19c77320
        ldr     x1, =0xffffffa0
        adr     x2, name_69
        bl      check
        b       1f
name_69: .asciz "ldapursb_w_119_x25"
        .balign 4
1:
        // ldapursb_w x0, [x25, #2]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #2]
        mov     x25, x3
        ldr     x0, =0x8dda7403d8cec544
        .inst   0x19c02320
        ldr     x1, =0x78
        adr     x2, name_70
        bl      check
        b       1f
name_70: .asciz "ldapursb_w_2_x25"
        .balign 4
1:
        // ldapursh_x x0, [x3, #-128]
        ldr     x5, =0xf6bd4e8cb2b481fb
        stur    x5, [x3, #-128]
        ldr     x0, =0x151ba740aa45b040
        .inst   0x59980060
        ldr     x1, =0xffffffffffff81fb
        adr     x2, name_71
        bl      check
        b       1f
name_71: .asciz "ldapursh_x_m128_x3"
        .balign 4
1:
        // ldapursh_x x0, [x25, #-1]
        ldr     x5, =0xd2197bd3f86be210
        stur    x5, [x3, #-1]
        mov     x25, x3
        ldr     x0, =0xd4588375bb764da3
        .inst   0x599ff320
        ldr     x1, =0xffffffffffffe210
        adr     x2, name_72
        bl      check
        b       1f
name_72: .asciz "ldapursh_x_m1_x25"
        .balign 4
1:
        // ldapursh_x x0, [x3, #0]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #0]
        ldr     x0, =0xf9f7b3464a52c82d
        .inst   0x59800060
        ldr     x1, =0xffffffffffff8788
        adr     x2, name_73
        bl      check
        b       1f
name_73: .asciz "ldapursh_x_0_x3"
        .balign 4
1:
        // ldapursh_x x0, [x3, #1]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #1]
        ldr     x0, =0x6671302cb3ab20f3
        .inst   0x59801060
        ldr     x1, =0x7778
        adr     x2, name_74
        bl      check
        b       1f
name_74: .asciz "ldapursh_x_1_x3"
        .balign 4
1:
        // ldapursh_x x0, [x25, #7]
        ldr     x5, =0xedfbd1d81ce49284
        stur    x5, [x3, #7]
        mov     x25, x3
        ldr     x0, =0xc276c4708e35b5a4
        .inst   0x59807320
        ldr     x1, =0xffffffffffff9284
        adr     x2, name_75
        bl      check
        b       1f
name_75: .asciz "ldapursh_x_7_x25"
        .balign 4
1:
        // ldapursh_x x0, [x3, #119]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #119]
        ldr     x0, =0xdba7cbe9ee7abf7b
        .inst   0x59877060
        ldr     x1, =0xffffffffffff8788
        adr     x2, name_76
        bl      check
        b       1f
name_76: .asciz "ldapursh_x_119_x3"
        .balign 4
1:
        // ldapursh_x x0, [x25, #-34]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #-34]
        mov     x25, x3
        ldr     x0, =0xb8cfdaf8f88facd4
        .inst   0x599de320
        ldr     x1, =0xffffffffffff8788
        adr     x2, name_77
        bl      check
        b       1f
name_77: .asciz "ldapursh_x_m34_x25"
        .balign 4
1:
        // ldapursh_w x0, [x3, #-128]
        ldr     x5, =0xf3b458c5439d3521
        stur    x5, [x3, #-128]
        ldr     x0, =0x7432a0e29f916200
        .inst   0x59d80060
        ldr     x1, =0x3521
        adr     x2, name_78
        bl      check
        b       1f
name_78: .asciz "ldapursh_w_m128_x3"
        .balign 4
1:
        // ldapursh_w x0, [x25, #-1]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #-1]
        mov     x25, x3
        ldr     x0, =0x376e02ccb3b7bc1b
        .inst   0x59dff320
        ldr     x1, =0xffff8788
        adr     x2, name_79
        bl      check
        b       1f
name_79: .asciz "ldapursh_w_m1_x25"
        .balign 4
1:
        // ldapursh_w x0, [x3, #0]
        ldr     x5, =0x8b11bca82baa2f54
        stur    x5, [x3, #0]
        ldr     x0, =0xa7f896aecfab94ed
        .inst   0x59c00060
        ldr     x1, =0x2f54
        adr     x2, name_80
        bl      check
        b       1f
name_80: .asciz "ldapursh_w_0_x3"
        .balign 4
1:
        b       2f
        .ltorg
2:
        // ldapursh_w x0, [x3, #1]
        ldr     x5, =0xf2ddc46d5d09684f
        stur    x5, [x3, #1]
        ldr     x0, =0xaa94ca29c08a4f4e
        .inst   0x59c01060
        ldr     x1, =0x684f
        adr     x2, name_81
        bl      check
        b       1f
name_81: .asciz "ldapursh_w_1_x3"
        .balign 4
1:
        // ldapursh_w x0, [x3, #7]
        ldr     x5, =0xbbfe2e7656e16cb4
        stur    x5, [x3, #7]
        ldr     x0, =0x367f1ced7241953
        .inst   0x59c07060
        ldr     x1, =0x6cb4
        adr     x2, name_82
        bl      check
        b       1f
name_82: .asciz "ldapursh_w_7_x3"
        .balign 4
1:
        // ldapursh_w x0, [x25, #119]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #119]
        mov     x25, x3
        ldr     x0, =0xebc92c46f28c6a5a
        .inst   0x59c77320
        ldr     x1, =0x7778
        adr     x2, name_83
        bl      check
        b       1f
name_83: .asciz "ldapursh_w_119_x25"
        .balign 4
1:
        // ldapursh_w x0, [x25, #-120]
        ldr     x5, =0x7172737475767778
        stur    x5, [x3, #-120]
        mov     x25, x3
        ldr     x0, =0x2ab2d30b87f19905
        .inst   0x59d88320
        ldr     x1, =0x7778
        adr     x2, name_84
        bl      check
        b       1f
name_84: .asciz "ldapursh_w_m120_x25"
        .balign 4
1:
        // ldapursw x0, [x25, #-128]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #-128]
        mov     x25, x3
        ldr     x0, =0x9975ebecbbc565ad
        .inst   0x99980320
        ldr     x1, =0xffffffff85868788
        adr     x2, name_85
        bl      check
        b       1f
name_85: .asciz "ldapursw_m128_x25"
        .balign 4
1:
        // ldapursw x0, [x25, #-1]
        ldr     x5, =0x88ab71b70096778a
        stur    x5, [x3, #-1]
        mov     x25, x3
        ldr     x0, =0x6fef0b44651920fb
        .inst   0x999ff320
        ldr     x1, =0x96778a
        adr     x2, name_86
        bl      check
        b       1f
name_86: .asciz "ldapursw_m1_x25"
        .balign 4
1:
        // ldapursw x0, [x3, #0]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #0]
        ldr     x0, =0xacd24ace578f4827
        .inst   0x99800060
        ldr     x1, =0xffffffff85868788
        adr     x2, name_87
        bl      check
        b       1f
name_87: .asciz "ldapursw_0_x3"
        .balign 4
1:
        // ldapursw x0, [x25, #1]
        ldr     x5, =0xbe2f2bf137eb0d5b
        stur    x5, [x3, #1]
        mov     x25, x3
        ldr     x0, =0xda0c3e7f61593fae
        .inst   0x99801320
        ldr     x1, =0x37eb0d5b
        adr     x2, name_88
        bl      check
        b       1f
name_88: .asciz "ldapursw_1_x25"
        .balign 4
1:
        // ldapursw x0, [x25, #7]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #7]
        mov     x25, x3
        ldr     x0, =0x903ba459e1d942dc
        .inst   0x99807320
        ldr     x1, =0xffffffff85868788
        adr     x2, name_89
        bl      check
        b       1f
name_89: .asciz "ldapursw_7_x25"
        .balign 4
1:
        // ldapursw x0, [x25, #119]
        ldr     x5, =0x8182838485868788
        stur    x5, [x3, #119]
        mov     x25, x3
        ldr     x0, =0xbb587800c7327b28
        .inst   0x99877320
        ldr     x1, =0xffffffff85868788
        adr     x2, name_90
        bl      check
        b       1f
name_90: .asciz "ldapursw_119_x25"
        .balign 4
1:
        // ldapursw x0, [x3, #102]
        ldr     x5, =0x868311f6e6b6231a
        stur    x5, [x3, #102]
        ldr     x0, =0x769cc4be16475b5c
        .inst   0x99866060
        ldr     x1, =0xffffffffe6b6231a
        adr     x2, name_91
        bl      check
        b       1f
name_91: .asciz "ldapursw_102_x3"
        .balign 4
1:
        test_exit
        .ltorg
