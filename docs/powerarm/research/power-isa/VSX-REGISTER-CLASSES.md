# VSX register classes: letting guest V16-V31 be operands, not copies

Written 2026-09-30 against the tree at `83e554aa9`, which is also the promoted stable
emulator. Every number says where it came from: (1) source reading of
`CodeEmitter/PPC64LE/{Emitter,Registers}.h`, `ArchHelpers/PPC64Emitter.{h,cpp}`,
`JIT/PPC64LE/{JITClass.h,VectorOps,A64FPOps,ALUOps,MemoryOps}.cpp`,
`IR/Passes/RegisterAllocationPass.cpp`, `A64Frontend/IRBuilder.cpp` and `IR.json`
[CODE]; (2) an exhaustive decode of the VMX/VSX opcode space with the host binutils 2.46
disassembler in its `power8`, `power9` and `power10` dialects, and with LLVM 22's
`llvm-mc` as a second reader, scripts and the per-level mnemonic lists in `vsx-probes/`
[SPEC]; (3) a guest program with one block per register shape, run under the stable
emulator with `FEX_CODEHASHLOG` so the emitted byte count of each block is read straight
out of the backend, on both ISA levels (`vsx-probes/gen_shapes.py`, `shapes-counts.txt`)
[MEASURED]; (4) the optimised arm64 code V8 emits for a double-precision kernel, from the
rootfs `node` v26 under the emulator with `--print-opt-code` (`vsx-probes/v8_fp_kernel.js`,
`v8-fp-register-mentions.txt`) [MEASURED]; (5) the cycle figures already on disk in
`NEON-LOWERINGS.md` §2(a) and `SCALAR-FP-LOWERING.md` §8, not re-measured. No build. No
source file changed.

**One-paragraph answer.** The hypothesis in the brief holds and is now verified rather than
believed: across every ISA level from 2.07 to 3.1 the VSX half of the file has *no* integer
compare, pack, unpack, saturating, shift, min/max, multiply or sum instruction -- the only
non-float VSX operations are the logicals, select, the word/doubleword permutes and
byte-reverse, and (ISA 3.0) `xxperm`, `xxspltib`, `xxextractuw`/`xxinsertw`. So "move the
dynamic pool to vs0-vs31 with VSX-form lowerings" dead-ends on 67 of the 116 vector IR ops
the A64 frontend emits, and in any case the low bank has three free registers, not fourteen.
What the measurements say is that the cost N13 left behind is not one `xxlor` but three to
five host instructions per guest instruction (`fadd d16,d17,d18` emits 16 against 12 for
`fadd d0,d1,d2`, 8 of them on the hot path against 4; `add v16.4s,...` emits 4 against 1),
ISA-independent, and on a loop-carried accumulator it puts 4 cycles of moves on a 6-cycle
add. The design that removes it is not a new pool and not a new register class in the
allocator's sense: it is to let the register allocator's existing static-register
coalescing apply to V16-V31 exactly as it does to V0-V15, gated on a per-value bit "every
reader and the writer of this value are VSX-form lowerings". That bit is computed in one
linear pre-pass, consulted in two lines of `DecodeSRANode`, and costs nothing when the unit
has no low-bank traffic. For the scalar FP path -- every op the A64 frontend emits for
`fadd`/`fmul`/`fmadd`/`fcvt`/`fcmp`/`fcsel`/`scvtf`/`fcvtzs`/`fmov`/`ldr d`/`str d` is
already VSX-form or one move away from it -- that makes `fadd d16,d17,d18` cost what
`fadd d0,d1,d2` costs today, with no change to the dynamic pool and no new pressure. What
keeps the map honest is not a rule: VSX-form emitter methods take `VSXR` and `VR` converts
to `VSXR` but never back, VSX-clean lowerings are written against a view of the JIT core
that does not contain `GetVReg` or any VMX-form method, the IR.json flag the allocator
reads is tied to that view by `static_assert`, and the one runtime check that stands behind
all of it is an unconditional die rather than the `LOGMAN_THROW_A_FMT` that guards
`GetVReg` today -- which this document found is compiled out of every Release build. The
item is worth doing in that shape; the part the brief called risky (a VSX dynamic pool) is
not worth doing at all, and §10 says why.

## 0. The ranked conclusions

| # | Finding | Source | What it decides |
|---|---|---|---|
| 1 | VSX has no integer compare/pack/unpack/saturate/shift/min/max/multiply on any level up to 3.1; the integer NEON family is VMX forever | §3 [SPEC] | no wholesale VSX move; the design must be per-value, by consumer |
| 2 | A scalar FP op on V16-V31 emits +4 host instructions (+5 for FMA), the hot path doubles from 4 to 8; a 1-instruction NEON op on V16-V31 emits 4 | §5 [MEASURED] | the cost is moves plus the alias stash they provoke, not instruction selection; ISA-independent |
| 3 | The low bank has exactly three unclaimed registers (vs11, vs13, vs15) | §2 [CODE] | a dynamic VSX pool has nowhere to live without evicting the cold-path scratch |
| 4 | 17 of the 116 A64-reachable vector ops are VSX-form already, 32 more are one twin-swap away in whole or in their 32/64-bit arms, and together those 49 are the entire scalar-FP path plus the float lanes, logicals, select and the 32/64-bit permutes | §4 [CODE] | the coalescing design covers the FP deficit; the integer family stays as it is |
| 5 | The `GetVReg` low-bank guard is `LOGMAN_THROW_A_FMT`, compiled out with `ENABLE_ASSERTIONS=OFF` (the Release default and the build on this box); a low-bank register reaching it today would index a 16-entry span out of bounds, silently | §7.3 [CODE] | fix regardless of this item; and no design may lean on that assert |
| 6 | V8's TurboFan reaches d16-d19 in a 12-live-double kernel: 18% of FP operand mentions in its hot loop are low-bank | §5.3 [MEASURED] | the Octane FP deficit plausibly contains this cost; its share is not measured (§12) |
| 7 | `lxsd`/`stxsd`/`lxssp`/`stxssp` (ISA 3.0 DS-form) reach only v0-v31 despite being VSX scalar mnemonics; `lvx`/`stvx` (the POWER8 aligned tier) and every `v*` splat are VMX | §3.3 [SPEC][CODE] | reach traps a VSX-clean lowering must not step on; the typed emitter makes them uncompilable |

## 1. The register file as the backend holds it today [CODE]

The 64-entry VSX file, by owner (`ArchHelpers/PPC64Emitter.h`, `CodeEmitter/PPC64LE/Registers.h`,
`JIT/PPC64LE/A64FPOps.cpp`, `VectorOps.cpp`):

| VSRs | Owner | Reachable by |
|---|---|---|
| vs0 (f0) | FPSCR scratch (`mffs`, `mffscrn*`), `SpillForABICall` stash | FPR-form, VSX-form |
| vs1, vs2 (f1, f2) | FABI argument/result in the softfloat bridge, scalar compare paths | FPR-form, VSX-form |
| vs3-vs10 | `P0..P7`, the A64 FP cold-stub scratch (COLD-BLOCK-DESIGN §5); vs2-vs9 doubles as SHA round scratch | VSX-form only |
| vs11, vs13, vs15 | **unclaimed** (vs15 was the AES-mask trap, `PPC64Emitter.h` HAZARD) | VSX-form only |
| vs12 | `VTMP3_VSX` | VSX-form only |
| vs14 | `VZERO_VSX`, dw0-only reads | VSX-form only |
| vs16-vs31 | guest V16-V31, pinned by N13 (`NumStaticVectorRegs = 32`) | VSX-form only |
| vs32-vs47 (v0-v15) | guest V0-V15, `SRAFPR` | VMX-form and VSX-form |
| vs48-vs61 (v16-v29) | `RAFPR`, the 14-register dynamic pool; v16-v19 ELFv2-volatile | VMX-form and VSX-form |
| vs62, vs63 (v30, v31) | `VTMP1`, `VTMP2` | VMX-form and VSX-form |

Two consequences the rest of the document leans on. First, the "move the pool to the low
bank" option in N13's own TODO has no room: eleven of the sixteen low-bank registers below
vs16 are spoken for, and the eight cold-body registers are there precisely because the
cold path needs six to nine live vectors while only two VMX temporaries exist. Second, the
ELFv2 rule recorded at `VZERO_VSX`'s declaration -- a host callee preserves only dw0 of
vs14-vs31 and leaves dw1 undefined -- means no 128-bit JIT value in the low bank survives a
host call; the pinned V16-V31 already pay for that through `SpillStaticRegs`, and a dynamic
pool there would need the same treatment on every crossing, whereas v20-v29 of the VMX pool
survive a call for free.

The allocator today (`RegisterAllocationPass.cpp:240-253`, `JITClass.h:911-920`): a guest
read of V0-V15 is a `LoadRegister` whose node the RA coalesces onto the static register
(`DecodeSRANode` returns the node, `PreferredReg` records the slot, `IsTrivial` deletes the
op post-RA), so the consumer reads v0-v15 directly. For V16-V31 `DecodeSRANode` returns
`nullptr`, the load is a real op, `DEF_OP(LoadRegister)` emits `xxlor` into a dynamic VMX
register, and the consumer reads that. The same in reverse for `StoreRegister`. `GetVReg`
refuses an `FPRFixed` register numbered 16 or above, so no lowering can ever see a low-bank
operand; `GetVSXReg` maps the whole file and is used by exactly the two register-move ops.

## 2. Method for the constraint map

The map has two layers, and they were built separately so that neither depends on reading
the other correctly.

**Host instruction layer, from the ISA.** `vsx-probes/enumerate_isa.py` writes every
encoding of primary opcode 4 (VMX: VX, VC and VA forms, all 2^11 extended-opcode values),
opcode 60 (VSX: XX1/XX2/XX3/XX4, all 2^11 low bits for all 32 values of the RA field, since
XX2 sub-opcodes live there), opcode 31 (X-form loads/stores and the `mtvsr*`/`mfvsr*`
moves, all 2^11 values) and opcodes 57/61 (DS/DQ-form VSX loads and stores). The host
`objdump -M power8|power9|power10` decodes the lot; the mnemonic sets are
`binutils-power{8,9,10}.txt` (571, 689 and 789 mnemonics). binutils gates by dialect and
LLVM's disassembler does not (it returned the same 852-mnemonic superset for every `-mcpu`,
including an ISA 3.2 `xvadduwm`/`xvmulhsw`/`xvrlw` family that exists on no machine this
project targets), so binutils is the authority for "which level" and LLVM is the check that
binutils missed nothing in the opcode-60 space -- it did not.

**IR op layer, from the lowerings.** `vsx-probes/defop_mnemonics.awk` lists, for every
`DEF_OP` body in the backend, the emitter mnemonics it calls, against the full method-name
list of `Emitter.h` (636 names); the result is `defop_mnemonics.txt`, 344 ops. It is a
lower bound per op: a mnemonic emitted through a helper (`EmitRoundNearestEven`,
`LoadFPRSized`, the FMA macros) is attributed to the helper, not the op, so every op in the
FP family was then read by hand (§4). The 116 ops the A64 frontend can emit are the
`_Name(` calls in `A64Frontend/*.cpp`, intersected with the ops of `IR.json` that produce or
consume an FPR (destination-only ops such as `VectorImm` and `VCastFromGPR` included).

## 3. The constraint map, host side: which half can each instruction name [SPEC]

### 3.1 Reach, by instruction form

| Form | Register field | Reach | Instructions |
|---|---|---|---|
| VX / VC / VA (opcode 4) | VRT/VRA/VRB/VRC, 5 bits | **v0-v31 only** | every `v*` mnemonic: integer arithmetic, compares, saturating ops, pack/unpack, merge, splat, shifts/rotates, `vperm`/`vpermr`/`vsel`/`vsldoi`, `vcipher*`, `vpmsum*`, `vshasigma*`, the VMX float set (`vaddfp`, `vmaddfp`, `vcmp*fp`, `vrfi*`, `vrefp`, `vrsqrtefp`, `vctsxs`), `vextu*rx`, `vinsert*`, `vctzlsbb`, `mfvscr`/`mtvscr` |
| VMX loads/stores (opcode 31) | VRT, 5 bits | **v0-v31 only** | `lvx`, `lvxl`, `stvx`, `stvxl`, `lvebx/lvehx/lvewx`, `stvebx/stvehx/stvewx`, `lvsl`, `lvsr` |
| DS-form VSX scalar (opcode 57/61, ISA 3.0) | VRT, 5 bits, **no TX** | **v0-v31 only** | `lxsd`, `lxssp`, `stxsd`, `stxssp` |
| XX1 (opcode 31 with TX) | T + TX, 6 bits | all 64 | `lxvx`, `stxvx`, `lxvd2x`, `stxvd2x`, `lxvw4x`, `stxvw4x`, `lxvdsx`, `lxvwsx` (3.0), `lxvb16x`/`lxvh8x` (3.0), `lxvl`/`lxvll` (3.0), `lxsdx`, `lxsiwzx`, `lxsiwax`, `lxsspx`, `lxsibzx`/`lxsihzx` (3.0), `stxsdx`, `stxsiwx`, `stxsspx`, `stxsibx`/`stxsihx` (3.0), `mtvsrd`, `mtvsrwa`, `mtvsrwz`, `mfvsrd`, `mfvsrwz`, `mtvsrdd`/`mtvsrws`/`mfvsrld` (3.0) |
| DQ-form (opcode 61, ISA 3.0) | T + TX | all 64 | `lxv`, `stxv` |
| XX2 (opcode 60) | T+TX, B+BX | all 64 | every unary `xs*`/`xv*`: converts, roundings, sqrt, abs/neg, `xxspltw`, `xxbrh/w/d/q` (3.0), `xxextractuw`/`xxinsertw` (3.0), `xxspltib` (3.0, XX1-form), `xvcvhpsp`/`xvcvsphp` (3.0), `xststdc*`/`xvtstdc*` (3.0), `xsxexpdp`/`xsxsigdp`/`xvxexp*`/`xvxsig*` (3.0) |
| XX3 (opcode 60) | T/A/B + TX/AX/BX | all 64 | every binary `xs*`/`xv*` float op and FMA, `xvcmp{eq,gt,ge}{sp,dp}[.]`, `xvmax/min`, `xvcpsgn`, `xxland/xxlandc/xxlor/xxlxor/xxlnor/xxlorc/xxlnand/xxleqv`, `xxpermdi`, `xxsldwi`, `xxmrghw/xxmrglw`, `xxperm`/`xxpermr` (3.0), `xsiexpdp`/`xviexp*` (3.0), `xscmpeq/gt/ge dp` and `xsmaxc/minc/maxj/minj dp` (3.0) |
| XX3 with BF | A/B + AX/BX, CR target | all 64 | `xscmpudp`, `xscmpodp`, `xscmpexpdp` (3.0) |
| XX4 (opcode 60) | T/A/B/C + 4 extension bits | all 64 | `xxsel` |
| FP (opcodes 59/63, `lfd`/`stfd`) | FRT, 5 bits | **f0-f31 only, dw0 only** | `fadd`..`fnmsub`, `fcmpu`, `fctid*`, `fcfid*`, `frin/z/p/m`, `mffs`, `mtfsf` |

### 3.2 What the VSX half can and cannot do, per level

The complete non-float VSX repertoire, read off `binutils-power9.txt` (3.0) and
`binutils-power8.txt` (2.07):

- **Logical** (2.06/2.07): `xxland`, `xxlandc`, `xxlor`, `xxlxor`, `xxlnor`, `xxlorc`, `xxlnand`, `xxleqv`. Twins of `vand`/`vandc`/`vor`/`vxor`/`vnor`/`vorc`/`vnand`/`veqv`, bit-identical semantics.
- **Select** (2.06): `xxsel`. Twin of `vsel`.
- **Permute, word and doubleword granularity**: `xxpermdi` (2.06; covers `xxmrghd`/`xxmrgld`/`xxspltd`/`xxswapd`), `xxsldwi` (2.06; word shifts, twin of `vsldoi` for multiples of 4 only), `xxmrghw`/`xxmrglw` (2.06; twins of `vmrghw`/`vmrglw`), `xxspltw` (2.06; twin of `vspltw`), `xxperm`/`xxpermr` (3.0; twins of `vperm`/`vpermr`), `xxextractuw`/`xxinsertw` (3.0; word twins of `vextuwrx`/`vinsertw`), `xxbrh/w/d/q` (3.0).
- **Immediates**: `xxspltib` (3.0; any byte, twin of `vspltisb` with a wider range). No halfword/word immediate; `mtvsrws` (3.0) splats a GPR word, `mtvsrdd` (3.0) builds a doubleword pair.
- **Everything else is float**: arithmetic, FMA, compares (mask and CR6 record forms), min/max, roundings, converts, estimates, sqrt, abs/neg/copysign, class tests, exponent/significand extraction.

What is **absent from VSX on every level, including 3.1 (`binutils-power10.txt`)**: integer
add/sub/mul (all widths), integer compares (`vcmpequ*`/`vcmpgts*`/`vcmpgtu*`, and 3.0's
`vcmpne*`), saturating add/sub, `vavg*`, `vmin*`/`vmax*` integer, pack/unpack
(`vpk*`/`vupk*`), byte and halfword merges (`vmrghb/h`, `vmrglb/h`), byte and halfword
splats, shifts and rotates (`vsl*`/`vsr*`/`vrl*`), `vsum*`, `vmsum*`, `vmul[eo]*`,
`vpopcnt*`, `vclz*`, `vabsdu*`, `vgbbd`, `vbpermq`, the crypto set, and the whole VMX
float set (`vaddfp` and friends -- irrelevant, they have VSX twins). ISA 3.1 adds
`vcmpequq`/`vcmpgtsq`/`vcmpgtuq`, integer divide/modulo and string ops, all VX-form, all
VMX-half. LLVM 22 knows an ISA 3.2 `xvadduwm`/`xvsubuwm`/`xvmuluwm`/`xvmulh*`/`xvrlw`
family in opcode 60; it is on no POWER8/9/10 and binutils does not list it for `power10`.

So the brief's belief is right, with one refinement: the dividing line is not "integer
versus float" but "lane width": at 32- and 64-bit lane granularity VSX has the permutes,
merges, splats, select, logicals and (3.0) extract/insert; it has none of them at byte or
halfword granularity, and no integer arithmetic at any width.

### 3.3 Reach traps a VSX-form lowering must know

- `lxsd`/`stxsd`/`lxssp`/`stxssp` are "VSX scalar" by name and ISA chapter, and reach v0-v31 only: the DS-form has no TX bit. The X-forms (`lxsdx`, `lxsiwzx`, `stxsdx`, `stxsiwx`, `lxsibzx`, `lxsihzx`) reach all 64. The emitter has only the X-forms today; keep it so, or give the DS-forms a `VR`-only signature.
- `lvx`/`stvx`: the POWER8 aligned-load tier (`TryEmitAlignedV128Load`), `SpillRegister`/`FillRegister`, `LoadNamedVectorConstant` (the constant pool), `VInsGPR`'s table path and the pre-3.0 size-1/2 scalar bounce in `LoadFPRSized`/`StoreFPRSized` all go through them. `lxvx`/`stxvx` (3.0) and `lxvd2x`/`stxvd2x`+`xxpermdi` (2.06) are the full-reach forms.
- The record forms `xvcmpeq{sp,dp}.` are declared `VR`-only in the emitter on purpose ("only the hot-path site wants CR6"); the A64 arithmetic sites are exactly those hot-path sites, so they need `VSXR` forms.
- `mtvsrd`/`mfvsrd`/`mtvsrwa`/`mtvsrwz`/`mfvsrwz`/`mtvsrdd`/`mtvsrws`/`mfvsrld` are XX1-form with a TX bit, i.e. full reach, but the emitter hardcodes `TX=1` in each; `xscmpudp`/`xscmpodp` add 32 to the index; `EmitXX2` hardcodes `BX|TX`. Each needs the derived-bit form `EmitXX2VSX`/`EmitXX3VSX` already use.
- Every `vspltis{b,h,w}` used as a zero or a shift count is VMX. A zero is `xxlxor t,t,t` (2.06); a byte pattern is `xxspltib` (3.0); a word pattern is `mtvsrws` (3.0) or `mtvsrd`+`xxpermdi`+`xxspltw` (2.06); a halfword pattern has no VSX form short of the constant pool.
- `vsldoi` by a multiple of 4 is `xxsldwi`; by 8 it is `xxpermdi`; by anything else it is VMX.

## 4. The constraint map, IR side: the 116 ops the A64 frontend emits [CODE]

Tiering is by what the *lowering* emits today on both ISA levels. "Twin" names the
VSX-form replacement where one exists. "VSX-clean" means every instruction that touches an
operand or the result is VSX-form; a VMX instruction on a temporary that only ever holds a
constant does not disqualify an op, because the temporary is `VTMP1`/`VTMP2`, which both
halves of the file can name.

### 4.1 Tier A: VSX-form already (17 ops: the 15 reachable rows below plus `VCastFromGPR` and `VLoadTwoGPRs`) -- the scalar-FP core

| IR op | Guest instructions | Host instructions today (3.0 / 2.07) | Note |
|---|---|---|---|
| `A64FArith` | `fadd/fsub/fmul/fdiv` scalar and lanes | `xv{add,sub,mul,div}{dp,sp}` + `xvcmpeq{dp,sp}.` + `bc`; stub `xxlor`×2 `mflr bl mtlr xv* b` on vs3-vs10 | record forms need `VSXR` twins |
| `A64FMinMax` | `fmin/fmax/fminnm/fmaxnm` | `xvcmpeq.`×2 + `bc` + `xv{max,min}` | same |
| `A64FMulAdd` | `fmadd/fmsub/fnmadd/fnmsub`, `fmla/fmls` lanes | `xxlor` + `xvmadda{dp,sp}` + `xvcmpeq.` + `bc` | same |
| `A64FloatToGPR` | `fcvtzs/fcvtns/.../fcvtau` | `xxsldwi`/`xxpermdi`, `xscvspdpn`, `xsrdpi*`, `xscvdp[su]x[dw]s`, `mfvsrd`/`mfvsrwz`, `xscmpudp`, `EmitRoundNearestEven` (`xsrdpic` + FPSCR bracket on `f0`) | `mfvsr*` need derived-TX forms |
| `A64FloatFromGPR` | `scvtf/ucvtf` from GPR | `mtvsrwa/wz/d`, `xscv[su]xd{dp,sp}`, `xscvdpspn`, `xxpermdi` against `VZERO_VSX`, `mfvsrwz`/`mtvsrd` | same |
| `A64VecIntToFloat` | `scvtf/ucvtf` vector | `xvcv[su]xw sp` / `xvcv[su]xd dp` | one instruction |
| `FCmp` | `fcmp/fcmpe` | `xxsldwi`/`xxpermdi` + `xscmpudp` + `mfocrf`/`rlwinm` | |
| `NZCVSelectV` | `fcsel` | `iselcc`/`li` + `mtvsrd`/`mtvsrdd` + `xxpermdi` + `xxsel` | |
| `VFAbs`, `VFNeg`, `VFSqrt` | `fabs/fneg/fsqrt`, lanes | `xvabs*`, `xvneg*`, `xvsqrt*` | |
| `VFAdd`, `VFSub`, `VFMul`, `VFDiv` | lanes without the NaN cold path | `xv*` | |
| `VFCopySign` | (x86 only) | `xvcpsgn*` | not A64-reachable |

Also VSX-form today but not an "op": `LoadRegister`/`StoreRegister` (`xxlor`),
`LoadMem`/`StoreMem` for sizes 4, 8 and 16 on both levels (`lxsiwzx`/`lxsdx`/`stxsiwx`/
`stxsdx` with `xxpermdi`; `lxvx`/`stxvx` or `lxvd2x`/`stxvd2x`+`xxpermdi`; `lxvdsx`) and
for sizes 1 and 2 on 3.0 (`lxsibzx`/`lxsihzx`), `VCastFromGPR` (`fmov d,x`: `mtvsrdd` /
`mtvsrd`+`xxpermdi`), `VLoadTwoGPRs`, `VExtractToGPR` 64-bit (`fmov x,d`: `mfvsrld` /
`xxpermdi`+`mfvsrd` once the P8 `vsldoi` is swapped), `VDupElement` 64-bit (`xxpermdi`),
`VMov` 64-bit (`xxpermdi` against `VZERO_VSX`), `CondJump`'s scalar arms.

### 4.2 Tier B: one twin-swap from VSX-form (32 ops, in whole or in the 32/64-bit arms named)

| IR op | VMX instruction today | VSX twin | Level |
|---|---|---|---|
| `A64FToF` (`fcvt`) | `vor` as a move | `xxlor` | 2.06 |
| `VFMLA` (and `VFMLS`/`VFNMLA`/`VFNMLS`) | `vmr` ×3 as moves | `xxlor` | 2.06 |
| `VMov` 128 | `vmr` | `xxlor` | 2.06 |
| `VMov` 32 (every `s`-register result) | `vspltisw` + `vsldoi`×2 | `xxlxor` + `xxsldwi`×2, same count | 2.06 |
| `VMov` 8/16 | `vspltisw` + `vsldoi`×2 | `li` + `mtvsrdd` + `xxland` (3.0, 3) or `mtvsrd`+`xxpermdi`+`xxland` (2.06, 4) | +1 on P8, rare |
| `VFCMPEQ`/`VFCMPLE`/`VFCMPLT`/`VFCMPUNO` f32 arms | `vcmpeqfp`/`vcmpgefp`/`vcmpgtfp` | `xvcmpeqsp`/`xvcmpgesp`/`xvcmpgtsp` | 2.06 |
| `A64VecFloatToInt` f32 round-to-nearest-even arm | `vrfin` (fixed RNE, no FPSCR read) | `xvrspic` under the FPCR bracket `EmitRoundNearestEven` already emits for f64 (9 static, 5 executed, against `vrfin`'s 1) | 2.06, +8 static / +4 executed on that arm |
| `Vector_FToI` f32 arms (`frint*`) | `vrfi{n,z,p,m}` | `xvrspi{c,z,p,m}` (RNE as above) | 2.06 |
| `Vector_FToF` f32<->f64 arms (`fcvtl`/`fcvtn`) | `vmrglw`, `vmrghw` | `xxmrglw`, `xxmrghw` | 2.06 |
| `VAnd`/`VOr`/`VXor`/`VAndn`/`VOrn`/`VNot` | `vand`/`vor`/`vxor`/`vandc`/`vorc`/`vnor` | `xxland`/`xxlor`/`xxlxor`/`xxlandc`/`xxlorc`/`xxlnor` | 2.06/2.07 |
| `VBSL` | `vsel` | `xxsel` | 2.06 |
| `VZip`/`VZip2`/`VTrn`/`VTrn2`/`VUnZip`/`VUnZip2` 32/64-bit arms | `vmrg{h,l}w`, `vmrgew`/`vmrgow`, `vperm`+pool control, `vsldoi` | `xxmrg{h,l}w`, `xxpermdi` (64-bit), `xxperm` (3.0) with an `lxv` control; `vmrgew`/`vmrgow` have no twin below 3.0's `xxperm` | 2.06 / 3.0 |
| `VExtr` by 4/8/12 | `vsldoi` | `xxsldwi` / `xxpermdi` | 2.06 |
| `VDupElement` 32-bit | VMX splat | `xxspltw` | 2.06 |
| `VDupFromGPR` 32/64-bit P8 arms | `vspltw` | `xxspltw` | 2.06 |
| `VExtractToGPR` 32-bit, 64-bit element 0 on P8 | `vextuwrx` (3.0) / `vspltw`; `vsldoi` | `xxextractuw` (3.0) / `xxspltw`+`mfvsrwz`; `xxpermdi` | 3.0 / 2.06 |
| `VInsElement`/`VInsGPR` 32/64-bit | `vinsertw`/`vinsertd` (3.0), `vperm` (P8) | `xxinsertw` (3.0), `xxpermdi` (64-bit, 2.06); 32-bit on P8 needs a word mask from the pool | 3.0 / partial |
| `VectorImm` | `vspltis{b,h,w}` | zero: `xxlxor`; byte: `xxspltib` (3.0); word: `mtvsrws` (3.0) or `mtvsrd`+`xxpermdi`+`xxspltw`; halfword: none | partial |
| `VRev32`/`VRev64` | `vrl*` rotates on P8 | `xxbr*` already on 3.0; no P8 twin | 3.0 only |
| `LoadNamedVectorConstant` (pool loads) | `lvx` | `lxvx` / `lxvd2x`+`xxpermdi` | 2.06 |
| `LoadMem`/`StoreMem` size 1/2 on P8; size 16 aligned tier on P8 | `lvx`/`stvx` bounce | keep the bounce but through `stxvx`-equivalents, or leave these arms VMX | P8 only |

Helpers that carry a `VR` through and need a `VSXR` form for the ops above:
`PositionElement0AsDouble`, `PlaceElement0`, `PlaceSingleFromDoubleword0`,
`EmitRoundNearestEven`, `EmitXX2WithRA`, `EmitHalfToDouble`/`EmitDoubleToHalf`,
`LoadFPRSized`/`StoreFPRSized`, `LoadUnalignedV128`/`StoreUnalignedV128`,
`EmitLoadPPC64VConst`, and the `FPColdStub` record (`Dst`/`A`/`B`/`C` are `VR`; the stub
already converts them with `AsVSX`).

### 4.3 Tier C: VMX by semantics (67 ops) -- no VSX form on any level

`VAdd`, `VSub`, `VMul`, `VNeg`, `VAbs`, `VCMPEQ`/`VCMPGT`/`VCMPGTU` and their `Z` forms,
`VSMax`/`VSMin`/`VUMax`/`VUMin`, `VUQAdd`/`VUQSub`/`VSQAdd`/`VSQSub`, `VShlI`/`VUShrI`/
`VSShrI`/`VUShl`/`VUShr`/`VSShr`, `VUShrNI`, `VSXTL`/`VSXTL2`/`VUXTL`/`VUXTL2`,
`VUMull`/`VUMull2`/`VSMull`/`VSMull2`, `VUABD`/`VUABDL`/`VUABDL2`, `VURAvg`, `VPopcount`,
`VCLZ`, `VAddV`/`VUMaxV`/`VUMinV`, `VSQDMulH`/`VSQRDMulH`, `VUDot`, `VTBL1`/`VTBL2`/`VTBX1`,
`VAESE`/`VAESD`/`VAESMC`/`VAESImc`, `VSha1*`/`VSha256*`, `PCLMUL`/`VPMullB`,
`VAnyNonZero`, `CondJump` in `VCmpElementSize` mode, `VExtr` by other than 4/8/12, the
byte/halfword arms of `VDupElement`/`VDupFromGPR`/`VExtractToGPR`/`VInsElement`/`VInsGPR`/
`VZip`/`VUnZip`/`VTrn`, `VFCVTL2`/`VFCVTN2` and the f16 arms of `Vector_FToF`.

For these the map is exact and final: a value they read or write must be in v0-v31 at that
instruction. Nothing in this document changes their cost on V16-V31 (4 host instructions
for a 1-instruction op, §5); §6.5 says what could, and why it is not this item.

## 5. What the split costs today, measured [MEASURED]

`vsx-probes/gen_shapes.py` emits one 2048-byte-aligned leaf function per shape, 16
repetitions each with the guest registers varied so nothing is shared, entered by `blr x9`
so the decoder never folds a function into its caller. The stable emulator ran it with
`FEX_CODEHASHLOG`, which records the emitted byte count per block; the empty function
(`ret` alone, 112 bytes) is subtracted and the rest divided by 64. Full table in
`shapes-counts.txt`; the deltas are identical on `POWERARM_HOSTFEATURES=disableisa30`, as
the mechanism predicts (moves, not selection). `fcmp` is omitted: fifteen of its sixteen
flag writes are dead and DFCE removes them, so its row measures nothing.

| Guest instruction (operands in V0-V15 / in V16-V31) | ISA 3.0 lo | hi | delta | POWER8 lo | hi |
|---|---|---|---|---|---|
| `fadd d, d, d` | 12 | 16 | +4 | 12 | 16 |
| `fadd d0, d0, d1` (accumulator) | 12 | 16 | +4 | 12 | 16 |
| `fmul d, d, d` | 12 | 16 | +4 | 12 | 16 |
| `fmadd d, d, d, d` | 17.25 | 22.25 | +5 | 17.25 | 22.25 |
| `fsqrt d, d` / `fneg d, d` | 2 | 4 | +2 | 2 | 4 |
| `fcsel d, d, d, gt` | 9 | 12 | +3 | 12 | 15 |
| `fcvt s, d` | 8 | 10 | +2 | 8 | 10 |
| `fcvtzs x, d` / `scvtf d, x` | 6 / 3 | 7 / 4 | +1 | 6 / 3 | 7 / 4 |
| `fmov d, x` / `fmov x, d` | 1 | 2 | +1 | 2 | 3 |
| `ldr d, [x, #8]` / `str d, [x, #8]` | 3 | 4 | +1 | 3 | 4 |
| `ldr q, [x, #16]` | 1 | 2 | +1 | 3 | 4 |
| `fadd v.2d` | 11 | 15 | +4 | 11 | 15 |
| `fmla v.2d` | 16.25 | 21.25 | +5 | 16.25 | 21.25 |
| `fcmgt v.2d` | 2.25 | 5.5 | +3.25 | 2.25 | 5.5 |
| `fabs v.2d` / `dup v.2d, v.d[1]` | 1 | 3 | +2 | 1 | 3 |
| `bsl` | 1 | 5 | +4 | 1 | 5 |
| `eor` / `and` / `add v.4s` / `cmeq v.4s` / `uzp1 v.4s` | 1 | 4 | +3 | 1 | 4 |
| `movi v.2d, #0` / `fmov v.2d, #1.0` | 1 | 2 | +1 | 1 / 3 | 2 / 4 |

### 5.1 Where the twelve come from, and where the extra four come from

The 12 for `fadd d0,d1,d2` is a static count and includes the cold path: 3 on the hot path
(`xvadddp`, `xvcmpeqdp.`, `bc`) + 1 for the `VMov(64)` upper-half zero (`xxpermdi` against
`VZERO_VSX`) + 7 in the per-site stub (`xxlor`×2, `mflr`, `bl`, `mtlr`, `xvadddp`, `b`) +
the shared `NaNFix_dp` body amortised over the 16 sites (~1.1). The executed hot path is 4.

The +4 for `fadd d16,d17,d18` is: `xxlor` for each `LoadRegister` (2), `xxlor` for the
`StoreRegister` (1), and one more that is a side effect rather than a move -- with both
sources now sitting in killed dynamic registers, the RA hands the result the lowest free
dynamic register, which is the one the first source just vacated, `Dst == V1` becomes true,
and `A64FArith`'s alias stash (`xxlor VTMP2, V1`) fires. The post-RA IR dump
(`POWERARM_DUMPIR=stderr`) shows it directly: the V16-V31 block reads `%8(v0) = A64FArith
v0, v1` -- result and first source in dynamic register 0 -- where the V0-V15 block reads
`%8(v0) = A64FArith V1, V2`, sources static, which the result can never share, so the stash
never fires there. The rows agree: ops with the stash (`fadd`, `fmul`, `fmadd`, `fmla`)
show one more than their load and store count; ops without it (`fsqrt`, `fcsel`, `fcvt`)
show exactly loads plus store. So the hot path goes from 4 to 8: every one of the extra
four instructions executes.

On a loop-carried accumulator the two moves sit on the dependent chain: `vs16 -> xxlor (2)
-> xvadddp (6) -> xxpermdi (3) -> xxlor (2) -> vs16` against `v0 -> xvadddp -> xxpermdi ->
v0`, i.e. 13 cycles per iteration against 9 [INFERENCE from the component latencies in
NEON §2(a) and FP §8; the chain itself was not timed]. The `xxpermdi` on both chains is the
producer-side zero-upper that ISA-OPPORTUNITIES §3.10's note identifies as the sound half of
item 12; it is independent of this item.

### 5.2 The integer side

A 1-instruction NEON op on V16-V31 becomes 4 (`add`, `and`, `cmeq`, `uzp1`), 5 for the
three-source `bsl`. Those moves are unavoidable per op for Tier C: the value has to be in
the VMX half when `vaddubm` executes. The only way to amortise them is to keep a VMX copy
of the guest register live across several guest instructions, which is a frontend register
cache for vectors with the same drain-point discipline the GPR cache has (§6.5). Tier B
ops in integer-looking code (`and`/`eor`/`bsl` are common in FP code too: sign flips,
branchless selects) do move to the VSX side under this design.

### 5.3 Who uses V16-V31

The brief's workloads are JIT engines. V8's arm64 backend allocates doubles from d0 upward
and continues into d16 and above when more are live (d0-d19 in this dump); in the kernel of `vsx-probes/v8_fp_kernel.js` (two
loop-carried velocities, two positions, an energy accumulator, a reciprocal-sqrt chain)
TurboFan's optimised code mentions d0-d15 449 times and d16-d19 100 times in its hot loop
(18%), with d16-d19 holding the unrolled iteration's temporaries. Box2D and NavierStokes
keep more doubles live than this kernel and the Octane sources are not on this machine, so
their share is unmeasured (§12); JSC's (Bun, the Claude CLI) is unmeasured for the same
reason. Compiled C/C++ uses V16-V31 for the same purpose -- `SCALAR-FP-LOWERING.md` §9
found nbody keeping its bodies in d16-d31 -- so the exposure is not confined to JITs.

## 6. The design: coalesce by consumer class, do not move the pool

### 6.1 Why not the pool

N13's TODO offered two routes: "the dynamic vector pool moved to vs0-vs31 with VSX-form
lowerings, or a VSX-aware allocator class". The first is closed by §3 (67 ops cannot follow)
and by §1 (three free registers; dw1 volatility across every host call). The second, read
as "two dynamic pools, one per half, with values classed by consumer", would be the right
shape if the low bank had registers to give a pool. It does not, so a second pool would
have to start by evicting the eight cold-body scratch registers, which would then need
their own save/restore inside the cold path, for a pool of eleven registers whose only
customers are VSX-clean temporaries -- values that today already fit in the fourteen VMX
registers with a spill volume `WARM-CODEGEN-RESEARCH` measured at 0.02% [not re-measured
for FP blocks specifically; §12]. The register count is not where the cost is. The cost is
the copy between the pinned register and the pool, and that is an allocator *decision*,
not a pool size.

### 6.2 What the allocator does

Today `DecodeSRANode` refuses to coalesce any `LoadRegister`/`StoreRegister` of V16-V31
because a coalesced value lives in a low-bank register and *some* consumer might be a VMX
form. The design makes that refusal exact instead of blanket:

1. **Pre-pass, one linear sweep per region, two bits per SSA value.** `VSXUsesOnly[v]`
   starts true and is cleared when any op that is not VSX-clean (per the IR.json table,
   §7.1) reads `v`. `VSXDef[v]` is true when `v`'s defining op is VSX-clean or is itself a
   `LoadRegister`/`FillRegister`... no: a `Fill` lands in the dynamic pool and is not a
   candidate, so `VSXDef` is simply "defining op is VSX-clean". The sweep runs only when
   the region contains at least one `LoadRegister`/`StoreRegister` of an FPR numbered 16 or
   above (one scan to find out; most units have none). Cost: two bit-vectors of SSA count
   and one pass over the ops, inside an RA that already does a backward and a forward pass.
   It is not on the compile-time budget in any measurable way [INFERENCE; §12].
2. **`DecodeSRANode` consults the bits.** For `LoadRegister(V >= 16)` it returns the node
   iff `VSXUsesOnly[node]`; for `StoreRegister(V >= 16, value)` it returns `value` iff
   `VSXDef[value] && VSXUsesOnly[value]` and `value` is an FPR-class dynamic value. V0-V15
   are unchanged. Everything downstream -- `PreferredReg`, the write-after-write guard that
   resets it when another SRA access intervenes, the evict-with-copy at an SRA access whose
   register holds an older live value, `IsTrivial` deleting the coalesced op -- is the code
   that already handles V0-V15 and needs no change, with one exception below.
3. **The evict copy.** When vs16 holds an older live value and the guest writes V16, the RA
   inserts `_VMov(Size, FPRFixed 16)` to a dynamic register. That `VMov` has a low-bank
   source, so `VMov` must be VSX-clean (Tier B, `xxlor`/`xxpermdi`/`xxsldwi`). Its result
   goes to the VMX pool and may feed anything.
4. **Consumers read the pinned register directly.** `GetVSXReg` on an `FPRFixed` 16-31
   returns vs16-vs31, as it does today for the two move ops. The dest side is symmetric:
   a VSX-clean op whose result is coalesced onto vs16 writes it in place.

After this, `fadd d16,d17,d18` is `xvadddp v_t, vs17, vs18; xvcmpeqdp.; bc; xxpermdi vs16,
VZERO, v_t` -- 4 on the hot path, the same as `fadd d0,d1,d2`, and the alias stash
disappears because the sources are fixed registers again. `fmla v16.2d, v17.2d, v18.2d` is
one `xvmaddadp vs16, vs17, vs18`, because `Add` and `Dst` coalesce onto the same register
and `VFMLA` already special-cases `Dst == Add`. `ldr d16, [x0, #8]` is `lxsdx; xxpermdi`
into vs16 with no trailing move. Static counts for every row of §5 can be predicted from
the table: the "hi" column becomes the "lo" column for Tier A and B shapes, and stays as it
is for Tier C.

**When the constrained half is full.** Nothing changes: the dynamic pool is still v16-v29,
spilling is still furthest-next-use into the frame, and `FPRFixed` values are never
spilled, as now. The design only ever *removes* demand on the pool (a coalesced load no
longer occupies a dynamic register for the copy; a coalesced store frees the result's
register at the def). A value with mixed consumers -- one VSX-clean, one VMX -- stays in the
pool, which is today's behaviour exactly.

**Allocation quality beyond that.** The remaining copy in mixed-consumer values can be
halved by coalescing anyway and inserting one shared VMX copy at the first VMX consumer,
remapping later VMX consumers to it (the `InsertFill` shape). That is never worse than
today (today copies for every consumer) and strictly better when any consumer is VSX-clean.
It is a second step (§10, Stage 2b) because it touches the forward pass rather than
`DecodeSRANode`, and because nothing yet says how common mixed consumers are (§12).

### 6.3 What this does not touch

The frontend emits the same IR. The dynamic pool, `VTMP1`/`VTMP2`, `SpillStaticRegs`/
`FillStaticRegs`, the signal-frame capture of vs16-vs31 (N13's `GetPPCVSXLowBankDW0/DW1`)
and `Push`/`PopDynamicRegs` are unchanged: a coalesced V16-V31 value is architectural guest
state in its pinned register, which is exactly what those paths already save. Hoisting the
guest write to the def point is the same hoist V0-V15 live with today, and the frontend
orders the `VMov`/`StoreRegister` pair immediately after the producing op within one guest
instruction, so no fault can fall between them.

### 6.4 The scalar `xs*` forms

The brief asks whether this work makes `xsadddp`/`fadd` reachable for scalar code. It does
not, and the reason is not the register half. `SCALAR-FP-LOWERING.md` §8 measured the
choice: the backend keeps element 0 in VSX doubleword 1, the scalar forms read doubleword 0,
and positioning costs a 3-cycle `xxpermdi` each way, more than the +0.4 cycles the `xv`
forms cost on add/mul and comparable to their 10-20% loss on divide/sqrt throughput. A
*layout* class -- scalar values kept in dw0 -- is a different axis from the half class, and
it conflicts with every full-width observer of the register (stores, exits, signal frames,
any lane consumer), each of which would need a swap. That is the per-PC-metadata problem
ISA-OPPORTUNITIES §3.10's note describes for the dirty upper lane, and it is not this item.
What the class work *does* do for scalar code is remove the two moves and the stash, and
keep the NaN pre-check honest: coalesced guest D-registers always carry a zero upper half
(`VMov(64)` writes it), so `xvcmpeqdp. t, Dst, Dst` cannot see a garbage lane.

### 6.5 The alternative considered: a frontend vector register cache

The +3 on Tier C ops would fall to roughly one copy per guest register per block if the
frontend kept V16-V31 in dynamic VMX registers across guest instructions and wrote them
back at exits, the way `GPRCache` carries X9-X18 (`IRBuilder.h:673-690`). It would also
need the same drain discipline: every instruction that may raise a signal has to see the
pinned registers current, which in FP loops is every load and store. The RA side of warm
G6 ("blocked on RA", checklist item 11) is the same blocker. It is the right shape for
integer NEON on V16-V31 one day; it is not cheaper or safer than the coalescing design for
the FP path, which needs no cache because the pinned register *is* the operand.

## 7. How the map stays honest

The failure the brief names -- a lowering added later that puts a VMX-form instruction on a
low-bank value -- has to be impossible to compile, impossible to run, and impossible to
misdeclare, in that order of preference. Four mechanisms, each with what it catches and
what it cannot.

### 7.1 The flag, generated from IR.json, is the only table

`IR.json` gains a per-op boolean `VSXClean` next to `HasSideEffects`/`TiedSource`/
`RAOverride`, which already generate per-op tables through `json_ir_generator.py`
(`IR::TiedSource(op)` is the precedent). The RA pre-pass reads `IR::VSXClean(op)`. No
second list exists anywhere.

### 7.2 The handler cannot name a VMX form: the view class

VSX-form emitter methods take `VSXR` only; `VR` gains an implicit conversion to `VSXR`
(`v_n` is `vs_{32+n}`, which is what `AsVSX` does by hand today) and `VSXR` has none to
`VR`. The encoders already derive TX/AX/BX from the index (`EmitXX3VSX`, `EmitXX2VSX`), so
existing `VR` call sites compile unchanged and emit the same bytes -- that is a gate, §10
Stage 0 -- while a `VSXR` argument to `vperm`, `vcmpequb`, `vspltisw`, `lvx` or any other
VMX form has no overload and does not compile.

VSX-clean lowerings are then declared with `DEF_OP_VSX(x)` instead of `DEF_OP(x)`. The
macro defines the handler as a member of `PPC64VSXView`, a class that privately inherits
`PPC64JITCore` and re-exports, by `using`, only: `GetVSXReg`, `GetReg`, the inline-constant
helpers, `VTMP1_VSX`/`VTMP2_VSX`/`VTMP3_VSX`/`VZERO_VSX`, the VSXR-typed helpers of §4.2,
and the VSX-form emitter methods. `GetVReg`, `VTMP1`/`VTMP2` as `VR`, `SRAFPR`, and every
VMX-form method are simply not names inside such a handler. The dispatch table entry is the
same `&PPC64JITCore::Op_x` thunk as now, which casts to the view. The cost is one `using`
line per re-exported method, about a hundred lines, written once.

What it catches: any VMX instruction, any `GetVReg`, any VR temporary, in a VSX-clean op,
at compile time. What it cannot catch: a helper that is itself declared VSXR-typed but
emits a VMX form inside -- so the helpers of §4.2 move into the view too, and the view's
own methods are the only helpers a VSX-clean op can call.

### 7.3 The flag and the macro cannot disagree

`DEF_OP_VSX(x)` contains `static_assert(IR::VSXClean(IR::OP_x))`; `DEF_OP(x)` contains
`static_assert(!IR::VSXClean(IR::OP_x))`. Every op has exactly one handler definition
(`IRDefines_Dispatch.inc` fills the table from the same list), so an op cannot be flagged
clean without its handler being in the view, and cannot be in the view without the RA
knowing. Ops with a size-dependent VMX arm (the byte/halfword arms of §4.2) are not
half-flagged: either the arm gets a VSX form or the op stays `DEF_OP`. If an arm is worth
splitting, it is split at the IR level into two opcodes the frontend chooses between, never
by a predicate the RA and the lowering could evaluate differently.

### 7.4 The runtime check that stands behind all of it is not an assert

`GetVReg` on an `FPRFixed` numbered 16 or above is guarded by `LOGMAN_THROW_A_FMT`
(`JITClass.h:916`). With `ENABLE_ASSERTIONS=OFF` -- the default in `CMakeLists.txt:22`,
`OFF` in `build-powerarm/CMakeCache.txt`, and therefore the state of the promoted binary
-- `LogManager.h:80` expands it to `(void)(pred)`, and `StaticFPRegisters[Reg.Reg]` then
reads past a 16-entry span. Today nothing reaches it, because the RA refuses to coalesce.
That is the only reason, and it is one RA edit away from a silent wrong-register
miscompile. Two changes, both independent of the rest of the item:

- `GetVReg` dies unconditionally (`ERROR_AND_DIE_FMT`) on a low-bank `FPRFixed`. It runs
  at block compile time, once per operand fetch; the cost is a compare.
- A post-RA validator, in the RA's forward pass where the register is assigned: for every
  op with an `FPRFixed` 16-31 operand or dest, `IR::VSXClean(op)` or the op is
  `LoadRegister`/`StoreRegister`/`VMov`, else die. This catches an RA bug (a future change
  to coalescing or eviction that hands a low-bank register to the wrong consumer), which
  the view class cannot, because the view only constrains lowerings.

Together: a wrong lowering fails to compile; a wrong flag fails to compile; a wrong RA
decision dies at translation of the first block that makes it, deterministically, in
Release, on the suite's `MAXINST=1` mode if nowhere else, because that mode turns every
guest instruction into a `LoadRegister`/op/`StoreRegister` triple and so exercises the
coalescing path on every op the tests touch.

### 7.5 The test that reads the other half

Encodings are checked the way `97b31fa57` checked `lxv`: assemble each VSXR-typed method
over registers drawn from both halves with `llvm-mc -triple=powerpc64le -mcpu=pwr9
--show-encoding` and compare bytes; a wrong TX/AX/BX bit is a wrong register, not a SIGILL,
so this is the only thing that finds it before a golden does. The goldens then read the
other half on purpose: a generated `fpregs` test (a `gen_simd.py` sibling; its `VREGS`
already spans both halves for the SIMD subset) runs every Tier A and B op with operands
from both banks, with the *twin* register of each operand poisoned with a distinct NaN
payload, so that an instruction that read vs17 when it should have read vs49 produces a
different number rather than the same one. It also includes the aliasing shapes
(`fadd d16,d16,d16`, `fmadd d16,d17,d16,d16`), an older-value-still-live shape (a guest
register rewritten while an earlier read of it feeds a later op in the same instruction
window), and sNaN/qNaN rows so the cold stubs run with low-bank operands.

## 8. ISA-OPPORTUNITIES §4 revisited on reach rather than count

Entries where the "same count" dismissal missed that only one of the pair can name the low
bank, and so only one can appear in a VSX-clean lowering:

| Entry | §4 verdict | On reach | Where it matters |
|---|---|---|---|
| `xxperm`/`xxpermr` vs `vperm` | wash, "N13 already exploits reach" | not a wash: `vperm` cannot read or write vs0-vs31; `xxperm` (3.0) can | 32-bit `uzp`/`zip`/`trn` and `ext` in float code (`fcvtn` pairs, complex kernels); the control still comes from the pool, now by `lxv` |
| `lxvwsx` (3.0) | "`ld1r` is 0.003% of Factorio" | the 4-instruction form ends in `vspltw`, VMX; `lxvwsx` is one VSX instruction with full reach | `ld1r {v16.4s}` feeding float lanes; small, now also a cleanliness question |
| `vextu*rx`, `vinsert*` | "in the emitter and used" | VMX-only; their 3.0 word twins `xxextractuw`/`xxinsertw` reach all 64 | `fmov w, v16.s[1]`, `ins v16.s[0], w1`, `mov s16, v17.s[2]` in float code |
| `xxspltib` | listed as done | it is also the only VSX byte immediate, so it is what a VSX-clean `VectorImm` has on 3.0 | masks for `fabs`/`fneg` done by `and`/`eor` in compiled code |
| `vcmpnezb`/`vcmpneb` | "a few hours when someone extends N1" | VMX-only, correctly so: N1 is Tier C | unchanged |
| `lxvl`/`stxvl`, `lq`/`stq`, `addpcis`, `cmpb`, `addex`, `bpermd`, `setb`, `modsd`, update forms | not levers | no reach dimension (GPR side, or VSX full-reach already) | unchanged |

And one entry the list does not have, because it is a trap rather than an opportunity:
the ISA 3.0 DS-form `lxsd`/`stxsd`/`lxssp`/`stxssp` that the "already done" paragraph
counts as a win reach v0-v31 only. They are correct where they are used today (dynamic and
V0-V15 registers); they must not be chosen for a low-bank target, and the typed emitter
makes that a compile error if their signatures stay `VR`.

## 9. What it is worth, and what it is not

Per guest instruction on V16-V31 operands, Tier A and B: -3 to -5 static host instructions
and -3 to -4 executed (§5), the alias stash included; on a loop-carried accumulator, 13 ->
9 cycles [INFERENCE]. Nothing on Tier C. Nothing on V0-V15 code, which must come out
byte-identical and is gated to (§10). Which fraction of the Octane deficit that is depends
on how much of Box2D's and NavierStokes' hot code V8 puts in d16-d27, which this document
could not measure; the one kernel it did measure puts 18% of its FP operand mentions there,
and `nbody` under GCC puts its working set there outright. Per the project's conventions
this is a direction, not an estimate: the thing to do after Stage 1 is run Octane once.

## 10. Staged plan, with the gate that would catch each stage being wrong

The floor for every stage: `unittests/A64Frontend/run.sh` in the three modes at 114 pass
(113 + 1 skip under `MAXINST=1`), `Scripts/powerarm/check-code-cache.sh` at 39 ok,
`unittests/MemoryModel/check.sh` at 6 unsound. Each stage adds the check that would fail if
*that* stage were wrong, because this project has shipped green suites over miscompiles
before (G2's Firefox crash with `cmpbranch` at 1954 cases green; the `MRS FPSR` CR0 clobber
live for a day with `mrsflags` not yet written).

**Stage 0 -- provable, zero behaviour change.** (a) `GetVReg` low-bank guard becomes
`ERROR_AND_DIE_FMT`. (b) VSX-form emitter methods retyped to `VSXR` with the `VR -> VSXR`
conversion; derived-TX forms for `mtvsr*`/`mfvsr*`/`xscmp[uo]dp`/`EmitXX2`/the record
compares. (c) `VSXClean` flag in IR.json, generated table, `DEF_OP_VSX` and the view class,
with the table empty and the static_asserts in place. (d) The post-RA validator.
*Gate:* the emitted-code identity gate. Run the suite and the slice under `setarch -R`
with `FEX_CODEHASHLOG` before and after; every `(RIP, size, hash)` line must be identical.
This is what catches a retyped method that changed a bit: the encoders derive the same
extension bits for vs32-vs63 that were hardcoded before, and the hash proves it over every
block the suite compiles. Plus the `llvm-mc` encoding sweep over both halves for every
retyped method, which catches the low-bank bits the identity gate cannot see (nothing emits
them yet).

**Stage 1 -- the scalar-FP set, behind a switch.** Flag and move to the view: Tier A, and
from Tier B `A64FToF`, `VFMLA` family, `VMov` (all sizes), the f32 compare arms,
`A64VecFloatToInt`/`Vector_FToI` f32 roundings, `Vector_FToF` f32<->f64, the logicals,
`VBSL`, `VCastFromGPR`, `VLoadTwoGPRs`, `VExtractToGPR` 64-bit, `VDupElement` 32/64,
`LoadMem`/`StoreMem` sizes 4/8/16 (the P8 aligned `lvx` tier skipped for low-bank
targets), the pool load. RA pre-pass and the `DecodeSRANode` gate, under
`POWERARM_VSXCLASSES=0` as the way back, hashed into the code-cache config id like
`DISABLECMPBRANCHFUSION` (`CodeCache.cpp:665`).
*Gate:* the floor; the `fpregs` golden of §7.5 from the Pi; `sigpreempt`/`sigedit` plus a
signal taken inside a d16-d31 loop whose handler reads `fpsimd_context` (goldened on the
Pi) -- the hoisted write is the same hoist V0-V15 have, and this is the test that says so
for the low bank; the identity gate restricted to units with no V16-V31 reference, which
must still be byte-identical; and `shapes-counts.txt` regenerated, where every Tier A/B
"hi" row must now equal its "lo" row and no row may grow. What catches it being wrong: a
wrong half is a diff against the poisoned twin; an RA hazard (a coalesced value clobbered
while live) is a diff in the aliasing and older-value rows, and in `MAXINST=1`, which
coalesces on every op; a misdeclared op does not compile; a bad RA decision dies.

**Stage 2 -- the float lanes and the 32/64-bit permute family.** `VZip`/`VUnZip`/`VTrn`
word and doubleword arms (`xxmrg*w`, `xxpermdi`, `xxperm` on 3.0), `VExtr` by 4/8/12,
`VInsElement`/`VInsGPR`/`VExtractToGPR` 32-bit on 3.0 (`xxinsertw`/`xxextractuw`),
`VectorImm` zero/byte/word arms, `VDupFromGPR`. Each op moves to the view only when every
arm is VSX-form on both levels; otherwise it is split at the IR level or left.
*Gate:* the floor; `simd_*` goldens (their `VREGS` already span both halves); static counts
on the §5 shapes extended with `zip1/uzp1/trn1 .4s/.2d`, `ext #8`, `ins`, `umov`.

**Stage 2b -- the shared VMX copy for mixed consumers.** Only with a count: add a one-line
counter of "coalescing refused because a consumer is not VSX-clean" per unit, run the
slice, Firefox and `node` on the kernel once. If it is rare, skip. Gate as Stage 1.

**Stage 3 -- a dynamic VSX pool / two-pool allocator class: not recommended.** Its only
customers are VSX-clean temporaries; the pool cannot exist without evicting the cold-body
scratch; dw1 of every low-bank register dies at every host call; and the only number that
could justify it, FPR-class spill volume in FP-heavy blocks, is unmeasured and was 0.02%
overall. If it is ever revisited, the view class and the flag are the same, and the RA
grows a real second class; nothing in Stages 0-2 has to be undone.

## 11. Is the item worth its risk?

Yes, in the shape of §6, and the reason is where the risk sits. The RA change is two lines
in `DecodeSRANode` and a pre-pass that writes two bit-vectors; it reuses the coalescing,
eviction and trivial-op deletion that V0-V15 have run through every block since M1, and it
is behind a config switch hashed into the cache id. The wide surface is the retyping of
lowerings and emitter methods, and that surface is covered by the compiler (§7.2), by the
identity gate (every block that does not reference V16-V31 must come out byte-identical,
which is most of them), and by an encoding sweep against an independent assembler. The one
truly new failure mode -- a low-bank register reaching a VMX form -- is made uncompilable
for lowerings, undeclarable for the table, and fatal-at-translation for the allocator. What
would make the answer "no" is a dynamic VSX pool, and that is the part to leave out.

## 12. What this could not measure

- **The share of Octane's FP work on V16-V31.** Octane is not on this machine; the V8
  figure (18% of FP operand mentions) is from one kernel, and Box2D keeps more live. JSC's
  (Bun) register allocation was not observed at all.
- **Cycles.** The 13 -> 9 loop-carried figure is composed from `NEON-LOWERINGS.md` §2(a)
  and `SCALAR-FP-LOWERING.md` §8 latencies; the chain was not timed, and whether `xxlor`
  on the critical path costs its full 2 cycles or is partially hidden by the `xxpermdi` is
  unknown. The alias stash is off the chain in theory and untested in practice.
- **FPR-class spill volume in FP-heavy blocks**, which is the only number that could
  reopen Stage 3. It needs a counter in `SpillReg` keyed on class; one run of the slice,
  Firefox and `node`.
- **How often a V16-V31 value has mixed consumers**, which decides whether Stage 2b is
  worth its forward-pass change. Same instrument.
- **The RA pre-pass cost**, asserted negligible from its shape; `instructions:u` on cold
  translation of the slice would confirm it.
- **Whether the `fcvt`/`fcvtzs`/`scvtf` rows' POWER8 fallbacks keep their counts** once the
  helpers are VSXR-typed; the design says yes (same instructions, derived bits), the
  identity gate will say.

## 13. Findings to carry out of this document, independent of the item

- `GetVReg`'s low-bank guard is dead in Release (§7.4). One line; do it now.
- The alias stash in `A64FArith`/`A64FMulAdd` fires whenever the RA reuses a killed source's
  register for the result, which the IR dump shows is the usual case for dynamic operands
  (§5.1). An RA
  preference to assign the dest a register different from the op's sources when the op is
  flagged "sources survive" would save one `xxlor` per scalar FP op on dynamic operands,
  with or without this item. Not costed here.
- `lxsd`/`stxsd`/`lxssp`/`stxssp` reach v0-v31 only (§3.3); keep their emitter signatures
  `VR` if they are ever added.
- LLVM's PPC disassembler does not gate on `-mcpu`; use binutils dialects for "which level"
  questions.

## 14. Files

- `vsx-probes/enumerate_isa.py`, `binutils-power{8,9,10}.txt`: the opcode-space enumeration and the per-level mnemonic sets (§3).
- `vsx-probes/defop_mnemonics.awk`, `defop_mnemonics.txt`: the per-`DEF_OP` mnemonic catalogue (§4).
- `vsx-probes/gen_shapes.py`, `shapes-counts.txt`: the per-shape static counts on both ISA levels (§5).
- `vsx-probes/v8_fp_kernel.js`, `v8-fp-register-mentions.txt`: the V8 register census (§5.3).
