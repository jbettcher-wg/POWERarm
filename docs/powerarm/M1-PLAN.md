# M1 plan: static AArch64 programs run, checked against real hardware

Written 2026-09-16, after M0 (`bface9d0d`). Scope, work split and exit criteria for M1.

## Decision: no separate interpreter

DESIGN.md §8 originally had M1 as an "A64 interpreter". **That's dropped.** FEX has been
JIT-only for years, and fastppcx86 has no interpreter backend. A separate interpreter would
implement every instruction's semantics twice and would only be run as a second opinion.
**The Pi 5 is a better second opinion: it's real hardware.** M1 therefore goes straight to A64 →
FEX IR → PPC64LE JIT. Correctness comes from differential testing against the Pi, not against
an in-tree interpreter. M1 and M2 from DESIGN.md §8 are merged and re-split below.

## Exit criteria (M1)

All of these must hold on **both** host kernels: the bare-metal 64K kernel, and the 4K kernel
in a KVM guest (the method in `research/va-size/vm_init.c`).

1. The instruction-level differential suite passes 100% for every instruction class M1
   implements. It includes positive controls, deliberate mismatches that must be reported.
2. Statically linked `hello` runs with the Pi's exact output and exit code, built against both
   **glibc** and **musl**.
3. A static `busybox` runs the M1 applet list (`echo`, `cat`, `ls`, `wc`, `sort`, `sha256sum`,
   `md5sum`, `grep`, `sed`, `awk`, `find`, `tar`, `gzip`, `sh -c` scripts) with output identical
   to the Pi on a fixed input corpus.
4. The syscall test programs match the Pi's results: return values, errno and struct contents
   after normalizing fields that legitimately differ (inode, dev, times).

## Workstreams

### Phase M1a: three agents in parallel, each in its own worktree and branch

| WS | Branch | Owns | Must not touch |
|---|---|---|---|
| **W1 frontend-core** | `powerarm-m1/frontend` | `FEXCore/Source/Interface/Core/A64Frontend/**`; additions to `CoreState.h`; IR ops only if an existing op truly can't express something (added, never repurposed) | `Source/Tools/LinuxEmulation/**` |
| **W3 syscalls** | `powerarm-m1/syscalls` | `Source/Tools/LinuxEmulation/**` (syscall table, arg/flag/struct translation, auxv, `uname`, `/proc` views) | `FEXCore/Source/Interface/Core/**`, `CoreState.h` |
| **W4 harness** | `powerarm-m1/harness` | `unittests/A64Diff/**`, `Scripts/powerarm/**`, the test corpus | Emulator sources |

**The seam between W1 and W3** is the existing `Syscall` IR op. W1 emits it for `SVC #0` with
the syscall number from X8 and arguments from X0–X5, and stores the result in X0. W3 implements
everything behind it. Neither side changes that contract without the orchestrator.

**W1 scope (integer core):**
- a table-driven decoder. Its source is upstream dynarmic's A64 decoder table (0BSD), recorded
  in `THIRD_PARTY.md`.
- block formation and terminators: `B`, `BL`, `BR`, `BLR`, `RET`, `B.cond`, `CBZ`/`CBNZ`,
  `TBZ`/`TBNZ`.
- data processing:
  - add/sub/logic with immediate, shifted and extended operands, plus their flag-setting forms
  - `ADR`/`ADRP`, `MOVZ`/`MOVN`/`MOVK`
  - bitfield ops, `EXTR`
  - the `CSEL`/`CSINC`/`CSINV`/`CSNEG` family, `CCMP`/`CCMN`
  - multiply and divide, including `UMULH`/`SMULH` and long multiply, with the ARM rule that
    division by zero gives 0
  - `CLZ`/`CLS`/`RBIT`/`REV*`
- load/store in every addressing mode: unsigned offset, pre/post-index, register offset with
  extend, literal, pairs, and all sizes and sign-extends.
- system instructions: `NOP` and the HINT space (including `BTI`/`PACIASP` as NOPs), `SVC`,
  `BRK` → SIGTRAP, `UDF` → SIGILL. `MRS`/`MSR` for `TPIDR_EL0`, `TPIDRRO_EL0`, `FPCR`, `FPSR`,
  `NZCV`, `CTR_EL0`, `DCZID_EL0`, `MIDR_EL1` and `ID_AA64*` (the kernel's sanitized EL0 view),
  and `DC ZVA`.

The 32-bit `W` forms must zero-extend (handbook zero-extension invariant). `SP` vs `XZR` is
register 31, decided by instruction form.

**W3 scope (Linux layer for static binaries):**
- **Syscalls:** `brk`, `mmap`/`munmap`/`mprotect`/`mremap`/`madvise`, `read`/`write`/`readv`/
  `writev`/`pread64`/`pwrite64`, `openat`, `close`, `lseek`, `newfstatat`/`fstat`/`statx`,
  `getdents64`, `readlinkat`, `faccessat`/`faccessat2`, `getcwd`, `chdir`, `dup`/`dup3`, `pipe2`,
  `fcntl`, `ioctl`, `uname`, `exit`/`exit_group`, `set_tid_address`, `set_robust_list`, `rseq`
  (ENOSYS is allowed), `prlimit64`, `getrandom`, `rt_sigaction`/`rt_sigprocmask`,
  `clock_gettime`, `gettimeofday`, `nanosleep`/`clock_nanosleep`, `getpid`/`getppid`/`gettid`,
  `get*uid`/`get*gid`, `sysinfo`, `umask`, `mkdirat`/`unlinkat`/`renameat2`/`symlinkat`/
  `linkat`, `utimensat`, `wait4`, `clone` (fork-style, no threads yet) and `execve`.
- **`ioctl`:** only the terminal ioctls busybox needs (`TCGETS`/`TCSETS*`, `TIOCGWINSZ`,
  `TIOCGPGRP`/`TIOCSPGRP`), with full termios translation.
- **Translation tables:** generated from the arm64 (asm-generic) and powerpc uapi headers in
  `~/Development/linux-7.2.6`, never hand-typed. They cover the `O_*`, `MAP_*`, `MCL_*`,
  `SA_*`/signal structs, `struct stat`, termios and `_IOC` encodings.
- **Auxv and `uname`:** `AT_HWCAP` = `fp`|`asimd`|`cpuid` for M1. The AArch64 ABI requires
  `fp`/`asimd`, and static glibc's string routines use NEON (see M1b). `uname -m` =
  `aarch64`. `AT_PAGESZ` = host page size.
- Remove the M0 `POWERARM-M0-TODO(syscalls|loader)` markers this work resolves.

**W4 scope (differential harness), independent of emulator internals:**
1. **Instruction-level tests.** A generator writes small static AArch64 ELF test programs.
   Each sets an initial state (GPRs, NZCV, SP, a data page, and later V registers/FPCR), runs
   one instruction or a short sequence, dumps the full state with `write(1, …)` in a fixed
   binary format, and calls `exit`. The state is random but reproducible (seeded), with an
   edge-case corpus: 0, −1, INT_MIN/MAX, carries and shifts at the boundaries.
2. **Golden and compare runs.** A golden run on the Pi executes natively and produces the
   expected dumps. A compare run on the POWER9 executes under `POWERarm` (with
   `POWERARM_HOSTPAGEMODE` unset) and diffs field by field. It has a **4K KVM mode** (initramfs
   plus the host's 4K kernel, as in `research/va-size/vm_init.c`).
3. **Positive controls:** a deliberately corrupted golden record must be reported as a
   mismatch, or the run fails.
4. **Program-level corpus.** Static `hello` (glibc and musl), static busybox and the M1 applet
   script, plus syscall test programs that print normalized results. All are built on the Pi,
   with golden outputs captured there.
5. **One command per side.** `Scripts/powerarm/a64diff-golden.sh` on the Pi,
   `Scripts/powerarm/a64diff-run.sh {64k|4k-kvm}` on the POWER9. Test binaries and goldens are
   not committed if they're large: generate them reproducibly and commit the generator.

### Phase M1b: after M1a merges

- **W2 frontend-simd:** the FP/SIMD subset static glibc and musl actually use. That means the
  string/memory routines (`memcpy`, `strlen`, `strchr`, `memchr`, `strcmp`), `printf`
  floating-point formatting, and `sha256sum`/`md5sum`. Measure the subset by disassembling
  the corpus rather than guessing.
- Scalar FP (`FMOV`, arithmetic, compares, conversions with ARM NaN and saturation rules),
  `FPCR` rounding.
- Busybox bring-up and the fixes it surfaces.

## Merge and review protocol

- Each agent commits on its branch in its own worktree, `~/Development/POWERarm-wt/<ws>`,
  and keeps its branch building (`build-<ws>` directory inside the worktree).
- The orchestrator merges `powerarm-m1/*` into `powerarm`, runs the harness on both kernels,
  and resolves conflicts.
- **Commits:** author Jordan Bettcher only. **No AI attribution anywhere**: commit messages,
  code comments, docs.
- Shared host: each agent caps ninja at `-j40` under `nice -n 10`.
