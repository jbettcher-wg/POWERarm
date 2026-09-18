# Cold translation: where the next 10-30% of a cold run is

Design research, 2026-09-17. No code. Companion to `OPTIMIZATION-CHECKLIST.md`
("Cold runs are a first-class metric", the X series, Q4) and `CODE-CACHE.md`.
Every citation below was re-read in the tree at `ae9d9b1dd`; line numbers are
from that commit.

**One-paragraph answer.** For the two monolithic reference targets the cold run
is not a first-run problem, it is *every* run: the code cache is scoped to the
RootFS (`Config.json.in:78-90`), so code-server's `node` is never cached, and
the Claude Code binary cannot be cached under any scope because the 64K-host
ELF loader `pread`s its segments into anonymous memory
(`ELFCodeLoader.h:241-258`, `HostPageMapping.h:51-71`), which also costs it a
50 ms `map` phase per launch and leaves no file section for the cache, AOT or
the JIT perf map to key on. Fixing the loader and the scope, then AOT-seeding
apps at install time, is the largest cold lever across the set and needs no
compiler work: measured, it takes code-server from 4.47 s to 2.35 s to its
first HTTP response (8.8x -> 4.6x the Pi). After that, the compiler's cold cost
is ~10-20 us per compile unit, spread thinly (backend 27%, RA 16%, frontend 15%,
passes 10%, install/link 7% of a one-shot run), and the only large structural
lever left is taking translation off the guest thread's critical path with a
translate-ahead helper. Per-op micro-work is worth 5-7% of a one-shot cold run
and must be gated on `instructions:u`, not wall time.

## 1. Method and caveats

- Emulator: the main tree's `build-powerarm/Bin/POWERarm` (built 22:39, same
  sources as `ae9d9b1dd`; the top commit is docs-only). Never promoted, never
  through binfmt: every run passes `POWERARM_PORTABLE=1` with an absolute
  `POWERARM_ROOTFS`, a fresh private `POWERARM_APP_CACHE_LOCATION` per series,
  and is wrapped in `env -u LD_LIBRARY_PATH PATH=/usr/local/bin:/usr/bin`.
- **Contended machine.** Another agent was building on CPUs 88-160 throughout
  (load average 2-6). All my runs are pinned with `taskset` to CPUs 40-47
  (two SMT4 cores: 40-43 and 44-47), one run per configuration. Profile
  *shares* are the numbers to trust; wall and user times are direction
  indicators (they reproduce the recorded `gcc -c empty.c` cold/warm numbers
  within 10%, which is reassuring).
- `perf` is the host's `/mnt/arch/usr/bin/perf` (needs
  `LD_LIBRARY_PATH=/mnt/arch/usr/lib`, which is why the workload must scrub it).
  `perf record -e cycles:u`, 5 kHz for the 0.5 s Claude run, 2 kHz for
  code-server. No JIT naming during profiled runs (it perturbs the thing being
  measured); the block census runs are separate.
- Pi 5 references were measured **once** each on `pi5` (load ~1.3, single
  job): Claude Code is installed there as 2.1.275 (the POWER9 runs 2.1.276;
  same binary generation), code-server 4.137.0 was copied to `/tmp` for two
  runs and removed, the slice ran from the Lua tarball in `/tmp`.
- Fresh profiles were taken for the two workloads with no recorded cold
  profile: Claude Code `--version` and code-server startup. The `gcc -c
  empty.c` and slice profiles are the recorded Q4 ones and were not re-derived.
- Scripts and raw outputs are in the session scratchpad
  (`scratchpad/cold/run.sh`, `cs.sh`, `cs-pi.sh`, `*.stderr`, `*.perf`,
  `*.perfmap`); nothing was added to the tree besides this file.

## 2. The reference workloads, cold, against the Pi

| Workload | POWER9 cold (today) | POWER9 warm | Pi 5 | x the Pi cold / warm | Notes |
|---|---|---|---|---|---|
| `gcc -c empty.c` (one-shot) | 0.35 s wall; `cc1` user 200 ms, save 30 ms | 0.12 s; `cc1` user 56 ms | 0.017 s | **20.6x / 7.1x** | recorded Q4: 311 / 93 ms. Pi number is mine (3 runs, 17 ms each; the S-series' 43 ms was a different harness) |
| Claude Code `claude --version` | 0.50 s wall; guest 431 ms, map 50 ms, save 12 ms | **no warm exists** (0.49 s) | 0.010 s | **50x / 50x** | 25,671 compile units, 24.3 MB host code; 94% of units are in the ELF's text but reach the JIT as anonymous memory (section 7) |
| code-server to first `/healthz` 200 | 4.47 s | **no warm exists** (4.44 s) | 0.51 s | **8.8x / 8.7x** | main `node` user 3.65 s at kill; 123,177 units, 105 MB host code; 84% in the `node` ELF, 14% V8-generated |
| code-server, `CodeCacheScope=all` | 3.84 s (writes 172 MB) | **2.35 s** | 0.51 s | 7.5x / **4.6x** | measured; `node` user 3.85 -> 1.60 s |
| slice (10 Lua objects + ar/ld) | 23.90 s (recorded) | 21.4-21.6 s (recorded) | 6.3 s | **3.8x / 3.4x** | Pi number is mine (one run, `user 6.0 s`) |

Reading: the general-compiled-code baseline (slice) is already within 12% of
its warm self, so the *compiler* is worth at most ~10% there. The one-shot and
the two app launches are 2-3x their own warm potential, and for the two apps
that potential is unreachable today by policy and by a loader bug.

## 3. Where a cold run's cycles go, per workload

Shares of all user cycles. `gcc empty.c` and slice rows are the recorded Q4
profile; Claude and code-server are new.

| Stage (self time, grouped) | `gcc -c empty.c` cold (recorded) | `claude --version` cold | code-server startup cold |
|---|---|---|---|
| POWERarm itself | 82.6% | 95.0% | 62.5% |
| translated guest code | 9.2% | 2.1% | 34.1% |
| backend `PPC64JITCore::CompileCode` self + `Op_*` handlers + `SpillStaticRegs` + high-zero walk | ~22% (15.6 + 2.0 + 1.9 + 1.7 + ...) | **27.5%** (18.3 + 2.6 + 1.6 + 1.2 + 1.2 + ...) | ~17% (11.1 + 1.9 + 1.1 + 1.0 + 0.9 + ...) |
| RA (`ConstrainedRAPass::Run` + `AssignReg`) | 11.9% | **16.4%** (13.4 + 3.0) | 10.8% (8.6 + 2.2) |
| frontend (A64 decoder, `IRBuilder::*`, `ContextImpl::CompileCode`, IR allocation) | ~7% (3.8 + 2.7 + ...) | ~15% (5.3 + 3.1 + 1.4 + 1.2 + 1.0 + 0.9 + 0.7 + 0.7 + ...) | ~8% (3.1 + 1.8 + 1.4 + 0.7 + 0.6 + ...) |
| IR passes (DFCE, compare-branch fusion, ScalarSplatChain, CFG) | ~6% (3.5 + 2.2 + ...) | **10.2%** (4.1 + 3.7 + 1.3 + 0.6 + 0.6) | ~5.5% (2.7 + 1.9 + 0.9) |
| block install and link (`ExitFunctionLinkWithRecord`, `FindBlock`, `AddBlockMapping`, `AddBlockLink`, `CompileBlock`, executable ranges) | ~6% | ~7.3% (2.7 + 1.5 + 1.2 + 0.9 + 0.6 + 0.4) | ~5% (1.9 + 1.2 + 0.7 + 0.8 + 0.6) |
| cache write (`SaveNewBlocks`, `AbsorbRelocations`, xxhash) | ~5% | ~2.4% | <1% |
| **translation total (frontend + passes + RA + backend)** | **~55-60%** | **~70%** | **~40-45%** |

Per-unit economics, from the JIT perf maps (`POWERARM_BLOCKJITNAMING=1`,
separate runs) and the cold-minus-warm user time:

| Process | compile units | host bytes | avg bytes/unit | cold-warm user | approx. per unit |
|---|---|---|---|---|---|
| `cc1` (`gcc -c empty.c`) | 13,664 (all rootfs files) | 10.8 MB | 792 | 144 ms | ~10 us |
| `claude --version` | 25,671 (1,455 rootfs libs, 24,216 in the ELF-but-anonymous) | 24.3 MB | 948 | n/a (never warm) | ~10 us if 60% of 440 ms |
| code-server main `node` | 123,177 (103,156 `node` ELF, 17,034 V8-generated, 2,987 rootfs) | 105 MB | 851 | 2.25 s (scope=all) | ~18 us |

So one compile unit costs 10-20 us for ~200-240 host instructions, i.e. on the
order of 1,000 cycles per guest instruction translated. That is the number a
cheaper tier would have to beat (section 6.3).

Deterministic baseline for future micro-work, `perf stat -e instructions:u`
over cold and warm `gcc -c empty.c` (CPU 41, one run each):
**cold 1,295 M instructions / 1,071 M cycles; warm 303 M / 294 M.** The cold
penalty is ~1.0 G instructions at IPC ~1.3. Use this, not the slice's wall
time, to gate anything in section 6.1-6.2 (X8's lesson: layout noise on the
slice is +/-0.25 s, larger than any per-op saving).

## 4. Findings the brief asked for

### 4.1 The RA's per-op work and its per-block reset (P1 row)

`ConstrainedRAPass::Run` (`IR/Passes/RegisterAllocationPass.cpp:584-838`) is
two walks per block: a backward walk that records SRA affinities and sets kill
bits (`:614-671`) and the forward allocation walk (`:680-812`). Per unit it
resizes three SSA-indexed vectors (`PreferredReg`, `SSAToReg`, `Seen`,
`:591-593`) and, in any block that spills, zero-fills `NextUses` over the
*whole unit's* SSA count (`CalculateNextUses`, `:270`), so a unit with several
spilling blocks pays O(blocks x SSA) fills (this is the 0.6%
`_M_fill_assign` in the Claude profile, shared with the backend's
`DynVRLiveIn.assign`).

`perf annotate` of `Run` on the Claude profile (no source lines in the Release
build; identified by disassembly): the single hottest instruction (7.6% of the
function) is the `Seen[Index]` bit load of the backward kill-bit pass
(`:658`, `Seen` is `fextl::vector<bool>`, so it is a `mulhwu`/`srwi` index
computation, a byte load and bit extraction); the next group (3.4%) is the
per-source pointer chase `IR->GetOp(Arg)->Op` in `IsValidArg` (`:160-167`),
then `FreeReg`/`SetReg` read-modify-writes of `Classes[].Available`
(`:187-194`, `:401-411`; `RegisterClassData` is 264 bytes so the class index is
a `mulli`). No single site dominates: the pass is a memory-bound walk of a
pointer-linked IR with three side tables indexed by SSA id. X8 already found
that per-op edits here are unmeasurable on the slice; they remain measurable
with `instructions:u` (section 3).

**The per-block reset** (`:607-610`, "At the start of each block, all
registers are available") is not a cold *cost*: it is what makes the RA cheap
(no live-in/live-out sets, no fixed point). It is a *warm* limiter, the P1(b)
blocker the checklist records. Giving the RA cross-block liveness would add a
backward dataflow walk plus edge fix-ups to every unit -- more cold cost, not
less. Two ways to keep cold cost flat: (a) leave the RA block-local and have the
*frontend* carry values across intra-unit edges through the existing SRA slots
(the P1 note's "force a spill at the edge": a `StoreRegister`/`LoadRegister`
pair at the edge is already free of RA changes); (b) only compute liveness for
units with a backedge (the spin-loop prefilter of X3 already finds those
cheaply). Either way P1(b) should be measured on warm workloads and is not part
of the cold budget. Recommendation: do not couple RA restructuring to cold work.

### 4.2 Backend emission hot paths

`PPC64JITCore::CompileCode` (`JIT/PPC64LE/JIT.cpp:4850-6274`) self time is
15.6% (gcc) / 18.3% (Claude) / 11.1% (code-server) and is the largest single
symbol everywhere. `perf annotate` plus disassembly of the hottest addresses in
the Claude profile:

- **10.0% of the function** (efcc0/efcc4: `lwzx`; `stw r3,120(r28)`) is the
  per-op `DynVRSpillMask = DynVRLiveIn[IRView->GetID(CodeNode).Value]`
  (`:5825-5827`), preceded by the `GetID` divide-by-16 (`mulhwu`). The vector
  is filled for *every* unit (`:5245-5247`), and for a unit without FPR work it
  is all zeros, so the load and store answer a question already known. About
  1.8% of the whole cold run, for one `if (UnitHasFPRWork)`.
- **~8%** (ec584-ec5a0; ef7a4-ef7c0; ec680): the per-op scaffolding of the
  emission loop and of the prepasses -- `IROp->Op` load, the `OP_LAST` range
  check, the `OpCacheFlags[Op]` byte table (`:5782`), the three cache
  invalidations (`:5788-5790`, `:5842-5844`, `:5853-5854`), and a per-op
  `switch` range test (`addi -91; cmplwi 61`) in a prepass walk
  (`Compute32MaskElision`/P6 const-exit prepass, `:5233`, `:5273-5308`).
- The handlers themselves are separate symbols and small: `Op_ExitFunction`
  1.2%, `Op_GuestOpcode` 1.2% (a `push_back` per guest instruction into
  `DebugData->GuestOpcodes`, later re-walked to encode the vl64pair RIP table
  at `:6208-6216`), `SpillStaticRegs` 1.6% (`PPC64Emitter.cpp:38-110`, ~40
  instructions emitted per shared exit stub; it is emitted once per unit via
  `SharedSpillExitLabel`/`SharedSpillLinkLabel`, `:6045-6063`, so its cost is
  the emit, not duplication), `InsertGuestRIPMove` 0.6%, `LoadImm64` 0.5%.
- `Emit32` (`CodeEmitter/PPC64LE/Emitter.h:161-169`) tests four constants per
  emitted instruction for the r0-dirty tracker; cheap, but it is on every one
  of the ~6 M host instructions emitted per Claude launch.

The emission loop is not dominated by codegen; it is dominated by bookkeeping
per IR op. Nothing here is a 10% lever on its own; together (4.2 + 4.1 + 4.6)
they are the "5-7% of a one-shot cold run" item in the ranking.

### 4.3 Does this JIT suit a cheap baseline tier?

Evidence for: cold code is mostly executed once (the slice's warm run has 0.15%
of samples in POWERarm, so every unit compiled cold ran few times), and a unit
costs ~1,000 cycles per guest instruction to translate, ~5-10x what a template
JIT pays. Evidence against: (1) after the cache policy fix (section 7) the
only code that is cold on *every* launch is runtime-generated code, and that is
14% of code-server's units (17,034 of 123,177; ~19 MB host code, ~0.2-0.3 s per
launch) and, judging by `--version`, a small share of Claude's; (2) `FEX_O0`
(passes off, RA still on) saves only 2.6% of `cc1`'s cold user time and 5% of
Claude's (measured: 194.7 vs 199.9 ms; 407 vs 431 ms), so skipping the
optimisation passes is not the cheap tier -- the RA and the backend are, and
this pipeline cannot emit without both; (3) the swap machinery
(`CODE-CACHE.md:246-256`: delink every inbound link, return-stack and BR
compare-cache entry under the exclusive lock while guest threads run) is the
same correctness surface as SMC invalidation, and a tier that never re-tiers
regresses steady state. Verdict: not now. Revisit only when, after section 7
lands, a cold profile of a JS-engine app still shows translation of anonymous
code as a top item; the numbers to collect first are the execution-count
distribution of anonymous units (a counter in the entry poke) and the fraction
that V8/JSC flushes before reuse.

### 4.4 Parallel or background translation

What exists: multi-threaded guests already translate concurrently -- in the
code-server profile `V8Worker` threads carry 28% of all samples inside
POWERarm against `MainThread`'s 32%. The frontend, passes and RA run outside
any global lock (`Core.cpp:1037` runs `PassManager->Run` inside `GenerateIR`,
before the backend), but **emission is serialised**: the PPC64 backend emits
straight into the shared code buffer under `CodeBufferWriteMutex` for the whole
emission window (`JIT.cpp:4865-4897`), unlike the arm64 backend's per-thread
staging buffer. The race-to-compile path already exists
(`Core.cpp:1078-1090`: a second thread that finds the block after its own
frontend run discards its IR). Background *install* of cached blocks was
measured slower (C12; `CODE-CACHE.md:234-244`) because it contended with the
guest thread on lookup-cache and code-buffer writes for cheap per-block work.

For single-threaded cold work (`cc1`, the Claude main thread, the code-server
main thread) the proposal is **translate-ahead**: after a unit is compiled, its
constant exit targets that are not yet in the lookup cache (the decoder already
knows them: `Decoder.cpp:235-250`, and the thunk records carry `GuestRIP`) go on
a queue serviced by one helper thread on a non-sibling core, which runs the
same `CompileBlock` path as a V8 worker would and publishes into the shared
map; the guest thread's next miss then hits the "raced" path. The compile work
is 10-20 us per unit and the install is 1-3 us, so the C12 contention ratio is
inverted. Requirements: a staging buffer so the helper's emission does not hold
the guest thread's emission lock (that is the arm64 model, and also what a
second guest thread needs today); a bound on speculation depth (both arms of a
conditional exit, one call target, one return point); saving speculative units
to the cache is harmless (already-validated by hash) but inflates it, so mark
them and save only on first execution if the cache size matters.
Expected: hides up to the fraction of translation that prediction gets right;
constant exits are the large majority of cold exits (P12's census: 370k
constant exit sites in `cc1 lvm.c`). Risk: medium-high (SMC invalidation while
a helper holds a unit: the shared `CodeInvalidationMutex` discipline in
`CompileBlock` already covers a compiling thread; memory and cache growth;
helper placement vs. SMT siblings, P11). Effort: high. This is the only lever
that can make the one-shot case approach its warm number without a cache.

### 4.5 Block formation size vs compile cost

Region formation (`Decoder.cpp:139-157`: 128-byte window, 8 leaders, linear
runs bounded only by `MaxInst` = 5000, `Config.json.in:12-18`) is on the right
side of the trade already. Measured on `gcc -c empty.c` cold/warm `cc1` user
time: default 200/56 ms; `POWERARM_MULTIBLOCK=0` 227/82 ms (+14% cold, +48%
warm, 31 vs 26 MB of cache -- single blocks re-translate entry/exit overhead);
`POWERARM_MAXINST=500` 200/59 ms (no unit hits the cap). On Claude `--version`:
default 431 ms, `MAXINST=500` 432 ms. The decoder's own table (`:147-153`)
shows 64 bytes/8 leaders equal or better than 128/8 on every column, inside
noise. Duplicate translation of blocks entered both from inside a region and
from outside (`:145-147`) is real but cannot be sized from the perf map (it
carries host sizes only); it would need the cache index's `GuestLength` per
entry summed against distinct guest bytes -- worth one census if 4.4 is
pursued, since translate-ahead multiplies whatever duplication exists. No cold
lever here.

### 4.6 The remaining IR-pass walks

DFCE 3.5-4.1%, compare-branch fusion 2.2-3.7%, ScalarSplatChain 0-1.3%
(`RedundantFlagCalculationElimination.cpp:786-854`, `:623-784`;
`CompareBranchFusion.cpp:197-329`). X6 measured that merging walks buys nothing
because the per-op bodies (`ClassifyFast`, `GetUses`, the fusion state machine)
are the cost. What has not been tried is **not walking**: the frontend knows,
as it emits, whether a block contains any NZCV writer or reader (every
`*WithFlags`/`*NZCV`/`StoreNZCV` goes through `IRBuilder`), so a per-block bit
would let DFCE skip `ProcessBlock` and fusion skip its walk for flag-free
blocks and still be exact (DFCE's cross-block `FlagsRead` for a flag-free block
is simply its successors' union, `:641-648`). X2 is the same idea per unit for
ScalarSplatChain and cut that pass 3.3x. Expected: proportional to the share
of flag-free blocks (GCC-built code: roughly half the blocks; JS engines
similar), so 3-5% of a one-shot cold run. Risk: low (a conservative bit; the
three-mode A64Frontend suite is the gate). Effort: a day.

### 4.7 AOT and cache policy for real apps

This is the section with the numbers, and the reason the ranking below leads
with policy:

1. **Scope.** `CodeCacheScope` defaults to `rootfs` (`Config.json.in:78-90`,
   `SyscallsSMCTracking.cpp:1951-1976`): only files under the RootFS are cached.
   Both monolithic targets live under `$HOME` (`~/.local/share/claude/versions/`,
   `~/.cache/powerarm/code-server/`), so their own text -- 84% of code-server's
   units -- is compiled on every launch. `CodeCacheScope=all` exists and works
   for `node`: first response 4.47 -> 3.84 s (writing 172 MB) -> **2.35 s**
   warm (`node` user 3.85 -> 1.60 s; 99,869 blocks loaded in `lookup-ms 286`).
2. **The Claude binary cannot be cached under any scope, and pays 50 ms to
   map.** Its PT_LOADs are 64K-aligned (`p_align 0x10000`) and 64K-*congruent*
   (text `p_offset 0x1581c00` / `p_vaddr 0x1791c00`, data `0x51e35e0` /
   `0x54035e0`), but `MapFile` computes `off = p_offset - PAGE_OFFSET(p_vaddr)`
   at the guest's 4K page (`ELFCodeLoader.h:244-246`), which is 4K- but not
   64K-aligned, so `RequiresFallback` (`HostPageMapping.h:51-71`,
   `!IsAligned(Offset)`) sends 209 MB of text and data through the anon+`pread`
   fallback (`MapFileFallback`, `:120-165`). Consequences, all measured on
   `--version`: the `map` phase is 49-51 ms (10% of the launch; `node`, whose
   text starts at offset 0, maps in 0.44 ms); the text is an anonymous VMA, so
   `LookupExecutableFileSection` returns nothing (`SyscallsSMCTracking.cpp:
   1698-1706`, `!Resource->MappedFile`), `TryLoadBlock` returns before
   counting (`CodeCache.cpp:1331-1338`), the JIT perf map names 24,216 of
   25,671 units `JIT_0x1791c00_...` instead of by file, and warm == cold
   (`--version` 431 vs 408 ms guest; `--help` 1346 vs 1360 ms). AOT is
   impossible for the same reason. Every Claude process also carries 209 MB
   of private anonymous memory instead of page-cache-shared file pages. The
   fix is the kernel's own rule for 64K-aligned ELFs: when
   `p_vaddr = p_offset (mod host page)`, map from the host-rounded address and
   offset (the extra head bytes are the same file's bytes; the shared-page
   protection union already exists for the tail, `:266-275`, and for the
   fallback, `:138-150`). The fallback then only remains for genuinely
   4K-congruent binaries.
3. **AOT for apps.** `aot-translate.sh` (`Scripts/powerarm/aot-translate.sh`)
   and `TranslateMainElf` (`AOTGenerator.cpp:278-356`: ELF entry, `.eh_frame_hdr`
   FDEs, `.symtab`/`.dynsym` functions, optionally call return points and far
   branch targets) already do the right thing for any ELF the cache can name.
   Q4-A1 found mode `entries` the right cost/size point (0.14 s, 28 MiB for the
   11 build tools; `cc1` in mode `entries` 87 MiB). For code-server the runtime
   cache after one launch is 172 MB for 87k blocks; mode `entries` over `node`
   would be smaller and would make the *first* launch behave like the 2.35 s
   warm one plus install. The hook is at install time (the Claude installer
   writes a new `versions/<v>` file; code-server is a tarball), i.e. an
   `aot-translate.sh` call from the port's packaging, not from the emulator.
4. **What the cache costs a one-shot.** The `save` phase is 30 ms of `cc1`'s
   217 ms (14%) and 4 ms of `as`'s 42 ms; on the slice it is 0.34 s of the
   2.42 s cold penalty (recorded); Claude 12 ms of 500; code-server 6-16 ms of
   thousands. C11 rejected an async writer because a thread cannot outlive
   `exit_group` and the build-wide share was 1.6%; for one-shots the share is
   ten times that, and a `fork()`ed writer after `TM.Stop`
   (`FEXInterpreter.cpp:862-873`) does outlive it, at C17's 1.4 ms per fork of a
   large process. `MinNewBlocksPerSegment` = 8 (`CodeCache.cpp:824`) already
   keeps trivial processes from writing.
5. **Install cost on the warm side** is the other half of the same policy:
   `TryLoadBlock` is already lazy per block (`CodeCache.cpp:1326-1460`: index
   probe, executable-range check, page registration, `XXH3` of the guest bytes,
   `memcpy`, relocations), and the `lookup-ms` counter shows 2.9 us per block
   (286 ms for 99,869 blocks in code-server warm, 18% of its 1.6 s user). Q2
   (queued) is the right follow-up and is not a cold item.

## 5. Ranked goals

Impact is given as movement of the "x the Pi" ratio on each reference workload,
cold. Measured numbers are marked; the rest are estimates from the profile
shares in section 3.

| # | Goal | gcc -c empty.c (20.6x) | Claude `--version` (50x) | code-server start (8.8x) | slice cold (3.8x) | Risk | Effort |
|---|---|---|---|---|---|---|---|
| **G1** | **Cache and AOT the apps**: (a) loader maps 64K-congruent segments as file mappings; (b) `CodeCacheScope` default covers user-installed apps (`all`, or rootfs + `$HOME`); (c) `aot-translate.sh -m entries` at app install | none (rootfs already cached) | 50x -> ~20x est. (map -50 ms measured; 24k ELF units become cacheable; JSC-generated code stays) | **8.8x -> 4.6x measured** (second launch); first launch ~5x with AOT | none | low: cache validates guest bytes; loader change is in the 64K fallback path only | loader 1-2 days; policy trivial; AOT hook in packaging |
| **G2** | **Translate-ahead helper thread** for single-threaded cold work (4.4), with a per-thread staging buffer so emission stops serialising | 20.6x -> ~12-14x est. (hides up to half of the 60% translation share) | -20-30% of what remains after G1 | main thread's 32% translation share partly hidden; V8 workers already parallel | 3.8x -> ~3.5x (cold approaches warm) | medium-high: concurrency with invalidation, cache growth, sibling placement | high (weeks) |
| **G3** | **Per-op cost cuts, gated on `instructions:u`**: skip `DynVRLiveIn` for non-FPR units (1.8%); per-block flag-free bit to skip DFCE/fusion (3-5%); hold the `CodeInvalidationMutex` guard once in `ExitFunctionLinkWithRecord` (`JIT.cpp:1752-1775`; 2.7-3.5%) and a lock-free `FindBlock` fast path (1.5-2%); RA side tables as one SSA-indexed struct and `Seen` as bytes (2-3%); `Op_GuestOpcode`/vl64pair encoded directly (1%) | 20.6x -> ~19x (5-7% of the run) | ~5% | ~4% | ~0.7% (translation is 8% of the slice) | low; each item is a contained edit with the three-mode suite as gate | days, one item at a time |
| **G4** | **Cache write off the exit path**: `fork()` writer after `TM.Stop`, parent exits; or at least skip the segment sweep on the one-shot path | 20.6x -> ~18.5x (`cc1` save 30 ms of 217) | -2% | ~0 | -1.4% (0.34 s recorded) | low-medium: the child must only write; `check-code-cache.sh` is the gate | small-medium |
| **G5** | **Cheap baseline tier** for runtime-generated code (4.3) | none (all file-backed) | JSC-generated share only | V8-generated units are 14% (~0.2-0.3 s per launch) | none | high (delink/swap surface, steady-state regression without re-tiering) | very high; **defer** until a post-G1 profile of a JS app shows anonymous-code translation on top |

Not levers (measured or already closed): block formation (4.5), IR-walk merging
(X6), `FEX_O0` (2.6-5%), RA cross-block liveness for cold (4.1), lower
`MaxInst` (no effect).

Weighing across the set: G1 is the only goal that changes an app-launch ratio
by 2x, and it helps every future non-rootfs binary (Electron proper, games)
the same way; it does nothing for the slice, which is why it did not show up in
build-centric X/Q4 work. G2 is the general lever for the one-shot and cold-build
cases and the only one that can approach the warm number without a cache. G3
and G4 are the compounding small wins; do them under a deterministic gate.
Nothing in the list helps only one runtime except G5, which is why it is last.

## 6. Gates

- **G1(a) loader:** `POWERARM_STARTUPTIMES=1 claude --version` shows `map`
  under 1 ms; `POWERARM_CODECACHESTATS=1` prints a line for the Claude ELF with
  `loaded > 0` on the second run; a JIT perf map of `--version` has no
  `JIT_0x1791c00_...` names; RSS of a Claude process drops by ~200 MB;
  a64diff `programs`/`rootfs` and the three-mode A64Frontend suite unchanged;
  `check-code-cache.sh` passes.
- **G1(b,c) policy/AOT:** code-server first-response cold and warm (today
  4.47 / 4.44 s; expect ~3.8 / 2.35 s, and ~2.5 s first launch after AOT);
  `claude --version` cold and warm; slice cold/warm unchanged; cache directory
  size under the 2 GiB cap after both apps.
- **G2:** guest-thread user CPU (`RUSAGE_THREAD`) and wall for cold
  `gcc -c empty.c` and the cold slice, against warm; `perf` share of
  `CompileCode` on the guest thread; the full gates (three modes, a64diff on
  64k and 4k-kvm, `check-code-cache.sh`, the litmus tests) because it adds a
  compiling thread to every process.
- **G3:** `perf stat -e instructions:u` over cold `gcc -c empty.c` (1,295 M
  today) per item, plus the three-mode suite; no wall-time claims.
- **G4:** `gcc -c empty.c` cold wall and `cc1` `save` phase (30 ms today);
  `check-code-cache.sh` including the parallel and forged-segment checks;
  the slice cold.

## 7. Measurements taken (all new, one run each unless noted)

POWER9, CPUs 40-47, contended (another agent building on 88-160):

| Run | Result |
|---|---|
| `gcc -c empty.c` cold / warm, CPU 40 | wall 0.35 / 0.12 s; `cc1` guest 185 / 58 ms, save 29.6 / 0 ms, user 200 / 56 ms; `as` 34.5 / 15.6 ms; driver 45 / 13 ms user; cache 26 MB |
| same, `POWERARM_O0=1` | `cc1` user 194.7 / 51.7 ms |
| same, `POWERARM_MAXINST=500` | `cc1` user 200.1 / 59.1 ms |
| same, `POWERARM_MULTIBLOCK=0` | `cc1` user 227.2 / 82.3 ms; cache 31 MB |
| `perf stat` cold / warm, CPU 41 | 1,295 M / 303 M instructions:u; 1,071 M / 294 M cycles:u |
| `claude --version` cold / warm, CPU 44 | main 476 / 468 ms; map 31-50 / 49 ms; guest 431 / 409 ms; save 12 / 8 ms; user 410 / 398 ms; cache 2.7 MB (libc, libm, ld.so only) |
| same, `CodeCacheScope=all` cold / warm / warm | guest 434 / 406 / 415 ms; still no cache file for the Claude ELF |
| same, `O0` / `MAXINST=500` | guest 407 / 432 ms |
| `claude --help` cold / warm (scope=all) | guest 1346 / 1360 ms (21 KB of help text) |
| `claude --version` perf, 5 kHz | POWERarm 95.0%, JIT 2.1%, libc 1.6%; symbols in section 3 |
| `claude --version` block census | 25,671 units, 24.3 MB; 1,455 rootfs-lib, 24,216 anonymous (ELF text) |
| code-server cold / warm, default scope, CPUs 40-43 | first `/healthz` 4.47 / 4.44 s; `/` 1.28 / 1.25 s; `node` user 3.65 / 3.64 s; cache 8 MB |
| code-server `CodeCacheScope=all` cold / warm | 3.84 / **2.35** s; `/` 1.40 / 1.17 s; `node` user 3.85 / 1.60 s; `node` cache 172 MB (86,896 blocks saved, save-ms 259; warm loaded 99,869, lookup-ms 286) |
| code-server perf, 2 kHz (two `node` processes) | POWERarm 62.5% (MainThread 32%, V8Worker 28%), JIT 34%; symbols in section 3 |
| code-server block census, main `node` | 123,177 units, 105 MB; 103,156 `node` ELF, 17,034 anonymous, 2,987 rootfs |

Pi 5 (`pi5`, load ~1.3, single job): `claude 2.1.275 --version` 0.010-0.011 s
(3 runs; first run 0.095 s with a cold page cache); `gcc -c empty.c` 0.017 s
(3 runs); slice 6.31 s wall / 6.01 s user (1 run); code-server first `/healthz`
0.51 s, `/` 0.35 s (2 runs).

Not measured, deliberately: the slice and `gcc -c empty.c` cold profiles (Q4
has them); any A/B of G3-style edits (they need the instruction-count gate, not
a research run); `cc1 lvm.c` (warm workload).
