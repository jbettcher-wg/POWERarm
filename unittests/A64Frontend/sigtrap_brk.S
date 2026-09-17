// SPDX-License-Identifier: MIT
// BRK must raise SIGTRAP (exit status 133 under the default action).
#include "common.S"
        test_start
        bl      dump
        brk     #0x3e8
        test_exit
