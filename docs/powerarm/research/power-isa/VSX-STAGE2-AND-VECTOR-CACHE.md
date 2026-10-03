# VSX register classes, continued: what the low bank actually carries, and the two routes left

Written 2026-10-03 against the tree at `df080ff1f` (Stages 0 and 1 of `VSX-REGISTER-CLASSES.md`
merged as `8edbf68df` and `c87bd6aad`; the promoted stable is `fcd525750`, which has both).
Every number says where it came from: (1) source reading of `JIT/PPC64LE/{VectorOps,A64FPOps,
MemoryOps,ALUOps}.cpp`, `ArchHelpers/PPC64Emitter.cpp`, `IR/Passes/RegisterAllocationPass.cpp`,
`A64Frontend/IRBuilder.{h,cpp}` and `IR.json` [CODE]; (2) the shape probe of
`VSX-REGISTER-CLASSES.md` §5 extended to the Stage 2 shapes and run on
`build-powerarm/Bin/POWERarm` in four configurations -- switch off and on, ISA 3.0 and
`disableisa30` -- with `POWERARM_CODEHASHLOG` and the code cache off, so every block is emitted
and counted (`vsx-probes/gen_shapes2.py`, `shapes2-counts.txt`) [MEASURED]; (3) a native
ppc64le probe of the VSCR this host hands a fresh process, and a guest probe of the f32 and
f64 compare arms on denormals in three FPSR/FPCR states, under both the stable and the build
emulator (`vsx-probes/vscr_native.c`, `denorm_guest.c`) [MEASURED]; (4) a static census of
the FP/SIMD instructions of eight guest binaries this port runs -- libc, libm, libstdc++,
libz, libpixman, libcairo, Firefox's libxul and Google Chrome's main binary -- classified by lowering family and by whether
any operand is V16-V31 (`census_classify.py`, `census-summary.txt`, `census_targeted.py`)
[MEASURED, unweighted by execution]; (5) post-RA IR dumps (`POWERARM_DUMPIR=stderr`) of two
real runs under the build emulator with the switch on, and one with it off: the code-server
`node` running the V8 double kernel of §5.3 of the previous document, and the Claude CLI's
`--version` (`irdump_analyze.py`, `irdump_producers.py`, `irdump-results.txt`) [MEASURED];
(6) one timing of that kernel, three runs each way alternating, on a box at load average 168
[MEASURED, direction only]. No build. No source file changed. **The Pi was unreachable
throughout** (`ssh pi5`: no route to host), so nothing here is goldened and no new test is
proposed as if it were.

**One-paragraph answer.** Stage 1 does what it claims structurally and its value in a real
workload is still unmeasured; the one number that stood for it was never Stage 1's (§1). The
census says where the low bank's traffic actually is, and it is not where the previous
document looked: in GCC-built code 57-88% of *all* FP/SIMD instructions touch V16-V31,
because GCC allocates v16-v31 before the callee-saved v8-v15, and the single largest
population is loads and stores -- `ldp/stp q16,q17` struct copies and `ldr/str d16` -- not
arithmetic (§2). The two real runs agree from the other side: of the low-bank copies Stage 1
leaves behind, about half feed or are fed by `LoadMem`/`StoreMem`, which are the one Stage 2
item this document finds both large and cheap, because the thing that kept them out of Stage
1 -- the POWER8 aligned `lvx`/`stvx` tier -- is dead code in this tree: there is no x86
frontend, the A64 frontend never certifies alignment, and it never emits the TSO forms (§4.1).
Of the rest of Stage 2, the f32 compares are settled and safe (the VSCR Java-mode difference
is nil on this host and nothing reads the FPSCR bits that differ, §4.2), the zero immediate
and the 32/64-bit inserts are worth an IR split (§4.3-4.4), and the round-to-nearest-even
question dissolves: there are no RNE vector converts in any of the five libraries counted, so
no row has to grow and no rule has to bend (§4.6). Stage 2b has no customers: every residual
low-bank load in both runs has only VMX consumers, zero mixed (§3). The vector register cache
of §6.5 is not blocked by either thing §6.5 said: the GPR cache it would mirror keeps every
store in place, so there is no drain discipline to pay, and the RA cross-block blocker was
removed by warm G6 on 2026-09-22 (`RegionPred`), which HANDOVER item 11 still lists as
"blocked on RA". In its cheap form it would remove 70-80% of the residual low-bank load
copies in Tier C code and it subsumes Stage 2b; its value is bounded by how much Tier C code
on V16-V31 the port runs, which the census puts at 22% of pixman's FP/SIMD instructions,
9% of libxul's and 8% of Chrome's (§5). The order this argues for: one Octane run each way by the owner before
anything else, because it decides whether the FP path is worth more work at all; the
loads/stores item regardless, because it is general-code and a dead-code deletion; the cache
after it; the small splits as one follow-up; and 2b, the RNE arms, `VUnZip`/`VTrn`, the
non-zero immediates and the 8/16-bit `VMov` declined with the numbers that decline them (§6).

## 0. The ranked conclusions

| # | Finding | Source | What it decides |
|---|---|---|---|
| 1 | The Box2D 9658 -> 16105 figure was taken with the switch parsed as off; Stage 1's only real-workload timing here is one V8 kernel on a contended box, within noise either way | §1 [CODE][MEASURED] | Stage 1 rests on static counts and structural coverage, not on a workload number; the owner's Octane flip is the missing measurement |
| 2 | In GCC-built code the majority of FP/SIMD instructions mention V16-V31 (libc 72%, libm 57%, libpixman 70%, libstdc++ 88%); loads/stores are the largest low-bank family everywhere | §2 [MEASURED, static] | the low bank is not an FP-loop corner; `LoadMem`/`StoreMem` is the Stage 2 item with the population |
| 3 | After Stage 1, residual low-bank copies have `StoreMem` as 49% of their consumers and `LoadMem` as 48-57% of their producers, in both real runs | §3 [MEASURED] | same item, from the dynamic side |
| 4 | Mixed consumers: 0 of 859 and 0 of 313 residual loads | §3 [MEASURED] | Stage 2b declined |
| 5 | The `lvx`/`stvx` aligned tier, the TSO forms and the GPR-class 128-bit arm of `LoadMem`/`StoreMem` are unreachable from the only frontend in the tree | §4.1 [CODE] | the memory ops can be VSX-clean by deletion plus two bounce rewrites, with no per-arm predicate |
| 6 | VSCR.NJ is 0 at exec on this host and after every guest `msr fpsr`; the f32 and f64 compare arms already agree on denormals in every state; FPSCR sticky bits are never read | §4.2 [MEASURED][CODE] | the f32 `VFCMP*` twin swap is safe, -3.25 per shape |
| 7 | RNE vector converts (`frintn`/`fcvtns` .4s/.2d) occur 0 times in five libraries; `movi #0` occurs 124-825 times per library, 36-92% on the low bank; 32/64-bit `ins` 3034 times in pixman, 82% low-bank | §4.3-4.6 [MEASURED, static] | split the zero immediate and the 32/64 inserts; leave the RNE arms alone |
| 8 | 82-88% of residual low-bank loads re-load a register already touched in the block, 68-70% within 8 guest instructions; the RA region mechanism the cache needs has been merged since 2026-09-22 | §5 [MEASURED][CODE] | the cheap cache has no blocker and a measured hit rate; its value is Tier C volume |
| 9 | `str b`/`str h` cost 7 host instructions on both ISA levels and `ldr b`/`ldr h` 9 on POWER8; the f16 converts cost ~240 on POWER8; `ins v.s[i]` costs 14-16 on POWER8 | §8 [MEASURED] | fixes independent of this item, two of them inside §4.1 |

## 1. What Stage 1 rests on, said plainly

**The number that was wrong.** HANDOVER item 54 records Octane Box2D 9658 -> 16105/16121
"with `VSXClasses=on`". The option is a bool; `"on"` went through `strtoull` and parsed as
false until `ba758b72b` made the loader refuse it. Those runs were therefore with the switch
off, and whatever moved Box2D by 67% that day was not Stage 1. The brief suggests the
headless-pinned-versus-interactive difference; the same day's cold-path round (X3, the decode
and handler tables, L4) is the other candidate; this document does not re-derive it and the
HANDOVER line should lose its attribution.

**What is measured and holds.** (a) The static counts: regenerated here on the current build,
switch off and on, both ISA levels (`shapes2-counts.txt`): `fadd v.4s` on V16-V31 15 -> 11,
`fmin v.4s` 17 -> 14, `fcvtn v.2s,v.2d` 7 -> 6, and no row grew on either level. (b)
Structural coverage in FP code: in the 25 blocks of the V8 run that contain a scalar or lane FP
op and touch the low bank, 49 reads name vs16-vs31 directly and 48 writes land in them, against
4 loads and 12 stores still copied; in the Claude CLI's 3 such blocks, 42 and 39 against 1 and
20 (§3). The remaining copies in those blocks belong to `VExtractToGPR`, `Vector_FToI` and
`StoreMem`. (c) Across the whole V8 run the switch removed 13% of the low-bank loads (987 ->
859) and 15% of the stores (641 -> 542); the rest have VMX consumers or producers, which is
what §4 and §5 are about.

**What is not.** Time. The kernel ran 1.10 s and 1.13 s with the switch off, 1.10 s and 1.01 s
on, after one cold run each, with the box at load average 168 (the owner was building). That
is one direction, and the direction is "not visible at this noise" -- the kernel's hot loop
is a handful of blocks whose low-bank share was 18% of operand mentions, and the run is
dominated by node's startup. Octane is not on this machine. The measurement that decides
whether the FP path deserves more register work is the owner's: flip `"VSXClasses"` in
`~/.config/powerarm/Config.json` -- it is a real bool now -- and read the Box2D and
NavierStokes rows once each way. Everything in §4-5 below that is specific to FP code waits
on that; the loads/stores item (§4.1) and the cache (§5) do not, because their populations
are general code and integer NEON.

## 2. Who uses V16-V31: the static census [MEASURED, unweighted]

`census_classify.py` reads `objdump -d` of each binary (the rootfs binutils, run under the
stable emulator by absolute path -- the host `objdump` cannot read AArch64) and classifies
each FP/SIMD instruction by the lowering family it reaches: **S1** (Stage 1's set: scalar FP,
the float lanes, the logicals, f64 compares), **S2** (this document's set: 32/64-bit
permutes, inserts, extracts, dups, immediates, converts, f32 compares), **C** (VMX by
semantics, including the byte/halfword arms of the S2 ops), and **ldst** (`ldr/str/ldp/stp`
of a V register, `ld1/st1`). "low" is the share of that family with any operand in V16-V31.

| binary | insns | FP/SIMD | % | low-bank share | S1 (low) | S2 (low) | C (low) | ldst (low) |
|---|---|---|---|---|---|---|---|---|
| libc.so.6 | 293k | 3,713 | 1.3 | **72%** | 564 (64%) | 303 (75%) | 412 (73%) | 2,434 (74%) |
| libm.so.6 | 91k | 30,445 | 33.6 | **57%** | 16,416 (78%) | 1,297 (51%) | 466 (72%) | 12,266 (28%) |
| libstdc++.so.6 | 395k | 2,540 | 0.6 | **88%** | 295 (71%) | 342 (94%) | 221 (98%) | 1,682 (88%) |
| libz.so.1 | 14k | 89 | 0.6 | 82% | 4 | 3 | 22 | 60 (88%) |
| libpixman-1 | 125k | 30,716 | 24.5 | **70%** | 8,748 (90%) | 3,763 (84%) | 11,637 (59%) | 6,568 (54%) |
| libcairo | 230k | 15,781 | 6.9 | **67%** | 7,690 (69%) | 347 (82%) | 806 (74%) | 6,938 (63%) |
| libxul.so (Firefox) | 28.8M | 1,613,815 | 5.6 | 21% | 429k (30%) | 143k (26%) | 300k (46%) | 742k (5%) |
| chrome (Google Chrome) | 56.4M | 2,041,578 | 3.6 | 16% | 403k (20%) | 215k (14%) | 320k (**48%**) | 1,103k (5%) |

Three things the table says that the previous document did not know.

**The low bank is the normal case in GCC-built code.** The six Arch Linux ARM libraries are
GCC builds, and GCC's AArch64 allocation order takes v0-v7, then v16-v31, then v8-v15
[INFERENCE from the shares; consistent with `SCALAR-FP-LOWERING.md` §9's nbody in d16-d31].
Every function with more than eight live vector values, and every leaf that wants to leave
the argument registers alone, lands in v16-v31. libxul and Chrome are the clang-built counterexamples
(Arch builds Firefox with clang; Google builds Chrome with its own clang [INFERENCE]):
21% and 16% overall, 5% of their loads and stores -- and in both, Tier C is the family
that sits on the low bank: 46% and 48% of their integer NEON.

**Loads and stores are the largest low-bank family, by a wide margin.** `census_targeted.py`
counts the pairs: `ldp q/d` 1080 in libm (712 low-bank), 1326 in cairo (670), 295 in libc
(267), 296 in libstdc++ (296); `stp q/d` 349 (184), 1112 (610), 638 (386), 508 (500). In
libstdc++ 500 of 508 `stp q` mention V16-V31: these are GCC's inline struct and buffer
copies, `ldp q16,q17,[x1]; stp q16,q17,[x0]`, which every C++ program carries. Each of those
costs one `xxlor` more than its V0-V15 twin today (`ldr_q` 2 vs 1, `str_d` 4 vs 3,
`shapes2-counts.txt`), and the pair forms cost two.

**Tier C is as large as the Stage 1 families in vector-heavy code.** libpixman: 11,637 C
against 8,748 S1, 59% of C on the low bank -- 22% of all its FP/SIMD instructions are
integer NEON on V16-V31, each paying +3 host instructions (`add_v4s` 4 vs 1). libxul: 300k C
against 429k S1, 46% low-bank, 8.6% of its FP/SIMD instructions; Chrome: 320k C against
403k S1, 48% low-bank, 7.6%. That is the population §5's
cache exists for, and it is Skia, the image codecs, and crypto -- the code behind Chrome,
VS Code and Firefox rendering -- not JIT output.

What the census cannot say: which of these instructions execute. A struct copy in a hot
loop and one in an error path count the same. §3 is the execution-side check on two runs.

## 3. Where the copies go after Stage 1: two real runs [MEASURED]

`irdump_analyze.py` reads the post-RA dump block by block. A `LoadRegister #0x10..#0x1f, FPR`
that still appears was refused coalescing; an op that names `V16`..`V31` as an argument
reads the pinned register directly; an op whose destination is `V16`..`V31` was hoisted.
Consumers of a refused load are traced through its physical register until it is redefined.

| | node + V8 kernel (switch on) | Claude CLI `--version` (switch on) |
|---|---|---|
| blocks compiled | 285,368 | 128,481 |
| blocks touching V16-V31 | 185 | 71 |
| direct reads / hoisted writes (Stage 1 working) | 100 / 99 | 110 / 107 |
| residual `LoadRegister` / `StoreRegister` copies | 859 / 542 | 313 / 259 |
| residual loads whose consumers are all VMX / mixed / all clean | **859 / 0 / 0** | **313 / 0 / 0** |
| top consumers of residual loads | `StoreMem` 419, `VAESE` 274, `VAESMC` 69, `VAdd` 28, `VUnZip` 18 | `StoreMem` 152, `VAdd` 65, `VUXTL2` 16, `VUXTL` 16, `VCMPEQZ` 14 |
| top producers of residual stores | `LoadMem` 311, `VAESE` 78, `VAESMC` 69, `VectorImm` 30 | `LoadMem` 125, `VAdd` 39, `VectorImm` 20, `VDupFromGPR` 15 |
| residual loads re-loading a register touched earlier in the block | 706 (82%) | 274 (88%) |
| of those, within 8 guest instructions | 602 (70%) | 212 (68%) |
| reuse distance, median / p90 guest instructions | 2 / 10 | 4 / 19 |
| per low-bank block: residual loads, distinct low-bank registers | 4.6, 2.4 | 4.4, 2.6 |
| 8- and 16-bit `VMov` in low-bank blocks | 6 | 12 |

Four readings.

- **Mixed consumers do not occur.** A V16-V31 value is either wholly in the VSX-clean world
  or wholly in the VMX one, in both runs. Stage 2b -- the shared VMX copy for mixed
  consumers, §6.2 of the previous document -- has nothing to do and is declined.
- **Memory ops are half of what is left.** `StoreMem` is 49% of residual load consumers in
  both runs; `LoadMem` is 57% and 48% of residual store producers. These are `str q16` and
  `ldr q16`, the struct copies the census found, and node's `stp q16,q17` on its own
  startup path.
- **FP blocks are already covered.** Restricting to blocks that contain a scalar or lane FP
  op: V8 run, 25 blocks, 49 direct reads and 48 hoisted writes against 4 residual loads and
  12 residual stores; the 4 loads feed `VExtractToGPR` (2), `Vector_FToI` (1) and
  `StoreMem` (1). Claude, 3 blocks, 42 and 39 against 1 and 20. Stage 1 is doing in FP code
  what §6.2 said it would; the residue there is small.
- **The rest is integer NEON, and it re-loads.** Both runs' startup paths are AES
  (`VAESE`/`VAESMC`: OpenSSL's self-test in node, with keys in V16-V31), `VAdd`, the
  widening `VUXTL`, compares and table lookups -- Tier C, where the only lever is §5's
  cache -- and 82-88% of their low-bank loads are re-loads of a register loaded or stored a
  median of 2-4 guest instructions earlier.

Neither run is FP-heavy, and neither executes any JIT-compiled NEON. They are two startup
paths plus one small kernel. They agree with the static census on every point they can
check, which is the reason to trust the census for the rest.

## 4. Stage 2, item by item

The §7.3 rule stands: an op is flagged only when every arm is VSX-form on both ISA levels;
otherwise it is split at the IR level into opcodes the frontend chooses between, or left.
Each item below says which, and what the count is for.

### 4.1 `LoadMem` / `StoreMem` / `LoadMemPair` / `StoreMemPair`: VSX-clean by deletion [CODE]

The previous document left these alone because the POWER8 aligned `lvx`/`stvx` tier would
need a "skip for low-bank targets" predicate, and because the pre-3.0 size-1/2 bounce puts
`lvx`/`stvx` on the operand. Three facts remove the first reason outright:

- `UseAlignedV128Access` (`MemoryOps.cpp:821`) fires only for a 128-bit access whose
  `Op->Align` certifies 16 bytes. The A64 frontend passes `OpSize::i8Bit` at every
  `_LoadMem`/`_StoreMem` site (`TranslateSIMDLoadStore.cpp:46,129,220` and the stores), as
  `LoadMem`'s own comment says: "the frontend never certifies $Align, because AArch64 has
  no alignment requirement to certify".
- There is no other frontend. `FEXCore/Source/Interface/Core/` contains `A64Frontend` and
  nothing that produces IR from x86; the x86 `OpcodeDispatcher` is not in this tree.
- `_LoadMemTSO`/`_StoreMemTSO` are emitted at zero sites in `A64Frontend/`. The TSO handlers
  stay as they are; they are not in this item.

So `TryEmitAlignedV128Load/Store`, `MakeVmxAddr` and the GPR-class 128-bit arm of `LoadMem`
(`GetVReg` on a GPR-class destination, an x86 leftover) are dead code for this emulator and
can be deleted, which leaves the FPR path as: `lxvdsx` (splat fusion), `LoadUnalignedV128`
(`lxv`/`lxvx` on 3.0, `lxvd2x`+`xxpermdi` on 2.07 -- VSX on both), and `LoadFPRSized`. The
second reason is a rewrite that pays for itself:

| arm today | instructions | VSX form | instructions |
|---|---|---|---|
| `LoadFPRSized` 1/2, pre-3.0: zero JITScratch, `lbzx`, `stb`, `lvx` | 9 (`ldr_b` P8 row) | `lbzx`/`lhzx`, `mtvsrd`, `xxpermdi` against `VZERO_VSX` | 3 + address |
| `StoreFPRSized` 1/2, both levels: `addi`, `li`, `stvx`, `lbz`, `stbx` | 7 (`str_b` row, **ISA 3.0 too**) | 3.0: `xxpermdi`(dw1->dw0) + `stxsibx`/`stxsihx`; 2.07: `xxpermdi` + `mfvsrd` + `stbx`/`sthx` | 2 / 3 |
| sizes 4/8/16 | unchanged | already VSX | unchanged |

The `str b`/`str h` bounce on ISA 3.0 is a finding on its own (§8): `StoreFPRSized`'s
fast path covers "the two sizes the guest actually hits in bulk" and the emitter has
`lxsibzx`/`lxsihzx` but not their store twins (`Emitter.h:1467-1472`); `stxsibx`/`stxsihx`
are ISA 3.0 XX1 forms with full reach (previous document §3.1) and need two encoders and
two rows in the `llvm-mc` sweep. Nothing in the rewrite reads the JITScratch slot, so the
store-forwarding stall the bounce carried (`MemoryOps.cpp:747` names it) goes with it.

What it is worth, from `shapes2-counts.txt` and the census: `ldr q` on V16-V31 2 -> 1 on
3.0 and 4 -> 3 on P8; `ldr/str d` 4 -> 3; `ldp/stp q` pairs lose two each; `ldr/str b/h`
lose 1 on V16-V31 and, on V0-V15, `str b/h` 7 -> 2 on 3.0 and `ldr b/h` 9 -> 4 on P8. The
population is the largest in the census (§2) and half of the residue in both runs (§3).

What it costs: the handlers move to `DEF_OP_VSX`, which means `LoadFPRSized`,
`StoreFPRSized`, `LoadUnalignedV128`, `StoreUnalignedV128` and `lxvdsx` take `VSXR`, and the
address-form helpers -- `MakeAddrForm`, `LegalizeForm`, `MaterializeAddr`, `PrepV128Addr`,
which take `PPC64EmitterBase&` -- need a view-typed overload, since a view handler cannot
produce the base from `this` (that is the view's purpose). Stage 1 did the same for
`PositionElement0AsDouble` and `PlaceElement0` (`A64FPOps.cpp:51,62`). About a day. The
gate: the identity gate of Stage 0 restricted to units with no V16-V31 reference must come
out byte-identical on ISA 3.0 *except* the `ldr/str b/h` blocks, which change on purpose and
are the rows to read in `shapes2-counts.txt`; on `disableisa30` the 1/2-size blocks change
too. The existing `simd_*` and load/store goldens on disk cover both sizes on both banks;
a new golden for the rewritten bounces cannot be made until the Pi is back.

### 4.2 `VFCMPEQ` / `VFCMPLT` / `VFCMPLE` / `VFCMPUNO` f32 arms: settled, swap [MEASURED][CODE]

The file comment at `VectorOps.cpp:2280` names the hazard: VMX float ops are "Java/IEEE-mode"
-- `VSCR.NJ` set makes `vcmpeqfp` and friends flush denormal inputs to zero, which the VSX
`xvcmp*sp` never do, and the two arms of each compare would then disagree on a denormal.
Three facts settle it.

- **This host hands a fresh process NJ = 0.** `vscr_native.c`, a ppc64le program run
  natively, reads `mfvscr` as `00000000 00000000 00000000 00000000`; its `vcmpeqfp` of
  1e-40f against 0 is `00000000`, the same as `xvcmpeqsp`. (The kernel's
  `start_thread` writes "Java mode disabled" into `vscr.u[3]`, which on little-endian is not
  the VSCR word [INFERENCE]; the measured value is what counts.)
- **The emulator never sets NJ.** The only `mtvscr` is `DEF_OP(StoreFPSR)`
  (`ALUOps.cpp:4397`), which builds VSCR from the guest FPSR's QC bit alone -- NJ lands as 0
  on every guest `msr fpsr`. `SetRoundingMode` drops guest `FPCR.FZ` by design
  (`ALUOps.cpp:3834`).
- **The guest sees the same thing from both arms.** `denorm_guest.c` runs `fcmeq`,
  `fcmgt` and `fmin` on `.4s` and `fcmeq`/`fadd` on `.2d` with a denormal against zero,
  fresh, after `msr fpsr, xzr`, and under `FPCR.FZ=1`, under both the stable and the build
  emulator: `fcmeq.4s = 0`, `fcmgt.4s = ffffffff`, `fadd.4s(den+den) = 00022d84`, identical to
  the `.2d` VSX arm's answers in all three states. (Under `FZ=1` real hardware would flush and
  give `fcmeq = ffffffff`, `fadd = 0`; the emulator's FZ divergence is the one
  `ALUOps.cpp:3834` documents, and it is the same for both arms.)

The remaining difference is FPSCR: `xvcmp*sp` set `VXSNAN`/`VXVC` on NaN inputs, `vcmp*fp`
set nothing. Nothing reads them: `DEF_OP(LoadFPSR)` synthesises the guest FPSR from
`CPUState.fpsr` and `VSCR.SAT` (`ALUOps.cpp:4357-4382`), the frontend never asks for
IOC/IXC, and the only host-FPSCR reads in the backend are of the RN field (`mffs`/`mffsl`
at `A64FPOps.cpp:107,371` and `ALUOps.cpp:3823-3889`). FPSCR.VE is 0, so no trap. The swap
is `vcmpeqfp -> xvcmpeqsp`, `vcmpgtfp -> xvcmpgtsp`, `vcmpgefp -> xvcmpgesp`, same count,
four arms (`VFCMPGT`/`ORD`/`NEQ` are not A64-reachable; swap them in passing). The
`fcmeq_v4s`/`fcmgt_v4s` rows go from 5.50 to 2.25 on V16-V31, the largest per-shape gain
left in the float family; the census puts the f32 compares at 68 in pixman (all low-bank)
and nowhere else in the five libraries, and in `FloatCompareKind` sites they are the
`fcmeq/fcmgt/fcmge/fcmlt/fcmle` and `facge/facgt` lanes (`TranslateSIMDFloat.cpp:268-277`).
Small population, zero risk, one line per arm.

### 4.3 `VectorImm`: split out the zero [CODE][MEASURED]

Only the zero arm can be clean without growth: `xxlxor` (or a copy of `VZERO_VSX`). A byte
splat is `xxspltib` on 3.0 but has no 2.07 form short of `mtvsrd`+`xxpermdi`+(no byte splat
at all); a word splat in -16..15 is `vspltisw` in one instruction against `li`+`mtvsrws`
in two; a halfword has nothing. So the non-zero arms stay VMX and the op cannot be flagged.
The zero is worth its own opcode: `movi #0` appears 825 times in libm (451 low-bank), 282
in cairo (102), 258 in pixman (230), 227 in libstdc++ (209), 124 in libc (105), and it is
`VectorImm`'s share of the residual producers in §3 (30 and 20). `movi0` on V16-V31 goes
2 -> 1. One IR entry, one frontend branch at the `movi` site, one `DEF_OP_VSX`.

### 4.4 `VInsElement` / `VInsGPR`: split by element width [CODE][MEASURED]

The 64-bit arm is `xxpermdi` on both levels already. The 32-bit arm on 3.0 is
`vspltw`+`vmr`+`vinsertw` today and `xxspltw`+`xxlor`+`xxinsertw` in VSX -- `xxinsertw`
takes word 1 of its source's doubleword 0, which is where `xxspltw` (and `mtvsrd`, for
`VInsGPR`) put it -- same count. The 32-bit arm on POWER8 is the general `vperm` with a
control built through the stack: 14 instructions for `ins v.s[1],v.s[2]` and 16 for
`ins v.s[1],w1` (`shapes2-counts.txt`, `ins_ss`/`ins_sw` P8). A VSX form is `xxspltw` the
source, a word mask from `li`/`mtvsrd`/`xxpermdi`/`xxspltw` (or the pool), and `xxsel`:
about 6, i.e. the P8 row shrinks. The 8/16-bit arms are `vinsertb/h` on 3.0 and the stack
`vperm` on P8, and no VSX form exists for them on P8, so the split is by element width:
`VInsElement`/`VInsGPR` keep 8/16, a 32/64 opcode pair moves to the view. The frontend
knows the width at all 21 sites. Population: 3034 32/64-bit `ins`/`mov v.s[i]` in pixman
(2499 low-bank), 104 in cairo (91), 23 in libc (22); `ins_ss` on V16-V31 5 -> 2, `ins_dd`
4 -> 1. Worth doing; pixman-shaped code (Skia, codecs) is where it lands.

### 4.5 `VDupFromGPR`, `VDupElement`, `VExtractToGPR`, `VExtr`, `VZip`/`VZip2`: the same split, smaller [CODE][MEASURED]

Each has clean 32/64-bit arms at equal or lower count and VMX byte/halfword arms with no
P8 form:

- `VDupFromGPR` 32/64: `mtvsrws`/`mtvsrdd` on 3.0 already; P8 `vspltw -> xxspltw` and the
  64-bit `vmr` tail becomes the `xxpermdi` writing `Dst` directly (-1). 8/16 need `vspltb/h`.
- `VDupElement` 32: `vspltw -> xxspltw`; 64 already `xxpermdi`; its Q=0 tail
  `vspltisb`+`xxpermdi` can take the zero from `VZERO_VSX` (-1, V0-V15 too). 8/16: no form.
- `VExtractToGPR` 32 on 3.0: `xxextractuw` at byte offset `(3-i)*4` puts the word where
  `mfvsrwz` reads it, 2 for 2; on P8 the `vsldoi` rotate becomes `xxsldwi`; 64-bit on P8
  `vsldoi`+`mfvsrd` becomes `xxpermdi`+`mfvsrd`. 8/16 on 3.0 would be 3 for 2
  (`xxextractuw`+`mfvsrwz`+`rlwinm`): a V0-V15 row grows, so they stay VMX.
- `VExtr` by 4/8/12 with Q=1: `vsldoi -> xxsldwi` by 1/2/3 words (8 is also `xxpermdi`);
  other indices and the Q=0 arm (`vsldoi` by arbitrary bytes) stay.
- `VZip`/`VZip2` 32: `vmrglw/vmrghw -> xxmrglw/xxmrghw`; 64 already `xxpermdi`; the Q=0 arm's
  `vspltisw`+`vsldoi`×2 is `xxlxor`+`xxsldwi`×2 for the 32-bit case. 8/16 stay.

Populations (five libraries, n / low-bank): 32/64 `dup` 62/31 pixman, 36/32 cairo, 22/21
libc, 17/17 libstdc++; `umov`/`fmov w,v.s[i]` 94/1 libm, 20/19 libc, 10/10 cairo -- the
32-bit extract almost never reads the low bank; `ext` 29/29 libstdc++, 9/9 libc; `zip1
.s/.d` 32/32 cairo, 19/19 pixman. Each split is the §4.4 shape again at a tenth of the
volume. Do them as one change when someone is in the file; none justifies its own.

### 4.6 `A64VecFloatToInt` and `Vector_FToI`: the rule does not have to bend [CODE][MEASURED]

The brief's framing was right about the instruction: `vrfin` is a fixed round-to-nearest-even
with no VSX twin, `xvrspi` is ties-away, and `xvrspic` honours FPSCR.RN, so the f32 RNE arm
needs the `EmitRoundNearestEven` bracket -- which already has a fast path that skips the
FPSCR write when the guest's FPCR.RMode is RN (`A64FPOps.cpp:93-120`), so the executed cost
is `lwz`+`rlwinm`+`cmpwi`+`bc`+`xvrspic` = 5 against 1, +8 static on 3.0, on V0-V15 code too.
(`frintn_v2d` and `fcvtns_v2d` on V0-V15 already pay it: 9 and 12 in the table.) The other
f32 arms are exact twins (`vrfim/p/z -> xvrspim/p/z`, as the brief noted; `Vector_FToI`'s
`Nearest`/32 arm is the only one left).

Two things make the question moot. First, the §7.3 shape exists: `Rounding` and
`ElementSize` are static at both frontend sites (`TranslateFP.cpp:260-265`,
`TranslateSIMDFloat.cpp:364,427`), so the f32-RNE case can be its own VMX opcode and the
rest of each op flagged, with no row growing anywhere. Second, nobody emits it:
`census_targeted.py` finds **zero** `frintn`/`fcvtns`/`fcvtnu` vector forms in libm, libc,
libstdc++, libpixman and libcairo, 2 and 6 truncating `fcvtzs` vector forms, and zero
`fcvtn`/`fcvtl` 32<->64 narrows and widens. Scalar `fcvtzs` goes through `A64FloatToGPR`,
already clean. So: leave both ops alone. The split costs an opcode and buys `fcvtzs_v4s`
5 -> 3 and `frintz_v4s` 3 -> 1 on V16-V31 for code that the census says does not exist in
these libraries; JIT engines use the scalar forms. No evidence supports bending "no row
may grow", and none is needed.

### 4.7 `Vector_FToF`: leave [CODE][MEASURED]

The f16 arms are halfword merges (`vmrglh`, `vpkuwum`) on 3.0 -- VSX has no halfword
granularity, and `xxperm` with a pool control would grow the V0-V15 row -- and on POWER8 a
software helper of about 240 host instructions with `stvx`/`lvx` on the operands
(`fcvtl_sh` 240, `fcvtn_hs` 245 in the table). The 32<->64 arms are `vmrglw`/`vmrghw`, exact
twins. The IR split is the §7.3 shape and would make `fcvtl v.2d,v.2s` 4 -> 2 and `fcvtn`
6 -> 5 on V16-V31. The count against it is the same as §4.6: zero such instructions in the
five libraries. Leave it; note the shape.

### 4.8 `VUnZip` / `VUnZip2` / `VTrn` / `VTrn2`: no twin without growth [SPEC][CODE]

`uzp1 .4s` is `vpkudum` (ISA 2.07, one instruction); `trn1/trn2 .4s` are `vmrgow`/`vmrgew`
(2.07). None has a VSX form on any level: the word-granularity VSX permutes are
`xxmrghw`/`xxmrglw` (same-index interleave), `xxsldwi`, `xxpermdi` and (3.0) `xxperm` with
a control register. A control from the pool is +1 on 3.0 and has no 2.06 equivalent; the
even/odd gathers cannot be composed from `xxmrg*w`/`xxsldwi` in fewer than three. The 64-bit
arms are `xxpermdi`, but a 64-bit-only split buys nothing the frontend could not get by
emitting `VZip` for `.2d`. Leave. `uzp1_v4s` stays at 4 on V16-V31.

### 4.9 `VMov` 8/16: the cost Stage 1 paid is not worth recovering [MEASURED]

Stage 1 made the B/H-register result tail 3 -> 3/4 (3.0/P8) for bytes and 3 -> 4/5 for
halfwords. Stage 2 neither worsens nor improves it. The count says it does not matter: 6
such ops in the V8 run's low-bank blocks, 12 in the Claude CLI's, out of 1,025 and 507
vector ops; A64 produces B/H results from FP16 and a handful of element ops. The only
improvement would be a pool mask (`lxv` + `xxland`, 2) and only if the pool is within the
DQ-form displacement of `STATE`, which `LoadNamedVectorConstant`'s `LoadConstant`+`add`
suggests it is not [INFERENCE]. Leave.

### 4.10 What Stage 2 is, then

| # | item | kind | per-shape gain on V16-V31 | population | size | gate |
|---|---|---|---|---|---|---|
| 1 | §4.1 memory ops | delete the dead tier, rewrite two bounces, flag four ops | `ldr q` 2->1, `ldr/str d` 4->3, pairs -2; plus `str b/h` 7->2 and P8 `ldr b/h` 9->4 on every bank | largest in the census; half the residue in both runs | ~1 day | identity gate (ISA 3.0, non-V16 units, excluding the b/h blocks), `simd_*`/`ldst` goldens on disk, the shape table |
| 2 | §4.3 `VectorZero` | IR split | `movi #0` 2->1 | 124-825 per library, 37-91% low-bank | hours | floor + table |
| 3 | §4.4 `VInsElement`/`VInsGPR` 32/64 | IR split by width; P8 32-bit via `xxsel` | 5->2, 4->1; P8 14-16 -> ~6 | 3034 in pixman | ~1 day | floor + `simd_*` + table |
| 4 | §4.2 f32 compares | twin swap | 5.5->2.25 | 68 in pixman; FP-lane code | hour | floor + table |
| 5 | §4.5 the small splits | IR split by width/index | 1-3 each | tens per library | ~1 day together | floor + table |
| -- | §4.6-4.9 | declined | -- | 0 or negligible | -- | -- |

## 5. The vector register cache, re-examined

### 5.1 The shape that works needs no drain

§6.5 of the previous document said the cache "would also need the same drain discipline:
every instruction that may raise a signal has to see the pinned registers current, which in
FP loops is every load and store". That describes a cache that defers the write-back to
exits. The GPR cache it would mirror does not do that: `StoreGPRSlot` emits every
`StoreContext` in place and only *records* the value so that a later `LoadGPRSlot` within
`GPR_CACHE_WINDOW` (8 guest instructions) reuses the SSA value instead of reloading
(`IRBuilder.cpp:302-331`; "every StoreContext stays where it was, so CPUState is still exact
at every guest instruction boundary"). The vector analogue is the same: `StoreV` of V16-V31
keeps emitting its `StoreRegister` (which Stage 1 hoists when the producer is clean and
leaves as one `xxlor` otherwise), and `LoadV` of V16-V31 returns the last loaded or stored
SSA value for that register if it is within the window and in the current region, else
emits `LoadRegister` as now. The pinned register is current at every instruction boundary
because nothing is deferred; a signal taken anywhere sees exactly what it sees today;
`sigfploop` remains the test, unchanged. There is nothing to quantify about drain cost
because there is no drain. The deferred-store form would need the pinned register current
before every memory op (the only faulting ops in FP code, since FPSCR.VE is 0 and FP
exceptions do not trap), which in a loop that loads and stores every few instructions is a
write-back per op -- that form is dismissed, and it was the only form the blocker applied to.

What the cheap form costs per Tier C op: today 2 loads + 1 store around a 2-source op, +3.
With the cache, loads of a register touched in the window are free; the store stays. §3
measured the hit rate on the residual loads: 82% and 88% re-load a register touched
earlier in the block, 70% and 68% within 8 guest instructions. So roughly +3 -> +1.3 for
the ops the cache reaches, and for a loop-carried value one load at the head and one store
at the tail per iteration instead of one of each per use.

### 5.2 The RA blocker is gone, and HANDOVER says otherwise

Checklist item 11 / HANDOVER item 11 reads "blocked on RA: `ConstrainedRAPass::Run` resets
`Class.Available` and spills per block; dynamic SSA values across blocks trigger allocator
assertion or SIGSEGV". That was true on 2026-09-17. Warm G6 landed on 2026-09-22
(`IRBuilder.cpp:287-301`): the GPR cache now survives an intra-unit edge whose successor has
exactly one in-unit predecessor, the frontend marks `IROp_CodeBlock::RegionPred`, and the
allocator treats the chain as one region -- registers not freed, spill slots not recycled at
the edge (`RegisterAllocationPass.cpp:150-170, 801-870`). The row in HANDOVER is stale. A
vector cache rides on the same `RegionPred` rows with no allocator change: a cached vector
value that survives an edge is an ordinary dynamic FPR live across a region, which the RA
already handles for GPRs. What is still not covered, for GPRs and vectors alike, is a loop
back-edge (a loop header has two predecessors), so a loop-carried V16-V31 value still pays
one load at the head and one store at the tail -- the same as today for the store, one
fewer than today for every use inside the body.

### 5.3 What it buys, and for whom

The cache's customers are the residual loads of §3 whose consumers are Tier C -- after
§4.1 takes the `StoreMem` ones, that is 440 of 859 in the V8 run and 161 of 313 in the
Claude CLI's, of which 70% would hit. In absolute terms those two runs would save some
three hundred and one hundred host instructions over their whole lives: nothing. The
population that matters is in §2: 22% of pixman's FP/SIMD instructions, 9% of libxul's and 8% of Chrome's
are Tier C on the low bank, i.e. the integer NEON of Skia, the image codecs, the crypto
routines and the string functions, where GCC put the working set in v16-v31 and every op
pays +3. The AES rounds in node's startup (`VAESE` 274 consumers, keys in V16-V31) are a
small live example. That is the code behind Chrome, VS Code and Firefox rendering, and the
census cannot weight it by execution; the owner's instrument for it is a page render in
Firefox or Chrome with the kill switch flipped, which is the measurement this document
asks for and cannot take.

The cache also subsumes Stage 2b for free: a load whose consumers are mixed would be
served from one copy. §3 says that case does not arise, so this is worth nothing, but it
means 2b never needs its forward-pass change.

### 5.4 Cost and risk

- **Pool pressure.** Fourteen dynamic VMX registers, four of them volatile. A cached value
  extends its live range by up to the window; §3 measured 2.4-2.6 distinct low-bank
  registers per block on average, but one block in the V8 run has 85 residual loads. The
  GPR cache's own sweep shows the shape of the risk: `vm` lost at window 32 as reused values
  were spilled (`IRBuilder.cpp:296-301`). The same sweep, once, on a NEON workload, is the
  cost measurement; the default should start at the GPR cache's 8.
- **Interaction with Stage 1.** A cached node gains readers. If all are clean it is
  coalesced as now; if any is VMX the RA refuses, as now, and every reader uses the one
  pool copy -- the clean readers lose their direct access but the total is one `xxlor`
  where today it is one `xxlor` for the VMX reader plus nothing for the clean one. Never
  worse, by construction; and §3 says the mixed case does not occur.
- **Correctness surface.** None new: no deferred state, no new RA decision, no new lowering.
  The post-RA validator of Stage 0 still polices every low-bank operand. The frontend change
  mirrors `LoadGPRSlot`/`StoreGPRSlot` for `LoadV`/`StoreV` on registers 16-31 (about 80
  lines, same window, same `CacheSurvivesEdge` rule, same `FEX_NOREGIONCACHE`-style kill
  switch), and `StoreVSized`'s `VMov` means the cached value is always the full 128-bit
  post-zero-extension value, which is what a reader wants.
- **Gates.** The floor; the identity gate on units without V16-V31 references (byte-identical,
  since the cache touches only low-bank slots); `simd_*` goldens on disk, whose `VREGS`
  span both banks; `MAXINST=1`, where no reuse can occur and the output must not change;
  the one window sweep.

### 5.5 Verdict

Worth building, in the cheap form only, after §4.1 and with its kill switch, and then
measured once by the owner on a rendering workload. It is the only route to the +3 on
integer NEON, the two things that were said to block it do not, its hit rate is measured
at about 70%, and its cost is a frontend change the size of the GPR cache's. What would
make the answer "no" is a spill regression in NEON-heavy blocks, which the window sweep
would show on the first run.

## 6. Recommendation, in order

1. **The owner runs Octane once each way** with `"VSXClasses"` flipped in `Config.json`,
   reading Box2D and NavierStokes. Ten minutes of wall clock. It decides whether the FP
   path (§4.2, and any future FP register work) deserves more, and it replaces the number
   in HANDOVER item 54 with one that is Stage 1's. Nothing below that is FP-specific should
   be started before it.
2. **§4.1, the memory ops, regardless of (1).** General code, dead-code deletion, two bounce
   rewrites that improve V0-V15 code on both levels, the largest population in the census
   and half the residue in both runs.
3. **§5, the vector cache,** cheap form, kill switch, window sweep once.
4. **§4.3, §4.4, §4.2, §4.5 as one follow-up**, in that order of value.
5. **Declined, with the number that declines each:** Stage 2b (0 mixed consumers of 1,172
   residual loads); the f32 RNE arms of §4.6 (0 RNE vector converts in five libraries);
   `Vector_FToF` (0 32<->64 narrows/widens); `VUnZip`/`VTrn` (no VSX form at equal count on
   any level); the non-zero `VectorImm` arms (a V0-V15 row grows on at least one level);
   `VMov` 8/16 (6 and 12 occurrences per run).

## 7. What this could not measure

- **Stage 1 in a real FP workload.** Octane is not on this machine; the one kernel timed
  here is a startup-dominated run on a box at load 168, and it shows no direction. This is
  the measurement the whole FP side waits on, and it is the owner's.
- **Execution weighting of the census.** Static instruction counts over seven binaries;
  a hot loop and an error path count the same. The two IR dumps are the only dynamic
  check, and both are startup paths.
- **JIT-compiled NEON.** Neither V8 nor JSC compiled any vector code in the runs here. The
  18% low-bank figure of the previous document is still the only JIT number.
- **The cache's spill cost.** Fourteen pool registers, a window of 8: the sweep that the
  GPR cache had (`IRBuilder.cpp:296`) has not been run for vectors, and cannot be without
  building it. The per-block maximum of 85 residual loads says a bad case exists.
- **The Claude CLI in the static census.** Its disassembly (234 MB through the emulated
  objdump, on a box at load 385) had not finished when this was committed; Chrome took 23
  minutes and is in. The CLI's dynamic slice is in §3.
- **POWER8.** Every `disableisa30` number is from the POWER9 running the 2.07 arms; no
  POWER8 was timed, and the P8 bounce rewrites of §4.1 are counted, not run.
- **Goldens.** The Pi was unreachable; `fp_lowbank` and `sigfploop` from Stage 1 are still
  inert, and nothing new here can be goldened until it is back.

## 8. Findings to carry out of this document, independent of the item

- **HANDOVER item 54's Box2D attribution is wrong** (the switch was off, §1); **item 11's
  "blocked on RA" is stale** (warm G6 merged the region mechanism on 2026-09-22, §5.2).
- **`str b`/`str h` cost 7 host instructions on ISA 3.0** through the JITScratch `stvx`
  bounce because the emitter lacks `stxsibx`/`stxsihx`; two encoders make them 2. **`ldr
  b`/`ldr h` on POWER8 cost 9** for the same reason and can be 4. Both are inside §4.1 but
  stand alone.
- **`fcvtl v.4s,v.4h` / `fcvtn v.4h,v.4s` cost about 240 host instructions on POWER8** (the
  software helper); on 3.0 they are 3 and 4. Any FP16 workload on a POWER8 is in trouble
  before register classes enter it.
- **`ins v.s[i], v.s[j]` and `ins v.s[i], w` cost 14-16 on POWER8** through a stack-built
  `vperm` control; §4.4's `xxsel` form is about 6 and is VSX-clean as a bonus.
- **The emulator ignores guest `FPCR.FZ`** on both VMX and VSX arms (confirmed by
  `denorm_guest.c`, documented at `ALUOps.cpp:3834`); not new, but now measured from the
  guest side.
- **`VDupElement`'s Q=0 tail** materialises a zero with `vspltisb` where `VZERO_VSX` is
  pinned; -1 instruction on every `dup` with a 64-bit result, any bank.
- **The host `objdump` is picked up under `taskset`.** `taskset` resolves to the host
  binary, and anything it execs resolves to host binaries too; the rootfs tools must be
  run through the emulator by absolute path. Cost this document an hour.

## 9. Files

- `vsx-probes/gen_shapes2.py`, `shapes2-counts.txt`: the Stage 2 shape probe and its four-configuration table (§4).
- `vsx-probes/vscr_native.c`, `denorm_guest.c`: the VSCR and denormal probes (§4.2).
- `vsx-probes/census_classify.py`, `census_targeted.py`, `census-summary.txt`: the static census (§2).
- `vsx-probes/irdump_analyze.py`, `irdump_producers.py`, `irdump-results.txt`: the post-RA dump analysis of the two runs (§3, §5).
