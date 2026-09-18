# Cold-block mechanism for A64 floating-point NaN semantics (design, not implemented)

Owner note: design only. Nothing in this file has been built or measured; every
number quoted is from the research docs and is cited as such. Rows F1, F2, F3, N5
of `OPTIMIZATION-CHECKLIST.md` (and the slow half of F6) are blocked on the
mechanism described here (checklist lines 62-70).

## 0. Recommendation in one paragraph

Three new A64-only IR ops (`A64FArith`, `A64FMinMax`, `A64FMulAdd`, one per
family, all lanes of the register) whose PPC64LE lowering is the plain VSX
instruction plus a two-instruction NaN check (`xvcmpeq{dp,sp}. VTMP1,r,r; bc`
on CR6) that branches forward to a per-site cold stub. Cold stubs are emitted
out of line inside the compile unit, at the end of the unit next to the block-link
thunks and the shared spill stubs, or earlier at a short-branch island when the
unit outgrows the `bc` reach, using the age bookkeeping that already exists for
`CondJump`'s short branches. Each stub copies its operands into fixed low-bank
VSX registers, `bl`s a per-unit shared body (one per rule and width, emitted on
demand: `NaNFix`, `NMPrep`, `FMAFix`), re-runs the site's own arithmetic on the
fixed operands, writes the destination and `b`s back to the join label. The
bodies are host-instruction transcriptions of the IR chains they replace
(`PropagateNaNOperand`, `FPMinMax`, `FPMulAddLanes`), which are already
Pi-golden verified, so the cold path's specification is the existing frontend
code. Sites write CR6 only; bodies write no CR field, no XER, no GPR other than
TMP1-TMP2, no memory, and LR only as a balanced `bl`/`blr` pair with LR parked
in TMP4 across it (section 5; the first draft wrongly held LR to be scratch
between ops). Everything is ISA 2.06/2.07; no ISA 3.0 form is used, so
`disableisa30` is the same code.
First landing: `A64FArith` (F1) alone with the stub/body infrastructure, gated
by `fp_scalar`, `simd_float`, `fp_half`, `fpbugs`, `fpmath` in all three modes
plus a positive control that skips the branch and must fail `fp_scalar`.

## 1. What exists today, and the constraints it sets

**Frontend.** Every scalar and vector FP arithmetic op runs a fix-up chain
unconditionally:
- `PropagateNaNOperand` (`A64Frontend/TranslateFP.cpp:135-145`): 8 IR ops
  (two `VFCMPUNO`, `VAnd`/`VNot`/`VCMPEQZ` pairs, a `VBSL`) plus a constant, in
  front of every `VFAdd/VFSub/VFMul/VFDiv` at `TranslateFP.cpp:384-388`,
  `TranslateSIMDFloat.cpp:66-70` and the FABD at `TranslateFP.cpp:707`.
- `FPMinMax` (`TranslateFP.cpp:147-170`): ordered compares and selects (the
  NM forms add the lone-qNaN-to-Inf replacement), calling `PropagateNaNOperand`
  again; used at `TranslateFP.cpp:389-392`, `TranslateSIMDFloat.cpp:71-74`, and
  through `FPBinaryLanes` by the pairwise forms (`TranslateSIMDFloat.cpp:158-187`).
- `FPMulAddLanes` (`TranslateFP.cpp:415-451`): `VFMLA` followed by roughly 30
  IR ops for NaN precedence over (addend, n, m) and the qNaN-addend-with-Inf x 0
  default-NaN rule; used by FMADD/FMSUB/FNMADD/FNMSUB (`:453-495`, operands
  pre-negated per B1) and by vector/by-element FMLA/FMLS
  (`TranslateSIMDFloat.cpp:125,147`).
- Half precision is widened to double with signalling NaNs kept
  (`HalfToDouble(.., true, true)`, `TranslateFP.cpp:377-379`), so it rides on the
  double path automatically.
- FPCR.DN and FZ are stored but not emulated (`TranslateFP.cpp:795`), FPSR
  cumulative flags are not emulated (`:335`), so the cold path has no DN/FZ/FPSR
  obligations either.

**Backend.** `VFAdd` is one `xvadddp`/`xvaddsp` (`JIT/PPC64LE/VectorOps.cpp:2192-2203`);
`VFMin`/`VFMax` implement x86 MINPS semantics (`:2278-2320`), which is why the
A64 frontend cannot use them; `VFMLA` copies the addend into Dst and stashes an
aliased source in VTMP1/VTMP2 (`:3799-3817`). The one A64-specific fused op with an
in-op cold branch already exists: `A64FloatToGPR` uses `xscmpudp cr1` and
`bc(Cond{4,7})` over a local `li Dst,0` (`A64FPOps.cpp:117-131`).

**Cost.** FP research §3.2: the branch-free fix is 11 dependent instructions at
18.35 cycles latency against 6.35 for the add alone; the pre-check is 3
instructions at 6.57 and the recording post-check (`xvaddsp; xvcmpeqsp.; bc`) 3 at
6.51 [MEASURED]. §13: the fix-up "costs more than the arithmetic itself, three
times over". NEON §4.3 says the same for the lane forms.

**Constraints read out of the code:**
1. IR blocks cannot be split per op: `ConstrainedRAPass::Run` resets
   `Class.Available` at every block (`IR/Passes/RegisterAllocationPass.cpp:607-609`)
   and no non-fixed SSA value crosses a block edge (checklist P1). So the fast
   path and the cold path must be one IR op, and the cold code must live outside
   the IR's notion of blocks entirely (host-level labels only).
2. `bc` reaches +-32 KB (`CodeEmitter/PPC64LE/Emitter.h:1664-1669`). Short
   forward branches to later blocks already go through `ShortCondLabel` and an
   island emitted between IR ops once the oldest branch is `kShortCondIslandAge`
   (12000) bytes old (`JITClass.h:316-336`, `JIT.cpp:2873-2898`, called at
   `JIT.cpp:5791`; the unit tail dies if any is still unbound, `JIT.cpp:5918`).
3. P6's block shape falls through into the next block whenever it can
   (`BranchOps.cpp:1302-1340`, `Jump` at `:1160-1176`), so there is no dead gap
   after a block where cold code could sit without a jump over it.
4. CR0 is guest NZCV (`JITClass.h:928-935`; `FCmp` writes CR0 and lifts to XER,
   `ALUOps.cpp:4207-4243`). CR1 caches the XER projection and every op not on the
   `kOpCacheKeepXER` allowlist invalidates it after the handler (`JIT.cpp:4782-4800`,
   `:5808-5820`). CR3 holds composites (`JIT.cpp:1034-1044`), CR5 is used inside
   the `dcbz` loop (`MemoryOps.cpp:2005`), CR6 is written by the record-form
   vector compares inside `CondJump` (`BranchOps.cpp:1183-1200`,
   `Emitter.h:1428-1444`), CR7 is the established scratch field for
   `Select`/`NZCVSelect`/`EmitCompare` (`JIT.cpp:918-925`, `ALUOps.cpp:3723-3733`).
   XER.SO must never be cleared (FP §7.2; `PPC64Emitter.h` `ZeroCAOV` note).
5. Scratch registers: TMP1-TMP4 = r3-r6, VTMP1/VTMP2 = v30/v31 (`PPC64Emitter.h:31-41`),
   VTMP3_VSX = vs12 (VSX-form only, not live across a host call, `:43-60`),
   VZERO_VSX = vs14 (read-only, and only its dw0 may be read, `:62-78,80-95`).
   The low bank vs16-vs31 is the AVX-high bank when `SupportsAVX` (`:140-160`,
   off on this tree); nothing may be *pinned* in vs14-vs31 (`:104`). The low bank
   is RA-invisible ("RA-invisible low bank", `:665`). r0 must stay zero between
   ops (`JIT.cpp` thunk comment, "the target block relies on it").
6. **LR must be preserved across an op. CORRECTED 2026-09-17 — the first draft
   of this document had this backwards and the error was load-bearing for §5.**
   The backend's rule is the opposite of "LR is scratch": it is stated in tree,
   twice, at `BranchOps.cpp:23` and `:1035` --

   > "Every `mflr(r0)` in the backend today is paired with a restore, so no bug
   > is visible -- but the invariant is currently globally assumed, and the
   > failure mode is silent guest memory corruption."

   and every one of the eight mid-op LR clobbers honours it: `ALUOps.cpp:3533/
   3550`, `:3749/3766`, `:3810/3835`, `AtomicOps.cpp:154/178`, `:190/224`,
   `VectorOps.cpp:4934/4948`, `:4977/4988`, `:5023/5035`. There is no precedent
   anywhere for clobbering LR mid-op and leaving it clobbered.

   What the earlier draft cited does not support the claim.
   `EmitStoreBlockBeginToInlineHeader`'s comment (`JIT.cpp:2727`) says LR is
   dead "at any dispatcher/link entry into a ppc64le block ... before any IR op
   runs" -- block entry, not between arbitrary ops -- and that function is
   gated off by default anyway (`FEX_NOBLOCKHEADER` ON). The thunk-leg comment
   quoted as "the established hot-path precedent" does not exist; that phrase
   appears nowhere in the tree. `JIT.cpp:2727` is the only statement about LR
   liveness in the backend.

   Second hazard, missed entirely by the earlier draft: **routing LR through r0
   breaks the global `r0 == 0` invariant**, because JIT blocks address guest
   memory as `ldx/stdx rX, rBase, r0`. `ALUOps.cpp:3768` records the real bug --
   `Yield` omitted the trailing `li r0, 0` and a guest died with SIGSEGV on its
   first push past the PAUSE threshold, `stdx r3, r11, r0` having indexed by a
   code address. Any LR save that goes through r0 must re-zero it.
7. The A64 static vector set is V0-V15 (`unittests/A64Frontend/gen_simd.py:79`,
   `JIT.cpp:2054 a64::SRAFPR`); the RA prefers the SRA register the result is
   next stored to (`RegisterAllocationPass.cpp:626-633`), so for accumulator
   shapes like `fadd d0, d0, d1` the destination routinely aliases a source
   (FP §3.2: "FEX's RA may tie the destination to a dead source"). The cold path
   must therefore see stashed originals whenever the destination aliases.
8. Emitter gaps: `xvcmpeqdp`/`xvcmpeqsp` exist only as Rc=0 (`Emitter.h:1110-1114`);
   the record-form twins exist for the integer compares only (`:1440-1443`).
   Low-bank `VSXR` overloads exist for `xvmadd*`, `xvneg*`, `xxpermdi`, `xxlxor`,
   `xxspltw`, `xxsel`, `xxlor` (`:930-980`); `xxland`/`xxlandc`/`xxlnor`/
   `xxlnand`, `xvcmpeq*`, `xvadd*`, `xvmax*`/`xvmin*` are VR-only (`:982-992`,
   `:905-910`, `:1106-1114`). No `setb`, `mffsl`, `xststdcdp` (not needed here).

## 2. IR surface

Three ops, one per family, in the `Conv` section of `IR/IR.json` next to
`A64FloatToGPR`/`A64FloatFromGPR`/`A64FToF` (`IR.json:2747-2767`), following their
conventions (`OpSize:#RegisterSize, OpSize:#ElementSize` like `VFAdd` at
`:2323`, `u8` kinds documented in `Desc` like `A64FloatToGPR`'s `Rounding`). No
`TiedSource`, no `HasSideEffects` (pure, DCE-able like `VFAdd`), no
`ImplicitFlagClobber` (that flag makes the generated emitter call `SaveNZCV`,
`Scripts/json_ir_generator.py:689-690`; the A64 IRBuilder inherits the no-op
`IREmitter.h:563`, and these lowerings touch no NZCV state anyway).

```
"FPR = A64FArith OpSize:#RegisterSize, OpSize:#ElementSize, FPR:$Vector1, FPR:$Vector2, u8:$Op": {
  "Desc": ["A64 FPAdd/FPSub/FPMul/FPDiv on every ElementSize lane of RegisterSize.",
           "Op: 0 Add, 1 Sub, 2 Mul, 3 Div (matches IRBuilder::FPBinaryOp order).",
           "NaN rule (FPProcessNaNs): a lane whose Vector1 is a quiet NaN and whose Vector2 is a",
           "signalling NaN yields quiet(Vector2) with its payload; every other NaN lane yields",
           "quiet(first NaN operand in order Vector1, Vector2). Inf-Inf, 0*Inf, 0/0, Inf/Inf give",
           "the default NaN. Non-NaN lanes are the host result in the host rounding mode.",
           "FNMUL and FABD are this op followed by VFNeg / VFAbs (FPNeg/FPAbs flip a NaN's sign too).",
           "Lowering: one xv{add,sub,mul,div}{dp,sp} plus a NaN check branching to an out-of-line",
           "cold stub (COLD-BLOCK-DESIGN.md); the fast path never enters the stub on ordered data."],
  "DestSize": "RegisterSize", "ElementSize": "ElementSize"
},
"FPR = A64FMinMax OpSize:#RegisterSize, OpSize:#ElementSize, FPR:$Vector1, FPR:$Vector2, i1:$IsMax, i1:$IsNumber": {
  "Desc": ["A64 FPMin/FPMax (IsNumber=0) or FPMinNum/FPMaxNum (IsNumber=1) per lane.",
           "Ordered lanes: the min/max, with -0 below +0 (max(+0,-0)=+0, min=-0).",
           "IsNumber=0: any NaN operand yields the FPProcessNaNs result (A64FArith's NaN rule).",
           "IsNumber=1: a quiet NaN whose other operand is not a quiet NaN is first replaced by",
           "-Inf (IsMax) / +Inf (!IsMax); the lane is then min/max'ed, so a lone qNaN loses and",
           "an sNaN or a pair of qNaNs still propagates per FPProcessNaNs.",
           "Lowering: xv{max,min}{dp,sp} plus the NaN check; xsmaxjdp/xsmaxcdp are not usable",
           "(FP research 6.1: Java and C NaN rules, neither A64's)."],
  "DestSize": "RegisterSize", "ElementSize": "ElementSize"
},
"FPR = A64FMulAdd OpSize:#RegisterSize, OpSize:#ElementSize, FPR:$Addend, FPR:$Vector1, FPR:$Vector2": {
  "Desc": ["A64 FPMulAdd: Addend + Vector1*Vector2 per lane, one rounding.",
           "Operand negations (FMSUB/FNMADD/FNMSUB, FMLS) are applied by the frontend BEFORE this",
           "op, as A64 does (FPNeg flips a NaN's sign); the host's negating fused forms are never",
           "used (checklist B1, FP research 6.2).",
           "NaN rule (FPProcessNaNs3 in order Addend, Vector1, Vector2): the first signalling NaN,",
           "else the first quiet NaN, quieted with payload; then, if Addend is a quiet NaN and the",
           "product is Inf*0 or 0*Inf, the default NaN instead. Non-NaN lanes with an invalid",
           "product (Inf*0, no NaN addend) give the default NaN on both architectures.",
           "Lowering: copy Addend, xvm{add,sub}a{dp,sp}, NaN check on the result, cold stub."],
  "DestSize": "RegisterSize", "ElementSize": "ElementSize"
}
```

Why not finer (one op per arithmetic kind): the checklist asks for one per
family, one `DEF_OP` per family keeps one validation path and one profiler
bucket family, and the `u8` kind mirrors `A64FloatToGPR`. Splitting `A64FArith`
into four ops later is cosmetic. Why not one op for everything: the three cold
bodies have different operand counts (2, 2+constant, 3+result) and the min/max
site needs a select the arithmetic site does not.

Why no conversions op: `A64FloatToGPR` already is the fused conversion op with
its in-op cold branch (`A64FPOps.cpp:127-130`); F6's work is inside that
`DEF_OP` (section 7).

RegisterSize is always `i128Bit` from the frontend, as today
(`_VFAdd(RS=i128, ...)`), and scalar results keep going through `StoreVSized`'s
`VMov` (`IRBuilder.h:302-308`). Folding the scalar zero-extend into the op is
the natural home for F4's elision later, but it is not part of this design.

**Frontend deletions once all three land:** `PropagateNaNOperand`, `FPMinMax`,
the tail of `FPMulAddLanes` after `_VFMLA`, and their declarations
(`IRBuilder.h:390-391`); `FPBinaryLanes`/`FPTwoRegister` become one op per
case; `FPThreeRegister` keeps its operand negations. Gate on a backend
capability flag `SupportsA64FPFused` set with the other backend-capability flags
(`Source/Common/HostFeatures.cpp:760-799`, pattern of `SupportsVCmpFlagBranch`,
`HostFeatures.h:77-88`), with the A/B knob one level up in the frontend
(`POWERARM_FPFUSED=0` keeps the old chains) for the checklist's before/after
measurement; delete the old chains in the commit that marks the rows done.

## 3. Fast-path lowering

All in `A64FPOps.cpp`. `Da`, `Db`, `Dd` are the RA registers; `dp`/`sp` selected
by ElementSize; `xvcmpeq*_` is the record form (new emitter twin, section 8).

**A64FArith** (post-check on the result, FP §3.2 row "f32 recording compare",
6.51 cycles; the dp case measured 6.62 for the `xscmpudp` post-check, the
`xvcmpeqdp.` form was only measured as a pre-check, see risk 1):
```
  [xxlor VTMP2, Ds, Ds]          ; only when Dd aliases a source Ds: stash the original
  xv{add,sub,mul,div}{dp,sp} Dd, Da, Db
  xvcmpeq{dp,sp}. VTMP1, Dd, Dd  ; CR6[0] (CR bit 24) set iff no lane is NaN; VTMP1 discards VRT
  bc 4, 24, .cold                ; Cond{4, 24}: branch if "all equal" is false
.join:
```
Two instructions on the ordered path, three when the destination aliases. The
post-check is complete for the binary ops: a NaN result implies a NaN operand or
an invalid operation, and invalid operations give the default NaN on both sides
(FP §3.1), so the cold stub's recomputation is always right and the branch is
never taken on ordered data. The compare runs off the result, so it does not
lengthen the dependent chain seen by the next op (the `bc` resolves late and is
predicted not taken). It also fires when a *garbage* lane is NaN (FP §3.2 note),
which is correct but slow; see risk 1.

**A64FMinMax** (pre-check on both operands; a post-check cannot work because
`xvmaxdp` returns the number for a lone NaN, FP §6.1):
```
  [xxlor VTMP2, Ds, Ds]          ; alias stash as above
  xvcmpeq{dp,sp}. VTMP1, Da, Da
  bc 4, 24, .cold
  xvcmpeq{dp,sp}. VTMP1, Db, Db
  bc 4, 24, .cold
  xv{max,min}{dp,sp} Dd, Da, Db
.join:
```
Four extra instructions, all independent of the min/max itself (pre-check
measured 6.57 vs 6.35 bare for the add, FP §3.2). `xvmaxdp` is +-0-correct
(`SPEC p.584`, FP §6.1 [MEASURED]).

**A64FMulAdd** (post-check; the a-form is destructive on the addend, so the
addend is always copied so the cold stub can see it):
```
  ; VR A = Add, N = V1, M = V2; the a-form needs T == Add on entry
  if Dd == Add:            xxlor VTMP2, Add, Add   ; original addend for the cold stub
  else:                    [stash N/M in VTMP2 if Dd aliases one, as DEF_OP(VFMLA) does]
                           xxlor Dd, Add, Add
  xvmadda{dp,sp} Dd, N, M
  xvcmpeq{dp,sp}. VTMP1, Dd, Dd
  bc 4, 24, .cold
.join:
```
One copy, the fused op, two check instructions; FP §6.2 measured "copy addend +
xsmaddadp + post-check 6.66" against 5.97 bare. The copy is inherent (the cold
path needs the original addend and the a-form overwrites T), so `TiedSource`
buys nothing and is omitted. FMSUB/FNMADD/FNMSUB arrive with `VFNeg`'d
operands from the frontend, so only `xvmadda*` is ever emitted here (B1 rule:
never `xvnm*`).

Static branch hints: the emitter's `bc(Cond, Label*)` emits BO 4/12 with no
hint (`Emitter.h:1684-1692`); the ISA `001at` form with a=1,t=0 ("very likely
not taken", BO=6) is available through `bc(bo, bi, offset)` but its effect on
POWER9 is unmeasured and the design does not rely on it.

## 4. Where the cold code lives

Options weighed:

(a) **Inline after the block's code, branched over.** Rejected. P6 makes blocks
fall into their successors (`BranchOps.cpp:1312-1340`, `Jump` elision `:1170-1176`),
so cold code placed after a block needs a `b over` on the hot path, and it lands
in the same icache lines as the hot code (P10: 1.07 G L1 icache misses, 18x
native, is a live problem). Per-op inline placement is the same but worse.

(b) **Per-compile-unit cold region.** Recommended. The unit tail already holds
cold-by-construction code: block-link thunks and the shared miss-leg spill stubs
(`JIT.cpp` "Shared miss-leg spill stubs ... Cold by construction"), emitted on
demand behind `SharedSpillExitUsed`/`SharedSpillLinkUsed` flags. Cold stubs and
the shared bodies go there, before the `ShortCondBranches` unbound check
(`JIT.cpp:5918`) so a stub can still be a `bc` target. Reach: the site's `bc`
must reach the stub within 32 KB. Reuse the island mechanism verbatim: a
`ColdStubs` list next to `ShortCondBranches` (`JITClass.h:329`), each entry
{site label, join label, emit callback}; `EmitShortCondIsland` (`JIT.cpp:2887`)
grows a sibling `EmitColdStubs()` that flushes every pending stub (and any
body they need) under the island's existing `b over`, triggered by the same
age test at `JIT.cpp:5791` (oldest pending entry > `kShortCondIslandAge`). A
stub is always emitted after its site, so the `bc` is forward and no suspend
poke applies (those are for backward edges only, `BranchOps.cpp:1322-1335`).
Inside the code buffer, so the signal delegator's `IsAddressInCodeBuffer` proxy
("SRA may be live") holds for a signal delivered mid-stub, exactly as for the
spill stubs. No absolute addresses in stubs or bodies (relative `bc`/`b`/`bl`
only, constants are immediates), so nothing new for `RetainRelocations` / the
code cache.

(c) **One shared routine per rule in the dispatcher.** Not for the first
landing. The `bl` reach (+-32 MB) is not guaranteed across code buffers (the
buffer is at least 16 MiB and configurable, `CPUBackend.cpp:757-764`; the
dispatcher is a separate allocation), so a site would reach it with
`ld TMPx, <slot>(STATE); mtctr; bctrl` via a CpuStateFrame pointer, the way
`PPC64_HelperTable` is reached (`JITClass.h:46-72, 1097-1125`). That is fine on
a cold path, and it removes per-unit body emission entirely, but it needs
dispatcher emission code, a frame slot and one more count-cache branch, and the
"single implementation" argument of FP §11 is already met by (b): one C++
function emits the body, per-unit copies are instances of it. Keep the stub to
body ABI (section 5) such that moving the bodies into the dispatcher later
changes only the call sequence in the stub.

**Per-site stub vs shared body.** A per-site inline body is roughly 15-30
instructions per FP op; a stub calling a shared body is 5-8 per site plus one
body per (rule, width) per unit. The bodies are emitted only when a site of
that kind exists in the unit (`Used` flag per body, the `SharedSpillExitUsed`
pattern), so a unit with no FMA carries no `FMAFix`. Translation cost is a
first-order concern on this tree (checklist X rows), but the dominant saving
either way is the 30-40 IR ops per FP op that stop flowing through the passes
and the RA; the stub-vs-inline difference is code-buffer bytes. Shared bodies
chosen. The `bl`/`blr` conflict this originally hedged against (risk 3) was
audited and resolved on 2026-09-17: LR needs a two instruction park, not a
different shape, so the per-site inline fallback is not required.

Stub shapes (dp shown; `P0..P5` are the fixed low-bank registers of section 5;
`Da'`/`Db'` are the RA registers or the VTMP2 stash when aliased):
```
.cold_arith:                       ; A64FArith
  xxlor P0, Da', Da'
  xxlor P1, Db', Db'
  bl    NaNFix_dp                  ; P0 <- A' (swapped where A qNaN and B sNaN)
  xv<op>dp Dd, P0, P1              ; the site's own arithmetic, ordered lanes unchanged
  b     .join

.cold_minmax:                      ; A64FMinMax, IsNumber=0
  xxlor P0, Da', Da' ; xxlor P1, Db', Db'
  bl    NaNFix_dp                  ; P2 <- unordered mask, P3 <- propagated quiet NaN per lane
  xv{max,min}dp VTMP1, P0, P1
  xxsel Dd, VTMP1, P3, P2          ; NaN lanes take P3
  b     .join

.cold_minmaxnm:                    ; A64FMinMax, IsNumber=1
  xxlor P0, Da', Da' ; xxlor P1, Db', Db'
  LoadConstant TMP1, {-Inf | +Inf} ; IsMax ? 0xFFF0.. : 0x7FF0.. (sp: word pattern replicated)
  mtvsrd VTMP1, TMP1 ; xxpermdi P4, VTMP1, VTMP1, 0
  bl    NMPrep_dp                  ; P0/P1 <- lone quiet NaNs replaced by P4
  bl    NaNFix_dp
  xv{max,min}dp VTMP1, P0, P1
  xxsel Dd, VTMP1, P3, P2
  b     .join

.cold_fma:                         ; A64FMulAdd
  xxlor P0, Add', Add' ; xxlor P1, N', N' ; xxlor P2, M', M'
  xxlor P3, Dd, Dd                 ; the fused result (used for non-NaN lanes)
  bl    FMAFix_dp                  ; P3 <- final result
  xxlor Dd, P3, P3
  b     .join
```
`.join` is already bound when the stub is emitted (backward `b`, 24-bit, always
in reach). The cold path takes two taken branches; its cost is irrelevant (FP
§11).

## 5. Register and CR discipline

**Site (hot path).** Writes Dd, VTMP1 (discard VRT of the record compare),
VTMP2 (alias stash), CR6. Reads its RA operands. No GPR, no CR0, no CR1, no
XER, no LR, no memory. Consequences: the new ops are correctly *not* on the
`kOpCacheKeepXER` allowlist (they write a CR field), so the CR1 projection cache
is invalidated after them by default (`JIT.cpp:5808-5820`); they write no dynamic
GPR, so they may join the `kOpCacheKeepConst` set alongside the scalar-insert
family (`JIT.cpp:4797-4800`), optional.

CR6 is safe to write here, but not for the reason the first draft gave: it
claimed the record-form compares inside `CondJump` were CR6's only writers,
which is false. SURVEYED 2026-09-17, three users and no more --
`BranchOps.cpp:1191-1192` (the `CondJump{VCmpElementSize}` vector compare),
`DEF_OP(MemCpy)` (`MemoryOps.cpp:1952-2060`, which deliberately stages a
`delta >= 16` predicate in CR6 *across* its alignment loop and reads it back at
`:2042`), and `DEF_OP(VAnyNonZero)` (`VectorOps.cpp:2628-2636`, read through
`mfocrf` FXM 0x02). All three are confined to one op, so CR6 never crosses an IR
op boundary and these sites may use it freely. The conclusion stands; it just
had to be earned rather than asserted. CR0/NZCV is untouched, so
none of the `mfocrf 0x80` save/restore ceremony that the atomics need
(`AtomicOps.cpp:490-500`, `:548-563`, `:1048-1058`) applies; the atomics need it
because `andi.`/`stdcx.` write CR0, which nothing here does. The lesson recorded
at `AtomicOps.cpp:552-558` (a stash register that is also the op's scratch
silently corrupts an aliased operand) is why the alias stash is VTMP2 and the
compare's discard target VTMP1, never the same register.

**Stub (cold, per site).** Adds TMP1 (constant materialisation for the NM Inf
and, in the bodies, the masks), TMP4 (the LR park, see below), VTMP1, P0..P5,
and LR itself -- saved and restored, not left clobbered. Still no CR write (the
bodies below are branch-free; the stub's only branches are `bl`, `blr`, `b`), no
XER, no r0 (deliberately: see the LR note), no r1/memory. Guest state is untouched until the final write of Dd, so a
signal delivered anywhere in the stub sees the pre-op state, exactly like any
multi-instruction `DEF_OP` today.

**Body ABI** (per unit, reached by `bl`, returns by `blr`):

| body | in | out | clobbers |
|---|---|---|---|
| `NaNFix_{dp,sp}` | P0=A, P1=B | P0=A' (B where A is qNaN and B sNaN, else A), P2=unordered mask, P3=`xvadd(A',B)` (the propagated quiet NaN in NaN lanes, garbage elsewhere) | P4, P5, VTMP1, VTMP2, TMP1 |
| `NMPrep_{dp,sp}` | P0=A, P1=B, P4=Inf constant | P0/P1 with lone-quiet-NaN lanes replaced by P4 | P5, VTMP1, VTMP2, TMP1 |
| `FMAFix_{dp,sp}` | P0=Addend, P1=N, P2=M, P3=fused result | P3=final | P4, P5, VTMP1, VTMP2, VTMP3_VSX, TMP1, TMP2 |

P0..P5 = vs3..vs8 (VSX-form only: `xxl*`, `xxsel`, `xxpermdi`, `xv*`). Why the
low bank: the bodies need six to nine live vector values and only two VMX
temporaries exist; vs3-vs11 and vs13 are unused by the backend (`f0`-`f2` are the
only named FPRs, `PPC64Emitter.h:45-47`; vs12 is VTMP3_VSX, vs14 VZERO_VSX; vs15
is free but was the AES-mask trap, `:104-107`; vs16-vs31 are the AVX bank when it
is on). They are RA-invisible, transient within the stub (no host call can occur
between `bl` and the stub's final `b`), and the "not pinned across calls" rule
(`:80-110`) is satisfied because nothing is pinned. Zero is materialised with
`xxlxor` inside the body; VZERO_VSX is never read full-width (its dw1 is
undefined after any host call, `:85-88`), and its dw0-only read rule is not
worth the permute here. The two VMX temporaries and TMP1/TMP2 are free inside
the stub because the site's op has finished with them.

**LR. REWRITTEN 2026-09-17; the first draft was wrong here.** `bl`/`blr` inside
a block is still a balanced link-stack push/pop, which is what P3 cares about,
and the shared-body shape survives. But LR is *not* scratch between ops
(section 1, item 6, corrected), so the stub must preserve it rather than simply
clobbering it. The per-site inline-body fallback is not needed.

The in-tree precedent for a mid-op LR clobber is the host-call ceremony at
`VectorOps.cpp:4934-4950`: `stdu` a mini-frame, `mflr r0; std r0, 16, r1`, then
on the way out `ld r0, 16, r1; mtlr r0; addi r1, r1, FrameSize; li r0, 0`. The
frame is not optional there, because `16(r1)` of a frame you do not own is the
*caller's* LR slot, and the trailing `li r0, 0` is not optional either (section
1, item 6, second hazard).

That whole ceremony is avoidable here. The bodies are JIT-emitted leaves: they
make no host call, so nothing forces LR through r0 and nothing needs a frame.
Park it in a GPR across the `bl` instead:

    mflr TMP4          ; save (TMP4 = r6)
    bl   Body          ; body ends in blr
    mtlr TMP4          ; restore

Two instructions, no frame, no `stdu`, and because r0 is never touched the
`r0 == 0` invariant is never at risk and needs no re-zero. `mflr`/`mtlr` move
the LR SPR and do not disturb the hardware link stack, so the `bl`/`blr` pair
stays balanced for the predictor.

TMP3 and TMP4 are free for this: the body ABI table above clobbers at most
TMP1 and TMP2, and since we write the bodies that is enforced by construction
rather than assumed. Keep it that way -- **a body that grows a TMP4 use silently
destroys the return address.** If one ever needs more GPRs than TMP1/TMP2,
switch that body to the `VectorOps` frame ceremony in full, including the
`li r0, 0`, rather than reaching for TMP3.

**Nothing to save on entry besides LR.** Unlike the atomics, the site does not
need to preserve any CR or XER field for a downstream reader, because it never
writes a field a downstream reader owns. LR is the one exception and the stub
handles it as above.

## 6. Cold body specification

Each body is the host transcription of the IR chain it replaces, lane-parallel,
so the scalar and NEON forms share it (the scalar op is lane 0 plus ignored
lanes). Constants (quiet bit `0x0008000000000000` / `0x00400000`, `+Inf`,
default NaN `0x7FF8000000000000` / `0x7FC00000`) are materialised with
`LoadConstant TMPn; mtvsrd VTMPn; xxpermdi splat` (sp: the 32-bit pattern is
replicated into the 64-bit immediate first). Quiet-bit tests are done as
`xxland t, X, Q` then `xvcmpeq{dp,sp} m, t, Q` (a denormal bit pattern compared
with itself; VSX has no DAZ, FP §6.3 denormals) so no VMX-form op is needed on
the low bank; the sticky VXSNAN this raises in FPSCR is unobservable (FPSR not
emulated).

`NaNFix` = `PropagateNaNOperand` (`TranslateFP.cpp:135-145`) plus the
`Unordered`/`NaNResult` pair of `FPMinMax` (`:164-166`):
```
  OrdA  = xvcmpeq(A,A)              OrdB = xvcmpeq(B,B)
  QclrA = xvcmpeq(xxland(A,Q), Q)==0 ... (quiet bit clear per lane)   QclrB likewise
  QuietA = ~OrdA & ~QclrA           SigB = ~OrdB & QclrB
  P0 = xxsel(A, B, QuietA & SigB)   ; A'
  P2 = xxlnand(OrdA, OrdB)          ; unordered
  P3 = xvadd(P0, B)                 ; quiet(A') where A' is NaN, else quiet(B)
```
The re-run of the site's own op on (A', B) is what makes the swap sufficient for
sub and div too: a NaN operand makes the result a NaN whatever the order, only
the choice of NaN changes (FP §3.4 [MEASURED: fsub/fdiv PASS]).

`NMPrep` = the `IsNumber` prologue of `FPMinMax` (`:149-162`): `QuietNaNA`,
`QuietNaNB` as above, `ReplaceA = QuietNaNA & ~QuietNaNB`, `ReplaceB =
QuietNaNB & ~QuietNaNA`, `P0 = xxsel(A, P4, ReplaceA)`, `P1 = xxsel(B, P4, ReplaceB)`.
After it, the numeric lanes are ordered and `xvmaxdp` is exact for them
(FP §6.1: `xsmaxdp` is FMAXNM-exact except the qNaN/sNaN shape, which `NaNFix`
then handles).

`FMAFix` = lines 422-450 of `FPMulAddLanes`: `NaN{A,N,M}`, `QBit{A,N,M}`, the
five `xxsel`s in the same order (sNaN M, sNaN N, sNaN A override the qNaN
chain), `xxlor` with Q, `AnyNaN` select against P3, then `InfTimesZero =
(|N|==Inf & M==0) | (N==0 & |M|==Inf)` with `xvabs`, and `DefaultCase = NaNA &
QBitA & InfTimesZero` selecting the default NaN. About 30 instructions plus four
constants; the ordering is the IR's, which the Pi goldens verify.

Every body ends in `blr`; none contains a conditional branch, so no CR field is
read or written inside.

## 7. Coverage of F1, F2, F3, F6, N5, and the independence of F4, F5, N1

| row | covered by | what it still needs on top |
|---|---|---|
| F1 | `A64FArith` | Frontend: FADD/FSUB/FMUL/FDIV/FNMUL (`_VFNeg` after) in `FPTwoRegister`, FABD (`_VFAbs` after), `FPBinaryLanes` Add/Sub/Mul/Div and the by-element FMUL (`TranslateSIMDFloat.cpp:120`). Half precision via `HalfToDouble` unchanged. F4's zero-upper stays a separate `VMov` change. |
| F2 | `A64FMinMax` | Frontend: `FPTwoRegister` Min/Max/MinNum/MaxNum, `FPBinaryLanes` (which also serves the pairwise and scalar-pairwise forms, `:158-187`). Delete `FPMinMax`, then `PropagateNaNOperand`. |
| F3 | `A64FMulAdd` | Frontend: `FPMulAddLanes` becomes the one op (negations stay in `FPThreeRegister`; FMLA/FMLS vector and by-element already pass the accumulator as the addend). The vector `VFMLA` lowering's alias handling (`VectorOps.cpp:3805-3810`) is the template for the copy rules. B1's `xvnm*` prohibition holds. |
| F6 | infrastructure only | `A64FloatToGPR` already has its in-op cold branch for NaN (`A64FPOps.cpp:127-130`). What F6 wants from this design is a place to put the FCVTNS slow path (`mffs; mtfsb0 x2; xsrdpic; mtfsf`, today inline at `:96-101`) out of line: a `ColdStubs` entry with no shared body, entered from the shadow-FPCR test. Independent of F6: the `mtvsrwa/mtvsrwz` (ISA 2.07, not in the emitter) W-source forms, the `fctid` fast path (needs the value in an FPR-half register, e.g. `xxlor vs0` then `fctid f0`), and a readable FPCR shadow in CpuState (the frontend syncs from an FPCR value, `IRBuilder.h:401-402`; where the backend can read it is for F6 to determine). No new IR op. |
| N5 | all three ops | The lane forms are the same ops with all lanes live; the bodies are lane-parallel by construction. Not covered: FRECPS/FRSQRTS (NEON §3.12: their own NaN rule plus `Inf*0 -> 2.0`), which keep their IR chains for now, and the vector FCVTZS lane form, which stays branch-free (3 instructions, NEON §3.12). |

Independence checked against the code:
- **F4** is `DEF_OP(VMov)` (`VectorOps.cpp:62-100`, `vspltisw` + two `vsldoi`)
  becoming `xxpermdi Dst, VZERO_VSX, Src, 0b01` for i64; XA=VZERO with DM high bit
  0 reads only dw0, satisfying the dw1 hazard rule (`PPC64Emitter.h:89-92`). The
  i32 case needs a second permute or the existing shifts. No relation to the cold
  path. Confirmed independent and small.
- **F5** is consumer-side: `FCmp` writes CR0 and lifts SO/!LT into XER
  (`ALUOps.cpp:4230-4243`), consumers read through `MapNZCVCC`/`ProjectXERToCR1`.
  Making them read CR0 bits directly (FP §7.1 table) and fusing FCSEL into
  `xvcmp*`+`xxsel` (FP §7.3) never touches the arithmetic ops. Confirmed
  independent. `setb` (3.0) is not in the emitter yet.
- **N1** is frontend recognition: the backend already lowers
  `CondJump{VCmpElementSize}` to `vcmpequ*.`+`bc` on CR6 (`BranchOps.cpp:1183-1200`),
  `SupportsVCmpFlagBranch` is set (`HostFeatures.cpp:760`), and no file under
  `A64Frontend/` references it. Confirmed independent; it also shares the CR6
  convention this design uses, so the two cannot interfere (both consume CR6 in
  the next instruction).

So the "cheapest next three" claim stands: F4, F5, N1 need none of sections 2-6.

## 8. Emitter and backend additions (all ISA 2.06/2.07)

- `Emitter.h`: `xvcmpeqdp_`/`xvcmpeqsp_` (XX3 with the Rc bit; verify the
  encoding against `llvm-mc` the way `xxlorc`/`xxlnand` were, `:986-990`);
  `VSXR` overloads for `xxland`, `xxlandc`, `xxlnor`, `xxlnand`, `xvcmpeq{dp,sp}`,
  `xvadd{dp,sp}`, `xvmax{dp,sp}`, `xvmin{dp,sp}`, `xvabs{dp,sp}` (one line each via
  `EmitXX3VSX`/`EmitXX2VSX`, `:855,940`). `mtvsrd` stays VR-form (materialise into
  VTMP1, `xxlor` to the low bank).
- `JITClass.h`: `ColdStubs` list, `EmitColdStubs()`, per-body labels and `Used`
  flags; `JIT.cpp`: flush at the island point (`:5791`) and at the unit tail before
  the unbound-short check (`:5918`); the `PPC64_OPSIZE_RECORD` accounting already
  charges a stub's bytes to the op that flushes it, which is acceptable
  (`:5781-5790` explains why bytes are charged to the emitting op).
- `A64FPOps.cpp`: three `DEF_OP`s and three body emitters; register the ops in
  the handler table as `A64FloatToGPR` is.
- `HostFeatures`: `SupportsA64FPFused`.
- Knobs: `POWERARM_FPFUSED=0` (frontend A/B, old chains), `POWERARM_FPCOLD=0`
  (diagnostic: emit the check but never branch, so the qNaN/sNaN rows of the
  goldens must FAIL, FP §12.1), optionally a per-thread cold-entry counter in the
  stub (`ld/addi/std` on a CpuStateFrame slot) for risk 1.

## 9. Staging and gates

Each stage is its own commit (checklist rule 3), gated on the differential
corpus in all three modes (64K host, `4k-kvm`, `POWERARM_HOSTFEATURES=disableisa30`;
`unittests/A64Frontend/README.md:47-52`), and additionally under
`POWERARM_MAXINST=1` because one-instruction units put every stub at a unit tail
of its own. `run.sh` compares stdout byte for byte and the exit status.

**Stage 1, the first landing: `A64FArith` + infrastructure (F1).** Independently
valuable (the "4 adds: 45 to 13 instructions" row) and independently testable:
- `fp_scalar` (gen_simd.py:475-535; kind 0 covers fadd/fsub/fmul/fdiv/fnmul in
  all four rounding modes; 25% of operand pairs come from `NAN_PAIRS64/32`,
  `:56-70`, which include the discriminating (qNaN, sNaN) pair
  `(0x7FF8000000000003, 0x7FF0000000000002)` and its reverse);
- `simd_float` (`:1074-1150`, `fvec_pair` lines NaN pairs up lane by lane,
  `:1046-1063`; kinds 0-1 and 11 are the lane arithmetic, kind 2 the by-element
  FMUL);
- `fp_half` (half through the double path, signalling NaNs kept);
- `fpbugs` (the 134-line discriminating test, `fpbugs.S`) unchanged;
- `fp_fpcr`, `fpmath`, `printf_float` and the `musl_*` builds for real code;
- positive control: `POWERARM_FPCOLD=0` must fail `fp_scalar` and `simd_float`.
Result column: instruction count per guest FADD from the emitted code (the
research's §2.3 method), IR ops per mnemonic (§2.2, generated), and the
A64Bench/`gcc -O2 lvm.c` slices as for every row. PropagateNaNOperand stays
until stage 2 (FPMinMax still calls it).

**Stage 2: `A64FMinMax` (F2).** Adds `NMPrep`. Gates: `fp_scalar` kind 1,
`simd_float` kinds 0-1 (fmin/fmax/fminnm/fmaxnm) and kind 4 (the pairwise
forms, vector and scalar), `fp_half`. Deletes `FPMinMax` and `PropagateNaNOperand`.

**Stage 3: `A64FMulAdd` (F3, and the FMLA half of N5).** Adds `FMAFix`. Gates:
`fp_scalar` kind 3 (fmadd/fmsub/fnmadd/fnmsub over NaN pairs and rounding
modes), `simd_float` kinds 2-3 (FMLA/FMLS vector and by-element), `fp_half`,
and `fpbugs.S:94-100`, whose B1 block includes the `n=0, m=+Inf, a=qNaN` row
(line 100) that exercises the default-NaN override. Check that the random
corpus also reaches an sNaN multiplicand against a qNaN addend (FP §6.2's
"POWER prefers a quiet addend over a signalling multiplicand" shape); if the
seeded generator does not, add the row to `fpbugs.S`, which exists for exactly
this.

**Stage 4 (with F6 proper):** move the FCVTNS slow path onto `ColdStubs`.

**Stage 5 (only if measured):** hoist the bodies into the dispatcher (option c)
if per-unit body bytes show up in translation cost or code-buffer occupancy.

## 10. Risks and what needs a measurement

1. **Post-check vs pre-check for the binary ops.** The research measured the
   dp post-check with `xscmpudp` (needs dw0 positioning) and the `xvcmpeqdp.`
   form only as a pre-check; the `xvcmpeqsp.` post-check is the measured f32
   winner (6.51). Whether the dp `xvcmpeqdp.` post-check matches is a
   microbenchmark away. The post-check also enters the cold path whenever a
   garbage lane of the *result* is NaN, which for scalar ops means a source
   whose upper lane was last written by NEON code with a NaN there. How often
   that happens in real code is unknown; the cold-entry counter answers it. If
   it matters, the fix is local to the lowering (pre-check, or compare the
   zero-extended result), not to the IR or the stubs.
2. **Destination aliasing frequency.** Every alias costs one `xxlor` on the hot
   path (three instead of two check-side instructions). The RA's SRA preference
   makes `fadd d0, d0, d1` shapes alias by design; how much of FP code that is
   has not been counted. An IR dump census of the fused ops' allocations settles
   it. If it is most sites, the pre-check (four instructions, no stash) is the
   alternative for `A64FArith`; the FMA copy is inherent either way.
3. ~~**`bl`/`blr` inside a block.**~~ **RESOLVED 2026-09-17, and resolved
   against this document's original claim.** The audit named here was done: all
   eight mid-op LR clobbers in the backend save and restore, the rule is stated
   in tree at `BranchOps.cpp:23` and `:1035`, and the "LR is dead between ops"
   reasoning was simply wrong -- see section 1 item 6 and the LR note in
   section 5, both rewritten. The `bl`/`blr` shape survives with a two
   instruction `mflr TMP4` / `mtlr TMP4` park, cold path only; the per-site
   inline-body fallback is not needed. The link stack still sees a balanced
   push/pop, so prediction is unaffected; not measured. The lesson worth
   keeping: two of this document's original register-discipline claims (LR here,
   CR6 in section 5) asserted a survey that had not been run, and one of them
   quoted a comment that does not exist. Treat the remaining file:line citations
   as needing the same check before anything is built on them.
4. **Per-unit code growth.** Up to six bodies (about 15-35 instructions each)
   plus 5-10 per site, only in units with FP arithmetic, never fetched on
   ordered data. Whether this is visible in the code-buffer occupancy or in the
   `gcc -c empty.c` cold-start number is a measurement; option (c) is the answer
   if it is.
5. **Record-form compare dispatch.** `xvcmpeqdp.` is a V-dispatch op (both
   slices, FP §3.2 note on `xvcmpeqdp`/`xxsel`); its throughput cost in a
   4-chain stream was measured for the f32 form only (one issue slot per op).
6. **The FPSCR sticky bits raised by the cold bodies' compares** (VXSNAN on
   sNaN operands) are unobservable today because FPSR flags are not emulated;
   if N11/FPSR emulation ever reads FPSCR, the bodies must be revisited (the
   fast path raises exactly what the guest op would).
7. **Could not determine from the code:** whether any IR pass reorders or
   duplicates ops in a way that would put a fused op's `bc` and its stub on
   different sides of an island flush (the island flushes are between IR ops and
   the stub list is flushed in emission order, so this should be impossible, but
   it is an invariant to assert: a stub's site offset must precede its emission
   offset by less than 32 KB, mirroring `BindShortCondBranches`'s check).

## 11. ISA level

Everything above is ISA 2.06 (VSX arithmetic, `xxsel`, `xvcmpeq*.` record
forms) or 2.07 (`mtvsrd`). No `SupportsISA30` gate is needed for the mechanism,
and the `disableisa30` test mode runs the identical code. The P9-only forms the
research mentions (`xststdcdp`, `setb`, `mffscrn`) are either useless here
(`xststdcdp` cannot separate sNaN from qNaN, FP §3.4) or belong to F5/F6.
