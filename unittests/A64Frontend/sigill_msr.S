// SPDX-License-Identifier: MIT
// MSR TPIDRRO_EL0 is not writable from EL0: SIGILL, exit status 132.
#include "common.S"
        test_start
        bl      dump
        msr     tpidrro_el0, x0
        test_exit
