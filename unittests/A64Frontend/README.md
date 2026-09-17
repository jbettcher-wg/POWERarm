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

`run.sh` sets `POWERARM_HOSTPAGEMODE=force` unless it is already set. Any
POWERarm configuration variable passes through, for example
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
| `simd_loadstore` | differential, generated | B/H/S/D/Q loads and stores in every addressing form, pairs, LD1/ST1 with 1-4 registers, literals; V registers and `buf` printed |
| `simd_copy` | differential, generated | DUP, INS, UMOV, SMOV, MOV d, MOVI/MVNI/ORR/BIC immediates (LSL, MSL, 64-bit masks), FMOV vector immediates |
| `simd_arith` | differential, generated | ADD/SUB, register and zero compares (vector and scalar), CMTST, bitwise with BSL/BIT/BIF, UMAX/UMIN/SMAX/SMIN, pairwise, ADDV/UMAXV/UMINV, CNT, NOT, NEG, ABS, REV32/REV64, signed/unsigned boundary vectors |
| `simd_shape` | differential, generated | XTN, SHRN, widening add/sub, ADDHN/SUBHN, SSHLL/USHLL/SXTL/UXTL, SSHR/USHR/SHL (vector and scalar, shift 0 and full width), EXT, UZP/ZIP/TRN |
| `fp_scalar` | differential, generated | FP arithmetic, FMIN/FMAX(NM), fused multiply-add group, FABS/FNEG/FSQRT, FCVT, FMOV forms, FCMP/FCMPE/FCCMP/FCSEL with every condition, under all four rounding modes, with NaN operand pairs |
| `fp_convert` | differential, generated | FCVT{N,P,M,Z,A}{S,U} to W/X, SCVTF/UCVTF from integers and fixed point, FCVTZS/FCVTZU to fixed point, scalar SIMD conversions; every edge value through the rounding variants |
| `fp_fpcr` | differential | FPCR round trip and the rounding mode reaching arithmetic and conversions |
| `fp_half` | differential, generated | half precision: arithmetic, FMIN/FMAX(NM), fused group, FABS/FNEG/FSQRT, compares, FCSEL, FMOV forms, FCVT to/from single and double, conversions to/from integers and fixed point, vector FCVTL/FCVTN (half/single and single/double); every rounding mode with and without FZ16; double-to-half rounding and overflow boundaries |
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
differ from the Pi 5 by design (the presented profile leaves out the crypto,
CRC32, atomics and later extensions). On the Pi those checks print FAIL.

## Positive controls

Three, and `run.sh` fails if any does not fire:

- `sysreg` contains `control-deliberately-wrong`, a check whose expected
  MIDR_EL1 value is deliberately wrong. It must print FAIL, and it must be the
  only FAIL.
- `run.sh` corrupts one character of a copy of `addsub.golden` and requires
  the comparison to report a mismatch.
- `run.sh` flips one hex digit of V17 on a `vdump` line of a copy of
  `fp_scalar.golden` and requires the comparison to report a mismatch.

## Known limitation in the tree

`common.S` puts one quad in `.data`. Without it the RW segment has
`p_filesz == 0` and, when its `p_vaddr` is 4K-aligned, the ELF loader on a 64K
host maps `.bss` starting at the next host page and leaves the first one
unmapped (`Source/Tools/FEXInterpreter/ELFCodeLoader.h`, the `AnonStart`
computation). Guest writes to the start of `.bss` then fault.
