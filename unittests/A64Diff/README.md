# A64Diff: differential tests against real ARM hardware

A64Diff checks that AArch64 programs behave the same under POWERarm on POWER9 as they do on a
real ARM CPU (the Raspberry Pi 5, a Cortex-A76). It has two levels:

- **Instruction level.** Thousands of small static programs, each running one instruction or a
  short branch sequence from a seeded state, then dumping the full machine state.
- **Program level.** Static `hello` built against glibc and musl, static busybox, and the M1
  applet script run on a fixed input corpus.

Both levels run natively on the Pi to capture goldens, then under POWERarm on the POWER9: on
the bare 64K-page host and in a KVM guest running the host's 4K kernel. Every compare injects
corrupted goldens that must be caught, or the run fails.

```
gen/a64gen.py              instruction test generator (Python 3, no dependencies)
tool/a64diff.h             record format
tool/a64diff.c             runner and comparator (C99 + POSIX, builds static)
tool/broken-runner.sh      deliberately wrong "emulator" for negative controls
programs/                  hello.c, build-programs.sh, programs.jobs, applets.sh, corpus/
vm/a64diff-init.c          /init for the 4K KVM guest
../../Scripts/powerarm/a64diff-golden.sh    golden side (Pi)
../../Scripts/powerarm/a64diff-selftest.sh  negative controls (Pi)
../../Scripts/powerarm/a64diff-run.sh       compare side (POWER9): 64k | 4k-kvm
```

Generated tests, binaries and goldens are never committed. A bundle is a pure function of
`--seed`, `--scale` and the sources, and its name carries a hash of those sources.

## Running it

**On the Pi (golden side):**

```sh
Scripts/powerarm/a64diff-golden.sh --selftest --push jbettcher@192.168.2.24:a64diff/bundles
```

This generates and builds the tests, runs them natively, checks every golden, builds the
program corpus and captures its goldens, and re-runs everything natively. The re-run must
match the goldens exactly, so a test that isn't deterministic fails here. `--selftest` then
runs the negative controls (see below). Finally it packs the bundle as
`$A64DIFF_OUT/a64diff-v<format>-s<seed>-x<scale>-<hash>.tar.gz` (default `~/a64diff`) and
copies it to the POWER9. Options: `--seed N` (default 1), `--scale N` (repetitions per field
combination, default 2), `-j N`, `--out DIR`.

**On the POWER9 (compare side):**

```sh
Scripts/powerarm/a64diff-run.sh 64k    build-powerarm/Bin/POWERarm
Scripts/powerarm/a64diff-run.sh 4k-kvm build-powerarm/Bin/POWERarm
```

By default it uses the newest bundle in `~/a64diff/bundles`; `--bundle PATH` picks another.
Useful options:

| Option | Meaning |
|---|---|
| `--time-limit SEC` | Wall-clock budget for the whole run (default 1800) |
| `--timeout SEC` | Per-test limit (default 30) |
| `-j N` | Parallel tests |
| `--skip CLASSES` | Classes to leave out |
| `--suites insn,programs` | Which suites to run |
| `--env K=V` | Set a variable for the emulator; repeatable |
| `--kernel`, `--cpus`, `--mem` | 4k-kvm guest settings |

Results go to `~/.cache/a64diff/results/<bundle>-<mode>-<time>/`: `insn.log`/`programs.log`
(the summaries), `insn.report`/`programs.report` (every failure in detail) and, for 4k-kvm,
`console.txt` and `qemu-cmdline.txt`.

**Exit codes** (the tool and both scripts):

| Code | Meaning |
|---|---|
| 0 | Every required test passed and every control fired |
| 1 | A required test failed |
| 2 | Harness error: a control didn't fire, a golden is invalid, or the wrong kernel was booted |
| 124 | Time limit reached |

Only the 64k mode reads the emulator environment from the caller, and only `HOME`,
`POWERARM_*` and `FEX_*` reach test processes. Children get `PATH=/usr/local/bin:/usr/bin:/bin`,
`LC_ALL=C`, `TZ=UTC` and umask 022. The M1 plan asks for runs with `POWERARM_HOSTPAGEMODE`
unset. The M0 binary refuses to start on a 64K host unless it's set, and the harness reports
that as `refused`, not as a harness failure.

### The 4K KVM guest

This uses the method from `docs/powerarm/research/va-size/`. `a64diff-run.sh 4k-kvm` builds an
initramfs with these contents:

- `/init`: `vm/a64diff-init.c`, static
- a static `a64diff`
- `POWERarm` and `POWERarmServer`, plus every library `ldd` reports, and the ELF interpreter
- the bundle (tests, goldens, program corpus)

It then boots the image with this command:

```
qemu-system-ppc64 -M pseries,accel=kvm -cpu host -smp 8 -m 4G -nographic -nodefaults \
  -serial stdio -no-reboot -kernel /boot/vmlinuz-linux-power9 -initrd initramfs.cpio \
  -append "console=hvc0 quiet A64DIFF_JOBS=8 A64DIFF_TIMEOUT=30 A64DIFF_DEADLINE=<s> A64DIFF_SUITES=insn,programs"
```

Settings reach the guest as kernel command-line environment variables. `--env` values are
appended the same way.

1. `/init` mounts `/proc`, `/dev`, `/dev/shm` and `/tmp`.
2. It checks that the page size is 4096. If not, it prints `A64DIFF-GUEST-ERROR` and the run
   exits 2.
3. It runs both suites as uid 65534 and prints the logs and reports between
   `A64DIFF-BEGIN/END` markers.
4. It prints `A64DIFF-GUEST-EXIT insn=N programs=M` and powers off.

The host extracts the sections from the console log. The guest stops starting tests at its
deadline, and `timeout(1)` kills QEMU if the guest wedges; either way the run exits 124.

## Test program anatomy

Each instruction test is a no-libc static `ET_EXEC` linked at `0x400000`:

```
_start:     [signals class: sigaltstack + rt_sigaction for SIGILL/TRAP/BUS/FPE/SEGV]
            adrp/add x30, init_state
            ldr x0,[x30,#248]; mov sp,x0           // SP
            ldr x0,[x30,#256]; msr nzcv,x0         // NZCV
            ldp x0,x1,[x30,#0] ... ldp x28,x29,[x30,#224]; ldr x30,[x30,#240]
test_insn:  <instruction or sequence under test>
test_end:
pad_0:      msr tpidr_el0,x0; adrp/add x0,rec; str x1,[x0,#24]; mov x1,#0; b dump_common
[filler, pad_1 (forward target); pad_2 (backward target) sits before _start]
dump_common: store pad id, X2-X30, X0 (from TPIDR_EL0), SP, NZCV into rec
            write(1, rec, 4944); exit(0)           // svc #0 with x8 = 64, then 93
```

The data page lives inside the record, so the `write` dumps it too. TPIDR_EL0 is the only
scratch location that doesn't disturb a register, so the prologue and dump stub depend on these
instructions: `adrp`, `add`/`mov sp` (add immediate), `ldr`/`str` (unsigned offset, X), `ldp`/`stp`
(signed offset, X), `movz`, `msr`/`mrs` for `nzcv` and `tpidr_el0`, `b`, and `svc`. The
`identity` class runs nothing in between, so it isolates exactly that set. When the emulator
reports `... at pc 0x<addr>`, the report says whether it stopped in the prologue, at the
instruction under test, or in the dump stub.

## Record format (`tool/a64diff.h`, format 1)

The record is 4944 bytes, little-endian.

| Offset | Field | Notes |
|---|---|---|
| 0 | `magic` `"A64D"`, `version` u16, `kind` u16, `flags` u32, `size` u32 | kind 1 = state, 2 = signal |
| 16 | `x[31]` | X0–X30 after the test. For signal records, from the ucontext |
| 264 | `sp` | |
| 272 | `pcmark` | State records: landing pad (0 fall-through, 1 forward, 2 backward). Signal records: ucontext PC |
| 280 | `nzcv` | Bits 31:28 |
| 288 | reserved | |
| 296 | `signo`, `sigcode` (sign-extended), `sigaddr` | Signal records only |
| 320 | `v[32×16]`, then `fpcr` at 832 and `fpsr` at 840 | Compared only when `flags & 1` (reserved for the SIMD classes) |
| 848 | `page[4096]` | The data page after the test |

`manifest.tsv` has one row per test with these fields: id, class, sub-kind, required, expected
kind, `test_insn` address, encodings, objdump disassembly, and the resolved initial state (X0–X30,
SP, NZCV).

Program jobs (`programs/programs.jobs`) are tab-separated: `id class required stdin argv...`.
`@ROOT@` expands to the bundle root, and stdin `-` means `/dev/null`. Each job runs in a fresh,
empty working directory. stdout, stderr and the exit status are all compared byte for byte.

## Classes

These are the counts at `--seed 1 --scale 2`:

| Class | Tests | What it covers |
|---|---|---|
| identity | 16 | Prologue and dump only (the bootstrap set above) |
| dp-imm | 376 | Add/sub immediate (sf, op, S, shift, Rn/Rd ∈ {31, GPR, same}); logical immediate over valid bitmasks, including non-canonical `immr`; MOVN/MOVZ/MOVK with every `hw`; ADR/ADRP |
| bitfield | 128 | SBFM/BFM/UBFM (`immr` ≥, <, 0, max `imms`), EXTR |
| dp-reg | 514 | Logical and add/sub shifted (all shift types and amounts); add/sub extended (all 8 options, `imm3` 0–4, SP forms); ADC/SBC(S); CLZ/CLS/RBIT/REV*; LSLV/LSRV/ASRV/RORV with out-of-range amounts |
| condsel | 240 | CSEL/CSINC/CSINV/CSNEG × 16 conditions × condition true/false × W/X |
| condcmp | 240 | CCMP/CCMN, register and immediate × 16 conditions × true/false |
| muldiv | 152 | MADD/MSUB, UDIV/SDIV (including /0 and INT_MIN/−1), [SU]MADDL/[SU]MSUBL, [SU]MULH |
| loadstore | 1376 | Every GPR size and sign-extend × unsigned offset/unscaled/pre/post/register (UXTW/LSL/SXTW/SXTX, shifted or not)/literal; SP and XZR forms |
| pairs | 180 | STP/LDP/LDPSW, W/X, post/offset/pre, SP base, `imm7` edges |
| branch | 369 | B.cond, exhaustive over 16 conditions × 16 NZCV; near/mid/far and forward/backward; B, BL, CBZ/CBNZ, TBZ/TBNZ, BR/BLR/RET, BL+RET |
| sysreg | 58 | MRS of CTR_EL0, DCZID_EL0, TPIDRRO_EL0, FPCR, FPSR, NZCV; MSR+MRS of NZCV, TPIDR_EL0, FPCR, FPSR; DC ZVA |
| hint | 40 | NOP, YIELD, CSDB, BTI*, PAC*/AUT* (NOPs without PAuth), XPACLRI, unallocated hints, PRFM |
| sysreg-id | 13 | *Optional.* MIDR/MPIDR/REVIDR/ID_AA64* as the kernel shows them to EL0. POWERarm presents its own profile, so differences are informational |
| tbi | 12 | *Optional.* Loads and stores through top-byte-tagged pointers |
| signals | 17 | *Optional.* UDF, BRK, HLT, unmapped load, store to text, misaligned SP, misaligned and null BR. The handler records signo, si_code, si_addr and the registers |

A test in an optional class is still run and reported, but its failure doesn't change the exit
code. Use `--require CLASS` or `--optional CLASS` on `a64diff compare` to override this, and
`--skip` to leave a class out.

Operands mix an edge corpus with random values. The corpus includes 0, 1, −1, INT32 and INT64
min/max, sign-bit boundaries, shift amounts 0/1/31/32/63/64 and beyond, and W values with junk in
the upper half. Registers not under test hold random canaries, so any stray write shows up. The
generator never emits UNPREDICTABLE encodings: no writeback with Rn == Rt, and no LDP with
Rt == Rt2. `finalize` rejects any non-signals test that objdump shows as undefined, and the golden
check rejects any test that didn't produce a record of the expected kind on the Pi.

## Controls

**Positive controls (every compare).** For each class, two golden records are corrupted in
memory: one flips a bit in a register, and the other changes NZCV, SP, the pad id, a page byte
or a signal field. Both of the following must hold for every one of them, or the run exits 2
with `CONTROL-FAIL`.

1. When the corrupted class is compared with the pristine goldens, exactly those records are
   flagged, and in each one exactly the corrupted field.
2. When a corrupted golden is compared with the actual result, it's flagged on that field.

The corrupting value is chosen to differ from the actual value as well, so a real bug that
happens to produce the corrupted value can't make a control look blind. Program jobs get the
same two controls per class, one on a stdout byte and one on the exit status.

**Negative controls (`a64diff-selftest.sh`, Pi).** Each case has a required verdict.

| Case | Required verdict |
|---|---|
| Neutral wrapper | Pass (exit 0) |
| `broken-runner.sh` replacing the instruction with NOP | Fail (exit 1) |
| `broken-runner.sh` flipping bit 0 of the instruction | Fail (exit 1) |
| Comparator blind to `nzcv` (`--blind-field`) | Exit 2 through `CONTROL-FAIL`. Every test "passes" |
| Comparator blind to `x7` | Exit 2 through `CONTROL-FAIL`. Every test "passes" |
| Program output truncated to 64 bytes | Fail (exit 1) |

A wrong verdict fails the selftest.

**Environment controls.** The 64k mode refuses to run unless the host page size is 65536, and
the guest refuses unless its page size is 4096. Booting the 64K image in `4k-kvm` mode exits 2.

## Adding an instruction class

1. Write `gen_<name>(rng, scale)` in `gen/a64gen.py`. It returns `Test` objects that set
   `t.x[r]`, `t.sp`, `t.nzcv` and optionally `t.page`, and append either `t.inst(word)` (built
   from the encoding fields) or an assembler line to `t.seq`. Register values can be expressions:
   `("page", off)` means the data page plus `off`, and `("sym", "pad_1", 0)` means a label.
   Memory operands must keep every accessed byte inside the page, and an SP base must be
   16-aligned; `place()` and `reg_offset()` handle both.
2. Enumerate the structural fields: sf, op, S, option, size, index mode, and the register-31 and
   Rd == Rn classes. Draw the value fields from `rng.val()`, `rng.imm_edge()` and `rng.simm()`.
   Skip UNPREDICTABLE combinations.
3. Add the class to `CLASSES`. Add it to `OPTIONAL_CLASSES` if the emulator isn't expected to
   match yet. Each class has its own RNG stream, so the other classes don't change.
4. Run `a64diff-golden.sh --selftest`. The new class gets controls automatically.

To add a program job, append a line to `programs/programs.jobs`, keeping the output free of
absolute paths, times, owners and readdir order. The job's class gets controls automatically.

## Notes for the emulator workstreams

- **Stop messages.** `unimplemented A64 instruction 0x<enc> at pc 0x<addr>` on stderr is
  classified as `unimplemented`, and the report places the PC. A message containing
  `HOSTPAGEMODE` without `continuing` is classified as `refused`. Keep these strings or update
  `classify_missing()`.
- **Exit convention.** A test passes only if it produces one full record and exits with status
  0 through `exit` (93). `write` must return the full 4944 bytes to a regular file.
- **Signals class.** This needs `sigaltstack`, `rt_sigaction` with `SA_SIGINFO|SA_ONSTACK`, and
  a kernel-shaped siginfo and ucontext (`uc_mcontext` at offset 176; `regs`, `sp`, `pc`,
  `pstate`).
- **busybox.** Debian's `busybox-static` is built with standalone-shell applets, so `sh -c`
  pipelines re-execute `/proc/self/exe`. Under POWERarm that must resolve to the guest binary
  and go through the emulator (W3's `execve` and `/proc` views).
- **Program provenance.** `programs/SOURCES.txt` in each bundle records exact package URLs,
  sha256 sums, and the toolchain and glibc versions.
