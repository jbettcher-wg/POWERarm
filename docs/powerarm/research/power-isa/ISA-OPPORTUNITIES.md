# POWER ISA opportunities: where the emitted code is longer than the host needs

Written 2026-09-23 from source reading only: `CodeEmitter/PPC64LE/Emitter.h`, the
`JIT/PPC64LE/*Ops.cpp` lowerings, the A64 frontend, the IR passes, `HANDOVER.md`, the
checklist, the four research docs and `git log`. No build, no instrumentation. Every host
instruction count below was taken by reading the emitter path for the op; every frequency
comes either from the executed-instruction census in `WARM-CODEGEN-RESEARCH.md` §5 (`cc1`
only) or from a static mnemonic census of the reference binaries that I ran with the host
`llvm-objdump` (§6: cc1, Firefox's libxul, VS Code's Electron binary, Factorio, the Claude
CLI, libc), and is labelled as one or the other. Where I extrapolate from a static count to
an executed share I say so; that is a guess until an implementation agent counts it.

**Already done, and not re-proposed here** (checked against the checklist and HANDOVER items
30-40): FPSR.QC via VSCR.SAT (N11); rounding modes via `mffscrn`/`mffscrni`/`mffsl` (F6, F7,
F8); vector FP16 via `xvcvhpsp`/`xvcvsphp` (N14) and scalar FP16 via `xscvhpdp`/`xscvdphp`
(`A64FPOps.cpp:EmitHalfToDouble`); `xxbr*` for REV (N7); `mtvsrws`/`mtvsrdd`/`mfvsrld`/
`vextu*rx`/`vinsert*` for DUP/INS/UMOV (N7); `xxspltib` splats; `vpermr` for TBL (N8);
`vabsdu*` (N6); AES via `vcipher*` (N9); `vmhaddshs` (N6); `xxpermdi` against the pinned zero
for scalar results (F4); CR0-direct FCMP consumers and branch-free FCSEL with `mtvsrdd` (F5);
`mcrxrx` for the XER projection (`JIT.cpp:962-1026`); `isel` for every select; compare+branch
and compare+select fusion for `SubNZCV`/`AddNZCV`/`SubWithFlags`/`AddWithFlags` (warm G2); the
vector-scan fusion (N1); `lxvdsx` load-and-splat for FMA operands; `vpmsumd` CRC32;
`lxvx`/`stxvx` spills; the `extsX.` record forms in `EmitTestNZSetCR`; `darn` for RDRAND;
`popcntd`, `cntlzd`, `rldimi`/`rlwimi`, `lq`/`lqarx`/`stqcx.`, DS-form `lxsd`/`stxsd`.

## 0. The ranked list

Rank is by expected value on the reference set (Firefox, VS Code, Factorio, the zlib/Lua
builds, the Claude CLI): static saving per site, times how often the site runs, discounted by
risk. Effort is given so the list can be scheduled, but it does not set the order. The
argument for each rank is in §3; this table is the conclusion.

| # | Item | Today (host insns) | After | ISA | Where it runs | Risk | Size |
|---|---|---|---|---|---|---|---|
| 1 | **NZCV dead at constant unit exits, flag-reading successors caught at link time** (§3.0). The gate on everything flag-shaped: today 54% of `cmp`+`B.cond` pairs keep their `subfco.` because a leg leaves the unit | W `cmp` 3 + projection kept at 54% of sites | dropped at most sites; items 2, 4, 5 reach full value | policy, not an instruction | every workload | medium-high | 3-5 days |
| 2 | **CCMP/CCMN fused into CR logic** (`cmp cr6; cmp cr7; crand; bc`) (§3.2) | 12-15 + one data-dependent host branch | 4, branch-free | base | 0.30% static in cc1, 0.24% Claude, 0.06-0.10% Firefox/VS Code; 0.7% executed in cc1 | medium | ~1 week |
| 3 | **Native `lwarx`/`stwcx.` for the LDXR..STXR loop** instead of the software monitor (§3.16) | LDXR 5, STXR ~43 + `hwsync`/`isync` | loop of ~4-7 | 2.06 | **Firefox only**: 0.76% of libxul is inline exclusives (219k sites, refcounts); ~0 elsewhere (outline atomics) | medium-high | 3-4 days |
| 4 | **TST/ANDS fused through record-form `andi.`/`and.`; TST as `TestNZ`** (§3.1) | 5 (W), 3 (X) | 2 (3-4 without item 1) | base | 0.65% static in cc1, 0.52% Claude, 0.41% VS Code, 0.25% Firefox; V8/JSC tag tests, GCC flag tests | low | 1-2 days |
| 5 | **Flag-liveness gate: `sradi`/`srad`/`sraw*` for ASR and SBFX, record forms for TBZ/CBZ** (§3.5) | ASR imm X 5, W 3, reg X 8, W 4; SBFX 5-6; TBZ 3 | 1-2; TBZ 2 | 2.06 | TBZ 2.3% static in cc1, 1.6% Firefox, 1.55% VS Code; ASR 0.22% in VS Code (V8 Smi untag) and Factorio | medium | 1-2 days |
| 6 | **Vector int<->float converts with VSX** (`xvcvsxwsp`, `xvcvspsxws`, ...) (§3.3) | 35-40 per 4-lane op | 1-4 | 2.06 | Firefox (23k static sites, 0.08%), Factorio (2.7k), VS Code (6k); zero in cc1 | low-medium | 1-2 days |
| 7 | **RBIT+CLZ -> `cnttzd`/`cnttzw`; RBIT via `vgbbd`/`xxbrd`** (§3.4) | 26 + a stack round trip (+3) | 1 (3-5 on 2.07) | 3.0 (2.07 fallback) | glibc string-function tails on every call; bitmap ctz in cc1, V8, JSC (0.05% static in Claude), allocators | low | half a day |
| 8 | **Register-offset addressing in X-form, `extswsli` for `sxtw #n`, `rldic` for `uxtw #n`** (§3.6) | 3-4 | 2 | 3.0 (`extswsli`), rest base | 0.5-1.2% static in every binary | low | half a day |
| 9 | **`VMov(i64)` against the pinned zero; FMOV D<-X via `mtvsrdd`** (§3.7) | 2; 3 | 1; 1 | 2.06; 3.0 | every D-register result; double constants in JIT code | none | hours |
| 10 | **SMULL -> `mullw`, MADD -> `maddld`, SMADDL -> `mullw; add`** (§3.8) | 3; 2; 4 | 1; 1; 2 | 2.07; 3.0; 2.07 | division by constants, hashing; 0.1-0.3% static | none | hours |
| 11 | **SDIV/UDIV without the double-select scaffold; `modsw`/`modsd` for the MSUB idiom** (§3.9) | UDIV 8-13, SDIV 12-18 | 3-4 / 6-7 | 2.07 (+3.0 for mod) | rare in hot code | low | 1 day |
| 12 | **Scalar-FP chain hygiene: dirty upper lane between scalar ops, `lxvdsx` for single-use loads** (§3.10) | 5 per scalar op, 2 per load | 4, 1 | 2.06 | Factorio (0.44% static scalar FP), Firefox (0.28%), JS numerics | medium | 2-3 days |
| 13 | **ADDP/UZP2 without pool-constant `vperm`** (§3.11) | 9 / 4 | 6 / 3 | 2.07 | reductions, N1 exit paths | none | hours |
| 14 | **Three-way compare idiom -> `setb`** (§3.12) | ~9 | 2 | 3.0 | comparators; rare | low | half a day |
| 15 | **Pre-index STP -> `stdu`** (§3.13) | 3 | 2 | base | prologues; cracked on POWER9, fetch only | medium | 1 day |
| 16 | **LSE atomics: CR0 save via `mcrf`; AMOs (`ldat`/`lwat`)** (§3.14) | 12 + a store-forward | 8 / 4 | base / 3.0 | negligible: LSE ops sit only in the outline-atomics helpers, ~2.5 M per V8 run | low | hours / 2 days |
| 17 | **FPSR IEEE flags from FPSCR on `MRS FPSR`** (§3.15) | flags never set | `mffs` on the rare read | 2.06 | fidelity, not speed | low | hours |

**Not levers** (§4): `lxvl`/`stxvl`, `lxvwsx`, `cmpb`, `addex`, `bpermd`, `darn`, `vcmpnezb`
beyond N1, `xxperm` vs `vperm`, `lq`/`stq` for LDP, `addpcis`, and update-form loads beyond
§3.13. The reasons matter because each was on the brief's list.

**Sums, guessed.** On the compiled-C/C++ workloads items 1, 2, 4 and 5 together are worth on
the order of 0.15-0.30 host instructions per guest instruction against the ~2.5
host-per-guest of `cc1` today, i.e. 6-12% of executed host instructions, plus one
data-dependent host branch removed per CCMP. Item 1 alone should recover a good part of the
6-10% G2 was estimated at and the 3% it delivered (its commit message says why: most compares
could not be dropped). Item 3 is a single-workload item: if Firefox's atomic refcounts are as
hot at run time as they are in its text, it is worth 2-5% of Firefox's host instructions plus
two heavyweight syncs per refcount, and nothing anywhere else. Items 6 and 12 are the FP/SIMD
side: invisible on the compiler, large where they fire. Sections in §3 are numbered by the
audit's order, not by rank; the table is the rank.

## 1. Method

- **Instruction vocabulary.** `Emitter.h` defines about 560 mnemonics. Absent, checked by
  grep: `setb`, `cmpb`, `modsd`/`modud`/`modsw`/`moduw`, `maddld`/`maddhd`, `cnttzd`/`cnttzw`,
  `addex`, `extswsli`, `mcrf`, `lxvl`/`stxvl`/`lxvll`/`stxvll`, `lxvwsx`, `lxvw4x`/`lxvb16x`,
  `xxextractuw`/`xxinsertw`, `vextract*`, `vgbbd`, `vcmpne*`/`vcmpnez*`, `vslv`/`vsrv`,
  `vrl*nm`/`vrl*mi`, `vnegw`, `vexts*2*`, `xvcvuxddp`, `xvcvspuxws`, `xvcvdpuxds`, `lwat`/`ldat`/
  `stwat`/`stdat`, `stxsibx`/`stxsihx`, `divwe`, `cmprb`/`cmpeqb`. Some of those answer a long
  sequence below; most are correctly absent.
- **Sequences** were read off the `DEF_OP` bodies for the common operand shape (register
  operands, immediates that fit). They are executed-path counts, not worst cases.
- **Frequencies.** (a) `WARM-CODEGEN-RESEARCH.md` §5 is the only executed data, for `cc1`
  only, and has no rows for TST, ASR, register-offset loads or the multiplies. (b) The static
  census in §6. Static counts over-weight cold code and under-weight loops; `cc1`'s CCMP is
  0.30% static and 0.7% executed, its TBZ 2.3% static and 1.3% executed, which gives the
  scale of the error either way.
- **What I did not do:** build, run or profile. Cycle numbers are quoted from the pipeline
  research where it measured the instruction (`mfxer` +11, `mtocrf` +7, store-forward 15-18,
  a mispredict ~25) and are otherwise absent.

## 2. The constraint that shapes the integer items: CR0/XER are the guest NZCV, and NZCV is live at every exit

`IRBuilder.h:17-20` states the contract: guest N and Z live in CR0.LT/CR0.EQ and C and V in
XER.CA/XER.OV between flag-producing and flag-consuming ops. That is what makes the flag
producers cheap (`subfco.`, `andi.`, `add.` set the packed state in the same instruction), and
it is also why every *non-flag* op is forbidden the record forms and the XER-writing forms:

- `DEF_OP(And)` (`ALUOps.cpp:944`) must not use `andi.`; `DEF_OP(Ashr)` (`:1462`) and
  `DEF_OP(Sbfe)` (`:1923`) must not use `sradi`/`srad`/`srawi`/`sraw`, which write XER.CA;
  `DEF_OP(Sub)` (`:360`) must not use `subfic`; `DEF_OP(CondJump)` (`BranchOps.cpp:1200`) and
  `DEF_OP(Select)` (`ALUOps.cpp:2839`) compare into cr7, and for TBZ extract the bit with a
  non-record `rldicl` and then `cmpldi cr7` rather than one `rldicl.`.
- The comments say "x86 CF", but the constraint is real for A64: `asr` does not write C, and
  a later `b.cs`/`adc`/`ccmp` may read the C a `sradi` would have destroyed.

The second half of the constraint is *liveness*. `RedundantFlagCalculationElimination.cpp`
(DFCE) computes per-op NZCV liveness for A64 and drives `CompareFusion::Run`
(`CompareBranchFusion.cpp:345`) with a block-level `NZCVLiveOut`. It seeds `FlagsRead = FLAG_ALL`
for any block that ends in something other than a `CondJump`/`Jump` to another block of the
unit (`:635`, `:890`): every unit exit is treated as reading all four flags. The G2 commit
(5558f8ae8) counted the consequence on `cc1`: of 112k `cmp`+`B.cond` pairs, **54% have a leg
that leaves the unit, so the compare is kept**, 37% have an in-unit leg that reaches an exit
without a full writer, and **only 9% could be dropped**. That is why G2 landed 3% against a
6-10% estimate, and it is the ceiling on every flag item below (§3.1, §3.2, §3.5, and the
CR0 save in §3.16) until §3.0 changes the policy.

## 3. The items

### 3.0 NZCV dead at constant unit exits, with flag-reading successors caught at link time

**Today.** A compare feeding a branch whose target is another compile unit is always kept,
because the exit block's live-in is `FLAG_ALL`. So the common shape

    cmp  w0, w1        ->  sldi TMP1,w0,32 ; sldi TMP2,w1,32 ; subfco. TMP3,TMP2,TMP1   (SubNZCV, ALUOps.cpp:2348)
    b.lt L_other_unit  ->  [cmpw cr7,w0,w1 when the condition reads XER]  bc ...          (fused consumer, CR0/XER producer kept)

still costs the three-instruction W producer at 54% of sites, and every flag-shaped item in
this document (§3.1, §3.2, §3.5) inherits the same ceiling: a fused CCMP or TST at a unit end can only
be fused *in addition to* the packed-NZCV producer, never instead of it. G2 tried the
alternative of recomputing the compare on the exit leg and measured -1.4% instructions but
+1.2-2.6% cycles (5558f8ae8), so the exit leg is the wrong place for it.

**What exists already.** `EntryNZCVLiveIn` (2f8013325): DFCE records in the IR header and the
`JITCodeTail` whether a unit reads any NZCV bit before writing it
(`RedundantFlagCalculationElimination.cpp:856-871`), and the linker refuses a *direct* link to
such a target (`JIT.cpp:1905-1909`), falling back to the thunk link. Today that gating has no
teeth because exits are conservative anyway; the thunk link does not recompute flags either.

**Proposed policy.**
1. DFCE seeds NZCV *dead* (`FlagsRead = 0` for the NZCV bits) at `ExitFunction`s whose target
   is a constant (B, B.cond, CBZ/CBNZ, TBZ/TBNZ, BL), and keeps `FLAG_ALL` at indirect exits
   (BR, BLR, RET), `Syscall` and `Break`. The unit's header records `ExitsAssumeNZCVDead`
   (only set when DFCE actually dropped a producer on the strength of it).
2. At link time, when `TargetReadsFlags` and the source has `ExitsAssumeNZCVDead`, do not
   link; instead invalidate the source unit through the SMC path (`LookupCache::Erase` and
   the delinkers that already exist) and recompile it with the conservative seed (a per-compile
   flag in the header, hashed into the code-cache config like `POWERARM_MAXLEADERS` is), then
   link. P12's link-first exits guarantee the linker runs before a constant exit is ever taken,
   so no stale flags reach a live-in target.
3. Signal frames taken at a unit entry may carry the flags of the last kept producer, not the
   architectural ones. G2 already accepts the within-block version of this ("NZCV in a signal
   frame taken after the last reader is not exact"); this extends it to the entry drain point.
   No guest code in the reference set reads PSTATE.NZCV out of a signal frame for control flow.

**Delta.** Every `cmp`/`cmn`/`subs`/`adds`/`tst`/`ccmp` whose flags are consumed in the unit and
whose readers are all fusable becomes droppable, i.e. most of them. G2's own arithmetic: the
research estimated compare+branch fusion at 0.25-0.3 host instructions per guest instruction
(~10% of executed host instructions) if compares could be dropped; 9% of pairs were. If the
policy makes 80-90% droppable, the remaining ~3-6% is on the table (guess). It also removes
the `mcrxrx`/CR composites that today's kept producers still force onto CSEL and CCMP
consumers (`MapNZCVCC`, `JIT.cpp:1043`), and it is what lets §3.1, §3.2 and §3.5 reach the
"after" numbers in the table instead of a fraction of them.

**Correctness.** Medium-high, and this is where the argument must be written down rather
than assumed: (a) the recompile path is a new invalidation/recompile cycle at link time, in
the code-cache and multi-threaded world (`check-code-cache.sh`, the `threadexit`/`sigpreempt`
stress loops); (b) indirect exits stay conservative, so a unit reached by `br` never depends
on the check; (c) `cmpbranch` already has "flags read across an exit that leaves the unit"
cases (1954 cases) and `sigpreempt` covers the drain point; add a generated test whose
successor units *start* with `b.cond`/`cset`/`csel`/`ccmp`/`adc` at a branch target so the
recompile path is exercised, in the three modes and with the cache on; (d) Firefox and VS Code
must run, since G2's earlier miscompile was found by Firefox and not by the sweep.

**Size.** 3-5 days: DFCE seeding, the header bit and its cache hashing, the link-time
recompile, tests. It is ranked first because it multiplies three other items and finishes G2.

### 3.1 TST/ANDS fused through record-form `andi.`/`and.`; TST as `TestNZ`

**Today.** `tst w0, #0x1 ; b.ne L` reaches the backend as `AndWithFlags` + `CondJump{FromNZCV}`.
`DEF_OP(AndWithFlags)` (`ALUOps.cpp:1280-1366`) emits, for a W-size immediate:

    andi.  rD, rS, 0x1          ; CR0 from the 64-bit result                    (:1301)
    addco  TMP1, r0, r0         ; clear XER.CA/OV: TST sets C=V=0               (:1345)
    extsw. TMP1, rD             ; refine CR0.LT/EQ to the low 32 bits           (EmitTestNZSetCR, :2395)
    rldicl rD, rD, 0, 32        ; zero-extend the result, dead for TST (Rd=XZR) (:1352)
    bc     ...                  ; MapNZCVCC(NEQ) = CR0.EQ, JIT.cpp:1043

Five (three at X: `andi.; addco; bc`); the register form uses `and.`, same count; `tst; cset`
is the same producer plus `isel`. The fusion pass does not accept `AndWithFlags`/`TestNZ`
producers (`CompareBranchFusion.cpp`: `SubNZCV`, `AddNZCV`, `SubWithFlags`, `AddWithFlags`), and the
frontend emits `_AndWithFlags` even when Rd is XZR (`TranslateDataProcessing.cpp:414`), so the
zero-extension of a value nobody reads is always paid.

**What the ISA offers.** Nothing new: `andi.`, `andis.`, `rlwinm.`/`rldicl.` and `and.` set
CR0 from the AND in one instruction, and CR0 is where the consumer reads EQ/NE. Three steps,
in order of independence from §3.0:

1. *Frontend:* TST (Rd = 31) emits `_TestNZ` (`ALUOps.cpp:2420`, no destination), dropping the
   `rldicl`: 5 -> 4 at W. Independent of liveness.
2. *Backend:* skip the `extsw.` refinement when the mask is an immediate that excludes bit 31
   (`andi.`, `andis.` below bit 31, single-run masks): the 64-bit result's sign is then
   provably 0 and its EQ is the low-32 EQ. 4 -> 3 at W. Independent of liveness.
3. *Fusion:* with the producer's NZCV otherwise dead (G2's precondition, which §3.0 makes
   common), lower the pair as `andi. TMP, rS, imm ; bc 4,2,L` = 2, dropping the `addco` too.
   Register W form: `and TMP,a,b ; rldicl. TMP,TMP,0,32 ; bc` = 3 (high garbage must not leak
   into Z); MI/PL at W: `and ; extsw. ; bc` = 3. CSEL consumer: `isel` on CR0.EQ.

**Delta.** -2 at W without §3.0 (steps 1-2), -3 with it. Frequency, static: `tst`+`ands` are
0.65% of cc1, 0.52% of the Claude CLI, 0.41% of VS Code's Electron binary, 0.25% of libxul
and libc, 0.14% of Factorio (§6). V8's `IsSmi` in C++ and GCC's flag tests are this shape;
JIT-generated V8 code uses `tbz` for the same test (§3.5).

**Correctness.** Low: only Z (and N at X) is read; the W-with-garbage hazard is handled by the
`rldicl.` form. Gates: `flags.S`, `flagsweep.S`, `cmpbranch`, `cmpchain` in three modes, plus a
generated `tst`/`ands` + `b.cond`/`csel` sweep (W/X, immediate/register, dirty high half).

**Size.** Steps 1-2 hours; step 3 one to two days inside the fusion pass.

### 3.2 CCMP/CCMN fused into CR logic

**Today.** `cmp w0, w1 ; ccmp w2, w3, #K, cond ; b.cond2 L` is three IR ops; the fusion pass
keeps the CMP because a CCMP reads it (`CompareBranchFusion.cpp:50-56`):

    sldi TMP1,w0,32 ; sldi TMP2,w1,32 ; subfco. TMP3,TMP2,TMP1        ; SubNZCV W: 3  (ALUOps.cpp:2348)
    [mcrxrx 1 ; crxor 12,0,4 ; crnor 13,12,2]                          ; MapNZCVCC(cond): 0-3 (JIT.cpp:1043)
    bc   !cond, False                                                  ; CondSubNZCV (ALUOps.cpp:3356)
    sldi TMP1,w2,32 ; sldi TMP2,w3,32 ; subfco. TMP3,TMP2,TMP1        ; 3
    b    Done
  False:
    mfocrf ; LoadConstant ; andc ; [LoadConstant ; or] ; mtocrf ; CA/OV writes   ; SetNZCVConstant (:3294), 7-9
  Done:
    [mcrxrx 1 ; crxor ; crnor]                                         ; MapNZCVCC(cond2): 0-3
    bc   cond2, L

Taken path, W, signed: 13, which is the 13 the executed census recorded (0.7% of `cc1`'s
guest instructions, 53 bytes per site). The false leg is as long. The `bc !cond` is a host
branch on a data-dependent condition, which is exactly what the guest compiler used CCMP to
avoid.

**What the ISA offers.** Eight CR fields and the CR-logic ops (`crand`, `crandc`, `crnand`,
`cror`, `crorc`, `crnor`, `crxor`, `creqv`, all in `Emitter.h`). `cmpw`/`cmplw`/`cmpd`/`cmpld`
into distinct fields give every A64 condition except MI/PL/VS/VC as one CR bit (G2's table,
`CompareBranchFusion.cpp:80-140`). One CCMP is `taken = cond(P) ? cond2(P2) : cond2(K)` with
`cond2(K)` a compile-time boolean, so:

    cmpw   cr6, w0, w1              ; cmplw for an unsigned cond
    cmpw   cr7, w2, w3              ; cmpwi cr7, w2, imm5 for the immediate form
    crand  cr3.lt, cr6.<cond>, cr7.<cond2>   ; cond2(K)=false; crorc when true; the *c/n* variants
                                             ; absorb the two polarities
    bc     cr3.lt, L

Four instructions, no XER traffic, the guest's own branch only. A CSEL consumer takes `isel`
on the composite; a chain `cmp; ccmp; ccmp; b` uses cr5/cr6/cr7 and two CR ops. Base ISA.

**Delta.** 13 -> 4 on the taken path, and one data-dependent host branch removed. Static:
`ccmp`/`ccmn` are 0.30% of cc1, 0.24% of the Claude CLI, 0.37% of libc, 0.10% of libxul,
0.06% of VS Code, 0.07% of Factorio; executed in `cc1` 0.7%, i.e. GCC's own code is where the
idiom is densest. At -9 per site that is about -0.06 host instructions per guest
instruction on `cc1` (~2.5% of executed host instructions), and if a third of those branches
mispredict at ~25 cycles the branch is worth as much again. Both extrapolations. Without
§3.0, the CMP and the CCMP stay as packed-NZCV producers at a unit end and only the
projection and the `bc !cond` go; the full number needs §3.0.

**Correctness.** Medium: 14 conditions x 2 polarities x K, W and X, immediate and register,
CCMN's add-sense exclusions (G2's `c == 0` and `c == 2^(n-1)` rules apply unchanged), chains,
and a CSEL consumer. MI/PL only against zero, VS/VC never, exactly as G2. Live-out keeps the
producers as G2 does; a CCMP cannot fault; `MayRaiseSignal` between producer and consumer
keeps the producer as now. Gates: `cmpbranch`, `cmpchain`, `flags.S`, plus a new generated CCMP
sweep (every cond x cond2 x K x size x form, chains of two and three, CSEL consumers) against
the Pi in three modes; run Firefox and VS Code before merging (HANDOVER item 20).

**Size.** About a week: a chain walker in the fusion pass, an IR op carrying two compares and
two conditions (or a `CondJump` variant), its lowering, the tests. Per site the biggest scalar
win left; ranked below §3.0 because it depends on it for most of its value.

### 3.3 Vector int<->float conversions with the VSX instructions

**Today.** The A64 frontend converts vector floats one lane at a time through the scalar path.
`FPVectorIntToFloat` (`TranslateFP.cpp:646-662`) is `VectorImm 0` then, per lane,
`_VExtractToGPR -> _A64FloatFromGPR -> _VInsElement`; `SIMDFloatToInt`
(`TranslateSIMDFloat.cpp:352-365`) is per lane `_VDupElement -> _A64FloatToGPR -> _VInsGPR`; the
fixed-point forms (`:391-430`) and the FP16 forms (`TranslateSIMDHalf.cpp:325,348`) do the same.
Reading the backend ops:

- `SCVTF v0.4s, v1.4s`: 1 + 4 x (`VExtractToGPR` 2 on ISA 3.0, `A64FloatFromGPR` 4-5
  (`mtvsrwa; xscvsxdsp; xscvdpspn; PlaceSingleFromDoubleword0`, `A64FPOps.cpp:133`), `VInsElement`
  2-3) = about 37, with four GPR<->VSR round trips on the dependent chain.
- `FCVTZS v0.4s, v1.4s`: 1 + 4 x (`VDupElement` 1-3, `A64FloatToGPR` 6-7 (`xxsldwi; xscvspdp;
  xscmpudp cr1; xscvdpsxws; mfvsrwz; bc; li`, `A64FPOps.cpp:80`), `VInsGPR` 2-3) = about 40.

The backend has `Vector_SToF`/`Vector_FToS`/`Vector_FToZS` (`VectorOps.cpp:5018-5132`) but with
x86 semantics (the INT_MIN sentinel select and two pool loads), and the i64 `Vector_SToF`
goes through a 13-instruction stack round trip with `fcfid` (`:5029-5045`) although
`xvcvsxddp` is in the emitter. The frontend cannot use them as they stand.

**What the ISA offers.** All ISA 2.06 VSX, ungated: `xvcvsxwsp`/`xvcvuxwsp` (i32 -> f32),
`xvcvsxddp`/`xvcvuxddp` (i64 -> f64), `xvcvspsxws`/`xvcvspuxws` (f32 -> i32, truncating,
saturating), `xvcvdpsxds`/`xvcvdpuxds`, and `xvrspi{c,z,m,p}`/`xvrdpi{c,z,m,p}` for the
FCVTN*/FCVTA*/FCVTM*/FCVTP* rounding. FPSCR.RN equals FPCR.RMode since F7, so SCVTF's
"round with FPCR.RMode" is free. `xvcvuxddp`, `xvcvspuxws`, `xvcvdpuxds` need adding.

    SCVTF.4S / UCVTF.4S      xvcvsxwsp / xvcvuxwsp                     1
    SCVTF.2D / UCVTF.2D      xvcvsxddp / xvcvuxddp                     1
    FCVTZS.4S                xvcvspsxws Dst,Src ; xvcmpeqsp. M,Src,Src ; xxland Dst,Dst,M   3
                             (NaN lanes must be 0; POWER's signed convert gives INT_MIN)
    FCVTZU.4S                xvcvspuxws + the same mask if hardware gives non-zero for NaN   1-3
    FCVTNS/AS/MS/PS.4S       xvrspi{c,-,m,p} then as FCVTZS                                4 (ties-even: the F6
                                                                                            mffscrni bracket, +2)
    2S / 2D / scalar forms:  the same on the low half, then the frontend's VMov(64)

**Delta.** ~37 -> 1 and ~40 -> 3 per 4-lane instruction; latency from four serialised
GPR<->VSR trips to one VSX convert. Static (§6): the vector converts are 0.08% of libxul
(about 23,000 sites: media, graphics, WebAudio), 0.026% of Factorio (2,700), 0.016% of VS
Code (6,000), 0.002% of the Claude CLI, zero in cc1. This is the largest static ratio in the
audit; its rank reflects that none of the integer workloads has it in a hot loop and
Factorio's simulation is integer, while Firefox's media and canvas paths do run it.

**Correctness.** NaN -> 0 (the mask), saturation (POWER matches), rounding under FPCR.RMode
(synced), no FPSR.QC (VSX converts do not touch VSCR; the VMX `vctsxs` must not be used since it
sets VSCR.SAT and N11 reads SAT as QC). FPSR.IOC is not tracked on either path (§3.15).
Gates: `gen_simd.py` already generates vector FCVTZS/FCVTZU/SCVTF/UCVTF and the rounding
forms (`unittests/A64Frontend/gen_simd.py:538-576, 614-655`) against Pi goldens; add explicit
NaN, +/-Inf, +/-0, 2^31, -2^31-1 and 2^32 lanes if the corpus lacks them.

**Size.** One to two days: A64-specific IR ops (mirroring `A64FArith`) or flags on the
existing `Vector_*` ops, three emitter lines, the frontend loops replaced.

### 3.4 RBIT+CLZ is CTZ: `cnttzd`/`cnttzw`; standalone RBIT through the vector unit

**Today.** `DEF_OP(Rbit)` (`ALUOps.cpp:1807-1892`) is a three-stage SWAR swap with three
`LoadConstant`s (2-5 instructions each) and a byte reverse through the stack:

    mr TMP1,Src ; LoadConstant 0x5555... ; srdi ; and ; and ; sldi ; or
                  LoadConstant 0x3333... ; srdi ; and ; and ; sldi ; or
                  LoadConstant 0x0F0F... ; srdi ; and ; and ; sldi ; or
    addi r1,r1,-16 ; stdbrx TMP1,r0,r1 ; ld Dst,0(r1) ; addi r1,r1,16

About 26 instructions, one store-to-load forward (15-18 cycles), and it moves r1. The A64
frontend has no peephole for the pair: `RBIT_int` (`TranslateDataProcessing.cpp:327`) and
`CLZ_int` (`:365`) are independent, so `rbit w0, w0 ; clz w0, w0`, which GCC and clang emit
for `__builtin_ctz` on ARMv8.0-8.8, costs ~26 + 3.

**Where it runs.** glibc's AArch64 `strlen`, `strnlen`, `memchr`, `strchr`, `strchrnul`,
`strcpy`, `strrchr` compute the match index on exit with `rbit ; clz` on the syndrome; N1
fused their loops and left the exit path, so every call pays this once. `ctz` is the
bitmap-iteration primitive in cc1 (`sbitmap`, `bitmap_first_set_bit`), V8 (`CountTrailingZeros`
in the GC mark bits and the register allocator), JSC/WTF, jemalloc/PartitionAlloc. Static:
`rbit` is 0.052% of the Claude CLI (JSC's bitmaps), 0.018% of libc (53 sites, the string
functions), 0.014% of cc1, 0.010% of Factorio.

**What the ISA offers.** ISA 3.0 `cnttzd`/`cnttzw` (one instruction; 64/32 on zero, which is
`rbit;clz` of zero). The IR already has `FindTrailingZeroes` with that contract
(`ALUOps.cpp:1691`: `addi; not; and; [rldicl]; popcntd`, 4-5, XER-safe: the 2.07 fallback).
For a standalone RBIT on ISA 3.0: `vgbbd` (2.07, transposes each doubleword's 8x8 bit matrix)
composed with `xxbrd` twice is a full bit reverse, `mtvsrd ; xxbrd ; vgbbd ; xxbrd ; vgbbd ;
mfvsrd` = 6, no memory (W form: `srdi Dst,Dst,32` after, 7). `vgbbd` is not in the emitter.

**Delta.** RBIT+CLZ: ~29 -> 1 (3.0) / -> 4-5 (2.07). RBIT alone: ~26 + stall -> 6 (3.0).

**Correctness.** None to speak of; add `rbit`/`clz` pairs with zero, one-bit and random inputs
at W and X to the `gen.py` sweep. The fold fires only when the RBIT result has no other reader.

**Size.** Half a day.

### 3.5 Flag-liveness gate: `sradi`/`srad`/`srawi`/`sraw` for ASR and SBFX, record forms for TBZ/CBZ

**Today.** Because `sra*` write XER.CA (§2), `DEF_OP(Ashr)` (`ALUOps.cpp:1462-1566`) emits:

    ASR imm, X:   rldicl TMP3,S,1,63 ; neg TMP3 ; rldicl TMP4,S,64-sh,sh ; sldi TMP3,64-sh ; or     5
    ASR imm, W:   extsw TMP4,S ; rldicl Dst,TMP4,64-sh,sh ; rldicl Dst,Dst,0,32                    3
    ASR reg, X:   rldicl ; rldicl ; neg ; srd ; li 64 ; subf ; sld ; or                            8
    ASR reg, W:   rldicl ; extsw ; srd ; rldicl                                                    4

`DEF_OP(Sbfe)` (`:1923`) for SBFX with lsb != 0 or a width other than 8/16/32:
`rlwinm/rldicl ; rldicl ; neg ; sldi ; or [; rldicl]`, 5-6; `CLS` inherits the 5-instruction
ASR. `TBZ` is `rldicl ; cmpldi cr7 ; bc` (`BranchOps.cpp:1252`) and `CBZ` on a value produced
by an ALU op in the same block is `op ; cmpdi cr7 ; bc`, because a record form would write CR0.

**What the ISA offers.** `sradi`/`srad` (1), `srawi`/`sraw` + `rldicl` (2), `sldi ; sradi` for
SBFX (2), all 2.06, *whenever the guest C flag is dead at that point*; `rldicl.`/`andi.` for
TBZ (2 instead of 3) and `add.`/`and.`/`or.` feeding CBZ (2 instead of 3) whenever all of NZCV
is dead. The pipeline research measured `add.` with an unread CR0 at zero cost (§4.2).

**How.** DFCE knows per op which flags are read after it (that is how it drops producers).
Export it as a per-op bit "no NZCV bit is read after this op before the next full writer, and
none is live out" in a side vector the backend indexes by node ID like `Elide32MaskSet`
(`JITClass.h:387-457`). `Ashr`/`Sbfe` pick `sra*` when set; `CondJump`'s TSTZ/TSTNZ and
compare-with-zero arms pick the record form. Use DFCE's notion of live-out, whatever §3.0
makes it; do not invent a second one. Under today's seed the bit is set only when a full
writer follows in the block; under §3.0 it is set at most sites. The first thing the
implementation should print is the gated fraction on `cc1` and `node`.

**Delta.** ASR imm X 5 -> 1, W 3 -> 2, reg X 8 -> 1, W 4 -> 2; SBFX 5-6 -> 2; TBZ 3 -> 2;
CBZ-after-ALU 3 -> 2. Static (§6): `tbz`/`tbnz` are **2.3% of cc1**, 1.6% of libxul, 1.55%
of VS Code, 1.15% of the Claude CLI, 0.77% of Factorio (executed in `cc1`: 1.3%), so the TBZ
half alone is worth ~0.5% of executed host instructions on `cc1`; `asr #imm` is **0.22% of VS
Code's Electron binary (84k sites, V8's Smi untag `asr x, x, #32`)** and 0.23% of Factorio,
against 0.04% of cc1 and Claude; V8's JIT output, which untags the same way, is in no census.

**Correctness.** Medium: a wrong bit corrupts guest C silently, and C is consumed rarely (ADC,
B.CS, CCMP, `cset cs`), so it would surface only in bignum, checked-arithmetic and range-check
code. Gate: `flags.S`/`flagsweep.S` plus a generated test putting `asr`/`sbfx`/`tbz` between a
flag producer and every C/V consumer, within and across blocks and units, in three modes.

**Size.** One to two days, most of it the DFCE export and the test.

### 3.6 Register-offset addressing in X-form; `extswsli` for `sxtw #n`; `rldic` for `uxtw #n`

**Today.** `LoadStoreRegOffset` (`TranslateLoadStore.cpp:166-186`) computes
`Address = _Add(Base, ExtendReg(Rm, Option, Scale))` and passes it to `LoadMem` without an
offset. `ExtendReg` (`IRBuilder.cpp:671-689`) is `_Sbfe(64,32,0)`/`_Bfe(64,32,0)` then `_Lshl`:

    ldr w0, [x1, w2, sxtw #2]:  extsw TMP,w2 ; sldi TMP,TMP,2 ; add TMP,x1,TMP ; lwz w0,0(TMP)   4
    ldr x0, [x1, x2, lsl #3]:   sldi ; add ; ld                                                  3
    ldr x0, [x1, x2]:           add ; ld                                                         2
    ldr w0, [x1, w2, uxtw #2]:  rldicl ; sldi ; add ; lwz                                        4
    ldr d0, [x1, x2, lsl #3]:   sldi ; add ; lxsdx ; xxpermdi                                    4

`MakeAddrForm` (`MemoryOps.cpp:614-650`) already takes a register index with
`MemOffsetType::UXTW`/`SXTW` and a scale and emits the X-form; the A64 frontend never hands it one.

**What the ISA offers.** X-form loads and stores for every size and class (base ISA); ISA 3.0
`extswsli rD, rS, n` (= `sxtw` and `lsl #n`); `rldic rD, rS, n, 32-n` (= `(uint32)x << n`, base).

    ldr w0, [x1, w2, sxtw #2]:  extswsli TMP,w2,2 ; lwzx w0,x1,TMP     2   (extsw ; sldi ; lwzx on 2.07)
    ldr x0, [x1, x2, lsl #3]:   sldi ; ldx                              2
    ldr x0, [x1, x2]:           ldx                                     1
    ldr w0, [x1, w2, uxtw #2]:  rldic ; lwzx                            2
    ldr d0, [x1, x2, lsl #3]:   sldi ; lxsdx X-form ; xxpermdi          3

With the same plumbing, the FPR immediate forms (`ldr q0, [x0, #32]`, today `addi ; lxvx`) can
take ISA 3.0's DQ-form `lxv`/`stxv` (1); that is two emitter lines once the displacement reaches
the backend.

**Delta.** -1 to -2 per register-offset load or store. Static (§6): the register-offset forms
are 1.18% of cc1 (0.34% of them `sxtw`-scaled: `int` array indices), 1.04% of the Claude CLI,
0.71% of Factorio, 0.68% of VS Code, 0.51% of libxul, 1.91% of libc.

**Correctness.** Low: address arithmetic only, and the backend path is the one x86 exercises.
Gates: `strmem.c`, `loadstore_rcpc2.S`, a64diff 64k/4k-kvm.

**Size.** Half a day.

### 3.7 `VMov(i64)` against the pinned zero; FMOV D<-X through `mtvsrdd`

**Today.** Every D-register result goes through `StoreVSized -> _VMov(i64)`
(`IRBuilder.h:369-375`), and `DEF_OP(VMov)` (`VectorOps.cpp:62-104`) emits `vspltisw VTMP1,0`
before its switch and then `xxpermdi Dst,VTMP1,Src,1`: two instructions where the N3 checklist
row says one. F4 introduced `VZERO_VSX` (vs14) for `PlaceElement0`; `VMov` did not pick it up.
That is one instruction after every scalar FP arithmetic op (`xv*; xvcmpeq*.; bc` + this pair
= 5) and every 64-bit NEON op. `FMOV d0, x0` and `FMOV d0, #imm` go through `_VCastFromGPR`
(`TranslateFP.cpp:192,217`), lowered (`VectorOps.cpp:4783-4820`) as `vspltisw VTMP2,0 ; mtvsrd
VTMP1,Src ; vsldoi Dst,VTMP2,VTMP1,8` (3; 4 with the `rldicl` for W).

**What the ISA offers.** `xxpermdi Dst, VZERO_VSX, Src, 1` alone (2.06). `mtvsrdd Dst, r0, Src`
(3.0; RA=0 supplies a literal zero; already in `Emitter.h`): dw0 = 0, dw1 = Src, which is FEX's
element 0 with the upper half clear, 1 instruction (`clrldi ; mtvsrdd` for S).

**Delta.** -1 per D-register result, -2 per FMOV D<-X. Static: `fmov d, x` is 0.08% of
Factorio, 0.06% of the Claude CLI; scalar FP arithmetic 0.44% of Factorio.

**Correctness.** None; the image is byte-identical. Gates: `fpmath.c`, `fpbugs.S`, `simd_*`.
**Size.** Hours.

### 3.8 SMULL -> `mullw`; MADD -> `maddld`; SMADDL -> `mullw; add`

**Today.** `DEF_OP(SMull)` (`ALUOps.cpp:543`) is `extsw ; extsw ; mulld` (3). `MADD`
(`TranslateDataProcessing.cpp:569`) is `_Mul + _Add` -> `mulld ; add` (2, `clrldi` at W).
`SMADDL` (`:585`) is `extsw ; extsw ; mulld ; add` (4).

**What the ISA offers.** `mullw` places the *full 64-bit signed product of the low 32 bits*
in RT (ISA 3.0B 3.3.9.1), which is SMULL exactly, regardless of high garbage: 1, base ISA, a
pure backend change that also serves x86's `imul r32`. ISA 3.0 `maddld RT,RA,RB,RC` =
`(RA*RB + RC) mod 2^64`: MADD in 1. SMADDL becomes `mullw ; add` (2). UMULL has no unsigned
32x32->64 instruction; `mulhwu ; mullw ; rldimi` equals today's 3 and only shortens the chain.

**Delta.** SMULL 3 -> 1, MADD 2 -> 1, SMADDL 4 -> 2. Static: `smull`/`umull` 0.09% of cc1,
`smaddl`/`umaddl` 0.16% of the Claude CLI, `madd` 0.09% of Claude and 0.08% of Factorio
(division by a constant is `umull`/`smull` + shift on AArch64).

**Correctness.** None (modular arithmetic); `maddld` behind `SupportsISA30`. **Size.** Hours.

### 3.9 SDIV/UDIV without the double-select scaffold; `modsw`/`modsd` for the MSUB idiom

**Today.** `UDIV` (`TranslateDataProcessing.cpp:251`) emits `Select(EQ div,0 -> 1 : div) ;
UDiv ; Select(EQ div,0 -> 0 : q)`, lowering to `cmpdi cr7 ; li 1 ; isel ; divdu ; mulld ; subf ;
cmpdi cr7 ; isel` (8 at X; the `mulld ; subf` compute a remainder nobody reads) and about 13
at W through `DEF_OP(UDiv)` (`ALUOps.cpp:781`: two masks, `divwu`, `mullw`, `subf`, two masks).
`SDIV` (`:268`) adds the INT_MIN/-1 guard and a negate: about 12 at X, 18 at W. The comment
says the host divide must never see a zero divisor or INT_MIN/-1, "both undefined on POWER":
undefined *result*, not a trap (ISA 3.0B 3.3.9.2; OV only with OE=1), so only the post-select
is load-bearing.

**What the ISA offers.** Nothing new for the divide; the win is dropping the scaffold:

    UDIV X:  divdu q,a,b ; cmpdi cr7,b,0 ; isel q,0,q                                              3
    UDIV W:  divwu q,a,b ; cmpwi cr7,b,0 ; isel q,0,q ; clrldi q,q,32                              4
    SDIV X:  divd q,a,b ; cmpdi cr7,b,-1 ; neg t,a ; isel q,t,q ; cmpdi cr7,b,0 ; isel q,0,q      6

and for `sdiv w2,w0,w1 ; msub w0,w2,w1,w0` with w2 otherwise dead, ISA 3.0
`modsw`/`modsd`/`moduw`/`modud` with the same two `isel`s (4-6 instead of the divide, `mullw ;
subf` and the scaffold). Skip the remainder in `Div`/`UDiv` when `OutRemainder` has no reader.

**Delta.** UDIV 8-13 -> 3-4, SDIV 12-18 -> 6-7, `sdiv+msub` ~15 -> 4-6. Static: `sdiv`+`udiv`
0.03% of cc1, 0.065% of libc; compilers strength-reduce constant divisors, and the divide's own
12-40 cycles dwarf the saved instructions unless the code is divide-bound.

**Correctness.** Low; the special cases keep their `isel`s. Gate: the `gen.py` SDIV/UDIV sweep.
**Size.** One day.

### 3.10 Scalar-FP chain hygiene: a dirty upper lane between scalar ops, `lxvdsx` for single-use loads

**Today.** `fadd d0, d1, d2` is `xvadddp ; xvcmpeqdp. ; bc ; vspltisw ; xxpermdi` (5, 4 after
§3.7); `ldr d1, [x]` is `lxsdx ; xxpermdi` (2: the value lands in dw0 and FEX's element 0 is
dw1, `ArchHelpers/PPC64Emitter.cpp:LoadFPRSized`). In a chain `fmul d0 ; fadd d0 ; fsub d0` the
upper-lane zeroing runs three times and is observable only at the end.

**What the ISA offers.** Nothing new; two elisions the machinery almost supports:

- **Dirty upper lane.** `SCALAR-FP-LOWERING.md` §3.3 measured 13.0 -> 10.0 -> 6.4 cycles per op
  for "zero with xxpermdi" -> "leave the lane dirty where a scalar consumer follows". F4 did
  the first; the second needs a block-local pass dropping the `VMov(64)` when every reader in
  the block is a scalar op and the guest register is rewritten (with zeroing) before any exit,
  drain point or vector reader: the `Elide32MaskSet` shape on the FPR side.
- **Single-use scalar loads.** `SplatCandidateLoads` (`MemoryOps.cpp:967-977`) already turns a
  single-use f64 load feeding an FMA insert into one `lxvdsx`. Widening the consumer set to
  `A64FArith`, `A64FMinMax`, `A64FMulAdd`, `FCmp`, `A64FToF` makes every single-use `ldr d` one
  instruction; F1's post-check compares the result with itself, so a duplicated lane cannot
  raise a spurious NaN.

**Delta.** -1 per scalar op in a chain, -1 per single-use scalar load. Static: scalar FP
arithmetic is 0.44% of Factorio and 0.07% of the Claude CLI, FPR loads/stores 1.6% and 2.5%;
zero on cc1.

**Correctness.** Medium for the dirty-lane elision (a stale upper half must never be
observable: exits, signal frames, `fmov x, v0.d[1]`, vector readers); low for the load fusion.
Gates: `fpmath.c`, `fpbugs.S`, `simd_*`, `sigedit`/`sigpreempt`. **Size.** Two to three days.

### 3.11 ADDP and UZP2 without pool-constant `vperm`

**Today.** `DEF_OP(VAddP)` (`VectorOps.cpp:1831-1890`) loads two permute controls from the pool
(`EmitLoadPPC64VConst`: `ld ; li ; lvx`, 3 each, `JITClass.h:1227`) and does `vperm ; vperm ;
vaddu*m`: 9, with two dependent loads on the path. `DEF_OP(VUnZip2)` (`:2097`) is `vspltisw 0 ;
vsldoi ; vsldoi ; vpk*` (4).

**What the ISA offers.** Even elements are one pack (`vpkuhum`/`vpkuwum`/`vpkudum`), odd ones
the same pack after a lane shift: `ADDP.4S = vpkudum e,VU,VL ; xxspltib c,32 ; vsrd t1,VL,c ;
vsrd t2,VU,c ; vpkudum o,t2,t1 ; vadduwm` (6 on 3.0; `vspltisw -16` for the 16-bit form on
2.07). No loads.

**Delta.** 9 -> 6, 4 -> 3. Static: `addp` is 0.03% of the Claude CLI (JSC's SIMD paths), less
elsewhere. **Size.** Hours.

### 3.12 The three-way compare idiom -> `setb`

`cmp w0, w1 ; cset w2, gt ; csinv w2, w2, wzr, ge` is two fused selects, ~8-9 host
instructions. ISA 3.0 `setb rD, crN` returns -1/0/+1 from LT/EQ/GT: `cmpw cr7 ; setb` (2), via a
frontend peephole after a fusable compare; the 2.07 fallback is today's code. Comparators only;
**low**, half a day.

### 3.13 Pre-index STP -> `stdu`

`stp x29, x30, [sp, #-32]!` (`STP_LDP_gen`, `TranslateLoadStore.cpp:205-262`) is `addi ; std ;
std` (3); `stdu x29,-32(sp) ; std x30,8(sp)` is 2 and keeps the store-before-writeback fault
order. Static: pre-index pairs are 0.42% of cc1, 0.35% of Factorio. POWER9 cracks update
forms into two internal ops, so the saving is fetch and I-cache only; a backend peephole across
two IR ops with the base's writeback. **Low**; not before the items ranked above it.

### 3.14 LSE atomics: save CR0 with `mcrf`; AMOs for LSE RMWs

Every `AtomicFetch*`/`AtomicSwap` (`AtomicOps.cpp:545-925`) brackets its LL/SC loop with
`mfocrf TMP4,0x80 ; std TMP4,-8(r1)` ... `ld ; mtocrf` to preserve CR0 across the `stdcx.`:
two SPR moves (+7 cycles each) and a store-forward. `mcrf cr5, cr0` / `mcrf cr0, cr5` (base
ISA, cr5 unused by the backend) does it in two ~1-cycle instructions; with §3.5's liveness bit
it can go entirely when NZCV is dead. ISA 3.0 `lwat`/`ldat`/`stwat`/`stdat` would replace the
loop for LDADD/LDCLR/LDSET/LDEOR/SWP/LDSMAX-family with no `stcx.` and no CR write, but they
execute at the L2 and are reported slower than an L1-hit `larx`/`stcx.` for uncontended data.
The P7 census puts V8 at ~2.5 M LSE RMWs per 3.9 s run, and the static census (§6) shows LSE
opcodes at 0.000% of every reference binary: they live only inside the outline-atomics
helpers (`__aarch64_ldadd4_acq_rel` and friends), which is how V8, Factorio and Bun reach
them. **Not a lever**; `mcrf` is hours of hygiene. The inline LDXR/STXR loops are the
different, Firefox-sized story in §3.16.

### 3.15 FPSR IEEE flags from FPSCR on `MRS FPSR`

`DEF_OP(LoadFPSR)` (`ALUOps.cpp:4240`) returns the stored word plus VSCR.SAT as QC;
IOC/DZC/OFC/UFC/IXC are never set, so `fetestexcept` and libm's error paths that read them see
zero. (Aside: that lowering's `andi.` appears to clobber CR0, i.e. guest N/Z, on an `MRS FPSR`;
a `rldicl` would not.) FPSCR accumulates VX/ZX/OX/UX/XX for free on every VSX op; `mffs` on
the rare `MRS FPSR` and a five-bit remap gives A64 the cumulative flags, `MSR FPSR` clears
them with `mtfsf`. Zero hot-path cost, fidelity only; the FMA cold stubs that re-run an
operation may raise a second XX, which needs thought.

### 3.16 Native `lwarx`/`stwcx.` for the LDXR..STXR loop

**Today.** LDXR/STXR go through the software exclusive monitor in CPUState
(`TranslateExclusive.cpp:1-13`, DESIGN.md §4.3). `LoadExclusive` (`:22-39`) is the load plus
four context stores (`excl_addr`, `excl_value`, `excl_size`, `excl_valid`): 5 host
instructions, plus `lwsync` for LDAXR. `StoreExclusive` (`:75-119`) is, reading the IR it emits
against the lowerings above:

    lbz ; lbz ; ld                          ; excl_valid, excl_size, excl_addr                 3
    cmpd cr7 ; li ; isel  |  cmpdi cr7 ; isel ; and                                          6   (two Selects, And)
    stb excl_valid,0 ; cmpdi cr7 ; bc       ; monitor cleared, branch on Match                3
  Try:
    mfocrf ; mfspr XER ; rlwinm ; rlwimi x3 ; ld excl_value                                  7   (_LoadNZCV, ALUOps.cpp:4179)
    hwsync ; [andi. ; bc] ; ldarx ; cmpd ; bne ; stdcx. ; bne ; isync                        8-10 (_CAS, AtomicOps.cpp:928, not Relaxed)
    cmpd cr7 ; li ; isel                    ; status                                          3
    rlwinm ; rlwimi ; mtocrf ; rlwinm ; SetCA ; rlwinm ; SetOV                                9   (_StoreNZCV, :4206)
    stw Ws ; b Join

About 43 instructions on the success path, two SPR round trips, the `hwsync`/`isync` bracket
(the CAS is emitted without the Relaxed flag P7 gave the LSE RMWs), and the reservation
state round-tripping through memory. A refcount `ldaxr ; add ; stlxr ; cbnz` is therefore
5 + 1 + 43 + 3 host instructions with three syncs, where the guest wrote four instructions.

**Where it runs.** Firefox only, and there heavily: **0.76% of libxul's instructions are
inline LDXR/STXR family (219k sites)**, plus 0.69% LDAR/STLR and 0.14% DMB. Mozilla's
`Atomic<T>` defaults to sequentially consistent, its C++ refcounts and Rust's `Arc` are
these loops, and the build does not use outline atomics. Every other reference binary is at
0.00% (they reach LSE through the outline helpers, §3.14), and the P7 census counted V8's
LDXR/STXR in the thousands. So this is a single-workload item with, if refcounting is as hot
at run time as it is in the text (it usually is in Gecko), the largest per-workload payoff
in this document. That "if" is the guess to check first (§5).

**What the ISA offers.** `lwarx`/`ldarx` and `stwcx.`/`stdcx.` are the native reservation
pair (2.06; `lbarx`/`lharx` 2.06 for the byte and halfword forms, all in `Emitter.h`). When
the LDXR and its STXR are in one compile unit with only register ops between them (the
canonical loop, which is the only form compilers emit and the only form the architecture
guarantees to make progress), the pair can be lowered directly:

    [lwsync]                 ; STLXR release
    loop:
    lwarx  rT, 0, rAddr      ; LDXR (the reservation replaces the software monitor)
    [lwsync]                 ; LDAXR acquire, as today's trailing fence
    <the guest's ALU ops>
    stwcx. rV, 0, rAddr      ; STXR
    bne-   loop              ; the guest's own CBNZ on Ws, fused: CR0.EQ is the status
    [li Ws,0 / isel]         ; only if Ws is read after the loop

Four to seven instructions. Ordinary stores by the same hart do not clear the reservation
(Book II 1.7.4), so the JIT's context stores and drain-point pokes inside the loop are
harmless; a context switch or a remote store clears it and the loop retries, which is the
guest's own semantics. The reservation granule (128 bytes) is larger than the A76's, so false
sharing retries more often but still makes progress.

**Delta.** ~50 -> 4-7 per pair, and the `hwsync`/`isync` bracket gone (the guest's own
acquire/release map onto the `lwsync`s shown). On libxul that is roughly -0.1 host
instructions per guest instruction if 0.25% of executed instructions are STXRs (guess, from
the static 0.76% for the family), i.e. 2-5% of Firefox's executed host instructions plus the
sync cycles, and 0 elsewhere.

**Correctness.** Medium-high, and it is memory-model work, so the gates are the formal ones:
(a) the fused form must only be used when LDXR and STXR are in the same unit with no memory
op, syscall or exit between them; otherwise the software monitor stays (and the two must
agree: a fused LDXR must still clear `excl_valid` so a later unpaired software STXR fails,
which is architecturally permitted); (b) `stwcx.` writes CR0, the guest N/Z: save with `mcrf`
or, with §3.5's bit, drop the save when NZCV is dead (it is, in a refcount loop); (c) the
`bne-` fusion needs the CBNZ to read only Ws and Ws to be dead after, else materialise it
with `isel`; (d) LDXP/STXP stay on the software path (`lqarx`/`stqcx.` need an even pair and
16-byte alignment); (e) the acquire/release placement must pass `unittests/MemoryModel/check.sh`
(herd7 with the LL/SC translation P7 step (b) already asked for) and `litmus.c` on the Pi
goldens, plus `exclusive_pair`, `lse*`, and the three-mode suite; (f) Firefox must run
(`~/.local/bin/firefox`) since it is the only workload that exercises it.

**Size.** Three to four days: a frontend idiom matcher over the decoded block (the N1
`TryFuseVectorScan` shape, `TranslateSIMD.cpp:1079`), two IR ops (`LoadReserved`,
`StoreConditional`) with the CR discipline, the fallback interplay, and the model check.

## 4. Not levers, and why

- **`lxvl`/`stxvl` (3.0).** The natural use is a tail: 8 bytes into a register, rest zeroed,
  i.e. `ldr d0` in one instruction. The length rides in bits 63:56 of RB, so a per-site `li ;
  sldi 56` costs two, the same as today's `lxsdx ; xxpermdi`, unless a pinned GPR holds it, and
  the dynamic GPR pool is at five registers (warm G3). Guest tails are the guest's own NEON
  code; the JIT's only bulk copies are x86 `MemCpy`/`MemSet`, which the A64 frontend never emits.
- **`lxvwsx` (3.0).** `LD1R.4S` would go from `lwzx ; mtvsrd ; xxpermdi ; vspltw` (4,
  `MemoryOps.cpp:2784`) to 1; the 64-bit form already uses `lxvdsx`. `ld1r` is 0.003% of
  Factorio and is hoisted out of loops; do it if someone is in that file.
- **`cmpb` (2.05).** The GPR SWAR primitive for `strlen`-style code; no A64 instruction maps to
  it and glibc's string functions are NEON, which N1 fused.
- **`addex` (3.0).** AArch64 has one carry; ADCS chains map onto `adde` (1 at X,
  `ALUOps.cpp:2561`). `adc`/`sbc` are 0.00-0.025% static.
- **`bpermd`, `darn`, `prtyd`.** `darn` serves x86 RDRAND (the A76 has no FEAT_RNG); `bpermd`
  permutes eight bits per instruction (a full reverse needs eight; §3.4's `vgbbd` route is
  better); `prtyd` serves x86 PF.
- **`vcmpnezb`/`vcmpneb` (3.0).** `strchr`'s `cmeq ; cmeq #0 ; orr` could be one `vcmpnezb.`
  feeding the N1 branch; `TryFuseVectorScan` (`TranslateSIMD.cpp:1079`) matches only the
  four-instruction shape. A few hours when someone extends N1; `strchr`/`strchrnul` only.
- **`xxperm`/`xxpermr` vs `vperm`.** Same cost (NEON §2(a)); only register-file reach differs,
  which N13's low-bank pinning already exploits.
- **`lq`/`stq` for LDP/STP.** Even register pair, natural alignment, not guaranteed atomic or
  fast outside `lqarx`; two D-form loads are right and pairs are already 2.6 host instructions.
- **`mfvsrld`, `mtvsrdd`, `xxbr*`, `vextu*rx`, `vinsert*`, `vctzlsbb`.** All in the emitter and
  used (N7, N11, F5). `vclzlsbb`/`vctzlsbb` in the N1 exit path would replace the syndrome's
  `fmov ; rbit ; clz` if the fusion reached the exit; §3.4 gets most of that more simply.
- **`addpcis`/`lnia`.** ADRP is folded into the consumer's `EntrypointOffset` delta (0.2 host
  instructions per ADRP in the executed census); nothing left for a PC-relative form.
- **Update-form loads beyond §3.13.** Post-index forms cannot use them (POWER updates before
  the access); the single-register pre-index forms are 0.02-0.2% static.
- **`setb` for CSET/CSETM generally.** Three-valued; a plain CSET is `li ; isel` (2-3) either
  way. Only the three-way idiom (§3.12) fits.
- **`modsd` for standalone SDIV.** The quotient is the result there.
- **`darn`-style hardware for `MRS CNTVCT_EL0`.** Already `mftb`-based (9fa8983a7).

## 5. What an implementation agent should count, and in what order

None of the items needs an instrumented build; each is a source change gated by the existing
suites. These counts turn the guesses above into numbers, in the order that decides
scheduling; each is a one-line print in a throwaway build, not a measurement campaign.

1. **Droppable-producer rate under §3.0.** Re-run G2's own count (112k pairs, 54% / 37% / 9%)
   with the dead-at-exit seed on `cc1 -O2 lvm.c`, and the number of link-time recompiles it
   triggers. If recompiles are more than a few percent of units, the policy needs the
   BL-target exclusion or a per-target cache of the answer.
1b. **Executed LDXR/STXR rate in Firefox** (§3.16): the P7 counting build's method (a
   counter slot per variant in a MAP_SHARED file, fresh cache dir) over a minute of browsing
   the same pages HANDOVER item 21 used. This is the number that decides whether §3.16 is
   worth its three days; the static 0.76% only says the sites exist.
2. **Fusion coverage after items 2 and 4.** Per unit, `AndWithFlags`/`TestNZ` and
   `CondSubNZCV` producers and how many fused, on `cc1` and a `code-server` request; also how
   often `MayRaiseSignal` (a load between `tst` and `b.ne`) blocks fusion.
3. **Gate rate for item 5.** The fraction of `Ashr`/`Sbfe`/TSTZ `CondJump` ops with the
   NZCV-dead bit set, same workloads plus Firefox.
4. **Executed shares.** The `cc1` executed census has no `tst`, `asr`, `ldr (register)` or
   `smull` rows; one `perf record` of `code-server` bucketed by guest mnemonic the way
   `WARM-CODEGEN-RESEARCH.md` §5 was made would rank items 4, 5 and 8 against each other.
5. **`xvcvspuxws` on NaN** on the POWER9 (one line of inline asm) before §3.3 decides whether
   the unsigned form needs the mask.

## 6. Static census of the reference binaries

Host `llvm-objdump -d --no-show-raw-insn --no-leading-addr <binary> | awk -f census.awk`
over each binary's text (the host LLVM 22 objdump at `/mnt/arch/usr/bin/llvm-objdump`, run
natively; the script and the six raw count files are in `census/` next to this document).
Buckets are by mnemonic and operand shape, summarised in the row names: `ls_regoff_*` are
single-register loads/stores by offset shape, `*_vec` have a vector arrangement in the
operands, `fmov_d_from_x` is `fmov d, x`, `cvt_*` are SCVTF/UCVTF/FCVTx*, `exclusives` is the
LDXR/STXR family, `acq_rel_ls` is LDAR/LDAPR/STLR. Percentages are of all decoded
instructions in the binary. Static counts: cold code is over-represented, loop bodies
under-represented (compare `ccmp` 0.30% static vs 0.7% executed and `tbz` 2.3% vs 1.3% on cc1).

| bucket | cc1 (gcc 16.1.1) | libxul (Firefox 156) | code (VS Code 1.138, Electron 42) | factorio 2.1.19 | claude 2.1.280 (Bun/JSC) | libc.so.6 |
|---|---|---|---|---|---|---|
| `total` | 6302726 | 28802025 | 38795975 | 10398128 | 15418808 | 293455 |
| `cmp` | 5.776 | 4.614 | 4.696 | 6.020 | 5.557 | 6.064 |
| `adds_subs` | 0.420 | 0.592 | 0.450 | 0.377 | 0.470 | 0.737 |
| `tst` | 0.562 | 0.204 | 0.310 | 0.124 | 0.422 | 0.207 |
| `ands` | 0.092 | 0.049 | 0.098 | 0.018 | 0.099 | 0.043 |
| `ccmp` | 0.296 | 0.096 | 0.063 | 0.069 | 0.240 | 0.371 |
| `b_cond` | 6.124 | 4.783 | 4.780 | 5.609 | 5.776 | 6.368 |
| `cbz` | 3.299 | 3.878 | 3.011 | 3.396 | 3.915 | 3.877 |
| `tbz` | 2.332 | 1.641 | 1.550 | 0.771 | 1.150 | 1.183 |
| `csel` | 0.452 | 0.593 | 0.710 | 0.844 | 0.632 | 0.383 |
| `cset` | 0.233 | 0.164 | 0.179 | 0.230 | 0.195 | 0.319 |
| `csinc_inv_neg` | 0.262 | 0.113 | 0.155 | 0.124 | 0.162 | 0.127 |
| `adc_sbc` | 0.000 | 0.002 | 0.006 | 0.006 | 0.025 | 0.020 |
| `asr_imm` | 0.035 | 0.070 | 0.217 | 0.227 | 0.036 | 0.082 |
| `asr_reg` | 0.035 | 0.002 | 0.007 | 0.001 | 0.001 | 0.007 |
| `sbfx` | 0.002 | 0.003 | 0.013 | 0.003 | 0.007 | 0.011 |
| `lsl_imm` | 0.271 | 0.208 | 0.273 | 0.191 | 0.281 | 0.297 |
| `lsr_imm` | 0.199 | 0.323 | 0.582 | 0.364 | 0.410 | 0.172 |
| `rbit` | 0.014 | 0.032 | 0.016 | 0.010 | 0.052 | 0.018 |
| `clz` | 0.018 | 0.051 | 0.036 | 0.017 | 0.070 | 0.054 |
| `rev64` | 0.000 | 0.005 | 0.002 | 0.006 | 0.006 | 0.004 |
| `rev32` | 0.000 | 0.023 | 0.017 | 0.005 | 0.007 | 0.050 |
| `madd` | 0.007 | 0.098 | 0.085 | 0.076 | 0.089 | 0.031 |
| `msub` | 0.076 | 0.020 | 0.030 | 0.016 | 0.016 | 0.052 |
| `smaddl_umaddl` | 0.065 | 0.056 | 0.058 | 0.042 | 0.157 | 0.021 |
| `smull_umull` | 0.094 | 0.021 | 0.025 | 0.032 | 0.021 | 0.061 |
| `mul` | 0.019 | 0.074 | 0.133 | 0.143 | 0.125 | 0.054 |
| `sdiv` | 0.006 | 0.005 | 0.010 | 0.008 | 0.002 | 0.018 |
| `udiv` | 0.021 | 0.013 | 0.022 | 0.022 | 0.008 | 0.047 |
| `ls_imm` | 24.730 | 23.204 | 20.935 | 24.456 | 24.296 | 20.063 |
| `ls_regoff_plain` | 0.470 | 0.317 | 0.342 | 0.354 | 0.528 | 1.289 |
| `ls_regoff_lsl` | 0.287 | 0.147 | 0.228 | 0.258 | 0.375 | 0.390 |
| `ls_regoff_sxtw` | 0.336 | 0.012 | 0.041 | 0.035 | 0.017 | 0.152 |
| `ls_regoff_uxtw` | 0.089 | 0.032 | 0.065 | 0.059 | 0.120 | 0.079 |
| `ls_preindex` | 0.019 | 0.481 | 0.259 | 0.194 | 0.117 | 0.135 |
| `ls_postindex` | 0.059 | 0.455 | 0.218 | 0.274 | 0.227 | 0.216 |
| `ls_fpr` | 0.459 | 1.744 | 1.406 | 1.595 | 2.505 | 0.500 |
| `ldp_stp_imm` | 5.428 | 6.264 | 6.894 | 4.893 | 2.649 | 5.590 |
| `ldp_stp_preindex` | 0.415 | 0.222 | 0.595 | 0.354 | 0.244 | 0.490 |
| `ldp_stp_postindex` | 0.658 | 0.273 | 0.731 | 0.405 | 0.312 | 0.700 |
| `bl` | 6.903 | 4.677 | 6.942 | 6.773 | 5.284 | 4.609 |
| `blr` | 0.340 | 2.351 | 0.495 | 1.251 | 0.393 | 0.192 |
| `ret` | 0.989 | 0.879 | 1.192 | 0.843 | 0.470 | 1.471 |
| `br` | 0.048 | 0.121 | 0.067 | 0.060 | 0.049 | 0.059 |
| `fp_scalar_arith` | 0.003 | 0.283 | 0.198 | 0.436 | 0.067 | 0.028 |
| `fcmp` | 0.001 | 0.118 | 0.131 | 0.210 | 0.070 | 0.006 |
| `fcsel` | 0.000 | 0.040 | 0.035 | 0.044 | 0.030 | 0.010 |
| `fmov_d_from_x` | 0.027 | 0.040 | 0.025 | 0.081 | 0.057 | 0.045 |
| `fmov_gpr_from_fpr` | 0.019 | 0.088 | 0.043 | 0.047 | 0.128 | 0.041 |
| `cvt_to_float_scalar` | 0.003 | 0.088 | 0.057 | 0.160 | 0.054 | 0.001 |
| `cvt_to_int_scalar` | 0.000 | 0.034 | 0.027 | 0.083 | 0.019 | 0.000 |
| `cvt_to_float_vec` | 0.000 | 0.039 | 0.010 | 0.016 | 0.002 | 0.000 |
| `cvt_to_int_vec` | 0.000 | 0.040 | 0.006 | 0.010 | 0.000 | 0.000 |
| `fcvt_scalar` | 0.000 | 0.025 | 0.024 | 0.055 | 0.005 | 0.000 |
| `frint` | 0.000 | 0.011 | 0.010 | 0.016 | 0.005 | 0.001 |
| `fp_vec_arith` | 0.000 | 0.451 | 0.157 | 0.140 | 0.014 | 0.003 |
| `int_vec_arith` | 0.011 | 0.335 | 0.200 | 0.127 | 0.382 | 0.043 |
| `vec_cmp` | 0.012 | 0.095 | 0.024 | 0.012 | 0.143 | 0.027 |
| `vec_shift` | 0.003 | 0.147 | 0.063 | 0.060 | 0.069 | 0.004 |
| `addp_vec` | 0.000 | 0.002 | 0.005 | 0.001 | 0.030 | 0.002 |
| `vec_reduce` | 0.001 | 0.016 | 0.012 | 0.004 | 0.017 | 0.003 |
| `narrow` | 0.001 | 0.053 | 0.059 | 0.013 | 0.043 | 0.009 |
| `widen` | 0.001 | 0.081 | 0.026 | 0.024 | 0.105 | 0.014 |
| `dup` | 0.001 | 0.135 | 0.056 | 0.132 | 0.047 | 0.012 |
| `ins_umov` | 0.009 | 0.128 | 0.075 | 0.088 | 0.060 | 0.027 |
| `ld1r` | 0.000 | 0.007 | 0.009 | 0.003 | 0.003 | 0.001 |
| `ldN_stN` | 0.006 | 0.074 | 0.050 | 0.024 | 0.051 | 0.010 |
| `tbl` | 0.000 | 0.078 | 0.008 | 0.004 | 0.027 | 0.001 |
| `zip_uzp_trn_ext` | 0.007 | 0.086 | 0.072 | 0.033 | 0.052 | 0.008 |
| `crypto_crc` | 0.000 | 0.000 | 0.005 | 0.004 | 0.015 | 0.000 |
| `exclusives` | 0.000 | 0.761 | 0.001 | 0.000 | 0.002 | 0.014 |
| `lse_cas` | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.002 |
| `lse_rmw` | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.005 |
| `acq_rel_ls` | 0.002 | 0.689 | 0.194 | 0.109 | 0.066 | 0.021 |
| `barriers` | 0.000 | 0.140 | 0.001 | 0.000 | 0.059 | 0.018 |
| `mrs` | 0.000 | 0.027 | 0.016 | 0.003 | 0.044 | 0.505 |
| `movwide` | 7.710 | 5.933 | 4.549 | 5.424 | 6.437 | 7.722 |
| `adrp` | 4.371 | 5.387 | 2.229 | 3.010 | 2.170 | 3.211 |
| `nop` | 1.249 | 0.112 | 0.180 | 0.173 | 5.476 | 3.396 |

Reading the table: LSE opcodes are 0.000% in every application binary because all of them reach LSE through the outline-atomics helpers; only libxul carries inline exclusives (and 40k `dmb` sites, each a `hwsync`). The `nop` rows are alignment padding, which the frontend translates to nothing. `blr` at 2.35% of libxul is virtual dispatch, i.e. P2 territory, not an ISA item.
