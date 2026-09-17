/* SPDX-License-Identifier: MIT */
/* A64Diff record format.  Keep in sync with gen/a64gen.py (REC_* constants).
 *
 * One record per test program, written with write(1, rec, A64D_REC_SIZE)
 * immediately before exit(0).  All fields little-endian.
 */
#pragma once
#include <stdint.h>

#define A64D_MAGIC "A64D"
#define A64D_FORMAT_VERSION 1
#define A64D_REC_SIZE 4944
#define A64D_PAGE_SIZE 4096

#define A64D_KIND_STATE 1  /* reached the dump pads after the test sequence */
#define A64D_KIND_SIGNAL 2 /* written from the SIGSEGV/SIGBUS/SIGILL/SIGTRAP/SIGFPE handler */

#define A64D_FLAG_SIMD 1 /* v[], fpcr and fpsr were captured (not yet generated) */

struct a64d_rec {
  char magic[4];      /*    0 "A64D" */
  uint16_t version;   /*    4 A64D_FORMAT_VERSION */
  uint16_t kind;      /*    6 A64D_KIND_* */
  uint32_t flags;     /*    8 A64D_FLAG_* */
  uint32_t size;      /*   12 A64D_REC_SIZE */
  uint64_t x[31];     /*   16 X0..X30 after the test (signal: from ucontext) */
  uint64_t sp;        /*  264 */
  uint64_t pcmark;    /*  272 state: landing pad id (0 fall-through, 1 forward, 2 backward)
                                signal: ucontext pc */
  uint64_t nzcv;      /*  280 NZCV in bits 31:28 */
  uint64_t reserved;  /*  288 */
  uint64_t signo;     /*  296 signal records only */
  uint64_t sigcode;   /*  304 si_code, sign-extended */
  uint64_t sigaddr;   /*  312 si_addr */
  uint8_t v[32 * 16]; /*  320 V0..V31 (A64D_FLAG_SIMD) */
  uint64_t fpcr;      /*  832 */
  uint64_t fpsr;      /*  840 */
  uint8_t page[A64D_PAGE_SIZE]; /* 848 the data page after the test */
};

_Static_assert(sizeof(struct a64d_rec) == A64D_REC_SIZE, "record size");
