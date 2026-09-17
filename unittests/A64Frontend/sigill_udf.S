// SPDX-License-Identifier: MIT
// UDF #0 must raise SIGILL (exit status 132 under the default action).
#include "common.S"
        test_start
        mov     x9, #0x1234
        bl      dump
        udf     #0
        test_exit
