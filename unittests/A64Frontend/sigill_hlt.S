// SPDX-License-Identifier: MIT
// HLT #0 must raise SIGILL (exit status 132 under the default action): HLT is
// undefined at EL0. POWERarm's thunk marker is HLT #0x0F3F; hlt.c covers the
// other immediates and the marker itself.
#include "common.S"
        test_start
        mov     x9, #0x1234
        bl      dump
        hlt     #0
        test_exit
