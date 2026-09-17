# POWERarm optimization checklist

This is the shared list of measured optimizations from the three research reports. **Read it
before you touch any lowering, translator, allocator or dispatcher code.** If your work passes
through the code of an item that's `not touched`, claim it and do it in the same pass. It's cheaper than
coming back later with a cold agent.

Sources:
- **[PIPE]** `research/pipeline/POWER9-VS-A76-PIPELINE.md`
- **[FP]** `research/scalar-fp/SCALAR-FP-LOWERING.md`
- **[NEON]** `research/neon/NEON-LANDINGS.md`

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
| B1 | `VFNMLA`/`VFNMLS`/`xsnm*`/`xvnm*` negate after rounding: negate the multiplicand, never the result | FP §10, §6.2; NEON §5 | `JIT/PPC64LE/VectorOps.cpp` ~3747/3767, FMA frontend | 2.06 | not touched | |
| B2 | `Float_FromGPR_S` rounds i64→f32 twice | FP §10 | `VectorOps.cpp` ~4557 | 2.07 | not touched | |
| B3 | 32-bit `VAddV` uses saturating `vsumsws` (also sets VSCR.SAT) | NEON §5 | `VectorOps.cpp` ~586 | 2.03 | not touched | |

Also check whether each B item affects fastppcx86. If it does, add a patch under
`outgoing-patches/fastppcx86/`.

## P: pipeline and code shape (largest multipliers)

| ID | Item | Spec | Touches | Expected impact | Status | Result |
|---|---|---|---|---|---|---|
| P1 | Keep non-pinned guest GPRs in host registers across a block (block-local/global allocation); no same-slot `std;ld` reloads | PIPE §6 Rule 1, §5.1 | register allocator, `a64::SRA`, `JIT.cpp` | crc32 8.1× → ~3× | done d0ad3e55c, part (a) only | (a) done: the A64 IR builder reuses the last stored or loaded SSA value of a context-backed GPR for a load within 8 guest instructions in the same IR block; every store stays in place, so CPUState is exact at each instruction boundary. A64Bench warm on CPU 104 (window sweep, 1 run): crc32 1450 to 402 ms (7.35x to 2.04x the Pi), sha256 1428 to 960, vm 2620 to 1742, sort 1049 to 970; wider windows lose on vm (1905 at 32) as the reused values get spilled. `gcc -O2 -c lvm.c` 9.75 to 9.46 s (-3%) on top of P10; zlib and Lua builds within noise. Register census for cc1 (guest register references weighted by block samples): the pinned set covers 94.4% (X0 25%, X1 14%, SP 11%, X2 9%); the best context-backed register is X25 at 1.1%, so re-pinning X8-X17 would not help GCC-built code and was not changed. Not done: (b) block-local allocation of context-backed registers with load at entry/write at exit needs precise state at faulting memory ops (a guest SIGSEGV handler may resume), i.e. per-fault-site recovery data or flushing before every guest load/store; cc1 shows about 3% of JIT samples on context GPR traffic, so it is not the build's bottleneck |
| P2 | Inline compare cache before the L1 lookup `bctr` for guest `BR` | PIPE Rule 2, §4.5, §5.2 | dispatcher, `BranchOps.cpp`, A64 `BR` | up to ~30% of interpreter-style loops | done 854893dc4 | 8 chained slots per `BR` site, each a guarded direct `b` filled by the record linker; an empty slot is filled by a sampled arrival (time-base low 6 bits zero), not the first one: first-come filling gave vm's eight slots to its setup opcodes and made it slower (4 slots 4500 ms, 8 slots 3410 ms per rep). vm 3303→2149 ms (−35% vs baseline, 2 of its ~15 opcodes still reach the probe); vm slots 0/4/6/8 (sampled): 3294/3056/2336/2258 ms. cc1 unchanged (10.69→10.67 s; 7.9M `BR` in the run); zlib 123.1→124.2 s, Lua 107.5→108.1 s (+0.9%/+0.6%, likely the extra ~1 KB of slots and thunks per `BR` site); A64Bench geomean 0.995→1.083. `POWERARM_BRCACHESLOTS=0..8` |
| P3 | Pair guest `BL`/`BLR` with LK=1 host branches and guest `RET` with `blr`; budget link stack 64 (16 under SMT4) | PIPE Rule 3, §4.4 | `BranchOps.cpp`, frontend `BranchHint` | +25.5 cycles per unpaired return avoided | done 26dc5f8c2 | Frontend hints 5a4f52c67. A64 layout (`EmitA64PairedCall`): the call pushes {X30, &trampoline} and ends in `bl`/`bctrl`; the trampoline is a link-first constant exit to the return address in the caller's unit; RET pops without a bounds check (guard-page fault resets the stack pointer register) and `blr`s on a match, else probes. Shadow stack now on by default. cc1 `lvm.c`: RET count-cache mispredicts gone (ccache mispredicts 55M→8M, lstack predictions 45M→192M with 1.8M mispredicted), cycles 41.6G→39.4G, 11.24→10.69 s (−4.9%); zlib 126.9→123.1 s (−3.0%); Lua 111.4→107.5 s (−3.5%). A64Bench vs baseline: vm 3284 (layout-sensitive, see P12), sort 1056 (link-first alone 1035: the pairing costs ~16 instructions per call/return pair where the count cache already predicted), bst 565, geomean 0.995. Tests: `unittests/A64Frontend/callret.c` (recursion to 120000, longjmp out of 300-deep recursion, RETs to other addresses, `ret x1`, `br x30`, frame skipping, 300000 unpaired RETs, tail calls, handlers making calls, ucontext switching, self-modifying caller) against the Pi golden in all three modes; both guard-page resets observed firing |
| P4 | At most one count-cache branch per aligned 32-byte block; emit `mtctr` as early as possible | PIPE Rule 4 | block emitter, dispatcher | +16 cycles and a guaranteed mispredict avoided | done 26dc5f8c2 (audit; the one change is in P3) | Audited after P12/P3/P2: a block exit now emits at most one count-cache branch (the probe `bctr` of a `BR`/`BLR`/mismatched `RET`); constant exits emit none (link-first) and a paired `RET` uses `blr`. `mtctr` follows the HostCode load directly, before `std rip` and the r0 re-zero, in every probe; the one change is the `RET` pop, which now moves the trampoline into LR before the guest-address compare (in the P3 commit). Remaining shared blocks: in 1 of 4 far-link thunks the `bctr` at +0x10 and the unlinked `bctr` at +0x28 share a 32-byte block, but only one of them ever executes. Verified: monomorphic workloads count-cache-mispredict ~0 (crc32 4965, bst 4874 per run) |
| P5 | Flags in CR fields; never read XER on hot paths; never clear XER.SO | PIPE Rule 5, §4.2; FP §13 | `ALUOps.cpp`, flag lowering | +4…+11 cycles per XER read; 147 cycles per SO toggle | not touched | |
| P6 | Block layout for fetch redirect: fall through on the common path, one taken branch per block | PIPE Rule 6 | block builder/layout | ~7-cycle loop floor; avoid extra taken branches | done 310ebcd2d | A guest conditional branch between two block exits (every `B.cond`/`CBZ`/`TBZ` the A64 frontend ends a unit with) was lowered `bc; b; b` into the taken and not-taken exit blocks: 2 taken host branches on the taken path, 3 on the other. `CondJump` now branches straight to the not-taken exit and falls into the taken one (1 and 2 taken branches, including the linked exit). Fallthrough elision (`FEX_FALLTHROUGH`) stays off: it was measured as a POWER8 regression and this shape does not depend on it. cc1 `lvm.c`: taken branches 4.43G→3.23G, 10.67→9.96 s (−6.7%); zlib 124.2→121.7 s (−2.0%); Lua 108.1→105.4 s (−2.5%); A64Bench geomean vs baseline 1.083→1.100 (vm 2030, sort 1044, bst 558). `POWERARM_NOEXITSHAPE=1` disables. Follow-up in powerarm-opt2/codeshape (617642fab, bd36bfe32): profiling `cc1 -O2 lvm.c` after P10 found the 29% of JIT samples on a unit's first instruction to be arrival cost, not entry work: linking forward exits past the entry poke (`ld`; `stb`) moved the samples to the next two instructions and left the slice unchanged (47.2/47.4 s), so that was reverted. The hot units were instead full of intra-unit hops: every in-unit B.cond successor went through a block holding only a Jump (`bc; b; b` chains), and CondJump/Jump emitted `b` to the next block. The frontend now targets in-unit successors directly, CondJump takes one `bc` to the forward leg and falls into the next block (islands keep the 14-bit displacement in reach; `POWERARM_NOSHORTCOND=1` restores the old shape), and a forward Jump to the next block emits nothing. Slice 47.2 to 41.1 s (successors) and cc1 cycles 29.31G to 26.46G, branches 5.01G to 3.90G (shape) |
| P7 | Lightest barrier/exclusive forms the memory model allows (`larx`/`stcx.` 74 vs 23 cycles; `sync` +42, `lwsync` +14) | PIPE Rule 8, §4.6 | exclusive monitor, atomics lowering | LSE/LL-SC heavy code | not touched | |
| P8 | Constants: materialise off the critical path; load them when they're on it; reuse pinned zero | PIPE Rule 9; FP §3.3; NEON §2(b) | constant emission | per-op re-materialisation removed | not touched | |
| P9 | Never let a wider load consume two narrower stores | PIPE Rule 10 | context layout, load/store lowering | +21 cycles and no forwarding avoided | done d0ad3e55c, dropped on measurement | The frontend writes every context GPR slot with one 64-bit store and reads it with one 64-bit load, so the JIT has no narrow-store/wide-load pair on guest state. `pm_cmplu_stall_lhs` for `gcc -O2 lvm.c`: 23.3 M under POWERarm, 20.0 M for native ppc64le gcc on the same input, so the stalls come from the guest program's own memory access pattern |
| P10 | Multi-block/region compilation (currently one guest block per compile) | PIPE §8; M1 TODO | frontend block formation, IR passes | enables P1 across block edges | done f90005f75 | The A64 decoder now grows a compile unit along B, B.cond, CBZ/CBNZ and TBZ/TBNZ targets within 128 bytes of the branch, up to 8 blocks (`POWERARM_MULTIBLOCK=0` or `POWERARM_MAXINST=1` restore one block). Profile first (`cc1 -O2 lvm.c`, CPU 104): context-backed GPR loads/stores held about 3% of JIT samples, but 29% landed on the first instruction of a block, the build had 4.3 G taken branches (4.8x native gcc) and 1.07 G L1 icache misses (18x), and translation was 43% of the Lua build's user cycles, so block transitions and compile cost ranked first. Region size was chosen on the builds: a 16 KiB window cost cold start 35% (`gcc -c empty.c` 0.41 to 0.55 s). Result on CPU 104 against f470d805f on CPU 104, 2 runs each: `gcc -O2 -c lvm.c` 12.32/12.32 to 9.75/9.70 s (-21%); zlib 134.0/132.9 to 123.5/123.2 s (-7.6%); Lua 127.1 (outlier)/118.9 to 106.8/107.3 s (-10%). A64Bench warm (CPU 104, 3x3): vm 3311 to 2640 ms (7.73x to 6.17x the Pi), crc32 unchanged (1450), sha256 +1.1%, bst +2.2%, sort +2.7% |
| P11 | Run one guest thread per core with SMT siblings idle (scheduling/pinning policy, docs) | PIPE Rule 7, §4.7 | launcher/config, docs | vm 1.9× slower with a busy sibling | not touched | |
| P12 | Link constant exits on their first execution: an unlinked exit goes to the record linker instead of running the inline L1 probe | PIPE §5.2 (probe chain); census below | `BranchOps.cpp` ExitFunction | 197M probed constant exits in `cc1 -O2 lvm.c` | done 309288eb5 | Branch census of `cc1 -O2 lvm.c`: 162k of 370k constant exit sites were never linked (their target was already in L1, so the probe hit and the linker never ran) and executed 197M times, 12% of all exits. cc1 12.42→11.24 s (−9.5%); zlib 134.5→126.9 s (−5.6%); Lua 118.9→111.4 s (−6.3%) (CPU 108, 2 runs each, spread <0.5%). A64Bench (CPU 108, 3 runs): vm 3303→3836 ms (+16%): 180M fewer predictable probe `bctr` changed the count cache's global history and vm's dispatch `br` mispredicts rose 46M→74M; bst 544→560, sort 1062→1035, crc32/sha256 flat; geomean 0.97. P2 is the fix for vm. `POWERARM_NOLINKFIRST=1` restores probe-first |

## F: scalar floating point

| ID | Item | Spec | Touches | Expected impact | Status | Result |
|---|---|---|---|---|---|---|
| F1 | NaN precedence: `xscmpudp` + `bso` cold pre-check; the cold path swaps operands only for op1 qNaN + op2 sNaN | FP §3.2–3.4, §11 | `A64Frontend/TranslateFP.cpp`, FP lowering | 4 adds: 45→13 instructions, 18.4→6.6 cycles | not touched | |
| F2 | FMIN/FMAX(NM) → `xvmaxdp`/`xvmindp` + the F1 pre-check (not `xsmaxjdp`/`cdp`) | FP §6.1 | FP translator | 40 IR ops → 3 | not touched | |
| F3 | FMA via `xsmaddadp`/`xsmsubadp` with `xsnegdp` on the multiplicand + NaN check (see B1) | FP §6.2 | FP translator, `VectorOps.cpp` | exact, fewer instructions | not touched | |
| F4 | Zero-upper via one `xxpermdi` against pinned zero (or elide) | FP §3.3 | FP translator/lowering | +7 → +4 cycles per scalar op | not touched | |
| F5 | FCMP consumers read CR0 bits directly (one bit or one `cror`); branch-free FCSEL (`xvcmp*`+`xxsel`; P9 `setb`+`mtvsrdd`) | FP §7 | FCMP/FCCMP/FCSEL translators | 16→6 instructions, 10.0→6.9 cycles | not touched | |
| F6 | Conversions: keep truncating converts + GPR round trip; `mtvsrwa/wz` for W sources; FCVTNS via shadow-FPCR + `fctid` (P8) or `mffscrni`/`mffscrn` (P9) | FP §5 | `A64FPOps.cpp` conversions | 57.6→31.5 (P8), 15.4 vs 37 (P9) cycles | not touched | |
| F7 | Keep host FPSCR.RN = FPCR.RMode; no FPSCR writes on hot paths; `mffsl` for reads on P9 | FP §4 | FPCR sync | avoids +139-cycle writes in FP streams | not touched | |
| F8 | Implement `FRINT*` (glibc `lrint`/`floor`) with the research sequences | FP §13, §5 | FP translator | functional gap + speed | not touched | |

## N: NEON

| ID | Item | Spec | Touches | Expected impact | Status | Result |
|---|---|---|---|---|---|---|
| N1 | Fuse the vector-scan idiom `CMEQ→UMAXP/UMINP/SHRN→FMOV→CBZ` into the existing CR6 `CondJump{VCmpElementSize}` (`vcmpequb.`+`bc`) | NEON §4.1, §2(c) | SIMD translator, `BranchOps.cpp` CondJump | ~2× fewer host instructions in glibc string loops (64% of their cycles) | not touched | |
| N2 | `VSRSHR`/`VSQSHL` scalar spill loops → 5–8-instruction vector forms | NEON §3.2 | `VectorOps.cpp` shifts | 60–100 instructions → 5–8 | not touched | |
| N3 | `StoreVQ` 64-bit `VMov` → `xxpermdi` with zero | NEON §0, §2(d) | `VectorOps.cpp` | 6 → 3 cycles per D-register result | not touched | |
| N4 | Cheap single ops: `CMHI`=`vcmpgtu*`; `CMTST`=`vand;vcmpgtu(t,0)`; `umaxp v,v` fast form; `ADDV.4S` rotate-add | NEON §3.1, §3.10 | SIMD translator/lowering | 3→1 instruction etc. | not touched | |
| N5 | Gate the NEON float NaN-precedence fix-up behind one `xvcmpeqsp.`+`bc` (replaces `PropagateNaNOperand`) | NEON §4.3, §3.12 | float-lane translator | free fast path | not touched | |
| N6 | One-instruction gap fills: `SQDMULH/SQRDMULH.8H`=`vmhaddshs/vmhraddshs`; `UDOT`=`vmsumubm`; `PMULL`=`xxmrgld+vpmsumd`; `UABD`=`vabsdu*` (P9); `UQADD/UQSUB.2D` in 3; `SQADD.2D` in 9 | NEON §3.4–3.6, §3.14–3.15 | SIMD translator/lowering | new coverage at native-like cost | not touched | |
| N7 | ISA 3.0 emitter additions: `vpermr`, `xxspltib`, `mtvsrws`/`mtvsrdd`, `vextu*rx`/`vinsert*`, `xxbr*`, `vctzlsbb`, `lxv` (TBL/DUP/INS/UMOV/REV/first-match) | NEON §7, §3.8–3.11 | `CodeEmitter/PPC64LE`, lowerings | e.g. TBL 6→3 instructions | not touched | |
| N8 | `TBL`/`TBX` 1–4 tables: P8 `vperm`+`idx^15`+mask; P9 `xxspltib 31; vminub; vpermr`; TBL3/4 via `vsel` on index bit 5 | NEON §3.9 | SIMD translator | exact, 3–12 instructions | done 8b2bf7e1c, 36b17e47b | TBL/TBX 1-4 tables translated (TBL3/4 OR a second lookup of index-32 instead of a vsel on bit 5). ISA 3.0: VTBL1 6→4, VTBL2 8→4, VTBX1 6→4 instructions; TBL1+TBL2+TBX1+EOR loop 570→494 ms best of 6 (noisy shared host). All 256 index values tested in all modes |
| N9 | AES rounds: `vxor;vcipher` with the state kept byte-reversed; AESE/AESD/AESMC/AESIMC 3–6 instructions; fix the `vsldoi 4`/`REV32` emitter comment | NEON §3.15, §4.2 | crypto ops | exact AES at native cost | not touched | |
| N10 | `LD2-4`/`ST2-4` interleave | NEON §3.13 | SIMD load/store | coverage + speed | done fe54a952c (with 678769e5a, 6023c6ec2 from powerarm-m2/gcc) | LD2/ST2: first translated on powerarm-m2/gcc (678769e5a) through unzip/zip; merged into one translator (LDx_STx_mult) that keeps that route and stores the 64-bit form with one zip instead of two inserts and a zip. LD3/LD4/ST3/ST4: two VTBL2 lookups with constant indices plus one VOr per register (each lookup 4 instructions on ISA 3.0 after N8). Single structures: LD1/ST1 first on powerarm-m2/gcc (6023c6ec2); LD1-4/ST1-4 and LD1R-4R in one translator (SIMDSingleStructure). Tests: simd_struct1, simd_struct2, simd_struct. Speed not measured separately |
| N11 | FPSR.QC from VSCR.SAT: exact for saturating adds and `vmhaddshs`; read `mfvscr` only at `MRS FPSR` (16-cycle serialising) | NEON §2(e) | FPSR emulation | QC correct with no hot-path cost | not touched | |
| N12 | Exact `FRECPE`/`FRSQRTE` tables | NEON §3.12 | float-lane translator | correctness | not touched | |
| N13 | VSX-aware allocator class: pin V16–V31 into vs0–vs31 for float-only/`xv*` values and VSX-only temporaries (not per-op moves; context round trip 15.2 cycles, half-to-half 4) | NEON §2(a); FP §8, §13 | vector register allocator | removes the ~18% of cycles lost to V16–V31 context traffic in FP blocks | not touched | |

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
| C15 | Relocatable variable-width guest-address loads (width recorded, re-emitted with nop padding) and the same-block delta form under the cache | CODE-CACHE.md "Next targets" | `JIT/PPC64LE/JIT.cpp`, `ALUOps.cpp` | cache-mode codegen +3.8% steady state | not touched | |
| C16 | Cache size cap, LRU sweep at compaction, stale build-id namespaces removed | CODE-CACHE.md "Default-on" | `Core/CodeCache.cpp` | default-on | not touched | |
| C17 | `fork` of a large POWERarm process (`sh` subshells) | strace of zlib `configure` | kernel, allocator | 1.4 ms per fork, 218 ms of `configure` | not touched | |

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
