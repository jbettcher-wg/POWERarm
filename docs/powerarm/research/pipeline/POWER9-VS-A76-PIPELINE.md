# POWER9 vs Cortex-A76: the execution pipeline, and how POWERarm should shape code for it

Research note for the POWERarm JIT (A64 guest -> FEX IR -> PPC64LE host, POWER8 floor,
POWER9-gated paths). It answers five questions: what the two pipelines look like side by
side; which properties of generated code let POWER9's width work and which defeat it; why
the M1 baseline stalls where it stalls; what the JIT should do about it, ranked; and where
POWER9 can beat the A76 despite emulation.

Tags: **[MEASURED]** this machine, the probes in `probes/`; **[DOC]** a document with
section/page; **[SPEC]** an architectural statement without a measurement here;
**[INFERENCE]** my reading of measured or documented facts. Cycle counts are `cycles:u`
per loop iteration from `perf stat`, so they do not depend on the clock (the core boosts
to 3.8 GHz under load); nanoseconds are wall time from `clock_gettime`.

Machine: AC922 8335-GTH, POWER9 DD2.3 (pvr 004e 1203), 2 x 22 cores SMT4, kernel 7.2.6-64k
(Radix, 64 KiB pages), `perf` 7.2 with the POWER9 event JSON from
`linux-7.2.6/tools/perf/pmu-events/arch/powerpc/power9/`. Probes pinned with `taskset` to
CPUs 120, 124, 128 and 136 (NUMA node 0, one hardware thread per core, siblings idle unless
the SMT experiment says otherwise). Reference ARM core: Raspberry Pi 5, Cortex-A76 at
2.4 GHz, kernel 6.18.39+rpt-rpi-2712, core 3, 4 KiB pages.

Primary sources: *POWER9 Processor User's Manual*, OpenPOWER v2.0, 9 April 2018 ("UM",
page numbers as printed); *Power ISA v3.0C*; *Arm Cortex-A76 Software Optimization Guide*
v10.0, PJDOC-466751330-7215 ("SOG"); B. Thompto, *POWER9: Processor for the Cognitive
Era*, Hot Chips 28 (2016), slides 8-9 ("HC28"). The M1 baseline is
`docs/powerarm/BENCH-M1-BASELINE.md`.

## 1. Executive summary

- **The width is real and it is idle.** POWER9 fetches 8, decodes/dispatches 6 and issues
  9 iops per cycle across four execution slices [DOC UM §25.1.1 p.321]. Under POWERarm the
  crc32 loop runs at IPC 0.34 with the dispatcher held on a full issue queue
  essentially every cycle (`pm_disp_held_issq_full` 16.5 G of 16.5 G cycles, scaled from a 75 % sample) [MEASURED,
  `probes/emu_stat.csv`]. The window is full of loads and stores waiting on each other,
  so no amount of width helps.
- **The M1 crc32 hypothesis is confirmed, with a correction.** The hot block
  (`probes/hot.crc32.crc32_0x624.objdump`) keeps every loop register (w9, x10, x11, w12) in
  `CpuState` and does `ld` / ALU / `std` per guest instruction, including 6 `std`-then-`ld`
  of the *same slot* per byte. Each hop through memory costs load-to-use 4 + forward 2
  cycles [DOC UM Table 25-6 p.348; MEASURED 22.6 vs 7.0 cycles per iteration,
  `S_ctx_rt1` vs `S_reg_rt1`]. The 15.6 % `pm_cmplu_stall_st_fwd` is the visible part;
  the rest of the same chain shows up as `pm_cmplu_stall_store_finish` (33 % of cycles)
  and `pm_cmplu_stall_load_finish` (13 %). The same arithmetic with the four slots loaded
  once per block and stored once runs **3.8x faster** (`S_ctx_rt4_x8` 139 cycles vs
  `S_blocklocal` 37) [MEASURED].
- **vm's "execution-unit" stalls are SPR moves, not arithmetic.** `pm_cmplu_stall_fxu`
  counts a next-to-finish fixed-point *or CR/SPR* iop; the probes put 2.8-3.2 stall cycles
  on every `mtctr`/`bctr` pair and 7-14 on `mfocrf`/`mtocrf`, `mflr`/`mtlr` round trips
  [MEASURED]. vm's dispatch block ends in `ld; rldic; add; ld; cmpd; bne; ldx; mtctr; bctr`
  (`probes/hot.vm.vm_0x688.objdump`): three dependent loads and a 5-cycle `mtctr`
  [DOC UM p.342] in front of every guest `br`, about 23 cycles per dispatch before any
  misprediction.
- **The count cache does not learn sequences at one site.** A shared `bctr` fed a
  period-8 opcode program mispredicts 75 % of the time, the same as uniform random; a
  compare chain on the same program is predicted perfectly (0.03 mispredicts) and runs
  2.1x faster (16.5 vs 35.3 cycles) [MEASURED `D_bctr_rep8`, `D_cmp_rep8`]. The A76's
  indirect predictor learns that program (6.0 cycles, no mispredicts) [MEASURED Pi 5].
  This is the vm gap: guest `br` patterns the A76 predicts are structurally
  mispredicted by a host `bctr`. A mispredict costs **~25 cycles** here, not 14
  [MEASURED, three independent estimates].
- **The Spectre-v2 mitigation is not the cause.** "Software count cache flush (hardware
  accelerated), Software link stack flush" means the kernel flushes both predictors in
  `_switch` (context switch only), not on syscalls or interrupts that return to the same
  task [DOC `arch/powerpc/kernel/switch.S:20-64`, `security.c:438-495`]. The count cache
  is enabled; the probes mispredict identically in a process that never context-switches.
- **XER is the expensive flag home, CR fields are the cheap one.** A dependent chain
  through XER.CA costs 6.3 cycles per op against 2.3 for the same ops without CA; reading
  CA back with `mfxer` costs 11 cycles per op; `mfocrf`/`mtocrf` 7 per round trip;
  a carry produced by `cmpld` into a CR field and consumed by `isel` is 7 cycles for the
  three-op chain [MEASURED]. The UM's "+3 cycles for a CR/XER/FPSCR source" [DOC p.341]
  is the documented floor.
- **Every loop back-edge costs 5-7 cycles of fetch redirect** on POWER9 regardless of
  direction predictability (`Q_loop_*`: 7.0 cycles for a one-add loop, `bdnz` or `bne`
  or `b`); forward taken branches to the next block cost 1-2 cycles; the A76 runs the
  same loop at 1.0 cycle [MEASURED]. Small guest loops therefore have a ~7-cycle floor
  per iteration on POWER9 that no code quality removes.
- **SMT siblings must stay idle.** One busy sibling makes the emulated vm 1.9x slower and
  native vm 2.9x slower; crc32 (memory-chain bound) is unaffected. Idle siblings behave
  exactly like a far idle core, so pinning with idle siblings is equivalent to SMT-off
  for this purpose [MEASURED `probes/smt_cpu128.csv`].
- **Where POWER9 wins:** cache- and memory-bound guests (bst is 1.22x the Pi under
  emulation because native is 3.2x faster: 10 MiB L3 per core pair at 35.5 cycles,
  512 KiB L2 at 15.5, 68 ns local memory [DOC UM Table 25-7 p.356]), and throughput per
  socket (88 hardware threads). It does not win tight integer loops or interpreter
  dispatch, where the A76's 1-cycle ALU, 1-cycle loop and pattern-learning indirect
  predictor beat POWER9's 2-cycle ALU, 7-cycle loop floor and last-target count cache
  even before emulation overhead.

## 2. Side-by-side pipeline model

Numbers marked "not public" could not be sourced from Arm or IBM documents reachable from
this host; the A76 secondary figures come from the widely reported Arm Hot Chips 2018
disclosure as summarised by WikiChip
(https://en.wikichip.org/wiki/arm_holdings/microarchitectures/cortex-a76) and AnandTech
(https://www.anandtech.com/show/12785/arm-cortex-a76-cpu-unveiled-7nm-powerhouse); both
pages refused connections from this network during the study, so those rows are tagged
[DOC-secondary] and should be re-verified against the pages themselves.

| Property | POWER9 (one SMT4 core) | Cortex-A76 |
|---|---|---|
| Pipeline depth | 11 stages I-cache to writeback for FX reg-reg, 13 for load/store (L1 hit), 17 for FP [DOC UM §2.1.3 p.36]; fetch-to-compute 5 cycles shorter than POWER8 [DOC HC28 slide 8] | 13 stages, 11-cycle minimum mispredict penalty [DOC-secondary] |
| Mispredict penalty, measured | **~25 cycles** (unpaired `blr` +25.5; `bctr` with 2 random targets +24; dispatch +25) [MEASURED] | **~15-18 cycles** (`D_br_rand4` vs `D_br_rep8`, `B_r2_bl_ret` vs `B_bl_ret`) [MEASURED Pi 5] |
| Fetch | up to 8 instructions (32 B) per cycle from a 32 KiB 8-way L1I; taken-branch target fetch 5-8 instructions; no fetch across a 128 B line in one cycle [DOC UM §25.1.3 p.324-325] | fetch width not stated in the SOG; 64 KiB L1I [DOC-secondary] |
| Decode / dispatch | 6 iops per cycle, split 3+3 per thread pair in SMT4; at most 2 branch iops per cycle; 2-way and 3-way cracking, microcode for >3 iops with a 2-cycle decode penalty [DOC UM §2.1.3 p.36, §25.1.4 p.330-332] | 4 Mops decoded and up to 8 uops dispatched per cycle; per-cycle caps 2 B, 4 S, 2 M, 2 per V, 2 per L [DOC SOG §4.1 p.41] |
| Issue | 9 ports: 4 AGEN + 4 EXEC + 1 BR; four 13-entry slice issue queues + one 16-entry branch queue = 68 issuable iops [DOC UM §25.1.5 p.337] | 8 execution pipes: B, I0, I1, M, L0, L1, V0, V1, one uop each per cycle [DOC SOG §2.1 p.7]; ~120 issue-queue entries across 8 queues [DOC-secondary] |
| In-flight window | ICT 256 iops (128 per thread SMT2, 64 SMT4) [DOC UM §25.1.4.4 p.332]; history buffer 464 GPR/FPR/VR entries, 96 CR/XER/FPSCR [DOC UM Table 25-3 p.336]; EAT 40 entries, so at most 40 predicted-taken branches in flight [DOC UM p.331] | 128-entry instruction window [DOC-secondary]; ROB/PRF sizes not public |
| Instruction grouping | none: POWER9 "removed instruction grouping and reduced cracking" [DOC HC28 slide 8]; the surviving group-like rules are per-superslice dispatch tuples and slice affinities (even-slice ops such as divide and SPR moves consume both slots of a superslice) [DOC UM §25.1.4.5 p.332-335] | n/a |
| Integer ALU | 4 ALUs, latency **2** cycles (ALU) or 3 (ALU2) [DOC UM Table 25-4 p.342]; measured 2.3 cycles per dependent `add`, ~3.3 independent adds/cycle sustained [MEASURED `A_chain1`, `A_indep16`, `Q_loop_long`] | 3 integer pipes (I0, I1, M), latency **1** [DOC SOG §3.4 p.9]; measured 1.0 per dependent `add`, 8 independent adds in 3 cycles [MEASURED Pi 5] |
| Multiply / divide | MUL 5 cycles (DP-MUL pipe, one per slice), DIV 12+, one DIV per superslice [DOC UM Table 25-4 p.342]; measured 8.7 cycles per dependent `mulld` [MEASURED `A_mulchain`] | MUL 2-3, one M pipe [DOC SOG §3.6 p.10-11] |
| Load-to-use | 4 cycles L1 hit; 5 for a 128-bit consumer; 6 if the access does not fit an aligned doubleword or a 16-byte access is not DW-aligned; +3 crossing a 32-byte granule; +2 when forwarded from a store [DOC UM §25.1.7.8, Table 25-6 p.347-348]; measured ~4 [MEASURED `A_ldchain`] | 4 cycles for GPR loads [DOC SOG §3.9 p.14] |
| L1D / LSU | 32 KiB 8-way, 128 B lines, 4 doubleword slices each returning 8 B/cycle; 4 AGEN per cycle; LRQ 76 entries (2 x (10 + 28)), SRQ 64 (4 x 16), LMQ 12 [DOC UM §25.1.7 p.343-344]; store-through L1, L2 store-in [DOC p.344, 355] | 2 load/store pipes; 2 x 128-bit loads and 1 x 128-bit store per cycle; 64 KiB L1D [DOC SOG §4.4 p.41; secondary]; queue sizes not public |
| Store-to-load forwarding | forwards from **one** store per LS slice plus L1 bytes; a load needing bytes from two stores in one slice waits for the store to drain to cache; EA(44:63) match then full EA/RA confirm, mismatch flushes [DOC UM §25.1.7.6 p.346-347]; measured: one store +2 cycles, two 4-byte stores feeding one 8-byte load **+21 cycles and no forward at all** [MEASURED `S_fwd_2stw_ld`] | load start must align with the start or middle of the store; loads >= 8 B may take from 2 stores (one per half), loads <= 4 B from one [DOC SOG §4.6 p.42]; measured two-store case +0.8 cycles [MEASURED Pi 5] |
| Memory disambiguation | loads are held behind older stores with a matching base register + displacement (or base + index), and behind any store once the load's address has suffered a store-hit-load flush [DOC UM §25.1.7.7 p.347]; measured no flushes and no difference for an aliased base register in the JIT's shape [MEASURED `S_alias_rt1`] | not described in the SOG |
| Direction prediction | 4 x 8K-entry BHTs (local, global, selector, local-selector) plus a TAGE with 4 history lengths and 10-bit tags; static hint bits ignored by default; predicted-taken latency 3 cycles (5 via TAGE), BTAC 1 cycle in ST mode only [DOC UM §25.1.3.4 p.327-330] | sizes not public |
| Indirect prediction | count cache: global 512 entries (low 32 bits of target, GHV-hashed index) + local 256 entries (full target, address-indexed) + 2-bit selector; pattern cache 256 entries; **one count-cache access per cycle, at most one `bcctr`/`bclr` per aligned 32-byte block** [DOC UM p.328-330]; BH hints on `bcctr` ignored [DOC Table 25-2 p.329]; measured: last-target behaviour, period-8 sequence mispredicts 75 % [MEASURED `D_bctr_rep8`] | not public; measured: learns a period-8 sequence with no mispredicts [MEASURED Pi 5 `D_br_rep8`] |
| Return prediction | link stack 64 entries, 32 per thread in SMT2, 16 in SMT4; pushed by any LK=1 branch (except the `bcl 20,31,$+4` special case), popped by `bclr` with BH=00 [DOC UM p.328-329]; measured no misses at depth 48, one per unwind at depth 96 [MEASURED `B_depth*`] | return stack, depth not public |
| Fusion | a CRACK/FUSE decode stage with "enhanced instruction fusion" [DOC HC28 slide 8]; fusible pairs listed per instruction in UM Table A-1 (not enumerated here) | not described in the SOG |
| Special registers | `mfocrf`/`mtocrf` round trip +7 cycles, `mfcr`/`mtcrf` +11, `mfxer` +11 per op, XER.CA as a source +4 per op [MEASURED]; `mt LR/CTR` 5 cycles, `mf LR/CTR` 6, other SPRs 12-14 [DOC UM p.342]; any change of XER.SO flushes the pipeline [DOC UM §25.1.6.1 p.342] | NZCV and SP fully renamed; FPSR/FPSCR reads wait for all older flag writers to retire; most other SPR writes non-speculative/in-order [DOC SOG §4.11 p.44-45] |
| SMT | SMT4 splits decode into two 3-wide halves, statically partitions ICT, IBUF, EAT and link stack, disables the BTAC and the instruction prefetcher [DOC UM §25.1.2 p.322-323, p.326, 328, 330-332]; measured: idle siblings == ST; one busy sibling costs 1.1x-6x on the probes and 1.9x on emulated vm [MEASURED] | none |
| L2 / L3 / memory | L2 512 KiB 8-way per core pair, 15.5-cycle load hit, 64 B/cycle; L3 10 MiB 20-way victim cache per pair, 35.5 cycles, 32 B/cycle; local memory 68 ns; 120 MiB L3 per chip [DOC UM §25.2, Table 25-7 p.355-356; HC28 slide 9] | Pi 5: 512 KiB L2 per core, 2 MiB shared L3 (BCM2712) [DOC BENCH-M1-BASELINE.md]; latencies not measured here |
| TLB / pages | I-ERAT 64 entries, two D-ERATs of 64 per LS pair; unified TLB 1024 4-way (in Radix: 512 TLB + 3 x 128 page-walk cache); 4K/64K/2M/16M/1G native; 64 KiB pages are a first-class ERAT size [DOC UM §25.1.3.1 p.325, §25.1.7.4-5 p.345-346]; this kernel runs 64 KiB pages, so guest 4 KiB granules are emulated (`POWERARM_HOSTPAGEMODE=force`) | 4 KiB pages on the Pi; TLB sizes not stated in the SOG |
| Prefetch | 8 hardware streams, stride-N, L1 and L3 prefetch, adaptive depth 4-24 lines [DOC UM §25.1.7.12 p.351-352]; instruction prefetcher 0-7 lines, off in SMT4 [DOC p.326] | not described in the SOG |
| Branch density rule | one count-cache branch per aligned 32 B; targets and inner loops on quadword boundaries [DOC UM p.324, 329]; measured: two `bctr` in one 32 B block +16 cycles, 8 not-taken `bc` in 32 B no penalty [MEASURED] | at most 4 branches per aligned 32 B; loops of <= 32 B inside one 32 B region; 32 B-aligned targets [DOC SOG §4.9 p.44] |

## 3. PMU event guide for this machine

`perf` here is `/mnt/arch/usr/bin/perf` with `LD_LIBRARY_PATH=~/.local/lib/perfshim`;
`kernel.perf_event_paranoid=2`, so use `:u`. POWER9 has four programmable counters (PMC1-4)
plus fixed instruction (PMC5) and cycle (PMC6) counters; the first hex digit of the event
code is the PMC it must run on (0 = any). Groups whose events collide on a PMC are
multiplexed and scaled; `run_probes.sh` and `emu_stat.sh` pick non-colliding groups.
Descriptions are from the kernel's `power9/*.json` (codes in
`arch/powerpc/perf/power9-events-list.h`); the mechanism column is UM chapter 25.

| Event (code) | Counts | Mechanism (UM) | What it told us |
|---|---|---|---|
| `pm_cmplu_stall` (1E054) | cycles nothing completed with the ICT non-empty | completion is in order [p.332] | 66 % of crc32 cycles, 61 % of vm |
| `pm_cmplu_stall_st_fwd` (4C01C) | completion stall due to store forward | +2 cycle forwarding delay [p.346, Table 25-6] | 15.6 % crc32: the visible part of the context round trips |
| `pm_cmplu_stall_store_finish` (2C014) | NTF is a store with dependencies met, waiting for the LSU pipe | stores dual-issue AGEN then data [p.337] | 33 % of crc32: the *other* half of the round-trip chain |
| `pm_cmplu_stall_load_finish` (4D014) | NTF is a load with dependencies met, in the LSU pipe | 4-cycle load-to-use [p.347] | 13 % crc32, 25 % vm |
| `pm_cmplu_stall_lhs` (2C01A) | NTF load hit an older store and waits for data | forwarding impossible from two stores per slice [p.346] | 19 cycles per iteration in `S_fwd_2stw_ld`; near zero in crc32 |
| `pm_st_fwd` (20018) | store forwards that finished | | 30 per crc32 iteration = every `ld` after a `std` of the same slot |
| `pm_cmplu_stall_exec_unit` (2D018) / `pm_cmplu_stall_fxu` (2D016) | NTF in FXU/VSU/CRU; FXU = scalar fixed-point **or CR** iop in ALU/ALU2/DIV | includes SPR/CR moves: `mtctr` 5, `mflr` 6 [p.342] | vm's 9.7 %: `mtctr`->`bctr` in every dispatch |
| `pm_cmplu_stall_fxlong` (4D016) | long-latency scalar FX (divide) | DIV 12+ [p.342] | negligible in both workloads |
| `pm_cmplu_stall_bru` (4D018) | NTF is in the branch unit | | 107 M in vm: branches waiting on `mtctr` |
| `pm_flush_mpred` (50A4) / `pm_br_mpred_cmpl` (400F6) | mispredict flushes / mispredicted branches completed | flush after execution [p.327] | vm 173 M flushes |
| `pm_br_pred_ccache` / `pm_br_mpred_ccache` (40A4 / 40AC) | XL-form branches predicted by the count cache / wrong | count cache [p.328-329] | vm 949 M / 136.5 M (14.4 %) |
| `pm_br_pred_lstack` / `pm_br_mpred_lstack` (40A8 / 48AC) | `bclr` predicted by the link stack / wrong | link stack [p.328] | 0 misses to depth 48, 1 per unwind at 96 |
| `pm_br_pred_pcache` / `pm_br_mpred_pcache` (48A0 / 48B0) | pattern-cache predictions / wrong | pattern cache [p.330] | 0-160 in the baseline: the pattern cache almost never fires here |
| `pm_disp_held` (10006), `pm_disp_held_issq_full` (20006), `pm_disp_held_hb_full` (3D05C) | dispatch held; held because an issue queue (or the branch queue) is full; because the history buffer is full | 13-entry slice queues, HB sizes [p.336-337] | crc32: issue-queue-full **every** cycle, HB-full 21 %; native crc32 the same 100 % issq-full (the loop is latency-bound natively too) |
| `pm_ict_noslot_cyc` (100F8), `pm_ict_noslot_br_mpred` (4D01E) | front end delivered nothing / because of a mispredict | | vm 0.9 G / 0.56 G: the mispredict shadow |
| `pm_cmplu_stall_flush_any_thread` (1E056) | completion blocked by a flush on any thread of the core | | vm 0.56 G, i.e. the flush recovery itself |
| `pm_cmplu_stall_other_cmpl` (30006) | instructions the *core* completed while this thread was stalled | | not a stall; ignore in single-thread analysis |
| `pm_lsu_flush_lhl_shl` (C8B4), `pm_flush_lsu` (58A4) | store-hit-load / any LSU flush | SHL flush [p.347] | 1.6 M in crc32, 26 M in vm: real but small |
| `pm_cmplu_stall_any_sync` (1E05A) | NTC `isync`/`lwsync`/`hwsync` not allowed to complete | [p.354-355] | 0.4 M: no barrier cost in these guests |
| `pm_ld_miss_l1` (3E054), `pm_cmplu_stall_dcache_miss` (2C012) | L1 misses at execution; NTF load waiting for the nest | | tiny for crc32/vm; the events to watch for bst |

## 4. Probe results

Sources: `probes/pipeprobe.c` (POWER9, 83 probes, each parity-checked against a C
reference at two sizes before timing), `probes/a76probe.c` (Pi 5, 15 probes of the same
shapes), raw counts in `probes/probes_cpu120.csv`, `probes/probes_q_cpu136.csv`,
`probes/a76probe.pi5.log`. All POWER9 probes ran 4 M iterations; Pi 5 probes 2 M. Every
probe passed parity. The loop scaffold (`addi`/`cmpdi`/`bne`) is included in every number;
its own cost is the `B_none`/`Q_loop_bne` row.

### 4.1 Width, latency, loop floor

| Probe | POWER9 cycles/iter | A76 cycles/iter | Reading |
|---|---|---|---|
| one `add` + loop (`Q_loop_bne`, `Q_loop`) | **7.0** | **1.0** | a taken back-edge costs ~5-6 cycles of fetch redirect on POWER9; `bdnz` (6.8) and an unconditional `b` back-edge (7.1) are the same |
| 8 dependent `add` (`A_chain1`) | 20.9 | 8.0 | 2-cycle vs 1-cycle ALU, plus the loop floor |
| 8 independent `add` (`A_indep8`) | 7.6 | 3.0 | POWER9 hides 8 adds inside the loop floor; A76 issues ~2.7/cycle |
| 16 / 64 independent `add` (`A_indep16`, `Q_loop_long`) | 9.8 / 24.6 | - | marginal 3.2-3.5 adds per cycle on POWER9 |
| 8 dependent `mulld` (`A_mulchain`) | 76.9 | - | 8.7 per multiply, above the documented 5 |
| 8 dependent L1 loads (`A_ldchain`) | 36.9 | - | ~4 per load-to-use |
| 4 blocks, forward taken `b` / `bne` / `mtctr;bctr` (`Q_loop_*_x4`) | 12.8 / 12.9 / 16.8 | - | a forward taken branch costs 1-2 cycles, a predicted `bctr` ~2.5; distance (16 B vs 256 B) does not matter (`Q_fwd_near_x4` 13.0, `Q_fwd_far_x4` 12.9) |

### 4.2 Flags

Sixteen dependent ops per iteration in every row; POWER9 cycles per *guest-level op* in
brackets.

| Probe | POWER9 | A76 | Reading |
|---|---|---|---|
| `add`;`addi` chain, no flags (`F_add_addi`) | 36.9 [2.3/op] | - | control |
| `addc`;`addze` chain through XER.CA (`F_addc_addze`) | **100.9 [6.3/op]** | 16.0 [1.0/op] (`adds`/`adc`) | +4 cycles per XER-sourced op (UM says >= +3, p.341); the A76 renames NZCV for free |
| carry as a CR bit: `add`;`cmpld`;`isel` (`F_cmpld_isel`) | 56.9 [7.1/3-op chain] | - | CR source costs ~1 extra cycle over pure data |
| ... plus `mfocrf`/`mtocrf` (`F_mfocrf_rt`) | 112.9 | - | **+7 cycles per CR-field round trip through a GPR** |
| ... plus `mfcr`/`mtcrf 0xff` (`F_mfcr_rt`) | 146.8 | - | +11 per whole-CR round trip |
| `addc`;`mcrxrx`;`isel` (POWER9 only, `F_mcrxrx_p9`) | 80.9 [10.1] | - | `mcrxrx` costs 3 more cycles than `cmpld` for the same bit |
| `addc`;`mfxer`;`rlwinm`;`add` (`F_mfxer_chain`) | 189.4 [23.7] | - | **+11 cycles per `mfxer`**: never read CA into a GPR on a hot path |
| `li`;`mtxer` before every `addc` (`F_mtxer_chain`) | 104.5 | - | clearing XER in the shadow of a chain is nearly free (+0.5/op); the 37 ns handbook number is the *dependent* round trip |
| 32-bit lazy carry via `srdi 32` (`F_srdi32`) | 103.0 [12.9/4-op chain] | - | no flags at all, but 4 dependent ops; use when the carry is consumed as data |
| `add.` unread CR0 (`F_add_dot`) | 20.9 | - | a record form costs nothing if nobody reads CR0 |
| `add.`;`isel` on CR0 (`F_add_dot_isel`) | 53.4 | - | CR0 consumed immediately: 3.3 per op, i.e. +1 over data |

### 4.3 Store forwarding and context round trips

| Probe | POWER9 | A76 | `pm_st_fwd` / `pm_cmplu_stall_lhs` per iter (P9) |
|---|---|---|---|
| loop-carried add in a register (`S_reg_rt1`) | 7.0 | 1.0 | - |
| loop-carried through one 8-byte slot, `ld`;`addi`;`std` (`S_ctx_rt1`) | **22.6** | 5.5 | 1.1 / 0 |
| 4 independent slots each round-tripping (`S_ctx_rt4`) | 21.7 | - | 3.5 / 0: forwarding is pipelined, the cost is latency |
| crc32-shaped loop, 3 context slots + table load (`S_ctx_crcshape`) | **42.9** | 13.0 | 3.3 / 0 |
| same loop, registers only (`S_reg_crcshape`) | 26.9 | 7.0 | - |
| 4 slots loaded once, 32 ops, stored once (`S_blocklocal`) | 36.6 | - | 4.3 / 0 |
| same 32 ops with a round trip per op (`S_ctx_rt4_x8`) | **139.4** | - | 32 / 0.05: **3.8x** |
| 4-byte store -> 8-byte load (`S_fwd_stw_ld`) | 25.0 | - | 1.1 / 0: store + cache merge works, +2.4 cycles |
| 8-byte store -> 4-byte load (`S_fwd_std_lwz`) | 22.5 | - | 1.1 / 0: subset forwards at full speed |
| two 4-byte stores -> one 8-byte load (`S_fwd_2stw_ld`) | **43.5** | 6.3 | **0 / 19**: no forward; the load waits for the stores to drain [DOC p.346] |
| two 1-byte stores -> one 2-byte load (`S_fwd_2stb_lhz`) | 43.5 | - | 0 / 19: same rule at byte size |
| 8-byte slot straddling a doubleword / a 32 B granule (`S_fwd_cross8/32`) | 22.0 / 22.6 | - | 2.2 forwards (two slices) / 0: hidden inside the round-trip latency here |
| `std` then `ld` via the same / an aliased base register (`S_samebase_rt1`, `S_alias_rt1`) | 22.5 / 22.5 | - | 1.0 / 0, no SHL flushes: the base-register predictor is not what limits the JIT's shape |

### 4.4 Branches, calls and returns

The two-site probes select the call site with an unpredictable bit, so every `B_r2_*`
row carries the same 0.5 direction mispredicts per iteration; compare rows with each
other, not with `B_bl_blr`.

| Probe | POWER9 | A76 | P9 `pm_br_mpred_*` per iter |
|---|---|---|---|
| `mtctr`;`bctr` to the next block, predicted (`B_bctr_next`) | 10.8 | 2.0 (`br`) | 0; `pm_cmplu_stall_fxu` 2.8 (the `mtctr`) |
| two predicted `bctr` in one aligned 32 B block (`B_bctr2_sameblock`) | **29.9** | - | **1.0 count-cache mispredict per iteration** |
| the same two `bctr` in separate 32 B blocks (`B_bctr2_sepblock`) | 13.8 | - | 0 |
| 8 not-taken `bc` in one 32 B block / spread (`B_bc8_packed/spread`) | 13.8 / 21.4 | - | 0 / 0: no density penalty for conditional branches |
| 8 taken `b` per iteration (`B_b8_taken`) | 13.8 | - | 0.85 cycles per taken forward branch |
| `bl`/`blr`, one site (`B_bl_blr`) | 13.8 | 3.0 | 0 |
| 2 random sites: `bl` / `blr` (`B_r2_bl_blr`) | 25.7 | 12.1 | lstack 0 |
| 2 random sites: `bctrl` / `blr` (`B_r2_bctrl_blr`) | 25.9 | - | lstack 0, ccache 0: **an indirect call with LK=1 pairs perfectly** |
| 2 random sites: `bl` / LR through memory / `mtlr`;`blr` (`B_r2_bl_mtlr_blr`) | 26.3 | - | lstack 0: reloading LR from a shadow stack costs 0.5 cycles |
| 2 random sites: `bl` / `mflr`;`mtctr`;`bctr` (`B_r2_bl_bctr`) | 37.8 | - | ccache **0.50**: a RET through the count cache mispredicts whenever the caller changes (+24 cycles each) |
| 2 random sites: `b` (no LK) / `mtlr`;`blr` (`B_r2_b_mtlr_blr`) | **51.2** | - | lstack **1.00**: an unpaired `blr` mispredicts every time, +25.5 cycles each |
| 2 random sites: `b` / `mtctr`;`bctr` (`B_r2_b_mtctr_bctr`) | 31.8 | - | ccache 0.18: the GHV sees the site-selecting branch, so the global count cache learns this one |
| recursion depth 8 / 24 / 48 / 96 (`B_depth*`) | 103 / 327 / 600 / 1219 | - | lstack 0 / 0 / 0 / **1.0** per unwind: the link stack is 64 deep in ST mode [DOC p.328]; `mflr`/`stdu`/`ld`/`mtlr` cost ~13 cycles per call/return pair (fxu 5.6 per pair) |

### 4.5 Dispatch (the vm shape): count cache vs compare chain

Sixteen handlers, one `bctr` at the dispatch head (or a compare chain over opcodes 0-3
falling back to `bctr`), programs of 2^20 opcodes.

| Program | shared `bctr` (P9) | compare chain (P9) | replicated `bctr` tails (P9) | `br` (A76) | compare chain (A76) |
|---|---|---|---|---|---|
| 4 opcodes uniform random | 35.2 cycles, 0.75 mispred | 32.7, 0.89 | 32.3, 0.75 | 17.4 | 16.8 |
| 16 opcodes uniform random | 40.5, 0.94 | 40.6, 1.01 (chain + bctr) | - | - | - |
| **period-8 program over 4 opcodes** | **35.3, 0.75** | **16.5, 0.03** | 35.7, 0.88 | **6.0** | 5.3 |
| period-64 program over 16 opcodes | 40.9, 0.95 | - | 37.0, 0.92 | - | - |
| 90 % one opcode | 20.2, 0.19 | 16.3, 0.23 | - | - | - |

Three conclusions [MEASURED]:

1. The POWER9 count cache predicts the *last* target at a site. It does not learn a
   period-8 sequence at one site or across 16 replicated sites; the GHV-indexed global
   array only helps when *conditional* branches in the path distinguish the cases
   (`B_r2_b_mtctr_bctr`, 0.18). The A76 learns the same sequence outright.
2. Direction prediction (BHT/TAGE) learns the sequence perfectly through a compare chain,
   so a small inline cache in front of the count cache is worth 2.1x on a patterned
   dispatch and costs nothing on a random one (32.7 vs 35.2).
3. A mispredict costs ~25 cycles: (35.2 - 16.5) / 0.75 = 25.

### 4.6 Barriers, atomics, constants

| Probe | POWER9 cycles/iter | Reading |
|---|---|---|
| `lwz`;`addi`;`stw` (`L_lwz_stw`) | 22.6 | control (one round trip) |
| `lwarx`;`addi`;`stwcx.` uncontended (`L_larx_stcx`) | **73.9** | 3.3x the plain round trip: a guest LDXR/STXR loop is 50 cycles dearer than a plain RMW |
| `lwsync`; larx/stcx; `isync` (`L_larx_stcx_acqrel`) | 96.8 | +23 for the acquire/release pair |
| plain RMW + `lwsync` / + `sync` (`L_lwz_stw_lwsync/hwsync`) | 36.8 / 65.0 | `lwsync` +14, `sync` +42: use the lightest barrier the guest ordering allows |
| 5-instruction 64-bit constant x4, off the critical path (`T_mat5_x4`) | 12.8 | vs pool `ld` x4 9.7, `addpcis`+`ld` 11.1: materialisation costs dispatch slots only |
| the same constant on the critical path (`T_mat5_dep` vs `T_ld_dep`) | 18.8 vs 12.8 | a pool load (4) beats a 5-op chain (10) when the value is needed at once |

### 4.7 SMT siblings (`probes/smt_cpu128.csv`)

Core 32 (CPUs 128-131). `spin` is a register-only add loop. `far3` puts three spinners on
a different core of the same chip.

| Workload | solo | 1 sibling busy | 3 siblings busy | 3 far cores busy |
|---|---|---|---|---|
| `A_indep8` ns/iter | 0.73 | **4.52** (6.2x) | 3.17 | 0.73 |
| `A_chain1` | 4.25 | 4.73 | 4.34 | 4.24 |
| `S_ctx_rt1` | 4.70 | 4.37 | 4.37 | 4.69 |
| `D_bctr_rand4` | 7.43 | **14.07** | 11.01 | 7.44 |
| `B_r2_bl_blr` | 5.53 | 10.50 | 7.70 | 5.52 |
| `B_bctr_next` | 1.59 | 2.63 | 2.66 | 1.59 |
| POWERarm crc32 (ms/rep) | 1477 | 1383 | 1315 | 1454 |
| POWERarm vm | 3313 | **6255** (1.9x) | 5931 | 3304 |
| native crc32 | 178 | 190 | 182 | 178 |
| native vm | 457 | **1317** (2.9x) | 1040 | 455 |

Reading [MEASURED + DOC]: with idle siblings the core is in ST mode (BTAC on, 6-wide
decode, whole ICT and link stack), and a far busy core changes nothing. One active
sibling switches the core to SMT2: branch-dense and dispatch-dense code loses 1.9-6x; the
store-forwarding-bound crc32 loses nothing (and gains a little, plausibly from the
sibling keeping the core out of idle states). The recommendation is in §6, rule 7.

## 5. The M1 baseline stalls, explained

### 5.1 crc32: confirmed, and the accounting

The guest loop (`out/aarch64/crc32` at 0x400624) is 23 instructions: four
`ldrb`/`eor`/`and`/`ldr`/`eor` byte steps with w9 (the CRC), x11 (the byte pointer), w12
(scratch), x10 (the counter), x27/x28 (bases). None of w9-w12 is in the static register
set (`a64::SRA` = X0-X8, X19-X24, X29, X30, SP; `PPC64Emitter.h:300`). The host block
(`probes/hot.crc32.crc32_0x624.objdump`, 110 instructions on the hot path, 0x8000454039fc
to 0x800045403bb4) shows what that costs per byte step:

```
ld   r24,144(r27)      ; w9      <- context
ld   r25,168(r27)      ; w12     <- context (just stored)
xor  r24,r25,r24
clrldi r24,r24,32      ; 32-bit zero-extension invariant
std  r24,168(r27)      ; w12     -> context
ld   r24,168(r27)      ; w12     <- the slot just written (same address, same size)
clrldi r24,r24,56
std  r24,168(r27)
ld   r24,288(r27)      ; x27
ld   r25,168(r27)      ; w12 again
sldi r25,r25,2 ; add ; lwz r24,0(r24)   ; the table lookup
std  r24,168(r27)
ld   r24,144(r27) ; srwi ; ld r25,168(r27) ; xor ; clrldi ; std r24,144(r27)   ; w9 -> context
```

Per byte step: about 11 loads and 6 stores (45 loads, 23 stores and 111 instructions on
the 110-word hot path), 17 of the stores to the two scratch slots +160/+168 that are
reloaded a few instructions later. Per iteration: 30 store forwards (`pm_st_fwd` 1.51 G /
50.3 M iterations), i.e. every reload after a store forwards, and the loop-carried CRC
value w9 passes through memory once per byte. Each memory hop is 4 (load-to-use) + 2 (forward) cycles [DOC
Table 25-6] against 2 for a register hop, and the chain per byte has 5 such hops plus 5
ALU ops, so ~40 cycles per byte of pure latency, 160 per iteration; the measured 328
cycles per iteration (16.5 G / 50.3 M) against 40 native leaves the rest to the two
table-lookup chains that also run through memory and to issue-queue pressure: the four
13-entry slice queues hold ~52 iops, and with 45 loads and 23 stores per iteration
waiting on each other the dispatcher is held on a full issue queue essentially every
cycle (`pm_disp_held_issq_full` 16.5 G, `pm_disp_held_hb_full` 3.5 G) [MEASURED
`emu_stat.csv`]. Nothing else contributes: `pm_ld_miss_l1` 1.9 M, mispredicts 0.12 M,
exec-unit stalls 5 M.

The probe that isolates the mechanism with the JIT's exact shape is
`S_ctx_crcshape` vs `S_reg_crcshape`: 42.9 vs 26.9 cycles for one byte step with three
context slots (1.6x), and `S_ctx_rt4_x8` vs `S_blocklocal`: 139 vs 37 cycles for 32 ALU
ops (3.8x) when the round trip is per-op rather than per-block. On the A76 the same
`S_ctx_crcshape` costs 13 cycles against 7 in registers (1.9x): its 5.5-cycle round trip
is cheaper in cycles and in clock, which is one reason the crc32 ratio to the Pi (7.3x)
is worse than to native POWER9 (8.1x) once the 1.58x clock difference is removed.

So the hypothesis stands: the store-forwarding stalls come from context-backed guest
registers. The refinement is that `pm_cmplu_stall_st_fwd` (15.6 %) is only the forwarding
delay; the full cost of the memory chain is in `pm_cmplu_stall_store_finish` (33 %),
`pm_cmplu_stall_load_finish` (13 %) and the issue-queue-full hold, which is why an
operation-count view under-ranks it.

### 5.2 vm: the exec-unit stalls are the `mtctr` in the L1 probe

`pm_cmplu_stall_exec_unit` 3.67 G is entirely `pm_cmplu_stall_fxu` 3.65 G [MEASURED], and
`pm_cmplu_stall_fxlong` (divides) is 42 k, so it is not multiply/divide. The probes show
what FXU counts in practice: every `mtctr`;`bctr` pair contributes 2.8-3.2 cycles
(`B_bctr_next`, `Q_loop_bctr_x4`), `mflr`;`mtctr` 12, `mfocrf`;`mtocrf` 24 per 8, and vm's
hot block (`probes/hot.vm.vm_0x688.objdump`) runs this for every guest `br x13`:

```
ld  r4,40(r27)          ; L1 pointer
rldic r6,r24,4,40 ; add r4,r4,r6
ld  r6,8(r4)            ; entry.GuestCode        (4 cycles after the add)
cmpd cr7,r6,r24 ; bne miss
xor r5,r6,r6 ; ldx r5,r4,r5   ; entry.HostCode    (4 more)
mtctr r5                ; 5 cycles [DOC p.342]
std r24,16(r27) ; bctr
```

That is a dependent chain of about 4 + 2 + 4 + 2 + 4 + 5 = 21 cycles from the guest target
register to a branch that can complete, plus the round trips that compute x13 itself
(`adr` materialised as `lis`/`ori` and *stored to* context, `ldrb`, reloaded, shifted,
added, stored, reloaded). vm executes 316 M guest `br` per repetition and 58 % of 12.5 G
cycles land in this block: 23 cycles per dispatch, of which 3.6 are the 14.4 %
mispredicts at ~25 cycles (3.4 G, 9 % of vm) and the rest is this chain. The FXU stall is
the `bctr` (and everything younger) waiting on `mtctr`, next to finish, every time.

The other 42 % is the four opcode handlers (blocks 0x65c, 0x678, 0x6d8, 0x6fc), each with
the crc32 pattern: `pm_st_fwd` 7.58 G for 3 reps = 8 forwards per guest `br`, and
`pm_cmplu_stall_load_finish` 9.5 G (25 % of vm).

### 5.3 The other baseline observations

- **3.2 host branches per crc32 iteration**: the back-edge is `subfco.`;`beq`;`b`;`b`
  (invert, skip, jump) plus the poke; all predicted; cost about 2 x 1-1.5 cycles per
  iteration [MEASURED `B_b8_taken`], under 1 % of crc32. Correct in the baseline, low
  priority.
- **Branch-misses 0.03 M** on crc32: consistent; nothing to do.
- **`pm_cmplu_stall_lhs` 19 M in vm, 0.36 M in crc32**: a few loads hit two stores per
  slice (the byte-wide `stb` pokes next to wider loads, or narrow guest stores followed
  by wider loads); small today, but the rule in §6.2 stops it growing.
- **Count cache and the mitigation**: `spectre_v2` reports "Software count cache flush
  (hardware accelerated), Software link stack flush". In this kernel that patches three
  call sites in `_switch` (`arch/powerpc/kernel/switch.S:20-26`) to `flush_branch_caches`,
  which executes 64 x `bl .+4` (link stack) and the `bcctr 2,0,0` flush encoding
  0x4c400420 (`PPC_INST_BCCTR_FLUSH`, count cache) [DOC `security.c:438-495`,
  `ppc-opcode.h:267`]. It runs only on a context switch; syscalls and interrupts that
  return to the same task do not flush. The count cache is enabled ("Indirect branch cache
  disabled" would be the disabled string). A pinned POWERarm thread with idle siblings
  context-switches a few times per second (HZ=1000, no competing runnable task), so
  the cost is a few hundred cold `bctr` per second, unmeasurable against vm's 136 M
  mispredicts per run. The probes reproduce the mispredict rate with no context switches
  at all.

## 6. Code-shaping rules for the POWERarm JIT, ranked by expected measured impact

Each rule: evidence, expected impact on the M1 workloads, how to verify after
implementation. Expected impacts are [INFERENCE] from the probe ratios unless marked.

### Rule 1: keep guest registers in host registers across a block; never round-trip a slot inside a block

- Evidence: §5.1; `S_ctx_rt4_x8` vs `S_blocklocal` 3.8x [MEASURED]; `S_ctx_crcshape` vs
  `S_reg_crcshape` 1.6x [MEASURED]; 30 forwards per crc32 iteration [MEASURED]; the
  `std`-then-`ld` of the same slot six times per byte in the hot block.
- What to do, in order of return: (a) the redundant same-slot reload after a store is
  free to remove and is 6 cycles on the chain each time; (b) block-local allocation of
  non-SRA guest registers: load once at entry, write back at exit (the SpillStaticRegs
  path on the dispatcher exit already writes 18 slots, so the write-back cost is
  bounded); (c) re-rank the static set by AArch64 usage: clang scratch X8-X17 and the
  loop-state callee-saved X19-X28 are the hot registers in compiled code, while X0-X7
  are argument registers that go cold after the prologue. The host has r24-r26, r30, r31
  free in `RA` plus the 18 SRA slots; the trade-off is against the temporaries the
  lowering needs.
- Expected impact: crc32 from 8.1x native toward ~3x (the per-iteration chain drops from
  ~5 memory hops per byte to the one native dependency per byte); vm's handler blocks
  (42 % of vm) by a similar factor; sha256 and sort (register-bound) proportionally.
  This is the largest single item by a wide margin.
- Verify: `pm_st_fwd` per guest instruction -> ~0 in the hot blocks;
  `pm_cmplu_stall_store_finish` + `pm_cmplu_stall_load_finish` + `pm_cmplu_stall_st_fwd`
  from 61 % of crc32 cycles toward native's 50 % (native crc32 is itself
  latency-bound: `pm_disp_held_issq_full` is 100 % there too); IPC from 0.34 toward 0.63.

### Rule 2: a guest `BR` gets an inline cache before the count cache; expect 25 cycles per mispredict

- Evidence: §4.5 [MEASURED]: the count cache is last-target per site; a compare chain
  on a period-8 program is 2.1x faster; random dispatch is unchanged by the chain;
  vm mispredicts 14.4 % of 949 M `bctr` [MEASURED baseline]; the A76 predicts the same
  patterns [MEASURED Pi 5], so guest code that is cheap on the reference is
  systematically mispredicted here.
- What to do: at each guest `BR` site, emit 2-4 `cmpd`/`beq` entries against the most
  recent targets (filled by the linker on first sight, patched on miss) before the L1
  probe. Keep the compare chain's branches out of the 32-byte block that holds the fallback
  `bctr` (rule 4) and keep the chain short: each entry is `cmpd`+`bc` = 2 issue slots and
  ~1 cycle when not taken (`B_bc8_packed`).
- Expected impact on vm: the mispredicts (9 % of cycles) plus the 21-cycle probe chain on
  hits, replaced by a ~6-cycle compare chain on the predictable fraction: up to ~30 % of
  vm. The count cache still serves genuinely random targets at the same cost as today.
- Verify: `pm_br_pred_ccache` count falls (fewer `bctr` executed), `pm_br_mpred_ccache`
  falls faster; cycles in the block ending in the guest `br` (perf map) fall from
  23 per dispatch.

### Rule 3: pair every guest call with an LK=1 host branch and every guest RET with `blr`; budget the link stack at 64 (16 under SMT4)

- Evidence: `B_r2_bctrl_blr` and `B_r2_bl_mtlr_blr` predict perfectly (LR may be reloaded
  from a shadow stack for +0.5 cycles); `B_r2_bl_bctr` mispredicts whenever the caller
  changes (+24 cycles); `B_r2_b_mtlr_blr` (a `blr` with no push) mispredicts every time
  (+25.5) [MEASURED]. The link stack holds 64 returns in ST mode, 48-deep recursion is
  clean, 96-deep costs one mispredict per unwind [MEASURED `B_depth*`; DOC p.328]. The
  backend's FEX_SHADOWRETSTACK pairing (`BranchOps.cpp:412-470`) is the right design; the
  probes confirm its premises on POWER9 (on the POWER8 it was designed against the count
  cache predicted nothing; on POWER9 it predicts the last target).
- What to do: keep the pairing on by default; make sure the RET fast path never falls to
  `mtctr`/`bctr` when the shadow entry matches; use `bctrl` (not `bl` to a thunk plus
  `bctr`) for indirect guest calls so the push happens at the real call.
- Expected impact: sort (BL/RET through a comparator pointer) is the workload to measure;
  vm's CALL/RET are inside the guest interpreter and go through `br`, so rule 2 governs
  them.
- Verify: `pm_br_mpred_lstack` stays at noise on sort; `pm_br_pred_lstack` equals the
  guest RET count.

### Rule 4: one count-cache branch per aligned 32-byte block, and put the `mtctr` as early as the value allows

- Evidence: two predicted `bctr` in one 32 B block cost +16 cycles and one guaranteed
  mispredict per iteration [MEASURED `B_bctr2_sameblock`; DOC p.329 "one branch per
  cycle... no more than one such branch per aligned 32-byte block"]; `mtctr` is 5 cycles
  on the execution slice and the `bctr` cannot finish before it [DOC p.342; MEASURED
  fxu 2.8 per pair]. Conditional-branch density is free (8 `bc` per 32 B, no penalty).
- What to do: the linker and the block-exit emitter must align so that the L1-probe
  `bctr`, any inline-cache fallback `bctr`, the miss-leg exit `bctr` and the trampolines
  never share a 32-byte block (the hot blocks dumped here happen to comply; the risk is
  in the small thunks). Hoist `mtctr` above the `std State.rip` and the CR compares that
  precede the branch, so its 5 cycles overlap them.
- Expected impact: 0 today (the dumped blocks comply), +16 cycles per offending exit if
  a layout change breaks it; this is a rule to keep, not a win to take.
- Verify: `pm_br_mpred_ccache` on a workload with all-monomorphic `bctr` sites must be
  ~0; a nonzero rate with monomorphic targets is the tell for a shared block.

### Rule 5: flags live in CR fields; XER is written but never read on the hot path

- Evidence: §4.2 [MEASURED]. The backend already keeps N/Z in CR0 and C/V in XER via
  `addco`/`subfco.` (`ALUOps.cpp:1171-1209`) and only materialises NZCV with
  `mfocrf`+`mfxer` on the dispatcher exit path (hot block tail): that is the right split.
  The costs to avoid: consuming CA/OV on a chain (+4 cycles per op), `mfxer` (+11),
  `mfocrf`/`mtocrf` (+7), `mfcr` (+11), `mcrxrx` (+3 over `cmpld`).
- What to do: for a guest branch on C or V after `SUBS`/`CMP`, prefer the CR route
  (`cmpld` for C, `cmpd`+ overflow-free forms, or `subfco.`-free lowering) so the
  consumer reads a CR field; reserve the XER route for `ADCS`/`SBCS` chains where the
  ISA-native `adde` (6.3 cycles per op) still beats any software carry (12.9 for the
  `srdi` form). Never read CA into a GPR in a loop. Note the `XER.SO` hazard: the first
  overflowing `addco` sets SO, which is a pipeline flush [DOC p.342]; since the backend
  never clears XER (no `mtxer` in the PPC64 JIT) this happens once per thread, which is
  fine, and clearing it would cost a 37 ns dependent `mtxer` (handbook `xerhead.c`).
- Expected impact: small on the M1 workloads (their flag consumers are `b.ne` on CR0);
  large on guest code with `adc`/`sbc` chains or frequent `b.cs`/`b.vs` (bignum, checked
  arithmetic).
- Verify: `pm_cmplu_stall_fxu` on a flag-heavy guest; the probe ratios above.

### Rule 6: shape blocks for the fetch redirect: fall through on the common path, one taken branch per block, and accept the ~7-cycle loop floor

- Evidence: 7.0 cycles for the smallest loop, 12.8 for four blocks each ending in a
  taken branch, 1-2 cycles per additional forward taken branch, 2.5 per predicted `bctr`
  [MEASURED §4.1]; the A76 does the same loop in 1 cycle. Predicted-taken latency 3-5
  cycles, BTAC 1 cycle ST-only [DOC p.327, 330].
- What to do: (a) invert conditions so the frequent successor falls through (the JIT's
  `beq`;`b`;`b` back-edge in crc32 is two taken branches where one would do); (b) link
  the loop back-edge directly to the block head, as today; (c) do not expect small-loop
  guests to go below ~7 host cycles per iteration whatever the code quality, and rank
  block-body work above branch shaving.
- Expected impact: ~1 % of crc32 for (a); the value of (c) is in not chasing it.
- Verify: taken branches per guest iteration (`pm_br_taken_cmpl` / iterations) -> 1.

### Rule 7: run one guest thread per core with idle siblings; do not co-schedule anything on them

- Evidence: §4.7 [MEASURED]: one busy sibling costs emulated vm 1.9x, native vm 2.9x,
  branch/dispatch probes 1.9-6x; idle siblings are indistinguishable from ST mode
  (BTAC on, whole decode, 64-entry link stack) and a far busy core costs nothing.
- What to do: pin POWERarm to the first thread of a core and keep the other three empty
  (cpuset or `taskset`); on a shared machine `ppc64_cpu --smt=1` is the safe default for
  latency-sensitive guests. Measurements comparing JIT versions must keep the siblings
  idle or they will move by more than the change under test. For throughput (many
  independent guests) SMT2/SMT4 may still win in aggregate; that is a different
  measurement and was not made here.
- Verify: `pm_run_cyc_st_mode` and `pm_run_cyc_smt4_mode` during the run (there is no SMT2
  mode event in this kernel's list).

### Rule 8: lower guest barriers and exclusives to the lightest POWER form the memory model allows

- Evidence: `lwarx`/`stwcx.` 74 cycles vs 23 for a plain RMW; `lwsync` +14, `sync` +42,
  acquire/release around a larx/stcx +23 [MEASURED §4.6]; EH hint honoured [DOC p.353].
- What to do: `DMB ISHLD`/`ISHST` -> `lwsync`, `sync` only for a full `DMB ISH`/`SY`
  that the handbook's memory-ordering note requires; set EH=0 on atomic updates; keep
  the `bne-` in the retry loop (the UM honours static prediction near `l*arx`, p.328).
- Expected impact: none on the M1 integer set; decisive for lock-heavy guests.
- Verify: `pm_cmplu_stall_any_sync`, `pm_lsu_stcx`, `pm_lsu_stcx_fail`.

### Rule 9: constants off the critical path are cheap to materialise; on it, load them

- Evidence: 5-op materialisation costs dispatch slots only (12.8 vs 9.7 cycles per four)
  but 10 cycles of latency on a chain (18.8 vs 12.8) [MEASURED §4.6]. The vm block
  materialises `adr` results with `lis`/`ori` and then stores them to context, which is
  rule 1's problem, not this one.
- What to do: keep `lis`/`ori` sequences for constants that feed a store or an address
  base; use a pool load (`ld` off `STATE` or `addpcis`+`ld` on POWER9) when the constant
  feeds the next dependent ALU op.
- Expected impact: a few percent of instruction count; no cycles on the M1 set.

### Rule 10: never let a wider load consume two narrower stores

- Evidence: two `stw` feeding one `ld`: +21 cycles and no forward [MEASURED
  `S_fwd_2stw_ld`; DOC p.346]; two `stb` feeding one `lhz` the same. One store feeding a
  wider or narrower load forwards fine (+2.4 / +0).
- What to do: when guest code writes a 32-bit register half and then reads the 64-bit
  register (or `strb`/`strh` then `ldr` of a struct field), lower the write as a full-slot
  read-modify-write in a register rather than a narrow store; keep context slots written
  by one store size. `pm_cmplu_stall_lhs` is the counter (19 M in vm today).
- Expected impact: small now; rule 1 removes most of the narrow context stores anyway.

## 7. Where POWER9 can beat the A76 despite emulation

- **Cache- and memory-bound guests.** Native bst is 3.2x faster on POWER9 than on the
  Pi [MEASURED baseline]; POWERarm bst is 1.22x the Pi, i.e. the emulation overhead
  (3.97x) is absorbed by the hierarchy: 512 KiB L2 per core pair at 15.5 cycles, 10 MiB
  L3 at 35.5 cycles, 68 ns local DRAM, 8 prefetch streams with stride detection [DOC
  UM Table 25-7 p.356, §25.1.7.12 p.351]. The A76 on the Pi has 512 KiB L2 per core and a
  2 MiB L3 in front of LPDDR4X. Any guest whose working set is between ~2 MiB and
  ~10 MiB per core pair, or that is latency-bound on DRAM, should run at or better
  than Pi speed under POWERarm once the JIT stops adding memory traffic of its own
  (rule 1). Expect this class: tree/graph walks, hash tables, database-like access,
  interpreters with large heaps.
- **Streaming bandwidth.** The AC922 has eight DDR4 channels per socket ("up to 120 GB/s
  sustained" [DOC HC28 slide 12, direct-attach memory]) against the Pi 5's single LPDDR4X
  channel (order of 17 GB/s, [SPEC], not measured here). memcpy/memset-heavy guests win
  once the NEON lowering exists to emit 16-byte VSX loads and stores (one 16 B load and
  one 16 B store per LS-slice pair per cycle [DOC p.348]).
- **Throughput per socket.** 22 cores x SMT4; a batch of independent guests scales to 22
  ST cores at the numbers above, and probably further at SMT2 for memory-bound guests
  (not measured; the SMT experiment shows the per-thread cost).
- **Clock.** 3.8 GHz against 2.4 GHz is 1.58x for anything bound by cycles rather than
  by fetch redirects or memory latency; it is already in the native columns.

Where POWER9 does not win: tight integer loops (2-cycle ALU, 7-cycle loop floor against
1 and 1), interpreter dispatch (last-target count cache, ~25-cycle mispredict against a
pattern-learning predictor and ~15), and flag-chain arithmetic (XER against renamed
NZCV). sha256, sort and vm will stay behind the Pi even with a perfect register
allocator; the target for them is native-POWER9-ratio, and the honest number for
"POWERarm vs Pi" on those is 1.5-2.5x slower after rule 1 and 2 [INFERENCE].

## 8. What to measure next

1. Rule 1 prototype on crc32: `pm_st_fwd`, `pm_cmplu_stall_store_finish`,
   `pm_disp_held_issq_full` before/after; target `pm_st_fwd` per iteration < 4.
2. Per-site indirect mispredicts: `perf record -e pm_br_mpred_ccache:u` with the perf
   map, to rank guest `BR` sites for the inline cache (rule 2), on vm and on a real
   interpreter (Lua or Python once libc startup works).
3. sort under `pm_br_pred_lstack`/`pm_br_mpred_lstack` to validate rule 3 on real
   `BL`/`RET` traffic, and the same with a sibling busy (link stack 16).
4. `pm_run_cyc_st_mode`/`pm_run_cyc_smt4_mode` logged by `run-power9.sh`, so every future timing
   records the SMT mode it ran in.
5. bst under `pm_cmplu_stall_dcache_miss`, `pm_cmplu_stall_dmiss_l2l3`,
   `pm_cmplu_stall_dmiss_lmem`, `pm_lsu_dtlb_miss_64k`: the memory-bound class in §7 needs
   its own baseline, including the 64 KiB-page TLB advantage.
6. The `mulld` latency (8.7 measured vs 5 documented): check UM Table A-1 for the
   `mulld` iop class and whether the DP-MUL pipe's interleaving rule applies; it matters
   for hash and PRNG guests.
7. A76 branch-predictor structure sizes, if Arm publishes them: the table in §2 has
   "not public" where a JIT author would want numbers.

## 9. Files

- `POWER9-VS-A76-PIPELINE.md` (this note)
- `probes/pipeprobe.c`: 83 POWER9 probes; `pipeprobe list|time|once <name> [iters]`
- `probes/run_probes.sh`: parity, wall time, then four PMC groups per probe -> CSV
- `probes/spin.c`, `probes/smt_probe.sh`: the SMT-sibling experiment
- `probes/emu_stat.sh`: POWERarm vs native stall breakdown, eleven PMC groups
- `probes/dump_hot_block.py`: dumps named JIT blocks from a child POWERarm via
  `/proc/<pid>/mem` (works under `ptrace_scope=1`) and disassembles them
- `probes/a76probe.c`: the Pi 5 side, 15 probes
- logs: `probes_cpu120.csv`, `probes_q_cpu136.csv`, `smt_cpu128.csv`, `emu_stat.csv`,
  `a76probe.pi5.log`, `hot.crc32.crc32_0x624.objdump`, `hot.vm.vm_0x{65c,678,688,6d8}.objdump`

Build and run on the POWER9:

```sh
gcc -O2 -mcpu=power9 -fno-pie -no-pie -o pipeprobe pipeprobe.c && gcc -O2 -o spin spin.c
./run_probes.sh 120 4000000 probes.csv            # all probes, CPU 120
./smt_probe.sh 128 smt.csv                        # needs build-rp/Bin/POWERarm and A64Bench out/
./emu_stat.sh <POWERarm> <A64Bench dir> 120 emu_stat.csv crc32 vm
python3 dump_hot_block.py <POWERarm> <guest> 124 hot.crc32 "crc32+0x624"
```
