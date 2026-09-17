# POWERarm handover

Written 2026-09-17. Where the project stands, how to work on it, and the traps that cost time.
Read this plus `DESIGN.md` and `OPTIMIZATION-CHECKLIST.md` before starting.

## Machines

| Role | Host | Notes |
|---|---|---|
| Build, test, everything heavy | POWER9 AC922, `ssh jbettcher@192.168.2.24` | repo `~/Development/POWERarm`; 176 threads; 64K-page kernel; KVM guests give a 4K kernel |
| ARM reference (goldens) | Raspberry Pi 5, Cortex-A76 | sees the repo over sshfs at `~/Development/power9_development/POWERarm`; **keep its load light**; it has rebooted mid-session more than once |
| GitHub | `github.com/jbettcher-wg/POWERarm` | push from the POWER9 (`~/.ssh/id_ed25519_github`); branch `powerarm` tracks `origin/main` |

The `fastppcx86` remote is fetch-only (push disabled). Backend fixes flow between the trees by
cherry-pick; patches for them go in `docs/powerarm/outgoing-patches/fastppcx86/`.

## State

- **M1** (static programs) and **M2** (Arch Linux ARM GCC builds zlib and Lua byte-identical to the
  Pi) are complete and tagged `m1`, `m2`.
- **Optimization rounds 1–3** merged: branch handling, code shape, translation speed, code cache
  (on by default), startup, opt-in AOT, cheaper linking.
- **The aarch64 Claude Code CLI installs and runs** under POWERarm.
- **Compile timings** are in `M2-PLAN.md`. **They predate real memory barriers** (see below) and
  must be re-measured before being quoted again.

## The bugs that mattered (and what they teach)

1. **No memory barriers at all** (fixed 2026-09-17). `DMB`/`DSB` decoded as hints; `LDAR`/`STLR`/
   `LDAPR` used the plain load/store path. Multi-threaded guests lost wakeups under load. Single-
   threaded tests can't see this: **add litmus-style ordering tests with Pi goldens** (still open).
2. **Destructive `madvise` over fallback-mapped segments** zeroed 40 MB of a guest's own data. On a
   64K host, segments whose file offset isn't 64K-congruent are loaded as anonymous memory plus a
   read, so `MADV_DONTNEED` had nothing to fall back to. Fixed by recording those ranges and
   re-reading the file. **fastppcx86 has this bug and it hits x86 guests far more often.**
3. **FEAT_LSE atomics were missing** from the decode table. Binaries set
   `__aarch64_have_lse_atomics = 1` unconditionally, so they run LSE regardless of HWCAP.
4. Several Linux-layer bugs (EFAULT paths, fd placement, vfork copy-back, `AT_PLATFORM`,
   `siglongjmp` frame leak) are fixed here and sent to fastppcx86 as patches 0001–0033.

## Traps that cost hours

- **`POWERARM_PORTABLE=1` breaks rootfs-by-name.** Portable mode moves the data directory, so a
  config `"RootFS": "ArchLinuxARM-m2"` resolves to nothing and guest paths silently fall through to
  host ppc64le binaries ("not a supported ELF"). Pass `POWERARM_ROOTFS=<absolute path>` with it.
  **Open item: make that combination fail loudly.**
- **binfmt pins the interpreter's inode.** Register the stable install
  (`~/.local/opt/powerarm-stable/Bin/POWERarm`, refreshed by `promote-powerarm-stable.sh`), never a
  build directory, and re-register after promoting.
- **A rebuild invalidates the code cache**, so the first run after building is cold. Discard the
  first run after a link.
- **`perf` leaks its environment into the guest.** Wrap workloads in
  `env -u LD_LIBRARY_PATH PATH=/usr/local/bin:/usr/bin`, or the guest shell runs host-native tools
  and the profile is wrong.
- **a64diff always works in `/tmp/a64diff-work`.** Two runs at once corrupt each other; check
  `pgrep -f a64diff-run` first. Beware `pgrep`/`pkill` patterns that match your own command line.
- **"Unimplemented A64 instruction" logs happen at translation, not execution.** Block formation
  walks past guards. Only a SIGILL (exit 132) proves execution.
- **Micro-edits in hot translation files move the slice ±0.25 s** through code layout alone. Use
  the emulator's own user CPU, not wall time, for small effects.

## How to work

- **Build:** `taskset -c 0-87 nice -n 10` + clang, Release, `-DBUILD_THUNKS=OFF -DBUILD_TESTING=OFF`.
- **Per-change check:** `unittests/A64Frontend/run.sh <POWERarm> <golden dir>` (default mode), with
  goldens in `~/Development/.powerarm-golden/claude-simd` (host path `/mnt/arch/home/...`).
- **Full gates, once per merge:** that suite in 3 modes (default, `POWERARM_MAXINST=1`,
  `POWERARM_HOSTFEATURES=disableisa30`); `Scripts/powerarm/a64diff-run.sh 64k` and `4k-kvm` with the
  latest bundle in `~/a64diff/bundles`; `check-user-strings.sh`, `check-rootfs-server.sh`,
  `check-code-cache.sh` (absolute build path).
- **Measurement:** one run per change on the small slice
  (`~/Development/.powerarm-golden/slice.sh`), **cold and warm as a pair**; full zlib/Lua builds and
  full gates once at the end. Bug-fix work carries no timing at all.
- **Agents:** at most two at once; brief them with the file areas they own; they smoke-test and hand
  back, and the orchestrator runs the final gates. An agent past about an hour is usually
  over-measuring.
- **Commits:** author is the owner only, no AI attribution anywhere. A commit that doesn't build
  says so in its message.

## Open items, roughly in order

1. Memory-ordering (litmus) differential tests with Pi goldens.
2. Make `POWERARM_PORTABLE=1` plus a named rootfs fail loudly.
3. Finish the LSE family (`LDSMAX`/`LDSMIN`/`LDUMAX`/`LDUMIN`, `CASP`), then advertise
   `HWCAP_ATOMICS`.
4. Re-measure the compile baseline now that barriers are real; then P7 (lightest correct fence per
   case) is a live optimization target.
5. The cold-block emission mechanism in the backend, which unlocks F1–F3, F6, N5; F4, F5 and N1 need
   no new machinery.
6. fastppcx86: patch `0034` for the madvise bug (diagnosis written, patch not yet made).
7. Thunks (GL/Vulkan first, then OpenSSL), per `DESIGN.md` §6.
