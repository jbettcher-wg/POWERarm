# Warm codegen: where the next gains in warm emulation speed are

Design research, 2026-09-17. No code, no builds: everything below was read out of
the tree at `dcb8da7e1` (line numbers are from that commit), the main tree's
existing `build-powerarm/Bin/POWERarm` (built 22:39 from the same sources), the
code-cache segment files that binary writes, and `perf` against it. Companion to
`OPTIMIZATION-CHECKLIST.md` (P/F/N rows) and to
`research/cold-translation/COLD-TRANSLATION-RESEARCH.md`, which owns everything
that happens before a block is warm.

**One-paragraph answer.** On the reference workloads a warm run is 66-91% translated
code, and the translated code executes ~2.4x the instructions of native ppc64le at
0.8x its IPC. Four things explain most of that, in this order of leverage across the
set. (1) The code buffer is 67% cold bytes interleaved with hot ones: every constant
exit carries a 104-byte link thunk plus record, and every compile unit carries one or
two 260-byte copies of `SpillStaticRegs`; guest instruction bodies are 19% of the
102 MB `cc1` buffer and 19% of the 97 MB `node` buffer. L1 icache demand misses are
5.9x native and branch-redirect L2 fetches 5.5x. Moving those bytes out of the hot
stream needs no allocator work and helps every workload. (2) No A64 compare-and-branch
is ever fused: `CompareBranchFusion` accepts only x86's `OP_SUBWITHFLAGS`, the A64
frontend emits `SubNZCV` for `CMP`, so `cmp w0,#n ; b.cc` costs 4 + 3.4 host
instructions with an XER projection where native does `cmpwi ; bc`. (3) A paired
`BL` executes 13 host instructions and a `RET` 14; 3.6% of `cc1`'s executed guest
instructions are calls and returns. (4) Register-copy and zero-extension debris:
`mr` is 7.1% and `clrldi ..,32` 3.6% of executed host instructions, and the `MOV`
alias (`orr xd,xzr,xm`) materialises a zero and ORs it. Context-backed GPR traffic
(the P1(b) row) is 2.8% of executed host instructions on compiled code and spills are
nil, so P1(b) is the lever for register-bound hot loops (A64Bench `crc32`), not for
the compiler or the JS engines. Compounded, the four items above are the owner's
20-30% on `cc1 -O2 lvm.c` (2.65x the Pi today, warm); each is estimated, not
measured, and section 8 says what a standard agent should instrument to price them
exactly.

## 1. Method and caveats

- Emulator: `/mnt/arch/home/jbettcher/Development/POWERarm/build-powerarm/Bin/POWERarm`,
  never promoted, never through binfmt; every run passes `POWERARM_PORTABLE=1` with an
  absolute `POWERARM_ROOTFS`, a private `POWERARM_APP_CACHE_LOCATION`, and is
  wrapped in `env -u LD_LIBRARY_PATH PATH=/usr/local/bin:/usr/bin`. Host `perf` is
  `/mnt/arch/usr/bin/perf` with `LD_LIBRARY_PATH=/mnt/arch/usr/lib`.
- **Contended machine.** Another agent built on CPUs 88-160 throughout. Runs are
  pinned to CPU 40 (`cc1`, native gcc) and CPUs 44-47 (Claude, code-server), one run
  per configuration. Shares and counter ratios are the numbers to trust; wall times
  are direction only.
- **Two env traps found on the way** (recorded so the next agent does not lose an
  hour): config options take the `POWERARM_` prefix, not `FEX_`
  (`Source/Common/Config.cpp:288-289`; `FEX_APP_*` are separate), so
  `FEX_BLOCKJITNAMING=1` silently does nothing. And every config option is hashed
  into the cache `ConfigId` (`CodeCache.cpp:355`), so adding
  `POWERARM_CODECACHESTATS=1` to a run made code-server's `node` recompile from
  scratch under a new namespace while the run looked warm. Check the
  `loaded N` line the stats print before believing a warm number.
- **Warmth per workload.** `cc1`: second run on the same private cache (`loaded`
  covers every reached block; `save 0.04 ms`). Claude Code: the binary cannot be
  cached (cold research 4.7), so a "warm" run still translates its own 25k units;
  the translated-code share is separated by DSO in the profile, and the
  translated-code numbers are steady state (a `-p` prompt runs 21 s of guest code
  after ~2 s of translation). code-server: `POWERARM_CODECACHESCOPE=all`, a second
  launch on a namespace written by the first (`loaded 97242`), then a 15 s request
  loop before any counter is read. What remains untranslated there is V8's own
  generated code, which no cache can hold (section 3.3).
- **Deterministic census without instrumentation.** Format-v5 cache segments
  (`CodeCache.cpp:836-910`) store each unit's host bytes as laid out, the
  `JITCodeTail` and its vl64pair RIP table (`CPUBackend.h:69-91`,
  `Utils/variable_length_integer.h`), so a script
  (`scratchpad/warm/census.py`) recovers, per unit: host bytes, guest instructions
  (one RIP entry per `Op_GuestOpcode`, `ALUOps.cpp:3912`), the guest word from the
  ELF, and the host instructions between consecutive entries. Link thunks and the
  shared spill stubs are found by their fixed opening words (`b +0x14 ; mflr r3 ;
  ld r4,0x2c(r3)` and `std r7,72(r27)`). Dynamic weighting joins the units with the
  `POWERARM_BLOCKJITNAMING=1` perf map (which names cache-installed units too:
  111,584 names for 109,629 units). Weighting is per unit, so it assumes every
  instruction of a sampled unit ran equally; cold miss legs are counted in the
  "bytes" columns but not in the executed-path counts quoted in section 5.
- Pi 5 references measured once each on `pi5` (load ~1.2): `gcc -O2 -c lvm.c`
  1.90 s wall / 1.87 s user (3 runs), `claude -p` one-word prompt 3.85 s wall /
  1.77 s user (1 run). Other Pi numbers are the recorded ones.
- Not done: A64Bench (binaries are not built in the tree; its recorded numbers are
  used), any instrumented build (section 8 specifies it), the F/N FP paths beyond
  their static share.

## 2. The reference workloads, warm, against the Pi

| Workload | POWER9 warm | Pi 5 | x the Pi | Source |
|---|---|---|---|---|
| `gcc -O2 -c lvm.c` | 5.04 s wall; `cc1` guest 4.91 s, user 4.86 s | 1.90 s | **2.65x** | measured (native ppc64le gcc: 1.70 s, so emulation is 3.0x native cycles) |
| slice (10 Lua objects + ar/ld) | 21.4-21.6 s | 6.3 s | **3.4x** | recorded |
| Claude Code, one `-p` prompt | user 21.5 s (guest 22.5 s) | user 1.77 s | **12.1x** as it runs; ~9.7x net of the 19% translation share (estimated) | measured |
| code-server first `/healthz` | 2.35 s | 0.51 s | **4.6x** | recorded (cold research) |
| code-server request loop, 15 s window | IPC 0.66; 66% translated code, 32% translating V8-generated code | no Pi number | n/a | measured, section 3.3 |
| A64Bench crc32 / sha256 / vm / sort / bst | 402 / 960 / 1742 / 970 / 540 ms | 197 / 293 / 428 / 282 / 442 | 2.04x / 3.3x / 4.1x / 3.4x / 1.22x | recorded (P1 row, P10 row, M1 baseline) |

Parity with the Pi means, on `cc1`, 2.65x fewer cycles: native POWER9 is already
1.12x the Pi on this compile (1.70 vs 1.90 s), so the emulation overhead to remove
is 3.0x -> ~1.1x of native.

## 3. Where warm time goes

### 3.1 By DSO (self time, `perf record -e cycles:u`)

| | `cc1 -O2 lvm.c` warm | Claude `-p` prompt | code-server loop (warm cache) |
|---|---|---|---|
| translated guest code | **91.3%** | **79.8%** | **66.3%** |
| POWERarm itself | 6.6% | 19.1% | 32.3% |
| of which translation (`CompileCode`, RA, decoder, DFCE, fusion, high-zero walk, `Op_*`) | ~0 (everything installs from cache) | ~12% | ~20% |
| of which cache install and link (`ApplyCodeRelocations`, `TryLoadBlock`, `FindBlock`, `AddBlockMapping`, `AddBlockLink`, `CacheBlockMapping`) | ~3.5% | ~1.5% | ~2% |
| of which SMC invalidation (`InvalidateCodeBuffersCodeRange`) | 0 | 0 | 0.8% |
| libc (mostly `memcpy` of installed blocks, rwlocks) | 0.9% | 0.6% | 1.0% |
| kernel (`sys` from `POWERARM_STARTUPTIMES`) | 0.9% (46 ms of 4.9 s) | 1.4% (316 ms of 22.5 s) | small |

So the dispatcher and lookup are not where warm time is: `FindBlock` is 0.4-0.8%,
the L1 probe runs inside translated code and is counted there (its `bctr` is 0.6%
of executed host instructions, section 5). Syscalls are ~1%. The 6.6% on `cc1` is
cache *install* (Q2 lazy install, the cold agent's territory). The 32% on code-server
is V8 compiling JavaScript and POWERarm translating the result: it is steady state
for that workload and it is cold-side work (cold research 4.3, G5); the warm items
below act on the 66%.

### 3.2 Inside translated code: the PMU view of `cc1 -O2 lvm.c`

One `perf stat` per event group, CPU 40, warm; native `/mnt/arch/usr/bin/gcc`
16.1.1 on the same input for comparison. Events with a multiplexing share are
marked (75%/25%/50%).

| Event (`:u`) | POWERarm warm | native ppc64le | ratio | per 1000 instructions, POWERarm / native |
|---|---|---|---|---|
| cycles | 18.57 G | 6.17 G | **3.01x** | |
| instructions | 21.90 G | 9.08 G | **2.41x** | IPC 1.18 / 1.47 |
| `pm_cmplu_stall` (completion stalled) | 8.47 G (45.6% of cycles) | 2.94 G (47.7%) | 2.9x | |
| `L1-icache-load-misses` = `pm_l1_icache_miss` (demand) | 350.1 M | 59.5 M | **5.9x** | 16.0 / 6.6 |
| `pm_ic_demand_l2_br_redirect` (demand fetch from L2 after a branch redirect) | 367.5 M | 66.9 M | **5.5x** | 16.8 / 7.4 |
| `pm_l1_icache_reloaded_all` (demand + prefetch) | 834.0 M | 150.4 M | 5.5x | 38 / 16.6 |
| `pm_l1_icache_reloaded_pref` | 488.0 M | 90.9 M | 5.4x | |
| `iTLB-load-misses` | 0.990 M | 0.451 M | 2.2x | 0.045 / 0.050 |
| branches (75%) | 2.79 G | 1.74 G | 1.6x | 127 / 192 |
| branch-misses (24%); `pm_br_mpred_cmpl` | 53.5 M; 52.8 M | 44.9 M; 43.6 M | 1.2x | 2.4 / 4.9 |
| `L1-dcache-load-misses` (50%) | 99.3 M | 80.3 M | 1.24x | 4.5 / 8.8 |
| `dTLB-load-misses` (75%) | 254 k | 56 k | 4.5x | |

Reading: the 3.0x cycle ratio splits into **2.41x more instructions** and **1.25x
lower IPC**. Data-side behaviour is native-like (dcache misses 1.24x, mispredicts
1.2x, the stalled-cycle fraction is the same 46-48%). The instruction side is not:
per instruction, demand icache misses are 2.4x native and prefetch cannot cover them
(reloads 5.5x). The stall breakdown by cause was not available (this `perf` lists
no `pm_ict_noslot_*` events; section 8 asks for the raw codes), so the cycle cost
of the fetch misses is estimated in section 7, not measured. **This is the new
footprint baseline: 350 M L1 icache misses on `cc1 -O2 lvm.c`, 5.9x native, against
the 1.07 G / 18x recorded before P6, P10, P12 and F1.**

Claude `--help` (71% of its 1.4 s is translation, so only the ratios matter):
IPC 1.13; icache misses 7.0 per 1000 instructions; branch-misses 4.9% of branches;
iTLB 113 per M instructions. code-server loop, warm cache: IPC **0.66**; icache
misses 10.0 per 1000; branch-misses 4.3%; iTLB **243 per M instructions** (5x
`cc1`'s rate); dTLB 1.35 M per 15 s. The JS engines have a worse fetch profile than
the compiler, as expected, and a third of their steady state is compilation.

### 3.3 What "warm" means for the two apps

- Claude Code: each launch retranslates 25k units of its own text (loader bug, cold
  research 4.7.2). In a `-p` prompt that is ~2 s of a 21.5 s CPU budget, so the
  prompt is a warm workload despite the cache never applying. The Pi does the same
  prompt in 1.77 s of CPU.
- code-server: with `node`'s text cached, the request loop still spends 32% of its
  CPU in POWERarm, and the profile says what it is: `CompileCode` 4.7%, RA 5.0%,
  decoder 1.6%, DFCE 1.6%, `InvalidateCodeBuffersCodeRange` 0.8%. V8 keeps
  generating and flushing code, and every new piece is translated from scratch.
  That is the cold agent's G5 (a cheap tier for runtime-generated code), and it
  bounds what the warm items can do for V8 at ~66% of the loop.
- SuperTuxKart (`--benchmark`, ArchLinuxARM-vk rootfs, measured by the
  orchestrator, one run each, direction only): cold compiled 146,856 blocks in
  67.9 s wall, warm compiled 1,274 in 65.8 s. So the cache removed ~145k
  translations and saved ~2 s of a ~30 s load-and-teardown. **Warm install cost does
  not explain the rest and is not in this ranking**: the code-server run above
  installed 97,242 blocks in `lookup-ms 243`, 2.5 us per block, which prices
  147k installs at under 0.5 s. The ~28 s that remain are the game's own loading
  code running as translated code, i.e. G1-G4 territory, plus whatever its GL path
  costs on the thunk side. A game load is also the first FP/NEON-heavy workload
  that exists on this machine; HANDOVER item 5 should consider it once its rootfs
  is stable (a separate agent owns the ~1.2k-blocks-per-launch cache coverage gap).

## 4. The footprint census (fresh baseline)

Static, over every unit the warm `cc1` run installed (109,629 units; the same
census over the 111,796 `node` units the cold agent's code-server run wrote gives
the same shape). Weighted = weighted by each unit's perf samples.

| Region of the code buffer | `cc1` bytes | share | weighted | `node` share | what it is |
|---|---|---|---|---|---|
| link thunks + records | 35.8 MB | **35.0%** | 33.7% | 32.9% | 343,996 x 104 B: 12-word thunk + 7-doubleword `PPC64BlockLinkRecord`, one per linkable constant exit (`JIT.cpp:5908-6030`) |
| shared spill stubs | 33.2 MB | **32.5%** | 28.3% | 34.2% | 127,682 x 260 B: `SpillStaticRegs` (62 instructions, `PPC64Emitter.cpp:38-110`) + 3, emitted per unit for the exit and the link path (`JIT.cpp:6045-6063`); 1.16 copies per unit |
| guest instruction bodies, inline exits included | 19.7 MB | **19.3%** | 26.5% | 19.4% | 12.6 B (3.15 host instructions) per translated guest instruction |
| `JITCodeTail` + vl64pair RIP table + 16 B pad | 7.5 MB | 7.3% | 6.9% | 7.7% | data, read only by C++ (`JIT.cpp:6180-6230`) |
| nop padding | 4.7 MB | 4.6% | 4.0% | 4.4% | `Align16B`, 8-alignment of thunks, `nop` fill after link-record pushes |
| entry-point prologue | 0.9 MB | 0.9% | 0.7% | 0.9% | 2 instructions per entry block: the suspend poke `ld ; stb` (`JIT.cpp:2905-2930`) |
| header word | 0.4 MB | 0.4% | | 0.5% | |

Derived: **65 host bytes per translated guest instruction in the buffer, of which
12.6 are the instruction's own code**; the median unit is 800 B for 10 guest
instructions (`node`: 688 B, 8). The hot stream is one fifth of the bytes fetched
into it. Every constant exit also keeps its unlinked leg inline in the body: the
20-byte fixed guest-RIP window, `std State.pc` and `b LinkPath`
(`BranchOps.cpp:770-800`): `ctx pc st` is 8.5% of body instructions and executes
only on the unlinked path.

Duplication from region formation (`Decoder.cpp:139-157`; 128-byte window, 8
leaders): **1.41 translations per distinct guest instruction on `cc1`** (25.1% of
its guest instructions are translated more than once), 1.29 on `node` (18.9%). That
is ~30% of body bytes and the same share of translation work; it is footprint, not a
warm cycle cost, because only one copy is hot at a time.

FP and NEON are 0.4% of `cc1`'s static guest instructions and 0.5% of `node`'s, so
the F1 per-site stubs and per-unit `NaNFix` bodies do not register in either census
(their whole class is 0.3% of body bytes). Atomics likewise: 4 `LDAR/STLR` sites in
`cc1`, 4,267 in `node` at 3.3 host instructions each (the barrier is 1.7 of them),
28 `CAS`/LSE sites. The CR/XER save-restore around atomics is real (4 instructions per
RMW, `AtomicOps.cpp:507-540`) and dynamically irrelevant on this set.

## 5. Expansion by guest instruction class

Static count and host bytes attributed to each class on `cc1` (range from the
instruction's RIP entry to the next; a branch's range holds its exit sequence).
`wN%` is the class's share of executed guest instructions (unit-sample weighted),
the last column the executed-path host instruction count read out of the lowering.

| Guest class | static n | B/insn | wN% | executed host instructions (typical) | where the bytes go |
|---|---|---|---|---|---|
| `LDR/STR` unsigned imm | 404 k | 5.6 | **22.9%** | 1.4 | 1 D-form load/store; 0.28 `mr` (RA copy), 0.11 context GPR traffic |
| `B.cond` | 123 k | 14.2 | **9.7%** | 2 + 0.57 CR ops when linked | `bc` + `b` (P6 shape); `mcrxrx`/`crand` composites for C/V conditions (`MapNZCVCC`, `JIT.cpp:962-1030`); the 5-word RIP window + `std pc` run unlinked only |
| `ADD/SUB` imm | 117 k | 6.4 | 8.5% | 1.6 | `addi`; 0.27 `mr`, 0.23 `clrldi ,32` |
| logical reg | 132 k | 8.8 | 7.7% | 2.2 | `MOV` alias (`orr xd,xzr,xm`) is `li 0 ; or` (+`clrldi` for W): `TranslateDataProcessing.cpp:384-410` passes `Constant(0)` as Src1 and `DEF_OP(Or)` only folds an inline Src2 zero (`ALUOps.cpp:852-884`) |
| `ADDS/SUBS` imm (`CMP`) | 84 k | 15.4 | **6.4%** | 4 (W), 2 (X) | `sldi ; li+sldi ; subfco.` (`ALUOps.cpp:2158-2166`): C and V are produced in XER by the 64-bit shift trick even when the only consumer is a `B.cond` |
| `LDP/STP` | 96 k | 10.4 | 5.9% | 2.6 | 2 loads/stores + 0.2 address arithmetic + 0.3 copies/context |
| `MOVZ/MOVN/MOVK` | 94 k | 4.6 | 4.2% | 1.1 | |
| `ADD/SUB` reg | 33 k | 9.5 | 4.0% | 2.4 | `add`; 0.50 `clrldi ,32`, 0.49 `mr`, 0.28 shift |
| `CBZ/CBNZ` | 72 k | 14.9 | 3.9% | 2 (`cmpdi ; bc`) + 1 `b` | fine |
| `ADDS/SUBS` reg | 46 k | 8.7 | 3.8% | 2.2 | `sldi ; subfco.` |
| `ADRP` | 49 k | 0.2 | 2.6% | 0 | folded into the consumer's `EntrypointOffset` delta |
| `BL` | 74 k | **48.0** | 2.0% | **13** | `EmitShadowCallPush` (`BranchOps.cpp:683-707`): `bcl ; mflr ; addi ; ld sp ; ld base ; addi ; cmpd ; bc ; std ; std ; std sp`, then `bl`; plus the trampoline `b` |
| `RET` | 20 k | **92.1** | 1.6% | **11 + trampoline + 2 (entry poke)** | pop (`BranchOps.cpp:730-768`): `ld sp ; ld tramp ; ld rip ; mtlr ; cmpd ; addi ; std sp ; bc ; std pc ; blr`; the miss leg is the full L1 probe |
| bitfield | 24 k | 8.1 | 2.0% | 2.0 | `rldic*` + 0.25 `clrldi` |
| `CSEL` family | 16 k | 16.9 | 1.3% | 4.2 | `li ; isel ; clrldi` + XER projection (`ALUOps.cpp:2750-2828`) |
| `TBZ/TBNZ` | 30 k | 18.6 | 1.3% | 3 + 1 `b` | `rldicl ; cmpldi ; bc` |
| `CCMP/CCMN` | 6 k | 53.0 | 0.7% | 13 | `CondSubNZCV`: branchy, `li` x2, XER moves x2 |
| `BLR` / `BR` | 6 k / 0.7 k | 88 / 565 | 0.3% | probe or inline cache | `BR` carries 8 compare-cache slots (P2), 565 B each site |
| scalar FP / NEON | 1.2 k / 2.2 k | 21 / 28 | 0.6% | 5-7 | `NaNFix` shared bodies amortise at 15 instructions per unit |

Executed host instruction mix, `cc1`, unit-sample weighted (what the 21.9 G
instructions are made of, to the extent unit weighting resolves it):

| host class | share | | host class | share |
|---|---|---|---|---|
| integer ALU | 15.2% | | `li`/`lis` immediates | 5.2% |
| guest loads | 11.4% | | `clrldi ,32` (W-form zero-extension) | **3.6%** |
| rotates/shifts | 8.8% | | record-form ALU (flag producers) | 3.6% |
| `b`/`bl` | 7.8% | | compares | 3.2% |
| **`mr` copies** | **7.1%** | | context GPR loads + stores (P1(b)) | **2.8%** |
| `std State.pc` (unlinked exit legs) | 6.7% (static-heavy) | | callret stack loads + stores | 2.4% |
| guest stores | 6.5% | | CR/XER moves + CR logic | 3.0% |
| `bc` | 6.3% | | 5-word constants | 1.1% |
| | | | spill/fill (RA) | **0.02%** |

## 6. Evaluation of the named items

**P1(b), register allocation across block edges.** Evidence: context-backed GPR
traffic is 2.8% of executed host instructions on `cc1` (1.5% loads, 1.2% stores),
spill/fill is 0.02%, and the pinned set covers 94% of references (P1 row). The
allocator's per-block reset (`RegisterAllocationPass.cpp:607-609`) is what blocks it,
as recorded. On compiled C/C++ (all three targets' engines are that) lifting it is a
~3% item; on register-bound loops it is the whole game: `crc32` went 7.35x -> 2.04x
the Pi on P1(a) alone, and the remaining 2x is the same chain through memory across
the loop back edge. Verdict: **not the biggest single lever for the set; the biggest
lever for A64Bench-shaped loops and probably game inner loops.** Cheapest form (cold
research 4.1): keep the RA block-local and let the frontend carry the P1(a) cache
across intra-unit edges by forcing a `StoreRegister`/`LoadRegister` pair at the edge
only for values live across it, so CPUState stays exact and no fault-recovery data is
needed. Gate: `crc32`/`vm` warm; `pm_st_fwd` per guest instruction on `crc32`;
`callret.c` and the three-mode suite.

**P5, flags in CR fields and XER traffic.** This is where the compiler and the
engines pay. Three separate costs, all measured statically above: (a) every `CMP`
is lowered for a consumer that might need C and V (`subfco.` on shifted operands,
4 instructions at W size) although (b) the consumer is nearly always a `B.cond`,
which then projects XER into CR1 (`mcrxrx`, ISA 3.0) and composes CR bits for
`LO/HS/GE/LT/GT/LE`; and (c) `CompareBranchFusion` never fires for A64 because
`IsFusableProducer` tests `Producer->Op != OP_SUBWITHFLAGS`
(`CompareBranchFusion.cpp:163`) while `CMP` reaches it as `SubNZCV`
(`TranslateDataProcessing.cpp:35-62`, `:429-434`). The census confirms it: every
one of the 84 k `ADDS/SUBS imm` sites still carries its `subfco.`, and only 5% of
`B.cond` ranges contain a compare. Extending the producer set to `SubNZCV`/`AddNZCV`
is the same rewrite the pass already performs for x86 (`:284-300`), with the same
soundness argument (CondClass is ARM-sense in both); `MI/PL` stay unfused except
against zero, as today. Second step, for the consumers that are not branches
(`CSEL` 4.2, `CCMP` 13 instructions): a CR-direct `NZCVSelect` when the producer is
a fusable compare is already half-built (the `SelectRewrites` path, `:258-283`).
Third: when a flag producer's only consumers are compares, a W-size `cmpw`/`cmplw`
pair into two CR fields replaces the shift trick outright.

**Spill/fill volume.** 0.02% of executed host instructions, 984 static fills in
`cc1`. Not an item. What the allocator does cost is copies: `mr` is 7.1% of executed
host instructions (SRA coalescing failures on `StoreRegister`, two-address
constraints in `Sub`/`Mask32Tail`, `LDR` results moved into their pinned home), and
`clrldi ..,32` 3.6% (the W-form zero-extension the producer high-zero elision could
not prove). Both are allocator/IR quality, cheap to attack one pattern at a time
(the `MOV` alias first: a frontend one-liner), and need the per-op instrumentation of
section 8 to be priced exactly.

**P8, constant materialisation.** Immediates are 6.4% of executed host instructions,
but most are owned by other items: 1.05 `li` per `CMP imm` (P5), 0.56 per logical
reg op (the `MOV` alias), and the 5-word windows are the sunk exit RIPs, executed
only unlinked. What is left (`MOVZ/MOVK` at 1.1, the delta cache for `ADRP`-relative
addresses at 0.2 per `ADRP`) is already good. Not a lever on its own.

**Exit and link sequences.** A linked constant exit executes `[li r0,0] ; b`
(P12 sink), a `B.cond` `bc ; b`, so the executed cost is the taken branch plus the
target's entry poke (2 instructions), i.e. ~3-4 host instructions and one fetch
redirect **per unit boundary**, and units are 8-10 guest instructions. That is the
0.3 host instructions per guest instruction and the 5.5x branch-redirect fetches
that the PMU sees. The static cost is the item: 32 hot-stream bytes of unlinked leg
per exit plus 104 cold bytes of thunk and record, plus the per-unit stubs. `BL` and
`RET` are the expensive exits by far (13 and 14 executed): the push does a bounds
check the pop avoids by guard page, computes the trampoline address with
`bcl ; mflr ; addi` at run time, and loads/stores `callret_sp` from the frame
(`CoreState.h:114`) on every call and return; `callret_sp` in a dedicated host
register (the dynamic pool spills 0.02% of the time, so it can afford one fewer) and
a guard page on the push side take the pair from 27 to ~17 instructions (estimated).

**The remaining F/N rows.** Invisible on this set: FP+NEON are 0.4-0.6% of static
guest instructions in `cc1` and `node`, and V8/JSC-generated code (where JS numbers
are doubles) is anonymous and outside the census. HANDOVER item 5 stands: an FP-heavy
benchmark, plus a census over anonymous units (section 8), before F2/F3/F6/N5. F5
(FCMP consumers read CR0 directly) shares its machinery with the P5 work and should
ride with it. N1 (vector scan fusion) is glibc `strlen`-family code, which the
engines call constantly; it stays a candidate but has no number here.

**Footprint items.**
- *Hot/cold splitting within a unit.* The F1 stub mechanism is the precedent
  (`EmitFPColdStubs`, `JIT.cpp:5949`) but the F1 stubs are 0.3% of bytes. The cold
  bytes that matter are the ones above: thunks+records (35%), spill stubs (32%), the
  unlinked exit legs (~8% of body bytes), the tail tables (7%). All are cold by
  construction (executed on a miss, on link, on a signal, or never) and all sit
  between hot bodies.
- *Smaller exit/link sequences.* Once the unlinked leg lives in the thunk, the
  in-body exit is `b LinkPath` (one word) and the thunk grows by 6; net hot-stream
  saving ~28 bytes per exit, ~9.6 MB on `cc1`.
- *Shared out-of-line helpers.* `SpillStaticRegs` is byte-identical in every copy
  (`PPC64Emitter.cpp:38-110`): 33 MB of the same 260 bytes. It must stay inside a
  code buffer only because the signal delegator uses `IsAddressInCodeBuffer` as its
  "SRA may be live" proxy (`JIT.cpp:6038-6044`); one copy per code buffer, reached
  from the thunk's LinkPath by `b` within 32 MB (an island per 16 MB of buffer, or
  `ld ; mtctr ; bctr` off a frame slot on this cold path) satisfies that.
- *Code-buffer placement and alignment.* 4.6% of bytes are `nop`; the 8-alignment of
  each thunk and the 16-byte `Align16B` per unit are the sources. Moving thunks out
  removes the first; the second is worth keeping for the fetch. iTLB misses are 2.2x
  native on `cc1` and 5x its rate on code-server, so the buffer should be THP-backed
  where the kernel allows (`Utils/THP.h` exists; whether the code buffer uses it is
  a one-line check for the implementer).
- *Block formation duplicating code.* 1.41 translations per guest instruction on
  `cc1`, 1.29 on `node` (measured above). It costs translation and buffer, not warm
  cycles; the trade against unit size is the item below.

## 7. Ranked goals

Impact is movement of the "x the Pi" ratio on each workload, warm, **estimated from
the shares in sections 3-5 unless marked measured**. The estimates assume cycles
scale with executed host instructions at today's IPC for instruction-count items and
put a range on IPC items. Compounding: G1+G2+G3+G4 on `cc1` is ~20-30%.

| # | Goal | `cc1 lvm.c` (2.65x) | slice (3.4x) | Claude `-p` (~9.7x warm-equiv.) | code-server (4.6x; loop 66% translated) | crc32 / vm (2.04x / 4.1x) | Risk | Effort |
|---|---|---|---|---|---|---|---|---|
| **G1** | **Cold bytes out of the hot stream**: (a) per-code-buffer `SpillStaticRegs` routine instead of per-unit copies; (b) link thunks and records allocated from a cold region of the buffer (top-down, or a paired cold buffer), body keeps one `b`; (c) unlinked exit leg (RIP window + `std pc`) moved into the thunk; (d) `JITCodeTail`+RIP tables out of line. Hot stream from 102 MB to ~15 MB on `cc1` (bodies minus unlinked legs), 5-7x denser. | icache misses 350 M -> ~100-150 M est.; 3-8% cycles -> **~2.5x** | -3-8% -> ~3.2x | same shape (node census); -3-8% | fetch-worst workload (iTLB 5x `cc1`'s rate): -5-10% of the 66% | ~0 (single hot unit) | low-medium: the linker computes record offsets from the thunk (`PPC64LinkRecordFromThunkStart`), the cache stores `CallerDelta`/`ThunkDelta` relative to the record (`JIT.cpp:6000-6015`), `check-code-cache.sh` and the delinkers must follow; the spill stub's `IsAddressInCodeBuffer` contract must hold | medium (days) |
| **G2** | **Flags via CR, not XER**: (a) `CompareBranchFusion` accepts `SubNZCV`/`AddNZCV` producers (`CompareBranchFusion.cpp:163`); (b) CR-direct `NZCVSelect` for `CSEL`/`CSET` after a fusable compare; (c) `cmpw`/`cmplw` two-field form for compares whose consumers are all compares/selects; (d) F5 for FCMP consumers. Saves ~3 of 4 per W `CMP` (6.4% of guest instructions), 1 of 2 per `SUBS reg`, the 0.57 CR/XER ops per `B.cond`, and the `mcrxrx` latency on every loop back edge: est. 0.25-0.3 host instructions per guest instruction, ~10% of executed host instructions | **-6-10% -> ~2.4x** | -6-10% -> ~3.1x | -5-8% (C++ has the same compare density) | -4-6% | vm: its dispatch compares; -3-5% | low for (a): the pass's soundness argument is per CondClass and already written; (b)/(c) medium | (a) a day; (b)-(d) a week |
| **G3** | **Cheaper paired calls and returns**: `callret_sp` pinned in a host GPR (frees 4 memory ops per pair), guard-page overflow on the push like the pop (frees `ld base ; cmpd ; bc`), trampoline address as a relocated constant instead of `bcl ; mflr ; addi` where the cache allows. Pair 27 -> ~17 executed instructions (est.), 3.6% of `cc1`'s guest instructions are `BL`/`RET` | -4-7% -> ~2.5x | -4-7% -> ~3.2x | -4-7% (JSC/Bun C++ is call-dense: `BL` 21% of `node`'s body bytes) | -3-5% | sort: BL/RET through a comparator; -5-10% | medium: the shadow stack's correctness surface (`callret.c`, guard-page resets, signal frames) | medium |
| **G4** | **Copy and zero-extension debris**: (a) `MOV` alias folded in the frontend (`LogicalShifted`: Rn==31, no shift, no invert -> `StoreReg(Rd, Is64, Operand)`), (b) `mr` sources found with the per-op instrumentation (SRA coalescing on `StoreRegister`, `Mask32Tail`, two-address `Sub`), (c) `clrldi ,32` elision extended to consumers that only read 32 bits. `mr` 7.1% + `clrldi` 3.6% + alias `li` ~2% of executed host instructions; est. half removable | -3-5% -> ~2.55x | -3-5% | -3-5% | -2-4% | sha256/crc32: W-form heavy; -3-6% | low; (a) is a one-line frontend change with the existing goldens as gate | (a) hours; (b)/(c) days, instrumentation first |
| **G5** | **Unit granularity**: re-sweep `RegionWindow`/`MaxLeaders` (`Decoder.cpp:155-156`) with the cache warm and G1 in place, and split the duplication (1.41x) from the size. Each unit boundary costs a taken branch plus the 2-instruction entry poke per 8-10 guest instructions (~0.3 host instructions per guest instruction, 5.5x native redirect fetches) | -3-6% warm, cold cost up; measure -> ~2.5x | -3-6% | -3-6% | -2-4% | vm: -5-10% (dispatch loop in one unit) | low (an env knob and a sweep); the decoder's 2026-09 table already shows 256/16 winning on `cc1` and losing on the cold Lua build | small |
| G6 | P1(b) in its cheap form (frontend carries the GPR value cache across intra-unit edges with edge stores) | -2-3% | -2-3% | -2-3% | -1-2% | **crc32 2.04x -> ~1.3x, vm -10-20% (est.)** | medium (exactness at faults is preserved by keeping every store) | medium |
| G7 | THP for the code buffer; `nop` pad trimmed with G1 | iTLB 2.2x -> ~1x; -1-2% | | code-server: iTLB 243/M -> lower; -2-4% | | | low | small |
| G8 | F/N series | none visible (0.4% of instructions) | none | unknown: JSC-generated code is anonymous | unknown: V8-generated code | none | | **blocked on an FP benchmark and the anonymous-code census (section 8)** |

Not levers on this set (measured or read): spill/fill (0.02%), constants beyond what
G2/G4 remove, atomics' CR save-restore (28 sites in `node`), the dispatcher and
`FindBlock` (<1%), syscalls (~1%), the F1 stubs and bodies (0.3% of bytes), the
BR compare cache's size (`BR` is 0.1% of executed instructions on `cc1`; vm is P2's),
and warm cache install (3.5% of warm `cc1`, 2.5 us per block on code-server; the
SuperTuxKart data point in 3.3 says the same from the other side: it is Q2's item,
not a codegen one).

Weighing across the set: G1 and G2 help every workload by about the same share
because all three targets are compiled C/C++ with the same compare, call and exit
density (the `cc1` and `node` censuses agree within a few percent on every row);
neither helps A64Bench's single-unit loops, which is where G6 lives. G3 is worth more
to the engines than to the compiler. Nothing in the top five helps only one
runtime. The one workload the warm items cannot reach is V8's own generated code
(32% of the code-server loop), which is the cold agent's G5.

## 8. What a standard agent should instrument before pricing G2-G4 exactly

Design-only rules kept this research on static reading plus counters. The following
instrumentation (throwaway build, never committed) turns the estimates into
measurements; each line says what result would change the ranking.

1. **Extend the op-size profiler** (`ENABLE_JIT_OPSIZE_PROFILE`, `JIT.cpp:2985-3300`;
   runtime `POWERARM_JITOPSIZEPROFILE=1`). Today it charges bytes per IR op only.
   Add buckets for the link thunks+records (loop at `:5955`), the shared spill stubs
   (`:6045`, `:6054`), the FP cold stubs (`:5949` and the in-loop flush at
   `:5817-5820`), the short-cond islands (`EmitShortCondIsland`, `:2891`) and the
   nop pads, so bucket totals equal `Tail->Size`. Expected: the section 4 census
   reproduced from inside the JIT; if bodies come out above 40% of bytes the census
   script is wrong and G1's estimate shrinks.
2. **Per-guest-class attribution** in the same profiler: at the end of `CompileCode`
   walk `DebugData->GuestOpcodes` (`:6208-6216`), read the guest word at
   `Entry + GuestEntryOffset`, classify it, and charge the host bytes between
   consecutive entries. This is the static half of section 5 without the ELF join.
3. **Execution counts per unit**, not perf samples: a counter increment in the entry
   prologue (next to the suspend poke, `:5685`) into a MAP_SHARED array indexed by
   unit, dumped at exit, then joined with 1-2. This gives executed host instructions
   per guest class exactly. What would change the ranking: if `CMP`+`B.cond` pairs
   are under 5% of executed guest instructions G2 drops below G3; if `BL`+`RET` are
   under 2% G3 drops below G4; if `mr` is under 4% of executed host instructions G4
   drops out of the top five.
4. **The `mr` census**: in `DEF_OP(Copy)`, `Mask32Tail`, `StoreRegister` and the
   backend's `mr` emit sites, count by emitting site under the profiler flag. Tells
   which of G4(b)'s three suspects is real.
5. **Anonymous-code census** for the engines: run 1-3 over units whose guest range
   has no file (`LookupExecutableFileSection` returns nothing) on a Claude `-p` run
   and a code-server loop. This is the only way to see V8/JSC-generated code, and it
   decides whether the F series matters for the two apps at all.
6. **Fetch-stall cost**: `perf stat` with the POWER9 raw codes for
   `PM_ICT_NOSLOT_CYC`, `PM_ICT_NOSLOT_IC_MISS`, `PM_ICT_NOSLOT_IC_L3`,
   `PM_ICT_NOSLOT_BR_MPRED` (this box's `perf` does not list them by name) on warm
   `cc1` and native. Turns G1's 3-8% into a number: if ICT no-slot cycles from icache
   misses are under 3% of cycles, G1 is a footprint and iTLB item only and moves below
   G3.
7. **A/B knobs for the sweeps**: an env override for `RegionWindow`/`MaxLeaders`
   (G5) and for the G4(a) fold, gated on `instructions:u` of warm `cc1` (21.90 G
   today) rather than wall time, per X8's lesson.

## 9. Gates for the goals

- G1: the extended profiler's bucket table (bodies over 70% of the hot span);
  `pm_l1_icache_miss` and `pm_ic_demand_l2_br_redirect` on warm `cc1` (350 M / 367 M
  today); warm slice; three-mode A64Frontend suite; a64diff 64k and 4k-kvm;
  `check-code-cache.sh` (the record layout changes); `callret.c`; the litmus tests
  (the spill stub sits on the signal path).
- G2: `instructions:u` on warm `cc1` (21.90 G); the profiler's `ADDS/SUBS imm` and
  `B.cond` rows; `unittests/A64Frontend` flag goldens in all three modes, plus a new
  golden that covers every CondClass after `CMP`/`CMN` at W and X size against
  both immediate and register operands, signed and unsigned edges (the fusion
  rewrite is only sound if the two lowering arms agree).
- G3: `callret.c` in all three modes; `pm_br_pred_lstack`/`mpred_lstack` on sort
  unchanged; warm `cc1` and `node` instructions.
- G4: `instructions:u` per pattern; the existing goldens.
- G5: warm `cc1` and slice per setting, with cold slice reported alongside (the
  cold agent's numbers are the reference).
- G6: `crc32` and `vm` warm; `pm_st_fwd`; the fault-exactness argument re-read
  against `SignalDelegator` before landing.

## 10. Measurements taken (all new, one run each unless noted)

POWER9, contended (another agent building on 88-160); CPU 40 for `cc1` and native
gcc, CPUs 44-47 for Claude and code-server. Raw files in the session scratchpad
(`scratchpad/warm/`: `gcc-series*.log`, `*.stat`, `*.perf`, `census-*.txt`,
`census.py`).

| Run | Result |
|---|---|
| `gcc -O2 -c lvm.c` cold, fresh cache | wall 6.97 s; `cc1` guest 6.57 s, save 24 ms; cache 185 MB (4 segments, 109,629 units) |
| same, warm (3 runs) | wall 5.04 / 5.06 / 5.15 s; `cc1` guest 4.91-4.96 s, user 4.81-4.87 s, sys 37-147 ms |
| warm `perf stat` | table in 3.2 |
| native `/mnt/arch/usr/bin/gcc` same flags | wall 1.70 s; table in 3.2 |
| warm `perf record` + `POWERARM_BLOCKJITNAMING=1` | 91.3% JIT / 6.6% POWERarm / 0.9% libc; 9,906 samples, 8,745 in named `cc1` units |
| census, `cc1` | section 4-5; bodies 12.6 B per guest instruction; duplication 1.41 |
| census, `node` (cold agent's `cache-cs-all` segments) | stubs 34.2% / thunks 32.9% / bodies 19.4%; 12.2 B; duplication 1.29 |
| `claude --help` (3 runs) | guest 1.36-1.40 s; 5.02 G cycles, 5.69 G instr, icache 40.0 M, iTLB 644 k, branches 841 M, misses 41.3 M; profile 70.7% POWERarm / 26.9% JIT |
| `claude -p` one-word prompt | wall 22.8 s, guest 22.5 s, user 21.5 s; profile 79.8% JIT / 19.1% POWERarm (translation ~12%) |
| code-server, scope=all, first launch under a new ConfigId | first `/healthz` 3.71 s; `node` user 3.79 s at startup; loop: 12.67 G cycles / 7.87 G instr per 15 s; profile 65% JIT / 33% POWERarm |
| code-server, second launch (loaded 97,242) | first `/healthz` 2.32 s; `node` user 1.55 s at startup; loop 59 rounds per 15 s; 12.67 G cycles / 8.36 G instr; icache 83.2 M; iTLB 2.03 M; branches 1.30 G; misses 55.3 M; dcache 106 M; dTLB 1.35 M; idle 10 s: 29 M cycles; profile 66.3% JIT / 32.3% POWERarm |
| Pi 5 `gcc -O2 -c lvm.c` (3 runs) | 1.95 / 1.89 / 1.91 s wall, 1.87-1.91 s user |
| Pi 5 `claude -p` (1 run) | 3.85 s wall, 1.77 s user, 0.11 s sys |

Not measured, deliberately: A64Bench (no binaries built; recorded numbers used), any
instrumented build (section 8), a second Pi run of anything.
