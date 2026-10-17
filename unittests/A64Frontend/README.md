# A64Frontend tests

Small static AArch64 programs that check the A64 → FEX IR frontend
(`FEXCore/Source/Interface/Core/A64Frontend/`) against real hardware. They use
only raw `write` and `exit` system calls, so they need nothing from the Linux
layer beyond those two.

## How to run

On an AArch64 Linux machine with gcc (the reference was a Raspberry Pi 5):

```sh
unittests/A64Frontend/golden.sh OUTDIR
```

**Which compiler.** Everything here targets AArch64, so `$CC` must be an AArch64
compiler; `golden.sh` checks `$CC -dumpmachine` and refuses up front otherwise.
On the POWER9 development box this is the easiest thing to get wrong, because two
different compilers both answer to `gcc`:

| shell | `gcc` is | use it for |
|---|---|---|
| the host console | native **ppc64le** | the host-side probes under `docs/powerarm/research/neon/probes` (via `clang`) |
| running under POWERarm | the rootfs's **aarch64** | everything in this directory |

The host console's gcc does not merely produce the wrong binary, it cannot
compile these at all: it rejects AArch64 mnemonics such as `prfm`, rejects the
`"w"` register constraint, and supplies PowerPC signal headers where the tests
expect AArch64's. A cross compiler works too:

```sh
CC=aarch64-linux-gnu-gcc unittests/A64Frontend/golden.sh OUTDIR
```

**And note what `golden.sh` does.** It builds *and runs* each test to capture its
golden. On a machine whose only AArch64 execution is POWERarm itself that would
mint goldens from the emulator under test, which compare equal to a failing run.
Tests marked `// NO-GOLDEN` are built but never run for that reason; to add one
test's binary to an existing corpus, compile just that file rather than running
this script.

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
| `cmpbranch` | differential, generated (`gen.py`) | compare fusion: every condition after CMP/CMN/SUBS/ADDS at W and X size, immediate and register (shifted, extended, SP, ZR) operands on the signed/unsigned/carry/overflow edges, W operands with dirty upper halves; flags then read by CSEL/CSET/CSINC/ADC/MRS/B.cond in the next block, across an exit that leaves the unit, and by the dump, or killed before it |
| `cmpchain` | differential, generated (`gen.py`) | compare fusion over chains in one block where a compare reads the previous CSEL's result (clamps, min/max of three, `csel ; subs ; csel`): 2-4 compares (CMP/CMN/SUBS/ADDS, W and X, immediate, register and shifted) each consumed by a CSEL-family select or a final B.cond, flags killed at the end or left for the dump. the Firefox miscompile of 2026-09-18 fails 48 of its 600 cases |
| `ccmpchain` | self-checking, no golden | conditional-compare chains (§3.2): `cmp ; ccmp ; b.cond` collapsed into two compares, one CR logical op and the guest's own branch. Each case runs that shape and the same arithmetic with an MRS between the conditional compare and the branch -- which the chain walker refuses, so it is the packed-NZCV lowering `flagsweep` already pins -- and requires the two to agree: 896 pairs over all 14x14 conditions, both reachable gating results, CCMP and CCMN, the register and immediate forms, W and X. Eleven hand-computed anchors pin the structure absolutely (gate true and false, cond2(K) true and false, cond2 reading C, Z and both). Forcing cond2(K) to false fails 13 checks; forcing a CR bit's polarity fails 11 |
| `flaglive` | self-checking, no golden | the flag-liveness permissions (§3.5): ASR by immediate and by register at W and X including a count at and past the width and one with dirty high bits, SBFX over every lsb/width shape, TBZ/TBNZ on bits 0/3/4/15/16/31/32/35/63 in both polarities and both outcomes -- each with every flag provably dead, so `sradi`/`srad`/`srawi`/`sraw` and `rldicl.` are allowed; then the same ops with C read by an ADC or a B.CS and with all four read by an MRS NZCV or a B.PL, where the permission must be withheld and the old sequence must run. The refusal cases are chosen so the forbidden instruction gives a different answer rather than the same one by luck; granting every permission unconditionally fails 14 of them, plus 9 of the differential tests here |
| `sigpreempt` | differential | signal handlers that move the guest PC: Go-style asynchronous preemption (redirect to a trampoline that returns to the interrupted PC) over fused compare-and-branch loops, and a SIGSEGV handler that skips a load between a compare and its B.cond |
| `sigedit` | differential | signal handlers that edit the interrupted context and return: every X register, SP, NZCV, V0-V31, FPSR and FPCR (checked by the rounding of a division), at the same PC and past it, for SIGSEGV (a guard-page load, retried after the handler fixes its base, once between a compare and the CSEL that reads the handler's flags), SIGILL (UDF, an unallocated encoding), SIGTRAP (BRK), a SIGUSR1 raised by a raw tgkill (the edited X0 replaces the syscall's result) and a timer SIGUSR1 in a loop; also checks each frame the handler sees, fpsimd_context record included |
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
| `memrev` | self-checking, no golden | the load+REV and REV+store fusion (POWER-STRENGTHS.md §2.2): every width (16/32/64) at every alignment 0-15, the unsigned-offset, unscaled, pre- and post-indexed and 64-bit register-index forms, `Rt == Rn`, and the forms the fusion refuses and must still answer correctly (a scaled or UXTW index, LDRSW, a 64-bit REV over a word load, a REV writing another register, a loaded value read again). The store direction checks the reversed register as well as the bytes, because the fusion keeps the REV, and `rev x0,x1 ; str x0,[x0]` pins the order. A doubleword flush against an unmapped page must load, one straddling it must raise SIGSEGV with the faulting PC at the load and Rt untouched. `run.sh` runs it fused and under `POWERARM_MAXINST=1`, which cannot fuse, and requires the two to agree |
| `prfmhint` | self-checking, no golden | PRFM reaching `dcbt`/`dcbtst` (§2.4): every hint kind including the unallocated prfops, the literal, unsigned-offset, unscaled and register forms, against a writable page, a read-only page, a PROT_NONE page, an unmapped hole, the last byte of a mapping, its own (write-protected) code page and addresses no mapping covers. A touch cannot fault, so the forced answer is no signal, no byte moved and the instructions after it still executed. `run.sh` runs it with `POWERARM_GUESTPREFETCH` on and off and requires identical output; the control proves the PROT_NONE page really is inaccessible |
| `loadstore_nopair` | differential, generated | STNP/LDNP (pair index bits 00, no writeback) for W/X and S/D/Q registers, static and context-backed base, as .inst words |
| `simd_shll` | differential, generated | SHLL/SHLL2 at every element width over lane boundary values |
| `simd_rbit` | differential, generated | RBIT (vector), 8B and 16B, over lane edge patterns and random data. It also covers `VectorImm` byte splats of 0x10-0x7F (the 0x55/0x33 masks), which the backend used to truncate |
| `simd_facross` | differential, generated | FMAXV/FMINV/FMAXNMV/FMINNMV on 4S: quiet and signalling NaN payloads in every lane position (which NaN propagates follows the reduction order `op(op(e0,e1), op(e2,e3))`), signed zeros, infinities and denormals, in every rounding mode. FPCR.DN and FZ stay clear, since no FP op honours them yet |
| `simd_recip` | differential, generated | FRECPE/FRSQRTE (vector 2S/4S/2D and scalar S/D) at every entry of the architecture's estimate tables, URECPE/URSQRTE at every table entry, FRECPX, and FRECPS/FRSQRTS: zeros, infinities, quiet and signalling NaNs, denormals on both sides of the FRECPE overflow bound, exponents with denormal results, every rounding mode (the overflow to infinity or the largest finite value follows it); NaN pairs, inf*0, products that cancel against 2 or 3 and products that overflow for the steps |
| `simd_dotmul` | differential, generated | SDOT/UDOT, vector and by element, 2S and 4S, over byte lane boundaries (-128, 127, 255) with wrapping accumulators; FMULX vector, scalar and by element on single and double lanes: inf*0 of every sign pairing (2.0 with the product's sign), NaN pairs and edge values, every rounding mode; PMUL 8B/16B |
| `simd_shiftsat` | differential, generated | SQSHL/UQSHL/SRSHL/URSHL/SQRSHL/UQRSHL by register, vector (every arrangement) and scalar (B/H/S/D; D for SRSHL/URSHL), with per-lane counts around the lane width in both directions and at -128/127 in the low byte, random bits above it; SUQADD/USQADD vector and scalar; SQDMULL/SQDMLAL/SQDMLSL vector, scalar and by element, including the "2" forms, with MIN*MIN and saturating accumulations; operands from the lane boundaries; FPSR.QC cleared before each case, so every saturation and every non-saturation is checked |
| `simd_fcvtxn` | differential, generated | FCVTXN/FCVTXN2 (vector) and FCVTXN (scalar), double to single rounded to odd, in every FPCR rounding mode (which must not matter): exact, halfway and near-halfway values, the overflow boundary (the largest finite value, never infinity), the single denormal range and doubles far below it, infinities, NaNs, both signs |
| `simd_half` | differential, generated | the FP16 Advanced SIMD group (asimdhp) on 4H/8H vectors and H scalars: FADD/FSUB/FMUL/FDIV, FMIN/FMAX(NM), FABD, FMULX, FRECPS/FRSQRTS, FMLA/FMLS (vector and by element), FMUL/FMULX by element, FADDP/FMAXP/FMINP(NM) vector and scalar, FMAXV/FMINV/FMAXNMV/FMINNMV, FCMEQ/FCMGE/FCMGT/FACGE/FACGT and the zero compares, FRINT*, FSQRT, FRECPE/FRSQRTE (every table entry) and FRECPX, FCVT{N,P,M,Z,A}{S,U} to 16-bit integers, SCVTF/UCVTF, the fixed-point conversions, FMOV immediates; half-precision NaN pairs lined up lane by lane, every edge value of both signs through the estimates and unary operations, every rounding mode with and without FZ16 |
| `simd_scalarshift` | differential, generated | scalar SLI/SRI/SSRA/USRA (D) and SQRSHRN/UQRSHRN/SQRSHRUN (B/H/S from H/S/D) at the ends of the shift range and at random shifts, over lane boundary values, FPSR.QC cleared first |
| `exclusive` | differential, generated | LDXR/LDAXR then STXR/STLXR at every width: success, store without a load, a second store, CLREX in between, NZCV across a successful store; LDAR/STLR. A store to a different address than the load is IMPLEMENTATION DEFINED (the Pi lets it succeed within a region) and is not tested |
| `exclusive_pair` | differential | LDXP/LDAXP then STXP/STLXP in 32-bit (W) and 64-bit (X) variants: reservation and atomic CASPair/CAS update, second store failure, store without load, memory modified concurrently, CLREX, size mismatch, arbitrary/non-consecutive registers, XZR/WZR, and NZCV preservation across success and fail paths |
| `hello`, `printf_float`, `strmem`, `fpmath` (and `musl_*`) | differential | static glibc (and musl) programs: printf float formatting, the string/memory routines over lengths and alignments, scalar FP code |
| `vdso` | self-checking | the guest vDSO (needs the guest thunk build, below): `AT_SYSINFO_EHDR`, the ELF and its four `__kernel_*` symbols under `LINUX_2.6.39`, a signal handler returning into `__kernel_rt_sigreturn`, every vDSO-served `CLOCK_*` id, `clock_getres`, `gettimeofday` and `time`, monotonic and agreeing with the raw syscalls |
| `vdso_syscalls` | differential | how many `clock_gettime`/`gettimeofday` reads are syscalls, counted with a seccomp filter (`vdso_syscalls.env` turns on POWERarm's seccomp emulation): 0 of 1000 through the vDSO, 1000 of 1000 raw `syscall()` reads as the positive control |
| `thunk_callback` | differential | host->guest callbacks through POWERarm's built-in `fex:callback_selftest` thunk: counts and sums, the caller's x19-x28/d8-d15 and SP across callbacks that overwrite them, three-deep nesting, signals raised and handled inside callbacks, interval-timer signals landing anywhere in the crossing, the registers timer signals and a SIGSEGV see in their ucontext inside a callback, SP alignment in callbacks, four threads at once, the caller's frame. On the Pi the thunk's HLT faults and the test makes the same calls directly; `thunk_callback.env` makes the POWERarm run fail unless the host path was taken |
| `dcmaint` | differential | DC CVAC, DC CIVAC and DC CVAU (cache maintenance Linux lets EL0 run) over a live buffer: no fault, data unchanged. Firefox runs DC CIVAC at startup |
| `procdirfd` | differential, self-checking | `/proc`, `/sys` and `/dev` opened as directories reach the kernel's filesystems, not the rootfs's empty mount points, and `*at()` calls relative to those descriptors work (Chromium's single-thread check, `fstatat(proc_fd, "self/task/")`, killed its zygote and GPU process) |
| `hoststack` | differential, self-checking | the emulator's host stack never reaches the guest's: 400 nested `SA_NODEFER` handlers, each raised from inside the last, keep that many interrupted syscalls' host frames and saved host contexts live on the main thread's host stack; the top of the guest stack (from main's frame to the end of the last argument, environment and `AT_EXECFN` string) must be unchanged afterwards. `run.sh` runs it with ASLR off (`setarch -R`) and an unlimited stack rlimit, the layout in which the ELF loader used to map the guest stack flush against the host stack every time (it failed with "stack smashing detected") |
| `forkexec` | differential | fork from a process with four running threads, run code only the child runs, and execve again, twelve times plainly and twelve times with `RLIMIT_AS` lowered to 4 GiB first (as glycin does before it execs bwrap); `getrlimit` reads the new limit back and the exec'd image reports the one it inherited. `run.sh` runs it with `POWERARM_PORTABLE=1` and a fresh code cache, so the children compile, save and compact every run. Under the limit the emulator used to be unable to map anything, and its pre-exec code-cache save crashed |
| `hostfault` | differential, plus host-side checks | natively: install `SA_NODEFER` fault handlers like a crash reporter's, then call `getppid`. `run.sh` also runs it (`hostfault_report`) with `POWERARM_HOSTFAULT_INJECT=173,segv,unwind`, which faults in POWERarm's own syscall body under a return address the unwinder cannot read, with and without the handlers: the fatal host-fault report must print its line first and once, survive its backtrace's own fault, not run the guest's handler, and end with the original SIGSEGV. It used to re-enter itself through the unwinder's fault dozens of levels deep, or die in the unwinder |
| `hlt`, `sigill_hlt` | self-checking; differential exit status | HLT raises SIGILL for every immediate, at its own address; the thunk marker `HLT #0x0F3F` does too when its hash names no thunk, or runs off executable memory |
| `bb_*` | differential | busybox `echo`, `cat`, `wc`, `sort`, `sort -n`, `sha256sum`, `md5sum` |
| `rootfs_overlay` | differential, self-checking, plus host-side checks | the per-user rootfs overlay (DESIGN §6.2a.1): create, modify, chmod, utimens, rename, delete, mkdir, rmdir, symlink, hard link and readdir (with rewinddir/seekdir) under `/usr` and `/etc`, relative and descriptor-relative paths, a path through the base's `lib -> usr/lib` symlink, `/tmp` staying the host's, and no fallthrough to the host's `/var/lib/pacman`. The Pi runs the same operations on a scratch copy of the fixture (`OVT_ROOT`). `run.sh` runs it with the fixture as the base rootfs and an empty overlay, then requires the base's snapshot (names, modes, sizes, mtimes, link targets, file hashes) to be unchanged, a whiteout and a copied-up file in the overlay, host `/usr/bin/env` still reachable, `getcwd` inside the base reading as the guest path with the overlay on and off (and a host directory reading as itself), and with the overlay disabled (`POWERARM_ROOTFSOVERLAY=0`) or absent the old fallthrough to the host's `/var/lib/pacman` |

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

## The guest vDSO

`vdso` and `vdso_syscalls` expect the guest vDSO, `libVDSO-a64-guest.so`,
which a `-DBUILD_THUNKS=ON` build puts in `<build>/Guest`. POWERarm looks for
it in `ThunkGuestLibs`, so run the suite with it pointed there:

```sh
POWERARM_THUNKGUESTLIBS=$PWD/build/Guest unittests/A64Frontend/run.sh "$PWD/build/Bin/POWERarm" OUTDIR
```

A POWERarm older than the thunk marker never loads a file of that name, so
the variable is harmless to the binfmt-launched emulator that runs the shell
itself when this shell is emulated.

A test that needs environment of its own under POWERarm (POWERarm
configuration, or a variable the guest reads) has a `<test>.env` file of
`NAME=value` words, which `run.sh` sets for that test only.

## Per-test build flags and stated goldens

A test source may carry a `// LDFLAGS: ...` line, which `golden.sh` adds to
the link. A test that the Pi's 16K kernel cannot run states its golden
instead: its `// EXPECT: ...` lines are the expected stdout and its
`// EXPECT-RC: n` line the exit status, and `golden.sh` writes those rather
than running it.
