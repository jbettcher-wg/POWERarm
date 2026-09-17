# A64Frontend tests

Small static AArch64 programs that check the A64 → FEX IR frontend
(`FEXCore/Source/Interface/Core/A64Frontend/`) against real hardware. They use
only raw `write` and `exit` system calls, so they need nothing from the Linux
layer beyond those two.

## How to run

On an AArch64 Linux machine with gcc (the reference is a Raspberry Pi 5):

```sh
unittests/A64Frontend/golden.sh OUTDIR
```

This generates the differential programs (`gen.py`, `gen_simd.py`), builds
them with `gcc -nostdlib -static` and the C corpus with `gcc -static`, runs
each one natively, and writes `OUTDIR/<test>.golden` (stdout) and
`OUTDIR/<test>.rc` (exit status).

Two optional inputs extend the corpus:

```sh
MUSL_ROOT=/path/to/unpacked/musl BUSYBOX=/path/to/busybox unittests/A64Frontend/golden.sh OUTDIR
```

`MUSL_ROOT` is Debian's `musl` and `musl-dev` packages unpacked with
`apt-get download musl musl-dev` and `dpkg-deb -x` (no root needed); it adds
`musl_*` builds of the C programs. `BUSYBOX` is a static AArch64 busybox
(Debian `busybox-static`, same method); it adds the `bb_*` applet tests on
the fixed input from `gen_applet_input.py`.

On the ppc64le host, with the same OUTDIR visible (the Pi writes it over the
shared mount):

```sh
unittests/A64Frontend/run.sh "$PWD/build-frontend/Bin/POWERarm" OUTDIR
```

`run.sh` leaves `POWERARM_HOSTPAGEMODE` at its default, `auto`: on a 64K host
every test runs natively except `loader_bss4k`, which is linked for 4K pages
and runs with the granule emulation. Any POWERarm configuration variable
passes through, for example `POWERARM_HOSTPAGEMODE=force`,
`POWERARM_MAXINST=1` (one instruction per block), `POWERARM_SMCCHECKS=full`
or `POWERARM_DISABLEDFCE=1`.

The POWER8 lowerings are exercised on a POWER9 with
`POWERARM_HOSTFEATURES=disableisa30`, which clears `SupportsISA30`. That the
knob takes effect was checked by scanning the JIT code buffer at guest exit:
the same program's translation holds 182 `mcrxrx` (an ISA 3.0 instruction)
by default and none with the knob set.

## What is checked

| Test | Kind | Covers |
|---|---|---|
| `addsub`, `adc`, `logic`, `movewide`, `bitfield`, `shiftvar`, `bits`, `csel`, `muldiv`, `branch`, `adr`, `loadstore`, `dczva` | differential, generated | one case per instruction: operands loaded, NZCV set, instruction run, every GPR, SP and NZCV printed (and `buf` after stores) |
| `flagsweep` | differential | ADDS/SUBS/CMP/CMN/ADCS/SBCS/ANDS/BICS/CCMP/CCMN and every condition consumer over all 8-bit operand pairs at the 32- and 64-bit sign boundary, hashed |
| `flags` | differential | NZCV across block exits, a syscall, BR, BL and a loop |
| `sigill_udf`, `sigill_msr`, `sigill_idreg`, `sigtrap_brk` | differential exit status | UDF, MSR to a read-only register and an unemulated ID register raise SIGILL (132); BRK raises SIGTRAP (133) |
| `sysreg` | self-checking | MRS/MSR against the presented CPU profile (`SystemRegisters.h`); prints PASS/FAIL per register |
| `loader_bss` | differential, self-checking | ELF loader: a host-aligned RW segment mapped straight from the file, followed by non-zero file bytes; everything past `p_filesz` reads as zero and is writable |
| `loader_bss_nofile` | differential, self-checking | ELF loader: an RW segment with `p_filesz == 0` and a 4K-aligned `p_vaddr` in a host page of its own |
| `loader_bss4k` | stated golden, self-checking | ELF loader: a 4K-aligned binary (`max-page-size=4096`) whose `p_filesz == 0` RW segment shares a host page with `.text`; 16K and 64K arm64 kernels cannot run it |
| `simd_loadstore` | differential, generated | B/H/S/D/Q loads and stores in every addressing form, pairs, LD1/ST1 with 1-4 registers, literals; V registers and `buf` printed |
| `simd_copy` | differential, generated | DUP, INS, UMOV, SMOV, MOV d, MOVI/MVNI/ORR/BIC immediates (LSL, MSL, 64-bit masks), FMOV vector immediates |
| `simd_arith` | differential, generated | ADD/SUB, register and zero compares (vector and scalar), CMTST, bitwise with BSL/BIT/BIF, UMAX/UMIN/SMAX/SMIN, pairwise, ADDV/UMAXV/UMINV, CNT, NOT, NEG, ABS, REV32/REV64, signed/unsigned boundary vectors |
| `simd_shape` | differential, generated | XTN, SHRN, widening add/sub, ADDHN/SUBHN, SSHLL/USHLL/SXTL/UXTL, SSHR/USHR/SHL (vector and scalar, shift 0 and full width), EXT, UZP/ZIP/TRN |
| `fp_scalar` | differential, generated | FP arithmetic, FMIN/FMAX(NM), fused multiply-add group, FABS/FNEG/FSQRT, FCVT, FMOV forms, FCMP/FCMPE/FCCMP/FCSEL with every condition, under all four rounding modes, with NaN operand pairs |
| `fp_convert` | differential, generated | FCVT{N,P,M,Z,A}{S,U} to W/X, SCVTF/UCVTF from integers and fixed point, FCVTZS/FCVTZU to fixed point, scalar SIMD conversions; every edge value through the rounding variants |
| `fp_fpcr` | differential | FPCR round trip and the rounding mode reaching arithmetic and conversions |
| `fp_half` | differential, generated | half precision: arithmetic, FMIN/FMAX(NM), fused group, FABS/FNEG/FSQRT, compares, FCSEL, FMOV forms, FCVT to/from single and double, conversions to/from integers and fixed point, vector FCVTL/FCVTN (half/single and single/double); every rounding mode with and without FZ16; double-to-half rounding and overflow boundaries |
| `fp_round` | differential, generated | FRINTN/P/M/Z/A/X/I on half, single and double: every edge value and ties (0.5 steps, just below 0.5, 2^52 boundary) in every FPCR rounding mode, plus random operands |
| `simd_struct1` | differential, generated | LD1/ST1 single-element structures: B/H/S/D lanes at every index, offset, post-index by element size and by register |
| `simd_projects` | differential, generated | instructions the M2 project builds reached that the groups above don't cover: scalar DUP (`mov b/h/s/dN, vM.<T>[i]`) of every element size |
| `simd_struct2` | differential, generated | LD2/ST2 interleaved structures: every element width, D and Q, offset and post-index |
| `simd_float` | differential, generated | Advanced SIMD floating point on single and double lanes, vector and scalar SIMD forms: FADD/FSUB/FMUL/FDIV/FMIN/FMAX(NM), FMUL/FMLA/FMLS by element, FMLA/FMLS, pairwise FADDP/FMAXP/FMINP(NM) vector and scalar, FCMEQ/FCMGE/FCMGT/FACGE/FACGT and the zero compares, FRINT*/FSQRT, FCVT{N,P,M,Z,A}{S,U}, fixed-point SCVTF/UCVTF/FCVTZS/FCVTZU, half-precision FNEG/FABS; NaN pairs lined up lane by lane, every edge value through the conversions and rounding |
| `simd_sat` | differential, generated | saturating SQADD/SQSUB/UQADD/UQSUB (vector and scalar, 64-bit lanes), SQABS/SQNEG, SQXTN/UQXTN/SQXTUN, RSHRN and the saturating (rounding) shift-right narrows, SRSHR/URSHR/SRSRA/URSRA, SQSHL/UQSHL/SQSHLU by immediate, UHADD/SHADD/URHADD/SRHADD/UHSUB/SHSUB, [SU]ABD/[SU]ABA(L), RADDHN/RSUBHN, SQDMULH/SQRDMULH (vector, scalar, by element), MLA/MLS and the long multiplies by element, UADDLV/SADDLV, SMAXV/SMINV, CLZ/CLS, UDOT; operands from the signed and unsigned lane boundaries; FPSR.QC cleared before each saturating case so every saturation is checked |
| `simd_crypto` | differential, generated | AESE/AESD/AESMC/AESIMC, PMULL/PMULL2 (8 and 64 bit), SHA1C/M/P/H/SU0/SU1, SHA256H/H2/SU0/SU1, CRC32B-X and CRC32CB-CX on random operands and aliased registers; FIPS-197 C.1 AES-128 encryption and decryption, SHA-1 and SHA-256 compressions of the padded "abc" block, the CRC-32/CRC-32C check values of "123456789" |
| `loadstore_rcpc2` | stated golden, self-checking | LRCPC2 STLUR*/LDAPUR*/LDAPURS* (the Pi 5 lacks LRCPC2): each load checked against the pattern stored with STUR, each store read back with LDUR |
| `exclusive` | differential, generated | LDXR/LDAXR then STXR/STLXR at every width: success, store without a load, a second store, CLREX in between, NZCV across a successful store; LDAR/STLR. A store to a different address than the load is IMPLEMENTATION DEFINED (the Pi lets it succeed within a region) and is not tested |
| `hello`, `printf_float`, `strmem`, `fpmath` (and `musl_*`) | differential | static glibc (and musl) programs: printf float formatting, the string/memory routines over lengths and alignments, scalar FP code |
| `bb_*` | differential | busybox `echo`, `cat`, `wc`, `sort`, `sort -n`, `sha256sum`, `md5sum` |

The SIMD/FP programs print `vdump` after `dump`: V0-V31, FPCR, and FPSR with
the cumulative exception bits (IOC, DZC, OFC, UFC, IXC, IDC) masked out,
because those are not emulated yet. FP operands carry random bits above the
operand width, so every clear of the upper register bits is checked, and
registers mix static (V0-V15) and context-backed (V16-V31) slots.

Generated inputs are an edge-case corpus plus seeded random values; the seed
is fixed in `gen.py`, so a rerun generates the same programs. Register
operands mix statically allocated guest registers (X0-X8, X19-X24, X29) and
context-backed ones (X9-X18, X25-X28).

Every program switches SP to a static stack in `.bss`, so SP and all data
addresses are identical on the Pi and under POWERarm and can be printed.

`sysreg` is not compared with the Pi: the presented ID_AA64ISAR0/ISAR1 values
differ from the Pi 5 by design (the presented profile leaves out atomics,
SHA-512 and the later extensions). On the Pi those checks print FAIL.

## Positive controls

Three, and `run.sh` fails if any does not fire:

- `sysreg` contains `control-deliberately-wrong`, a check whose expected
  MIDR_EL1 value is deliberately wrong. It must print FAIL, and it must be the
  only FAIL.
- `run.sh` corrupts one character of a copy of `addsub.golden` and requires
  the comparison to report a mismatch.
- `run.sh` flips one hex digit of V17 on a `vdump` line of a copy of
  `fp_scalar.golden` and requires the comparison to report a mismatch.

## Per-test build flags and stated goldens

A test source may carry a `// LDFLAGS: ...` line, which `golden.sh` adds to
the link. A test that the Pi's 16K kernel cannot run states its golden
instead: its `// EXPECT: ...` lines are the expected stdout and its
`// EXPECT-RC: n` line the exit status, and `golden.sh` writes those rather
than running it.
