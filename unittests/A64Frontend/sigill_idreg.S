// SPDX-License-Identifier: MIT
// MRS in the ID space with CRm=1 is not emulated by Linux: SIGILL, exit status 132.
#include "common.S"
        test_start
        bl      dump
        mrs     x0, S3_0_C0_C1_0
        test_exit
