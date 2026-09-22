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
  It needs the stable install promoted, not just a fresh build (a trap that applies to any multi-process app): code-server spawns child Node
  processes, children reach the emulator through binfmt, and binfmt runs the stable copy.
- **FEAT_LSE is complete and advertised** (2026-09-17): the min/max forms and `CASP` landed, so
  `ID_AA64ISAR0_EL1.Atomic`, `AT_HWCAP` and `/proc/cpuinfo` all report it. Tests `lse`,
  `lseminmax`, `lsecasp` against Pi goldens.
- **Compile timings** in `M2-PLAN.md` predate real memory barriers. Do not re-measure them for
  their own sake — stop quoting them, and let the next optimization supply fresh numbers as a
  side effect. Slice baseline on `774e9ce8a`, CPU 104: cold 23.90 s, warm 21.4–21.6 s.
- **2026-09-18:** games and GPU work unthunked. SuperTuxKart (the ALARM aarch64 build) runs its
  benchmark at ~100 fps on the RX 7900 XTX, with MangoHud showing GPU and live JIT stats (item 11).
  The two-tier rootfs is merged and guest pacman installs into the vk overlay (item 8). A warm
  launch no longer recompiles the blocks of small, 0644 or dlclose'd libraries (item 10). The
  guest main stack can no longer sit where the host stack grows (item 12). getcwd inside the
  rootfs returns guest paths.
- **VS Code 1.138.0 (arm64 Electron 42) runs** (2026-09-18, dev build): the workbench, the
  extension host (up in ~4 s) and the built-in git/GitHub extensions. It lives at
  `~/Development/vscode-arm64/<version>`; its 61 library packages (gtk3, nss, cups, ...) are in
  the vk overlay. `~/.local/bin/code` launches it through binfmt: vk rootfs, `--no-sandbox`,
  `~/Development/vscode-arm64/current` -> the version dir. It needs a stable with 0e4825b60 and
  84ffbdcf8. An undocumented x86 VS Code under fastppcx86 from 2026-09-12 (it never worked) was
  removed, and its launcher replaced. The two fixes: `/proc`, `/sys` and `/dev` are host-only
  (the zygote and GPU process died on Chromium's `fstatat(proc_fd, "self/task/")`), and
  FMAXV/FMINV/FMAXNMV/FMINNMV (the renderer died on FMINV). Remaining unimplemented-instruction
  reports are all feature probes that also SIGILL on an A76 (GCS/TPIDR2 MRS, MTE, SVE `cnt`,
  SME).
- **End of 2026-09-18/19 session** (the ranked queue for what comes next is its own section below). main has: G1 (lld apps cached, `home` cache scope), G2
  (compare fusion, on), the NEON gap closure, the signal-frame fixes (register edits at the same
  PC, fpsimd_context, RIP table, drain points), the RLIMIT_AS fixes (allocator skip + guest limit
  held until execve), the fault reporter that survives its own faults, the /proc host-only fix,
  DC CIVAC/CVAC/CVAP, RBIT, the VectorImm byte splat. Gate: A64Frontend 83 pass, 2 vDSO skips.
  Firefox 156, VS Code 1.138 and Factorio 2.1.19 all run with windows, on stable since the
  2026-09-19 promote. Launchers: `~/.local/bin/{code,firefox,factorio}` plus
  `~/.local/share/applications/*-arm64.desktop`.
- **`powerarm-stable` is at `12f845814`** (promoted 2026-09-19, binfmt re-registered and
  `check-binfmt-inode.sh` passing; `.prev` holds the same binary from Jordan's own promote of the
  G1 build). Everything in the session bullet above is therefore live for binfmt-launched
  programs, including Firefox, VS Code and Factorio. Promoting is what moves binfmt-launched programs -- including
  the Claude CLI and any guest child process -- onto new work, and a promote MUST be followed by
  `sudo sh ~/Development/register-powerarm-binfmt.sh`, because binfmt pins the interpreter's inode
  and the promote gives it a new one. `powerarm-stable.prev` is kept for rollback.

## What POWERarm is for, and the reference workloads

POWERarm runs binaries that are not built for POWER on Jordan's Omarchy ppc64le port. It emulates
arm64 rather than x86 on purpose: AArch64 is weakly ordered like POWER, so it lowers with fences only
where the guest asks for them, while x86's TSO needs SAO or fences on essentially every access and
carries a higher IPC penalty. (fastppcx86 covers x86 -- Steam and the like -- at that cost.) The aim
is to be well rounded, and the concrete targets are:

1. **Newer Claude Code**: the Bun single binary ships only for x64/arm64 (JSC).
2. **VS Code**, insurance in case VSCodium is deprecated: Electron, i.e. Chromium plus Node/V8.
   Today's proxy is code-server (server and extension host); the desktop app needs the GPU thunks.
3. **arm64-native Linux games**, if any ship: Vulkan through the thunks. Until one exists the
   proxy is vkcube/vkmark, native against thunked.
4. **General compiled code**, the well-rounded baseline: the slice and the M2 zlib/Lua builds.

**North star: per-core parity with a 2.5 GHz Cortex-A76** -- at which point the 176-thread AC922 is, in effect, an Ampere Altra. The Pi 5 reference machine IS a 2.4 GHz A76, so the metric is the "x the Pi" ratio the checklist already reports, driven to ~1.0. Last recorded: A64Bench crc32 ~2x the Pi, vm ~6x. The POWER9 runs at 3.8 GHz, so parity means emulation overhead near 1.5x native POWER9. Every optimisation is judged by how far it moves that ratio across the reference workloads.

**Priority: optimise the emulator first, then gauge thunks by measurement.** Two of the three targets are
monolithic: the Claude binary is 221 MB and its only DT_NEEDED is glibc (JSC and Bun are inside it), and
Electron carries Chromium, V8 and Node inside likewise. For them almost everything runs emulated whatever gets
thunked, so emulation speed is the lever. Thunks come after, and each earns its place with a measured gain
against the optimised emulator. The candidates, in likely order: Vulkan (games, the VS Code GPU path), and a
**glibc pure-function redirect** -- memcpy/memmove/memset/memcmp, the str* routines, libm -- as a JIT-level
direct native call at the guest function's entry, the one thunk that reaches the monolithic binaries too
(`THUNKS-DESIGN.md` §4). Stateful glibc (malloc, stdio, pthreads, TLS, locale) stays emulated: the guest's own
glibc owns that process state.

**Measure against this set, not against whatever is at hand.** A result from one runtime is a data
point, not a priority: 2026-09-17's P7 census was V8-only, and JSC turned out to use acquire/release
CAS five times as heavily in proportion.

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
  Fixed: that combination now fails loudly.
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

## Next optimizations, in order (2026-09-19)

The two research docs rank the goals: `docs/powerarm/research/warm-codegen/WARM-CODEGEN-RESEARCH.md`
(§7, emitted-code quality) and `docs/powerarm/research/cold-translation/COLD-TRANSLATION-RESEARCH.md`
(§5, translation cost). Their estimates and baselines are recorded; do not re-derive them. Measure the
new thing once, prefer deterministic metrics (`instructions:u`, emitted bytes, block counts) over
wall clock, and keep correctness gates exhaustive.

**Already done, do not redo:** cold G1(a,b) (lld/64K-congruent apps cached, `CodeCacheScope=home`;
(c) AOT-at-install is still only a plan in CODE-CACHE.md); warm G2 (compare+branch and
compare+select fusion, FCMP direct CR0 decoding (F5), EntryNZCVLiveIn tracking, direct link gating, 2f8013325; exit sinking rejected on cycle overhead; all of Warm G2 complete);
warm G1(a) (the shared spill stubs moved to a context-lifetime island, 10245fbbf);
warm G1(c) (unlinked exit leg in link thunk, b1c6b1fcc);
warm G1(b,d) (link thunks, records, JITCodeTail, and RIP entries moved out of hot stream to cold region, 003933d8b, 53bd42492; warm slice 18.65 s, -11.8%);
warm G3 (pin `callret_sp` in host GPR `r22`, unpin guest X24 to keep dynamic RA at 5 registers, guard-page overflow on push/pop, dropping 4 memory ops per BL/RET pair, ca634b80f, b538d7e70; warm slice 17.69 s -> 17.37 s, -2.4%);
warm G4 (fold MOV/MVN aliases, coalesce copy debris, and extend clrldi elision, fd48d5360; warm slice 17.37 s -> 16.80 s, -3.3%);
warm G5 (unit granularity re-sweep across 64/8, 128/8, 256/8, 256/16 with dynamic environment overrides `POWERARM_REGIONWINDOW` and `POWERARM_MAXLEADERS` hashed in CodeCache; confirmed 128/8 default delivers optimal cold time 19.69 s and warm slice 16.91 s; all of Warm G5 complete);
FEAT_DotProd (asimddp), FEAT_RDM (asimdrdm), and FEAT_LRCPC (lrcpc)
implemented and advertised across HWCap, ISAR0/ISAR1, /proc/cpuinfo and sysreg tests;
test runner core-dump spam suppressed (3ca864f65);
the NEON gap closure; the signal-frame fixes; the RLIMIT_AS, /proc, DC CIVAC, RBIT and VectorImm fixes;
scalar FP optimizations complete: F1 (A64FArith, 603741288), F4 (zero-upper xxpermdi, 74bc1da0d),
F6 (mtvsrwa/wz & mffscrni, 74bc1da0d), F8 (FRINT* native roundings, 74bc1da0d),
F5 (CR0 direct FCmp & branch-free FCSEL/NZCVSelectV, 1cb25df75), F2 (A64FMinMax cold stubs, c38b6c8d8),
and F3 (A64FMulAdd cold stub, c38b6c8d8); Gates: Gate 1 86/86, Gate 2 24/24, Gate 3 0 required fail.

| # | Goal | Source | Estimate | Effort |
|---|---|---|---|---|
| 1 | **Warm G1, cold bytes out of the hot stream** (a) done 10245fbbf, (c) done b1c6b1fcc, (b) done 003933d8b, (d) done 53bd42492; **all of Warm G1 complete**. Hot stream contains solely hot instructions; link thunks, records, tail tables and RIP entries live in the cold region. Warm slice 20.95 s -> 18.65 s (-11%) | warm §7 G1 | all done; -11.8% warm slice | complete |
| 2 | **Warm G3, cheaper paired calls and returns** done: `callret_sp` pinned in host GPR `r22`, guest X24 unpinned to keep dynamic RA pool at 5 registers, guard-page overflow on push/pop, eliminating 4 memory ops per BL/RET pair. Warm slice 17.69 s -> 17.37 s (-2.4%) | warm §7 G3 | done; -2.4% warm slice | complete |
| 3 | **Warm G4, copy and zero-extension debris**: sound frontend MOV/MVN/NEG alias folds in `TranslateDataProcessing.cpp` and commutative constant identities in `ALUOps.cpp` landed. Producer masking and 64-bit consumer reads kept soundly isolated without touching RA or eliding zero-extensions | warm §7 G4 | frontend folds & commutative identities done | complete |
| 4 | **Warm G2, flags via CR / fusion** done 2db45e763, 2f8013325: A64 CMP/SUBS/CMN/ADDS fuse into B.cond and CSEL family; FCMP direct CR0 decoding; EntryNZCVLiveIn tracking; linker gating prevents direct linking to live-in flag targets. G2(c) exit-sinking recompute tested and rejected on cycle overhead (+1.2-2.6% cycles). **All of Warm G2 complete** | warm §7 G2 | done; warm cc1 cycles -4.5% | complete |
| 5 | **Warm G5, unit granularity re-sweep** done: re-swept `RegionWindow`/`MaxLeaders` (`Decoder.cpp:155-156`) with the cache warm and G1-G4 in place across 64/8, 128/8, 256/8, and 256/16. Added dynamic environment overrides `POWERARM_REGIONWINDOW` and `POWERARM_MAXLEADERS` hashed in CodeCache config ID. Default 128/8 confirmed optimal for cold compile (19.69 s) and warm slice (16.91 s) | warm §7 G5 | done; sweep verified | complete |
| 6 | **Cold G3, per-op cost cuts** done: skip `DynVRLiveIn` lookup and allocation for non-FPR compile units, add per-block `HasFlags` bit to skip DFCE backwards walk, `CompareFusion`, and `FoldBranch` scans for flag-free blocks, and hold `CodeInvalidationMutex` shared lock once across hit-path in `ExitFunctionLinkWithRecord`. Cold slice 19.69 s -> 19.51 s (-0.9% overall cold time, ~4% off cold JIT overhead); warm slice 16.92 s | cold §5 G3 | done; -4% cold JIT overhead | complete |
| 7 | **Cold G4, cache write off the exit path** done: background writer `fork()` after `TM.Stop` (and `exit_group`), plus skip `SweepCacheDirectory` and vector repacking on one-shot/final saves. Cold slice 19.51 s -> 16.97 s (-2.54 s / -13% faster), bringing cold within 0.04 s of warm (16.93 s) | cold §5 G4 | cold slice -13% (-2.54 s) | complete |
| 8 | **Thunks**: ship the guest vDSO by default (item 7), then Vulkan/GL. Guest vDSO done: decoupled guest thunk build, host stack guard clamping, HostOwnedRanges deduplication, Gate 1 85/85 pass, promoted to stable `GuestThunks/` | THUNKS-DESIGN.md | large on GPU apps, none on `cc1`/slice | complete |
| 9 | **Cold G2, translate-ahead helper thread** for single-threaded cold work, with a per-thread staging buffer | cold §5 G2 | `gcc -c empty.c` 20.6x -> ~12-14x est. | high risk, weeks |
| 10 | **Cold G5, cheap baseline tier for runtime-generated code.** Now has a motivating workload: Firefox hitches while browsing, and SpiderMonkey's JIT code is anonymous (never cached) and W^X (every patch invalidates translations). **Profile first**: `Scripts/powerarm/shmstats.py -- firefox <js-heavy url>` for the SMC and JIT-time share, plus the anonymous-code census in warm §8 | cold §5 G5 | JS runtimes only | very high; defer until the profile says |
| 11 | **Warm G6, P1(b) in its cheap form**: the frontend carries the GPR value cache across intra-unit edges with edge stores. Gated on register allocator cross-block liveness: `ConstrainedRAPass::Run` resets `Class.Available` and spills per block; dynamic SSA values across blocks trigger allocator assertion or SIGSEGV | warm §7 G6 | -2-3% general; **crc32 2.04x -> ~1.3x**, vm -10-20% | blocked on RA |
| 12 | **Warm G7**: THP for the code buffer, `nop` pad trimmed with G1. Done: code buffer PMD-aligned with `MADV_HUGEPAGE` hint (935b6b1f1), nop pads trimmed when link thunks and records moved out of hot stream to cold region (003933d8b, 53bd42492) | warm §7 G7 | iTLB 2.2x -> ~1x; -1-2% | complete |
| 13 | **Startup S4**: L1 lookup cache fault reduction: resized to 128k entries (2 MiB, 32 x 64K pages) + prefault writable via `MADV_POPULATE_WRITE` (fallback single-byte touch per host page) avoiding zero-page read faults followed by COW write faults. `true` minor faults 372 -> 299; cold slice 20.26 -> 17.45 s (-2.81 s / -14%), warm slice 17.83 s | OPTIMIZATION-CHECKLIST S4 | cold slice -14% (-2.81 s) | complete |
| 14 | **P7's last lever**: census of acquire/release-only RMWs across the reference set, then relax what the census allows | OPTIMIZATION-CHECKLIST P7 | small, compounding | small |
| 15 | **Warm G8, the F/N series** done for scalar FP: F1, F2, F3, F4, F5, F6, F8 complete (Gate 1: 86/86, Gate 2: 24/24, Gate 3: 0 required fail); NEON vector FP remaining to loop back | warm §7 G8 | scalar FP done; NEON next | scalar FP complete |

**Not levers** (measured, recorded in the docs): spill/fill volume (0.02%), block formation, IR-walk
merging, `FEX_O0`, lower `MaxInst`, RA cross-block liveness for cold, the dispatcher and lookup
(<1% warm), async cache writes (C11) and background cache install (C12).

**Correctness and compatibility queue** (not optimizations, but ahead of them when they block an
app): FPCR.DN and FZ (item 14); the signal-frame fidelity items (item 23); glycin icons
(item 21); Chromium's sandbox (item 16); the VS Code cache at 1.12 GB for one binary -- check whether
its ~14 processes each save their own copy of the same blocks; and Factorio's warm launch taking 38 s
to initialise against 27 s cold (suspect cache install cost, item 24).

## Open items, roughly in order

1. ~~Memory-ordering (litmus) differential tests with Pi goldens.~~ **Done:**
   `unittests/A64Frontend/litmus.c` (hardware, Pi golden; its contended LDUMAX/LDSMIN counters
   also exercise `AtomicMinMax`'s retry back edge) and `unittests/MemoryModel/check.sh` (herd7,
   by model, a regression gate). Read both headers before trusting a PASS: POWER9 never shows
   store->store reordering, so release-side changes can only be validated by the model.
2. ~~Make `POWERARM_PORTABLE=1` plus a named rootfs fail loudly.~~ **Done:**
   fails loudly with a clear diagnostic instead of silently falling through to host binaries.
3. ~~`MRS` of `CNTVCT_EL0` and `CNTFRQ_EL0`~~ **Done** (9fa8983a7); code-server now serves.
4. **P7, last lever: acquire-only and release-only LSE RMWs. A standard-agent task.** Relaxed
   RMWs and acquire fences are done (d96394b3d, 147903303) and trailing-sync is formally rejected
   (35f48f28b); see the checklist P7 row. What remains is the full hwsync/isync bracket still
   paid by RMWs with exactly one of acquire/release. Its value depends on the runtime. A census
   of executed variants (a throwaway counting build; see below) put them at ~6% of V8's atomics
   (~2.2 M per 3.9 s run, so under 1% even with every such fence deleted) but **33.5% of JSC's**
   (Bun, `claude --version`: CAS acquire 7.4 k, CAS release 7.3 k of 44 k). `--version` is too short
   to give JSC's steady-state rate, so the steps are, in order:
   a. Census the reference workloads (top of this file) -- a sustained Claude Code run, code-server,
      the slice -- not V8 alone. Rebuild the census: in the A64 frontend, each atomic or
      barrier translator emits a load/add/store to a counter slot in a MAP_SHARED file named by an
      env var, keyed by variant (A/R bits for LSE, bit 15 for LDXR/STXR, CRm[1:0] for DMB). Never
      commit it; use a fresh code-cache dir per run, since the counter address is baked into code.
      If acquire/release-only RMWs are not a large share across the set, stop here.
   b. Extend `unittests/MemoryModel` to RMWs. `diyone7 -arch AArch64 -show edges` lists the Amo
      edges; jingle7 needs lwarx/stwcx. rules, and herd7's PPC handling of reservations (no loop:
      a single attempt, conditioned on success) must be confirmed on a known case first.
   c. Candidate mapping. Release-only RMW: lwsync; loop. Acquire-only RMW: probably KEEPS the
      leading hwsync -- under the leading-sync convention that is what orders a preceding release
      before it (RCsc), exactly as LDAR keeps its own. Let the model decide, not reasoning.
   d. Implement it like the Relaxed flag (IR.json defaulted field, AtomicOps.cpp, the three LSE
      translators); gate with check.sh, litmus.c and the three-mode suite; measure once.
5. **Scalar FP series (F1-F6, F8) complete.** F1 (A64FArith, 603741288), F4 (xxpermdi, 74bc1da0d),
   F6 (mtvsrwa/wz & mffscrni, 74bc1da0d), F8 (FRINT*, 74bc1da0d), F5 (FCmp CR0 direct & branch-free FCSEL, 1cb25df75),
   F2 (A64FMinMax cold stubs, c38b6c8d8), and F3 (A64FMulAdd cold stub, c38b6c8d8) are landed.
   F7 (FPSCR sync) analyzed and skipped (~0% win, no hot FPSCR writes). All Gates pass
   (Gate 1 86/86 default and disableisa30, Gate 2 24/24, Gate 3 0 required fail).
   NEON vector float (N5) can loop back after pipeline/frontend optimizations.
6. fastppcx86: patch `0034` for the madvise bug (diagnosis written, patch not yet made).
7. Thunks, per `docs/powerarm/THUNKS-DESIGN.md` (fastppcx86 already runs them on this GPU).
   **Stage 0 landed (f2f9d3cfb):** the AArch64 thunk ABI (HLT #0x0F3F marker, X16/X17, X30 callback
   return) and a guest vDSO that takes clock_gettime/gettimeofday from 1000/1000 syscalls to 0/1000.
   **Next, and it is an emulation win, not a library thunk: ship the vDSO.** Today it builds only
   under `-DBUILD_THUNKS=ON`, which fails overall on the x86-layout host libraries, and nothing
   installs or promotes it, so no normal build or user gets it. Build `libVDSO-a64-guest.so` by
   default (it needs thunkgen/libclang and the rootfs gcc under the just-built POWERarm, not the
   host libs), install it to `GuestThunks/` (DESIGN.md §6.2a), have `promote-powerarm-stable.sh`
   carry it, and point the default `ThunkGuestLibs` there. The name was changed from
   `libVDSO-guest.so` on purpose so pre-marker emulators never map it. `run.sh` already gates
   itself either way (SKIPs the two vDSO tests when a build has none). Host.h's AArch64 stack rule
   (SP 0 mod 16) must not be cherry-picked to fastppcx86.
8. **Two-tier rootfs** (`DESIGN.md` §6.2a.1): merged 2026-09-18 (37be7c9aa).
   `ArchLinuxARM-vk-overlay` is bootstrapped with pacman, libxkbcommon and MangoHud v0.8.4 (built
   from source; not in the ALARM repos). Stable has had the overlay since the 2026-09-18 promote.
   The general trap remains: guest `execve` goes through binfmt, so every child process runs on
   STABLE, whatever build launched the parent. To keep children on a dev build, use
   `POWERARM_PORTABLE=1` with an absolute `POWERARM_ROOTFS`.
9. **Research landed (Fable, design docs, no code):** COLD, the cost of translating
   (`docs/powerarm/research/cold-translation/`), and WARM, the quality of the emitted code
   (`docs/powerarm/research/warm-codegen/`). Each gives a ranked top five across the reference
   workloads. Warm's cheapest lever is its G2: A64 `CMP` arrives as `SubNZCV`, while
   `CompareBranchFusion` accepts only `OP_SUBWITHFLAGS`, so no A64 compare+branch is ever fused.
   Its §8 specifies the instrumented build a standard agent does before G2-G4 are priced.
10. **Code cache gaps still open** (found fixing the per-launch recompiles, 63b2b607e):
    `check-code-cache.sh`'s exec'd children (cc1, as) run on STABLE through binfmt, because the
    exec'd config id is the same across builds, so those results test stable. It needs
    `POWERARM_PORTABLE=1`. A code-buffer rotation drops all unsaved blocks. A process killed by a
    signal saves nothing. lld-linked executables are never cached on 64K hosts (the MapFile 4K
    offset, cold research G1).
11. **Game metrics.** MangoHud for aarch64 guests is built by `Scripts/powerarm/mangohud/build-mangohud.sh`
    into the vk overlay. Its FEX panel (`fex_stats`) reads POWERarm's live stats through
    `powerarm-stats.patch`. `Scripts/powerarm/shmstats.py` logs the same stats to CSV (JIT time
    and blocks, SIGBUS, SMC, lookup-cache misses, cache lock time) for any run, with
    `POWERARM_PROFILESTATS=1` set for it. Desktop FPS moves with contention: POWER9 is SMT4, and
    a game thread sharing a core with build jobs slows sharply. Compare frames only on a quiet box.
12. ~~A large host stack frame in syscall context can corrupt the guest's stack.~~ **Fixed.**
    The guest's main thread runs on the process's own stack, and the ELF loader's
    MAP_FIXED_NOREPLACE scan settled the guest stack flush under the host `[stack]` mapping
    whenever its first try overlapped it (about one launch in eight with ASLR; every launch with
    `setarch -R` and `ulimit -s unlimited`). The next host frame deeper than any before wrote
    straight into the top of the guest stack with no fault. The loader now keeps the guest stack
    below the host stack's RLIMIT_STACK growth range and a PROT_NONE guard
    (`Threads::ReserveMainThreadStack`), and guest threads' host stacks got a 1 MiB guard too.
    Regression test: `hoststack` (run.sh forces the layout). Still on the stack, harmless now but
    82 KiB each on every call: the 64 KiB EXDEV copy buffers in `FileManager::Linkat` and
    `FileManager::Renameat2`.
13. **Guest self-signals read as host faults.** Chromium's crash handler re-raises a caught
    SIGILL on itself with `rt_tgsigqueueinfo` (si_code ILL_ILLOPC, si_addr = the guest pc). It
    arrives while the passthrough syscall is in a deferred-signal section, so POWERarm reports
    "FATAL host fault" and terminates instead of delivering it to the guest. A thread sending a
    signal to itself should mark that window so the classifier treats the signal as the guest's.
14. **FPCR.DN and FPCR.FZ have no effect** (`TranslateFP.cpp` POWERARM-M1-TODO(fpu)). No test
    sets them. Apps that enable flush-to-zero or default-NaN (audio DSP, some engines) get IEEE
    results instead.
15. ~~FP16 vector coverage vs the `asimdhp` claim.~~ **Done:** audited against ARMv8.2-A FP16
    specification. The entire group is implemented in `TranslateSIMDHalf.cpp` and `TranslateSIMDEstimate.cpp`
    (711c5963b) and verified by `simd_half` (2,536 test cases against Raspberry Pi 5 goldens).
    Remaining commented-out FP instructions in `a64.inc` belong to FEAT_FHM (HWCAP_FHM), FEAT_FCMA
    (HWCAP_FCMA), and BFloat16, none of which are advertised.
16. **Chromium sandbox.** VS Code runs with `--no-sandbox`. The real sandbox needs user
    namespaces and seccomp-bpf filters over arm64 syscall numbers; seccomp emulation is opt-in
    (`POWERARM_NEEDSSECCOMP`).
17. **Desktop rootfs installer script (Jordan's direction, 2026-09-18).** Tonight's vk setup should
    become one reproducible script (e.g. `Scripts/powerarm/rootfs/setup-desktop.sh <rootfs>`):
    overlay-init with pacman; the app package sets (VS Code's 61, Firefox's 60); host fonts
    (`~/.local/share/powerarm/host-fonts -> /usr/share/fonts`, plus the guest fontconfig snippet
    `/etc/fonts/conf.d/60-powerarm-host-fonts.conf` with `<dir prefix="xdg">powerarm/host-fonts</dir>`,
    which takes the guest from 36 to 848 fonts); re-running from the guest root the hooks guest
    pacman gets wrong; and the `~/.local/bin` launchers plus `~/.local/share/applications/*-arm64.desktop` entries.
    They exist by hand as of 2026-09-19: `code`, `firefox` (own profile ~/.mozilla/firefox-arm64) and
    `factorio` (~/Development/factorio-arm64/current -> 2.1.19). Each sets `POWERARM_ROOTFS=<vk>`
    and runs through binfmt.
    **Hook fidelity bug:** `40-fontconfig-config` reported success but linked nothing. It tests
    relative paths (`usr/share/fontconfig/conf.default/*`), and under POWERarm pacman's
    chroot("/") + chdir("/") doesn't leave the hook's cwd at the guest root. Re-run by hand with
    `cd /` in the guest it links all 21, and `sans-serif`/`serif`/`monospace` resolve to
    Noto Sans/Noto Serif/JetBrainsMono like the host. Harmless failures in a user namespace:
    sysusers, the systemctl reloads, and fc-cache's system cache (host `/var/cache`).
18. **Warm G2 complete** (2db45e763, 2f8013325): A64 CMP/SUBS/CMN/ADDS fuse into B.cond and the CSEL family,
    W and X. Warm cc1: instructions:u 21.90 -> 21.25 G (-3.0%), cycles -4.5%; warm slice
    20.95 s (was 21.4-21.6). Entry NZCV liveness and linker direct-link gating landed (2f8013325).
    Sinking recompute to exit legs is formally rejected due to cycle regression (+1.2-2.6% cycles),
    and jump-thunk recompute is blocked by fixed-layout cache records; all of Warm G2 is complete.
    G2 also fixed two signal bugs (RIP table zero-extended negative offsets; a back-edge drain point
    reported the branch, not its target).
    Tests: cmpbranch (1954 cases), sigpreempt.
19. ~~Signal handler register edits are ignored when the handler leaves the PC unchanged~~ **Fixed** (7d1bc2e39; test sigedit).
    (`RestoreFrame_Arm64`, found by the G2 agent, not fixed). A handler that fixes up registers
    and resumes at the same PC (emulating an instruction, or patching x0 after a fault) loses its
    edits. It's a correctness bug for runtimes with SIGSEGV/SIGILL handlers (JVMs, V8/JSC guard
    pages, Wine-style emulation).
20. ~~G2 fusion off by default~~ **Fixed and back on** (merge after f577af240; test cmpchain, 600
    cases). Chained compares (a compare reading the previous CSEL's result, as in clamps and
    min/max-of-three at X size) referenced a node an earlier rewrite had removed; operands are now
    read at rewrite time (eaf03c95d). Firefox renders with fusion on. The history of the bug: Firefox 156 segfaults
    deterministically with it on: `POWERARM_PORTABLE=1 POWERARM_ROOTFS=<vk> POWERarm
    <vk>-overlay/usr/lib/firefox/firefox --profile <tmp> --headless --screenshot out.png
    https://www.mozilla.org/firefox/` crashes in ~30 s (rc 139, twice) and renders with
    `POWERARM_DISABLECMPBRANCHFUSION=1`. So cmpbranch's 1954 cases miss a real-world pattern.
    Suspects: flags live across something the liveness proof misses, CSEL rewriting, or the CMN
    constant rule. G2's two signal fixes (RIP table, drain point) stay on. Re-enable only with a
    test that reproduces the Firefox miscompile.
21. **Firefox 156 runs with a window** (24e199f3f; Jordan browsed example.com, OSnews and YouTube).
    The root cause of the child crashes: guest `setrlimit(RLIMIT_AS)` (glycin lowers it in each
    loader fork before exec'ing bwrap) went straight to the host process, which the 128 TiB
    reservation puts far over any limit. After that the kernel refused every mmap, and
    `OSAllocator_64Bit::Mmap` hid the failure by returning the unmapped address, which
    CacheSegment::Open then read. Now the guest's RLIMIT_AS is held and applied at its execve
    (a0ce66b42; test forkexec), and the fatal-fault report writes its line first and survives
    unwinder faults (43187f44a; tests hostfault and hostfault_report). **Next:** icons still fail
    ("Could not load a pixbuf"); glycin-thumbnailer reports "Operation not supported", on stable
    too. The notes below are from the investigation.
    Earlier status: Installed in the vk overlay (60 packages). Headless
    rendering works (`--headless --screenshot`). The windowed browser never maps a window, because
    of several separate emulator bugs, all in child processes:
    - **Fixed tonight:** `DC CIVAC` (16d6213d0); `RBIT` vector (16d6213d0); VectorImm byte splats
      (dfd13ec41); startup under `RLIMIT_AS`: glycin runs GTK's image loaders under
      `bwrap` with an address-space limit, and POWERarm's 128 TiB 48-bit reservation crashed
      every such start. It's now skipped when RLIMIT_AS is finite, and run.sh checks `rlimit_as`.
    - **Open (a): code-cache compaction crashes in a forked child.** A fork (no exec) of the
      80-thread main process or of the fork server calls execve; the pre-exec save runs
      `SaveNewBlocks -> CompactSegments -> CacheSegment::Open`, which SIGSEGVs (full backtrace
      from core 1450583).
    - **Open (b): the fatal-fault handler calls backtrace() and faults recursively**
      (`SignalHandlerThunk -> backtrace -> _Unwind_Backtrace`, dozens of nested frames). That
      buries the original fault report and truncates cores. It should print its line first,
      without unwinding, and guard against re-entry.
    - **Open (c):** with `POWERARM_ENABLECODECACHINGWIP=0` the whole browser dies (2 cores,
      truncated). Not yet diagnosed.
    - **Open (d):** G2 fusion miscompiles Firefox (item 20).
    Launch for testing: `POWERARM_PORTABLE=1 POWERARM_ROOTFS=<vk> <dev POWERarm>
    <vk>-overlay/usr/lib/firefox/firefox --profile ~/.mozilla/firefox-arm64 <url>`. Check windows
    with `hyprctl clients`; unwind cores with `coredumpctl dump <pid>` + gdb `bt -30`.
    Never pattern-kill: the orchestrator's shell got killed three times by patterns matching its
    own command line.
22. **NEON gap closure landed** (b1a10d960, 6 new Pi-golden tests, 9,460 cases): FRECPE/FRSQRTE/
    FRECPS/FRSQRTS/URECPE/URSQRTE/FRECPX bit-exact; SDOT/UDOT by element, FMULX, PMUL; the
    saturating shifts by register, SUQADD/USQADD, SQDMULL/SQDMLAL/SQDMLSL; FCVTXN; the whole FP16
    vector/scalar group (exact but slow, one lane at a time through double; ISA 3.0
    xvcvhpsp/xvcvsphp if an FP16-heavy workload appears); scalar SLI/SRI/SSRA/USRA and
    SQRSHRN/UQRSHRN/SQRSHRUN. LDXP/STXP/LDAXP/STLXP (exclusive pairs, base ARMv8.0)
    implemented in TranslateExclusive.cpp via 32-bit (64-bit memory CAS) and 64-bit (128-bit memory CASPair)
    software monitor lowering with acquire/release barriers; test exclusive_pair.
    FEAT_DotProd (281e837b7, asimddp), FEAT_RDM (a9f830ba4, SQRDMLAH/SQRDMLSH, asimdrdm), and
    FEAT_LRCPC (lrcpc) are implemented and advertised (ELFCodeLoader.h HWCap,
    SystemRegisters.h ISAR0/ISAR1, cpuinfo, the sysreg test). FCADD/FCMLA and SHA-512/SHA-3/SM3/SM4
    are not on the A76 and not advertised.
23. **Signal-frame fidelity, still open** (found fixing item 19): a signal delivered during a syscall
    shows X0 as the syscall's first argument, not its result, so a handler that moves the PC
    resumes with that stale X0. There's no `esr_context` record. UDF/BRK set `fault_address`
    to the PC, where the kernel gives 0. `uc_stack` isn't read back at sigreturn. The frame
    has carried `fpsimd_context` (V0-V31, FPSR, FPCR) since 7d1bc2e39.
24. **Factorio 2.1.19 (linux-arm64) runs** (2026-09-19, promoted stable): native Wayland or X11, OpenGL 4.6
    on radeonsi, PulseAudio, auth TLS OK. fastppcx86's graphics benchmark (`--benchmark-graphics
    bench_default.zip --benchmark-ticks 2000 --benchmark-runs 4 --output-perf-stats`, save copied
    from ~/Development/fexrootfs/factorio-backup/saves, SDL x11, taskset 0-87, one run on a cold
    cache): cpu-frame median 16.67 ms (the 60 fps cap), 12.0 ms of CPU work without sleep, 76% of
    frames at 60 fps and 93.5% with idle time left. fastppcx86's x86-64 2.0.77 on the same box and
    save: 19.6 ms, CPU-bound. Factorio versions and Mesa differ, and x86 was SMT2.
    **Same-day head-to-head (2026-09-19, same benchmark, SDL x11, taskset 0-87, neither GL
    thunked):** arm64 2.1.19 under POWERarm, WARM: 1,997 frames, busy median 10.5 ms, update 3.24,
    prepare 1.21, cpu-render 4.26 ms, 77% at 60 fps, 0.3% stalls >50 ms. x86-64 2.0.77 under
    fastppcx86 (binfmt build-smc, Ubuntu 24.04 rootfs, kisak Mesa 25.3/LLVM): 1,930 frames, busy
    13.5 ms, update 3.22, prepare 1.70, cpu-render 5.10 ms, 67% at 60 fps, 0.8% stalls. That's
    ~22% less CPU per frame for POWERarm; simulation is even and the lead is in the guest Mesa
    driver work. The cold arm64 run delivered only 940 frames (first-run stalls). Oddity: the warm
    arm64 launch took 38 s to initialise vs 27 s cold. Suspect cache install cost; to investigate.
25. **sleeve, the app manager TUI: implemented (Phase 1 & 2) and extracted as a standalone companion repo** (2026-09-21).
    Design: `docs/powerarm/APPS-TUI-DESIGN.md`. Standalone repository: `~/Development/sleeve`.
    Multi-backend support: `POWERarm` (AArch64) and `fastppcx86` (x86_64), selectable via `--backend=powerarm|fastppcx86`
    or `SLEEVE_BACKEND` (defaults to `powerarm` on this host).
    - **Features live:** Scanner (shape heuristics: Electron, Gecko, Game, Pacman, CLI), wrapper generation
      (`~/.local/bin/<app>`, `~/.local/share/applications/<app>-<arch>.desktop` with golden formatting, atomic rename,
      and ownership checks), AppConfig writer (`~/.config/<backend>/AppConfig/<program>.json` with key merging and
      validation), health checking (`sleeve check <app>` running target and classifying output/exit signals), and
      live Omarchy theme recolouring via inotify.
    - **Gates verified:** All 4 Phase 2 gates passed (`POWERarmGetConfig --current-rootfs`, live `ProfileStats` + `shmstats.py`,
      health check classification for normal exit and bad-ELF exit 248, dynamic theme tinting), plus 22/22 Catch2 unit tests.
    - **Installed:** `~/.local/bin/sleeve`. Existing launchers for `code`, `firefox`, `factorio`, and `antigravity-ide`
      are wrapped and registered.
26. **FEAT_RDM and FEAT_LRCPC implemented and advertised** (2026-09-21, a9f830ba4): Antigravity IDE's
    bundled `language_server_linux_arm` crashed at startup via Google toolchain `go/sigill-fail-fast`
    checks requiring `-march=armv8.2-a+dotprod` features (`asimddp`, `asimdrdm`, `lrcpc`). Fixed by:
    - Implementing `SQRDMLAH` and `SQRDMLSH` (vector and by-element forms) in `TranslateSIMDSaturate.cpp`
      via `SIMDDoublingMultiplyAccumulateHigh` with intermediate rounding and destination saturation (FEAT_RDM).
    - Advertising `asimdrdm` and `lrcpc` (FEAT_LRCPC, whose LDAPR instructions were already fully implemented)
      in `ELFCodeLoader.h` (HWCap), `SystemRegisters.h` (`ID_AA64ISAR0_EL1` RDM=1 matching Cortex-A76 / Pi 5
      at `0x0000100010211120`, and `ID_AA64ISAR1_EL1` LRCPC=1 at `0x0000000000100000`), `/proc/cpuinfo`
      Features line, and updating `unittests/A64Frontend/sysreg.S`.
    - Tested: `language_server_linux_arm --help` runs cleanly to exit 0. All 3 test gates pass
      (83 pass, 0 fail; a64diff 64k pass; check-code-cache 24/24 pass).
27. **Cold G3 per-op cost cuts completed** (2026-09-21): per-op overheads in emission, flag optimization, and link locking cut:
    - Skipped `DynVRLiveIn` vector allocation and per-op `IRView->GetID(CodeNode)` division/mask lookups when `!UnitHasFPRWork`.
    - Added `HasFlags` tracking bit to `CodeBlock` in `IR.json`/`IREmitter.h` set by `IsFlagOp()`, skipping the backwards DFCE walk, `EntryNZCVLiveIn` walk, `CompareFusion`, and `FoldBranch` on blocks with no flag operations.
    - Refactored `ExitFunctionLinkWithRecord` to hold the shared `CodeInvalidationMutex` guard once across both `FindBlock` and `LinkAndPatch` on cache hits, avoiding redundant unlock/relock overhead.
    - Gates verified: 83 pass / 2 vDSO skip, `check-code-cache` 24/24 pass, `a64diff 64k` pass across all 5 test suites. Cold slice 19.69 s -> 19.51 s; warm slice 16.92 s.
28. **Cold G4 cache write off the exit path completed** (2026-09-21): eliminated synchronous exit-path disk I/O and one-shot directory sweep overhead:
    - Skipped `SweepCacheDirectory` on one-shot exit paths (`Kind == CodeCacheSaveKind::Final && !RanPeriodicPass`) and skipped `CompiledBlocks`/`RelocationSink` vector repacking when `Kind == CodeCacheSaveKind::Final`.
    - Added background `fork()` writer in `FEXInterpreter.cpp` (after `TM.Stop(true)`) and in `Thread.cpp` (`exit_group`), allowing the parent process to exit immediately without waiting for disk serialization when `!Stats`.
    - Gates verified: `unittests/A64Frontend/run.sh` 83 pass / 2 skip, `check-code-cache.sh` 24/24 pass, `a64diff-run.sh 64k` pass across all 5 test suites. Cold slice 19.51 s -> 16.97 s (-2.54 s / -13% faster; cold compilation now within 0.04 s of warm 16.93 s).
29. **Guest vDSO enabled by default** (2026-09-21, 1a9f70395): `libVDSO-a64-guest.so` built and installed by default, eliminating context switches and host kernel syscall overhead for `clock_gettime`, `gettimeofday`, and `time`. Also resolved host stack collision avoidance and portable path probing.
30. **Vector-scan idiom branch fusion (N1)** (2026-09-21, 11c5be45c): fused CMEQ → UMAXP/ADDP → FMOV → CBZ/CBNZ idiom in A64Frontend via `TryFuseVectorScan`, lowering directly to backend `_CondJump` in `VCmpElementSize` mode (`vcmpequb.` + `bc` on CR6[2]). Loop hot edge skips reduction, GPR move, and compare.
31. **Native SQDMULH.8H and SQRDMULH.8H (N6)** (2026-09-21, 01494f413): lowered 16-bit doubling high multiply with and without rounding directly to native AltiVec `vmhaddshs` and `vmhraddshs` (~25 instructions -> 2 instructions).
32. **LookupCache L1 resize & prefault (S4)** (2026-09-21, 11b6bfd77): L1 resized to 128k entries (2 MiB, 32 x 64K pages) and prefaulted writable via `MADV_POPULATE_WRITE` (with single-byte touch fallback), eliminating zero-page COW faults. Cold slice 20.26 s -> 17.45 s (-14%).
33. **Exclusive pairs LDXP, STXP, LDAXP, STLXP** (2026-09-21, ec69f58ed): 32-bit (64-bit pair) and 64-bit (128-bit pair) exclusive load/store pairs implemented in `TranslateExclusive.cpp` via CAS/CASPair software monitor lowering with acquire/release barriers; test `exclusive_pair`.
34. **`/proc/self/exe` re-exec resolution** (2026-09-21, 290a21401): resolved `/proc/self/exe` to guest binary path inside RootFS / overlay, allowing self-re-executing applications (such as Chromium / Electron zygotes and sandboxed child processes) to re-exec cleanly under emulation.
35. **Hardware Vector FP16 Conversions (ISA 3.0) with verified ISA 2.07 fallback (N14)** (2026-09-21, 42c94ed53):
    - Added `xvcvhpsp` and `xvcvsphp` to `CodeEmitter/PPC64LE/Emitter.h`.
    - Implemented `Vector_FToF` (f16x4 <-> f32x4), `VFCVTL2` (f16x4 -> f32x4), and `VFCVTN2` (f32x4 -> f16x4) in `VectorOps.cpp` using `vmrglh`/`vmrghh`, `xvcvhpsp`, `xvcvsphp`, `vpkuwum`, and `xxpermdi`.
    - Preserved exact ISA 2.07 fallback via software FABI helpers, fixing IEEE-754 / ARMv8 NaN quieting in `f16_to_f32` and `f32_to_f16` (setting bit 22 for f32 and bit 9 for f16 on NaNs) to ensure 100% byte-for-byte golden matching under `POWERARM_HOSTFEATURES=disableisa30`.
    - Measured throughput gain: **5.0× speedup** on vector FP16 conversions (425.5 ms / 5.32 ns/op on ISA 3.0 vs 2,123.4 ms / 26.54 ns/op on ISA 2.07 fallback); 4.6× speedup on buffer conversions (30.4 vs 139.5 ms); 14% speedup on N-body double-precision integrator (100.6 vs 114.5 ms).
    - All verification gates passed (Gate 1: 86/86 default and disableisa30; Gate 2: 24/24; Gate 3: 0 required fail).



