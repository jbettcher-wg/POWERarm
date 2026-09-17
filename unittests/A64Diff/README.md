# A64Diff: differential tests against real ARM hardware

A64Diff checks that AArch64 programs behave the same under POWERarm on POWER9 as they do on a
real ARM CPU (the Raspberry Pi 5, a Cortex-A76). It has three levels:

- **Instruction level.** Thousands of small static programs, each running one instruction or a
  short branch sequence from a seeded state, then dumping the full machine state.
- **Program level.** Static `hello` built against glibc and musl, static busybox, and the M1
  applet script run on a fixed input corpus.
- **Rootfs level.** Dynamically linked programs run inside an AArch64 root filesystem, as
  command sequences whose output files are byte-compared (see [Rootfs jobs](#rootfs-jobs)).

Both levels run natively on the Pi to capture goldens, then under POWERarm on the POWER9: on
the bare 64K-page host and in a KVM guest running the host's 4K kernel. Every compare injects
corrupted goldens that must be caught, or the run fails.

```
gen/a64gen.py              instruction test generator (Python 3, no dependencies)
tool/a64diff.h             record format
tool/a64diff.c             runner and comparator (C99 + POSIX, builds static)
tool/broken-runner.sh      deliberately wrong "emulator" for negative controls
programs/                  hello.c, build-programs.sh, programs.jobs, rootfs.jobs, applets.sh, corpus/
rootfs/a64diff-rootfs.py   rootfs content hash (same definition as the sysroot builder)
rootfs/mkrootfs-minimal.sh the "minimal-debian" prototype rootfs
../../Scripts/powerarm/rootfs/run-in-sysroot.sh  native rootfs runner (from the sysroot work)
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
| `--suites LIST` | Which suites to run: `insn` and job suites named after `programs/<suite>.jobs` (default all) |
| `--rootfs NAME=DIR` | Where an external rootfs lives (default `~/.local/share/powerarm/RootFS/NAME`) |
| `--rootfs-transport ext4\|squashfs` | How a rootfs reaches the 4K guest (default ext4) |
| `--env K=V` | Set a variable for the emulator; repeatable |
| `--kernel`, `--cpus`, `--mem` | 4k-kvm guest settings |

Results go to `~/.cache/a64diff/results/<bundle>-<mode>-<time>/`: `<suite>.log` (the summary)
and `<suite>.report` (every failure in detail) per suite and, for 4k-kvm, `console.txt` and
`qemu-cmdline.txt`.

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
- the bundle (tests, goldens, program corpus), without the instruction tests unless `insn` runs
- for rootfs jobs: the booted kernel's `virtio_ring`, `virtio`, `virtio_pci*` and `virtio_blk`
  modules (and `squashfs` for that transport), taken from the `/lib/modules` directory whose
  `vmlinuz` matches the kernel image

It then boots the image with this command:

```
qemu-system-ppc64 -M pseries,accel=kvm -cpu host -smp 8 -m 4G -nographic -nodefaults \
  -serial stdio -no-reboot -kernel /boot/vmlinuz-linux-power9 -initrd initramfs.cpio \
  -append "console=hvc0 quiet A64DIFF_JOBS=8 A64DIFF_TIMEOUT=30 A64DIFF_DEADLINE=<s> A64DIFF_SUITES=insn,programs"
```

Settings reach the guest as kernel command-line environment variables. `--env` values are
appended the same way.

1. `/init` mounts `/proc`, `/dev`, `/dev/pts`, `/dev/shm` and `/tmp`.
2. It checks that the page size is 4096. If not, it prints `A64DIFF-GUEST-ERROR` and the run
   exits 2.
3. If the command line has `A64DIFF_ROOTFS=name:fstype,...`, it loads the modules and mounts
   `/dev/vda`, `/dev/vdb`, ... read-only at `/a64diff/rootfs/<name>`.
4. It runs each suite as uid 65534 and prints the logs and reports between
   `A64DIFF-BEGIN/END` markers.
5. It prints `A64DIFF-GUEST-EXIT <suite>=N ...` and powers off.

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

## Jobs format

Each `programs/<suite>.jobs` file is one job suite, with goldens in `golden-<suite>/`. Fields are
TAB-separated, `#` starts a comment, `@ROOT@` expands to the bundle root and stdin `-` means
`/dev/null`. Every job runs in a fresh, empty working directory. There are two forms.

**One command per line** (the M1 form): `id class required stdin argv...`. stdout, stderr and
the exit status are compared byte for byte.

**A block** of directives, one per line (leading blanks are ignored):

```
job     ID  CLASS  REQUIRED
        rootfs  NAME             run every step inside this rootfs
        input   PATH             copy a file, or a directory's contents, into the working directory
        env     KEY=VALUE        added to every step's environment (repeatable)
        timeout SEC              for the whole block
        run     STDIN argv...    a step; the block stops at the first non-zero status
        try     STDIN argv...    a step whose non-zero status doesn't stop the block
        output  PATH             a declared output file, or PATH/ for every file below a directory
end
```

A step's stdin, and any relative path, resolves in the working directory. An `input` directory
is copied as its contents, and an `input` file keeps its name. With `--workroot DIR`, a block
job works in `DIR/<id>`, which is emptied first; otherwise it uses `<out>/<id>.cwd`. For
example, a build job for M2:

```
job     zlib.build  zlib  1
        rootfs  alarm-m2
        input   @ROOT@/sources/zlib-1.3.1
        env     SOURCE_DATE_EPOCH=0
        run     -  /usr/bin/sh  ./configure  --static
        run     -  /usr/bin/make  -j1  libz.a  example  minigzip
        run     -  ./example
        output  libz.a
        output  example
end
```

A block job is compared on these parts:

- the step list with each step's status (`<id>.steps`)
- every step's stdout and stderr (`<id>.<k>.out`/`.err`)
- the final status
- the declared outputs

Each output is recorded as `sha256 size path` in `<id>.outputs`, with a copy kept in
`<id>.files/`, so a mismatch report gives the first differing byte. A missing output is recorded
as `missing`, and a symlink as `link TARGET`.

The scripts run every job suite with `--workroot /tmp/a64diff-work/<bundle>/<suite>`: on the Pi,
on the 64K host and in the 4K guest. A job's working directory therefore has the same absolute
path everywhere, as the sysroot runner and byte-identical builds require. Still use
`-ffile-prefix-map=$PWD=.` with `-g`. Two runs of the same bundle on one machine at the same
time would share that directory; don't do that.

## Rootfs jobs

A block with `rootfs NAME` runs dynamically linked programs inside an AArch64 root filesystem.
Guest absolute paths (`/usr/bin/gcc`, `/lib/ld-linux-aarch64.so.1`) resolve inside the rootfs,
and relative paths resolve in the working directory.

**Rootfs names.** The golden script builds `minimal-debian` into the bundle. Every other name a
jobs file uses is external. The golden script finds it in `~/.local/share/powerarm/RootFS/NAME`
or through `--rootfs NAME=DIR`, and records only its hash. `ArchLinuxARM-m2` (the pinned Arch
Linux ARM sysroot from `Scripts/powerarm/rootfs`) is found this way, with content hash
`sha256:0f4a923096d21697dab39bc13172118a5db96b6ce3c2a6e998a270001bf577d5`. It must be present
at the same path on the POWER9.

**Identity.** A bundle records each rootfs in `rootfs/<name>.id` (name, entry count,
`content-hash sha256:…` and location) and in `rootfs/<name>.contents`. The content hash is the
same definition the Arch Linux ARM sysroot builder uses: sorted paths with the file sha256,
mode and symlink target, and no owners or times. `rootfs/a64diff-rootfs.py hash|verify`
computes it. Both sides verify the tree against the bundle before running anything. A
`bundle` rootfs travels inside the bundle. An `external` one (`a64diff-golden.sh --rootfs
NAME=DIR`, e.g. the Arch Linux ARM sysroot) is found on the POWER9 under
`~/.local/share/powerarm/RootFS/NAME` or through `--rootfs NAME=DIR`.

**Where each side runs a rootfs job:**

| Side | How |
|---|---|
| Pi (golden) | `a64diff run --rootfs NAME=DIR --rootfs-exec Scripts/powerarm/rootfs/run-in-sysroot.sh`. Each step runs as `run-in-sysroot.sh --env K=V... DIR WORKDIR -- argv...`: bwrap (or unshare) in a user namespace, no root, the rootfs read-only as `/`, and the working directory bound read-write at its real path |
| POWER9 `64k` | `a64diff run --rootfs NAME=DIR -- POWERarm`. Each step runs as `POWERarm argv...` with `POWERARM_ROOTFS=DIR` |

A rootfs step's environment is the sysroot runner's fixed set on every side, plus the job's
`env` lines:

```
PATH=/usr/local/sbin:/usr/local/bin:/usr/bin  LC_ALL=C  LANG=C  TZ=UTC  SOURCE_DATE_EPOCH=0
HOME=/tmp  TMPDIR=/tmp  SHELL=/bin/sh  TERM=dumb  PWD=<workdir>
```

a64diff hands the job's variables to the runner with `--env`, because the runner clears the
environment. Under POWERarm, the emulator's own `POWERARM_*`/`FEX_*` knobs are added.
| POWER9 `4k-kvm` | The rootfs is attached as a read-only virtio-blk disk image and mounted in the guest (below); the guest runs the steps as in `64k` |

**4K guest transport.** The rootfs is too big for the initramfs, and the 4K kernel
(`/boot/vmlinuz-linux-power9`, 7.2.6) rules out the shared-directory options:

- `CONFIG_NET_9P`, `CONFIG_VIRTIO_FS` and `CONFIG_EROFS_FS` are not set, and `/lib/modules/7.2.6`
  has no 9p, virtiofs or erofs module.
- There is no `virtiofsd` on the host.

What's left is a read-only disk image of a filesystem the kernel has: ext4 (built in) or
squashfs (a module, with zlib/lzo/xz and single-threaded decompression). Both images are built
without root (`mke2fs -d`, `mksquashfs -all-root`) from the verified tree and cached in
`~/.cache/a64diff/rootfs-img/` by content hash.

Measured on a 765 MB, 66k-file tree (larger than the 670 MB, 27k-entry Arch Linux ARM M2
sysroot), with an 8-vCPU, 4 GB guest and a cold cache:

| Transport | Image | Build (once per hash) | Mounted after boot | Read every file |
|---|---|---|---|---|
| **ext4** (no journal) | 943 MB | 15.9 s | 0.34 s | **6.0 s** |
| squashfs, lzo | 184 MB | 3.9 s | 0.59 s | 16.0 s |
| squashfs, xz | 135 MB | 7.1 s | 0.37 s | 30.8 s |

**ext4 is the default.** It reads 2.7× faster than the best squashfs, and the only extra cost
is disk space in the image cache. `--rootfs-transport squashfs` (lzo by default,
`A64DIFF_SQUASHFS_COMP` to change it) is kept for when disk space matters more. To repeat the
measurement, run with `A64DIFF_TEST_ROOTFS_UNVERIFIED=1 --rootfs NAME=TREE
--env A64DIFF_ROOTFS_BENCH=1`. The guest reads every file once and reports the time instead of
running suites.

**The prototype rootfs.** `minimal-debian` stands in until the Arch Linux ARM sysroot is ready,
and ships inside the bundle. `rootfs/mkrootfs-minimal.sh` assembles it from the golden
machine's own Debian `ld-linux-aarch64.so.1` and `libc.so.6` in multiarch layout, plus
`hello-dyn` (PIE), `hello-dyn-nopie` and the pinned static busybox. `<name>.sources` records the
libc6 and gcc versions. `programs/rootfs.jobs` runs the following against it:

- both hellos
- ld.so run as a program, both with a program and with `--version`
- a six-step busybox sequence with inputs, `env`, a `try` step and declared output files, one of
  them in a subdirectory
- a job whose declared output is never written

`programs/alarm.jobs` runs the following in the Arch Linux ARM sysroot:

- `/usr/bin/true`
- `gcc --version`
- a build sequence: `gcc -O2 -c hello.c`, `gcc -o hello hello.o`, then running `./hello`,
  with `hello.o`, `hello` and the run output byte-compared

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
same two controls per class, one on a stdout byte and one on the exit status. A class with
declared output files gets a third: one byte of a golden output file is flipped and its hash
recomputed. It must be flagged in exactly that job, on the outputs alone, and against the
actual result.

**Negative controls (`a64diff-selftest.sh`, Pi).** Each case has a required verdict.

| Case | Required verdict |
|---|---|
| Neutral wrapper | Pass (exit 0) |
| `broken-runner.sh` replacing the instruction with NOP | Fail (exit 1) |
| `broken-runner.sh` flipping bit 0 of the instruction | Fail (exit 1) |
| Comparator blind to `nzcv` (`--blind-field`) | Exit 2 through `CONTROL-FAIL`. Every test "passes" |
| Comparator blind to `x7` | Exit 2 through `CONTROL-FAIL`. Every test "passes" |
| Program output truncated to 64 bytes | Fail (exit 1) |
| Rootfs jobs, neutral | Pass (exit 0) |
| Rootfs jobs with `--break-outputs` (every declared output damaged before hashing) | Fail (exit 1), on the output files and nothing else |

A wrong verdict fails the selftest.

**Environment controls.** The 64k mode refuses to run unless the host page size is 65536, and
the guest refuses unless its page size is 4096. Booting the 64K image in `4k-kvm` mode exits 2.
Rootfs content hashes are verified on both sides. The golden script also checks isolation: through
the native runner, the rootfs's `/usr/lib/aarch64-linux-gnu` must show exactly the two files the
builder put there, not the golden machine's hundreds.

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

To add a program job, append a line or block to a `programs/*.jobs` file (a new file is a new
suite), keeping the output free of absolute paths, times, owners and readdir order. The job's
class gets controls automatically.

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
