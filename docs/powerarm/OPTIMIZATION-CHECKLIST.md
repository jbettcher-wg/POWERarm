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
| P1 | Keep non-pinned guest GPRs in host registers across a block (block-local/global allocation); no same-slot `std;ld` reloads | PIPE §6 Rule 1, §5.1 | register allocator, `a64::SRA`, `JIT.cpp` | crc32 8.1× → ~3× | done (this commit), part (a) only | (a) done: the A64 IR builder reuses the last stored or loaded SSA value of a context-backed GPR for a load within 8 guest instructions in the same IR block; every store stays in place, so CPUState is exact at each instruction boundary. A64Bench warm on CPU 104 (window sweep, 1 run): crc32 1450 to 402 ms (7.35x to 2.04x the Pi), sha256 1428 to 960, vm 2620 to 1742, sort 1049 to 970; wider windows lose on vm (1905 at 32) as the reused values get spilled. `gcc -O2 -c lvm.c` 9.75 to 9.46 s (-3%) on top of P10; zlib and Lua builds within noise. Register census for cc1 (guest register references weighted by block samples): the pinned set covers 94.4% (X0 25%, X1 14%, SP 11%, X2 9%); the best context-backed register is X25 at 1.1%, so re-pinning X8-X17 would not help GCC-built code and was not changed. Not done: (b) block-local allocation of context-backed registers with load at entry/write at exit needs precise state at faulting memory ops (a guest SIGSEGV handler may resume), i.e. per-fault-site recovery data or flushing before every guest load/store; cc1 shows about 3% of JIT samples on context GPR traffic, so it is not the build's bottleneck |
| P2 | Inline compare cache before the L1 lookup `bctr` for guest `BR` | PIPE Rule 2, §4.5, §5.2 | dispatcher, `BranchOps.cpp`, A64 `BR` | up to ~30% of interpreter-style loops | not touched | |
| P3 | Pair guest `BL`/`BLR` with LK=1 host branches and guest `RET` with `blr`; budget link stack 64 (16 under SMT4) | PIPE Rule 3, §4.4 | `BranchOps.cpp`, frontend `BranchHint` | +25.5 cycles per unpaired return avoided | not touched | |
| P4 | At most one count-cache branch per aligned 32-byte block; emit `mtctr` as early as possible | PIPE Rule 4 | block emitter, dispatcher | +16 cycles and a guaranteed mispredict avoided | not touched | |
| P5 | Flags in CR fields; never read XER on hot paths; never clear XER.SO | PIPE Rule 5, §4.2; FP §13 | `ALUOps.cpp`, flag lowering | +4…+11 cycles per XER read; 147 cycles per SO toggle | not touched | |
| P6 | Block layout for fetch redirect: fall through on the common path, one taken branch per block | PIPE Rule 6 | block builder/layout | ~7-cycle loop floor; avoid extra taken branches | not touched | |
| P7 | Lightest barrier/exclusive forms the memory model allows (`larx`/`stcx.` 74 vs 23 cycles; `sync` +42, `lwsync` +14) | PIPE Rule 8, §4.6 | exclusive monitor, atomics lowering | LSE/LL-SC heavy code | not touched | |
| P8 | Constants: materialise off the critical path; load them when they're on it; reuse pinned zero | PIPE Rule 9; FP §3.3; NEON §2(b) | constant emission | per-op re-materialisation removed | not touched | |
| P9 | Never let a wider load consume two narrower stores | PIPE Rule 10 | context layout, load/store lowering | +21 cycles and no forwarding avoided | done (this commit), dropped on measurement | The frontend writes every context GPR slot with one 64-bit store and reads it with one 64-bit load, so the JIT has no narrow-store/wide-load pair on guest state. `pm_cmplu_stall_lhs` for `gcc -O2 lvm.c`: 23.3 M under POWERarm, 20.0 M for native ppc64le gcc on the same input, so the stalls come from the guest program's own memory access pattern |
| P10 | Multi-block/region compilation (currently one guest block per compile) | PIPE §8; M1 TODO | frontend block formation, IR passes | enables P1 across block edges | done f90005f75 | The A64 decoder now grows a compile unit along B, B.cond, CBZ/CBNZ and TBZ/TBNZ targets within 128 bytes of the branch, up to 8 blocks (`POWERARM_MULTIBLOCK=0` or `POWERARM_MAXINST=1` restore one block). Profile first (`cc1 -O2 lvm.c`, CPU 104): context-backed GPR loads/stores held about 3% of JIT samples, but 29% landed on the first instruction of a block, the build had 4.3 G taken branches (4.8x native gcc) and 1.07 G L1 icache misses (18x), and translation was 43% of the Lua build's user cycles, so block transitions and compile cost ranked first. Region size was chosen on the builds: a 16 KiB window cost cold start 35% (`gcc -c empty.c` 0.41 to 0.55 s). Result on CPU 104 against f470d805f on CPU 104, 2 runs each: `gcc -O2 -c lvm.c` 12.32/12.32 to 9.75/9.70 s (-21%); zlib 134.0/132.9 to 123.5/123.2 s (-7.6%); Lua 127.1 (outlier)/118.9 to 106.8/107.3 s (-10%). A64Bench warm (CPU 104, 3x3): vm 3311 to 2640 ms (7.73x to 6.17x the Pi), crc32 unchanged (1450), sha256 +1.1%, bst +2.2%, sort +2.7% |
| P11 | Run one guest thread per core with SMT siblings idle (scheduling/pinning policy, docs) | PIPE Rule 7, §4.7 | launcher/config, docs | vm 1.9× slower with a busy sibling | not touched | |

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

## Follow-ups to measure

- `mulld` measured 8.7 cycles vs the documented 5 (PIPE §8).
- Whether mixed integer/vector workloads use more of the core's width (issue-queue stall events).
- Half-precision conversion `xscvhpdp` at 38 cycles (NEON §7): is there a faster exact path?
