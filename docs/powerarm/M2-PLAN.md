# M2 plan: Arch Linux ARM's GCC runs under POWERarm

Written 2026-09-16, after M1 (tag `m1`). This milestone is goal-driven: the failures the toolchain
hits decide what gets built.

## Status: complete (2026-09-17)

All exit criteria pass on the bare-metal 64K host and on the 4K kernel in KVM (commit `7c2bca629`,
bundle `a64diff-v1-s1-x2-41c1f1e4dd1e`):

| Suite | 64K host | 4K KVM |
|---|---|---|
| a64diff instruction suite | 3713/3731, required-fail=0 | same |
| programs (static glibc/musl, busybox) | 25/25 | 25/25 |
| alarm (Arch toolchain `--version`, `gcc -c`, link) | 3/3 | 3/3 |
| **projects: zlib 1.3.2 and Lua 5.4.9 built with Arch gcc/make, every output byte-identical to the Pi, tests and scripts run** | **2/2** | **2/2** |
| rootfs (Debian minimal rootfs: dynamic hello, libm, dlopen/TLS) | 6/6 | 6/6 |
| A64Frontend (default, `POWERARM_MAXINST=1`, ISA 3.0 off) | 45/45 each | n/a |
| `check-user-strings.sh`, `check-rootfs-server.sh` | OK | n/a |

**Compile baseline**, measured before any optimization (single pinned core, code cache off,
2 runs each, spread under 0.5%). The script is kept on the shared mount as
`.powerarm-golden/m2time.sh`:

| Build | Pi 5 native | POWERarm, POWER9 64K | Ratio |
|---|---|---|---|
| zlib (tar + configure + make) | 12.6 s | 133.6 s | 10.6× |
| Lua (tar + make linux) | 12.6 s | 118.3 s | 9.4× |

**Breakdown:**
| Step | Pi | POWERarm | Ratio |
|---|---|---|---|
| `cc1 -O2 lvm.c` (steady-state translated code) | 2.29 s | 12.3 s | 5.4× |
| `gcc -c empty.c` (cold translation) | 0.043 s | 0.388 s | 9× |
| zlib `configure` alone (hundreds of short processes) | 0.55 s | 14.0 s | 25× |

Roughly 75–80% of the build time is steady-state compute, and 20–25% is re-translating cold
code in every new process.

**Known limitations carried forward:**
- vfork copy-back is skipped for multithreaded parents (`POWERARM-M2-TODO`).
- The code cache (`POWERARM_ENABLECODECACHINGWIP`) stalled during a timing run and is untested.
- fastppcx86 shares the client rootfs-from-server bug fixed in `eaa81b2ae`; no patch was made.

Local tag `m2`.

## Optimization round 1 (2026-09-17)

Merged at `2c1d36246`:
- **OPT-BRANCHES:** link-first exits, call/return pairing, BR compare cache, direct not-taken exits.
- **OPT-REGALLOC:** small multiblock regions, context-register load reuse.
- **OPT-CODECACHE and OPT2-CACHEDEFAULT:** correct, keyed code cache; variable-width relocations; size cap and eviction; **on by default** for rootfs binaries.
- **OPT2-CODESHAPE:** direct in-unit conditional branches, displacement loads and stores.
- **OPT2-XLATE:** single decode lookup, early-out passes, shared backend walks.

Plus these fixes: `siglongjmp` host-frame leak, vfork copy-back with SMC, fast path copy, bounded fd table, NULL envp.

Same method as the baseline (`m2time.sh`, CPU 100, one run each; cache off via
`POWERARM_ENABLECODECACHINGWIP=0`; private cache directory):

| Build | Pi 5 native | Baseline (cache off) | Now, cache off | Now, cache cold | **Now, cache warm** |
|---|---|---|---|---|---|
| zlib | 12.6 s | 133.6 s (10.6×) | 92.4 s (7.3×) | 49.6 s (3.9×) | **46.6 s (3.7×)** |
| Lua | 12.6 s | 118.3 s (9.4×) | 79.0 s (6.3×) | 42.1 s (3.3×) | **41.3 s (3.3×)** |

- JIT alone (cache off): 1.45× / 1.50× faster than the baseline.
- Default configuration (cache on, warm): 2.9× faster than the baseline on both projects.

**After optimization rounds 2–3** (code cache default-on, translation speed, code shape,
startup, AOT option, cheaper linking; commit `c4948204c`, same method, CPU 100, one run each):

| Build | Pi 5 native | Baseline | Cache off | Cache cold | **Cache warm** |
|---|---|---|---|---|---|
| zlib | 12.6 s | 133.6 s (10.6x) | 88.0 s (7.0x) | 45.4 s (3.6x) | **42.3 s (3.4x)** |
| Lua | 12.6 s | 118.3 s (9.4x) | 76.1 s (6.0x) | 39.5 s (3.1x) | **38.7 s (3.1x)** |

Warm builds are 3.2x (zlib) and 3.1x (Lua) faster than the M2 baseline.

Remaining costs and next targets are in `OPTIMIZATION-CHECKLIST.md`:
- merging the register allocator, flag elimination and compare-branch fusion IR walks (translation);
- scalar FP F1–F8;
- NEON N1 (string early-exit fusion);
- P1(b) (context registers across blocks);
- memory/issue-queue stalls.

## Exit criteria

All must hold on **both** the 64K host and the 4K kernel in KVM:

1. Arch Linux ARM's own toolchain runs under POWERarm from an Arch Linux ARM rootfs: the `gcc`
   driver, `cc1`, `as`, `collect2`/`ld`, `ar` and `make`.
2. It builds **zlib** and **Lua 5.4** (both pinned by version and sha256) with `make`. Every
   object, archive, shared library and executable produced is **byte-identical** to the same
   toolchain and sysroot running natively on the Raspberry Pi 5.
3. The built programs run correctly under POWERarm: zlib's `example` and `minigzip` round trips,
   and a small Lua script set covering numbers/FP formatting, strings, tables/GC, errors and
   `pcall`. Output is identical to the Pi.
4. Wall time for each build is recorded for information only; it isn't a gate.

## Phase 0: prep (parallel)

| Task | Owner | Deliverable |
|---|---|---|
| A. Sysroot builder | fresh agent | `Scripts/powerarm/rootfs/` script that fetches pinned Arch Linux ARM aarch64 packages (toolchain plus dependencies) without root, extracts them into `~/.local/share/powerarm/RootFS/<name>/` (DESIGN §6.2a), and writes a manifest (package, version, sha256, mirror snapshot) |
| B. Native ARM runner | same agent | a way to run the same sysroot natively on the Pi for reference results: an unprivileged user-namespace chroot (`unshare`/`bwrap`), no root |
| C. Harness rootfs mode | W4 (resumed) | a64diff program jobs that run dynamically linked binaries inside a rootfs: natively on the Pi via B, under POWERarm via `POWERARM_ROOTFS`, and in `4k-kvm` via a shared directory or disk image instead of the initramfs |

## Phase 1: frontier ladder (W6 lead, resumed)

Each rung must pass on both kernels:

1. dynamically linked `hello` through the guest `ld.so` and `libc.so.6`
2. `gcc --version`, `cc1 --version`, `as --version`, `ld --version`, `make --version`
3. `gcc -c hello.c` (the driver spawns `cc1` and `as`; fix CLONE_VM/vfork properly if it bites)
4. `gcc hello.c -o hello` (`collect2`/`ld`)
5. zlib with `make`, byte-compared, then its tests run
6. Lua 5.4 with `make`, byte-compared, then the script set run

**Specialists**, resumed with batches of gaps: W2 for instruction gaps (integer, NEON, FP) and
W3 for syscall and ABI-table gaps. Each works in its own worktree; the orchestrator merges and
runs both kernels at each merge.

## Rules for every M2 agent

- **Optimization checklist:** read `docs/powerarm/OPTIMIZATION-CHECKLIST.md` before touching
  lowering, translator, allocator or dispatcher code. Claim any item whose code you pass
  through (`in progress`) and implement it in its own commit, with differential tests before
  and after.
- **ISA 3.0 gating:** every ISA 3.0 instruction is gated on `SupportsISA30` with a POWER8 path,
  and tests also pass with `POWERARM_HOSTFEATURES=disableisa30`.
- **Commits:** author Jordan Bettcher only, no AI attribution anywhere. A non-building commit
  carries a `DOES NOT BUILD:` line.
- **Host usage:** builds and tests on CPUs 0–87. Expected-crash runs use `ulimit -c 0`.
- **Pi usage:** only reference runs, single job, and only when the results change.
