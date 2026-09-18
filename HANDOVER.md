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
- **code-server 4.137.0 (VS Code, bundled Node v24.18.1) starts and serves** under POWERarm
  (2026-09-17), once `MRS CNTVCT_EL0` landed -- V8's clock source, and the one instruction that
  was stopping it. Verified end to end: HTTP 200 on the workbench HTML, `/healthz`,
  `/manifest.json` and a 1.05 MB `nls.messages.js`, the extension host agent (a child process)
  starting, and **zero** unimplemented-instruction reports for the whole session.
  It needs the stable install promoted, not just a fresh build: code-server spawns child Node
  processes, children reach the emulator through binfmt, and binfmt runs the stable copy.
- **FEAT_LSE is complete and advertised** (2026-09-17): the min/max forms and `CASP` landed, so
  `ID_AA64ISAR0_EL1.Atomic`, `AT_HWCAP` and `/proc/cpuinfo` all report it. Tests `lse`,
  `lseminmax`, `lsecasp` against Pi goldens.
- **Compile timings** in `M2-PLAN.md` predate real memory barriers. Do not re-measure them for
  their own sake — stop quoting them, and let the next optimization supply fresh numbers as a
  side effect. Slice baseline on `774e9ce8a`, CPU 104: cold 23.90 s, warm 21.4–21.6 s.
- **`powerarm-stable` is at `c37536838`** (promoted 2026-09-17, binfmt re-registered and
  `check-binfmt-inode.sh` passing). Promoting is what moves binfmt-launched programs -- including
  the Claude CLI and any guest child process -- onto new work, and a promote MUST be followed by
  `sudo sh ~/Development/register-powerarm-binfmt.sh`, because binfmt pins the interpreter's inode
  and the promote gives it a new one. `powerarm-stable.prev` is kept for rollback.

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
- **binfmt pins the interpreter's inode, and the entry lies about it.** Register the stable install
  (`~/.local/opt/powerarm-stable/Bin/POWERarm`, refreshed by `promote-powerarm-stable.sh`), never a
  build directory, and re-register after promoting. Skip the re-register and the entry still
  *prints* the stable path while running the previous build out of `powerarm-stable.prev`; `ps` and
  the process's own argv show the stable path too, because the kernel passes the registered string
  as argv[0] whatever inode it opened. Only `/proc/<pid>/exe` (or `maps`) tells the truth, which is
  what `Scripts/powerarm/check-binfmt-inode.sh` reads. Run it after every promote: a bug that
  "came back" after a fix is this until proven otherwise.
- **A process started before a promote keeps the old emulator.** Long-lived guests (an editor, a
  `claude` session, a terminal left open) hold the binary they started with. Compare the process
  start time against the promote time before believing a bug report.
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

1. ~~Memory-ordering (litmus) differential tests with Pi goldens.~~ **Done:**
   `unittests/A64Frontend/litmus.c` (hardware, Pi golden; its contended LDUMAX/LDSMIN counters
   also exercise `AtomicMinMax`'s retry back edge) and `unittests/MemoryModel/check.sh` (herd7,
   by model, a regression gate). Read both headers before trusting a PASS: POWER9 never shows
   store->store reordering, so release-side changes can only be validated by the model.
2. Make `POWERARM_PORTABLE=1` plus a named rootfs fail loudly.
3. ~~`MRS` of `CNTVCT_EL0` and `CNTFRQ_EL0`~~ **Done** (9fa8983a7); code-server now serves.
4. **P7, last lever: acquire-only and release-only LSE RMWs. A standard-agent task.** Relaxed
   RMWs and acquire fences are done (d96394b3d, 147903303) and trailing-sync is formally rejected
   (35f48f28b); see the checklist P7 row. What remains is the full hwsync/isync bracket still
   paid by RMWs with exactly one of acquire/release. Its value depends on the runtime. A census
   of executed variants (a throwaway counting build; see below) put them at ~6% of V8's atomics
   (~2.2 M per 3.9 s run, so under 1% even with every such fence deleted) but **33.5% of JSC's**
   (Bun, `claude --version`: CAS acquire 7.4 k, CAS release 7.3 k of 44 k). `--version` is too short
   to give JSC's steady-state rate, so the steps are, in order:
   a. Census a sustained Bun/JSC workload. Rebuild the census: in the A64 frontend, each atomic or
      barrier translator emits a load/add/store to a counter slot in a MAP_SHARED file named by an
      env var, keyed by variant (A/R bits for LSE, bit 15 for LDXR/STXR, CRm[1:0] for DMB). Never
      commit it; use a fresh code-cache dir per run, since the counter address is baked into code.
      If acquire/release-only RMWs are not a large share of a sustained run, stop here.
   b. Extend `unittests/MemoryModel` to RMWs. `diyone7 -arch AArch64 -show edges` lists the Amo
      edges; jingle7 needs lwarx/stwcx. rules, and herd7's PPC handling of reservations (no loop:
      a single attempt, conditioned on success) must be confirmed on a known case first.
   c. Candidate mapping. Release-only RMW: lwsync; loop. Acquire-only RMW: probably KEEPS the
      leading hwsync -- under the leading-sync convention that is what orders a preceding release
      before it (RCsc), exactly as LDAR keeps its own. Let the model decide, not reasoning.
   d. Implement it like the Relaxed flag (IR.json defaulted field, AtomicOps.cpp, the three LSE
      translators); gate with check.sh, litmus.c and the three-mode suite; measure once.
5. **An FP-heavy benchmark. This now blocks the rest of the F series.** F1 landed with the
   cold-block mechanism (`docs/powerarm/COLD-BLOCK-DESIGN.md`, `A64FArith`), and its only
   measurable effect on the workloads we have is a slice regression: cold 23.90 -> 24.58 s
   (+2.8%), warm flat. That matches its instruction counts -- the executed path loses 20 host
   instructions per FP arithmetic op, but a one-FP-op unit emits 4 more because the 15-instruction
   shared `NaNFix` body does not amortise there. The slice is a C compile and barely executes FP,
   and nothing in A64Bench (crc32, sha256, vm, sort, bst) is FP-heavy either, so the win is real
   and currently unmeasurable. Fix that before F2/F3/F6/N5 rather than shipping four more
   landings on inference; they share the same stubs and bodies, so the per-unit cost amortises as
   sites multiply.
6. fastppcx86: patch `0034` for the madvise bug (diagnosis written, patch not yet made).
7. Thunks, per `docs/powerarm/THUNKS-DESIGN.md` (fastppcx86 already runs them on this GPU).
   Stage 0 (AArch64 thunk ABI + guest vDSO) in progress 2026-09-17.
8. **Two-tier rootfs** (`DESIGN.md` §6.2a): designed, and being implemented 2026-09-17. Until it
   lands, guest `pacman` reads the HOST's package DB through fallthrough -- never trust its output.
9. **Research queued overnight 2026-09-17 (Fable agents, design docs, no code):** cold translation
   cost (`docs/powerarm/research/cold-translation/`) and code footprint / icache
   (`docs/powerarm/research/code-footprint/`). Each returns a ranked top five.
