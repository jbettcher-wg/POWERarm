# POWERarm: lowering A64 scalar floating point onto POWER

Research date 2026-09-16. Research only: no emulator source was modified. The
probes, their logs and the workload sources are under `probes/` next to this
document.

Evidence tags: **[MEASURED]** probe or profile output in `probes/logs/`;
**[CODE]** read in source (file:line, POWERarm at `c2d113228` on
`powerarm-m1/research-scalar`); **[SPEC]** Power ISA v3.0C (`PowerISA_public.v3.0C.pdf`,
page numbers are the PDF's printed page numbers), the POWER9 User's Manual v2.0
(`POWER9_um_OpenPOWER_v20GA_09APR2018_pub.pdf`, "UM") or the Arm ARM pseudocode;
**[INFERENCE]** a conclusion drawn from measurements that no experiment here has
isolated. Both PDFs are in
`~/Development/powerpc64le-ports/flap-standalone/docs/power9/isa/` on the host.

Machines: POWER9 DD2.3 (AC922, 3.8 GHz boost, Arch, kernel 7.2.6-64k), timings
pinned with `taskset -c 88|92|96` on NUMA node 0 with `numactl --membind=0`; the
Raspberry Pi 5 (Cortex-A76) was used only to validate the ARM reference model and
to capture workload checksums. "POWER8 path" numbers are the ISA 2.07 instruction
sequences timed on the POWER9; no POWER8 machine was available, so POWER8 *timings*
are [INFERENCE] from the UM tables, POWER8 *legality* is checked with
`objdump -M power8`.

---

## 0. Executive summary

Scalar FP is currently 17–24× slower than native ppc64le on FP-bound loops,
against 4.9× for the integer M1 baseline [MEASURED, §2]. The IPC of the emulated
code is close to native (1.2–1.5); the cost is almost entirely the **13–21×
instruction expansion**, and 60–70 % of the host instructions in the hot blocks
belong to the branch-free NaN-precedence fix-up and the constants it materialises
[MEASURED, §2.3]. One guest `FMADD` lowers to 63 host instructions, one `FADD` to
about 40.

The cheapest exact fix is a two-instruction pre-check (`xscmpudp` of the operands
into a scratch CR field, `bso` to a cold block), which costs +0.6 cycles of latency
and +2 instructions on the non-NaN path, against +12 cycles and +40 instructions
today [MEASURED, §3]. Everything else in this study is second order to that.

### Ranked recommendations

| # | Recommendation | Expected effect, per guest op unless stated | ISA | Semantic risk |
|---|---|---|---|---|
| 1 | **NaN precedence by pre-check branch** on FADD/FSUB/FMUL/FDIV/FNMUL (`xscmpudp crN,a,b; bso crN,cold; xv*`), cold block = swap operands when op1 is a qNaN and op2 an sNaN | latency 18.4→6.6 cycles, instructions 45→13 per 4 chained adds [MEASURED §3]; hot-block instruction count −60 % [MEASURED §2.3]; nbody/lu/fmix32 wall time projected 2.5–4× faster [INFERENCE] | 2.06 | low: 2.76 M-case corpus incl. all NaN shapes and 4 rounding modes passes, control fires on exactly the 72 qNaN/sNaN pairs |
| 2 | **FMIN/FMAX/FMINNM/FMAXNM → `xvmindp/xvmaxdp` (`xvminsp/xvmaxsp` for f32) + the same pre-check**; `xsmaxdp` is FMAXNM-exact except the qNaN/sNaN order, and FMAX-exact whenever no operand is a NaN | 40 IR ops → 3 host instructions [MEASURED §2.2, §6.1] | 2.06 | low: 166 k-pair corpus; the only cold-path shapes are enumerated in §6.1 |
| 3 | **FMADD family**: `xsmaddadp`/`xsmsubadp` with `xsnegdp` on the multiplicand where needed, post-check on the result (`xscmpudp cr,r,r; bso`) or pre-check on the inputs (4 instructions) | 47 IR ops / 63 host instructions → 3–5; latency 6.0→6.6 cycles [MEASURED §6.2] | 2.06 | medium: NaN order and NaN *sign* differ from ARM in all four forms, so the cold path is mandatory; `xsnmaddadp/xsnmsubadp` must not be used (sign of zero) |
| 4 | **Zero the upper doubleword with one `xxpermdi` against the pinned zero VSR (vs14)** instead of `vspltisw`+2×`vsldoi`; or leave the upper lane dirty where a scalar consumer follows | latency 13.0→10.0 (→6.4 if elided), 4→2 instructions [MEASURED §3.3] | 2.06 | none for the xxpermdi form |
| 5 | **FCMP consumers read CR bits directly**: every A64 condition after FCMP is one CR0 bit or a `cror` of two; skip the XER lift and its projection | 16→6 host instructions, 10.0→6.9 cycles per FCMP+B.cond (P8); FCSEL: branch-free `xxsel` (`setb` on P9, `mfocrf` mask on P8) or fused `xvcmpgtdp`+`xxsel` saves ~9 cycles per unpredictable select (≈21 per mispredict) [MEASURED §7] | 2.06 (`setb` 3.0) | low; the NZCV lift is still needed when NZCV escapes the block |
| 6 | **Conversions**: keep the truncating `xscvdp[su]x[dw]s` path and the GPR round trip; use `mtvsrwa/mtvsrwz` for 32-bit sources; drop the `vspltisw` in `PlaceElement0`; for ties-to-even (FCVTN*, FRINTN/X) test the shadow FPCR and use `fctid`/`xsrdpic` directly when RMode is nearest, else the `mffscrni/mffscrn` sandwich on P9 | tie-even conversion 57.6→31.5 cycles (P8, shadow+`fctid`), →51 (P9 sandwich) [MEASURED §5]; SCVTF from W −1.9 cycles/−1 instruction | 2.07 / 3.0B | low; the shadow test is exact by construction |
| 7 | **FPSCR policy**: keep host FPSCR.RN equal to FPCR.RMode (as now). `MSR FPCR` costs 36 cycles standalone and up to +140 inside an FP stream, but no workload here executes one; never put `mffs`/`mtfsf` on a hot path | [MEASURED §4] | – | – |
| 8 | **Register file**: no change. FPR-half vs VMX-half VSX scalar ops are identical (6.0 cycles), vector-lane forms cost +0.4 cycles (add), +1 (div), +10–20 % divide/sqrt throughput, but a permute round trip to doubleword 0 costs more (+6.5). `mtvsrd`→`xv*` costs +1.7 cycles over `mtvsrd`→`xs*` (UM claim confirmed) | [MEASURED §8] | – | – |
| 9 | (Out of scope, but the next largest item) **V16–V31 and non-SRA GPR context traffic**: 38 `lxvx` + 24 `stxvx` + 46 `ld/std` per hot nbody block; LSU stalls are 18 % of nbody's cycles | [MEASURED §9] | – | – |

Suggested implementation order: 1, 2, 3 (one mechanism: a "NaN cold block" helper
in the backend plus fused A64 arithmetic IR ops), then 4, then 5, then 6.
Items 1–4 are measured per operation; their combined effect on wall time is a
projection until implemented, and §2 gives the baseline to re-run.

---

## 1. Method and files

- **Parity before timing.** `probes/armref.h` is a portable C model of the Arm ARM
  pseudocode (FPProcessNaNs/3, FPAdd/Sub/Mul/Div, FPMulAdd, FPSqrt, FPMax/Min and
  the Num forms, FPCompare, FPToFixed, FixedToFP, FPConvert incl. half and
  FPConvertNaN), with the integer conversions and narrowings done in integer
  arithmetic so it depends on no host conversion instruction. `probes/fpsem.c` runs
  a corpus (65 doubles and 56 floats of edge values: ±0, min/max denormal, min
  normal, 0.5/1.5/2.5/3.5 ties, 1±ulp, 2^31/2^32/2^52/2^53/2^63/2^64 edges, max,
  ±Inf, default and payload qNaN/sNaN of both signs, half-precision boundaries;
  plus 4 000 biased random values per axis) through every candidate under all four
  FPCR rounding modes. Every candidate gets a PASS/FAIL line; candidates named
  `*_ctl` are **positive controls** that must fail.
  - On the Pi 5 the same harness runs the real A64 instructions against the model:
    **71/71 PASS** after one model bug (32-bit saturation value not masked) was
    caught by the hardware [MEASURED `logs/fpsem-pi5.log`]. The model is therefore
    a validated oracle; the Pi was used for one 6-second run.
  - On the POWER9, both the `-mcpu=power9 -DP9` and the `-mcpu=power8` builds:
    **all candidates PASS, all 34 controls FAIL as expected**
    [MEASURED `logs/fpsem-p9.log`, `logs/fpsem-p8.log`].
  - A harness defect worth recording: passing an f32 through a `float` argument let
    clang emit a signalling `xscvdpsp`, which quieted sNaN inputs before the
    candidate saw them and silently made one control pass. The f32 plumbing now
    builds vectors from integer bits. The verification-discipline rule "controls
    must fire" is what caught it.
- **Timing.** `probes/fptime.c` has 128 closed `mtctr/bdnz` asm kernels, one
  question each, in a dependent chain ("lat") and with four independent chains
  ("_t"); `probes/fptime.sh` runs each under `perf stat` (cycles, instructions,
  `pm_flush`, `pm_cmplu_stall_mtfpscr`, `pm_br_mpred_cmpl`). Results:
  `logs/fptime-p9.csv`, `logs/fptime-p8.csv` (cycles per loop iteration, 4 ops per
  chain per iteration). The P8-legal binary gives the same numbers within 1 %
  except where noted.
- **Workloads.** `probes/work/*.c`: `nbody` (double arithmetic, fmadd, sqrt, div),
  `lu` (96×96 LU with pivoting: fmsub chains, fabs/fcmp/branches), `fmix32`
  (float arithmetic, fcmp/fcsel clamps, fcvtzs/scvtf), `cvtmix` (fcvtzs/fcvtzu/
  fcvtas/scvtf/ucvtf/fcvt), `cmpsel` (fminnm/fmaxnm, fcsel, data-dependent fcmp
  branches). Static glibc, `-O2 -fno-vectorize -fno-slp-vectorize`, built with
  clang 19 on the Pi (`-march=armv8-a`) and clang 22 on the POWER9
  (`-mcpu=power9`). Checksums match on the Pi, natively and under POWERarm for all
  five [MEASURED `logs/perf/*.time`, `logs/perf/pi5-reference.log`].
  `probes/work/runperf.sh` does the pinned timing (3 process runs × 2 reps), six
  non-multiplexed `perf stat` event groups, and `perf record` with
  `POWERARM_BLOCKJITNAMING=1`; `probes/work/dumpblocks.py` dumps the host code of
  named JIT blocks from inside gdb; `probes/work/ircount.py` + `irjoin.py` produce
  the IR-ops-per-guest-mnemonic tables from `POWERARM_DUMPIR` output and the guest
  disassembly. Logs: `logs/perf/`, `logs/hostcode/`, `logs/ir/`, `logs/guest/`.
- `lrint`/`floor` were removed from `cvtmix` because glibc lowers them to
  `frintx`/`frintm`, which POWERarm reports as
  "unimplemented A64 instruction 0x1e674000" [MEASURED]. FRINT* is a gap.

---

## 2. Where the time goes today

### 2.1 Wall time and counters [MEASURED]

ms per in-program repetition, median of 3 process runs [min–max], POWER9 3.8 GHz:

| workload | Pi 5 | native P9 | POWERarm | **POWERarm / native** | insn expansion | IPC emu / native |
|---|---|---|---|---|---|---|
| nbody | 224.5 | 228.0 [227.9–228.2] | 4753 [4752–4783] | **20.9×** | 21.4× | 1.27 / 1.24 |
| lu | 8.3 | 7.72 [7.70–7.77] | 174.5 [174.4–175.2] | **22.6×** | 15.3× | 1.45 / 2.26 |
| fmix32 | 13.5 | 8.78 [8.78–8.84] | 181.0 [180.3–182.3] | **20.6×** | 13.3× | 1.29 / 2.09 |
| cvtmix | 11.8 | 41.5 [41.5–41.5] | 140.9 [140.8–141.1] | **3.4×** | 11.0× | 1.20 / 0.40 |
| cmpsel | 73.6 | 72.78 [72.76–72.83] | 1258 [1258–1263] | **17.3×** | 14.6× | 1.25 / 1.51 |

- The emulated IPC is the same as or better than native on nbody, so the ratio is
  the instruction count. Reducing instructions is the lever, unlike the integer
  baseline where store-forwarding stalls dominated.
- Native `cvtmix` is an outlier: clang's ppc64le code chains `xscvdpsxds` into
  `xscvsxddp` inside a VSR, which trips the UM's forwarding-restriction flush
  (5.1 M `pm_flush`, 3.5 M `pm_cmplu_stall_ntc_flush`, IPC 0.40). POWERarm moves the
  integer through a GPR and takes 3 k such stalls. See §5.5.

Completion-stall breakdown of the emulated runs (share of cycles):

| workload | pm_cmplu_stall | lsu | exec unit | of which vdp / vdplong / fxu | st_fwd | branch mispredicts (emu / native) |
|---|---|---|---|---|---|---|
| nbody | 38.6 % | 18.0 % | 20.5 % | 13.2 / 6.8 / 0.6 % | 2.0 % | 20.7 M / 2.2 k |
| lu | 29.8 % | 17.6 % | 12.2 % | 1.3 / 0.0 / 1.7 % | 0.4 % | 0.80 M / 66 k |
| fmix32 | 41.5 % | 10.9 % | 30.9 % | 8.4 / 4.3 / 17.7 % | 0.7 % | 0.35 M / 22 k |
| cmpsel | 37.7 % | 12.5 % | 25.1 % | 5.7 / 0.0 / 15.3 % | 1.5 % | 10.7 M / 4.1 M |

`pm_cmplu_stall_mtfpscr` is 0 in every run: nothing on these paths writes FPSCR.
nbody's 20.7 M mispredicts are one per inner-loop iteration (2 M steps × 10 pairs);
they are the guest loop exits (`cbz`/`b.ne` blocks, 7 IR ops each), not FP.

### 2.2 IR ops per guest FP mnemonic [MEASURED, generated]

From `POWERARM_DUMPIR` over every block of each workload, joined with the guest
disassembly (`logs/ir/irexp-*.csv`). Average IR ops per guest instruction
(vector-class IR ops in brackets):

| mnemonic | nbody | fmix32 | cvtmix | cmpsel |
|---|---|---|---|---|
| fmadd.d / .s | **46.6** (43.2) | 47.0 (44.0) | 45.0 | – |
| fmul.d / .s | 15.8 (14.3) | 14.5 (14.1) | 14.3 | 14.9 |
| fadd, fsub | 15.3 | 14.1 | 14.3 | 14.7–15.8 |
| fdiv.d | 14.7 | – | – | 14.0 |
| fnmul.d | 16.0 | – | – | – |
| fminnm.d / fmaxnm.d | – | – | – | **40.0 / 39.7** (37) |
| fsqrt | 2.3 | 2.0 | – | – |
| fcvt (s↔d) | – | 2.0 | 2.0 | – |
| fcmp | – | 1.3 | 1.7 | 1.8 |
| fccmp.s | – | 6.5 | – | – |
| fcsel | – | 2.7 | 2.0 | 2.8 |
| fcvtzs x/w, fcvtzu | – | 3.0 / 2.0 | 3.0 / 2.0 | – |
| scvtf, ucvtf | – | 2.0 / 5.2 | 2.0 / 4.2 | – / 5.2 |
| fmov, fneg | 1.5–1.9 / – | 1.7 / 3.0 | 1.5 / 2.0 | 1.2–1.7 |

The 14 extra IR ops on every binary op are `PropagateNaNOperand` [CODE
`A64Frontend/TranslateFP.cpp:135-145`]: 2 `VFCMPUNO`, 2 `VAnd`+`VCMPEQZ`+`VNot`
quiet-bit tests, `VAnd`, `VBSL`, plus the `FPConstant` for the quiet bit. `FMADD`
adds `FPThreeRegister`'s six `VBSL` NaN-order chain and the Inf×0 default-NaN
selection [CODE `TranslateFP.cpp:345-414`]. FMIN/FMAX build compares, selects and
a `VFAdd` for the NaN result [CODE `TranslateFP.cpp:147-175`].

### 2.3 Host instructions in the hot blocks [MEASURED]

The hottest block of each workload (52.9 % of nbody's cycles is `nbody+0xbe0`,
64.6 % of fmix32's is `fmix32+0x9e0`, 70.4 % of cmpsel's is `cmpsel+0xac0`,
77.2 % of lu's is `lu+0xd00`, 76.4 % of cvtmix's is `cvtmix+0x94c`
[`logs/perf/*.report`]) was dumped from the running JIT and disassembled
(`logs/hostcode/*.dis`; guest listings in `logs/guest/`):

| block | guest insns | of which FP arith (other FP) | host insns | expansion | NaN-fix-up-attributable host insns |
|---|---|---|---|---|---|
| nbody+0xbe0 | 36 | 21 (8 fmadd, 5 fmul, 3 fsub, 3 fnmul, fsqrt, fdiv) | 1086 | **30.2×** | ≈700 (xvcmpeqdp 128, vand 132, vsel 68, vspltisw 69, xxlnand 48, vcmpequd 48, vnot 36, mtvrd 36, xxspltd 36, vor 32, vandc 24, part of vmr 44 / vsldoi 42) |
| nbody+0xb00 | 39 | 15 (15 fmadd) | 1312 | 33.6× | ≈900 (xvcmpeqdp 150, vand 135, vsel 105, …) |
| cmpsel+0xac0 | 41 | 19 (6 fmul, 6 fsub, 3 fminnm, 3 fmaxnm, fadd; plus 7 fcmp, 7 fcsel) | 1066 | 26.0× | ≈650 |
| fmix32+0x9e0 | 46 | 12 (6 fmul, 3 fadd, fsub, 2 fneg; plus 6 fcmp, 6 fcsel, 2 fccmp, 4 fcvtzs, 2 scvtf) | 716 | 15.6× | ≈350 (vand 50, vspltisw 45, vsldoi 43, vcmpeqfp 40, xxlnand 20, vcmpequw 20, …) |
| lu+0xd00 | 19 | 4 (4 fmadd) | 406 | 21.4× | ≈250 |
| cvtmix+0x94c | 34 | 9 (3 fmul, 3 fadd, 2 fmadd, fsub; plus 4 fcvtzs, fcvtzu, 2 scvtf, 2 ucvtf, 2 fcvt, fcmp, fcsel) | 568 | 16.7× | ≈260 |

Guest counts are the `GuestOpcode` markers of each block's IR dump; the guest
listings are in `logs/guest/`.

One guest `fmadd d19,d16,d17,d18` in `nbody+0xbe0` is lowered to `xvmaddadp` plus
**62 instructions**: three `(xvcmpeqdp, xvcmpeqdp, xxlnand)` unordered tests, three
`(vand, vspltisw, vcmpequd, vnot)` quiet-bit tests, six `vand/vandc + vsel` for
the NaN order, two `li/sldi/mtvrd/xxspltd` constant materialisations (quiet bit
and +Inf, rebuilt for every op, never hoisted), the Inf×0 test (`xvabsdp` ×2,
`xvcmpeqdp` ×4, `vand`/`vor`), a third constant (`li 4095; sldi 51` = the
default NaN) and the final `vsel` (`logs/hostcode/nbody_0xbe0.dis`). `vspltisw
v30,0` is re-emitted for every `VCMPEQZ` although vs14 is a pinned zero
[CODE `ArchHelpers/PPC64Emitter.h:62-70`].

---

## 3. NaN precedence (question 1)

### 3.1 Semantics [SPEC][MEASURED]

- ARM `FPProcessNaNs`: the first *signalling* NaN operand, else the first *quiet*
  NaN operand, quieted; with FPCR.DN=0 the payload is kept
  (Arm ARM, shared/functions/float/fpprocessnans,
  https://developer.arm.com/documentation/ddi0602/latest/Shared-Pseudocode/shared-functions-float).
- VSX scalar arithmetic returns operand 1 if it is any NaN, else operand 2,
  quieted [SPEC ISA §7.6.1 tables e.g. xsadddp p.517; MEASURED: the raw
  `xsadddp/xssubdp/xsmuldp/xsdivdp` controls fail on exactly 72 of 2 756 672 pairs,
  all of them (qNaN op1, sNaN op2), and on nothing else in any rounding mode].
- So the **only divergent shape** for the binary ops is `op1 = qNaN, op2 = sNaN`,
  where ARM returns `quiet(op2)` and POWER returns `op1`. Both give the default
  NaN for Inf−Inf, 0×Inf, 0/0, Inf/Inf, and both quiet an sNaN in place with its
  payload (the f32 `xvaddsp`, `xvsubsp`, `xvmulsp`, `xvdivsp` lane forms behave the
  same: 136 of 2 464 316 controls, same shape).

### 3.2 Candidates and cost [MEASURED, `logs/fptime-p9.csv`]

Cycles per op in a dependent chain, and cycles per op with four independent
chains of four ops each (16 ops per iteration), at 3.8 GHz:

| candidate | instructions / op | latency (cycles) | 4-chain throughput (cycles/op) | flushes |
|---|---|---|---|---|
| `xsadddp` alone (reference) | 1 | 6.00 | 1.52 | 0 |
| `xvadddp` alone (today's op) | 1 | 6.35 | 1.53 | 0 |
| **pre-check**: `xscmpudp cr1,a,b; bso cr1,cold; xsadddp` | 3 | **6.57** | **1.76** | 0 |
| post-check: `xsadddp; xscmpudp cr1,r,r; bso cr1,cold` | 3 | 6.62 | 1.84 | 0 |
| branch-free vector fix (9 mask ops + `xxsel` + `xvadddp`; today's shape) | 11 | **18.35** | **7.15** | 0 |
| f32: `xvaddsp` alone | 1 | 6.38 | – | 0 |
| f32 pre-check: 2×`xxsldwi`, 2×`xscvspdpn`, `xscmpudp`, `bso`, `xvaddsp` | 7 | 6.64 | 3.31 | 0 |
| f32 post-check: `xvaddsp; xxsldwi; xscvspdpn; xscmpudp; bso` | 5 | 6.62 | – | 0 |
| f32 recording compare: `xvaddsp; xvcmpeqsp. t,r,r; bc 4,4*cr6+lt,cold` | 3 | 6.51 | – | 0 |

- The pre-check's compare runs in parallel with the arithmetic (it reads the
  inputs); its branch resolves about three cycles after the compare and is never
  taken on real data, so the measured latency cost is 0.57 cycles and the
  throughput cost one issue slot per op. This is the "wide out-of-order core"
  case the coordinator asked about: the extra instructions are independent, so
  they are nearly free; the branch-free fix is a *dependent* chain of 3-cycle ALU2
  and 2-cycle permute ops in front of the add, so it is not.
- The branch-free fix also costs 4× in throughput because it is 11 instructions
  of which several are `V`-dispatch (both slices of a superslice) [SPEC UM Table
  A-1: `xvcmpeqdp`, `xxsel`, `vcmpequd` are "V" dispatch, 2 per cycle].
- `xvcmpeqsp.`/`xvcmpeqdp.` (Rc=1) set CR6 bit 0 when *all* lanes compare equal
  and bit 2 when none do [SPEC p.671-672]; branching on bit 0 clear is a
  three-instruction NaN test, but it also fires when a *garbage* lane is a NaN.
  After a scalar write the upper lanes are zero, after a NEON write they may not
  be, so it is correct but occasionally slow; fine as the f32 fast path where the
  two `xscvspdpn` of the pre-check are otherwise needed.
- The post-check needs the original operands alive in the cold block; the
  pre-check does not. FEX's RA may tie the destination to a dead source, so the
  pre-check is the safer default for binary ops; for FMA see §6.2.

### 3.3 The upper-doubleword zeroing that comes with every scalar op [MEASURED]

Today a scalar result is zero-extended by `VMov` = `vspltisw` + 2×`vsldoi`
[CODE `JIT/PPC64LE/VectorOps.cpp:62-100`] on the result's critical path:

| tail after `xvadddp` | instructions | latency |
|---|---|---|
| none | 1 | 6.35 |
| `vspltisw v30,0; vsldoi v31,a,v30,8; vsldoi a,v30,v31,8` (today) | 4 | **12.98** |
| `xxpermdi a, vs14(zero), a, 1` | 2 | 9.98 |

`xxpermdi XT,XA,XB,DM` with XA = zero and DM=0b01 puts 0 in doubleword 0 and keeps
XB's doubleword 1 [SPEC p.780]. Eliding the zeroing when the next consumer is
scalar would recover the remaining 3.6 cycles; that needs an "upper lanes dead"
analysis in the frontend or a lazy zero at block exit and is [INFERENCE] until
built.

### 3.4 Recommended sequences

POWER8 (ISA 2.06/2.07) and POWER9 are the same here. `crN` is any scratch CR
field the backend does not use for NZCV (CR1 or CR5–CR7; CR0 holds NZCV [CODE
`JIT/PPC64LE/JITClass.h:883`]). `Z` = pinned zero VSR (vs14).

```
; FADD/FSUB/FMUL/FDIV/FNMUL Dd, Da, Db   (Da, Db, Dd guest V registers, element 0 in dw1)
    xscmpudp  cr1, Da, Db          ; unordered iff either is a NaN (reads dw0: see note)
    bso       cr1, .cold
    xvadddp   Dd, Da, Db           ; or xvsubdp / xvmuldp / xvdivdp; FNMUL = xvmuldp + xvnegdp
    xxpermdi  Dd, Z, Dd, 1         ; zero dw0 (drop when a scalar consumer follows)
.join:
    ...
.cold:                             ; out of line, any shape; ARM-exact
    ; result = op(Da,Db) unless Da is a qNaN and Db an sNaN, then op(Db,Da).
    ; Swapping is safe for sub/div too: a NaN operand makes the result a NaN
    ; whatever the order, so only the choice of NaN changes [MEASURED: fsub/fdiv PASS].
    mfvsrd r3, Da ; mfvsrd r4, Db
    rldicl r5, r3, 13, 52 ; cmpldi cr1, r5, 0xFFF ; bne cr1, .straight   ; (Da<<1)>>52 == 0xFFF <=> qNaN
    rldicl r5, r4, 13, 52 ; cmpldi cr1, r5, 0xFFE ; bne cr1, .straight   ; exponent all ones, quiet bit clear
    clrldi r5, r4, 13     ; cmpldi cr1, r5, 0     ; beq cr1, .straight   ; payload != 0 <=> sNaN (not Inf)
    xvadddp Dd, Db, Da ; b .zero
.straight:
    xvadddp Dd, Da, Db
.zero: xxpermdi Dd, Z, Dd, 1 ; b .join
```

Note on the compare's lane: `xscmpudp` reads doubleword 0 of each VSR [SPEC p.534]
while the guest scalar lives in doubleword 1. Either compare the *result*
(post-check) after a lane-0 op that produced it, or keep the pre-check and use the
recording vector compare `xvcmpeqdp. t,Da,Da` / `xvcmpeqdp. u,Db,Db` (two
instructions, CR6 bit 0 clear ⇒ some lane is a NaN); or, cheapest when the value
is already in doubleword 0 (conversions, FCMP), `xscmpudp` directly. The `fpsem.c`
candidates place the value in doubleword 0 and pass; `fptime.c` measures both
placements (`add_pre` and `add32_cr6`). Whichever is chosen must be re-verified
on the corpus (§12).

f32 (values in SP format in the low word): `xvaddsp` on the lane, pre-check via
`xvcmpeqsp.` ×2 or post-check via `xxsldwi; xscvspdpn; xscmpudp` (§3.2 table);
`xscvspdpn` is bit-preserving for sNaN [MEASURED: the `xscvspdpn` control fails
on exactly the 19 sNaN inputs; SPEC p.562 "non-signalling"], so it does not
disturb the check.

Alternatives considered and rejected: quieting operands up front changes ARM's
choice (ARM prefers the *signalling* operand); `xsmaxcdp`/`xsmaxjdp` have C and
Java NaN rules, neither ARM's (§6.1); `xststdcdp` (ISA 3.0, p.659) classifies
NaN/Inf/Zero/Denormal but cannot tell an sNaN from a qNaN, so it cannot shorten
the cold path.

---

## 4. FPSCR and the rounding mode (question 2)

### 4.1 Costs [MEASURED, `logs/fptime-p9.csv`]

Cycles per instruction in a loop of four, plus the `pm_cmplu_stall_mtfpscr`
cycles per instruction, and the cost of inserting one of them into a stream of
four independent `xsadddp` (24.1 cycles per group without it):

| instruction | cycles each | mtfpscr stall each | +cycles in an FP stream | ISA |
|---|---|---|---|---|
| `mffs` | 14.2 | 8.0 | +36 | P1 (p.168) |
| `mffsl` | **0.74** | 0 | – | 3.0B (p.168) |
| `mffscrni f,0` | 6.0 | 0 | – | 3.0B (p.168) |
| `mtfsf 0xFF,f` (all fields) | 21.2 | 16.0 | **+139** | P1 (p.170) |
| `mtfsf 1,f` (field 7 = XE/NI/RN only) | 21.2 | 16.0 | +64 | |
| `mtfsfi 7,0` (RN only, immediate) | 18.7 | 7.0 | – | P1 (p.170) |
| `mtfsfi 0,0` (sticky field) | 21.2 | 16.0 | – | |
| `mtfsb0 30` (RN bit, control) | 6.0 | 0 | +8 | P1 (p.171) |
| `mtfsb0 3` (OX, sticky) | 11.4 | 9.0 | – | |
| tie-even sandwich, P8 form (`mffs; mtfsb0 30; mtfsb0 31; xsrdpic; mtfsf 1,f`) | **37.0** | 24 | (4-chain: 36.0, fully serialised) | |
| tie-even sandwich, P9 form (`mffscrni f,0; xsrdpic; mffscrn f,f`) | **15.4** | 0 | (4-chain: 12.1) | 3.0B |
| `xsrdpic` alone | 12.0 | 0 | | 2.06 (p.634) |
| shadow test + `xsrdpic` (`lwz; rlwinm; cmplwi cr1; bne` + `xsrdpic`) | **12.15** | 0 | | |
| `SetRoundingMode` as the JIT emits it (`mffs; mfvsrd; rldicr; or; mtvsrd; mtfsf 0xFF`) [CODE `ALUOps.cpp:3594-3620`] | 36.2 | 24 | up to +139 | |

- The UM is right that only the lightweight forms (`mffsl`, `mffscrn[i]`,
  `mtfsb0/1` on control bits) execute out of order; `mffs`/`mtfsf` are
  next-to-complete and drain the FP pipeline [SPEC UM 25.1.5.6 "Move-To and
  Move-From FPSCR", p.342-343; Table A-1 rows mffs C2/ALU2 latency 3, mtfsf].
  The +139 cycles for `mtfsf 0xFF` inside an FP stream is the cost of a full
  `MSR FPCR` on a hot path.
- `mtfsb0` on a control bit really is free (6 cycles = the loop), which is why the
  P8 sandwich's cost is the `mffs` and the `mtfsf`, not the two `mtfsb0`.

### 4.2 Precedent: the x86 path [CODE]

The x86 frontend keeps the host FPSCR.RN equal to MXCSR.RC (`SetRoundingMode`
on LDMXCSR, `ALUOps.cpp:3594`; `PushRoundingMode`/`PopRoundingMode` bracket F16C
converts, `ALUOps.cpp:3622-3648`, `JIT.cpp:307`), and every instruction with an
immediate rounding mode uses an explicit-rounding host instruction
(`xsrdpiz/xsrdpim/xsrdpip`, `VectorOps.cpp:4177-4181`, `5015-5020`; truncating
converts `xscvdpsxds` in `Float_ToGPR_ZS`, `ALUOps.cpp:4188`). Only
`RoundMode::Nearest` and `Host` share `xsrdpic` and therefore assume RN is
nearest [CODE `VectorOps.cpp:4180-4181`, `5018-5019`]. The A64 frontend follows
the same policy [CODE `TranslateFP.cpp:649-657`, `TranslateBranchSystem.cpp:239`]
and this study confirms it: FCVTZ*, FCVTA*, FCVTP*, FCVTM*, SCVTF/UCVTF, FCVT
between precisions and all arithmetic never touch FPSCR. Only the ties-to-even
conversions (FCVTNS/FCVTNU, and FRINTN once implemented) need RN=nearest
regardless of FPCR.RMode; POWER has no explicit ties-to-even integral or convert
instruction (`fctid` uses RN, `xscvdp*x*s` truncate, `xsrdpi` is ties-away)
[SPEC p.157, p.542, p.633].

### 4.3 Recommendation

- Keep the host FPSCR.RN synced to FPCR.RMode at `MSR FPCR` (36 cycles; the
  workloads execute none, and the 87 static FPCR references in the M1b census are
  `MRS` loads in libc's printf/strtod paths).
- For the ties-to-even cases, test the guest FPCR shadow in the context
  (`lwz` of `CPUState.fpcr`, mask bits 23:22, `cmplwi cr1`, `bne` to the sandwich):
  measured 0.15 cycles over the bare `xsrdpic`, and it lets FCVTNS use `fctid`
  directly (§5.2). On POWER9 the slow side of the test can use the `mffscrni/mffscrn`
  sandwich (15.4 vs 37.0 cycles); on POWER8 the existing `mffs/mtfsb0/mtfsf` one.
  Do not replace the `mtfsf 1,f` restore with `mtfsfi`: the field write is what
  serialises, and `mtfsfi 7` is only 2.5 cycles cheaper.
- `mffsl` is nearly free and reads RN: on POWER9 `GetRoundingMode` can use it
  instead of `mffs` (14.2 → 0.7 cycles) [MEASURED].
- FPSCR sticky bits are set by every inexact op (`XX`), and reading them
  (`mcrfs`, `mffs`) is "F"-dependent on the speculation mode [SPEC UM Table A-1];
  mapping FPSR's cumulative flags onto them, as DESIGN.md §4.4 plans, must
  therefore read FPSCR only at `MRS FPSR`, never per op.

---

## 5. Conversions (question 3)

### 5.1 Semantics on POWER [SPEC][MEASURED]

- `xscvdpsxds`/`fctidz`: NaN → `0x8000_0000_0000_0000`, saturating, VXCVI set
  [SPEC p.542, p.158; MEASURED: the "no NaN fix" controls fail on exactly the
  NaN inputs, 37 of 64 260 per mode]. ARM wants 0 (FPToFixed, Invalid). The fix
  is one `isel` against CR1.SO from an `xscmpudp x,x` (`isel RT,0,RT,4*cr1+3`
  reads literal zero for RA=0 [SPEC p.89]) or the JIT's `bc`+`li`.
- `xscvdpuxds`/`fctiduz`: NaN → 0, negative → 0, > 2^64−1 saturates
  [SPEC p.546, p.159; MEASURED PASS in all modes]: ARM-exact with no fix.
- `xscvdpsxws`/`xscvdpuxws`: saturate to int32/uint32; same NaN behaviour
  [MEASURED PASS with the `isel` fix, controls fail].
- Rounding first with `xsrdpi` (ties away), `xsrdpip`, `xsrdpim`, `xsrdpic`
  (current mode) then truncating is exact in every mode [MEASURED, all FCVT*
  variants PASS, 4 modes × 5 roundings × signed/unsigned × 32/64].
- f32 sources: `xscvspdpn` (exact) then the double path is exact; `xxsldwi`
  brings the low word to word 0.
- Fixed-point `FCVTZ[SU] #fbits`: `xsmuldp` by 2^fbits is exact or overflows to
  Inf, which then saturates identically [MEASURED PASS for fbits 1, 32, 63/64].
- int → FP: `xscvsxddp/xscvuxddp` (i64→f64) and `xscvsxdsp/xscvuxdsp` (i64→f32,
  one rounding) are exact in every mode [MEASURED 80 120 cases × 4 modes];
  `fcfids` (ISA 2.06, p.162) is the FPR-side single-rounding form and also passes.
  `mtvsrwa`/`mtvsrwz` (2.07, p.112-113) do the 32-bit sign/zero extension in the
  move [MEASURED PASS], saving the `extsw`/`clrldi` [CODE `A64FPOps.cpp:138-146`].
- `fcfid` + `frsp` rounds twice: the control fails on `0x8000004000000001`
  (expected `0xdeffffff`, got `0xdf000000`) [MEASURED]. That is the
  `Float_FromGPR_S` bug, §10.

### 5.2 Costs [MEASURED]

Each kernel converts and comes back (`mtvsrd; xscvsxddp; xsadddp`, ≈14 cycles of
the total) so that only the candidate differs:

| FCVTZS x,d candidate | instructions | cycles (chain) |
|---|---|---|
| JIT today: `xxpermdi; xscmpudp; xscvdpsxds; mfvsrd; bns; li` [CODE `A64FPOps.cpp:86-131`] | 6 | 30.0 |
| `isel` instead of `bns/li` | 6 | 31.5 |
| `xvcmpeqdp` mask + `xxland` (branch-free, no CR) | 6 | 31.7 |
| no NaN fix (control) | 5 | 29.7 |
| same but value already in doubleword 0 (no `xxpermdi`) | 5 | **23.5** |
| FCVTZU x,d: `xxpermdi; xscvdpuxds; mfvsrd` | 3 | 29.7 |

The predicted-not-taken branch is 1.5 cycles *cheaper* than `isel` (an `isel`
adds a dependent 2-cycle ALU op with the +3 CR-source penalty [SPEC UM Table A-1]).
The permute that positions element 0 costs 6.5 cycles here (3-cycle permute plus
the V-dispatch issue penalty of the following op).

| FCVTNS x,d candidate | instructions | cycles |
|---|---|---|
| P8 sandwich (`mffs; mtfsb0×2; xsrdpic; mtfsf`) + convert (today) | 11 | **57.6** |
| P9 sandwich (`mffscrni; xsrdpic; mffscrn`) + convert | 9 | 51.3 |
| shadow FPCR test + `xsrdpic` + convert | 11 | 39.2 |
| **`fcmpu; fctid; mfvsrd; isel`** (valid when RN = nearest; `fctid` rounds with RN, p.157) | 5 (+4 shadow test) | **31.5** |
| FCVTAS: `xsrdpi` + convert | 6 | 38.6 |

| SCVTF d,x / d,w candidate | instructions | cycles |
|---|---|---|
| JIT today: `mtvsrd; xscvsxddp; vspltisw; xxpermdi` [CODE `A64FPOps.cpp:146-151`, `PlaceElement0` :73-76] | 4 | 25.5 |
| `mtvsrd; xscvsxddp; xxpermdi Z` | 3 | 25.5 |
| W source, JIT: `extsw; mtvsrd; xscvsxddp` | 3 | 20.8 |
| W source: `mtvsrwa; xscvsxddp` | 2 | **19.0** |
| x86 path shape `std; lfd; fcfid` (stack bounce) [CODE `VectorOps.cpp:4536-4560`] | 3 | 36.8 |

| FCVT d,s candidate | instructions | cycles |
|---|---|---|
| JIT today: `xxsldwi; xscvspdpn; xscmpudp; bns; (cold: LoadConstant, mtvsrd, vor); vspltisw; xxpermdi` [CODE `A64FPOps.cpp:340-353`] | 7 hot | **18.0** |
| `xxsldwi; xscvspdp; xxpermdi Z` (signalling convert quiets the sNaN itself [MEASURED PASS], p.561) | 3 | 21.0 |

The shorter sequence is slower by 3 cycles because `xscvspdp` is a 5–7-cycle DP
op while `xscvspdpn` is a 3-cycle ALU2 op and the NaN branch is off the path
[SPEC UM Table A-1]. Choose by what the block is bound by; in a dispatch-bound
block the 3-instruction form wins, in a latency-bound one the current form does.

### 5.3 Half precision

`xscvhpdp`/`xscvdphp` (ISA 3.0, p.550/p.538) match FPConvert exhaustively for
h→d (65 536 inputs) and on the corpus for d→h in all four rounding modes
[MEASURED `fcvt_dh.xscvhpdp`, `fcvt_hd.xscvdphp` PASS]. The POWER8 GPR sequences in
`A64FPOps.cpp:169-324` were not timed (they cannot be run outside the JIT); the
census has no half-precision arithmetic in any static program, so leave them.

### 5.4 Fixed rules from the measurements

- Never write FPSCR for a conversion whose rounding is explicit or truncating.
- Keep the integer in a GPR between a float→int and an int→float step; see §5.5.
- Drop `vspltisw` from `PlaceElement0` (pinned zero exists); prefer `mtvsrwa/wz`
  for 32-bit sources; use the `fctid` short path for FCVTNS when the shadow test
  says RN = nearest.

### 5.5 Forwarding-restriction flushes [MEASURED][SPEC]

UM 25.1.6.2 lists dependent pairs that flush instead of forwarding: convert
float→int feeding an FP input; FP output feeding convert int→float; 64-bit FP
result feeding a 32-bit FP input; 32-bit FP result feeding a 64-bit FP input.

| pair | cycles per pair | `pm_flush` per pair |
|---|---|---|
| `xscvdpsxds` → `xscvsxddp` directly in VSRs | **49.1** | 0.51 (P9 build), 0.57 (P8 build) |
| same through a GPR (`mfvsrd`/`mtvsrd`) | 23.2 | 0.0001 |
| `xscvdpsp` → `xsadddp` | 14.9 / 29.1 | 0.04 / 0.28 (two builds of identical code: the rate varies run to run) |
| `xscvdpspn` → `xscvspdpn` → `xsadddp` | 16.0 | 0 |
| `xvadddp` → `xvcvdpsp` → `xvcvspdp` | 19.0 | 0 |

A flush costs about 50 cycles here. The JIT's GPR round trip is the right
shape; the native `cvtmix` binary shows what happens otherwise (§2.1). When a
guest `SCVTF` consumes the result of an `FCVTZS` through an X register the pair
is already separated; the risk is only in fused lowerings that would keep the
integer in a VSR.

---

## 6. FMIN/FMAX, fused multiply-add, FABS/FNEG/FSQRT (question 4)

### 6.1 FMIN/FMAX and the NM forms [MEASURED][SPEC]

Controls over 166 481 double pairs (all edge pairs plus random):

| host instruction | vs FMAX / FMIN | vs FMAXNM / FMINNM |
|---|---|---|
| `xsmaxdp`/`xsmindp` (2.06, p.584/590; `xvmaxdp/xvmindp/xvmaxsp/xvminsp` lanes behave identically) | 8 475 mismatches: returns the number for a lone qNaN | **16 mismatches**, all `(qNaN, sNaN)`: returns op1, ARM wants `quiet(op2)` |
| `xsmaxjdp`/`xsminjdp` (3.0, p.588/594) | 8 506: propagates NaNs but does not quiet an sNaN | 16 965 |
| `xsmaxcdp`/`xsmincdp` (3.0, p.586) | 12 749: C semantics, and −0/+0 is order-dependent | 12 748 |

So: `xsmaxdp` is ±0-correct (max(+0,−0)=+0, min = −0 [SPEC p.584 "The maximum of
+0 and −0 is +0"; MEASURED]) and NaN-correct for FMAXNM except the same
qNaN/sNaN shape as the arithmetic ops; for FMAX/FMIN it is correct whenever no
operand is a NaN. The lowering is therefore the §3.4 pre-check with a cold block
that implements FPMax/FPMin/FPMaxNum/FPMinNum's NaN rule (the `p_fix_*` and
`p_fixnm_*` candidates in `fpsem.c` PASS on the whole corpus). ISA 3.0's J/C
forms buy nothing.

### 6.2 Fused multiply-add [SPEC][MEASURED]

ARM: FMADD = a + n·m, FMSUB = a + (−n)·m, FNMADD = (−a) + (−n)·m, FNMSUB =
(−a) + n·m, one rounding, with FPNeg applied to the operands *before*
FPMulAdd (it flips a NaN's sign too), and FPProcessNaNs3 in the order
(addend, n, m) plus the "qNaN addend with Inf×0 product → default NaN" rule
(Arm ARM FPMulAdd). POWER a-form: `xsmaddadp T,A,B`: T ← A·B + T; `xsmsubadp`:
T ← A·B − T; `xsnmaddadp`: T ← −(A·B + T) and `xsnmsubadp`: T ← −(A·B − T) with
the negation **after** rounding [SPEC p.575, 596, 613, 624: "result ←
NegateDP(RoundToDP(RN, v))"].

Measured against the model over 1 000 188 triples × 4 modes:

| lowering | mismatches | what differs |
|---|---|---|
| FMADD → `xsmaddadp(T=a, A=n, B=m)` | 17 245 | NaN only: POWER prefers a *quiet* addend over a *signalling* multiplicand (e.g. n=0, m=sNaN, a=qNaN gives a; ARM gives quiet(m)); and a qNaN addend with Inf×0 returns the addend, ARM the default NaN |
| FNMSUB → `xsmsubadp(T=a)` | 125 067 | NaN only: ARM negates the addend first, so a NaN addend comes back with its sign flipped; POWER passes it through |
| FMSUB → `xsnegdp n; xsmaddadp` | 17 749 | NaN only (as FMADD) |
| FNMADD → `xsnegdp n; xsmsubadp` | 124 563 | NaN only (as FNMSUB) |
| FMSUB → `xsnmsubadp` (control) | **412 979** | sign of an exact zero (0·0−0: ARM +0, POWER −0), directed-mode magnitudes, NaN sign |
| FNMADD → `xsnmaddadp` (control) | 520 932 | same |
| all four with the post-check cold path | 0 | |

Hence: use only the plain fused forms with `xsnegdp` on the multiplicand
(`xvnegdp`/`xvnegsp` for the lane forms), never the `xsnm*` forms, and always
attach the NaN cold path. The frontend already does the operand negation
[CODE `TranslateFP.cpp:345-376`]; what is expensive is its 30-IR-op branch-free
NaN and Inf×0 handling.

Costs: `xsmaddadp` 5.97 cycles; `xvmaddadp` 6.39; negate + `xsmsubadp` +
post-check 6.62; copy addend + `xsmaddadp` + post-check 6.66 (the a-form
overwrites the addend, so the cold path needs a copy unless the RA gives a fresh
destination); pre-check on all three inputs (`xscmpudp cr1,n,m; xscmpudp
cr5,a,a; cror; bso`) 6.58 [MEASURED]. A NaN result implies a NaN input or an
invalid product, and the invalid products give the default NaN on both
architectures, so the post-check on the result is complete.

### 6.3 FSQRT, FABS, FNEG, FCVT

- `xssqrtdp`/`xvsqrtdp`/`xvsqrtsp`: sNaN quieted with payload, negative → default
  NaN `0x7FF8…`, −0 → −0: ARM-exact with no fix [MEASURED PASS, 20 260 inputs ×
  4 modes each]. Latency 37 cycles (`xssqrtdp`) vs 38 (`xvsqrtdp`); 4-chain
  throughput 14.9 vs 16.5 cycles per op; SP 10.4 vs 12.0 [MEASURED].
- `xvabsdp`/`xvnegdp` (2 cycles, bit ops, no NaN interaction) are right as they are
  [CODE `VectorOps.cpp:375-397`].
- FCVT d↔s: §5.2; both `xscvdpsp` and `xscvspdp` quiet an sNaN and keep the
  top payload bits, matching FPConvertNaN [MEASURED PASS in all modes].
- Denormal operands and results carry no penalty on POWER9 (`xsmuldp` with a
  min-denormal operand 12.04 cycles vs 12.12 normal; `xsadddp` on denormals
  6.17) [MEASURED], so FPCR.FZ=0 (the default) costs nothing to honour and
  FZ=1 would need explicit flushing if ever emulated.

---

## 7. FCMP, FCCMP, FCSEL (question 5)

### 7.1 NZCV from the compare [SPEC][MEASURED]

`fcmpu`/`xscmpudp` set exactly one of LT, GT, EQ, SO (unordered) [SPEC p.165,
p.534]. FPCompare's NZCV is LT→1000, EQ→0110, GT→0010, unordered→0011, so
N = LT, Z = EQ, C = ¬LT, V = SO — the mapping `DEF_OP(FCmp)` already uses
[CODE `ALUOps.cpp:4199-4243`], and the `xscmpudp`/`fcmpu` candidates match the
model on the whole corpus incl. f32 via `xscvspdp` [MEASURED PASS]. Every A64
condition after an FCMP is one CR bit or a `cror` of two, no XER needed:

| cond | after FCMP | CR0 test |
|---|---|---|
| EQ / NE | Z | `eq` / `¬eq` |
| MI / PL, CC(LO) / CS(HS) | N / ¬N, ¬C / C | `lt` / `¬lt` |
| VS / VC | unordered | `so` / `¬so` |
| GT | ordered and greater | `gt` |
| LE | ¬GT | `¬gt` (includes unordered, as ARM) |
| GE | N = V: EQ or GT | `cror t,gt,eq; t` |
| LT | N ≠ V: LT or unordered | `cror t,lt,so; t` |
| HI | C and ¬Z: GT or unordered | `cror t,gt,so; t` |
| LS | ¬C or Z: LT or EQ | `cror t,lt,eq; t` |

Composed conditions must be written to a different CR field than the compare's
(handbook rule 3) so the compare can be reused by several consumers.

### 7.2 Costs [MEASURED]

Per FCMP + conditional branch (branch never taken), cycles in a chain:

| sequence | instructions | cycles |
|---|---|---|
| JIT today, P8 consumer: 2×`xxpermdi`, `xscmpudp cr0`, lift (`mfocrf`, `rlwinm`, `xori`, `addic`, `rlwinm`, `sldi`, `addo`), projection (`mfxer`, `rlwinm`, `mtocrf`), `cror`, `bc` | 16 | **10.0** |
| same with the P9 projection (`mcrxrx`) | 14 | 7.9 |
| direct: 2×`xxpermdi`, `xscmpudp cr0`, `cror`, `bc` | 6 | 6.9 |
| direct, operands already in doubleword 0 | 4 | 6.7 |
| the XER lift alone (7 instructions, independent) | 7 | 2.2 |

The lift is cheap in isolation because it is independent work; the projection
back through `mfxer` (cracked, 3-cycle) or `mcrxrx` is what a consumer pays.
Keep the lift only where NZCV escapes the block (a later `MRS NZCV`, `CSEL`
family, or a block exit with live flags).

XER.SO: `addo` setting OV also sets the sticky SO. POWER9 speculates that SO
does not change; **each change of SO costs two flushes, 147 cycles per
set/clear pair** [MEASURED `so_toggle`; SPEC UM p.342]. The backend never clears
SO (`ZeroCAOV` is `addco`, the fallback masks keep SO) [CODE
`PPC64Emitter.h:535-543`], so it is set once per thread and stays. Keep it that way.

### 7.3 FCSEL

`NZCVSelectV` is `vmr; bc; vmr` [CODE `ALUOps.cpp:2821-2844`]. With a
data-dependent condition (random doubles against 0.5, 43 % mispredicted) the
select costs 17.0 cycles; the same loop with a predictable condition costs 8.0,
so each mispredict is ≈21 cycles [MEASURED]. Branch-free forms in the same loop:

| select | instructions (beyond the compare) | cycles | ISA |
|---|---|---|---|
| branch, predictable | 3 | 8.03 | |
| branch, 43 % mispredicted | 3 | **16.96** | |
| `xvcmpgtdp` + `xxsel` (compare fused into the select; the FCMP is then only needed for other consumers) | 2 | **8.80** | 2.06 |
| `setb r,cr0; mtvsrdd v,r,r; xxsel` | 3 | 9.11 | 3.0 (`setb` p.120, `mtvsrdd` p.113) |
| `mfocrf; rlwinm; neg; mtvsrd; xxpermdi; xxsel` | 6 | 9.64 | 2.01/2.07 |

`fsel` (p.166) selects on `FRA ≥ 0` with +0 = −0 and NaN → false: it cannot take
a CR condition and is FPR-only; not useful here. For the fused form the mask
conditions are: GT→`xvcmpgtdp a,b`; GE→`xvcmpgedp a,b`; EQ→`xvcmpeqdp`; LT/LE/HI/LS
are the complements or swapped forms (ARM LT includes unordered, so LT ≡
¬GE(a,b) with the select operands swapped). cmpsel takes 10.7 M mispredicts
against 4.1 M native (+6.6 M × ~21 cycles ≈ 1.4 % of its cycles) [MEASURED];
the bigger win is the instruction count, which the fused form also halves.

FCCMP (6.5 IR ops, rare: 2 static in fmix32, none in the census) is fine as is.

---

## 8. FPR-half vs VMX-half, moves, permutes (question 6) [MEASURED]

| kernel | cycles |
|---|---|
| `fadd` f12 chain / `xsadddp` on vs12 / `xsadddp` on vs40 | 6.01 / 5.99 / 6.00 |
| `xsadddp` alternating halves (vs12 ↔ vs40) | 6.01 per op |
| `xxlor` move between halves | 2.0 |
| `mtvsrd` → `mfvsrd` → `addi` chain | 6.0 (2 + 2 + 2) |
| `std` → `lfd` (stack bounce, GPR→FPR through memory) → `mfvsrd` → `addi` | **17.2** |
| `stfd` → `ld`, `stfs` → `lwz` | 17.2 |
| `mtvsrd` → `xsadddp` → `mfvsrd` | 10.98 |
| `mtvsrd` → `xvadddp` → `mfvsrd` | 12.71 (**+1.7**: UM 25.1.5.4's "+1 for a non-synchronous producer feeding a V op") |
| `xxpermdi`, `xxsldwi`, `vsldoi` (latency) | 3.0 |
| `xxpermdi` → `xsadddp` → `xxpermdi` (positioning round trip) | 13.6 |
| `xsdivdp` / `xvdivdp` latency; 4-chain throughput | 28.0 / 29.0; 10.9 / 12.0 per op |
| `xsdivsp` / `xvdivsp` | 23.0 / 26.0; 8.6 / 10.5 |
| `xvdivdp` with a NaN or denormal in the other lane | 29.0 (no penalty) |

Conclusions: (1) which half a scalar lives in is irrelevant, and moving between
halves is a 2-cycle logical op, so pinning more guest V registers into vs0–vs31
(the M0 TODO in `PPC64Emitter.h:322-333`) costs nothing at the ALU; (2) the
element-0-in-doubleword-1 layout forces either lane (`xv*`) forms, which cost
+0.4 cycles on add/mul and 10–20 % of divide/sqrt throughput, or a permute round
trip that costs more (+6.5 to +7.6 cycles); keep the lane forms; (3) the
stack-bounce moves in the x86-shaped ops (`Float_FromGPR_S`) cost 11 cycles
more than `mtvsrd`, and the UM's stated forwarding hazard between GPR-typed
stores and FPR loads did not show as flushes here, only as latency; (4) after
a `mtvsrd`, prefer an `xs*` consumer (conversions already do).

---

## 9. Scalar integer patterns next to FP code (question 7) [MEASURED]

In `nbody+0xbe0`: 38 `lxvx` + 24 `stxvx` (`addi r6,r27,offset` + indexed) and
25 `std` + 29 `ld` for a block with 36 guest instructions: the guest keeps its
bodies in d16–d31 (`LoadContext FPR, #0x310…#0x350` in the IR dump), which are
not pinned (only V0–V15 are [CODE `PPC64Emitter.h:322-345`]), and X9–X17/X25–X28
in the context. LSU stalls are 18 % of nbody's cycles and 17.6 % of lu's;
store-forwarding stalls 2.0 %/0.4 %; no `mfspr`/XER use appears on the FP paths
beyond the FCMP lift (§7.2). Pinning V16–V31 into vs0–vs31 with VSX-form
lowerings is the second-largest item after the NaN fix-ups, and is outside this
study's scope.

---

## 10. The three known backend bugs, precisely

1. **`VFNMLA`/`VFNMLS` negate after rounding** [CODE `VectorOps.cpp:3733-3771`]:
   lowered to `xvnmsubadp`/`xvnmaddadp`, whose result is `−(round(A·B ∓ T))`
   [SPEC p.613/624]. IR.json defines VFNMLA = −(V1·V2) + Add and VFNMLS =
   −(V1·V2) − Add as single-rounding fused ops of *negated products*. Differences
   [MEASURED, the `xsnmsubadp_ctl`/`xsnmaddadp_ctl` controls, 412 979 / 520 932
   of 1 000 188 cases]: (a) sign of an exact zero in round-to-nearest, e.g.
   V1=0, V2=0, Add=+0: VFNMLA should be −0·0 + 0 = +0, the host gives −(0 − 0) =
   −0; (b) directed modes: −round_up(x) ≠ round_up(−x); (c) NaN sign. Correct
   sequences: VFNMLA = `xvnegdp t,V1; xvmaddadp T(=Add),t,V2`;
   VFNMLS = `xvnegdp t,V1; xvmsubadp T(=Add),t,V2` (one rounding, negation exact).
   The A64 frontend never emits VFNMLA/VFNMLS (it negates operands and uses VFMLA
   [CODE `TranslateFP.cpp:365-376`]), so this affects the x86 path only.
2. **`Float_FromGPR_S` i64→f32 rounds twice** [CODE `VectorOps.cpp:4536-4560`]:
   `fcfid` then `frsp`. Counterexample [MEASURED `scvtf_sx.fcfid_frsp_ctl`]:
   input `0x8000004000000001` (−2^63 + 2^38 + 1): the double rounding lands on
   `0xdf000000` (−2^63), the single rounding on `0xdeffffff`. Correct: `fcfids`
   (ISA 2.06, p.162) or `mtvsrd; xscvsxdsp` (2.07, p.564), then `xscvdpspn` for
   the SP bit pattern, as `A64FloatFromGPR` already does [CODE `A64FPOps.cpp:155-157`].
   The stack bounce it uses also costs 11 cycles more than `mtvsrd` (§8).
3. **32-bit `VAddV` uses `vsumsws`** [CODE `VectorOps.cpp:586-588`]: `vsumsws`
   is a *saturating* signed sum (VSCR.SAT) [SPEC vsumsws, Vector Sum Across
   Signed Word Saturate]; ADDV.4S wraps modulo 2^32. Correct:
   `vsldoi t,V,V,8; vadduwm t,V,t; vsldoi u,t,t,4; vadduwm t,t,u` then place
   word 0 (all modular). Not an FP issue; listed for completeness.

---

## 11. Recommended sequences, consolidated

All are ISA 2.06/2.07 unless marked 3.0; `Z` = pinned zero VSR (vs14), `crS` a
scratch CR field, element 0 of guest registers in VSX doubleword 1 (low LE
half), f32 in the low word.

| guest | POWER8 path | POWER9 addition |
|---|---|---|
| FADD/FSUB/FMUL/FDIV Dd | `xvcmpeqdp. t,Da,Da; bc 4,4*cr6+lt,cold; xvcmpeqdp. t,Db,Db; bc …; xv{add,sub,mul,div}dp Dd,Da,Db; xxpermdi Dd,Z,Dd,1` (or the `xscmpudp` pre-check when operands are in dw0; or the post-check when the RA keeps the sources) | same |
| FADD/… Sd | `xvcmpeqsp.` ×2 + `bc` ×2 (or post-check `xxsldwi; xscvspdpn; xscmpudp; bso`), `xv{add,sub,mul,div}sp`, zero the other 96 bits | same |
| FNMUL | as FMUL + `xvnegdp`/`xvnegsp` | |
| FMADD / FMSUB / FNMSUB / FNMADD | `[xvnegdp t,Dn]`; `xvmaddadp` (FMADD, FMSUB with −n) / `xvmsubadp` (FNMSUB, FNMADD with −n) with T = copy of Da; `xscmpudp crS,Dd,Dd` on the dw0-positioned result or `xvcmpeqdp.`; `bso cold`; zero upper | |
| FMAX/FMIN, FMAXNM/FMINNM | pre-check as FADD; `xvmaxdp/xvmindp` (`xvmaxsp/xvminsp`); cold block per §6.1 | (`xsmaxjdp` etc. not useful) |
| FSQRT, FABS, FNEG | `xvsqrtdp/sp`, `xvabsdp/sp`, `xvnegdp/sp` (no fix) | |
| FCVT s←d, d←s | `xscvdpsp` / `xscvspdp` (+ positioning), both quiet an sNaN | |
| FCVT h↔d/s | GPR code (existing) | `xscvhpdp`/`xscvdphp` (3.0) |
| FCVTZS/FCVTZU x/w | `xscvdps/uxds`, `xscvdps/uxws`; signed adds `xscmpudp crS,x,x` + `bns/li` or `isel r,0,r,4*crS+3`; `mfvsrd`/`mfvsrwz` | |
| FCVTAS/AU, FCVTPS/PU, FCVTMS/MU | `xsrdpi` / `xsrdpip` / `xsrdpim` then as FCVTZ* | |
| FCVTNS/NU (ties even) | `lwz; rlwinm; cmplwi crS; bne slow` on the FPCR shadow; fast: `fcmpu; fctid; mfvsrd; isel` (signed) or `xsrdpic` + `xscvdpuxds` (unsigned); slow: `mffs; mtfsb0 30; mtfsb0 31; xsrdpic; mtfsf 1,f0` | slow: `mffscrni f0,0; xsrdpic; mffscrn f0,f0` (3.0B) |
| FCVTZS/ZU #fbits | `xsmuldp` by 2^fbits, then as FCVTZ* | |
| SCVTF/UCVTF d,x / s,x | `mtvsrd; xscvsxddp/xscvuxddp` / `xscvsxdsp/xscvuxdsp; xscvdpspn`; then `xxpermdi Dd,Z,t,0` | |
| SCVTF/UCVTF d,w / s,w | `mtvsrwa/mtvsrwz` instead of `extsw/clrldi; mtvsrd` | |
| fixed-point SCVTF #fbits | as now (`xsmuldp` by 2^−fbits after the single rounding, exact) | |
| FCMP/FCMPE | `xscmpudp cr0` on dw0-positioned operands; consumers read CR0 per §7.1; lift to XER only when NZCV escapes | |
| FCSEL | `mfocrf; rlwinm; neg; mtvsrd; xxpermdi; xxsel` or, fused with the preceding FCMP on the same operands, `xvcmpg[te]dp/eqdp; xxsel` | `setb; mtvsrdd; xxsel` (3.0) |
| MSR FPCR | as now (`mtfsf 0xFF`); accept 36+ cycles, it is rare | `mffsl` for the read side |

The cold blocks (binary NaN rule, FMA NaN rule with Inf×0, min/max NaN rule)
should be shared out-of-line routines or a JIT helper call rather than inline
per site; their cost is irrelevant (an ordered-data workload never enters them)
and a single implementation is easier to keep corpus-verified.

---

## 12. What to verify when implementing

1. Re-run `fpsem.c` logic against the *emitted* code: build small guest
   programs per instruction group (the frontend's `unittests/A64Frontend`
   generators) with the §1 corpus embedded, compare against the Pi 5 captures, and
   keep the positive controls (a build flag that disables the cold path must make
   the qNaN/sNaN rows fail).
2. The compare's lane: whichever placement is used (`xscmpudp` on dw0 or
   `xvcmpeqdp.` over both lanes), verify with a NaN *only in the garbage lane*
   that the fast path still produces the right value (it will take the cold
   path; the result must be identical).
3. Destination aliasing: when the RA ties Dd to Da or Db (or the FMA addend), the
   cold path must see the original operands; test with the aliasing shapes the RA
   actually produces (`IR dump` the block).
4. All four rounding modes for every path, including the shadow-FPCR test for
   FCVTNS: set FPCR.RMode to RP/RM/RZ in the guest and run the tie-even corpus;
   the `assumeRN_ctl` controls in `fpsem.c` show what going wrong looks like
   (7 795 mismatches).
5. CR field discipline: the pre-check must not write CR0 (NZCV) and must not leave
   a composed condition in the compare's own field (handbook rule 3).
6. FPSCR: assert with `pm_cmplu_stall_mtfpscr` and `pm_flush` in the workload
   profile that the hot paths write FPSCR zero times and take no forwarding
   flushes (`fwd_cvt2fp_direct` is the shape to avoid).
7. Re-run `probes/work/runperf.sh` after each item and compare against §2.1; the
   Pi checksums in `logs/perf/pi5-reference.log` are the parity reference.
8. Implement FRINT{N,P,M,Z,A,X,I} (currently unimplemented): `xsrdpi[cpmz]` cover
   A/P/M/Z; N needs the tie-even path of §5.2; X/I are `xsrdpic` (X must raise
   Inexact once FPSR flags are emulated).

---

## 13. Surprises and open points

- The **branch-free NaN fix-up costs more than the arithmetic itself, three
  times over**, and its constants are rebuilt for every op. That, not the
  compare-and-branch that was avoided, is the 20× (§2.3).
- **A predicted-not-taken branch is cheaper than `isel`** on POWER9 (1.5 cycles),
  and the pre-check's compare is free because it is independent of the add.
- `xsmaxdp` is *almost* FMAXNM (16 corpus cases short) rather than FMAX; the
  ISA text "the maximum of any value and an SNaN is that SNaN" does not hold
  when the other operand is a qNaN [MEASURED vs SPEC p.584].
- POWER's fused multiply-add prefers a quiet addend over a signalling
  multiplicand, the opposite of ARM's order; the sign of a NaN addend differs
  in the negated forms. Every FMA form needs the cold path.
- `mffsl` is 0.7 cycles; `mffs` is 14; a full `mtfsf` in an FP stream is 140.
- `xscvspdpn` really is bit-preserving for sNaN and the JIT comment is right;
  the harness initially said otherwise because the compiler quieted the input.
- Denormals are free on POWER9; a garbage NaN/denormal lane is free.
- A direct VSR dependency from a float→int convert into an int→float convert
  flushes (≈50 cycles); clang's own native ppc64le code for `cvtmix` pays it and
  is 3.5× slower than the Pi on that kernel.
- FRINT* is unimplemented in the frontend; glibc's `lrint`/`floor` hit it.
- Not measured: POWER8 hardware timings; the half-precision GPR paths; FCCMP;
  the effect of the recommendations on wall time (projection only).
