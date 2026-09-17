# M1b: the FP/SIMD subset static programs use

Measured, not guessed: every instruction word in the executable sections of
the corpus below is decoded against the frontend's own table (`a64.inc`, in
`DecodeTable.cpp`'s order) by `unittests/A64Frontend/simd_census.py`, and the
words that land on an FP/SIMD entry are counted. Counts are static
occurrences, not execution counts.

## Corpus

All built on the Raspberry Pi 5 (Debian trixie, gcc 14.2, glibc 2.41,
musl 1.2.5 from Debian's `musl-dev`, used without root through a rewritten
`musl-gcc.specs`), `-static -O2`:

| Name | Binary |
|---|---|
| `hello` | `unittests/A64Frontend/hello.c` against glibc |
| `printf` | `printf_float.c` (`%f %e %g %a`, `strtod`) against glibc |
| `strmem` | `strmem.c` (memcpy/memmove/memset/strlen/strnlen/strchr/strrchr/memchr/memrchr/strcmp/strncmp/memcmp/strcpy/stpcpy over lengths 0-129 and 16 alignments) against glibc |
| `fpmath` | `fpmath.c` (scalar arithmetic, compares, int/float conversions, `fmin`/`fmax`/`lround`) against glibc |
| `m_hello`, `m_printf`, `m_strmem` | the same sources against musl |
| `busybox` | Debian `busybox-static` 1:1.37.0-6+b9 (stripped, glibc) |

To regenerate: build the corpus as above, then
`simd_census.py --markdown hello=... printf=... ... busybox=...`.

## Reachable IFUNC variants

glibc's aarch64 string routines are IFUNCs. For the presented CPU (HWCAP
`fp|asimd|cpuid`, MIDR Cortex-A76 r4p1, DCZID 64-byte blocks, no SVE, no MOPS)
the selectors pick the `_generic`/`_zva64`/`_asimd` variants; `_sve`, `_mops`,
`_a64fx`, `_oryon1`, `_emag`, `_kunpeng`, `_thunderx*` and `_falkor` are never
selected. Those are counted separately at the end (by symbol name, so only for
the unstripped glibc binaries; busybox is stripped and counts everything).
For example `memset`'s resolver (`__libc_memset_ifunc`) returns
`__memset_zva64` when the ZVA size is 64 and `__memset_generic` otherwise; both
start with `dup v0.16b, w1`.

SVE instructions are not in `a64.inc` at all and are therefore not counted.

## Headline

| Rank | Class | Count |
|---:|---|---:|
| 1 | SIMD&FP register load/store (`stp/ldp/str/ldr/stur/ldur` of d/q, `ld1`) | 4815 |
| 2 | FP/integer conversion (`fmov` gpr<->fp dominates: 788; `scvtf`, `fcvtzs`, `ucvtf`, `fcvtzu`, `fcvtas`) | 843 |
| 3 | SIMD three same (`umaxp`, `orr`/`mov`, `add`, `cmeq`, `eor`, `cmhs`, `bit`, `addp`, `and`, `uminp`, `sub`, `cmtst`, `bic`, `orn`) | 536 |
| 4 | SIMD modified immediate (`movi`, `mvni`, `bic`) | 392 |
| 5 | SIMD two-register misc (`cmeq #0`, `cnt`, `xtn`, `rev64`, `rev32`, `mvn`) | 150 |
| 6 | SIMD shift by immediate (`shrn`, `shl`, `ushr`, `uxtl`, `sxtl`/`sshll`) | 147 |
| 7 | FP two-register (`fmul`, `fdiv`, `fadd`, `fsub`, `fminnm`, `fmaxnm`) | 109 |
| 8 | SIMD copy (`dup`, `umov`/`mov`, `ins`) | 108 |
| 9 | FP one-register (`fmov`, `fabs`, `fcvt`, `fneg`, `fsqrt`) | 104 |
| 10 | MRS FPCR/FPSR | 87 |
| 11 | SIMD three different (`saddl`, `saddl2`, `addhn`, `uaddw`) | 60 |
| 12 | FP compare (`fcmp`, `fcmpe`) | 56 |
| 13 | FP immediate (`fmov #imm`) | 38 |
| 14 | FP conditional select (`fcsel`) | 33 |
| 15 | `ext` | 14 |
| 16 | scalar SIMD conversions (`fcvtzs`, `scvtf`, `ucvtf`, `fcvtzu` on d registers) | 12 |
| 17 | `uzp1` | 10 |
| 18 | `addv` | 8 |
| 19 | scalar shifts (`shl`, `ushr` on d registers) | 7 |
| 20 | `cmge #0` (scalar) | 5 |
| 21 | `fmadd`, `fmsub` | 2 |
| 22 | `scvtf` fixed-point | 1 |

No `fccmp`, `frint*`, `fcvtns`/`fcvtnu`, `tbl`/`tbx`, `pmull`, AES, SHA, CRC32
or LD2-4/ST2-4 structure loads appear in the reachable corpus; `sha256sum` and
`md5sum` in busybox are plain integer code.

## Implementation status

Translated (decoder entries with a translator, `IRBuilder::HandlerTable`):

| Class | Entries |
|---|---:|
| SIMD&FP register loads/stores (literal, unsigned offset, unscaled, pre/post-index, register offset, pairs) | 10 |
| LD1/ST1 (1-4 registers; LD2-4/ST2-4 not translated) | 4 |
| SIMD copy (DUP general/element, INS general/element, UMOV, SMOV) and scalar DUP | 7 |
| SIMD modified immediate (MOVI/MVNI/ORR/BIC, FMOV single/double; FMOV half not translated) | 2 |
| SIMD three same (ADD, SUB, CMEQ/CMGT/CMGE/CMHI/CMHS/CMTST, AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF, UMAX/UMIN/SMAX/SMIN, ADDP/UMAXP/UMINP) | 23 |
| SIMD scalar three same (ADD, SUB, CMEQ/CMGT/CMGE/CMHI/CMHS/CMTST on D) | 8 |
| SIMD two-register misc (CMEQ/CMGT/CMGE/CMLE/CMLT #0, CNT, NOT, NEG, ABS, REV32, REV64, XTN, FCVTL, FCVTN) | 14 |
| SIMD scalar two-register misc (CMEQ/CMGT/CMGE/CMLE/CMLT #0 on D; FCVTZS/FCVTZU/SCVTF/UCVTF scalar) | 9 |
| SIMD across lanes (ADDV, UMAXV, UMINV) | 3 |
| SIMD three different (SADDL, UADDL, SSUBL, USUBL, SADDW, UADDW, ADDHN, SUBHN, with the 2 forms) | 8 |
| SIMD shift by immediate (SSHR, USHR, SHL, SHRN, SSHLL, USHLL; scalar SSHR/USHR/SHL) | 9 |
| EXT, UZP1/UZP2/ZIP1/ZIP2/TRN1/TRN2 | 7 |
| FP/integer and FP/fixed conversions (all FCVT[NPMZA][SU], SCVTF/UCVTF, FMOV general) | 17 |
| FP one register (FMOV, FABS, FNEG, FSQRT, FCVT; no FRINT*) | 5 |
| FP compare, immediate, conditional compare/select | 6 |
| FP two register (FADD/FSUB/FMUL/FDIV/FNMUL, FMIN/FMAX/FMINNM/FMAXNM) | 9 |
| FP three register (FMADD, FMSUB, FNMADD, FNMSUB) | 4 |
| **SIMD and FP total** | **145** |
| Exclusive and atomic-width loads/stores (LDXR/LDAXR/STXR/STLXR, LDAR/LDLAR/STLR/STLLR), needed by glibc's locks | 8 |

Scalar FP covers half, single and double precision. FPCR.RMode reaches the
host rounding mode on MSR; FZ16 is emulated for half precision; FZ, DN and
AHP are stored only, and FPSR's cumulative exception bits are not raised
(`POWERARM-M1-TODO(fpu)` markers).

IR ops added (IR.json, lowered in `JIT/PPC64LE/A64FPOps.cpp`), because the
existing conversion ops have x86 semantics:

- `A64FloatToGPR`: FPToFixed with every A64 rounding, saturation, NaN -> 0.
  `Float_ToGPR_ZS` returns INT_MIN for NaN and positive overflow and has no
  unsigned form.
- `A64FloatFromGPR`: signed or unsigned, one rounding. `Float_FromGPR_S` is
  signed only and converts i64 -> f32 through a double.
- `A64FToF`: FCVT half/single/double, quieting a signalling NaN.
  `Float_FToF` keeps it signalling and has no half precision. The half
  conversions use `xscvhpdp`/`xscvdphp` on ISA 3.0 and GPR code on POWER8.

Everything else maps onto existing IR ops; A64 semantics the host op does
not have (NaN operand precedence, FMIN/FMAX, the fused multiply-add group,
unsigned vector compares, 64-bit vector lanes) are composed in the frontend.

Tests (`unittests/A64Frontend`, 46 programs, about 6000 generated SIMD/FP
cases plus the corpus): all pass on the POWER9 by default, with
`POWERARM_MAXINST=1` and with `POWERARM_HOSTFEATURES=disableisa30`.

Exit targets, all matching the Pi byte for byte: static glibc and musl
`hello`, `printf_float` (`%f %e %g %a`, `strtod`) against both libcs, the
string/memory exerciser and scalar FP program against both, and busybox
`echo`, `cat`, `wc`, `sort`, `sort -n`, `sha256sum`, `md5sum`.

### Backend lowering bugs found

In `FEXCore/Source/Interface/Core/JIT/PPC64LE/VectorOps.cpp` (they may also
affect fastppcx86):

- `VFNMLA` (3747-3748) and `VFNMLS` (3767-3768) use `xvnmsub*`/`xvnmadd*`,
  which negate after rounding. The IR ops (x86 FNMADD/FNMSUB) are one
  rounding of the negated expression, so results differ under FE_UPWARD and
  FE_DOWNWARD, and the sign of a NaN result is flipped.
- `Float_FromGPR_S` (4557-4558) converts i64 -> f32 with `fcfid` then `frsp`:
  two roundings, wrong for integers that are not exact in double (e.g.
  2^53+1 near a single-precision tie). `fcfids` rounds once.
- `VAddV` 32-bit (586) uses `vsumsws`, which saturates; ADDV wraps.


Per-entry counts, grouped by class, per binary. "Entry" is the `a64.inc`
decoder entry, "Mnemonic" is objdump's alias.

### SIMD&FP register load/store: 4815

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| STP_LDP_fpsimd | stp | 1432 | 182 | 188 | 187 | 190 | 9 | 30 | 15 | 631 | .text, _IO_init, _IO_init_internal |
| STR_imm_fpsimd_2 | str | 965 | 95 | 95 | 95 | 96 | 5 | 63 | 27 | 489 | .text, _IO_fwide, _IO_init |
| LDR_imm_fpsimd_2 | ldr | 811 | 91 | 93 | 91 | 92 | 0 | 114 | 40 | 290 | .text, _IO_fwide, _IO_init |
| STP_LDP_fpsimd | ldp | 602 | 89 | 90 | 89 | 92 | 0 | 5 | 1 | 236 | .text, _Unwind_Backtrace, _Unwind_ForcedUnwind |
| STUR_fpsimd | stur | 523 | 80 | 80 | 80 | 80 | 2 | 3 | 2 | 196 | .text, _IO_default_setbuf, _IO_init |
| LDUR_fpsimd | ldur | 252 | 22 | 24 | 25 | 24 | 0 | 4 | 2 | 151 | .text, _IO_printf, ___asprintf |
| LDR_imm_fpsimd_1 | ldr | 63 | 12 | 12 | 13 | 12 | 0 | 0 | 0 | 14 | .text, __memchr_generic, __memcmpeq |
| LDx_mult_1 | ld1 | 47 | 9 | 9 | 10 | 9 | 0 | 0 | 0 | 10 | .text, __memchr_generic, __memrchr |
| STR_reg_fpsimd | str | 47 | 7 | 7 | 7 | 7 | 0 | 0 | 0 | 19 | .text, __memset_generic, __stpcpy |
| LDR_reg_fpsimd | ldr | 36 | 5 | 6 | 5 | 7 | 0 | 1 | 0 | 12 | .text, __stpcpy, _dl_load_cache_lookup |
| STR_imm_fpsimd_1 | str | 32 | 5 | 5 | 5 | 5 | 0 | 0 | 0 | 12 | .text, __memmove_generic, __stpcpy |
| LDx_mult_2 | ld1 | 5 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 1 | .text, strrchr |

### Conversion between floating point and integer: 843

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| FMOV_float_gen | fmov | 788 | 68 | 99 | 72 | 70 | 0 | 170 | 90 | 219 | .text, ____strtod_l_internal, ____strtof_l_internal |
| SCVTF_float_int | scvtf | 24 | 0 | 1 | 0 | 1 | 0 | 5 | 0 | 17 | .text, __floatscan, decfloat |
| FCVTZS_float_int | fcvtzs | 13 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 11 | .text, main |
| UCVTF_float_int | ucvtf | 10 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10 | .text |
| FCVTZU_float_int | fcvtzu | 7 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 5 | .text, main |
| FCVTAS_float | fcvtas | 1 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | __lround |

### SIMD three same: 536

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| UMAXP | umaxp | 116 | 21 | 21 | 23 | 21 | 0 | 0 | 0 | 30 | .text, __memchr_generic, __memcmpeq |
| ORR_asimd_reg | mov | 100 | 6 | 8 | 6 | 6 | 0 | 44 | 9 | 21 | .text, __floatscan, __printf_fp_buffer_1.isra.0 |
| ADD_vector | add | 79 | 14 | 14 | 14 | 14 | 0 | 0 | 0 | 23 | .text, find_derivation, read_alias_file |
| CMEQ_reg_2 | cmeq | 71 | 13 | 13 | 16 | 13 | 0 | 0 | 0 | 16 | .text, __memchr_generic, __memrchr |
| EOR_asimd | eor | 45 | 8 | 8 | 8 | 8 | 0 | 0 | 0 | 13 | .text, __gconv_find_shlib, __memcmpeq |
| CMHS_2 | cmhs | 35 | 7 | 7 | 7 | 7 | 0 | 0 | 0 | 7 | .text, __strchrnul, strchr |
| BIT | bit | 25 | 5 | 5 | 5 | 5 | 0 | 0 | 0 | 5 | .text, strchr, strrchr |
| ADDP_vec | addp | 15 | 3 | 3 | 3 | 3 | 0 | 0 | 0 | 3 | .text, __strlen_asimd, strrchr |
| AND_asimd | and | 12 | 2 | 2 | 2 | 2 | 0 | 0 | 0 | 4 | .text, __strlen_asimd |
| ORR_asimd_reg | orr | 12 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 12 | .text |
| UMINP | uminp | 10 | 2 | 2 | 2 | 2 | 0 | 0 | 0 | 2 | .text, __strlen_asimd |
| SUB_2 | sub | 8 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 4 | .text, uw_frame_state_for |
| CMTST_2 | cmtst | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 4 | .text |
| BIC_asimd_reg | bic | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 3 | .text |
| ORN_asimd | orn | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | .text |

### SIMD modified immediate: 392

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| MOVI | movi | 353 | 40 | 53 | 40 | 41 | 0 | 24 | 8 | 147 | .text, _IO_default_setbuf, _IO_init |
| MOVI | mvni | 29 | 1 | 5 | 1 | 1 | 0 | 2 | 0 | 19 | .text, ____strtof_l_internal, __trunctfsf2 |
| MOVI | bic | 10 | 2 | 2 | 2 | 2 | 0 | 0 | 0 | 2 | .text, strrchr |

### SIMD Two-register misc: 150

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| CMEQ_zero_2 | cmeq | 120 | 24 | 24 | 24 | 24 | 0 | 0 | 0 | 24 | .text, __stpcpy, __strlen_asimd |
| CNT | cnt | 8 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 4 | .text, __sched_cpucount |
| XTN | xtn | 7 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 3 | .text, __libc_mallinfo |
| REV64_asimd | rev64 | 5 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 1 | .text, find_derivation |
| REV32_asimd | rev32 | 5 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 1 | .text, _nl_load_domain |
| NOT | mvn | 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 5 | .text |

### SIMD Shift by immediate: 147

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| SHRN | shrn | 99 | 19 | 19 | 21 | 19 | 0 | 0 | 0 | 21 | .text, __memchr_generic, __memrchr |
| SHL_2 | shl | 40 | 8 | 8 | 8 | 8 | 0 | 0 | 0 | 8 | .text, uw_frame_state_for |
| USHR_2 | ushr | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 4 | .text |
| USHLL | uxtl | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | .text |
| SSHLL | sshll | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | .text |
| SSHLL | sxtl | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | .text |

### Floating point data processing two register: 109

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| FMUL_float | fmul | 53 | 1 | 14 | 1 | 2 | 0 | 14 | 1 | 20 | .text, ____strtod_l_internal, ____strtof_l_internal |
| FDIV_float | fdiv | 20 | 2 | 2 | 2 | 4 | 0 | 2 | 2 | 6 | .text, __sfp_handle_exceptions, main |
| FADD_float | fadd | 18 | 1 | 3 | 1 | 3 | 0 | 1 | 1 | 8 | .text, __sfp_handle_exceptions, main |
| FSUB_float | fsub | 16 | 1 | 1 | 1 | 3 | 0 | 1 | 1 | 8 | .text, __sfp_handle_exceptions, main |
| FMINNM_float | fminnm | 1 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | main |
| FMAXNM_float | fmaxnm | 1 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | main |

### SIMD Copy: 108

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| DUP_gen | dup | 74 | 12 | 12 | 13 | 12 | 1 | 1 | 1 | 22 | .text, __memchr_generic, __memrchr |
| UMOV | mov | 9 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 2 | .text, __tcgetattr, memset |
| INS_gen | mov | 8 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 4 | .text, do_lookup_x |
| DUP_elt_2 | dup | 6 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 2 | .text, __gconv_find_shlib |
| UMOV | umov | 6 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 2 | .text, _nl_intern_locale_data |
| INS_elt | mov | 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 5 | .text |

### Floating point data processing: 104

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| FMOV_float | fmov | 76 | 0 | 19 | 0 | 3 | 0 | 21 | 0 | 33 | .text, __floatscan, main |
| FABS_float | fabs | 11 | 2 | 2 | 2 | 3 | 0 | 0 | 0 | 2 | .text, __printf_fp_buffer_1.isra.0, __printf_fphex_buffer |
| FCVT_float | fcvt | 10 | 0 | 3 | 0 | 2 | 0 | 3 | 0 | 2 | .text, main |
| FNEG_float | fneg | 6 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 4 | .text, ____strtod_l_internal, ____strtof_l_internal |
| FSQRT_float | fsqrt | 1 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | main |

### System register FPCR/FPSR: 87

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| MRS | mrs | 87 | 10 | 12 | 10 | 10 | 0 | 18 | 12 | 15 | .text, __addtf3, __divtf3 |

### SIMD three different: 60

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| SADDL | saddl | 20 | 4 | 4 | 4 | 4 | 0 | 0 | 0 | 4 | .text, uw_frame_state_for |
| SADDL | saddl2 | 20 | 4 | 4 | 4 | 4 | 0 | 0 | 0 | 4 | .text, uw_frame_state_for |
| ADDHN | addhn | 10 | 2 | 2 | 2 | 2 | 0 | 0 | 0 | 2 | .text, __strlen_generic |
| UADDW | uaddw | 10 | 2 | 2 | 2 | 2 | 0 | 0 | 0 | 2 | .text, _nl_load_domain |

### Floating point compare: 56

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| FCMP_float | fcmp | 34 | 4 | 4 | 4 | 6 | 0 | 0 | 0 | 16 | .text, __printf_fp_buffer_1.isra.0, __printf_fphex_buffer |
| FCMPE_float | fcmpe | 22 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 21 | .text, main |

### Floating point immediate: 38

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| FMOV_float_imm | fmov | 38 | 2 | 4 | 2 | 2 | 0 | 7 | 2 | 19 | .text, __floatscan, __sfp_handle_exceptions |

### Floating point conditional select: 33

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| FCSEL_float | fcsel | 33 | 0 | 14 | 0 | 0 | 0 | 1 | 0 | 18 | .text, ____strtod_l_internal, ____strtof_l_internal |

### SIMD Extract: 14

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| EXT | ext | 14 | 2 | 2 | 2 | 2 | 0 | 0 | 0 | 6 | .text, __libc_start_call_main, execute_stack_op |

### Scalar two register misc: 12

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| FCVTZS_int_2 | fcvtzs | 6 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 5 | .text, main |
| SCVTF_int_2 | scvtf | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 3 | .text |
| UCVTF_int_2 | ucvtf | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | .text |
| FCVTZU_int_2 | fcvtzu | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | .text |

### SIMD Permute: 10

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| UZP1 | uzp1 | 10 | 2 | 2 | 2 | 2 | 0 | 0 | 0 | 2 | .text, __libc_mallinfo |

### SIMD across lanes: 8

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| ADDV | addv | 8 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 4 | .text, __sched_cpucount |

### SIMD Scalar shift by immediate: 7

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| SHL_1 | shl | 4 | 0 | 0 | 0 | 0 | 0 | 4 | 0 | 0 | scalbn |
| USHR_1 | ushr | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 3 | .text |

### Scalar two-register misc: 5

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| CMGE_zero_1 | cmge | 5 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 1 | .text, uw_frame_state_for |

### Floating point data processing three register: 2

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| FMADD_float | fmadd | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | .text |
| FMSUB_float | fmsub | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | .text |

### Conversion between floating point and fixed point: 1

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| SCVTF_float_fix | scvtf | 1 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | decfloat |

### SIMD&FP register load/store (unreachable IFUNC variants only): 420

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| STP_LDP_fpsimd | stp | 200 | 50 | 50 | 50 | 50 | 0 | 0 | 0 | 0 | __memcpy_sve, __memmove_sve, __memset_kunpeng |
| STUR_fpsimd | stur | 68 | 17 | 17 | 17 | 17 | 0 | 0 | 0 | 0 | __memmove_sve, __memset_kunpeng, __memset_sve_zva64 |
| STP_LDP_fpsimd | ldp | 64 | 16 | 16 | 16 | 16 | 0 | 0 | 0 | 0 | __memcpy_sve, __memmove_sve |
| STR_imm_fpsimd_2 | str | 64 | 16 | 16 | 16 | 16 | 0 | 0 | 0 | 0 | __memcpy_sve, __memset_kunpeng, __memset_sve_zva64 |
| STR_reg_fpsimd | str | 12 | 3 | 3 | 3 | 3 | 0 | 0 | 0 | 0 | __memset_sve_zva64, __memset_zva64 |
| LDR_imm_fpsimd_2 | ldr | 4 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | __memcpy_sve |
| LDUR_fpsimd | ldur | 4 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | __memmove_sve |
| STR_imm_fpsimd_1 | str | 4 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | __memmove_sve |

### SIMD Copy (unreachable IFUNC variants only): 12

| Entry | Mnemonic | Total | hello | printf | strmem | fpmath | m_hello | m_printf | m_strmem | busybox | Example functions |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| DUP_gen | dup | 12 | 3 | 3 | 3 | 3 | 0 | 0 | 0 | 0 | __memset_kunpeng, __memset_sve_zva64, __memset_zva64 |
