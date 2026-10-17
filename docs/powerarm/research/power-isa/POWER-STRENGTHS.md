# POWER strengths: what this POWER9 does that AArch64 does not, and where POWERarm can lean on it

Written 2026-10-11 against the tree at `80ec9ac23`. This runs in the opposite direction from
`ISA-OPPORTUNITIES.md`: that document started from the emitted code and asked where it is
longer than the host needs; this one starts from the Power ISA and the AC922 and asks what
the host does well, or does at all, that AArch64 does differently or not at all, and where the
emulator -- emitted code and its own native code alike -- could lean on it. Facilities with no
ARM analogue are the interesting ones, because nobody translating ARM to POWER instruction by
instruction would ever reach for them.

Every number says where it came from. [CODE] is read out of this tree: `CodeEmitter/PPC64LE/
Emitter.h`, the `JIT/PPC64LE/*Ops.cpp` lowerings, the A64 frontend, `unittests/MemoryModel/`,
`CMakeLists.txt`, `git log`. [MEASURED] is one of three things run here: (1) a static census of
the reference binaries -- `cc1`, Firefox's `libxul`, VS Code's Electron binary, Factorio, the
Claude CLI, the rootfs `libc` and `libm` -- with the host `llvm-objdump` and
`census/census_power.py`, counting the *shapes* each POWER facility would touch, not just
mnemonics (`census/*.strengths.census`); (2) a disassembly of the existing
`build-powerarm/Bin/POWERarm` to see which ISA level the emulator's own code is compiled to;
(3) a disassembly of the rootfs glibc's `memset` to see which `DC ZVA` block sizes it will
use. [SPEC] is read out of the Power ISA 3.0B or the Arm ARM. Cycle costs are quoted from
`research/pipeline/POWER9-VS-A76-PIPELINE.md` where it measured them, and are marked so. No
build, no instrumented run, no source file changed. The Pi is gone, so every test proposed
below is self-checking in the style of `unittests/A64Frontend/ld1r.c` and `exitdead.S`: the
test computes the architecturally forced answer itself.

**One-paragraph answer.** Most of the obvious POWER-only instructions are already in use
(§3): `isel` is the select everywhere and is settled by measurement, `vpmsumd` is the CRC32
and PMULL, `vupk*` is every SXTL, `vmsumubm` is UDOT, `vpopcnt*` is CNT, and the native
`lwarx`/`stwcx.` loop, the count cache and the link stack are all landed. What is left is
not instructions the emitter lacks but *facilities the A64 frontend never reaches for*: the
VMX compares' record form, which writes CR6 and turns the four-instruction "did any lane
match" idiom (3,284 sites in libxul, 1,539 in VS Code) from ~15 host instructions into two
(§2.1); the byte-reversing loads and stores that the backend already has for x86 MOVBE and
the A64 frontend never emits, worth 3-4 instructions and a store-forward stall at each of
the 3,600-4,600 load- or store-adjacent REVs in libxul and VS Code (§2.2); the scalar
`popcntd` that AArch64 lacks, which is why compilers emit a four-instruction trip through
the vector unit at 778 sites in VS Code (§2.3); and `dcbt`/`dcbtst`, which the backend has
in a `Prefetch` op that the frontend's PRFM handlers never call, at 6,822 sites in VS Code
and 1,626 in the Claude CLI (§2.4). On the native side the finding is blunter: the
emulator's own binary contains **zero** ISA 3.0 instructions, because the tune block tries
`-march=native`, which clang rejects on PowerPC, so the decoder, the IR passes, the register
allocator, the dispatcher and the code cache all run at the POWER8 baseline (§2.5). Two more
are semantic rather than instructional: `DC ZVA` is lowered as eight scalar stores because
the guest is told the block is 64 bytes on a 128-byte-line host, and this glibc's `memset`
takes `dc zva` only when the block is exactly 64 (§2.6); and `xvmaxdp`'s NaN rule is already
A64's FMAXNM rule for single-NaN inputs, which lets the two input checks become one result
check (§2.7). The leads that die, with reasons, are in §4: binary128 hardware has 44 call
sites in Factorio and none anywhere else; `cmpeqb`/`cmprb` are reachable only through a
redirect that would call a host glibc already using them; `vexts*2*` is not SXTL; `vbpermq`
buys nothing the CR6 fusion does not; DMB ISHST's `hwsync` is model-pinned by cumulativity;
and `scv` is already what glibc issues. The ordering lever this document hoped for -- the
ctrl+`isync` acquire that GCC and LLVM both choose for their own code against the `lwsync`
this port emits after every LDAR, at 126,727 sites in libxul -- **is dead: the model rejected
it** (§2.9). ctrl+`isync` is a ppo relation and not a cumulative fence, so it loses the
read->write cumulativity `lwsync` carries, and `check.sh ctrlisync` is 10 unsound against
`current`'s 6. The two compilers can take that form because C++ acquire gets its
cumulativity from the release store; AArch64's LDAR does not get to assume one.

## 0. The ranked conclusions

Rank is expected value on the reference workloads (Firefox, VS Code, Factorio, the Claude
CLI, the compiled-C baseline), discounted by risk. Effort is given separately so the list
can be scheduled. "ISA" is the level the item needs; this host is POWER9 (ISA 3.0) and the
suite gates a `disableisa30` mode, so every item names its 2.07 fallback.

| # | Item | Today (host insns) | After | ISA (2.07 fallback) | Where it runs [MEASURED, static] | Risk | Effort |
|---|---|---|---|---|---|---|---|
| 1 | **"Any lane matched" through the VMX record-form compare and CR6** (`cm*; umaxv; fmov; cbz` -> `vcmpequb.; bc`) (§2.1) | ~15 | 2-3 | base | libxul 3,284 sites, VS Code 1,539, Factorio 123, Claude 77 (+`uminv` "all lanes" 53/45/9/99) | medium-low: the idiom's V intermediates must be rematerialised or proven dead | 1-2 days |
| 2 | **Load+REV and REV+store through `lhbrx`/`lwbrx`/`ldbrx` and `sthbrx`/`stwbrx`/`stdbrx`; standalone REV64 through `xxbrd`** (§2.2) | W 4, X 4 + a store-forward stall; REV64 alone 3 + stall | 1; 3 without the stall | base/2.06 (`xxbrd` 3.0; 2.07 keeps today's REV64) | VS Code 4,577 adjacent pairs, libxul 3,604, Claude 1,049, Factorio 307, libc 160 | low: same fault behaviour as the load | 1 day |
| 3 | **PRFM -> `dcbt`/`dcbtst`** (§2.4) | 0 (nop) | 1 | base | VS Code 6,822, Claude 1,626, libxul 1,023, Factorio 722, cc1 16 | none for correctness; the *sign* of the gain is the one thing to confirm by a run | hours |
| 4 | **Scalar popcount idiom -> `popcntd`** (`fmov d,x; cnt; addv; fmov w,s`) (§2.3) | 9-11 | 2-3 | 2.06 (`mtvsrdd` 3.0 for the V rematerialisation; 2.07 +1) | VS Code 778, Claude 75, libxul 66, libc 7, Factorio 5, cc1 2 | low | hours to 1 day |
| 5 | **Build the emulator's own binary for POWER9** (§2.5) | native text has 0 ISA 3.0 instructions; 750 `xxswapd`, 9,734 `lxvd2x`/`stxvd2x` | the compiler's P9 codegen for the decoder, IR passes, RA, dispatcher, cache | 3.0 (native; a packaging decision, not a `disableisa30` one) | every cold-translation path; Firefox's hitching is cold translation (HANDOVER item 10) | low technically; packaging must say which hosts the package runs on | hours + one packaging decision |
| 6 | **DC ZVA on a 128-byte-line host** (§2.6): `CacheLineZero` as `clrrdi; stxv x4` against the pinned zero, or DZP=1 | 9 (`clrrdi` + 8 `std`), 8 store slots | 5, 4 store slots (DZP=1: glibc's own `stp q` loop, 7 per 64 B) | 3.0 `stxv` (2.07: `stxvd2x` + `addi`, 8) | `memset` of 128+ bytes of zero in every guest: calloc, V8/JSC heap clearing; libc 2 sites, Claude 1 | low | hours; bounded by the memset redirect (THUNKS §4), which would use the host's `dcbz` natively |
| 7 | **FMAXNM/FMINNM through the hardware maxNum rule: one result check instead of two input checks** (§2.7) -- **DONE**, measured 2.26 -> 1.30 cycles throughput | 5 | 3 (4 where the RA ties Dst to a source) | 2.06 | scalar: libxul 5,328, Factorio 3,256, VS Code 652, Claude 137; vector `fmaxnm`/`fminnm` 456/502/143/18 | medium: the both-NaN order and FPCR.DN are exactly what the result check must route to the stub | done |
| 8 | **`vnegw`/`vnegd` for NEG.4S/.2D** (§2.8) | 2 | 1 | 3.0 (2.07 stays 2) | libxul 979, VS Code 868, Claude 249, Factorio 106, cc1 39 | none | hours |
| 9 | **CRC32: hoist the two Barrett constants out of the pool** (§3, already-there family) | ~22, 6 of them two `ld; li; lvx` pool loads | ~16 | 2.07 | Claude 143 sites, VS Code 168, libxul 7; zero in the A64Bench `crc32`, which is table-driven | none | hours; only if a CRC-bound workload appears |
| ~~10~~ | ~~**Acquire load as ctrl+`isync` instead of trailing `lwsync`**~~ -- **REJECTED by the model** (§2.9): `check.sh ctrlisync` is 10 UNSOUND against `current`'s 6. ctrl+`isync` is a ppo relation, not a cumulative fence, and the four new failures are all read->write cumulativity (`WWC012/018`, `IRWIW008/012`). SC-IRIW1 stays forbidden, so this is not trailing-sync; it is a different hole. Record in `unittests/MemoryModel/ctrlisync.known`. | `sync; ld; lwsync` | unchanged | -- | libxul 126,727 LDAR-family sites, VS Code 49,453, Factorio 10,885, Claude 9,068 -- all moot | -- | none; closed |

**Not levers** (§4): binary128 in hardware, `cmpeqb`/`cmprb`, `vexts*2*`, `vbpermq`/`vgbbd`
for movemask, `vmsum*` beyond UDOT, `dcbt` stream hints for the emulator's own paths, DMB
ISHST as `lwsync`, dropping a second LDAR's leading `sync`, `xsmaxjdp`, `vslv`/`vsrv`,
`vrl*mi` for SLI/SRI, `scv`, `copy`/`paste`, HTM, DFP, BCD, `vprtyb*`, `vmul10*`,
`setbool`-shaped sequences. Each has a reason, because each was on someone's list.

**What the leads in the brief turned into** (§3): `vpmsum*` -- there (CRC32 and PMULL),
with the pool loads as the only slack; `isel` -- there, settled by measurement; binary128 --
no track; `dcbt`/`dcbtst` -- there in the backend, unreachable from the frontend (item 3),
and not for the emulator's own paths; `vbpermq`/`vgbbd` -- there for x86, no A64 customer;
`cmpeqb`/`cmprb` -- no; `ldbrx`/`stdbrx` -- there in the backend, unreachable from the
frontend (item 2); `vexts*2*` -- no, and `vupk*` already is SXTL; `vmsum*` -- there for
UDOT; `vnegw`/`vnegd` -- yes, small (item 8); `vprtyb*`, `vmul10*`, DFP, HTM, `setbool` --
no.

**Sums, guessed.** Items 1-4 are idiom fusions on general code and NEON scan code; by
static density they are each 0.01-0.05% of a binary's instructions, so on the integer
workloads they are each a fraction of a percent of executed host instructions, and item 1
is the one with a 13-instruction delta per site. Item 5 touches no emitted byte and is the
only item that reaches every native path. Item 6 is the one whose dynamic weight nobody has
counted and whose upside is largest if it is high: §6 names the count. Item 10 was the other
one and is now closed unsound, so its 126,727 sites buy nothing.

## 1. Method

- **Instruction inventory.** `Emitter.h` (2,518 lines) was grepped for every lead and for
  the rest of the ISA 3.0 vocabulary [CODE]. Present: `vpmsum[bhwd]`, `isel`, `dcbt`, `dcbtst`,
  `dcbz`, `vbpermq`, `vgbbd`, `ldbrx`/`stdbrx` and the halfword/word forms, `vmsumubm`/
  `vmsumshm`/`vmsumuhm`/`vmsummbm`, `mcrf`, `vclzlsbb`/`vctzlsbb`, `xxbrd`/`xxbrq`, `lxvwsx`,
  `lxv`/`stxv`, `mtvsrdd`/`mfvsrld`, `cnttzd`, `maddld`, `extswsli`, `darn`, `mffscrn`. Absent:
  the binary128 family (`xsaddqp`, `xsmulqp`, `xsdivqp`, `xssqrtqp`, `xscvqpdp`, `xscmpuqp`),
  `cmpeqb`, `cmprb`, `vextsb2d`/`vextsh2d`/`vextsw2d`/`vextsb2w`/`vextsh2w`, `vprtyb*`, `vnegw`/
  `vnegd`, `vmul10*`, `setb`, `tbegin`, every DFP mnemonic, `xxextractuw`/`xxinsertw`, `vslv`/
  `vsrv`, `vrl*nm`/`vrl*mi`, `xsmaxjdp`/`xsmaxcdp`/`xscmpeqdp`/`xststdcdp`, `vmsumudm`,
  `copy`/`paste`/`cpabort`.
- **Use sites.** For every present mnemonic, which `DEF_OP` emits it (`census/` holds no
  copy of this; it is the grep in §3). That is how "there in the backend, never reached from
  the frontend" was found for `LoadMemRev`/`StoreMemRev` and `Prefetch`: both exist for the
  x86 frontend this fork does not have, and `TranslateLoadStore.cpp:89-95` returns `true`
  from `PRFM_lit`/`PRFM_imm` without emitting anything [CODE].
- **Shapes, not mnemonics.** `census_power.py` keeps an eight-instruction window and counts
  the idioms a facility would replace: a GPR REV whose source is the destination of the
  immediately preceding load (and the mirror for stores), the `fmov d,x; cnt; addv; fmov
  w,s` popcount, a vector compare followed within three instructions by `umaxv`/`uminv`/
  `addv`, a compare followed by `shrn`, three consecutive `addp`s, `and` then `addp`, the
  `ushr; shl; orr` rotate, `prfm` by hint, `dmb` by domain, `mrs` by register, `dc zva`, the
  `crc32*` forms, the widening and dot/MAC families, and every `bl` target so that calls to
  the libgcc binary128 helpers (`__multf3` and friends) and the `*l` libm entry points could
  be counted. Static counts over-weight cold code; the numbers say where the sites are, not
  how often they run.
- **The native binary.** `llvm-objdump -d build-powerarm/Bin/POWERarm` (696,689
  instructions), grepped for ISA 3.0 mnemonics and for the POWER8-era pairs they replace.
- **glibc's memset.** `llvm-objdump --disassemble-symbols=memset` of the rootfs
  `libc.so.6` (ALARM glibc 2.43), read for its `DCZID_EL0` test.
- **What I did not do:** build, run a guest, or profile. The host instruction counts below
  are read off the emitter path for the common operand shape.

## 2. The items

### 2.1 "Any lane matched": the VMX compares write CR6, and NEON compares write nothing

**The facility.** Every VMX compare has a record form (`vcmpequb.`, `vcmpgtub.`, `vcmpgtsb.`,
and the halfword/word/doubleword forms) that sets CR6 from the whole result: CR6[0] = all
lanes true, CR6[2] = all lanes false [SPEC, ISA 3.0B, the vector compare descriptions]. A conditional branch reads it
directly. NEON has no flags from vector compares; getting a scalar answer out of a compare
mask costs a reduction and a GPR move.

**What the guest does.** The compiler idiom for "does any lane match" is

    cmeq   v1.16b, v0.16b, #0        ; or cmeq/cmhi/cmhs/cmgt/cmge v1, v0, v2
    umaxv  b1, v1.16b                ; uminv for "do all lanes match"
    fmov   w0, s1
    cbnz   w0, L                     ; or cbz, or cmp #0 + b.ne

It is the exit test of every SIMD scan that is not the glibc `strlen` family N1 already
fused: Rust's `memchr`, Skia's and WebRender's pixel scans, V8's and JSC's string and JSON
scanners, Firefox's `nsTArray`/`Vector` searches. Static [MEASURED]: a vector compare followed
within three instructions by `umaxv` 3,284 times in libxul, 1,539 in VS Code, 123 in
Factorio, 77 in the Claude CLI; by `uminv` 53/45/9/99; by `addv` ("how many matched") 35/39/
33/88. Zero in cc1 and libc (libc's are the N1 shape).

**Today** [CODE]. `cmeq ... #0` is `vspltisw 0 ; vcmpequb` (2). `SIMDAcrossLanesMinMax`
(`TranslateSIMD.cpp:418-434`) emits `_VUMaxV`, and `DEF_OP(VUMaxV)` (`VectorOps.cpp:765-790`)
is `vmr` + four rounds of `vsldoi ; vmaxub` + `vspltisw ; vsldoi` = 11 for bytes (9 for
halfwords, 7 for words). `fmov w0, s1` is one `mfvsrwz`. The CBNZ is a fused compare-branch,
1-2. About 15-16 host instructions for byte lanes, with the VMX -> GPR crossing on the
dependent path.

**After.**

    [vspltisw VTMP, 0]               ; only for the #0 form
    vcmpequb. VTMP, v0, VTMP         ; CR6 from the whole compare; the mask is a dead temp
    bc        4, cr6.eq, L           ; "not all lanes false" = any lane matched

Two to three instructions, no reduction, no crossing. `uminv` ("all lanes") branches on
CR6[0] instead. Base ISA: the record forms are VMX from 2.03, so there is no 2.07 fallback to
write.

**Guest state at the boundary.** The idiom writes v1 twice (the compare mask, then the
`umaxv` result) and w0 once. w0 is the branch operand and is the CBNZ's own read; it is
written by `fmov` and the fused form must write it only if it is live after the block, which
the frontend already decides for the N1 fusion. v1 is the question: after the idiom it holds
`{0xFF or 0x00, zeros}`, which is guest-visible. Three sound choices, in order of cost: prove
v1 is overwritten before any exit or drain point in the block (the common case: it is a
compiler temp reused by the next compare), rematerialise it from the CR bit (`isel` to -1/0
then `mtvsrdd`, 2-3 instructions, still a win), or refuse the fusion. The N1 fuser
(`TryFuseVectorScan`, `TranslateSIMD.cpp:1079`) answers the same question for the same
registers and is the template; it currently accepts only the four-instruction shape that
ends the block.

**Test plan** (self-checking). For each compare kind (eq/hi/hs/gt/ge, byte/half/word, Q=0 and
Q=1), each consumer (`umaxv`+`cbnz`, `umaxv`+`cbz`, `uminv`+both, `cmp #0; b.ne`) and each of
the fused and the unfused forms (v1 read afterwards): inputs where exactly lane i matches for
every i, no lane, all lanes, and a dirty upper half of w0 before the `fmov`. The expected
branch direction is computed by the test from the lanes; after the idiom the test stores v1
and w0 and compares them with the values the unfused sequence must leave (the mask, the max
of the mask, the zero-extended count). Run in the three modes and with `disableisa30`.

### 2.2 Byte-reversing loads and stores: `lhbrx`/`lwbrx`/`ldbrx`, `sthbrx`/`stwbrx`/`stdbrx`

**The facility.** POWER has load-and-byte-reverse and store-and-byte-reverse for halfwords,
words and doublewords (X-form; the doubleword forms are 2.06) [SPEC]. AArch64 has REV as a
separate ALU instruction and no byte-swapping memory form, so every big-endian field a guest
reads or writes is a load or store plus a REV.

**What is in the tree** [CODE]. `DEF_OP(LoadMemRev)` and `DEF_OP(StoreMemRev)`
(`MemoryOps.cpp:516-545`) lower exactly these, including the TSO `lwsync` arm, and the
emitter has all six mnemonics. They exist for x86 MOVBE. Nothing in `A64Frontend/` emits
either op. Standalone `DEF_OP(Rev)` (`ALUOps.cpp:1773-1828`) is `rlwinm ; rlwimi` (2) for
halfwords, `rlwinm ; rlwimi ; rlwimi` (3) for words, and for doublewords `addi r1 ; stdbrx ;
ld` through the red zone: three instructions and a store-to-load forward, which the pipeline
research measured at 15-18 cycles. The comment in that op says "brh/brw/brd if available
(POWER10)"; on this POWER9 the memoryless form is `mtvsrd ; xxbrd ; mfvsrd` (3.0), three
instructions through the VSU with no stall.

**What the guest does** [MEASURED]. GPR REVs with the load that feeds them or the store they
feed adjacent: VS Code 3,833 + 744, libxul 2,786 + 818, Claude 648 + 401, Factorio 250 + 57,
libc 77 + 83 (77% of libc's 208 scalar REVs: `resolv`, `inet`, the NSS wire formats), cc1 0.
These are the WebRTC/ffmpeg/PNG/ICU big-endian parsers in libxul and Chromium, JSC's and V8's
serialisers, Factorio's network protocol. The `rev16` share is large in VS Code (5,553 of
11,617 GPR REVs) and libxul (1,968 of 10,154): 16-bit fields.

**After.** `ldr w0,[x1,#8] ; rev w0,w0` -> `lwbrx w0, x1, TMP` (the offset materialised the
way `MakeAddrForm` already does, 1-2); `ldr x0 ; rev x0` -> `ldbrx` (1, and the stall gone);
`ldrh w0 ; rev16 w0` -> `lhbrx` (1); `rev w0 ; str w0` -> `stwbrx`. Standalone `rev x` ->
`mtvsrd ; xxbrd ; mfvsrd` on 3.0, today's form on 2.07. The frontend side is a two-instruction
adjacency matcher of the `TryFuseRbitClz` kind (`TranslateDataProcessing.cpp:387`): the REV's
source must be the load's destination, the load's value must have no other reader, and the
pair must be in one block.

**Guest state at the boundary.** The fused instruction faults where the load faults, with the
same PC and the same unchanged Rt, so a resumed fault sees exactly the unfused state. The REV
cannot fault. An asynchronous signal delivered between the load and the REV in the unfused
code would see Rt unreversed; the fused code cannot produce that state, which is the same
acceptance the RBIT+CLZ, vector-scan and exclusive-loop fusions already made.

**Test plan** (self-checking). Every width, every alignment 0-15, with and without an
immediate offset, the pre- and post-indexed forms, Rt == Rn, and the store direction; the
expected value is assembled byte by byte by the test. One case puts the loaded field flush
against an unmapped page and requires no fault (`ldbrx` reads exactly 8 bytes, like `ld`);
one puts it across the page boundary and requires `SIGSEGV` with the faulting PC at the
load and Rt untouched, checked from the signal frame the way `sigedit` does. The unfused
form (REV destination different from the load's, or the loaded value read again) is tested
beside each fused case.

### 2.3 Scalar popcount: `popcntd` exists, and ARMv8.0 has to go through the vector unit

**The facility.** `popcntd`/`popcntw` (2.06) count bits in a GPR [SPEC]. AArch64 without
FEAT_CSSC (the A76 is ARMv8.2) has only the vector `CNT`, so `__builtin_popcount` is four
instructions across two register files:

    fmov   d0, x0
    cnt    v0.8b, v0.8b
    addv   b0, v0.8b
    fmov   w0, s0

**What the guest does** [MEASURED]: exactly this sequence 778 times in VS Code (V8's register
allocator and GC bitmaps, `base::bits::CountPopulation`), 75 in the Claude CLI (JSC/WTF), 66
in libxul, 7 in libc, 5 in Factorio, 2 in cc1 (GCC's own bitmaps use a table). Looser forms
(`fmov` then `cnt`, `cnt` then `addv`, with other instructions between) are 1,092 and 1,020 in
VS Code.

**Today** [CODE]. `fmov d0, x0` is `mtvsrdd` (1, 3.0) or `mtvsrd ; xxpermdi` (2); `cnt v0.8b`
is `vpopcntb` (1) plus the 64-bit form's upper clear (1); `addv b0` is `DEF_OP(VAddV)`'s
`vspltisw ; vsum4ubs ; vsumsws` (3) plus the Q=0 `LowHalves` (1-2) plus the sized store
(1); `fmov w0, s0` is `mfvsrwz` (1). Nine to eleven host instructions with two register-file
crossings on the dependent chain, for one guest bit count. The backend already has
`DEF_OP(Popcount)` (`ALUOps.cpp:1644-1660`): a mask for sub-64-bit sizes and `popcntd`,
written for x86 POPCNT and never reached from A64.

**After.** `popcntd w0, x0` (1; `rldicl` first for a W source whose upper half may be dirty,
2), plus `mtvsrdd v0, r0, w0` to leave v0 holding what the `addv` would have left -- the
count in byte 0, zeros above -- unless the fuser proves v0 dead. Two to three instructions on
3.0, one more on 2.07.

**Correctness.** None in the arithmetic. The only question is v0's final value, answered as
in §2.1. Note `popcntw` is a *per-word* count (two 32-bit results in one register), which is
why `DEF_OP(Popcount)` masks and uses `popcntd`; the fusion must keep that.

**Test plan** (self-checking). The idiom compiled by the rootfs GCC from `__builtin_popcount`
and `__builtin_popcountll`, applied to 0, all-ones, every single bit, random values, and W
inputs with a dirty upper half; the expected count is computed by a loop in the test; v0 is
stored after the idiom and compared with `{count, 0 x 15}`. Both ISA levels.

### 2.4 PRFM is a nop today; `dcbt`/`dcbtst` are one instruction and cannot fault

**The facility.** `dcbt` and `dcbtst` are the data-cache touch for load and for store;
neither can cause a data storage exception, so a prefetch of an unmapped or protected
address is silently dropped [SPEC, ISA 3.0B Book II, the `dcbt`/`dcbtst` descriptions]. The
TH field selects transient and stream forms.

**What is in the tree** [CODE]. `DEF_OP(Prefetch)` (`MemoryOps.cpp:2246-2260`) emits `dcbt`
or `dcbtst` by the op's `ForStore` bit and documents why the stream encodings are not used
("want a measured depth, which we do not have"). The A64 handlers `PRFM_lit`, `PRFM_imm` and
`PRFM_unscaled_imm` (`TranslateLoadStore.cpp:89-95`) return `true` and emit nothing; the
generic load path returns the unscaled prefetch as a nop too (`:122-126`). The
register-offset form (`prfm pldl1keep, [x0, x1, lsl #3]`) was not traced here; the
implementation must find where it lands and cover it, since the census counts it with the
rest.

**What the guest does** [MEASURED]. `prfm` 6,822 times in VS Code (5,502 `pldl1keep`, 856
`pldl3keep`, 202 `pstl1strm`, 151 `pstl1keep`, 97 `pldl1strm`), 1,626 in the Claude CLI
(1,535 `pldl1keep`), 1,023 in libxul, 722 in Factorio (692 `pldl1keep`), 16 in cc1, 0 in
libc. These are Chromium's and V8's `PrefetchRead`/`PrefetchWrite` in hash-table probes,
marking and the Oilpan/V8 heap visitors, JSC's `prefetch` in its GC marking, Factorio's own
containers: explicit, compiler-visible prefetches on access patterns a hardware prefetcher
cannot predict, which is why they were written.

**After.** Rt bits 4:3 select PLD/PST, bit 0 KEEP/STRM, bits 2:1 the level [SPEC]. PLD* ->
`dcbt r0, Addr`; PST* -> `dcbtst r0, Addr`; PLI* (instruction prefetch) stays a nop; the
level and the STRM policy are dropped, as the backend op already does. One instruction per
site, address computed by the existing `MakeAddrForm`. Base ISA.

**Correctness.** None: the instruction is a hint and cannot fault. It must use the RA=0
form (`r0` is the backend's literal-zero register, which the x86 `Prefetch` already does);
the indexed and unprivileged encodings of size=11 opc=10 are unallocated and must keep
decoding as such (`TranslateLoadStore.cpp:122-126`).

**Value, honestly.** This is the one item in the table whose sign a run must confirm. A
prefetch tuned for a Cortex-A76 (64-byte lines, ~10-cycle L2) lands on a 128-byte-line
POWER9 with a stronger hardware prefetcher; the explicit ones in hash tables and GC marking
are the kind that help on every microarchitecture, and a useless `dcbt` costs an issue slot
and nothing else. §6 asks for one run of code-server each way under an env toggle, the
owner's instrument.

**Test plan** (self-checking). `prfm` of every hint kind to a mapped address, to an unmapped
page, to a read-only page, to a guest-code page under mtrack protection, and at the last
byte of a mapping: no signal in any case, memory unchanged (checked), the instruction
counted as executed by the surrounding code's own results.

### 2.5 The emulator's own binary is compiled for POWER8

**What the tree does** [CODE]. `CMakeLists.txt:479-500`: with `TUNE_CPU=native` the build
tries `-march=native`, which clang rejects on PowerPC (`-mcpu=native` is the spelling there),
so no CPU flag is added; otherwise it adds `-mcpu=${TUNE_CPU}`. `build-powerarm/CMakeCache.txt`
carries `-mtune=native` and nothing else; `CMAKE_CXX_FLAGS_RELEASE` is `-O3 -DNDEBUG`.

**What the binary says** [MEASURED]. `build-powerarm/Bin/POWERarm` (4.2 MB, 696,689
instructions) contains **0** of `cnttzd`, `cnttzw`, `setb`, `maddld`, `extswsli`, `modsd`,
`modud`, `modsw`, `moduw`, `addpcis`, `cmprb`, `cmpeqb`, `darn`, `lxv`, `stxv`, `mffsl`,
`mtvsrdd`, `mfvsrld`, `xxspltib`, `xxbrd`, `lxvx`, `stxvx` -- every emitted-code mnemonic in
the JIT's own tables is data, not text -- against 13,227 of the POWER8-era set, among them
6,177 `stxvd2x`, 3,557 `lxvd2x`, 750 `xxswapd`, 1,234 `isel`, 285 `popcntd`, 211 `cntlzd`,
296 `cntlzw`. Those are the compiler's own struct copies, memcpy expansions and bit tricks at
the ISA 2.07 level: every `lxvd2x`/`stxvd2x` with a displacement is an `addi` or index
register plus the load, and every `xxswapd` is dead on a host with `lxv`/`stxv`.

**What it would change.** `-mcpu=power9` gives clang `lxv`/`stxv` with displacement,
`mtvsrdd`/`mfvsrld` for the GPR<->VSX crossings in the IR passes, `cnttzd` for every
`__builtin_ctz` in the register allocator's and DFCE's bitmaps, `setb`/`isel` improvements,
`maddld`, and the P9 scheduling model. The reach is the whole native side: the decoder, the
IR builder and passes, the register allocator, the dispatcher and lookup-cache C++, the code
cache's serialisation and load, the syscall layer and the loader. Nothing about emitted code
changes, and `disableisa30` -- which gates *emitted* code -- is untouched. Expected gain on
compile time: small, 0-3% (guess; generic compiler-shaped C++ rarely gains more from P9
codegen); it is ranked where it is because it is free, touches no guest semantics, and is the
only item that reaches every native path at once.

**The catch is packaging, not code.** A binary built with `-mcpu=power9` executes ISA 3.0
instructions unconditionally and dies with `SIGILL` on a POWER8. The ArchPOWER glibc this
host runs (`2.43+r37`) says nothing about the distribution's baseline, and
`packaging/archpower` has to state one. Two sane shapes: a `-mcpu=power9` package that
refuses to start on a host without `PPC_FEATURE2_ARCH_3_00` in `AT_HWCAP2` with a message
naming the reason (one `getauxval` at the top of `main`), or two package flavours. The
owner's own machine is the AC922 and the port is for it; the decision is his.

**Test plan.** No guest-visible change: the full three-mode gate plus `check-code-cache.sh`,
and the `HostFeatures` ISA probe must still drive emitted code (the `disableisa30` suite must
pass on the P9-compiled binary exactly as before).

### 2.6 DC ZVA on a host with 128-byte lines: what `dcbz` cannot do, and what to do instead

**The facility, and why it does not fit.** `dcbz` zeroes one host data-cache line -- 128 bytes
on POWER8 and POWER9, which `HostFeatures` reads from `AT_DCACHEBSIZE` [CODE,
`MemoryOps.cpp:2181-2200`] -- at an address truncated to the line. A64's `DC ZVA` zeroes
`4 << DCZID_EL0.BS` bytes at an address truncated to that size [SPEC]. The tree advertises
`DCZID_EL0 = 0x4`, the Pi's value, i.e. 64 bytes (`SystemRegisters.h:42-43`), so a `dcbz`
would zero 64 bytes the guest did not ask for; `DEF_OP(CacheLineZero)` therefore walks the
block with `clrrdi` and eight `std r0` (9 instructions, 8 store slots) and keeps the `dcbz`
arm behind a line-size check that the comment itself says is never taken on this hardware.

**Who executes DC ZVA** [MEASURED]. Nobody but `memset`. The instruction appears 2 times in
libc, once in the Claude CLI, 0 times in libxul, VS Code, Factorio and cc1; `mrs dczid_el0`
3 times in libc and once in Claude. The rootfs glibc 2.43 `memset` (disassembled at
`libc.so.6+0x9e700`) does this for 128 bytes or more of zero:

    mrs  x5, DCZID_EL0
    and  x5, x5, #0x1f          ; DZP and BS together
    cmp  x5, #0x4               ; exactly "ZVA allowed, 64-byte block"
    b.ne L(no_zva)              ; anything else: the stp q0,q0 loop
    ...
    add  x3, x3, #0x40
    dc   zva, x3
    subs x2, x2, #0x40
    b.hi .-12

So the `dcbz` route -- advertise `DCZID_EL0 = 0x5` (128-byte blocks) and lower `DC ZVA` as one
`dcbz` -- is dead on this rootfs: glibc would take the `b.ne` and never issue a `DC ZVA`
again. (Earlier glibc memsets carried a 128-byte ZVA arm for ThunderX-class parts; the
version pinned in this rootfs does not, and that is the one that runs here.) The route is
listed in §4 with that reason.

**Two routes that are live** [CODE]. (a) Keep `DCZID = 0x4` and lower `CacheLineZero` as
`clrrdi TMP ; stxv VZERO,0(TMP) ; stxv VZERO,16(TMP) ; stxv VZERO,32(TMP) ; stxv VZERO,48(TMP)`
-- the ISA 3.0 DQ-form `stxv` takes a displacement that is a multiple of 16 and the pinned
`VZERO_VSX` is already in the register file: 5 instructions, 4 store slots, against 9 and 8
today; on 2.07, `stxvd2x` is X-form and needs an `addi` per store, 8 instructions. glibc's
loop is then `add ; dc zva ; subs ; b.hi` = 1 + 5 + 2 = 8 host instructions per 64 bytes.
(b) Advertise `DCZID_EL0 = 0x10` (DZP=1, ZVA prohibited, an architecturally valid value
[SPEC]): glibc takes its `stp q0,q0 ; stp q0,q0 ; add ; subs ; b.hi` loop, which with the
DQ-form `stxv` the backend already uses for displaced Q stores (`PPC64Emitter.cpp:523-528`)
is 2 + 2 + 1 + 2 = 7 host instructions per 64 bytes and no emulator code at all -- but it
changes a guest-visible register away from the Pi's value for every guest, and anything that
reads `DCZID_EL0` for a reason other than `memset` (Go's runtime does; nothing in the
reference set does) sees a machine without ZVA. (a) is the one to take: same cost, no ABI
change.

**Bounded by the memset redirect.** `THUNKS-DESIGN.md` §4 specs a symbol-level redirect of
`memcpy`/`memmove`/`memset` to the host glibc, whose `memset` uses `dcbz` natively for large
zeroing. If that lands, every large `memset` leaves the JIT and this item is moot; if the
redirect stays unbuilt, this is the cheapest way to get most of the way there. Either way the
cost today is paid by every `calloc`, every V8 new-space and JSC heap clearing, every
zero-initialised buffer of 128 bytes or more.

**Test plan** (self-checking). Read `DCZID_EL0`, compute the block size, `DC ZVA` at every
offset within a guarded buffer, and require exactly the block at the truncated address to be
zero and every guard byte intact; a block whose last byte abuts an unmapped page must
complete; a block starting in an unmapped page must `SIGSEGV` with the faulting PC at the
`DC ZVA`. Then `memset` of 127, 128, 255, 256, 4096 and 65536 bytes, checked by scan, so
the glibc path is exercised whichever arm it takes.

### 2.7 FMAXNM/FMINNM: POWER's own maximum is already A64's maxNum

**The facility.** ISA 2.06 `xvmaxdp`/`xvmindp`/`xvmaxsp`/`xvminsp` return the non-NaN
operand when exactly one is a QNaN and a quieted NaN when either is an SNaN [SPEC, ISA 3.0B,
the `xvmaxdp`/`xvmindp` descriptions]. That is A64's FMAXNM/FMINNM rule for every single-NaN
input. The cases that can differ are two QNaN inputs (which operand's NaN comes back is a
priority rule on each side, and the test in this section pins it rather than this document),
SNaN quieting with the IOC flag (which this port does not track, `ISA-OPPORTUNITIES.md`
§3.15), and FPCR.DN=1, where A64 substitutes the default NaN.

**Today** [CODE]. `DEF_OP_VSX(A64FMinMax)` (`A64FPOps.cpp:629-680`) emits, for the
number-form and the propagating form alike, `xvcmpeqdp. V1,V1 ; bc cold ; xvcmpeqdp. V2,V2 ;
bc cold ; xvmaxdp`: five instructions, two data-dependent branches, with every NaN case in a
cold stub (F2). Scalar FMAXNM/FMINNM sites [MEASURED]: 5,328 in libxul, 3,256 in Factorio
(its geometry and physics), 652 in VS Code, 137 in Claude; the vector forms 456/143/502/18.
The NaN-propagating FMAX/FMIN scalars are 0, 0, 11 and 2 -- compilers lower `fmax()` and
`std::fmax` to the NM forms and `std::max` to `fcsel` -- which is also why `xsmaxjdp` is in
§4 and not here.

**After.** For the NM forms only: `xvmaxdp Dst,V1,V2 ; xvcmpeqdp. T,Dst,Dst ; bc cold`:
three instructions, one branch. A NaN result means both inputs were NaN or one was an SNaN,
which are exactly the cases where hardware and A64 may disagree, and the cold stub (with its
existing DN handling) recomputes them. A single QNaN input never reaches the stub and the
hardware answer is the architectural one. With DN=1 and a single QNaN input, A64 FMAXNM still
returns the number, so DN does not reopen that case. The propagating forms keep the input
checks (hardware returns the number where A64 wants the NaN).

**Correctness.** Medium, because it is a change to a NaN path that the goldens covered and
that the self-checking test must now cover from the architecture: the test implements
`FPProcessNaNs` and `FPMaxNum` itself, bit for bit including payloads, for (QNaN,x), (x,QNaN),
(QNaN,QNaN) with distinct payloads, (SNaN,x), (x,SNaN), (SNaN,QNaN), (+0,-0), (-0,+0),
(+Inf,x), denormals under FZ, each with DN=0 and DN=1, scalar and vector, f32 and f64, and
compares the emulator's result with it. 2.06 throughout.

**DONE, 2026-10-17.** `DEF_OP_VSX(A64FMinMax)` now takes the hardware max/min first and
branches to the stub only when the RESULT is a NaN, for the NUMBER forms. The propagating forms
keep their input checks, because A64 wants the NaN back where hardware hands back the number, so
a result check would see a number and never branch.

**The ISA-book claim was checked against the chip first** (`maxnan.c`), because it is what the
whole change rests on and because a wrong answer here would be silent -- the single-QNaN case
stops reaching the stub entirely:

| inputs | `xvmaxdp` | `xvmindp` | A64 wants |
|---|---|---|---|
| (QNaN, 5.0) and (5.0, QNaN) | 5.0 | 5.0 | the number |
| (-QNaN, 5.0) | 5.0 | 5.0 | the number |
| (QNaN_A, QNaN_B) | QNaN_A -> a NaN, so the stub still runs | | stub |
| (SNaN, 5.0) | quieted NaN, so the stub still runs | | stub |
| (+0, -0) and (-0, +0) | +0 both orders | -0 both orders | +0 / -0 |

So every case the result check stops sending to the stub is one where the hardware answer already
IS the architectural answer, at f32 and f64, in either operand order and either NaN sign. The ±0
rows also show this shape neither fixes nor breaks ±0: it is the same `xvmaxdp` either way.

**Cost.** 2.26 -> 1.30 cycles throughput, 1.74x (`../neon/probes/RESULTS-p9.txt`), identical at
f32 and f64. Latency barely moves, 3.50 -> 3.33, which is the expected shape rather than a
disappointment: the branches are control dependencies the core speculates past, so what the
shorter form buys is issue slots. Three instructions where the result register is distinct from
both sources; four where it is not, since an aliased source must be stashed for the stub, exactly
as `DEF_OP_VSX(A64FArith)` already does -- and at most one stash is ever needed, because Dst
aliasing both sources means the two sources are the same register. One degenerate shape,
`fmaxnm d0, d1, d1`, was 3 instructions before and can be 4 now; compilers do not emit it.

**Test.** `unittests/A64Frontend/fminmaxnm.c`, self-checking and `// NO-GOLDEN`: it computes
FPUnpack, FPProcessNaNs, FPMax/FPMin and FPMaxNum/FPMinNum from the pseudocode, bit for bit with
NaN payloads, over 25 operand pairs x 4 FPCR modes x scalar, accumulator-shaped, and vector
forms. 614 checks, 0 failures. Two controls, because a self-checking FP test can pass vacuously
in two different ways: `control-reference-pinned` asserts the reference itself against 13
architecturally-known values (a reference that returned the NaN for (QNaN,num) would agree with
precisely the lowering bug this test exists to catch), and the suite runs it a second time under
`POWERARM_FPCOLD=0`, where the cold path is unreachable and a clean run would mean the test never
exercises it -- so zero failures there is itself a failure.

**Two things the test turned up.** FPCR.DN and FZ are stored but not emulated (COLD-BLOCK-DESIGN
§53), so 120 rows cannot give the architectural answer; they are bucketed and reported rather
than skipped, and a DN/FZ row still FAILS if its answer is neither architectural nor the
documented DN=0,FZ=0 one. The bucket also reports when a row starts giving the architectural
answer, so it cannot silently rot once DN/FZ lands. And under `POWERARM_FPCOLD=0` only the
propagating `fmax`/`fmin` rows fail, never the NM ones: across this matrix, in the modes this port
implements, the hardware answer alone is already ARM-exact for FMAXNM/FMINNM, so the stub is
DN insurance for them rather than load-bearing today. That is 25 pairs and not a proof over all
inputs, and the stub stays.

### 2.8 `vnegw`/`vnegd`

`DEF_OP(VNeg)` (`VectorOps.cpp:346-359`) is `vspltisw 0 ; vsub*` because the pinned zero is
`VZERO_VSX`, a VSX-numbered register the VMX subtract cannot name [CODE]. ISA 3.0 `vnegw`/
`vnegd` negate word and doubleword lanes in one instruction [SPEC]; the byte and halfword
forms have no `vneg` and stay at two. Sites [MEASURED]: 979 in libxul, 868 in VS Code, 249 in
Claude, 106 in Factorio, 39 in cc1. Hours, no risk, behind `SupportsISA30`; the self-check is
every lane value including the minimum, both widths, Q=0 and Q=1.

### 2.9 The acquire load: `lwsync` after, or ctrl+`isync`

**What the tree emits** [CODE]. `current.map`, the lowering the model check runs:
`LDAR = sync; ld; lwsync`, `LDAPR = ld; lwsync`, `STLR = sync; st`. The leading `sync` on LDAR
is RCsc: it is what keeps a STLR before a later LDAR ordered, and the trailing-sync proposal
that moved it to the store side was rejected on SC-IRIW1 (`trailing.known`, with the 2016
counterexample cited). The trailing `lwsync` is the acquire half. The comment above
`DEF_OP(LoadMemTSO)` (`MemoryOps.cpp:1107-1145`) records that the port *originally* used the
other valid acquire, a self-compare, a never-taken branch and `isync`, that it was replaced by
`lwsync` "on architectural grounds, not measured ones", and that the cost of the choice "is
UNMEASURED". The pipeline research priced `lwsync` at +14 cycles and `sync` at +42 [MEASURED,
PIPELINE §4.6]; it did not price `isync`.

**Why it is a POWER strength.** `ld ; cmpd ; bne- ; isync` is the acquire that GCC's
`rs6000` backend and LLVM's PPC backend both emit for their own `__atomic_load(acquire)`:
the compare-and-never-taken-branch creates a control dependency that `isync` turns into
ordering against every later instruction, at the cost of a pipeline refetch rather than a
barrier that waits on the store queue. Two compilers' maintainers judged it the cheaper form
on this hardware. herd7's `ppc.cat` knows the shape as `ctrlisync`.

**Where it runs** [MEASURED]. LDAR-family sites: 126,727 in libxul (122,529 `ldar`: Mozilla's
`Atomic<T>` defaults to SeqCst loads), 49,453 in VS Code, 10,885 in Factorio (8,833 of them
`ldarb`, spin flags), 9,068 in the Claude CLI; 0 in cc1 and libc. Executed share unknown.

**What this document asked for, and the answer.** It asked for a map file `ctrlisync.map`
with `LDAR = sync; ld; cmp; bne; isync` and `LDAPR = ld; cmp; bne; isync`, run through
`unittests/MemoryModel/check.sh`, and said it should come out sound. **It does not, and the
item is closed as rejected** [MEASURED, `check.sh ctrlisync` at `0f50cc889`]:

```
current     1052 translated,  6 UNSOUND
ctrlisync   1052 translated, 10 UNSOUND
```

The six are current's accepted divergences unchanged; the four new ones are `WWC012`,
`WWC018`, `IRWIW008` and `IRWIW012`. The prediction above was right about SC-IRIW1 -- it
stays forbidden, the leading `sync` is untouched, and this is **not** trailing-sync again --
but wrong about soundness, for a reason the document did not consider: ctrl+`isync` is a
**ppo relation, not a fence**. All four new failures are read->write, where the write the
acquire load read from must also propagate before a later store. That is A-cumulativity;
`ppc-checks.cat` builds it only from `(rfe;fence)` with `fence = sync|lwsync|eieio`, and
`ctrlisync` lives in `ci0`, reaching `ppo` and `hb` and no further. The local edge is not the
problem -- LB with ctrl+`isync` on both threads is forbidden. `lwsync` supplies the same
local ordering *and* the cumulative term, which is the whole of the difference. GCC and LLVM
can choose this form because C++ acquire only has to synchronize with a *release* store, so
its cumulativity comes from the other side; AArch64's LDAR is other-multi-copy-atomic and
promises more. The full argument, the rc11 check showing all four are nonetheless outside
what C or C++ can rely on, and why they were still not written into a `.known` file, are in
`unittests/MemoryModel/ctrlisync.known`. No backend code exists for this; the trailing
`lwsync` stays, and §6's counted run is moot. The leading `sync` stays whatever the answer;
nothing on the store side moves.

## 3. The leads from the brief, each answered

| Lead | Status in the tree [CODE] | Answer |
|---|---|---|
| `vpmsumb/h/w/d` | `DEF_OP(CRC32)` (`ALUOps.cpp:4724-4790`) is the two-`vpmsumd` Barrett reduction, ~22 host instructions of which 6 are two `ld ; li ; lvx` pool loads; `DEF_OP(PCLMUL)` and `DEF_OP(VPMullB)` lower PMULL with `vpmsumd`/`vpmsumb` | **There.** The A64Bench `crc32` row is a table-driven software CRC built `-mgeneral-regs-only` (`unittests/A64Bench/src/crc32.c`): no `CRC32*` instruction executes in it, and its 2.04x is the register-through-memory chain HANDOVER item 11 (P1(b)) names. Real CRC32 sites: Claude 143, VS Code 168, libxul 7. The only slack is the pool loads (rank 9) |
| `isel` | Every select (`ALUOps.cpp:2893-3110`), both constant and register forms, with the comment block recording that a branchy constant form was tried, measured, and lost | **There, settled.** CSINC/CSINV/CSNEG are `_Add`/`_Not`/`_Neg` + `_NZCVSelect` (`TranslateDataProcessing.cpp:582-610`), i.e. `isel` + 1 |
| Native binary128 (`xsaddqp` ...) | Absent from the emitter | **No track.** Call sites of the libgcc `__*tf*` helpers [MEASURED]: Factorio 44 (its own statically linked copy; 13 of them `__extenddftf2`), VS Code 2, libxul 0, Claude 0 (`strtold` once), cc1 0 (GCC uses its own `real.c`), libc 0; libm's 186 `*l` calls are its own `sqrtl`/`modfl` family. A symbol-level redirect of `__multf3` and friends would carry the thunk call cost for 44 sites in one game. The host ABI's double-double `long double` is indeed irrelevant to the hardware instructions, and the hardware instructions are irrelevant to these workloads |
| `dcbt`/`dcbtst` with TH streams | `DEF_OP(Prefetch)` exists, unreached; stream TH encodings deliberately unused | **Guest side: item 3.** Emulator side: not levers (§4) |
| `vbpermq`, `vgbbd` for movemask | `DEF_OP(VExtractSignBits)` (`VectorOps.cpp:2691-2720`) is `li ; lvsl ; vspltisb ; vslb ; vbpermq ; mfvsrd`, for x86 PMOVMSKB; `vgbbd` is RBIT | **No new customer** (§4) |
| `cmpeqb`, `cmprb` | Absent | **No** (§4) |
| `ldbrx`/`stdbrx` | `LoadMemRev`/`StoreMemRev` present, never emitted by the A64 frontend | **Item 2** |
| `vextsb2d`/`vextsh2d`/`vextsw2d` | Absent; `VSXTL`/`VSXTL2`/`VSSHLL` are `vupklsb`/`vupklsh`/`vupklsw` and the high forms (`VectorOps.cpp:1197-1280`), one instruction | **No**: `vexts*2*` sign-extends the low bits of each lane *in place* (byte-of-doubleword to doubleword); SXTL is an unpack, which `vupk*` already is in one instruction (§4) |
| `vmsumubm`/`vmsumshm`/`vmsumuhm` | `VUDot` is one `vmsumubm` (`VectorOps.cpp:421`); SDOT is three `VUDot`s with a bias (`TranslateSIMDSaturate.cpp:984-1021`); `vmsumshm` serves `VMaddPairwise16` | **There for UDOT.** SDOT could be `vmulesb ; vmulosb ; vmsumshm ; vmsumshm` (4 + a splat), but its sites are runtime-dispatched Chromium media code (§4) |
| `setbool`-shaped sequences | `setb` is open in `ISA-OPPORTUNITIES.md` §3.12 and left alone here; the P10 `setbc` family does not exist on this host | **No** |
| `vprtyb*`, `vnegw`/`vnegd`, `vmul10*`, DFP, HTM | All absent | parity, BCD, decimal, transactions: **no** (§4); `vnegw`/`vnegd`: **item 8** |

## 4. Not levers, and why

- **Binary128 in hardware.** 44 call sites in Factorio, 2 in VS Code, 0 elsewhere (§3).
  A redirect track exists for `memcpy` because every binary calls it on every hot path; it
  does not exist for `__multf3` because almost nothing calls it.
- **`cmpeqb`/`cmprb`** (3.0, is-byte-in-set and is-byte-in-range). No A64 instruction maps
  to them; `isalpha`-shaped guest code is a table lookup through `__ctype_b_loc`, which is a
  load, not an idiom, and `strspn`/`strcspn`/`strpbrk` are glibc NEON. The only way to reach
  them is the string-function redirect of `THUNKS-DESIGN.md` §4, and the host glibc that
  redirect would call already uses them. Nothing for the JIT to do.
- **`vexts*2*`.** Not SXTL (§3). The one A64 shape it would serve -- sign-extend the low byte
  of each doubleword lane in place -- does not exist in NEON.
- **`vbpermq`/`vgbbd` for movemask.** NEON has no movemask, so compilers use either the
  `cm*; umaxv; fmov; cbz` form (item 1 takes it to CR6, no mask needed) or the nibble mask
  `cm*; shrn v.8b,v.8h,#4; fmov x,d` (1,172 sites in Claude, 244 in VS Code, 105 in libxul),
  whose consumers are `rbit; clz` (already `cnttzd` via `TryFuseRbitClz`) divided by four.
  `DEF_OP(VUShrNI)` is already `vspltish ; vsrh ; vspltisw ; vpkuhum` (4), so the nibble
  idiom costs 1 + 4 + 1 host instructions; a `vbpermq` route produces a *different* mask (one
  bit per byte, 16 bits), needs a control constant, and would have to rewrite the `rbit;
  clz; lsr #2` consumer to match. Six instructions either way, with a pool load on the
  `vbpermq` side.
- **`vmsum*` beyond UDOT.** SDOT: 4,056 sites in VS Code and 16 in libxul, zero elsewhere,
  and VS Code's are libyuv/Skia/ffmpeg kernels behind runtime feature dispatch that an
  emulated A76 does select -- but the 4-instruction form above is a few hours of work for
  code whose execution nobody has seen in a profile. Do it when a media workload is on the
  bench. `smlal`/`umlal` (22k sites in VS Code, 2.9k in libxul) are `vmule*`/`vmulo*` plus
  an add and need no `vmsum`.
- **`dcbt`/`dcbtst` streams for the emulator's own paths.** The inline L1 probe at every
  block exit (`BranchOps.cpp:124-260`) is two dependent loads whose address is the guest
  target, known one instruction before the probe; a prefetch cannot shorten a dependent
  chain it is part of, and the count-cache work (`f691b1089`) measured the probe at 3.8-7.2
  cycles, latency not bandwidth. The publish path is a `memcpy` of ~230 bytes per unit
  (`JIT.cpp:6249`) followed by the `dcbst; sync; icbi; sync; isync` the hardware requires
  (`JIT.cpp:1691-1700`); the host `memcpy` already touches ahead. The block-link walk is cold
  by construction. The stream forms want a measured depth, as the `Prefetch` comment says,
  and nothing here has one.
- **DMB ISHST as `lwsync`.** `DEF_OP(Fence)` lowers `FenceType::Store` to `hwsync` and says
  why: an AArch64 barrier is cumulative over coherence order, `lwsync` is not, and `ppc.cat`
  allows the R shape under `lwsync`; `check.sh` pins it with the CO-R1/CO-R2 controls. The
  reason holds. Sites: 3,608 in Claude, 17 in libc, 0 in libxul/VS Code/Factorio.
- **Dropping the leading `sync` of a second LDAR in the same block.** Tempting at 126k sites,
  and unsound: two LDARs with no store between them are exactly the SC-IRIW1 reader, and
  IRIW is forbidden on Power only with `sync` between the reads; `lwsync` admits it. The
  leading `sync` is the one that keeps the shape forbidden.
- **`xsmaxjdp`/`xsminjdp`** (3.0, Java semantics, scalar DP only). Their NaN priority matches
  A64's FMAX/FMIN for DN=0, but the NaN-propagating scalar forms have 0-11 sites per binary
  (§2.7) and the f32 scalar would need two converts around the DP instruction. The NM forms,
  which have the sites, are served by 2.06 (item 7).
- **`xststdcdp`/`xvtstdcdp`** (3.0, test data class). The F1 post-check `xvcmpeqdp.` is
  already one instruction into a CR field; the data-class test is one instruction into a
  vector mask with no record form, so it is never shorter.
- **`vslv`/`vsrv`** (3.0, per-byte variable shift). `USHL.16B` already uses `vslb`, which is
  per-byte; the cost in `_VUShl` + `_VNeg` + `_VCMPLTZ` + `_VSShr` + `_VBSL`
  (`TranslateSIMD.cpp:824-828`) is A64's "negative count shifts right", which no POWER shift
  expresses.
- **`vrlwmi`/`vrlwnm`** (3.0, rotate-and-insert under mask). SLI/SRI (642/454 sites in VS
  Code and libxul) are a shift plus `xxsel` with a mask constant, 3; `vrlwmi` is 1 plus a
  control-vector constant from the pool, 3-4. No gain.
- **`scv`.** Host syscalls go through glibc's `::syscall()`
  (`Source/Tools/LinuxEmulation/LinuxSyscalls/Syscalls.cpp:387,700,779`), and this host's
  glibc 2.43 issues `scv 0` when `AT_HWCAP2` has `PPC_FEATURE2_SCV`. Already there.
- **`copy`/`paste`** (3.0, 128-byte block copy). On POWER9 the paste side is supported to
  accelerator windows (VAS/NX), not to ordinary memory [SPEC, POWER9 user manual; the owner's
  console can confirm with `grep -o 'copy\|paste' /proc/cpuinfo` and the kernel's VAS state];
  and a guest `memcpy` is either its own NEON loop or the redirect.
- **HTM (`tbegin.`).** Off by default on POWER9 Linux unless the kernel is booted with
  `ppc_tm=on` (the owner's console says whether `htm` is in `/proc/cpuinfo`; the emulated
  one does not), no guest construct maps to it, and the one emulator use
  anyone proposes -- speculative SMC-free stores -- is already answered by the store
  backpatcher (`SMCStoreBackpatch.h`), which uses a POWER facility of a different kind: the
  26-bit `b` displacement, to patch a faulting store site into a stub.
- **DFP, BCD (`vmul10*`, `addg6s`, `cdtbcd`), parity (`vprtyb*`, `prtyd`).** No A64
  instruction, no library idiom, no guest in the set that does decimal arithmetic in
  hardware's terms (cc1's `libdecnumber` is software and runs at `-O0` speed in any case).
- **`dcbz` for DC ZVA via `DCZID_EL0 = 0x5`.** This glibc's `memset` takes `dc zva` only at
  exactly 64 bytes (§2.6); advertising 128 would steer it to the `stp` loop and the `dcbz`
  would never execute. The `stxv` form is the lever.
- **`setb` for CSET/CSETM, `lxvl`/`stxvl`, `cmpb`, `addex`, `bpermd`, `xxperm` over `vperm`,
  `lq`/`stq` for LDP, `addpcis`, update-form loads.** Declined in `ISA-OPPORTUNITIES.md` §4;
  the reasons were re-read and hold. One refinement to the `cmpb` entry: it is now doubly
  moot, because the string redirect that could reach a `cmpb`-shaped `strlen` would call the
  host's, which already uses it.

### 4a. Scalar half converts on POWER8, branch-free -- REJECTED on measurement, 2026-10-17

After the vector FP16 converts landed, the scalar ones looked like the last place a pre-ISA-3.0
host pays an order of magnitude: `fcvt d,h` 43 host instructions against 4 on ISA 3.0, `fcvt h,s`
80 against 19. Recommended twice on that basis. The basis was wrong.

**43 is the STATIC size of `EmitHalfToDouble`'s whole decision tree.** A normal half executes 17
of it -- `NotSpecial` then `Normal`, both branches taken -- and a denormal about 26, through the
`cntlzd` renormalise. So the comparison was never 43 against 4; it was a branch-free sequence
against 17 instructions and two branches.

Measured (`../neon/probes/RESULTS-p9.txt`, the `h2d_*` rows), throughput in cycles:

| shape | insns | lat | tp |
|---|---|---|---|
| current, normal input | 17 | 25.29 | **7.88** |
| current, denormal input | 26 | 36.59 | 11.08 |
| the vector identity, one lane, constants pooled | 15 | 33.45 | **13.89** |
| the same, constants built through a GPR as today | 24 | 34.26 | **20.03** |

**1.76x worse** with the constants pooled and 2.5x worse without, and worse on latency too. The
identity's strength is that it converts four lanes for ~26 instructions; spent on ONE lane it pays
the whole sequence and all three constants for a single value, while the branchy GPR path runs
cheap (~0.5 cycle) integer ops and its two branches are perfectly predicted. Pooling the
constants helps -- 20.03 to 13.89, which is the `PPC64VConstIndex` follow-up measured in passing
-- but not nearly enough to close the gap.

**The limit of this result.** The probe feeds one value, so those two branches are predicted every
iteration. A guest stream that makes them unpredictable would shift the comparison, and that is a
property of the workload rather than of the lowering; it was not measured. For anything
predominantly normal -- which is what FP16 image and inference code is -- the current code wins
decisively and should be left alone.

## 5. What a POWER10 host would change

Not a proposal -- this port runs on a POWER9 -- but the list says what the ceiling of
"lean on the host" is, so nobody re-derives it: `setbc`/`setbcr`/`setnbc` (CSET and CSETM in
one instruction, no `li`), `brd`/`brh`/`brw` (REV in one GPR instruction, making §2.2's
standalone case trivial), `vextractbm`/`vextracthm`/`vextractwm` (movemask in one instruction,
reopening §4's `vbpermq` entry), `cfuged`/`pdepd`/`pextd` (BFI/BFXIL/UBFX families and the
bit-gathers behind `tbl`-based decoders), `paddi` and the prefixed loads (34-bit
displacements: every `adrp`+`add` and most `movz`/`movk` pairs become one instruction, and
the JIT's 5-word constants two), `plxv`/`pstxv` (PC-relative vector constants, retiring the
pool), `xxgenpcvbm`/`vstribl`/`vclrlb` (the string-scan primitives N1 fuses by hand),
`vrlq`/`vslq`/`vsrq` (quadword shifts), `xxeval` (any three-input bitwise function, which
is BSL/BIT/BIF and every mask combine in one instruction), and `xxsplti32dx`/`xxspltiw`
(immediate vector constants). `darn`, `vpmsumd` and the count cache are the same.

## 6. What an implementation agent should count, and in what order

None of items 1-8 needs an instrumented build to be written; each is a source change gated
by the suites plus the self-checking test its section names. The counts below decide
scheduling and the two measurement-gated items, and each is one number from a build that
already exists or a throwaway counter, not a campaign.

1. **Item 3, the sign.** Build PRFM -> `dcbt` behind `POWERARM_PRFM=0/1`, then one
   code-server session and one `claude --version` each way, timed once. Positive or flat: ship
   on. Negative: ship off and record the number. This is the only item whose direction is in
   doubt.
2. ~~**Item 10, soundness then cost.**~~ **Done, and it stopped at the first gate.**
   `check.sh ctrlisync` is 10 UNSOUND against `current`'s 6 (§2.9,
   `unittests/MemoryModel/ctrlisync.known`), so there is nothing to build and nothing to
   count. Nobody needs to read `pm_cmplu_stall_any_sync` for this.
3. **Item 6, the dynamic weight.** Count `CacheLineZero` executions (one counter in the
   existing stats path) over a code-server start and a Factorio benchmark. Below a few
   million it is hours of hygiene; above, it is the item -- or the argument for the memset
   redirect.
4. **Item 5, the compile-time direction.** Cold slice once on a `-mcpu=power9` build; the
   recorded baseline is the comparison point. Any gain is a gain; the item is free.
5. **Items 1, 2, 4: executed idiom counts**, if the owner wants them ranked against each
   other before scheduling: a per-fusion counter at translation (static over a run) in a
   `code-server` request and a Firefox page load is enough. Static densities are in §7; the
   fusions are cheap enough to build in the order of the table without this.
6. **Item 7: nothing to count.** The delta is deterministic (5 -> 3 at every site) and the
   gate is the NaN table test.

## 7. Static census of the reference binaries [MEASURED]

Host `llvm-objdump -d --no-show-raw-insn --no-leading-addr <binary> | python3 -I
census/census_power.py`, run natively; the script and the seven raw outputs
(`census/*.strengths.census`, which also carry the top-120 `bl` targets of each binary) sit
next to this document. Counts are sites, not executions. Totals differ by a few percent from
`ISA-OPPORTUNITIES.md` §6 because this script counts every decoded line including PLT stubs.

| shape | cc1 (gcc 16.1.1) | libxul (Firefox 156) | code (VS Code 1.138) | factorio 2.1.19 | claude 2.1.296 | libc.so.6 |
|---|---|---|---|---|---|---|
| `total` | 6,302,726 | 28,802,025 | 39,071,029 | 10,398,128 | 15,487,349 | 293,455 |
| vector compare then `umaxv` within 3 (§2.1) | 0 | 3,284 | 1,539 | 123 | 77 | 0 |
| vector compare then `uminv` within 3 | 0 | 53 | 45 | 9 | 99 | 0 |
| vector compare then `addv` within 3 | 0 | 35 | 39 | 33 | 88 | 0 |
| vector compare then `shrn` within 3 (§4) | 0 | 105 | 244 | 3 | 1,172 | 15 |
| `umaxv` / `uminv`, all | 0 / 0 | 3,536 / 137 | 2,014 / 99 | 157 / 31 | 480 / 1,160 | 0 / 0 |
| GPR `rev`/`rev16`/`rev32`, all (§2.2) | 22 | 10,154 | 11,617 | 1,316 | 2,650 | 208 |
| load then REV of its result | 0 | 2,786 | 3,833 | 250 | 648 | 77 |
| REV then store of its result | 0 | 818 | 744 | 57 | 401 | 83 |
| vector `rev16`/`rev32`/`rev64` | 8 | 1,334 | 2,502 | 433 | 1,240 | 2 |
| popcount idiom, exact 4-sequence (§2.3) | 2 | 66 | 778 | 5 | 75 | 7 |
| `fmov d,x` then `cnt` (loose) | 6 | 56 | 1,092 | 5 | 120 | 7 |
| `cnt` then `addv` (loose) | 17 | 84 | 1,020 | 5 | 228 | 10 |
| `prfm`, all (§2.4) | 16 | 1,023 | 6,822 | 722 | 1,626 | 0 |
| `prfm pldl1keep` | 0 | 983 | 5,502 | 692 | 1,535 | 0 |
| `prfm pst*` | 1 | 35 | 353 | 9 | 66 | 0 |
| `dc zva` / `mrs dczid_el0` (§2.6) | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 1 / 1 | 2 / 3 |
| scalar `fmaxnm`+`fminnm` (§2.7) | 0 | 5,328 | 652 | 3,256 | 137 | 0 |
| vector `fmaxnm`+`fminnm` | 0 | 456 | 502 | 143 | 18 | 0 |
| scalar `fmax`+`fmin` (propagating) | 0 | 0 | 11 | 0 | 2 | 0 |
| vector `fmax`+`fmin` (propagating) | 0 | 20,779 | 5,335 | 0 | 8 | 0 |
| vector `neg` (§2.8) | 39 | 979 | 868 | 106 | 249 | 0 |
| `ldar`/`ldarb`/`ldarh` (§2.9) | 0 | 122,529 / 4,132 / 66 | 17,455 / 19,642 / 12,356 | 2,052 / 8,833 / 0 | 5,997 / 3,066 / 5 | 0 |
| `stlr`/`stlrb`/`stlrh` | 0 | 71,483 / 329 / 0 | 3,986 / 806 / 12,636 | 102 / 299 / 0 | 1,027 / 270 / 0 | 0 |
| `ldaxr` / `stlxr` (native loop since `TryFuseExclusiveLoop`) | 0 | 13,659 / 90,034 | 46 / 48 | 6 / 8 | 66 / 74 | 0 |
| `dmb ish` / `ishld` / `ishst` | 2 / 6 / 0 | 416 / 39,389 / 0 | 99 / 124 / 0 | 1 / 22 / 0 | 3,630 / 1,278 / 3,608 | 6 / 28 / 17 |
| `crc32*` + `crc32c*` (§3) | 0 | 7 | 168 | 0 | 143 | 0 |
| libgcc binary128 helper calls (§3) | 0 | 0 | 2 | 44 | 0 | 0 |
| `sdot` / `udot` (§4) | 0 | 0 / 16 | 4,056 / 3,373 | 0 | 0 / 9 | 0 |
| `smlal*`+`umlal*` | 0 | 4,264 | 27,359 | 202 | 1,219 | 0 |
| widening (`sxtl`/`uxtl`/`sshll`/`ushll`, all) | 39 | 23,357 | 9,999 | 2,498 | 16,463 | 40 |
| `sli` / `sri` | 0 | 454 / 621 | 642 / 160 | 131 / 0 | 665 / 91 | 0 |
| `umulh` (already `mulhdu`) | 111 | 4,580 | 7,738 | 1,728 | 12,255 | 76 |
| `rbit` (RBIT+CLZ fused) | 887 | 9,322 | 6,297 | 1,038 | 7,995 | 53 |
| `mrs tpidr_el0` | 2 | 7,678 | 6,134 | 294 | 6,770 | 1,441 |

Reading the table: the SIMD-scan idioms and the byte-reversal pairs live in the two
browsers' engines and in JSC, not in the compiler; the compiled-C baseline (`cc1`, libc) is
untouched by items 1-4 and 6-8 and reached only by item 5. Factorio is the one FP-heavy
binary (3,256 NM min/max sites, 21,875 scalar compares) and the only binary128 user. libxul's
acquire/release density -- one LDAR per 227 instructions -- is an order of magnitude above
every other binary, which is why item 10 is a Firefox measurement and nothing else.
