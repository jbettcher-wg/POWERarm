# NEON landings on POWER: the lowering catalogue

Research for POWERarm's next NEON workstreams: for each Advanced SIMD
operation family, the best *exact* POWER8 sequence, the POWER9 (ISA 3.0)
alternative where one exists, its measured cost on the POWER9, and the
cross-cutting decisions (register placement, constants, movemask idioms,
store forwarding, serialising state) that bound what any lowering can do.
No emulator source was changed; everything here is a probe result or a
citation.

Tags: **[MEASURED]** this run, on the machines below; **[CODE]** file:line in
the `powerarm-m1/research-neon` worktree at `c2d113228`; **[SPEC]** Power ISA
3.0C (`PowerISA_public.v3.0C.pdf`, page numbers as printed) or the POWER9
User's Manual (`POWER9_um_OpenPOWER_v20GA`, "UM"), or the Arm ARM;
**[INFERENCE]** reasoning not yet measured, stated so it can be.

Companion studies (on `powerarm`, read with `git show powerarm:<path>`):
`docs/powerarm/research/scalar-fp/SCALAR-FP-LOWERING.md` (scalar FP; its §8
measured FPR-half vs VMX-half for `xs*`/`xv*` and the `mtvsrd`→`xv*` +1.7-cycle
hazard, its §9 the V16-V31 context traffic at 18 % of nbody's cycles) and
`docs/powerarm/research/pipeline/POWER9-VS-A76-PIPELINE.md` (pipeline model,
PMU guide, the 22.6-cycle loop-carried store→load round trip, the 5-7-cycle
back-edge, code-shaping rules 1-10). This document does not repeat them; it
covers the vector-integer side and cites them where the numbers meet.

Machines: POWER9 DD2.3 host (Arch, kernel 7.2.6-64k, GCC 16.1.1), timings
pinned to NUMA node 0 hardware threads 100/104/108/112 with the node quiet
(each CPU 0-5 % busy before the runs); Raspberry Pi 5 (Cortex-A76, Debian
trixie, GCC 14.2) for the golden captures only, one core, ~4 s of total CPU.

## 0. Executive summary

**Method.** A C reference model of the ARM semantics (`probes/neon_ref.h`,
`neon_corpus.h`) was first proved against the real instructions on the Pi:
351 (op, width) cases plus fused/convert cases under all four rounding modes,
exhaustive over all 65536 byte pairs for 8-bit binary ops, edge×random
corpora (20000 vectors per case, 4096 for 8-bit) otherwise, with shift counts
covering [-2W, 2W] and random high bits, table indices clustered at the
boundaries, and NaN/±0/denormal/overflow float lanes. Eight model bugs were
caught by the Pi and fixed (`REFERENCE-MODEL-VERIFIED`, `probes/logs/pi.golden`).
Then every candidate POWER sequence in `probes/neon_parity_p9.c` (inline asm,
exactly the instructions a JIT would emit, constants included) was checked
lane by lane against that model on the POWER9, once compiled for POWER8
(`-mcpu=power8`, zero ISA 3.0 encodings per `isa30scan.sh`) and once for
POWER9: **344 of 344 candidate lowerings are lane-exact in both builds**, the
reference hashes are byte-identical to the Pi's, and the only failing rows are
the deliberately naive "documenting" forms (24 rows) that show *why* the exact
form is needed. Costs come from `probes/neon_bench_p9.c` (latency of a
dependent chain, throughput over four independent chains, in ns and in core
cycles at the 3.80 GHz the pinned thread ran at; `probes/logs/bench-p9.out`).
Workload evidence is a glibc string microbenchmark (`probes/workloads/strloop.c`)
run under POWERarm with the POWER9 PMU and the JIT's own host code dumped
(`probes/logs/strloop-*`).

**Ranked recommendations** (impact is measured host-instruction or cycle
change on the affected path; "risk" is semantic risk, all rows below are
parity-checked unless marked):

| # | Change | Measured impact | ISA | Semantic risk |
|---|---|---|---|---|
| 1 | **Fuse the string early-exit idiom** `CMEQ → UMAXP/UMINP/SHRN → FMOV x,d → CBZ/CBNZ` into the backend's existing CR6 `CondJump` mode (`VCmpElementSize`, `[CODE] BranchOps.cpp:876-909`, set by nobody today) | The three hot glibc loops (`strchr`, `memchr`, `strlen`: 64 % of the workload's cycles) spend 17-29 host instructions per 5-6 guest instructions; the compare+branch part is 11-13 of them and carries the only VSU→FXU crossing. `vcmpequb.`+`bc` costs 1.1 cycles latency vs 8-14 for the `umaxp`→`fmov`→`cmpdi` chain [MEASURED]. Expected: ~2× fewer host instructions in those loops (3.75 G → ~2 G over the run) | 2.07 (`vcmpequb.`), 3.0 adds `vctzlsbb` for the exit path | Low: the lane mask and the `fmov` value are still materialised on the exit path only; the loop path is exact by construction |
| 2 | **Replace the scalar spill loops** for `VSRSHR` (`[CODE] VectorOps.cpp:1367-1424`, ~60 host instructions, stack round trip) and `VSQSHL` (`:1437-1521`, ~100 instructions with branches) by the vector forms: rounding shift = `(x >> n) + ((x >> n-1) & 1)` (5 insns, 7.5 cyc), saturating shift = shift/shift-back/compare/select (8 insns, 9.9 cyc) | 60→5 and 100→8 instructions; no store→load through the stack (15-18 cycles each on POWER9 [MEASURED]) | 2.03 | None: exhaustive for 8-bit, all immediates |
| 3 | **Q=0 results: drop the 3-instruction `VMov(i64)`** (`vspltisw`,`vsldoi`,`vsldoi`, `[CODE] VectorOps.cpp:62-104`, emitted by `StoreVQ` for every D-register result, `[CODE] IRBuilder.h:222-228`) for `xxpermdi Dst,ZERO,Dst,1` | 6.0 → 3.0 cycles latency, 3 → 2 insns (1 with a hoisted zero) on every 64-bit-vector instruction (`shrn`, `xtn`, `cmeq v.8b`, `addp v.8b`…) | 2.06 | None |
| 4 | **`CMHI` = `vcmpgtu*`** (1 insn) instead of `VNot(VCMPEQ(VUMax))` (3; `[CODE] TranslateSIMD.cpp:256`); **`CMTST` = `vand`+`vcmpgtu*(x,0)`** (2+zero) instead of `VNot(VCMPEQZ(VAnd))` (3+zero; `:257`); `CMHS`/`CMGE` = compare-swapped + `vnor` (2) | 8.0 → 3.0 cyc; 7.1 → 5.1 cyc [MEASURED] | 2.03 | None: exhaustive |
| 5 | **`UMAXP`/`UMINP`/`ADDP` with both operands equal** (the string idiom; 116 of 116 `umaxp` in the census are `v,v`) as `vsrh 8; vmaxub; vpkuhum` (4 insns incl. the count) instead of unzip/unzip2/max (6) | 10.3 → 8.0 cyc latency, 3.4 → 2.9 tp [MEASURED]; superseded by #1 in loops but it also covers the exit path | 2.03 | None |
| 6 | **`ADDV.4S` fix** (`vsumsws` saturates, `[CODE] VectorOps.cpp:586`): the frontend already sidesteps it (`:368`), the backend op should use the rotate-add (4 insns, 10 cyc) so the IR op is honest and VSCR.SAT stays usable as QC (the buggy form sets SAT on 40 % of random inputs); `ADDV.16B/8H` may keep `vsum4ubs`+`vsumsws` (cannot saturate at those widths: proof in §3.10, measured SAT-clear in §2(e)) | correctness; unblocks QC-via-SAT | 2.03 | None |
| 7 | **`FMLS`/`VFNMLA`/`VFNMLS`: negate the operand, never the result.** `xvnmsubasp` rounds `(A×B−T)` then negates: wrong under RU/RD (1 ulp) and flips the NaN sign [MEASURED, `probes/logs/parity-p9.out` rows `fmls_nv.*`]. Exact form: `xvnegsp A'; xvmaddasp T,A',B` | 2 insns, same latency (6.3 cyc); fixes the known VFNMLA/VFNMLS bug | 2.06 | None for the numeric lanes; NaN precedence still needs #8 |
| 8 | **FMA NaN precedence: gate a fixup behind one record-form compare.** POWER's `xvmadd` takes A's NaN before T's; ARM takes the addend's first and sNaN before qNaN, and returns the default NaN for `inf×0 + qNaN` [MEASURED]. `xvmaddasp; xvcmpeqsp. r,r; bc` = 3 insns, 6.3 cyc on the no-NaN path; the 30-insn fixup (`fmla_exact`) runs only when a lane is NaN | Exactness for `FMLA/FMLS` vector at 1.0× the cost of the bare instruction; the same gate replaces `PropagateNaNOperand`'s dozen instructions per scalar FP op (`[CODE] TranslateFP.cpp:135-160`) | 2.06 | None (exact under all four rounding modes) |
| 9 | **`TBL`/`TBX`** (glibc/musl census: 0; codecs and base64: many): POWER8 `vperm` with `idx^15` and a `vcmpgtub` mask = 6 insns, 5.5 cyc; POWER9 `xxspltib 31; vminub; vpermr ZERO-second` = 3 insns, 3.0 cyc; `TBL2` = 4/8, `TBL3/4` = 8/12 (vsel by index bit 5) | 6 → 3 insns on P9, all sizes exact incl. indices ≥ 64 | 2.03 / 3.0 | None |
| 10 | **`SQDMULH.8H` = `vmhaddshs a,b,0`, `SQRDMULH.8H` = `vmhraddshs a,b,0`** (1 insn + zero, 7 cyc); 32-bit forms via `vmulesw/vmulosw` + `vmrgow` + one compare pair for the `INT_MIN×INT_MIN` case (9 insns) | closes a "known gap" at 1-2 instructions for the 16-bit forms that codecs use | 2.03 (16-bit), 2.07 (32-bit) | None: edge corpus incl. `0x8000×0x8000` |
| 11 | **64-bit saturating add/sub**: `UQADD` = 3 insns (`vaddudm`, `vcmpgtud`, `vor`), `UQSUB` = 3, `SQADD/SQSUB` = 9 with two constants; `SQNEG/SQABS.2D` = 6-7 | closes the "64-bit saturating" gap without scalar code | 2.07 | None |
| 12 | **AES**: `AESE` = `vxor; vcipherlast x,0; vsldoi x,x,4` (3), `AESD` = same with `vncipherlast`/`vsldoi 12`, `AESMC` = `REV32·vcipher(vncipherlast(REV32 x,0),0)` (4 on P9 with `xxbrw`, 6 on P8), and the round pair `AESE+AESMC` = `vxor; vcipher` (2, 8 cyc) once the state is kept byte-reversed | AES at 2-3 host instructions per ARM instruction; the round pair is exact and 3.5× cheaper than the parts | 2.07 | None (probed with the C AES model the Pi verified) |
| 13 | **`UDOT` = `vmsumubm a,b,acc`** (1 insn, 7 cyc, exact); `USDOT` = `vmsummbm` (1); `SDOT` = `vmsummbm(a, b^0x80) − (Σa)<<7` (6 insns) | closes dotprod at 1 instruction for the unsigned form | 2.03 | None |
| 14 | **`PMULL` = `xxmrgld ZERO,x; vpmsumd`** (2 insns, 9 cyc), `PMULL2` = `xxmrghd x,ZERO; vpmsumd`, `PMULL.8B` = two `vmrglb ZERO` + `vpmsumb` (3) | closes the "PMULL" gap; note `vpmsumd` is 6 cycles, not a serialising op | 2.07 | None |
| 15 | **Shift counts for 64-bit lanes 16..47 need a GPR splat on POWER8** (`vspltisb` reaches only 0-15 and 48-63 through the sign wrap): `xxspltib` on POWER9 (2.4 cyc) vs `li; mtvsrd; xxpermdi` (3.5 cyc, 3 insns) | small, but it is the reason `VShlI i64` costs 4 instructions today (`[CODE] VectorOps.cpp:790-800`) | 3.0 | None |

**Cross-cutting conclusions** (§2): (a) VMX-form and VSX-form instructions are
indistinguishable in latency and throughput (`vand` = `xxland` = 2.0 cyc,
`vsel` = `xxsel`, `vperm` = `xxperm` = 3.0 cyc [MEASURED]); moving a value into
the FPR half and back costs two `xxlor` (4 cyc round trip) and buys nothing
unless a whole computation stays there, so **pinning more than 16 guest
V registers is worth it only through a VSX-aware allocator**, not through
per-op moves. (b) `vspltisb`/`vspltisw` (2.4 cyc) beat every other constant
source; a constant-pool `lvx` is 2.0 cyc latency but costs a load slot and an
`ld`+`li` to form the address in the current `EmitLoadPPC64VConst`; the GPR
splat is 3.5 cyc / 4 insns; `xxspltib` (3.0) is the POWER9 answer for any
byte pattern. (c) The fastest "any lane matched" is the record-form compare
feeding `bc` (1.1 cyc, no GPR); `vbpermq`+`mfvsrd` (8.9 cyc) and
`mfocrf`+`rlwinm` (14 cyc) are for when the mask value itself is needed;
`vctzlsbb` gives the first-match index in 6 cyc on POWER9. (d) Any
store→load through memory is 15-18 cycles on POWER9 regardless of width
mixing; the D/Q split is not a special partial-register penalty, the backend's
`mtvsrd`/`mfvsrd` paths (4 cyc round trip) are already right, and the only
in-loop store→load in the string workload is the JIT's own context traffic.
(e) `mfvscr`/`mtvscr` are 16-cycle serialising operations, so VSCR.SAT models
FPSR.QC only if it is read at `MRS FPSR` and never in the op stream; that is
sufficient: SAT tracks QC exactly for every saturating lowering here and is
never set by a non-saturating one (exhaustive/random, `vscr_probe.c`), and
saturating VMX ops themselves (`vaddubs`) cost 3 cyc like a compare, with no
throughput penalty. `vpmsumd`, `vcipher*` are 6-cycle pipelined ops; the
32-bit float→int conversions are 22 cycles (10 throughput) and `xscvhpdp`
is 38 cycles: half-precision conversion is the one genuinely slow instruction.

## 1. Method and evidence base

### 1.1 Parity

- `probes/neon_corpus.h`: PRNG, corpora, lane-level Arm ARM semantics
  (unbounded-integer shifts with the count taken from the low byte,
  saturation, `FPRecipEstimate`/`FPRSqrtEstimate` tables, AES steps,
  carry-less multiply, `FPProcessNaNs` order).
- `probes/neon_ref.h`: vector-level reference for 120 operations and the
  corpus driver (hash of all results + first mismatching vector).
- `probes/neon_cases.h`: the shared case table (name, width, immediate range,
  corpus kind).
- `probes/neon_golden_pi.c`: the same cases through `arm_neon.h` on the Pi.
  Result: `REFERENCE-MODEL-VERIFIED (0 failing cases)` after fixing the model
  for `SQSHL` with counts ≤ −W (sign fill, not zero), `FRECPE` denormal
  results (input exponent 253/254 give denormal outputs, not zero),
  `FRSQRTE` exponent parity (`'01':fraction` for an *odd* biased exponent) and
  the `RecipSqrtEstimate` doubling, and `FRECPS`/`FRSQRTS` negating operand 1
  *before* NaN processing and rounding `(3 − a·b)/2` once. Each of these is
  a place where a lowering written from memory of the pseudocode would have
  been wrong; the Pi caught all of them.
- `probes/neon_parity_p9.c`: 344 candidate lowerings as inline asm, plus
  idiom probes (`vbpermq` control order with a reversed-control positive
  control, `vctzlsbb`/`vclzlsbb`, `vextubrx`, the `umaxp v,v` fast form).
  Build and results: `logs/parity-p8.out`, `logs/parity-p9.out`
  (`PARITY-FAILED (24 of 343 cases failing, 0 idiom failures)`: the 24 are
  the six documenting forms × four rounding modes). The P8 build has zero
  ISA 3.0 encodings (`isa30scan.sh`: "undecodable-as-POWER8 instructions: 0");
  the P9 build uses `vpermr`, `xxbrw`, `vabsdub`, `xxspltib`.

### 1.2 Timing

`probes/neon_bench_p9.c` measures every catalogue sequence as one asm block:
latency = the block chained on itself, throughput = four independent chains
interleaved (so throughput numbers for 2-3 cycle ops are latency-bound at
0.5-0.75 cyc and mean "at least"). Time base ticks → ns → cycles at the
cpufreq clock read after a warm-up (3.80 GHz throughout; a dependent `add`
comes out at 2.01 cycles, which is the POWER9 FXU's dependent latency, and
`vaddubm` at 2.01, `vperm` 3.02, `vmladduhm` 7.04, matching UM §2.3.9's
"simple 2 / permute 2 / complex 5 / float 5 execution cycles" plus forwarding).

### 1.3 Workload

`probes/workloads/strloop.c` (static glibc, `-O2`, built on the Pi): 200000
repetitions of `strlen`/`memchr`/`strchr`/`memcmp`/`strnlen` over 4 KiB, with
a compiler barrier so the calls are not hoisted. Under POWERarm (Release,
`POWERARM_HOSTPAGEMODE=force`, pinned to CPU 112, node 0) it runs in 607 ms;
natively on the Pi the same binary takes ~2 ms per 2000 repetitions
(≈200 ms per 200000, so POWERarm is ~3× slower than the A76 on this loop
mix, the best ratio in the M1 baseline so far because the loops are
vector-bound rather than register-spill-bound). PMU (`logs/strloop-perf-stat.txt`):

| event | value |
|---|---|
| cycles / instructions | 2.17 G / 3.75 G (IPC 1.73) |
| branches / mispredicts | 584 M / 0.82 M |
| `pm_vsu_fin` | 4.06 G |
| `pm_cmplu_stall` | 767 M (35 %) of which `exec_unit` 633 M, **`fxu` 576 M**, `lsu` 133 M, `st_fwd` 2.1 M |
| `pm_flush_mpred` | 1.0 M |

The three hot guest blocks (`strchr+0x68` 22.7 %, `__memchr_generic+0x60`
21.9 %, `__strlen_asimd+0xa0` 19.3 %) were dumped from the live JIT
(`logs/strloop-jit-hostcode.txt`) and are analysed in §4.1. The FXU stall
share is the `mfvrd → cmpdi → bc` tail of every iteration.

## 2. Cross-cutting decisions

### 2(a) Register placement: VMX half vs both halves

[MEASURED] `vand` 2.01 cyc / `xxland` 2.01; `vor`/`xxlor` 2.01; `vsel`/`xxsel`
2.01; `vperm` 3.02 / `xxperm` 3.01 / `xxpermdi` 3.01; throughput identical.
A chain that keeps its value in the FPR half (`xxlor vs0,…; xxland vs0,…;
xxlor back`) costs 6.03 cyc against 2.01 for the same work in place; an
`xxlor` there-and-back is 4.02 cyc. So:

- There is no execution-side reason to prefer `v*` over `xx*` forms; the
  constraint is purely encoding: VMX-form ops (`vperm`, `vsel`, `vcmp*`,
  `vpk*`, `vmrg*`, `vsl*`, all integer arithmetic, `vcipher`, `vpmsum*`)
  address v0-v31 = vs32-vs63 only `[SPEC ISA 3.0C §6.2, VX/VA/VC forms]`.
  Only logical (`xxl*`), select, permutes (`xxperm*`, `xxsldwi`, `xxmrg*`),
  splats, byte-reverse, float (`xv*`, `xs*`) and loads/stores reach vs0-vs31.
- The current split (16 pinned + 14 dynamic + 2 temporaries, all VMX;
  `[CODE] ArchHelpers/PPC64Emitter.h:328-341`, `Registers.h:22-31`) is
  the right one *unless* the allocator learns register classes. Any scheme
  that moves a hot guest register into vs0-vs31 and pulls it back per
  integer op pays ≥ 4 cycles per touch, more than most ops cost.
- What a VSX-aware allocator could do: keep the **float**-only guest
  registers (FMLA/FADD chains, `xv*` ops reach both halves) and the
  **temporaries of VSX-only sequences** in vs0-vs31, and use the freed VMX
  registers to pin V16-V31. The census says V16-V31 are used mainly by
  compiler-scheduled loops (codecs, xxHash's `XXH3_accumulate` uses v16-v31
  heavily), which are exactly the loops with float or wide-multiply work.
  **[INFERENCE, testable]**: pinning V16-V31 pays only when the workload
  keeps > 16 vectors live across a loop; the string routines never do.
- Guest-register spills today: a context load/store round trip
  (`stxvd2x`/`lxvd2x`, `stxv`/`lxv`) is 15.2 cyc latency [MEASURED] (the
  pipeline study's 22.6 for a loop-carried GPR slot includes the `addi` and
  the back-edge redirect; same mechanism): the store-forwarding stall the
  M1 baseline found for GPRs applies to V16-V31, and the scalar-FP study
  measured it at 18 % of nbody's cycles (38 `lxvx` + 24 `stxvx` per hot
  block). That is the case for pinning V16-V31; the mechanism has to be a
  VSX-aware allocator class, not per-op moves.
- The scalar study's `mtvsrd`→`xv*` +1.7-cycle hazard (UM 25.1.5.4) applies
  to `DUP Vd, Wn` feeding a vector op; my `mtvsrd; xxpermdi; vspltb` chain
  (11.7 cyc) already includes it. Prefer `mtvsrws`/`mtvsrdd` on 3.0.

### 2(b) Masks and constants

[MEASURED] latency / throughput, cycles, of "materialise constant then use it":

| source | insns | lat | tp | ISA |
|---|---|---|---|---|
| `vspltisb`/`vspltish`/`vspltisw` (−16..15 per lane) | 1 | 2.40 | 1.16 | 2.03 |
| `xxspltib` (any byte) | 1 | 2.42 | 1.16 | 3.0 |
| pool load `li; lvx` (L1 hit) | 2 | 2.01 | 1.59 | 2.03 |
| pool load `li; lxvd2x; xxpermdi` | 3 | 2.57 | 2.35 | 2.06 |
| `lvsl` ramp `+ vslb` (the `VExtractSignBits` control) | 3 | 3.19 | 3.08 | 2.03 |
| GPR splat `li; mtvsrd; xxpermdi; vsplt*` (current `VDupFromGPR`/`VectorImm` path for bytes ≥ 0x80, `[CODE] VectorOps.cpp:112-123`) | 4 | 3.53 | 3.36 | 2.07 |
| `mtvsrws` (word splat from GPR) / `mtvsrdd` | 2 | 6.5 / 5.6 | 1.8 / 1.6 | 3.0 |

Rules that follow: (1) a splat immediate is always the first choice, and the
sign wrap makes `vspltisb` cover shift counts 0-15 *and* 48-63 for `vsld`
and 16-31 for `vslw` (the shifters read only the low log2(W) bits); (2) when
the pattern needs a GPR anyway, `xxspltib` (3.0) replaces the 4-instruction
splat for byte patterns and `mtvsrws` for word patterns; (3) the pool load is
cheapest *per use* but occupies one of the two load slots per cycle
(POWER9 UM §2.3.9: four 8-byte load buses) and the current
`EmitLoadPPC64VConst` (`[CODE] JITClass.h:1076-1083`) spends `ld base`
+ `li off` + `lvx` = 3 instructions and one extra load per constant per
emission; a JIT-level "constant already in a register" cache within a block
(the `ScalarSplatChain` pass exists for GPRs) would remove most of these; (4)
the 64-bit shift-count and `INT_MAX`/`INT_MIN` constants used by the
saturating sequences below are the main users of the GPR path on POWER8 and
of `xxspltib`+`vextsb2d`-style forms on POWER9.

### 2(c) Movemask and early exit

[MEASURED] "any lane set" and "index of first set lane" after a `vcmpequb`:

| idiom | insns | lat cyc | tp cyc | needs GPR? | ISA |
|---|---|---|---|---|---|
| `vcmpequb. ; bc` on CR6 | 2 | **1.14** | 1.25 | no | 2.03 |
| `vcmpequb. ; mfocrf; rlwinm` (`VAnyNonZero`, `[CODE] VectorOps.cpp:2576-2592`) | 4 | 14.1 | 3.9 | yes | 2.03 |
| `lvsl; vslb; vbpermq; mfvsrd` (x86 movemask, `VExtractSignBits` `:2540-2570`) | 4 (2 with a hoisted control) | 8.9 | 3.4 | yes | 2.07 |
| `vsldoi 8; mfvsrd` (`fmov x, d` = `VExtractToGPR i64 #0`, `[CODE] ALUOps.cpp:4061-4080`) | 2 | 4.0 (round trip) | 1.1 | yes | 2.07 |
| `vctzlsbb rX, v` (first lane whose LSB is set) | 1 | 6.0 | 1.7 | yes | 3.0 |
| `mfvsrld` (lane 0 of a doubleword, replaces `vsldoi`+`mfvsrd`) | 1 | ~2 | | yes | 3.0 |

Lane-order facts [MEASURED, `idiom_probes()`]: the `vbpermq` control that
yields bit *i* = lane *i* (x86/ARM order) is memory bytes
`{120,112,…,8,0}`, i.e. the `lvsl` ramp shifted left by 3 as the backend
builds it; the reversed control gives the bit-reversed mask (positive control
caught). `vctzlsbb` counts from LE lane 0 and returns 16 for "none"
(`vclzlsbb` counts from lane 15). All 2000 random masks agreed.

**Should the frontend recognise the idiom across instructions? Yes.** The
loop path of every glibc/musl vector-scan routine is
`cmeq → umaxp v,v | uminp | shrn #4 | addp → fmov x,d → cbz/cbnz` (§4.1 shows
the three hot blocks), the backend already has the fused branch
(`IROp_CondJump` with `VCmpElementSize`, "no lane mask is materialised
anywhere", `[CODE] IR.json:310-320`), and nothing sets it. The recognition
is local to a block: the `fmov` GPR and the pairwise result are dead on the
loop back-edge and live only on the exit edge, so the exit block recomputes
them from the compare (which stays live) — the same shape the x86
`pcmpeqb/pmovmskb/test/jcc` fusion in fastppcx86 used. For `shrn #4`-based
exits the exit path is `fmov; rbit; clz; lsr #2`, which on POWER9 collapses to
`vctzlsbb` (index) and on POWER8 to `vbpermq`+`cntlzd` on the byte mask; both
are exact and worth a second, smaller recognition.

### 2(d) Partial-register and store-forwarding stalls

[MEASURED] every path through memory on POWER9 costs 15-18 cycles of
latency: `stvx`→`lvx` 15.2; `std`→`lvx` (64-bit store, 128-bit load, the
"D then Q" case) 18.2; two `std`→`lvx` 17.6; `stvx`→`ld` (Q then D) 17.3;
`stxvd2x`→`lxvd2x` and `stxv`→`lxv` 15.2. So the width mix adds at most
2-3 cycles; the real cost is any store→load at all. The register paths
`mfvsrd`→`mtvsrd` (4.0 cyc), `mtvsrd; xxpermdi; vspltb` (11.7) and the
current `UMOV.B` (`vspltb; mfvsrd; srdi`: 11.1 with its return trip)
never touch memory. Consequences:

- The backend's decision to route `VCastFromGPR`, `VDupFromGPR`,
  `VLoadTwoGPRs`, `LoadPermCtrl` through `mtvsrd` (`[CODE] VectorOps.cpp:
  4442-4530, 2057-2066`) is right: the `std`+`lvx` forms it replaced would be
  15-18 cycles each.
- The remaining stack round trips are `VSRSHR` and `VSQSHL`
  (`stvx`, 8-16 scalar loads/stores, `lvx`): recommendation #2 removes them.
- Guest `D` vs `Q` views of the same register never alias through memory in
  the JIT (both are one VR), so "partial register" stalls do not exist here;
  what exists is the context spill of V16-V31 (2(a)). This is the vector
  data the pipeline study listed as missing: mixing `std`/`ld` with
  `lvx`/`stvx` on the same 16 bytes adds 2-3 cycles to the 15-cycle
  forward, and never a flush (`pm_lsu_flush_lhs` = 0 in the workload).
- In the string workload `pm_cmplu_stall_st_fwd` is 2.1 M of 2.17 G cycles:
  the vector loops have no forwarding problem; `pm_cmplu_stall_fxu` (576 M)
  is where the time goes, i.e. the VSU→GPR→compare→branch tail.

### 2(e) Serialisation and latency hazards

[MEASURED] `mfvscr` 16.1 cyc and `mtvscr` 16.1 cyc, both with throughput
equal to latency (fully serialising); `vaddubs` (which sets VSCR.SAT
`[SPEC ISA 3.0C p.270]`) 3.0 cyc latency / 0.75 tp, i.e. the same class as
a compare, no serialisation on the write. `vpmsumd`/`vpmsumb` 6.0,
`vcipher`/`vcipherlast`/`vncipher` 6.0, `vshasigmaw` 3.0, `vmsumubm`/
`vmladduhm`/`vsum4ubs`/`vsumsws` 7.0 (the "complex" pipe), `xvcvspsxws`/
`xvcvsxwsp` 22 (10 tp), `xvdivsp` 26, `xvsqrtsp` 29, `xscvhpdp` 38 (13 tp),
`xvcvsphp` 22 (11 tp). `xvcmpeqsp.` (record form) costs exactly what
`xvcmpeqsp` costs (3.0), so CR6-gated fixups are free on the fast path.

FPSR.QC: VSCR.SAT is sticky like QC and is set by exactly the VMX
saturating families NEON also saturates in (`vadd*s`, `vsub*s`, `vpk*s*`,
`vmhaddshs`/`vmhraddshs`, `vsum*s`, `vctsxs`/`vctuxs`). Mapping QC → SAT
works if (1) `MRS FPSR` reads it with `mfvscr` (16 cyc, rare), (2)
`MSR FPSR` writes it with `mtvscr` (16 cyc, rare), and (3) the JIT never
uses a saturating VMX op for a *non*-saturating guest op where it could
saturate: `ADDV.16B/8H` via `vsum4ubs/vsumsws` cannot (§3.10), the SSE-era
`VAddV i32` via `vsumsws` can (and is wrong anyway), `vpk*ss` is used only
for `SQXTN`-class ops. **[MEASURED, `probes/vscr_probe.c`, `logs/vscr-probe.out`]**: `vaddubs`
and `vaddsbs` set SAT exactly when ARM sets QC (all 4096 exhaustive byte
vector pairs, 2160/1144 saturating); `vsum4ubs`/`vsum4sbs`/`vsum4shs` +
`vsumsws` (the `ADDV.16B/8H`, `SADDLV` forms) never set SAT in 200000
random vectors nor on the all-0xFF / all-0x8000 worst cases; the modulo
packs never set it; `vmhaddshs`/`vmhraddshs` set it exactly for the
`0x8000×0x8000` lanes where `SQDMULH` sets QC. The one offender is the
`VAddV i32` bug (`vsumsws` on four words: SAT set in 40 % of random inputs,
QC must stay 0), fixed by #6. So SAT can carry QC with zero per-op cost:
reading it per op would cost 19× (`vaddubs` 0.11 → 2.16 time-base ticks with
a trailing `mfvscr`).

Pipeline claims for the coordinator's question, stated as testable:
- **Claim P1**: a `vcmpequb.`+`bc` loop exit does not flush when predicted;
  `pm_flush_mpred` stays at the guest-branch mispredict count. Supported by
  1.14 cyc latency and 1.25 tp in the microbench (a flush would be ≥ 20).
- **Claim P2**: the VSU→FXU crossing is 4 cycles when the consumer is
  `mtvsrd`, but a `cmpdi` consumer waits for `mfvsrd` completion plus the
  FXU's 2-cycle latency; the 576 M `pm_cmplu_stall_fxu` cycles in the
  string workload (27 % of its cycles) are that chain. Test: after #1 the
  FXU stall share should fall below 5 %.
- **Claim P3**: no fusion of VMX pairs was observed (`vspltisw`+`vsldoi`
  sequences cost the sum of their parts: `vmov64_cur3` 6.03 = 3 × 2.01),
  so instruction count and dependency depth are the whole story for VMX
  code; independent instructions overlap freely (throughput ≈ latency/4
  with four chains for every simple op).

## 3. The catalogue

Notation: `Z` = a zero register (`vspltisw Z,0`, 1 insn, usually hoisted),
`K(n)` = a shift-count splat (1 insn via `vspltis*` when representable, see
2(b)), `C(x)` = a constant needing the GPR or pool path (3-4 insns on P8,
1-2 on P9). Counts exclude `Z` and constants unless stated. "cyc" = measured
latency at 3.8 GHz; "parity" = the row in `logs/parity-p9.out`. Lane facts
used throughout (all [MEASURED] here and in the handbook): guest lane *i* is
ISA byte 15−*i*; `vpk*um(A,B)` puts B's lanes in the low half; `vmrgl*(A,B)`
= `{b0,a0,b1,a1,…}`; `vupkl*` widens the low half; `vmule*` multiplies the
LE *odd* lanes, `vmulo*` the LE *even* lanes; `vmrgow(A,B)` = `{b0,a0,b2,a2}`,
`vmrgew(A,B)` = `{b1,a1,b3,a3}`; `vsldoi(A,B,n)` = `(A‖B) << 8n` bytes in ISA
order, so `EXT #n` = `vsldoi(B,A,16−n)`; `vperm` control byte *k* selects
ISA byte *k* of `VRA‖VRB`, `vpermr` selects LE byte *k* of `VRB‖VRA`.

### 3.1 Arithmetic, compare, bitwise, BSL

| ARM op | sizes | POWER8 | P9/P10 | insns | parity | cyc |
|---|---|---|---|---|---|---|
| `ADD`/`SUB` | 8-64 | `vaddu*m`/`vsubu*m` `[p.268,275]` | same | 1 | (trivial; current) | 2.0 |
| `CMEQ` | 8-64 | `vcmpequ*` `[p.303]` | same | 1 | current | 3.0 |
| `CMGT` | 8-64 | `vcmpgts*` | same | 1 | current | 3.0 |
| `CMGE` | 8-64 | `vcmpgts*(b,a); vnor` | same | 2 | OK ×2 | 5.0 |
| **`CMHI`** | 8-64 | **`vcmpgtu*(a,b)`** `[p.305]` | same | **1** (was 3) | OK ×4 | 3.0 (was 8.0) |
| `CMHS` | 8-64 | `vcmpgtu*(b,a); vnor` | same | 2 (same count as now, shorter chain) | OK ×4 | 5.0 |
| **`CMTST`** | 8-64 | **`vand; vcmpgtu*(t,Z)`** | 3.0 `vand; vcmpne{b,h,w}(t,Z)`: same count | **2**+Z (was 3+Z) | OK ×4 | 5.1 (was 7.1) |
| `CMEQ/CMGT/CMLT/CMGE/CMLE #0` | 8-64 | compare vs `Z` (+`vnor` for GE/LE) | 3.0: `vcmpnez*`? no gain | 1-2 | current | 3-5 |
| `AND/BIC/ORR/ORN/EOR/NOT` | — | `vand/vandc/vor/vorc/vxor/vnor` | `xxl*` reach both halves | 1 | current | 2.0 |
| `BSL/BIT/BIF` | — | `vsel` with the mask as VRC `[p.259]` | `xxsel` | 1 | current | 2.0 |
| `NEG` | 8-64 | `vsubu*m(Z,a)` | 3.0 `vnegw/vnegd` `[p.291]` | 1+Z / 1 | OK | 2.0 |
| `ABS` | 8-64 | `vsubu*m(Z,a); vmaxs*` | same | 2+Z | OK | 5.0 |
| `MUL` | 16 | `vmladduhm a,b,Z` `[p.284]` | same | 1+Z | OK | 7.0 |
| `MUL` | 32 | `vmuluwm` `[p.282]` | same | 1 | OK | 7.0 |
| `MUL` | 8 | `vmulesb; vmulosb; vslh 8; vsel(mask 0x00FF)` | same | 5 (+K, C) | OK (exhaustive) | 11.6 |
| `MUL` | 64 | `vrld a,32; vrld b,32; vmulouw ×3; vaddudm; vsld 32; vaddudm` | 3.1 `vmulld` (1) | 8 (+C(32)) | OK | 15.1 |
| `MLA`/`MLS` | 16 | `vmladduhm a,b,acc` / `vmladduhm a,b,Z; vsubuhm` | same | 1 / 2 | OK | 7 / 9 |
| `MLA`/`MLS` | 32 | `vmuluwm; vadduwm`/`vsubuwm` | same | 2 | OK | 9 |
| `MLA` | 8 | MUL.8B + `vaddubm` | | 6 | OK | |

### 3.2 Shifts

Shift-by-register semantics (Arm ARM `SSHL`/`USHL`): count = signed low
byte of each lane; ≥ 0 shifts left (≥ W gives 0), < 0 shifts right by the
magnitude (≥ W gives 0 for unsigned, sign fill for signed; rounding forms give
0 for ≥ W+1 and, for unsigned counts of exactly W, bit W−1 of the input).
POWER shifters take the count modulo W `[p.315-317]`, so every over-width case
needs a mask or a clamp.

| ARM op | sizes | POWER8 sequence | insns | parity | cyc |
|---|---|---|---|---|---|
| `SHL #n` | 8-64 | `K(n); vsl*` | 2 | OK ×4 | 2-4 |
| `SSHR/USHR #n` | 8-64 | `K(n); vsra*/vsr*` (`#W` for signed = `K(W−1)`, unsigned = `Z`) | 2 | OK ×8 | 2-4 |
| **`SRSHR/URSHR #n`** | 8-64 | `vsra n; vsra n−1; vand 1; vadd` (identity `(x>>n)+((x>>(n−1))&1)`, no overflow) | 5 with K,K,1 | OK ×8 | 7.5 (was ~60 insns + stack) |
| `SSRA/USRA` | 8-64 | shift + `vadd` (current `VUShraI`) | 3 | current | |
| **`SQSHL #n`** | 8-64 | `vsl; vsra back; vcmpequ; sat=(a>>s W−1)^MAX; vsel` | 8 (+C(MAX)) | OK ×4 | 9.9 (was ~100 insns + branches) |
| `UQSHL #n` | 8-64 | `vsl; vsr back; vcmpequ; vsel(ones)` | 5 | OK ×4 | ~7 |
| `SQSHLU #n` | 8-64 | UQSHL form + `vcmpgts(Z,a); vandc` | 7 | OK ×4 | ~9 |
| **`SSHL/USHL` (reg)** | 8-64 | sign-extend count byte (`vsl W−8; vsra W−8`, 0 for W=8); `neg=Z−cnt`; `sl=vsl(a,cnt)`; signed: `sr=vsra(a, vminu(neg,W−1))`, unsigned: `sr=vsr(a,neg)`; `lm=(W >u cnt)`, `rm=(W >u neg)`; `sg=(0 >s cnt)`; `vsel(sl&lm, sr&rm, sg)` | 11-13 | OK ×8 | 9.2 |
| `SRSHL/URSHL` (reg) | 8-64 | the above + rounding bit `((a>>(neg−1))&1)` + the `neg==W` case | 17-19 | OK ×8 | ~13 |
| `SQSHL/UQSHL` (reg) | 8-64 | `SSHL` form + saturation test `((a<<c)>>c)!=a` restricted to `0≤c<W`, and `c≥W && a≠0` | ~24 | OK ×8 | ~16 |
| `SQRSHL/UQRSHL` | | not probed (compose the two above) | | SKIP | |
| **`SHRN #n`** | 16/32/64→ | `K(n); vsr*; vpk*um(Z, t)` (current, `[CODE] VectorOps.cpp:925-1000`) | 3+Z | OK ×3 | 5.3 |
| `RSHRN #n` | | as `SHRN` with the rounding identity at 2W | 6 | OK ×3 | ~8 |
| `SQSHRN / SQRSHRN` | | `vsra*; vpk*ss(Z,t)` `[p.247]` / + rounding | 3 / 6 | OK ×6 | ~5 / ~8 |
| `UQSHRN / UQRSHRN` | | `vsr*; vpk*us(Z,t)` (unsigned→unsigned: `vpkuhus/vpkuwus/vpkudus`) | 3 / 6 | OK ×6 | |
| `SQSHRUN / SQRSHRUN` | | `vsra*; vpkshus/vpkswus/vpksdus(Z,t)` | 3 / 6 | OK ×6 | |

The 64-bit lane count constants (16..47) are the POWER8 weak spot: use
`xxspltib` on ISA 3.0 (2(b)).

### 3.3 Widening, narrowing, long

| ARM op | POWER8 | insns | parity | cyc |
|---|---|---|---|---|
| `SXTL`/`SXTL2` | `vupkls*`/`vupkhs*` `[p.252]` | 1 | OK ×6 | 3 |
| `UXTL`/`UXTL2` | `vmrgl*(Z,a)`/`vmrgh*(Z,a)` `[p.253]` | 1+Z | OK ×6 | 3 |
| `XTN` | `vpk*um(Z,a)` (modulo pack) | 1+Z | OK ×3 | 3 |
| `SQXTN`/`UQXTN`/`SQXTUN` | `vpk*ss`/`vpk*us`(unsigned in)/`vpk*us`(signed in) with `Z` first | 1+Z | OK ×9 | 3 |
| `SADDL/UADDL/SSUBL/USUBL` | widen both, add | 3 | OK ×5 | 5-8 |
| `SADDW/UADDW` | widen one, add | 2 | current | |
| `ADDHN/SUBHN` | `vadd 2W; K(W); vsr; vpk*um(Z,t)` | 4 (+K) | OK ×5 | ~8 |
| `RADDHN` | + `vadd C(1<<(W−1))` | 5 | OK ×3 | |
| `SMULL/UMULL` (low) | `vmules*; vmulos*; vmrgl*(e,o)` (`xxmrgld` for 32→64) | 3 | OK ×6 | 10.6 |
| `SMULL2/UMULL2` | same with `vmrgh*`/`xxmrghd` | 3 | OK ×5 | 10.6 |
| `SMLAL/UMLAL/…` | + `vadd` at 2W | 4 | (composition) | |
| `SQDMULL` 16→32 | `vmulesh; vmulosh; vmrglw; vaddsws t,t` | 4 | OK | ~13 |
| `SQDMULL` 32→64 | `vmulesw; vmulosw; xxmrgld; vaddudm t,t;` + INT_MIN² fixup (`vcmpequw ×2, vand, vmrglw, vandc, vand, vor`) | 11 (+C) | OK | |
| `SABDL/UABDL` | ABD then widen (`vmrgl*(Z,·)`); signed: widen first then max−min at 2W | 4-5 | OK ×5 | |
| `SADDLP/UADDLP` 16→32 | `vmsumshm(a, ones16, Z)` / `vmsumuhm` `[p.285-286]` | 1 (+C(1)) | OK ×2 | 7 |
| `SADDLP/UADDLP` 8→16 | `vmulesb(a,1); vmulosb(a,1); vadduhm` | 3 (+K) | OK ×2 | ~12 |
| `SADDLP/UADDLP` 32→64 | `vupklsw; vupkhsw; xxmrgld; xxmrghd; vaddudm` | 5 | OK ×2 | |
| `SADALP/UADALP` | + `vadd` (the 16→32 form folds the accumulator into `vmsum*`'s VRC: 1 insn) | 1-4 | OK ×4 | |

### 3.4 Saturating arithmetic (incl. 64-bit lanes)

| ARM op | POWER8 | insns | parity | cyc |
|---|---|---|---|---|
| `SQADD/UQADD/SQSUB/UQSUB` 8/16/32 | `vadd*s`/`vsub*s` (sets VSCR.SAT, 2(e)) | 1 | OK ×12 (exhaustive at 8) | 3.0 |
| **`UQADD.2D`** | `vaddudm; vcmpgtud(a,sum); vor` | 3 | OK | 7.4 |
| **`UQSUB.2D`** | `vsubudm; vcmpgtud(b,a); vandc` | 3 | OK | ~7 |
| **`SQADD.2D`** | `vaddudm; vxor(a,b); vxor(a,s); vandc; vsrad 63; sat=vsrad(a,63)^MAX; vsel` | 9 (+C(0xFF), C(MAX)) | OK | 10.5 |
| **`SQSUB.2D`** | as `SQADD` with `vand` for the overflow test | 9 | OK | |
| `SQNEG` 8-32 / `.2D` | `vsub*s(Z,a)` / `vsubudm; vand(a,d); vsrad 63; sat=vsrad(a,63)^MIN; vsel` | 1 / 6 | OK ×3 | |
| `SQABS` 8-32 / `.2D` | `SQNEG` + `vmaxs*` | 2 / 7 | OK ×3 | |
| `SUQADD/USQADD` | not probed (rare); compose from the mixed-sign compare | | SKIP | |

### 3.5 `SQDMULH`/`SQRDMULH`

| ARM op | POWER8 | insns | parity | cyc |
|---|---|---|---|---|
| **`SQDMULH.8H`** | `vmhaddshs a,b,Z` `[p.283]` (`(a×b)>>15`, saturating) | 1+Z | OK | 7.0 |
| **`SQRDMULH.8H`** | `vmhraddshs a,b,Z` (adds 0x4000 before the shift) | 1+Z | OK | 7.0 |
| `SQDMULH.4S` | `vmulesw; vmulosw; vsrad 31 ×2; vmrgow; sat: vcmpequw(a,MIN) & vcmpequw(b,MIN); vxor` | 9 (+C(31), C(MIN)) | OK | 14.2 |
| `SQRDMULH.4S` | + `vaddudm C(1<<30)` ×2 before the shifts | 11 | OK | |

The WebAssembly port in the handbook uses the same `vmhraddshs` for
`q15mulr_sat_s` and reported no NaN-style traps; this probe adds the
`0x8000×0x8000 → 0x7FFF` and full random confirmation.

### 3.6 `ABD`/`ABA`

| ARM op | POWER8 | POWER9 | insns | parity | cyc |
|---|---|---|---|---|---|
| `UABD` 8/16/32 | `vmaxu; vminu; vsubu*m` | **`vabsdu*`** `[p.296]` | 3 / **1** | OK ×3 both builds | 5.7 / 3.0 |
| `SABD` 8/16/32 | `vmaxs; vmins; vsubu*m` (the difference wraps exactly as ARM's) | same | 3 | OK ×3 | 5.7 |
| `UABA/SABA` | + `vaddu*m` | | 4 / 2 | (composition) | |
| `UABAL/SABAL` | `UABDL`/`SABDL` + add | | 5-6 | | |

### 3.7 `CNT`/`CLZ`/`CLS`/`RBIT`

| ARM op | POWER8 | insns | parity | cyc |
|---|---|---|---|---|
| `CNT` | `vpopcntb` `[p.344]` | 1 | OK | 2.0 |
| `CLZ` 8/16/32 | `vclzb/h/w` `[p.339]` | 1 | OK ×3 | 3.0 |
| `CLS` 8/16/32 | `vsra 1; vxor; vclz; vsub 1` | 4 (+K) | OK ×3 | ~9 |
| `RBIT` | nibble table: `vsrb 4; vand 0x0F; vperm ×2 (16-entry reversal table); vslb 4; vor` | 6 (+C(table), K, C) | OK | 10.3 |
| `RBIT` alt | three mask-select rounds (`vsl`/`vsr`/`vsel` with 0x55/0x33/0x0F) as SIMDe | 9 (+3 C) | not probed | |

### 3.8 `ZIP`/`UZP`/`TRN`/`EXT`/`REV`

| ARM op | sizes | POWER8 | POWER9 | insns | parity | cyc |
|---|---|---|---|---|---|---|
| `ZIP1`/`ZIP2` | 8-32 / 64 | `vmrgl*(b,a)` / `vmrgh*(b,a)`; 64: `xxmrgld/xxmrghd(b,a)` | same | 1 | OK ×8 | 3.0 |
| `UZP1` | 8-32 / 64 | `vpk*um(b,a)`; 64: `xxmrgld` | same | 1 | OK ×4 | 3.0 |
| `UZP2` | 8/16 | `K(W); vsr 2W ×2; vpk*um` | `xxperm` + C | 3+K / 1+C | OK ×2 | 5.1 |
| `UZP2` | 32 | `vrld 32 ×2; vpkudum` | `xxperm` + C | 3+C(32) | OK | |
| `UZP2` | 64 | `xxmrghd(b,a)` | | 1 | OK | |
| `TRN1`/`TRN2` | 32 | `vmrgow(b,a)` / `vmrgew(b,a)` `[p.255]` | | 1 | OK ×2 | 3.0 |
| `TRN1`/`TRN2` | 8/16 | mask/shift/or: `vand m; vsl W; vand; vor` | `xxperm` + C | 4 (+C,K) | OK ×4 | ~7 |
| `TRN1`/`TRN2` | 64 | `xxmrgld` / `xxmrghd` | | 1 | OK ×2 | |
| `EXT #n` | | `vsldoi(b,a,16−n)` (n=0: move) `[p.261]` (current `VExtr`) | | 1 | OK ×16 (all n) | 3.0 |
| `REV64` | 8/16/32 | `vrld 32` (+`vrlw 16` +`vrlh 8`) | `xxbrd` (1) | 1-3 (+K) | OK ×3 | 2-6 |
| `REV32` | 8/16 | `vrlw 16` (+`vrlh 8`) (current) | `xxbrw` | 1-2 | OK ×2 | 4.8 |
| `REV16` | 8 | `vrlh 8` | `xxbrh` | 1 | OK | 2 |

### 3.9 `TBL`/`TBX`

| op | POWER8 (`vperm`, control = idx^0x0F) | POWER9 (`vpermr`) | insns P8/P9 | parity | cyc P8/P9 |
|---|---|---|---|---|---|
| `TBL1` | `vspltisb 15; vcmpgtub m,idx,15; vxor i,idx,15; vperm t,t,i; vandc` (current `VTBL1`, `[CODE] VectorOps.cpp:3570-3595`) | **`xxspltib 31; vminub i,idx,31; vpermr r,Z,t,i`** (indices 16-31 select the zero register; ≥32 clamp to 31) — or `vspltisb 15; vcmpgtub; vpermr t,t,idx; vandc` | 6 / **3** | OK ×4 both | 5.5 / **3.0** |
| `TBL2` | `C(31); vcmpgtub; vspltisb 15; vxor; vperm t1,t2,i; vandc` | `C(31); vcmpgtub; vpermr r,t2,t1,idx; vandc` | 6 / 4 | OK | |
| `TBL3`/`TBL4` | two 32-byte lookups + `vcmpgtub idx,31` select + `vcmpgtub idx,16n−1` zeroing: `vperm ×2; vsel; vandc` + masks | `vpermr ×2; vsel; vandc` + masks | 10 / 8 | OK | |
| `TBX1-4` | `TBL` result + `vsel(res, dst, oob)` | same | +1 | OK ×4 | |

The `xxspltib 31; vminub; vpermr(Z, t)` form is the one to adopt on ISA 3.0:
no XOR, no separate out-of-range mask, exact for all 256 index values. It
answers the "no pshufb on POWER" gap: POWER *has* it, in `vperm`, only the
index direction (ISA byte numbering) differs, and `vpermr` fixes the
direction in hardware.

### 3.10 `ADDP`/`ADDV`/`UMAXV`/`UMINV`/`SADDLV`

| ARM op | POWER8 | insns | parity | cyc |
|---|---|---|---|---|
| `ADDP/UMAXP/UMINP/SMAXP/SMINP` 8/16/32 (`a≠b`) | even = `vpk*um(b,a)`; odd = `K(W); vsr 2W ×2; vpk*um`; op | 6 (+K) | OK ×14 | 10.3 |
| **same with `a==b`** | `K(W); vsr 2W; op(a,t); vpk*um(t,t)` | 4 | idiom probe OK | 8.0 |
| `ADDP.2D` | `xxmrgld; xxmrghd; vaddudm` | 3 | OK | |
| `ADDV.16B` / `.8H` | `vsum4ubs/vsum4shs(a,Z); vsumsws(t,Z)`; result in lane 0; mask to W | 2+Z (+mask) | OK ×2 | 14.1 |
| **`ADDV.4S`** | `vsldoi 8; vadduwm; vsldoi 4; vadduwm` (never `vsumsws`: it saturates) | 4 | OK | 10.1 |
| `SADDLV/UADDLV` 8→16 | `vsum4sbs/vsum4ubs; vsumsws` (fits: 16×127 < 2^31) | 2+Z | OK ×2 | |
| `SADDLV` 16→32 | `vsum4shs; vsumsws` (8×32767 < 2^31: no saturation possible) | 2+Z | OK | |
| `UADDLV` 16→32 | zero-extend halves (`vmrglh/vmrghh` with Z), `vadduwm`, `vsumsws` | 4+Z | OK | |
| `SADDLV/UADDLV` 32→64 | widen (`vupkl/hsw` or merge-Z), `vaddudm`, rotate-add | 5-6 | OK ×2 | |
| `UMAXV/UMINV/SMAXV/SMINV` 8/16/32 | log2(N) × (`vsldoi`; `vmax*`) then place (`vsldoi ×2` with Z) | 10 / 8 / 6 | OK ×12 | 27.2 (16B) |

Why `ADDV.16B` may keep the saturating sums: `vsum4ubs` adds four unsigned
bytes into a 32-bit word (max 1020 `[p.290]`), `vsumsws` adds four such
words (max 4080) plus VRB's word (zero): both are far below 2^31, so no
saturation can occur and the low 8 bits are the exact modular sum; for `.8H`
the signed halfword sums are within ±131068 and ±524272; the wrapped low 16
bits equal the unsigned modular sum. `ADDV.4S` breaks the bound (four
words), hence the rotate-add.

`UMAXV.16B` at 27 cycles is the slowest common reduction; there is no
single-instruction max reduction on POWER. In the string routines it is not
used (0 in the census); glibc uses `umaxp v,v` + `fmov`, which #1/#5 cover.

### 3.11 `MOVI`/`DUP`/`INS`/`UMOV`

| ARM op | POWER8 | POWER9 | insns P8/P9 | cyc |
|---|---|---|---|---|
| `MOVI` imm in −16..15 at any width, or replicated byte 0x00-0x0F/0xF0-0xFF | `vspltisb/h/w` | same | 1 | 2.4 |
| `MOVI` any byte pattern | `li; mtvsrd; xxpermdi; vspltb` (current `VectorImm`, `[CODE] VectorOps.cpp:112-123`) or pool `lvx` | **`xxspltib`** `[p.781]` | 4 / 1 | 3.5 / 2.4 |
| `MOVI` word/dword pattern (e.g. `0x0000FFFF`, `0x00FF00FF`, `MOVI.2D` masks) | pool load (2-3) or `li/lis/ori; mtvsrd; xxpermdi; vspltw` (4-5) | `li…; mtvsrws` (2) / `mtvsrdd r,r` (2) | 2-5 / 2 | 2.0 / 5.6-6.5 |
| `MVNI`, `BIC/ORR` immediate | as `MOVI` + `vnor`/`vandc`/`vor` | | +1 | |
| `DUP Vd, Wn` 8/16 | `mtvsrd; xxpermdi; vsplt*` (current `VDupFromGPR`) | `mtvsrws; vsplt*` or `mtvsrd; vspltb` | 3 / 2 | 11.7 / ~9 |
| `DUP Vd, Wn` 32 / `Xn` 64 | same | **`mtvsrws`** / **`mtvsrdd r,r`** | 3 / 1 | 11.7 / 6.5, 5.6 |
| `DUP Vd, Vn[i]` | `vsplt*` (`xxspltd` for 64) | | 1 | 3.0 |
| `UMOV Wd, Vn.B[i]` | `vspltb; mfvsrd; srdi` (current) | **`li i; vextubrx`** `[p.342]` (`vextuhrx/vextuwrx` for H/S) | 3 / 2 | 11.1 / 6.0 |
| `UMOV Xd, Vn.D[0]` (`fmov x,d`) | `vsldoi 8; mfvsrd` | **`mfvsrld`** `[p.110]` | 2 / 1 | 4.0 / ~2 |
| `INS Vd.B[i], Wn` | `mtvsrd` + `vperm`/`vsel` with a constant (current `VInsGPR`, `[CODE] VectorOps.cpp:3187`) | **`mtvsrd; vinsertb Vd,t,15−i`** `[p.266]` (`vinserth/w/d`) | 4-5 / 2 | / 8.7 |
| `INS Vd[i], Vn[j]` | `vsplt* t,Vn,j` + mask select | `vsplt*; vinsert*` | 3-4 / 2 | |

The `vextubrx` index is the LE lane index directly (probed for all 16 lanes);
`vinsertb`'s immediate is the ISA byte, 15−lane.

### 3.12 Float lanes

| ARM op | POWER8 | notes | insns | parity | cyc |
|---|---|---|---|---|---|
| `FADD/FSUB/FMUL/FDIV/FSQRT` | `xvaddsp…` | NaN precedence: POWER returns operand A's NaN, ARM the first sNaN then first qNaN (`TranslateFP.cpp:9-18`); gate a fixup as in #8 | 1 (+3 gated) | | 6.3 / 26 / 29 |
| **`FMLA`** | `xvmaddasp T,A,B` `[p.711]` + `xvcmpeqsp. r,r; bc` to the NaN fixup | exact in RN/RU/RD/RZ (`fmla.* ` rows OK) | 1 (+2) | OK ×8 | 6.3 |
| **`FMLS`** | **`xvnegsp A'; xvmaddasp T,A',B`** + gate | `xvnmsubasp` is *not* exact: `fmls_nv.ru/rd` rows differ by 1 ulp and NaN signs differ in every mode | 2 (+2) | OK ×8 | 6.3 |
| `FMAX/FMIN` | `xvmaxsp` + NaN fixup (`vnor`/`vor qbit`/`vsel` chain: a's NaN wins, sNaN before qNaN, all quieted) | `xvmaxsp` alone returns the non-NaN operand (`fmax_naive` row) | 1 (+~12) | OK ×3 | 3.0 (+) |
| `FMAXNM/FMINNM` | `xvmaxsp` + sNaN/both-qNaN fixup only | | 1 (+~10) | OK ×3 | 3.0 (+) |
| `FRINTN` | `vrfin` (ties-to-even, handbook) / `xvrdpic` in RN | | 1 | OK ×2 | 6.4 |
| `FRINTA` | `xvrspi`/`xvrdpi` (ties away) `[p.753]` | | 1 | OK ×2 | 6.4 |
| `FRINTM/P/Z` | `xvrspim/ip/iz`, `xvrdpim/ip/iz` | | 1 | OK ×6 | 6.4 |
| `FRINTX/I` | `xvrspic`/`xvrdpic` (current mode) | not probed separately (same instruction class) | 1 | | |
| **`FCVTZS`** | `xvcvspsxws; xvcmpeqsp m,a,a; xxland` (NaN → 0; ±overflow already saturate) | | 3 | OK ×2 | 9.5 |
| `FCVTZU` | `xvcvspuxws` (NaN → 0 already) | | 1 | OK ×2 | 22 |
| `FCVTNS/AS/MS/PS…` | round (`vrfin`/`xvrspi`/`…`) then `FCVTZS` | | 4 | (composition) | |
| `SCVTF/UCVTF` | `xvcvsxwsp`/`xvcvuxwsp`, `xvcvsxddp`/`xvcvuxddp` | exact in all four modes (`scvtf.*`, `ucvtf.*` rows) | 1 | OK ×8 | 22 |
| **`FRECPE`** | exact table: scaled = 256+frac<22:15>, est = ((2^19/(2·scaled+1))+1)>>1, exponent 253−exp, denormal outputs for exp 253/254, `|x|<2^-128 → ±inf` | the C model (verified on the Pi) is the specification; a vector lowering needs an integer division per lane (`xvdivdp` on doubles is exact here) or a 256-entry lookup; **the `xvresp` estimate is not bit-exact and must not be used** | ~20 | model OK; vector form not built | |
| **`FRSQRTE`** | exact table with the `'01':fraction<22:16>` case for odd biased exponents and `RecipSqrtEstimate`'s loop | same remark | ~24 | model OK | |
| `FRECPS` | `xvnegsp; xvmaddasp 2.0` + 2-operand NaN fixup + `inf×0 → 2.0` | operand 1 is negated *before* NaN processing (sign of a NaN result flips) | 2 (+~14) | OK | |
| **`FRSQRTS`** | `xvnegsp; halve a (or b if a<2·FLT_MIN); xvmaddasp 1.5` + fixup | `(3−a·b)/2` is rounded **once**: `xvnmsubasp 3; xvmulsp 0.5` is wrong at overflow (`frsqrts_nv` row) | 6 (+~14) | OK | |
| `FCVTL/FCVTN` half↔single | `xvcvhpsp`/`xvcvsphp` (3.0) | 22 cycles each; POWER8 needs software | 1 + unpack | current (`A64FToF`) | 22 |

### 3.13 `LD2/LD3/LD4`, `ST2/ST3/ST4`

Not probed (not translated yet, `M1b-SIMD-SUBSET.md`); the building blocks
are all probed above. `[INFERENCE]` from the permute table:

| op | POWER8 | insns |
|---|---|---|
| `LD2 {.16B}` | 2 loads; `vpkuhum(b,a)` (even bytes); `vsrh 8 ×2; vpkuhum` (odd) | 2 + 6 |
| `LD2 {.8H}` | 2 loads; `vpkuwum`; `vsrw 16 ×2; vpkuwum` | 2 + 6 |
| `LD2 {.4S}` | 2 loads; `UZP1.4S` = `vpkudum(b,a)`, `UZP2.4S` = `vrld 32 ×2; vpkudum` | 2 + 4 |
| `LD3/LD4 {.16B}` | 3-4 loads; per output `vperm ×2 + vsel` (or `xxperm`) with constants; POWER9 `vpermr` with the LE indices directly | 3-4 + 9-12 (+ 6-8 constants) |
| `ST2 {.16B}` | `vmrglb(b,a); vmrghb(b,a)`; 2 stores | 2 + 2 |
| `ST4 {.16B}` | two merge levels (`vmrgl/hb` ×4, `vmrgl/hh` ×4); 4 stores | 8 + 4 |

xxHash's `XXH3` loop is the concrete case in hand: it interleaves with
`ld2`/`st2` 24 times and needs `umlal`/`umlal2` (`SMULL`-class, §3.3),
`ext #8` (`vsldoi 8`), `uzp1/uzp2 .4S` (§3.8), `shrn #32`/`xtn` from `.2D`
(`vpkudum`), 178 `eor` and 37 `ushr`: every piece is a 1-3 instruction row
above.

### 3.14 `SDOT`/`UDOT` (and `i8mm`)

| ARM op | POWER8 | insns | parity | cyc |
|---|---|---|---|---|
| **`UDOT`** | `vmsumubm a,b,acc` `[p.284]` | 1 | OK | 7.0 |
| `USDOT` (i8mm) | `vmsummbm a_s?`: `vmsummbm` is signed(VRA)×unsigned(VRB); `USDOT` is unsigned×signed → `vmsummbm b,a,acc` | 1 | (by definition; not in the Pi's ISA) | 7.0 |
| **`SDOT`** | `vxor(b,0x80); vmsummbm(a,b',acc); vmsummbm(a,ones,Z); vslw 7; vsubuwm` | 6 (+C(0x80), K(1), K(7)) | OK | 12.2 |
| `SUDOT`, `SMMLA/UMMLA/USMMLA` | compositions of the above with `xxpermdi` row broadcasts | | | |

### 3.15 `PMULL`, AES, SHA

| ARM op | POWER8 | insns | parity | cyc |
|---|---|---|---|---|
| **`PMULL`** (low D) | `xxmrgld t,Z,a; vpmsumd t,b` `[p.335]` | 2 | OK | 9.1 |
| **`PMULL2`** | `xxmrghd t,a,Z; vpmsumd t,b` | 2 | OK | 9.1 |
| `PMULL.8B`/`PMULL2.8B` | `vmrglb/vmrghb (Z,·) ×2; vpmsumb` | 3 | OK ×2 | |
| **`AESE`** | `vxor; vcipherlast t,Z; vsldoi t,t,4` `[p.332]` | 3 | OK | 11.1 |
| **`AESD`** | `vxor; vncipherlast t,Z; vsldoi t,t,12` `[p.333]` | 3 | OK | |
| **`AESMC`** | `REV32(x); vncipherlast t,Z; vcipher t,Z; REV32` (`xxbrw` on 3.0) | 4 / 6 | OK | |
| **`AESIMC`** | `REV32; vcipherlast t,Z; vncipher t,Z; REV32` | 4 / 6 | OK | |
| **`AESE`+`AESMC` (one round)** | with the state kept fully byte-reversed across rounds (`xxbrq`/`vperm` once at entry and exit, keys reversed once): `vxor; vcipher` per round | 2 per round | (algebra §4.2; each part probed) | 8.0 |
| `SHA1C/M/P/H/SU1`, `SHA256H/H2/SU0/SU1` | existing IR ops `VSha1*`, `VSha256*` (`[CODE] VectorOps.cpp:5764…`) | not re-probed here; `vshasigmaw` is 3.0 cyc | | (fastppcx86 x86 SHA-NI parity) | |

## 4. Idiom-level wins

### 4.1 The vector-scan loop (glibc/musl `strlen`/`memchr`/`strchr`/`strchrnul`/`memrchr`/`strcpy`)

The three hot blocks under POWERarm, guest → host (`logs/strloop-jit-hostcode.txt`):

`__memchr_generic+0x60` (21.9 % of cycles), 5 guest instructions:
`ldr q1,[x3,#32]!; cmeq v2.16b,v1.16b,v0.16b; umaxp v3.16b,v2.16b,v2.16b; fmov x5,d3; cbnz x5` →
`ld r3,936(r27); stb r0,0(r3)` (the deferred-signal poke, 2) · `addi; lxvx` (2) ·
`vcmpequb` (1) · `vmr; vpkuhum; vspltisw; vsldoi; vsldoi; vpkuhum; vmaxub` (UMAXP, 7) ·
`vsldoi 8; mfvrd` (FMOV, 2) · `cmpdi; beq; b; b` (CBNZ, 4) = **18 host instructions**.

`strchr+0x68` (22.7 %), 6 guest: `ldr; cmeq; cmhs; umaxp; fmov; cbz` →
2 + 2 + 1 + `vmaxub; vcmpequb` (CMHS, 2) + 7 + 2 + 4 = **20**.

`__strlen_asimd+0xa0` (19.3 %), 6 guest: `ldp q1,q2; uminp v0,v1,v2; uminp v0,v0,v0; cmeq v0.8b,#0; fmov; cbz` →
2 + 3 + `vpkuhum; vspltisw; vsldoi; vsldoi; vpkuhum; vminub` (6) + `vmr` + 6 + `vspltisw; vcmpequb; vspltisw; vsldoi; vsldoi` (CMEQ.8B with the 64-bit `VMov`, 5) + 2 + 4 = **29**.

Across the run: 3.75 G host instructions for ≈140 M guest loop iterations
(27 per iteration), 2.17 G cycles (15.5 per iteration), 27 % of the cycles
in FXU completion stalls (the `mfvrd→cmpdi→beq` tail), 0.1 % in store
forwarding. With #1 the loop path becomes `poke(2) + load(2) + vcmpequb.(1)
+ bc(1)` = 6 for `memchr` (plus `vmaxub` for `strchr`'s `CMHS`, plus the
`vminub` pair for `strlen`), the VSU→FXU crossing leaves the loop, and the
exit block recomputes `umaxp`/`fmov` from the live compare. **[INFERENCE,
testable]**: host instructions per iteration 18-29 → 6-11, cycles per
iteration bounded by the load-use latency (≈ 5) rather than the 15.5 now;
expect `strloop` ≈ 2.5× faster, i.e. at or above Pi speed.

The exit path (`shrn #4; fmov; rbit; clz; lsr #2` or `umaxp…; fmov; rbit; clz`)
is a second idiom: index-of-first-match = `vctzlsbb` (3.0, 6 cyc) or
`vbpermq`+`mfvsrd`+`cnttzd` (2.07). It runs once per call and matters less.

Even without recognition, #3 (`VMov` 3 → 2), #4 (`CMHS` chain) and #5
(`umaxp v,v` 7 → 4) take the three blocks to 13/16/21 instructions.

### 4.2 AES round chains

`AESE+AESMC` pairs are every round but the last in every AES-NEON
implementation (OpenSSL, mbedTLS, libgcrypt use exactly `aese; aesmc`).
With `R` = 16-byte reversal and `vcipher(x,0) = MC_p(SB(SR_p(x)))` where
`SR_p = R∘SR∘R` and `MC_p = R∘MC∘R` (both measured), one round
`MC(SB(SR(s⊕k))) = R(vcipher(R(s⊕k), 0))`; consecutive rounds cancel the
inner reversals, so an `aese/aesmc` chain of *n* rounds is `R` once,
*n*×(`vxor` with a pre-reversed key, `vcipher`), `R` once, and the final
`aese` alone is `vxor; vcipherlast; R`. Per round 2 instructions (8.0 cyc
latency, 2.0 tp) instead of 3 + 4-6. The frontend needs to recognise the
pair at least (2 insns for 2 guest insns: `vxor; vcipher` then the
`vsldoi`/`REV32` corrections cancel to a single fixed permutation `P` with
`round = REV32(vcipher(P(s⊕k)))`, 4 insns); the chain form needs a
block-level pass. Decryption (`aesd; aesimc`) is the mirror with
`vncipher`.

### 4.3 FMA fixup gating

`fmla`/`fmls`/`fmul`/`fadd`/`fmax` vector and scalar all share one pattern:
the POWER instruction is exact except for NaN precedence (and the sign of a
negated NaN operand). `xvcmpeqsp. t,r,r` on the *result* is 3.0 cyc, sets
CR6 "all equal" when no lane is NaN, and `bc` around the 14-30-instruction
fixup costs nothing when predicted. This replaces the unconditional
`PropagateNaNOperand` sequence (`[CODE] TranslateFP.cpp:135-160`, "about a
dozen vector instructions to every scalar FP arithmetic op").

### 4.4 Compiler-output idioms in the census

- glibc/musl/busybox: `cmeq`+`umaxp`/`uminp`/`addp`+`fmov`+`cbz` (§4.1);
  `movi`+`bit` masks (`strchr`), `dup w` splats (§3.11), `saddl/saddl2`
  pairs (widen-add: 3 insns each), `addhn` (§3.3), `rev32/rev64` (§3.8),
  `cnt`+`addv` (popcount: `vpopcntb` + `vsum4ubs`/`vsumsws`; the pair could
  be `vpopcntd`+`vaddudm` fold, 3 insns).
- xxHash `XXH3` (`-O3`, GCC 14): `umlal`+`umlal2` (each `vmule*/vmulo*` +
  merge + add: 4), `ext #8` (1), `uzp1/uzp2 .4S` (1/3), `shrn #32` from
  `.2D` (`vpkudum` after `vsrd 32`: 3) and `xtn .2S` (1), `eor` (1),
  `ld2/st2` (§3.13): the whole accumulate step is 1-4 host instructions per
  guest instruction with no memory traffic; the `uzp2`+`umlal2` pair could
  fold into `vmulosw` on the un-unzipped inputs (the odd-lane product is
  what `umlal2` after `uzp2` computes) — a 2-for-1 recognition.
- Widening `UXTL`+`SHL` (`ushll #n`) is one `vmrgl*(Z)` + `vsl` (2); the
  frontend already emits that.

## 5. Known backend bugs, with fixes

1. **`VAddV` i32 uses `vsumsws`** (`[CODE] VectorOps.cpp:586`): saturates;
   `ADDV` wraps. Fix: `vsldoi t,V,V,8; vadduwm t,V,t; vsldoi u,t,t,4;
   vadduwm Dst,t,u` then clear lanes 1-3 (`vsldoi` with Z twice or an AND
   mask); parity row `addv 32 OK`, 10.1 cyc. The frontend's own workaround
   (`[CODE] TranslateSIMD.cpp:368-372`) can then go.
2. **`VFNMLA`/`VFNMLS` use `xvnmsubasp`/`xvnmaddasp`**
   (`[CODE] VectorOps.cpp:3747-3785`): the ISA defines these as the negation
   of the rounded `(A×B)∓T` `[p.745]`; the IR wants one rounding of the
   negated expression. [MEASURED] rows `fmls_nv.ru/rd` differ by 1 ulp
   (e.g. `a=88a42fd6 b=cf000000 c=5201ed86`: ARM `5201ed86`, POWER
   `5201ed85`) and the NaN sign differs in all modes. Fix: `xvnegsp` (or
   `xvnegdp`) the multiplicand into a temporary, then `xvmaddasp`
   (`VFNMLA`) / `xvmsubasp` (`VFNMLS`); rows `fmls_ng.*` are numerically
   exact in all four modes, and `fmla_ex/fmls_ex` are fully exact with the
   §4.3 fixup.
3. **`VSRSHR` and `VSQSHL` scalar spill loops** (`[CODE] VectorOps.cpp:
   1367-1424, 1437-1521`): correct but 60-100 instructions with a
   15-18-cycle store→load; vector replacements in §3.2 (5 and 8
   instructions, all widths incl. 64-bit, parity OK).
4. **`Float_FromGPR_S` double rounding** (`[CODE] VectorOps.cpp:4557-4558`,
   from `M1b-SIMD-SUBSET.md`): not re-probed here; `fcfids` is the fix.
5. Not a bug but a trap the probes closed: `vcipher`'s state layout
   (§3.15) — the emitter's comment `AESIMC(S) == vncipher(vcipherlast(S,0),0)`
   (`[CODE] Emitter.h:1552`) holds only in ISA byte order; in the guest's
   lane order the `REV32` conjugation is required (`aesimc_v` row).

## 6. Prior art: SIMDe's NEON→AltiVec paths, reviewed

(`simde/simde/arm/neon/*.h`, checked out on the host, `~/rn-scratch/simde`.)
Coverage is thin: of the families here, only `vshl`, `vrshl`, `vqsub`,
`vabd` (P9 `vec_absd`), `vpaddl`, `vrev64`, `vcnt`, `vrbit`, `vqtbl*`,
`vmla` have `SIMDE_POWER_ALTIVEC_*` paths; `vaddv/vmaxv/vminv`, `vqdmulh/
vqrdmulh`, `vqshl`, `vshrn_n`/`vqshrn_n`, `vaese*`, `vtbl` (non-q) have none
(they fall to the generic C loops). Correctness of what exists:

- `vshlq_s8` P6: `b_abs = vec_abs(b)`, left shift masked by `b_abs < 8`,
  right shift by `min(b_abs, 7)`, select on `b < 0`: matches the verified
  semantics including `−128` (`vec_abs(−128) = 0x80` reads as 128, masked
  to 0 on the left, clamped to 7 on the right = sign fill). Correct. The
  16/32-bit forms sign-test the count byte with `vec_sl(b, W−8) < 0`,
  also correct.
- `vrshlq_s8` P6: `(a >> (n−1)) >> 1 + ((a >> (n−1)) & 1)` masked by
  `b_abs < 8`: correct for all counts including `−8` (0, as ARM).
- `vqtbl1q` P6: `vec_perm(t,t,idx) & (idx < 16)`: correct only because
  GCC's `vec_perm` builtin is endian-adjusted on LE (it complements the
  control and swaps operands, compiling to `vspltisb`/`vnor`/`vperm`); raw
  `vperm` would need the `^15`. Same shape as the current `VTBL1`.
- `vrbitq` P6: three `vec_sel(vec_sl, vec_sr, mask)` rounds: correct,
  9 instructions + 3 constants vs the 6-instruction nibble table in §3.7.
- `vrev64q` P7: `vec_reve(vec_reve(a) as long long)`: correct; on ISA 3.0
  it compiles to `xxbrq; xxswapd` (2) where `xxbrd` is 1.
- `vpaddlq` P8: `vec_mule(a,1) + vec_mulo(a,1)`: correct, same as §3.3.
- `vmlaq_f32` P6: `vec_madd(b,c,a)` — **fused**; the intrinsic is
  `FMUL`+`FADD` per the ACLE (two roundings), so SIMDe differs from ARM
  when the product is inexact. Not relevant to the JIT (which lowers
  `FMLA`, the fused instruction), but do not copy it for `VFMul`+`VFAdd`
  pairs.

Net: SIMDe confirms the lane-order conventions but offers nothing for the
hard families; every sequence in §3 was derived and verified here.

## 7. POWER8 vs POWER9 vs POWER10: what matters

| family | POWER8 (2.07) | POWER9 (3.0) gain | POWER10 (3.1) |
|---|---|---|---|
| `TBL1` | 6 insns | **3** (`vpermr`, `xxspltib`) | same |
| `UABD` | 3 | **1** (`vabsdu*`) | |
| byte/word constants | 4 (GPR path) or pool | **1** (`xxspltib`, `mtvsrws`) | `xxspltiw`/`xxspltidp` (1, any 32-bit) |
| `DUP` from GPR | 3 | **1-2** (`mtvsrws`, `mtvsrdd`) | |
| `UMOV`/`INS` | 3 / 4-5 | **2** (`vextu*rx`, `vinsert*`) | |
| `REV64/32/16` | 1-3 | **1** (`xxbr*`) | |
| first-match index | `vbpermq`+`mfvsrd`+`cnttzd` | **`vctzlsbb`** (1) | |
| `AESMC` | 6 | 4 (`xxbrw`) | |
| unaligned V load/store | `lxvd2x`+`xxpermdi` (2.6 cyc) | `lxvx`/`lxv` (2.0) | |
| `MUL.2D` | 8 | 8 | **1** (`vmulld`) |
| `UZP2/TRN` 8/16 | 3-4 | `xxperm` + constant | |
| half precision | software | `xvcvhpsp`/`xvcvsphp` (22 cyc) | |
| everything else in §3 | as listed | no change | `vcmpne*`, `vextdu*`, `xxgenpcvbm` (3.1) could shorten `CMTST` and `TBL` masks; unmeasured |

`objdump -M power8` vs `-M power9` on the two probe binaries confirms the
split: the P8 build decodes clean as POWER8, the P9 build's ISA 3.0 content
is exactly `vpermr`, `vabsdu*`, `xxspltib`, `xxbrw`, `vextubrx`, `vctzlsbb`,
`vclzlsbb`, `lxv*/stxv*`, `mtvsrws/mtvsrdd`, `vinsertb`, `vextsb2w`,
`xscvhpdp/xvcvsphp` plus the compiler's own.

## 8. Suggested implementation order

1. **String early-exit fusion** (#1) in the A64 frontend, reusing
   `IROp_CondJump{VCmpElementSize}`: largest measured share of real time,
   backend support exists, one pass. Ship with the `strloop` PMU numbers as
   the acceptance test (`pm_cmplu_stall_fxu` < 5 %, ≤ 8 host instructions
   per loop iteration).
2. **Cheap single-op corrections** (#3 `VMov` 64, #4 `CMHI/CMTST/CMHS/CMGE`,
   #5 `umaxp v,v`, #6 `ADDV.4S`, `fmov x,d` via `mfvsrld` on 3.0): each a
   few lines in `TranslateSIMD.cpp`/`VectorOps.cpp`, all parity rows exist.
3. **Shift families** (#2, §3.2): new IR ops or widened `VSRSHR`/`VSQSHL`
   lowerings, plus `SSHL/USHL/SRSHL/URSHL` by register, `RSHRN`/`SQ*SHRN`
   through the existing pack ops. Unblocks codec code.
4. **FMA/float exactness** (#7, #8, §3.12): `xvneg`+`xvmadd` for the
   `FNML*` family, then the CR6-gated NaN fixup shared by vector and scalar
   FP (removes `PropagateNaNOperand` from the hot path).
5. **Missing families** with one-instruction landings: `SQDMULH/SQRDMULH.8H`
   (#10), `UDOT/USDOT` (#13), `PMULL` (#14), `UABD` (P9), 64-bit saturating
   (#11), `SMULL/UMULL/SQDMULL`, `SADDLP/UADDLP`, `CLS`, `RBIT`.
6. **Emitter additions** for ISA 3.0 (`vpermr`, `xxspltib`, `vextu*rx`,
   `vinsert*`, `vctzlsbb`, `xxbr*`, `mtvsrws`, `mtvsrdd`, `mfvsrld`, `lxv`,
   `xxperm`), then the P9-gated `TBL` (#9), `DUP`/`INS`/`UMOV` (§3.11) and
   `REV*` forms.
7. **AES** (#12) with the pair recognition, then `LD2-4/ST2-4` (§3.13).
8. **`FRECPE`/`FRSQRTE`** exact vector forms and the VSX-aware allocator
   (2(a)) last: both are real work with narrow benefit today.

## 9. Reproduce

```sh
# Pi (once): golden capture of the reference model against the hardware
gcc -O2 -march=armv8.2-a+crypto+dotprod -o neon_golden_pi neon_golden_pi.c -lm && taskset -c 3 ./neon_golden_pi > pi.golden
# POWER9: parity, POWER8-only and POWER9 builds
gcc -O2 -mcpu=power8 -DP9=0 -w -o neon_parity_p8 neon_parity_p9.c -lm && taskset -c 100 ./neon_parity_p8 > logs/parity-p8.out
gcc -O2 -mcpu=power9 -DP9=1 -w -o neon_parity_p9 neon_parity_p9.c -lm && taskset -c 104 ./neon_parity_p9 > logs/parity-p9.out
bash ~/Development/powerpc64le-handbook/probes/isa30scan.sh neon_parity_p8   # must report 0 undecodable-as-POWER8
diff <(awk '{print $1,$2,$3}' pi.golden) <(grep -v '^#' logs/parity-p9.out | awk '{print $1,$2,$3}')   # only SKIP rows differ
# timing
gcc -O2 -mcpu=power9 -DP9=1 -w -o neon_bench_p9 neon_bench_p9.c && taskset -c 108 ./neon_bench_p9 > logs/bench-p9.out
# VSCR.SAT vs FPSR.QC
gcc -O2 -mcpu=power8 -w -o vscr_probe vscr_probe.c -lm && taskset -c 116 ./vscr_probe > logs/vscr-probe.out
# workload (binary built on the Pi: gcc -O2 -static -o strloop strloop.c)
POWERARM_HOSTPAGEMODE=force numactl --membind=0 taskset -c 112 perf stat -e cycles:u,instructions:u,pm_cmplu_stall_fxu,... -- build-rn/Bin/POWERarm ./strloop 4096 200000
```

`logs/isa30c-vector-index.txt` is the mnemonic → form → page → ISA version
index for the 522 vector/VSX instructions, extracted from Appendix F of the
ISA 3.0C PDF (the handbook's scalar index deliberately trims these).
