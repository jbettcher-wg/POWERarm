# POWERarm optimization checklist

This is the shared list of measured optimizations from the three research reports. **Read it
before you touch any lowering, translator, allocator or dispatcher code.** If your work passes
through the code of an item that's `not touched`, claim it and do it in the same pass. It's cheaper than
coming back later with a cold agent.

Sources:
- **[PIPE]** `research/pipeline/POWER9-VS-A76-PIPELINE.md`
- **[FP]** `research/scalar-fp/SCALAR-FP-LOWERING.md`
- **[NEON]** `research/neon/NEON-LOWERINGS.md`

The section references in the tables point into these files. **The research doc is the spec**:
exact instruction sequences, parity corpora and costs are there, not here.

## How to use it

1. Before editing a path in the **Touches** column, check the item's **Status**.
2. **Claim** the item by setting Status to `in progress <branch> <date>`, and commit that edit
   on your branch together with the work (or on its own).
3. **Implement it in its own commit**, separate from any functional fix, so it can be bisected
   and reverted.
4. **Gates, all required:**
   - the differential test for that path passes before and after (A64Frontend, a64diff and
     any new generated cases, against Pi goldens);
   - both kernels: 64K host and `4k-kvm`;
   - every ISA 3.0 instruction sits behind `SupportsISA30` (host `AT_HWCAP2` →
     `PPC_FEATURE2_ARCH_3_00`) with a POWER8 path, and the tests also pass with
     `POWERARM_HOSTFEATURES=disableisa30`.
5. **Mark it done** by setting Status to `done <commit>` and adding the measured result
   (instruction count, cycles or benchmark delta) in **Result**.
6. Don't claim items your work doesn't touch; the dedicated optimization phase picks those up.

Status values: `not touched`, `in progress <branch> <date>`, `done <commit>`.
An item dropped on measurement stays `done`, with the reason in **Result**.

## B: known backend bugs (correctness first)

| ID | Item | Spec | Touches | ISA | Status | Result |
|---|---|---|---|---|---|---|
| B1 | `VFNMLA`/`VFNMLS`/`xsnm*`/`xvnm*` negate after rounding: negate the multiplicand, never the result | FP §10, §6.2; NEON §5 | `JIT/PPC64LE/VectorOps.cpp` ~3747/3767, FMA frontend | 2.06 | done df0da97a9 | `xvnegdp t,V1` then the plain `xvmaddadp`/`xvmsubadp` with T = Add, in the four vector DEF_OPs and in `DEF_FMA_SCALAR_INSERT` (new `NEGA` flag, negate into VTMP2, no extra pressure). `Emitter.h` gains `EmitXX2VSX` plus vs0-vs63 `xvnegsp`/`xvnegdp`. Not reachable from the A64 frontend (it pre-negates FMA operands and uses VFMLA/VFMLS), so no POWERarm-visible change: A64Frontend 53/53 before and after, slice 21.8-23.8 s (unchanged, integer workload). fastppcx86 patch `outgoing-patches/fastppcx86/0031-*`. |
| B2 | `Float_FromGPR_S` rounds i64→f32 twice | FP §10 | `VectorOps.cpp` ~4557 | 2.07 | done 0424fac91 | `fcfid; frsp` → `fcfids`, one instruction and one rounding. Also not reachable from A64 (the frontend has its own `A64FloatFromGPR`, already single-rounding). fastppcx86 patch `0032-*`. |
| B3 | 32-bit `VAddV` uses saturating `vsumsws` (also sets VSCR.SAT) | NEON §5 | `VectorOps.cpp` ~586 | 2.03 | done 0575dcc1a | modular `vsldoi/vadduwm` fold (5 instructions). The A64 frontend had open-coded the same fold at two sites to dodge the bug (`TranslateSIMD` ADDV, `TranslateSIMDSaturate` widening across-lane adds); both go back to `_VAddV`, 4 IR ops fewer each for the same host sequence, which also puts the backend arm under the `simd_arith`/`simd_sat` Pi goldens. fastppcx86 patch `0033-*`. |

Also check whether each B item affects fastppcx86. If it does, add a patch under
`outgoing-patches/fastppcx86/`.

All three are in `outgoing-patches/fastppcx86/0031-0033` (applied and committed on a
`daedalao-wt` worktree, so they are that tree's own commits, not rewrites of the POWERarm ones —
B3 there is the backend hunk only, since fastppcx86 has no A64 frontend).

**None of B1-B3 was reachable from an A64 guest.** The frontend had already routed around each:
FMA operands are pre-negated into `VFMLA`/`VFMLS`, `SCVTF` uses `A64FloatFromGPR`, and both
across-lane adders open-coded the modular fold. `unittests/A64Frontend/fpbugs.S` (commit
5b96f1396, 134 lines, Pi golden) enumerates the discriminating cases anyway — FMA sign-of-zero
over every operand sign, the directed rounding modes, NaN sign and payload, the i64→f32 rounding
witnesses and overflowing ADDV word vectors — and passes both before and after the fixes. It is
the test that fails if a later change routes an A64 op onto a shorter host form without the
matching semantics, and it is the reason B3's frontend workaround could be removed with the
existing `simd_arith`/`simd_sat` goldens as the gate.

**Not started, and why it is more than an afternoon:** F1-F6 and N5 all need the same new backend
mechanism — a NaN pre-check that branches to an out-of-line cold block. The frontend cannot
express it (splitting an IR block per FP op is worse than the fix-up it replaces), so it wants a
fused arithmetic IR op per family whose backend lowering emits `xvcmpeqdp. / bc` over a local
label, plus a shared cold routine per rule (binary, FMA with Inf x 0, min/max). The frontend side
is then deleting `PropagateNaNOperand` and `FPMinMax` in favour of those ops. F4 (`xxpermdi`
zero-upper in `VMov` i64), F5 (CR0-direct FCMP consumers, branch-free FCSEL) and N1 (the
`CMEQ→UMAXP→FMOV→CBZ` fusion into `CondJump{VCmpElementSize}`) are independent of that mechanism
and are the cheapest next three.

## P: pipeline and code shape (largest multipliers)

| ID | Item | Spec | Touches | Expected impact | Status | Result |
|---|---|---|---|---|---|---|
| P1 | Keep non-pinned guest GPRs in host registers across a block (block-local/global allocation); no same-slot `std;ld` reloads | PIPE §6 Rule 1, §5.1 | register allocator, `a64::SRA`, `JIT.cpp` | crc32 8.1× → ~3× | done d0ad3e55c, part (a) only | (a) done: the A64 IR builder reuses the last stored or loaded SSA value of a context-backed GPR for a load within 8 guest instructions in the same IR block; every store stays in place, so CPUState is exact at each instruction boundary. A64Bench warm on CPU 104 (window sweep, 1 run): crc32 1450 to 402 ms (7.35x to 2.04x the Pi), sha256 1428 to 960, vm 2620 to 1742, sort 1049 to 970; wider windows lose on vm (1905 at 32) as the reused values get spilled. `gcc -O2 -c lvm.c` 9.75 to 9.46 s (-3%) on top of P10; zlib and Lua builds within noise. Register census for cc1 (guest register references weighted by block samples): the pinned set covers 94.4% (X0 25%, X1 14%, SP 11%, X2 9%); the best context-backed register is X25 at 1.1%, so re-pinning X8-X17 would not help GCC-built code and was not changed. Not done: (b) block-local allocation of context-backed registers with load at entry/write at exit needs precise state at faulting memory ops (a guest SIGSEGV handler may resume), i.e. per-fault-site recovery data or flushing before every guest load/store; cc1 shows about 3% of JIT samples on context GPR traffic, so it is not the build's bottleneck. **2026-09-17 (powerarm-q3/compute-backend), the real blocker, measured and read out of the allocator**: even the cheap restricted form -- extending the frontend's GPR value cache (P1(a), `IRBuilder::LoadGPRSlot`) across block edges inside a compile unit, keeping every store so CPUState stays exact and no fault recovery data is needed -- cannot work as things stand, because `ConstrainedRAPass::Run` resets `Class.Available = (1u << Class.Count) - 1` at the top of every block ("At the start of each block, all registers are available") and restarts spill bookkeeping per block. No non-fixed SSA value may be live across a block boundary; only the pinned GPRFixed/SRA slots cross. So P1(b) in ANY form is gated on giving RA cross-block liveness (live-in/live-out sets, or forcing a spill at the edge), not on fault recovery. Do that first or leave the row alone |
| P2 | Inline compare cache before the L1 lookup `bctr` for guest `BR` | PIPE Rule 2, §4.5, §5.2 | dispatcher, `BranchOps.cpp`, A64 `BR` | up to ~30% of interpreter-style loops | done 854893dc4 | 8 chained slots per `BR` site, each a guarded direct `b` filled by the record linker; an empty slot is filled by a sampled arrival (time-base low 6 bits zero), not the first one: first-come filling gave vm's eight slots to its setup opcodes and made it slower (4 slots 4500 ms, 8 slots 3410 ms per rep). vm 3303→2149 ms (−35% vs baseline, 2 of its ~15 opcodes still reach the probe); vm slots 0/4/6/8 (sampled): 3294/3056/2336/2258 ms. cc1 unchanged (10.69→10.67 s; 7.9M `BR` in the run); zlib 123.1→124.2 s, Lua 107.5→108.1 s (+0.9%/+0.6%, likely the extra ~1 KB of slots and thunks per `BR` site); A64Bench geomean 0.995→1.083. `POWERARM_BRCACHESLOTS=0..8` |
| P3 | Pair guest `BL`/`BLR` with LK=1 host branches and guest `RET` with `blr`; budget link stack 64 (16 under SMT4) | PIPE Rule 3, §4.4 | `BranchOps.cpp`, frontend `BranchHint` | +25.5 cycles per unpaired return avoided | done 26dc5f8c2 | Frontend hints 5a4f52c67. A64 layout (`EmitA64PairedCall`): the call pushes {X30, &trampoline} and ends in `bl`/`bctrl`; the trampoline is a link-first constant exit to the return address in the caller's unit; RET pops without a bounds check (guard-page fault resets the stack pointer register) and `blr`s on a match, else probes. Shadow stack now on by default. cc1 `lvm.c`: RET count-cache mispredicts gone (ccache mispredicts 55M→8M, lstack predictions 45M→192M with 1.8M mispredicted), cycles 41.6G→39.4G, 11.24→10.69 s (−4.9%); zlib 126.9→123.1 s (−3.0%); Lua 111.4→107.5 s (−3.5%). A64Bench vs baseline: vm 3284 (layout-sensitive, see P12), sort 1056 (link-first alone 1035: the pairing costs ~16 instructions per call/return pair where the count cache already predicted), bst 565, geomean 0.995. Tests: `unittests/A64Frontend/callret.c` (recursion to 120000, longjmp out of 300-deep recursion, RETs to other addresses, `ret x1`, `br x30`, frame skipping, 300000 unpaired RETs, tail calls, handlers making calls, ucontext switching, self-modifying caller) against the Pi golden in all three modes; both guard-page resets observed firing |
| P4 | At most one count-cache branch per aligned 32-byte block; emit `mtctr` as early as possible | PIPE Rule 4 | block emitter, dispatcher | +16 cycles and a guaranteed mispredict avoided | done 26dc5f8c2 (audit; the one change is in P3) | Audited after P12/P3/P2: a block exit now emits at most one count-cache branch (the probe `bctr` of a `BR`/`BLR`/mismatched `RET`); constant exits emit none (link-first) and a paired `RET` uses `blr`. `mtctr` follows the HostCode load directly, before `std rip` and the r0 re-zero, in every probe; the one change is the `RET` pop, which now moves the trampoline into LR before the guest-address compare (in the P3 commit). Remaining shared blocks: in 1 of 4 far-link thunks the `bctr` at +0x10 and the unlinked `bctr` at +0x28 share a 32-byte block, but only one of them ever executes. Verified: monomorphic workloads count-cache-mispredict ~0 (crc32 4965, bst 4874 per run) |
| P5 | Flags in CR fields; never read XER on hot paths; never clear XER.SO | PIPE Rule 5, §4.2; FP §13 | `ALUOps.cpp`, flag lowering | +4…+11 cycles per XER read; 147 cycles per SO toggle | done 2db45e763, 2f8013325, 1cb25df75, eaf03c95d | Warm G2 compare-branch/select fusion (`CompareBranchFusion`) fuses A64 `SubNZCV`, `AddNZCV`, `SubWithFlags`, and `AddWithFlags` (CMP, CMN, SUBS, ADDS at W and X sizes) into `CondJump` (`B.cond`) and `NZCVSelect` (`CSEL`/`CSINC`/`CSINV`/`CSNEG`/`CSET`) directly testing `cr7`, bypassing `subfco.` pre-shifts and XER reads. F5 directly reads CR0 for `FCmp` consumers in `MapNZCVCC` (`FlagsFromFCmp`), bypassing `ProjectXERToCR1`. XER.SO is never cleared by the JIT. `cc1`: instructions:u 21.90 -> 21.25 G (-3.0%), cycles -4.5%, warm slice 20.95 s. |
| P6 | Block layout for fetch redirect: fall through on the common path, one taken branch per block | PIPE Rule 6 | block builder/layout | ~7-cycle loop floor; avoid extra taken branches | done 310ebcd2d | A guest conditional branch between two block exits (every `B.cond`/`CBZ`/`TBZ` the A64 frontend ends a unit with) was lowered `bc; b; b` into the taken and not-taken exit blocks: 2 taken host branches on the taken path, 3 on the other. `CondJump` now branches straight to the not-taken exit and falls into the taken one (1 and 2 taken branches, including the linked exit). Fallthrough elision (`FEX_FALLTHROUGH`) stays off: it was measured as a POWER8 regression and this shape does not depend on it. cc1 `lvm.c`: taken branches 4.43G→3.23G, 10.67→9.96 s (−6.7%); zlib 124.2→121.7 s (−2.0%); Lua 108.1→105.4 s (−2.5%); A64Bench geomean vs baseline 1.083→1.100 (vm 2030, sort 1044, bst 558). `POWERARM_NOEXITSHAPE=1` disables. Follow-up in powerarm-opt2/codeshape (617642fab, bd36bfe32): profiling `cc1 -O2 lvm.c` after P10 found the 29% of JIT samples on a unit's first instruction to be arrival cost, not entry work: linking forward exits past the entry poke (`ld`; `stb`) moved the samples to the next two instructions and left the slice unchanged (47.2/47.4 s), so that was reverted. The hot units were instead full of intra-unit hops: every in-unit B.cond successor went through a block holding only a Jump (`bc; b; b` chains), and CondJump/Jump emitted `b` to the next block. The frontend now targets in-unit successors directly, CondJump takes one `bc` to the forward leg and falls into the next block (islands keep the 14-bit displacement in reach; `POWERARM_NOSHORTCOND=1` restores the old shape), and a forward Jump to the next block emits nothing. Slice 47.2 to 41.1 s (successors) and cc1 cycles 29.31G to 26.46G, branches 5.01G to 3.90G (shape) |
| P7 | Lightest barrier/exclusive forms the memory model allows (`larx`/`stcx.` 74 vs 23 cycles; `sync` +42, `lwsync` +14) | PIPE Rule 8, §4.6 | exclusive monitor, atomics lowering | LSE/LL-SC heavy code | done for fences (d96394b3d, 147903303); trailing-sync formally rejected (35f48f28b) | **Done so far, V8 GC workload (Node 24, CPUs 100-107, warm): ~3.91 -> ~3.74 s mean, and barrier instructions executed per run ~79 M -> ~38 M.** Step 1, litmus harness: e69b5fb9d. Step 2, relaxed LSE RMWs drop their hwsync/isync bracket: d96394b3d. Step 3a, acquire fences (LDAR/LDAPR trailing, DMB ISHLD) are lwsync, not lwsync;isync: 147903303. **Census** (throwaway counting build, one run): LDAR 16.9 M (54%), CAS relaxed 9.8 M (31%), LSE RMW relaxed 2.1 M (7%), CAS release/acquire 1.8 M, LSE RMW release 0.4 M, STLR 0.19 M, DMB 5.7 k, LDXR/STXR ~2.5 k, LDAPR 0. **The trailing-sync question is settled: REJECTED (35f48f28b).** Moving LDAR's leading hwsync to the release side was checked formally with herd7 (`unittests/MemoryModel/check.sh`: Arm's aarch64.cat against ppc.cat over 474 generated litmus tests). It is unsound for the all-seq_cst IRIW -- STLR writers, LDAR;LDAR readers, which is what C++ seq_cst compiles to -- reproducing the published trailing-sync counterexample (Manerkar et al. 2016). LDAR keeps its leading hwsync. The same check shows today's lowering is sound for everything C/C++ promises; its six divergences are IRIW shapes with lwsync-separated reader loads, accepted and listed in `current.known`, and check.sh is now a regression gate against new ones. **Possible next step, not started:** acquire-only and release-only LSE RMWs (CAS release/acquire ~1.8 M per run) still carry the full hwsync/isync bracket; whether a release RMW can take lwsync needs the RMW (lwarx/stwcx.) translation added to the formal check first. Earlier plan and bound follow. **Original upper bound, RMW fences only:** a throwaway build that skipped the `hwsync`/`isync` bracket on all 11 atomic RMW lowerings (never committed) ran a GC-heavy V8 job (code-server's Node 24, 40x200k object allocations + JSON, CPUs 100-107, warm) in 3.68 s against 3.80-4.05 s fenced: **~5% at most**. It is an upper bound twice over: a correct lowering keeps the fences acquire/release ops need, and the bound excludes the `hwsync`s `DMB` and `LDAR`/`STLR` carry. **The same build aborted in 3 of 4 runs** (SIGABRT, ~21 s), so the fences are load-bearing for V8, not just paranoia. **Plan:** (1) litmus harness first (HANDOVER item 1), with that no-fence build as the positive control that must show forbidden outcomes; (2) honour A/L on LSE RMWs so only fully relaxed ops drop fences, which is safe under either convention; (3) then weigh leading-sync (current; see `LoadStoreAtomicWidth`) against trailing-sync for acquire/release, plus lighter `DMB` forms. Worth doing because it compounds across every app. |
| P8 | Constants: materialise off the critical path; load them when they're on it; reuse pinned zero | PIPE Rule 9; FP §3.3; NEON §2(b) | constant emission | per-op re-materialisation removed | done (subsumed by P5 & Warm G4) | Immediates are 6.4% of executed host instructions, but 1.05 `li` per `CMP imm` is owned and eliminated by P5 fusion (`cmpwi`/`cmpdi` imm16), 0.56 per logical reg op folded by Warm G4, ADRP delta cache (0.2 per `ADRP`), and `r0`-dirty tracking (`bd99fbf98`) avoids defensive zeroing at block exits. Not an independent lever (`WARM-CODEGEN-RESEARCH.md` §7). |
| P9 | Never let a wider load consume two narrower stores | PIPE Rule 10 | context layout, load/store lowering | +21 cycles and no forwarding avoided | done d0ad3e55c, dropped on measurement | The frontend writes every context GPR slot with one 64-bit store and reads it with one 64-bit load, so the JIT has no narrow-store/wide-load pair on guest state. `pm_cmplu_stall_lhs` for `gcc -O2 lvm.c`: 23.3 M under POWERarm, 20.0 M for native ppc64le gcc on the same input, so the stalls come from the guest program's own memory access pattern |
| P10 | Multi-block/region compilation (currently one guest block per compile) | PIPE §8; M1 TODO | frontend block formation, IR passes | enables P1 across block edges | done f90005f75 | The A64 decoder now grows a compile unit along B, B.cond, CBZ/CBNZ and TBZ/TBNZ targets within 128 bytes of the branch, up to 8 blocks (`POWERARM_MULTIBLOCK=0` or `POWERARM_MAXINST=1` restore one block). Profile first (`cc1 -O2 lvm.c`, CPU 104): context-backed GPR loads/stores held about 3% of JIT samples, but 29% landed on the first instruction of a block, the build had 4.3 G taken branches (4.8x native gcc) and 1.07 G L1 icache misses (18x), and translation was 43% of the Lua build's user cycles, so block transitions and compile cost ranked first. Region size was chosen on the builds: a 16 KiB window cost cold start 35% (`gcc -c empty.c` 0.41 to 0.55 s). Result on CPU 104 against f470d805f on CPU 104, 2 runs each: `gcc -O2 -c lvm.c` 12.32/12.32 to 9.75/9.70 s (-21%); zlib 134.0/132.9 to 123.5/123.2 s (-7.6%); Lua 127.1 (outlier)/118.9 to 106.8/107.3 s (-10%). A64Bench warm (CPU 104, 3x3): vm 3311 to 2640 ms (7.73x to 6.17x the Pi), crc32 unchanged (1450), sha256 +1.1%, bst +2.2%, sort +2.7% |
| P11 | Run one guest thread per core with SMT siblings idle (scheduling/pinning policy, docs) | PIPE Rule 7, §4.7 | launcher/config, docs | vm 1.9× slower with a busy sibling | not touched | |
| P12 | Link constant exits on their first execution: an unlinked exit goes to the record linker instead of running the inline L1 probe | PIPE §5.2 (probe chain); census below | `BranchOps.cpp` ExitFunction | 197M probed constant exits in `cc1 -O2 lvm.c` | done 309288eb5 | Branch census of `cc1 -O2 lvm.c`: 162k of 370k constant exit sites were never linked (their target was already in L1, so the probe hit and the linker never ran) and executed 197M times, 12% of all exits. cc1 12.42→11.24 s (−9.5%); zlib 134.5→126.9 s (−5.6%); Lua 118.9→111.4 s (−6.3%) (CPU 108, 2 runs each, spread <0.5%). A64Bench (CPU 108, 3 runs): vm 3303→3836 ms (+16%): 180M fewer predictable probe `bctr` changed the count cache's global history and vm's dispatch `br` mispredicts rose 46M→74M; bst 544→560, sort 1062→1035, crc32/sha256 flat; geomean 0.97. P2 is the fix for vm. `POWERARM_NOLINKFIRST=1` restores probe-first |

## F: scalar floating point

| ID | Item | Spec | Touches | Expected impact | Status | Result |
|---|---|---|---|---|---|---|
| F1 | NaN precedence: `xscmpudp` + `bso` cold pre-check; the cold path swaps operands only for op1 qNaN + op2 sNaN | FP §3.2–3.4, §11 | `A64Frontend/TranslateFP.cpp`, FP lowering | 4 adds: 45→13 instructions, 18.4→6.6 cycles | done 603741288 | `A64FArith` and FP cold-block infrastructure landed. Result post-check (`xvcmpeq{dp,sp}.` + `bc` to out-of-line `NaNFix` stub) replaces unconditional `PropagateNaNOperand`. Executed path per guest FP arithmetic instruction drops by 20 host instructions. Slice: warm flat, cold 23.90 -> 24.58 s (+2.8% because shared `NaNFix` body does not amortize in single-FP-op units of C builds). |
| F2 | FMIN/FMAX(NM) → `xvmaxdp`/`xvmindp` + the F1 pre-check (not `xsmaxjdp`/`cdp`) | FP §6.1 | FP translator | 40 IR ops → 3 | done | `A64FMinMax` lowers FMIN/FMAX/FMINNM/FMAXNM to fast-path `xvmaxdp`/`xvmindp`/`xvmaxsp`/`xvminsp` with pre-check `xvcmpeq{dp,sp}.` branching to out-of-line `NaNFix` / `NMPrep` cold stubs. Replaces 40-IR-op fallback chain. Gate 1: 86/86 (default + disableisa30); Gate 2: 24/24; Gate 3: 0 required fail. |
| F3 | FMA via `xsmaddadp`/`xsmsubadp` with `xsnegdp` on the multiplicand + NaN check (see B1) | FP §6.2 | FP translator, `VectorOps.cpp` | exact, fewer instructions | done | `A64FMulAdd` lowers FMADD/FMSUB/FNMADD/FNMSUB and NEON FMLA/FMLS to fast-path `xvmaddadp`/`xvmaddasp` with post-check `xvcmpeq{dp,sp}.` branching to out-of-line `FMAFix` cold stub. Operand negations pre-applied. Replaces 47-IR-op fallback chain. Gate 1: 86/86 (default + disableisa30); Gate 2: 24/24; Gate 3: 0 required fail. |
| F4 | Zero-upper via one `xxpermdi` against pinned zero (or elide) | FP §3.3 | FP translator/lowering | +7 → +4 cycles per scalar op | done | `PlaceElement0` uses `xxpermdi` against pinned `VZERO_VSX` (`vs14`), eliminating `vspltisw` and `VTMP2` clobber. |
| F5 | FCMP consumers read CR0 bits directly (one bit or one `cror`); branch-free FCSEL (`xvcmp*`+`xxsel`; P9 `setb`+`mtvsrdd`) | FP §7 | FCMP/FCCMP/FCSEL translators | 16→6 instructions, 10.0→6.9 cycles | done | Direct CR0 condition evaluation in `MapNZCVCC` for `FCmp` consumers (`FlagsFromFCmp`), bypassing `ProjectXERToCR1` (no `mcrxrx`/`mfxer` projection or cross-field CR ops). Branch-free `NZCVSelectV` (`FCSEL`) via GPR `iselcc` + `mtvsrdd` (ISA 3.0) / `mtvsrd`+`xxpermdi` (ISA 2.07) + `xxsel`, eliminating branch misprediction penalties on data-dependent float selects. Gate 1: 86/86 (default + disableisa30); Gate 2: 24/24; Gate 3: 0 required fail. |
| F6 | Conversions: keep truncating converts + GPR round trip; `mtvsrwa/wz` for W sources; FCVTNS via shadow-FPCR + `fctid` (P8) or `mffscrni`/`mffscrn` (P9) | FP §5 | `A64FPOps.cpp` conversions | 57.6→31.5 (P8), 15.4 vs 37 (P9) cycles | partial (fast paths done) | `A64FloatFromGPR` uses native `mtvsrwa`/`mtvsrwz` for 32-bit GPR inputs, eliminating `extsw`/`clrldi`; `A64FloatToGPR` uses `mffscrni`/`mffscrn` on ISA 3.0. |
| F7 | Keep host FPSCR.RN = FPCR.RMode; no FPSCR writes on hot paths; `mffsl` for reads on P9 | FP §4 | FPCR sync | avoids +139-cycle writes in FP streams | done | Host FPSCR.RN kept synced with FPCR.RMode on MSR FPCR. Implemented ISA 3.0 `mffsl` (lightweight out-of-order FPSCR read, 0.7 vs 14.2 cycles) in `Emitter.h` and `DEF_OP(GetRoundingMode)`. Optimized `DEF_OP(SetRoundingMode)` to use `mffscrn` instead of `mffs`/`mtfsf` (no pipeline flush), and `PushRoundingMode`/`PopRoundingMode` to use `mffscrni`/`mffscrn` on ISA 3.0 with verified ISA 2.07 fallbacks. Gate 1: 86/86 pass in default, disableisa30, and MAXINST=1 modes. |
| F8 | Implement `FRINT*` (glibc `lrint`/`floor`) with the research sequences | FP §13, §5 | FP translator | functional gap + speed | done | `FRINT*` in A64Frontend directly emits `Vector_FToI` with `RoundMode::NearestAway` and `Nearest`. `Vector_FToI` lowers 32-bit to `vrfin` (Nearest Even) and `xvrspi` (Nearest Away), and 64-bit to `xvrdpi` (Nearest Away) and `xvrdpic` with `mffscrni`/`mffscrn` (P9) or `mtfsb0` sandwich (P8), eliminating 15–20 IR ops of software remainder synthesis. |

## N: NEON

| ID | Item | Spec | Touches | Expected impact | Status | Result |
|---|---|---|---|---|---|---|
| N1 | Fuse the vector-scan idiom `CMEQ→UMAXP/UMINP/SHRN→FMOV→CBZ` into the existing CR6 `CondJump{VCmpElementSize}` (`vcmpequb.`+`bc`) | NEON §4.1, §2(c) | SIMD translator, `BranchOps.cpp` CondJump | ~2× fewer host instructions in glibc string loops (64% of their cycles) | done 11c5be45c | Fused CMEQ → UMAXP/ADDP → FMOV → CBZ/CBNZ idiom in A64Frontend via `TryFuseVectorScan`, lowering directly to backend `_CondJump` in `VCmpElementSize` mode (`vcmpequb.` + `bc` on CR6[2]). Loop hot-edge skips reduction, GPR move, and compare; exit path reconstructs CMEQ, reduction, and GPR architectural state. Gate 1: 83 pass, 0 fail (default + disableisa30); Gate 2: 24/24 pass; Gate 3: 0 required fail. |
| N2 | `VSRSHR`/`VSQSHL` scalar spill loops → 5–8-instruction vector forms | NEON §3.2 | `VectorOps.cpp` shifts | 60–100 instructions → 5–8 | done bb436ee2a | Vectorized immediate shifts in `VectorOps.cpp`, replacing scalar spill loops. `VSRSHR` uses `(V >>s N) + ((V >>s (N-1)) & 1)`; `VSQSHL` uses pure vector shift-back comparison and `vsel`. |
| N3 | `StoreVQ` 64-bit `VMov` → `xxpermdi` with zero | NEON §0, §2(d) | `VectorOps.cpp` | 6 → 3 cycles per D-register result | done 7b5f5016b | Replaced 3-instruction `vspltisw/vsldoi/vsldoi` with single `xxpermdi Dst, 0, Dst, 1`, halving latency from 6.0 to 3.0 cycles. |
| N4 | Cheap single ops: `CMHI`=`vcmpgtu*`; `CMTST`=`vand;vcmpgtu(t,0)`; `umaxp v,v` fast form; `ADDV.4S` rotate-add | NEON §3.1, §3.10 | SIMD translator/lowering | 3→1 instruction etc. | done 7b5f5016b, 143d24c75 | `CMHI`=`vcmpgtu*`, `CMTST`=`vand; vcmpgtu(t,0)`, `umaxp/uminp/addp v,v` fast form via `vsrh/vmaxub/vpkuhum` (6→4 instructions, 10.3→8.0 cycles); `ADDV.4S` rotate-add (B3, 0575dcc1a). |
| N5 | Gate the NEON float NaN-precedence fix-up behind one `xvcmpeqsp.`+`bc` (replaces `PropagateNaNOperand`) | NEON §4.3, §3.12 | float-lane translator | free fast path | done | All vector float arithmetic, min/max, and FMA operations routed through `A64Arith`, `FPMinMax`, and `FPMulAddLanes`, compiling to `_A64FArith`, `_A64FMinMax`, and `_A64FMulAdd` with out-of-line cold blocks (`xvcmpeqsp.` + `bc`). |
| N6 | One-instruction gap fills: `SQDMULH/SQRDMULH.8H`=`vmhaddshs/vmhraddshs`; `UDOT`=`vmsumubm`; `PMULL`=`xxmrgld+vpmsumd`; `UABD`=`vabsdu*` (P9); `UQADD/UQSUB.2D` in 3; `SQADD.2D` in 9 | NEON §3.4–3.6, §3.14–3.15 | SIMD translator/lowering | new coverage at native-like cost | done 01494f413 | Coverage landed: UDOT = one vmsumubm (new VUDot op); PMULL.2D via PCLMUL (xxpermdi + vpmsumd), PMULL.8B via new VPMullB (vmrgl/h with zero + vpmsumb); UQADD/UQSUB.2D in 3 ops, SQADD/SQSUB.2D in 7-8; UABD/UABDL via new VUABD op using ISA 3.0 vabsdu* (3→1 insn, 5.7→3.0 cyc on P9; P8 fallback); SQDMULH/SQRDMULH.8H lowered via new VSQDMulH/VSQRDMulH ops to native vmhaddshs/vmhraddshs (1 insn + zero, ~25→2 insns); 32-bit forms via widening multiply fallback. Gate 1: 83 pass, 0 fail (default + disableisa30); Gate 2: 24/24 pass; Gate 3: 0 required fail. Parity: simd_sat, simd_crypto. |
| N7 | ISA 3.0 emitter additions: `vpermr`, `xxspltib`, `mtvsrws`/`mtvsrdd`, `vextu*rx`/`vinsert*`, `xxbr*`, `vctzlsbb`, `lxv` (TBL/DUP/INS/UMOV/REV/first-match) | NEON §7, §3.8–3.11 | `CodeEmitter/PPC64LE`, lowerings | e.g. TBL 6→3 instructions | done | Landed in `Emitter.h` behind `SupportsISA30` and wired into lowerings: `vpermr` (TBL/TBX N8), `xxspltib` (shifts and TBL), `vabsdub`/`vabsduh`/`vabsduw` (UABD/UABDL N6), `xxbrq` (AES N9), `xvcvhpsp`/`xvcvsphp` (vector FP16 N14), `lxvx`/`stxvx` (vector loads/stores), `mfvsrld` (LoadFPSR N11, VExtractToGPR), `vextubrx`/`vextuhrx`/`vextuwrx` (VExtractToGPR), `vinsertb`/`vinserth`/`vinsertw`/`vinsertd`, `vctzlsbb`/`vclzlsbb`. Gate 1: 86/86 pass in default, disableisa30, and MAXINST=1 modes. |
| N8 | `TBL`/`TBX` 1–4 tables: P8 `vperm`+`idx^15`+mask; P9 `xxspltib 31; vminub; vpermr`; TBL3/4 via `vsel` on index bit 5 | NEON §3.9 | SIMD translator | exact, 3–12 instructions | done 8b2bf7e1c, 36b17e47b | TBL/TBX 1-4 tables translated (TBL3/4 OR a second lookup of index-32 instead of a vsel on bit 5). ISA 3.0: VTBL1 6→4, VTBL2 8→4, VTBX1 6→4 instructions; TBL1+TBL2+TBX1+EOR loop 570→494 ms best of 6 (noisy shared host). All 256 index values tested in all modes |
| N9 | AES rounds: `vxor;vcipher` with the state kept byte-reversed; AESE/AESD/AESMC/AESIMC 3–6 instructions; fix the `vsldoi 4`/`REV32` emitter comment | NEON §3.15, §4.2 | crypto ops | exact AES at native cost | in progress (claude-simd) | Exact AES landed as compositions of the x86-shaped ops: AESE = AESENCLAST(s^k, 0), AESD = AESDECLAST(s^k, 0), AESMC = AESENC(AESDECLAST(s, 0), 0), AESIMC = AESIMC (FIPS-197 C.1 passes). The byte-reversed vxor;vcipher round form and the vsldoi/REV32 comment fix are not done. Not timed |
| N10 | `LD2-4`/`ST2-4` interleave | NEON §3.13 | SIMD load/store | coverage + speed | done fe54a952c (with 678769e5a, 6023c6ec2 from powerarm-m2/gcc) | LD2/ST2: first translated on powerarm-m2/gcc (678769e5a) through unzip/zip; merged into one translator (LDx_STx_mult) that keeps that route and stores the 64-bit form with one zip instead of two inserts and a zip. LD3/LD4/ST3/ST4: two VTBL2 lookups with constant indices plus one VOr per register (each lookup 4 instructions on ISA 3.0 after N8). Single structures: LD1/ST1 first on powerarm-m2/gcc (6023c6ec2); LD1-4/ST1-4 and LD1R-4R in one translator (SIMDSingleStructure). Tests: simd_struct1, simd_struct2, simd_struct. Speed not measured separately |
| N11 | FPSR.QC from VSCR.SAT: exact for saturating adds and `vmhaddshs`; read `mfvscr` only at `MRS FPSR` (16-cycle serialising) | NEON §2(e) | FPSR emulation | QC correct with no hot-path cost | done | Eliminated software saturation tracking (VSub/VAdd, VXor, VAnyNonZero) on saturating adds/subs and doubling multiplies; hardware VSCR.SAT is read via mfvscr on MRS FPSR and synchronized via mtvscr on MSR FPSR (ISA 3.0 mfvsrld, ISA 2.07 vsldoi+mfvsrd fallback). Inactive lanes on scalar/64-bit ops masked to zero so only active lanes affect VSCR.SAT. Gate 1: 86/86 pass in default, disableisa30, and MAXINST=1 modes. |
| N12 | Exact `FRECPE`/`FRSQRTE` tables | NEON §3.12 | float-lane translator | correctness | done d3ff30d2d | Bit-exact `FRECPE`, `FRSQRTE`, `FRECPX`, `FRECPS`, `FRSQRTS`, `URECPE`, `URSQRTE` implemented in `TranslateSIMDEstimate.cpp` matching architecture RecipEstimate/RecipSqrtEstimate tables. Covered by generated test `simd_recip` (2,388 cases against Pi golden). |
| N13 | VSX-aware allocator class: pin V16–V31 into vs0–vs31 for float-only/`xv*` values and VSX-only temporaries (not per-op moves; context round trip 15.2 cycles, half-to-half 4) | NEON §2(a); FP §8, §13 | vector register allocator | removes the ~18% of cycles lost to V16–V31 context traffic in FP blocks | not touched | |
| N14 | Vector FP16 conversions: ISA 3.0 `xvcvhpsp`/`xvcvsphp` for `FCVTL`, `FCVTN`, `Vector_FToF` (half↔single) with verified ISA 2.07 fallback | NEON §3.12 | `TranslateFP.cpp`, `VectorOps.cpp`, `JIT.cpp` | 22→2–3 instructions; 5.0× throughput speedup | done 42c94ed53 | Vectorized `FCVTL` and `FCVTN` in `TranslateFP.cpp` (`_Vector_FToF`, `_VFCVTL2`, `_VFCVTN2`). ISA 3.0 path emits `vmrglh`/`vmrghh` + `xvcvhpsp` for f16→f32 and `xvcvsphp` + `vpkuwum` + `xxpermdi` for f32→f16. ISA 2.07 fallback retains software FABI helpers; fixed IEEE-754 / ARMv8 NaN quieting in `f16_to_f32` and `f32_to_f16` (sets bit 22 for f32 and bit 9 for f16) for 100% golden parity under `POWERARM_HOSTFEATURES=disableisa30`. Measured: 425.5 ms (5.32 ns/op) on ISA 3.0 vs 2,123.4 ms (26.54 ns/op) on ISA 2.07 fallback (4.99× speedup on 80M ops); 4.59× speedup on buffer conversion (30.4 vs 139.5 ms); 14% speedup on N-body double-precision integrator (100.6 vs 114.5 ms). Gates: Gate 1 86/86 (default + disableisa30), Gate 2 24/24, Gate 3 0 required fail. |

## C: code cache and translation of cold code

Spec: `docs/powerarm/CODE-CACHE.md`. Timings on CPU 100, `m2time` script, cache
directory private per series. "f470" is the M2 tree, "powerarm" is f0a9da187
(OPT-BRANCHES merged), "branch" is powerarm-opt/codecache after merging it.

| ID | Item | Spec | Touches | Expected impact | Status | Result |
|---|---|---|---|---|---|---|
| C1 | Build the A64 decode table by enumerating each matcher's free index bits into a CSR bucket array; name-sorted handler index | CODE-CACHE.md "Where cold-process time goes" | `A64Frontend/DecodeTable.cpp`, `IRBuilder.cpp` | 15% of `POWERarm /usr/bin/true` | done c620b91b8 | `true` 22.5 -> 20.1 ms per process (100 runs) |
| C2 | Code cache with block linking: `RELOC_LINK_RECORD`, record GuestRIP and StubAddr relocations, linking no longer forced off | CODE-CACHE.md "The stall" | `JIT/Relocations.h`, `JIT/PPC64LE/JIT.cpp` | cache-on steady state | done 67d516d18, 17673ac6a | `cc1 -O2 lvm.c` with the cache enabled: 21.3 -> 12.35 s (cache off 11.90 s) |
| C3 | Code cache format v4: per-block lazy install with guest-byte and integrity checks, keys with host features and build id, append-only segments with flock compaction, saves at exit/execve | CODE-CACHE.md "Design" | `Core/CodeCache.cpp`, `Core.cpp`, `SyscallsSMCTracking.cpp`, `Syscalls/Thread.cpp` | 20-25% of builds is re-translation | done 17673ac6a | warm vs powerarm (f0a9da187): `gcc -c empty.c` 0.404 -> 0.157 s, zlib `configure` 12.94 -> 5.15 s, zlib build 117.7 -> 71.3 s, Lua build 103.3 -> 66.1 s; cold 0.409 / 6.58 / 74.7 / 70.2 s |
| C4 | File identity from `fstat` instead of streaming XXH3 over every mapped executable | CODE-CACHE.md "Design" | `Core/CodeCache.cpp` | 2% of `gcc -c empty.c`, cache on or off | done 78e3583bf | xxhash 2.1% -> 0.0% of `gcc -c empty.c` with the cache off |
| C5 | Code map writer only for `POWERARM_SERVERCODECACHE=1` | CODE-CACHE.md "Design" | `FEXInterpreter.cpp` | a server round trip plus a write per block | done c481ea16a | not measured separately |
| C6 | `check-code-cache.sh` stress test | CODE-CACHE.md "Correctness" | `Scripts/powerarm` | | done 5d2e27803 | all checks pass (75 s) |
| C7 | Write-protect a cached block's pages before checking its guest bytes | CODE-CACHE.md "Design" | `Core/CodeCache.cpp` | closes an SMC race | done d3a9714da | correctness only |
| C8 | Entry hashes checked only for another boot's segments; lock-free segment list; per-thread file memo | CODE-CACHE.md "Design" | `Core/CodeCache.cpp` | xxhash 8% of warm `gcc -c empty.c` | done d03b76fac | warm `gcc -c empty.c` 164 -> 148 ms |
| C9 | Link records' unlinked words derived after relocation (link-first exits start with a RIP window) | CODE-CACHE.md "Design" | `Core/CodeCache.cpp` | cache after the OPT-BRANCHES merge | done 892c0a2c9 | 68694 of 68701 `cc1` blocks load, reloc-failed 0 |
| C10 | vfork copy-back of only written pages (soft-dirty or exclusive-page scan) | brief | `Syscalls/Thread.cpp` | vfork-heavy `make`/`gcc` | dropped on measurement | `process_vm_readv` total 25 ms of a 12 s zlib `configure` (469 calls), 1.4 ms of `gcc -c empty.c` |
| C11 | Asynchronous cache writes on a background core | owner direction | `Core/CodeCache.cpp` | save cost off the guest thread | dropped on measurement | writes are 1.5 s of a 91.5 s cold Lua build (116 processes); a writer cannot outlive `exit_group` |
| C12 | Background install of cached blocks on non-sibling cores (`SCHED_IDLE`) | owner direction | `Core/CodeCache.cpp` | 7.7 s of guest-thread install time in a warm Lua build | dropped on measurement | warm Lua 86.9 -> 90.5 s with it (8.7M blocks installed vs 6.6M used; lock contention) |
| C13 | Server-side pre-translation of never-seen binaries | owner direction | `POWERarmServer`, offline compiler | cold runs | not touched | bounded by the cold-warm gap: 3.4 s of a 74.7 s zlib build, 4.1 s of 70.2 s Lua |
| C14 | Tiered compilation (fast tier, background optimised tier, relink swap) | owner direction | JIT, lookup cache | steady state | evaluated only | see CODE-CACHE.md "Background work" |
| C15 | Relocatable variable-width guest-address loads (width recorded, re-emitted with nop padding) and the same-block delta form under the cache | CODE-CACHE.md "Next targets" | `JIT/PPC64LE/JIT.cpp`, `ALUOps.cpp` | cache-mode codegen +3.8% steady state | done | slice (10 Lua objects + ar/ld, CPU 100): cache off 45.02 s; cold 31.18 -> 30.28 s, warm 28.28 -> 27.22 s; `cc1` reloc-failed 0; format version 5 |
| C16 | Cache size cap (`CodeCacheMaxSize`, 2048 MiB), LRU eviction of whole namespaces, other builds' namespaces removed after 1 h unused | CODE-CACHE.md "Default-on" | `Core/CodeCache.cpp`, `Config.json.in` | default-on | done | sweep after a published segment, at most once a minute; 20 MiB cap test: 187 -> 7.5 MiB, stale foreign namespaces removed; check-code-cache.sh passes |
| C17 | `fork` of a large POWERarm process (`sh` subshells) | strace of zlib `configure` | kernel, allocator | 1.4 ms per fork, 218 ms of `configure` | not touched | |
| C18 | Config id from the distinct host MIDRs (not one per CPU in the affinity mask); `/` appended to `POWERARM_APP_CACHE_LOCATION` | pinned and unpinned runs wrote separate caches | `Core/CodeCache.cpp`, `Common/Config.cpp` | cache sharing across `taskset` | done | one config id for CPU 100 and CPUs 0-87 |
| C19 | Code cache on by default (`CodeCacheScope=rootfs`), opt-out `POWERARM_ENABLECODECACHINGWIP=0`; incompatible SMC modes turn it off | owner direction | `Config.json.in`, `FEXInterpreter.cpp`, CODE-CACHE.md | builds | done | slice with defaults (CPU 100): cold 33.02 s, warm 27.37 s, opt-out 45.01 s (no cache directory created) |
| C20 | fastppcx86: host features missing from the code cache config id (inherited bug; ISA 3.0 and dcbz line size) | inherited cache | fastppcx86 `Core/CodeCache.cpp`, `Core.cpp` | correctness | done, patch `outgoing-patches/fastppcx86/0022-*` | builds on `fastppcx86/daedalao-wt` (9da3ccf9f); not run there |

## X: translation (JIT compile) cost

Workload: `~/Development/.powerarm-golden/slice.sh` (10 Lua objects plus ar/ld, cache off) on
CPU 108, one run per change; shares are `perf record -e cycles:u` over one slice run.
Translation was 53% of the Lua build's cycles. Before this series: backend `CompileCode`
12.9%, RA 6.6% (`Run` + `AssignReg`), flag elimination 2.8%, ScalarSplatChain 2.5%, A64 decoder
2.2%, `ContextImpl::CompileCode` 1.8%, compare-branch fusion 1.5%. Annotating `CompileCode`
put 44% of its self time in three IR walks before emission (spin-loop analysis, consumer mask
elision, producer high-zero elision) and 12% in the per-block FPR live-mask scan. The
slice varies by several percent between single runs on the shared host, so the results below
are A/B runs back to back.

**2026-09-17: what the X series is now worth.** Since C19 turned the code cache on by default, a warm
slice run does almost no translation at all. `perf record -e cycles:u` over one warm slice on CPU 108
(`POWERARM_PORTABLE=1`, app cache warm): **99.7% of samples are in translated guest code** (resolved
through the per-pid perf map to guest cc1 symbols, and flat -- the hottest is `bitmap_set_bit` at 1.7%),
**0.15% in POWERarm's own binary** (top symbol `LookupCache::CacheBlockMapping`), 4% in guest libc.
Cold-to-warm on the same build is 23.9 -> 21.2 s, i.e. all of translation is 11% of a cold run and
effectively 0% of a warm one. Translation cost now only shows up in a first build, and any further X
work has to be measured with `POWERARM_ENABLECODECACHINGWIP=0` (34.5 s slice) to be visible at all.
The lever on a warm build is the quality of the emitted code, not the compiler.

**2026-09-17 (q5/xlate2): the cold profile above is a `gcc -c empty.c` profile, not a slice profile.**
The "82.6% POWERarm itself" table in the Q4 section was measured over a one-shot cold `empty.c`, where
one process pays for all the translation. On the **slice** the 10 gcc invocations share one cache
directory, so process 1 translates and the other 9 install from cache. `perf record -e cycles:u -F 999`
over one cold slice (fresh cache dir, CPU 108) attributes cycles as: **JIT'd guest code 76.5%,
POWERarm itself 20.6%, libc 1.5%, libxxhash 0.7%**. Inclusive shares of all cycles inside POWERarm:
`ContextImpl::CompileBlock` 17.0%, of which **translation (`ContextImpl::CompileCode`) 8.6%**
(backend `PPC64JITCore::CompileCode` 4.1%, `ConstrainedRAPass::Run` 1.6%, A64 decoder 0.5%, DFCE 0.4%),
**cache load (`CodeCache::TryLoadBlock`) 5.3%**, **cache save (`SaveCodeCaches`) 1.5%**; outside it,
**block install/link** `ExitFunctionLinkWithRecord` self 2.7% + `LookupCache::AddBlockMapping` 1.8% +
`CacheBlockMapping`/`AddBlockExecutableRange` 1.1% + `FindBlock` 1.2%. So on the slice the whole
translation budget is about **2.0 s of a 23.9 s cold run** and the cold-to-warm delta is 2.5 s: a 10%
translation win is 0.2 s, at the edge of what this workload can resolve. Measure with own user CPU
(`time -p` around `slice.sh`, children included) rather than wall, and discard the first run after a
link (page-cache cold: one such run read 25.10 s against 23.78/23.81 s for the same binary).
`ExitFunctionLinkWithRecord`'s self time is ~45% `lwarx`/`stwcx.` pairs -- it takes the
`CodeInvalidationMutex` shared guard, drops it, and retakes it before the link section even when

| ID | Item | Touches | Status | Result |
|---|---|---|---|---|
| X1 | A64 decoder: one decode-table lookup per instruction (was three: region walk, layout pass, IR builder); Mask/Expect inline in bucket entries; `CodePages` inserted on page change only | `A64Frontend/Decoder.*`, `DecodeTable.cpp`, `IRBuilder.cpp` | done bb6f6efc5 | with X2: slice 47.98 -> 43.21 s (one run each, not back to back); decoder plus `ContextImpl::CompileCode` 4.0% -> 3.4% of cycles |
| X2 | ScalarSplatChain returns before any per-block work when the unit has no VF*ScalarInsert producer | `IR/Passes/ScalarSplatChain.cpp` | done 0831edf57 | pass 2.5% -> 0.75% of cycles. Also applies to fastppcx86 |
| X3 | Backend prepasses: the producer (high-zero) elision walk shares the consumer/TSO-pair walk, one op behind; spin-loop analysis returns without a backedge before its per-op walk | `JIT/PPC64LE/JIT.cpp` | done ef0c55e7a | slice 45.40 -> 42.81 s. Pure backend; applies to fastppcx86 (x86 guests have backedges more often, so the spin prefilter will save less there) |
| X4 | Skip the per-block FPR live-mask and splat-candidate scan in units with no FPR/FPRFixed destination and no FMA op (masks filled with the zeros it computes) | `JIT/PPC64LE/JIT.cpp`, `JITClass.h` | done 7c821da56 | slice 42.80 -> 41.66 s. Pure backend; applies to fastppcx86 |
| X5 | Overall X1-X4 | | done | `perf` samples for one slice 44.4k -> 41.0k (-7.7%); `CompileCode` 12.9% -> 10.2% (8.8% plus the now out-of-line high-zero walk 1.5%). Not done, next by size: RA (6.6%, two walks per block), flag elimination (2.8%) and compare-branch fusion (1.5%) are one walk each whose cost is the walk itself; merging them is the next step. Block linking (`AddBlockLink` 1.4%, `ExitFunctionLinkWithRecord` 1.3%) is install cost, not translation |
| X6 | Merge the remaining IR walks: compare-branch fusion folded into DFCE's CFG-gather walk (one walk over the unit instead of two), standalone pass kept for `FEX_DISABLEDFCE=1` | `IR/Passes/CompareBranchFusion.*`, `RedundantFlagCalculationElimination.cpp`, `PassManager.cpp` | done (measured, dropped, powerarm-q3/compute-backend 2026-09-17) | Implemented and measured, then reverted: no win. Slice on CPU 108, `POWERARM_ENABLECODECACHINGWIP=0` (translation is ~13 s of the 34.5 s there, so a walk saving is visible): baseline 34.51/34.46 s, merged 34.50/34.51 s. Warm code cache: 21.24 -> 21.21 s. The X3/X4 merges paid because they removed whole per-op walks whose per-op bodies were also cheap; what is left of fusion, DFCE and RA is the per-op work itself (Classify, DecodeSRANode, kill bits), not the iteration, so merging the iteration buys nothing. **RA's two walks cannot be merged at all**: the first is a backwards walk computing kill bits and SRA affinities that the forward allocation walk consumes. Treat the X series as closed |
| X7 | Backend P6 const-exit prepass: stop the per-block op walk once a second non-marker op is counted (only `CodeOps == 1` can mark a block) | `JIT/PPC64LE/JIT.cpp` | done c93fbf379 | slice cold 23.85 -> 23.71 s, warm 21.24 -> 21.19 s; own user CPU cold 23.21 -> 23.05 s, warm 20.74 -> 20.67 s. Pure backend prepass; applies to fastppcx86 |
| X8 | RA per-op bodies: `Seen` bitvector -> byte vector, hoist the duplicated `GetID(CodeNode)` in the backwards pass, drop the `GetNode()`/`GetID()` round trip in the forward source loop | `IR/Passes/RegisterAllocationPass.cpp` | done (measured, dropped, powerarm-q5/xlate2 2026-09-17) | Implemented and reverted: **regression**. Own user CPU cold 23.05 -> 23.27/23.33 s, warm 20.67 -> 20.93/20.82 s. Warm runs barely enter the RA at all, so an equal-sized warm regression means this is code layout in `libPOWERarmCore.so`, not the pass. Micro-edits inside hot translation TUs move the slice by +/-0.25 s of layout noise, which is larger than the whole per-op saving they can buy -- do not chase RA per-op work by wall or CPU time on this workload |

Gates at 7c821da56: A64Frontend 45/45 in default, `POWERARM_MAXINST=1` and
`POWERARM_HOSTFEATURES=disableisa30`; a64diff (bundle 41c1f1e4dd1e, -j 16) on 64k and 4k-kvm:
insn required-fail 0 (optional-fail 18), alarm 3/3, programs 25/25, projects 2/2, rootfs 6/6;
`check-user-strings.sh` and `check-rootfs-server.sh` pass.

## S: process startup (OPT3-STARTUP)

Workload: 50 x `gcc -c empty.c` (driver, `cc1`, `as`) under POWERarm, warm private cache,
CPU 100, wall time per invocation; one run per change. Before this series: 121.1 ms
(0.078 s user, 0.025 s sys per invocation), native POWER9 12 ms, Pi 5 43 ms.
`POWERARM_STARTUPTIMES=1` prints each process's phases to stderr.

`FindBlock` hit and no `CompileBlock` ran. Holding it across the hit path is the next item by share.

| ID | Item | Touches | Status | Result |
|---|---|---|---|---|
| S0 | Env-gated phase timer `POWERARM_STARTUPTIMES=1`: CPU used before `main`, config, server, loader, core init, map, guest, cache save, teardown | `FEXInterpreter.cpp`, `LinuxSyscalls/StartupTimes.h`, `Syscalls/Thread.cpp` | done | warm, per process (ms): premain CPU 2-3, config 0.25, server 0.05, loader 0.05, core 1.0, map 0.2-0.6, guest `cc1` 59 / `as` 12.5 / driver 11 (own), save 0, teardown 6.1-6.8 (every process, `true` included: 6 of its 11 ms) |
| S1 | Telemetry file at exit: one write, no `fsync` (was a `write` per line and an `fsync` that took 6-16 ms in every process) | `FEXCore/Source/Utils/Telemetry.cpp` | done | 121.1 -> 96.2 ms; teardown 6.1-6.8 -> 0.15-0.3 ms per process. Shared code: applies to fastppcx86 |
| S2 | Code cache load timer (two `clock_gettime` per installed block, 1.2% of cycles) only with `POWERARM_CODECACHESTATS=1`; phase timer also prints minor/major faults | `Core/CodeCache.cpp`, `LinuxSyscalls/StartupTimes.h` | done | 94.8 -> 94.2 ms, within noise (both measured with `POWERARM_PORTABLE=1`, see below) |
| S3 | L2 lookup cache indexed per A64 instruction (1024 entries per guest page, not 4096) | `Core/LookupCache.h` | reverted | 96.2 -> 95.1 ms, minor faults unchanged (1411 vs 1412 for `cc1`): the faults are in L1, not L2 |
| S4 | Per-thread L1 (16 MiB at `MAX_L1_ENTRIES`, indexed by RIP) faulted and zeroed page by page: about 500 read plus write faults of 64 KiB pages per `cc1`, 30% of all faults | `Core/LookupCache.*` | done | L1 resized to 128k entries (2 MiB, 32 x 64K pages); prefault writable via `MADV_POPULATE_WRITE` (fallback single-byte touch per host page) avoids zero-page read faults followed by COW write faults. `true` minor faults 372 -> 299; cold slice 20.26 -> 17.45 s (-2.81 s / -14%), warm slice 17.83 s. All gates pass (Gate 1: 85/85 in all 3 modes; Gate 2: 24/24; Gate 3: 0 required fail) |
| S5 | Q2: `GuestToHostMap::AddBlockLink` walked the destination's whole inbound chain for a duplicate on every link (quadratic in fan-in, 4.8% of warm cycles). `ExitFunctionLinkWithRecord` now refuses any site whose caller word is no longer the unlinked word (under the write lock), so no duplicate can reach it | `Core/LookupCache.h`, `JIT/PPC64LE/JIT.cpp` | done | CPU 108, 50 x warm `gcc -c empty.c`, `POWERARM_PORTABLE=1`: 96.8 -> 93.2 ms |
| S6 | Q2: `FlushICacheRange` does `sync; isync` only when AT_HWCAP has `PPC_FEATURE_ICACHE_COHERENT` (as the kernel and vDSO do), not a dcbst/icbi pair per 128 bytes (2.7% of warm cycles: every installed block and patched link word) | `include/FEXCore/Utils/ArchHelpers/PPC64CacheFlush.h` | done | 93.2 -> 90.7 ms. Shared code: applies to fastppcx86 |
| S7 | Q2: interpolation search of the segment block index instead of `lower_bound` (index probes were 3% of warm cycles, mostly cache misses) | `Core/CodeCache.cpp` | reverted | 90.7 -> 92.0 ms, no win |

Q2 gates at S6 (CPU 108, private cache /tmp/q2-cache, POWERARM_PORTABLE=1): slice 23.76 s cold, 21.36 s warm;
A64Frontend claude-simd default mode passed 52, failed 0; check-code-cache.sh passes (parallel, ISA 3.0, replace,
forged, corrupt, SMC). Profile of 20 warm gcc runs at the baseline: link/install dominates -- ExitFunctionLinkWithRecord
8.9%, AddBlockLink 4.8%, FindBlock 4.3% (mostly lock atomics), ApplyCodeRelocations 4.2%, AddBlockMapping 3.5%,
segment index probe 3.0%, FlushICacheRange 2.7%, memcpy 2.6%, AddBlockExecutableRange 2.1%.

Measurement note: at 09:41 on 2026-09-17 a binfmt_misc handler `POWERarm-aarch64` was registered
(interpreter `~/Development/POWERarm/build-powerarm/Bin/POWERarm`, flags POCF). From then on every
guest `execve` runs that binary, not the one under test, so `cc1` and `as` of a `gcc` run silently
use another build (warm 94.8 ms became 109 ms with the same binary). `POWERARM_PORTABLE=1` skips
binfmt; S2 and later numbers use it. S0 and S1 were measured and gated before the registration.

After S2 (1b1235ead), warm, `POWERARM_PORTABLE=1`, per process (ms): premain CPU 2.1-2.8, config 0.26,
server 0.05, loader 0.04, core 0.95, map 0.24-0.44, guest `cc1` 57.5 / `as` 11.8, save 0.02,
teardown 0.04-0.09. Slice (CPU 100, private cache): 25.91 s cold, 23.72 s warm; zlib `./configure`
alone: 4.00 s, then 3.61 s. Both ran while the gates below used CPUs 0-87.

Gates at 1b1235ead (emulator runs with `POWERARM_PORTABLE=1` where guest `execve` matters): A64Frontend
45/45 in default, `POWERARM_MAXINST=1` and `disableisa30`; a64diff (bundle 41c1f1e4dd1e, -j 16) on
64k and 4k-kvm: insn required-fail 0 (optional-fail 18), alarm 3/3, programs 25/25, projects 2/2,
rootfs 6/6; `check-code-cache.sh`, `check-user-strings.sh` and `check-rootfs-server.sh` pass.

## Follow-ups to measure

- Done f4aa730da (test 0442316d6): abandoning a guest signal handler (`siglongjmp`/`longjmp` out
  of it) leaked the handler's host stack frame in the signal delegator (`SignalDelegator.cpp`
  StoreThreadState; shared with upstream FEX) and crashed after ~2500 abandoned handlers; 150
  abandoned handlers also crashed or hung intermittently (2 of 32 runs), when the guest stack sat
  just under the host stack and the leaked frames ran into it. Found by OPT-BRANCHES while writing
  `callret.c`. Deliveries now record handler levels, detect abandoned ones (guest SP past the frame
  at delivery or syscall entry, or the frame's private words overwritten) and build the next frame
  where the abandoned ones were, saving and restoring the interrupted context's live host stack;
  see "Abandoned guest handlers" in `SignalDelegator.cpp`. `unittests/A64Syscalls/sys_siglongjmp`
  fails every run before and passes 24/24 in default and `POWERARM_MAXINST=1` after; `callret.c`
  has its siglongjmp section back (golden in `.powerarm-golden/fix-siglongjmp`). fastppcx86 patch:
  `docs/powerarm/outgoing-patches/fastppcx86/0017-*`.

- `mulld` measured 8.7 cycles vs the documented 5 (PIPE §8).
- Whether mixed integer/vector workloads use more of the core's width (issue-queue stall events).
- Half-precision conversion `xscvhpdp` at 38 cycles (NEON §7): is there a faster exact path?


## Cold runs are a first-class metric (owner, 2026-09-17)

**Every measurement reports cold and warm.** Cold is a real user's first run: first launch of a
program, one-shot commands, CI, a fresh install, and anything after a rebuild or cache eviction.
Optimizing only the warm number hides that cost.

- **Per change:** run the slice twice with a **fresh cache directory** (cold), then again (warm),
  and report both.
- **Translation cost (X series) is reopened on that basis.** It is ~11% of a cold run and ~0% of a
  warm one; closing it because warm runs dominate was the wrong call.
- **AOT pre-translation** (`POWERARM_AOTTRANSLATE`, `Scripts/powerarm/aot-translate.sh`) is the
  other lever on cold, especially for the short-lived tools a build spawns.

### Where a cold run's time actually goes (Q4, 2026-09-17)

Measured on the q4-cold build at 4bef173bf. Slice on CPU 108, `POWERARM_PORTABLE=1`,
private cache: **cold 24.42 s, warm 21.30 s**. `POWERARM_STARTUPTIMES=1` over the same
slice, summed across its 35 emulated processes: own user time **23.07 s cold vs 20.65 s
warm** (+2.42 s) and sys 0.61 vs 0.45 s, so the whole cold penalty is user cycles in
POWERarm, and **cache writing is 0.34 s of it** (the `save` phase; 1 ms warm).

One-shot cold, `gcc -c empty.c` with an empty cache: **311 ms, against 93 ms warm**.
Per process (`save` is the cache write): `cc1` guest 181 ms / save 27 ms cold vs 52 / 0
warm; `as` 32 / 3.8 vs 12 / 0; the `gcc` driver's own user time 40 vs 7.5 ms.

`perf record -e cycles:u -F 10000` over that cold `gcc -c empty.c` (the run must
be wrapped in `env -u LD_LIBRARY_PATH PATH=/usr/local/bin:/usr/bin`, or the perf
shim's `LD_LIBRARY_PATH` leaks into the guest and it runs host-native tools --
that silently produced a "0.6% POWERarm" profile first time round):

| Share of all user cycles | Where |
|---|---|
| 82.6% | POWERarm itself |
| 7.7 + 1.1 + 0.4% | translated guest code (`[JIT] tid N`) |
| 2.5% | libxxhash (cache write: `GuestHash` and `EntryHash` per block) |

Inside POWERarm, by self time over the whole run: backend `PPC64JITCore::CompileCode`
15.6%, `ConstrainedRAPass::Run` 8.8% + `AssignReg` 3.1%, A64 decoder 3.8%, DFCE 3.5%,
`ExitFunctionLinkWithRecord` 3.5%, `ContextImpl::CompileCode` 2.7%, compare-branch
fusion 2.2%, high-zero elision 2.0%, `FindBlock` 2.0%, `SpillStaticRegs` 1.9%,
`Op_ExitFunction` 1.7%, `CollectLiveBlocks` 1.6%, allocator 1.3%,
`ApplyCodeRelocations` 1.0%. So on a cold run **translation is ~55-60%, block
install/linking ~6%, and the cache write ~5%** of user cycles. The X series is
where cold time is, and the two items it left open -- the register allocator's
per-op work (11.9%) and backend emission (15.6%) -- are the two biggest.

| ID | Item | Touches | Status | Result |
|---|---|---|---|---|
| Q4-A1 | AOT policy for cold runs: pre-translate the short-lived tools a build spawns in mode `entries`, not `cc1`; `-s MAXBYTES` size cap in `aot-translate.sh` for the whole-rootfs form | `Scripts/powerarm/aot-translate.sh`, CODE-CACHE.md | done | 11 tools (`bash`, `gcc`, `as`, `ar`, `ld`, `ld.bfd`, `collect2`, `libc`, `ld.so`, `libbfd`, `libopcodes`), mode `entries`: 0.14 s of AOT, **28 MiB** of cache, slice **cold 24.42 -> 23.73 s (-2.8%), warm 21.30 -> 21.20 s**. Mode `all` is 268 MiB for cold 23.65 s, i.e. 10x the size for another 0.08 s; adding `cc1` in mode `entries` (115 MiB total) gives cold 23.70 s, no win, matching the earlier `cc1`-only finding. `-m entries -s 4194304 /usr/bin /usr/lib` covers 877 files in 2.5 s for 466 MiB. Recipe and table in CODE-CACHE.md, "Pre-translating the tools a build spawns" |

## Queue (scheduled 2026-09-17, in order)

Starts after the CLAUDE-SIMD workstream (Claude CLI instruction gaps) merges.

| Step | Work | Owner | Gate |
|---|---|---|---|
| Q1 | AOT/background pre-translation of rootfs binaries (POWERarmServer or `POWERarmOfflineCompiler`), so cold runs behave like warm ones | standard agent | slice cold ≈ slice warm; zlib `configure` cold |
| Q2 | Lazy cache install: install cached blocks on first reach instead of all ~11.4k at `cc1` start | standard agent (may run alongside Q1 if file areas don't overlap) | `gcc -c empty.c` warm; slice warm |
| Q3 | Compute checklist items for the remaining `cc1` gap: P1(b), IR-walk merging, F1–F8, N1, remaining P/N rows | standard agents, split by area | `cc1 lvm.c`; slice |
| Q4 | Review of lowerings and performance changes (correctness, ISA gating, missed wins) against the research docs | **one Fable agent**, after Q1–Q3 | written review; fixes routed to standard agents |

Q1 result (branch `powerarm-q/aot`, smoke test, one run each, CPU 100, private cache dir):

| Q1 measurement | Value |
|---|---|
| slice, empty cache | 24.84 s |
| slice, after pre-translating `gcc`, `cc1`, `as`, `ld`, `make`, `bash`/`sh`, `libc.so.6`, `ld-linux-aarch64.so.1` (mode `all`) | 23.93 s (warm reference 23.7 s) |
| pre-translation wall time (`-j 22`, CPUs 0-87) | 13.3 s, nearly all of it `cc1` (55,182 function entries, 825,563 seeds) |
| cache size after pre-translation | 1.4 GiB (`cc1` 1.2 GiB; `cc1` in mode `calls` is 651 MiB, in `entries` 87 MiB) |
| `cc1 -O2 lvm.c` after `cc1` pre-translation: loaded / not in index | `calls` 55,297 / 53,571; `all` 96,657 / 12,040 |
| A64Frontend (default mode) / `check-code-cache.sh` | 52 passed, 0 failed / pass |

Measurement rules for every step: one run per change on a small slice, full gates and full zlib/Lua numbers once at the end, `POWERARM_PORTABLE=1` while a binfmt registration exists, and a report within about an hour.
