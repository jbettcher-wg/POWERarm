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

This generates the differential programs (`gen.py`), builds everything with
`gcc -nostdlib -static` (and `hello.c` with `gcc -static`), runs each one
natively, and writes `OUTDIR/<test>.golden` (stdout) and `OUTDIR/<test>.rc`
(exit status).

On the ppc64le host, with the same OUTDIR visible (the Pi writes it over the
shared mount):

```sh
unittests/A64Frontend/run.sh "$PWD/build-frontend/Bin/POWERarm" OUTDIR
```

`run.sh` sets `POWERARM_HOSTPAGEMODE=force` unless it is already set. Any
POWERarm configuration variable passes through, for example
`POWERARM_MAXINST=1` (one instruction per block), `POWERARM_SMCCHECKS=full`
or `POWERARM_DISABLEDFCE=1`.

## What is checked

| Test | Kind | Covers |
|---|---|---|
| `addsub`, `adc`, `logic`, `movewide`, `bitfield`, `shiftvar`, `bits`, `csel`, `muldiv`, `branch`, `adr`, `loadstore`, `dczva` | differential, generated | one case per instruction: operands loaded, NZCV set, instruction run, every GPR, SP and NZCV printed (and `buf` after stores) |
| `flagsweep` | differential | ADDS/SUBS/CMP/CMN/ADCS/SBCS/ANDS/BICS/CCMP/CCMN and every condition consumer over all 8-bit operand pairs at the 32- and 64-bit sign boundary, hashed |
| `flags` | differential | NZCV across block exits, a syscall, BR, BL and a loop |
| `sigill_udf`, `sigill_msr`, `sigill_idreg`, `sigtrap_brk` | differential exit status | UDF, MSR to a read-only register and an unemulated ID register raise SIGILL (132); BRK raises SIGTRAP (133) |
| `sysreg` | self-checking | MRS/MSR against the presented CPU profile (`SystemRegisters.h`); prints PASS/FAIL per register |
| `hello` | differential | static glibc `printf` hello world |
| `loader_bss` | differential, self-checking | ELF loader: a host-aligned RW segment mapped straight from the file, followed by non-zero file bytes; everything past `p_filesz` reads as zero and is writable |
| `loader_bss_nofile` | differential, self-checking | ELF loader: an RW segment with `p_filesz == 0` and a 4K-aligned `p_vaddr` in a host page of its own |
| `loader_bss4k` | stated golden, self-checking | ELF loader: a 4K-aligned binary (`max-page-size=4096`) whose `p_filesz == 0` RW segment shares a host page with `.text`; 16K and 64K arm64 kernels cannot run it |

Generated inputs are an edge-case corpus plus seeded random values; the seed
is fixed in `gen.py`, so a rerun generates the same programs. Register
operands mix statically allocated guest registers (X0-X8, X19-X24, X29) and
context-backed ones (X9-X18, X25-X28).

Every program switches SP to a static stack in `.bss`, so SP and all data
addresses are identical on the Pi and under POWERarm and can be printed.

`sysreg` is not compared with the Pi: the presented ID_AA64PFR0/ISAR0/ISAR1
values and the FPCR writable mask differ from the Pi 5 by design (M1 HWCAP is
fp|asimd|cpuid). On the Pi those four checks print FAIL.

## Positive controls

Two, and `run.sh` fails if either does not fire:

- `sysreg` contains `control-deliberately-wrong`, a check whose expected
  MIDR_EL1 value is deliberately wrong. It must print FAIL, and it must be the
  only FAIL.
- `run.sh` corrupts one character of a copy of `addsub.golden` and requires
  the comparison to report a mismatch.

## Per-test build flags and stated goldens

A test source may carry a `// LDFLAGS: ...` line, which `golden.sh` adds to
the link. A test that the Pi's 16K kernel cannot run states its golden
instead: its `// EXPECT: ...` lines are the expected stdout and its
`// EXPECT-RC: n` line the exit status, and `golden.sh` writes those rather
than running it.
