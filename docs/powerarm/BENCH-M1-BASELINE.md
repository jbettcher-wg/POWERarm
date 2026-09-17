# POWERarm M1 benchmark baseline

This is the first timing of POWERarm against a real ARM core. It uses five small
freestanding, integer-only workloads (`unittests/A64Bench`) and is sized as a quick look,
not an exhaustive benchmark. Re-run it at every milestone with the scripts in
`unittests/A64Bench/README.md`. The raw CSVs, `perf stat` output and profiles are in
[`bench-m1-baseline/`](bench-m1-baseline/).

Tags: **[MEASURED]** means the numbers come from these runs. **[SPEC]** marks an
inference that no experiment here has isolated.

## Machines and configuration

| | Raspberry Pi 5 (reference) | POWER9 (AC922 8335-GTH) |
|---|---|---|
| CPU | Cortex-A76, 4 cores, L2 512 KiB/core | POWER9 DD2.3, 2×22 cores, SMT4, L3 10 MiB/core |
| Kernel | 6.18.39+rpt-rpi-2712 (4K pages) | 7.2.6-64k (64K pages, Radix) |
| Governor | `ondemand`, 2.4 GHz before/after (not changeable) | `ondemand`; cpu88 2.33 GHz before, 3.8 GHz (boost) after |
| SMT | n/a | on (SMT4); one hardware thread used, siblings idle |
| Pinning | `taskset -c 3`, sequential, one untimed warm-up | `numactl --membind=0 taskset -c 88` (node 0); other work kept on node 8 |
| Load | loadavg 1.6–1.8 (desktop session on other cores) | node-0 busy before each process: median 1 %, max 4 % |
| Compiler | Debian clang 19.1.7, `-O2 -mgeneral-regs-only -march=armv8-a -mtune=cortex-a76` | clang 22.1.8, `-O2 -mcpu=power8` / `-mcpu=power9`, `-mno-altivec -mno-vsx -msoft-float` |
| Emulator | – | POWERarm `dcfb220e8` (`powerarm-m1/bench` base), Release, `POWERARM_HOSTPAGEMODE=force` |

Repetitions: five process runs × three in-process repetitions on the POWER9, three × three
on the Pi. The native `-mcpu=power9` `crc32` control drifted by +0.04 % between the start
and the end of the run, so the run is valid. `qemu-aarch64` is not installed on the POWER9,
and installing packages is out of scope, so there is no QEMU column.

The compiler versions differ (clang 19 on the Pi, clang 22 on the POWER9). The POWER9
native column is the same C source, not the same compiler version.

## Parity [MEASURED]

Every configuration reproduced the Pi's checksum in every repetition.

| bench | Pi checksum | native P8 | native P9 | POWERarm |
|---|---|---|---|---|
| bst | `0ded589ff2a3247b` | match | match | match |
| crc32 | `323d083d79762f78` | match | match | match |
| sha256 | `4238e6322fef7301` | match | match | match |
| sort | `f05bf4351ec0e785` | match | match | match |
| vm | `0000814e78cfc084` | match | match | match |

## Timing [MEASURED]

Each cell is ms per in-program repetition: the median over process runs, with [min–max].
Cold is rep 0, which includes translation. Warm is the median of reps 1–2. Startup is the
wall time of a whole process at `scale=1`.

| bench | Pi 5 | P9 native P8 | P9 native P9 | POWERarm cold | POWERarm warm | **POWERarm / Pi** | **POWERarm / native P9** | startup POWERarm / native |
|---|---|---|---|---|---|---|---|---|
| bst | 442 [440–444] | 137 [136–145] | 136 [136–137] | 543 [541–544] | 540 [540–542] | **1.22×** | **3.97×** | 32 / 7 ms |
| crc32 | 197 [197–198] | 178 [178–178] | 178 [178–178] | 1449 [1448–1450] | 1448 [1448–1452] | **7.34×** | **8.14×** | 76 / 12 ms |
| sha256 | 293 [293–293] | 357 [357–359] | 362 [362–362] | 1485 [1484–1485] | 1484 [1484–1488] | **5.07×** | **4.10×** | 83 / 18 ms |
| sort | 282 [281–283] | 349 [349–349] | 348 [347–348] | 1064 [1063–1065] | 1062 [1062–1063] | **3.77×** | **3.06×** | 34 / 6 ms |
| vm | 428 [428–428] | 455 [454–456] | 454 [454–454] | 3310 [3303–3312] | 3309 [3306–3310] | **7.73×** | **7.28×** | 32 / 6 ms |

Headline figures:

- **Emulation overhead** (POWERarm / native ppc64le on the same core): 3.1× to 8.1×,
  geometric mean 4.9×.
- **POWERarm against the Pi 5:** 1.2× to 7.7× slower, geometric mean 4.2×.
- **Memory-bound work is closest to the Pi.** On `bst`, the POWER9's cache hierarchy
  makes native 3.2× faster than the Pi, which absorbs most of the emulation cost.
- **Tight register-bound loops are furthest from the Pi** (`crc32`, `vm`).
- `-mcpu=power8` and `-mcpu=power9` native builds are within 1.5 % of each other.

## Profile: the two worst workloads (crc32, vm) [MEASURED]

Method:

- `POWERARM_BLOCKJITNAMING=1` makes the JIT write `/tmp/perf-<pid>.map`.
- `perf record -e cycles:u -F 4999` was pinned the same way as the timings.
- `kernel.perf_event_paranoid=2` hides kernel samples, so kernel time comes from `rusage`.

| where the time goes | crc32 | vm |
|---|---|---|
| JIT-generated code (`[JIT]` DSO) | 99.86 % | 99.93 % |
| POWERarm C++ (dispatcher entry, syscalls, loader) | 0.11 % | 0.05 % |
| ld64 / libs | 0.03 % | 0.02 % |
| kernel (rusage sys / user) | 0.004 s / 4.37 s | 0.006 s / 9.91 s |
| hottest guest block | `crc32+0x624` (the whole inner loop): 99.5 % | `vm+0x688` (the 5-instruction `add; adr; ldrb; add; br x13` dispatch block): 57.8 % |

Nothing is spent in the dispatcher or in translation. The 7–8× cost is the quality of
the code generated for the steady-state loop. In `vm`, 58 % of cycles land in the block
that ends in the guest's indirect `br`, which holds the inline L1 lookup probe and the
host `bctr`. The next four blocks, the individual opcode handlers, take 4–12 % each.

## Stall and miss comparison [MEASURED]

These are user-space counts for three repetitions of each workload. `cycles`,
`instructions` and `branches` were multiplexed at 49–75 % and are scaled by perf. The
`pm_*` events ran at 100 %.

| event | crc32 POWERarm | crc32 native P9 | vm POWERarm | vm native P9 |
|---|---|---|---|---|
| cycles | 16.59 G | 2.03 G | 37.71 G | 5.14 G |
| instructions | 5.62 G | 1.27 G | 37.51 G | 7.94 G |
| IPC | **0.34** | 0.63 | **0.99** | 1.54 |
| branches | 162.0 M | 50.6 M | 4179 M | 2271 M |
| branch-misses | 0.03 M | 0.0004 M | 162.8 M | 17.4 M |
| pm_cmplu_stall (all stalled cycles) | 10.97 G | 1.02 G | 23.41 G | 2.16 G |
| pm_cmplu_stall_st_fwd (store forwarding) | **2.58 G** | 0.0000005 G | **3.39 G** | 0.107 G |
| pm_cmplu_stall_exec_unit | 0.002 G | 0.0001 G | **3.65 G** | 0.021 G |
| pm_cmplu_stall_fxu | 0.002 G | 0.000002 G | **3.65 G** | 0.021 G |
| pm_br_pred_ccache / pm_br_mpred_ccache | 51 k / 4.7 k | 0 / 0 | **949 M / 136.5 M** | 0 / 0 |
| pm_flush_mpred | 0.12 M | 0.0005 M | **173.2 M** | 17.4 M |
| pm_cmplu_stall_bru | 23 k | 44 | 107.1 M | 3.4 k |
| pm_br_mpred_lstack | 1.2 k | 8 | 1.8 k | 6 |
| pm_ld_miss_l1 | 1.76 M | 1.59 M | 0.56 M | 0.006 M |
| pm_cmplu_stall_lhs | 0.36 M | 0 | 19.3 M | 8 |

Caveat for `vm`: clang lowered the native ppc64le `switch` to a compare tree (the
binary contains no `bctr`), while the AArch64 build dispatches through a jump table
(`br x13`). The native `vm` column therefore has no indirect branch at all. Its 17.4 M
mispredictions are the data-dependent conditionals.

## Top findings, ranked by time impact

**1. Hot guest registers live in memory, and store forwarding stalls the loop.**
**[MEASURED]** counters, **[SPEC]** that spilled registers are the cause.

- Evidence:
  - The static register set covers only X0–X8, X19–X24, X29, X30 and SP
    (`a64::SRA` in `FEXCore/Source/Interface/Core/ArchHelpers/PPC64Emitter.h`, 18 slots).
  - clang uses X9–X17 as scratch and X25–X28 for loop state. Every register in the
    `crc32` inner loop (x9, x10, x11, x12, x27, x28) is outside that set. The `vm`
    dispatch loop is the same: w9, x10–x14, x26, x28.
  - Store-forwarding stalls take 2.58 G of crc32's 16.59 G cycles (15.6 %) against
    essentially zero natively, and 3.39 G of vm's 37.71 G (9.0 %).
  - A loop-carried guest register that round-trips through `CPUState` on every iteration
    turns a register dependency chain into a store→load chain. POWER9 then stalls on each
    forward.
- What would help:
  - Allocate SRA slots by AArch64 usage frequency (X8–X17 first).
  - Or give non-SRA guest registers block-local register allocation, loaded once at block
    entry and written back at exit.

**2. Instruction expansion of about 4.5× over native.** **[MEASURED]**

- Evidence:
  - crc32 runs 5.62 G host instructions against 1.27 G native (4.4×). At 50.3 M inner
    iterations, that is about 111 host instructions per iteration against 25 native, for
    23 guest instructions.
  - vm runs 37.5 G against 7.9 G (4.7×).
  - IPC falls from 0.63 to 0.34 on crc32, so the lost throughput is roughly
    expansion × IPC loss: 4.4 × 1.85 ≈ 8.1×, which matches the measured ratio.
- Part of this expansion is the load/store traffic from finding 1.

**3. Guest indirect branches mispredict in the count cache.** **[MEASURED]**

- Evidence:
  - vm makes 949 M count-cache predictions with 136.5 M wrong (14.4 %).
  - `pm_flush_mpred` is 173 M against 17 M native (+156 M flushes), and
    `pm_cmplu_stall_bru` is 107 M against 3.4 k.
  - The block ending in the guest `br` takes 57.8 % of vm's cycles.
- **[SPEC]** Cost: at about 20 cycles per flush, the extra flushes are about 3.1 G
  cycles, roughly 8 % of vm.
- Link-stack mispredicts are negligible (1.8 k). BL/RET under `BranchHint::None` was not
  exercised here, because neither profiled workload makes guest calls on its hot path (`sort` does, and is the one to profile for it).
- Separately, vm spends 3.65 G cycles (9.7 %) in exec-unit/FXU completion stalls, against
  21 M natively. The cause is not isolated **[SPEC]**: candidates are long dependency
  chains through spilled registers, or the `mul`/flag materialisation in the handlers.

**4. Loop back-edges cost extra host branches.** **[MEASURED]**

- Evidence:
  - crc32 takes 162 M host branches against 50.6 M native: 3.2 per guest loop iteration
    instead of 1.
  - All of them are well predicted (0.03 M misses), so the cost is instructions and
    decode, not flushes.
- A guest backward branch to its own block goes through the block-exit/link sequence
  rather than a direct in-block branch **[SPEC, from the counts]**.

**5. Translation, startup, dispatcher and kernel are not where the time goes.** **[MEASURED]**

- Evidence:
  - Cold (rep 0) is within 0.3 % of warm on every workload.
  - The dispatcher and C++ take ≤ 0.11 % of samples, and sys time is ≤ 6 ms.
  - Process startup is 32–83 ms against 6–18 ms native, a fixed ~25 ms plus emulated setup.
- The code cache knob (`POWERARM_ENABLECODECACHINGWIP=1`, `POWERARM_CODECACHESCOPE=all`)
  wrote nothing under `~/.cache/powerarm/cache` for these static binaries across repeated runs, and the loader reports "Cache file does not
  exist" on every run. With cold ≈ warm it would have no steady-state headroom here anyway.
- Of these five items, this is the least worth optimising for M1.

## Re-run after FP/NEON lands

- **Current runs:** rebuild POWERarm, then run `run-power9.sh`, `summarize.py` and
  `profile-power9.sh crc32 vm`. `pi.csv` stays valid until `src/` changes.
- **New workloads:** once glibc/musl static startup works, add CoreMark with the stock
  POSIX port, a real compressor (lz4/zstd), and a libc `qsort` variant of `sort`. They
  exercise `memcpy`/`memset` NEON paths and BL/RET-heavy code, which would measure the
  link-stack issue that finding 3 leaves unexercised.
- **Native vectorisation:** rebuild the native ppc64le column with VSX allowed, as a
  second native reference.
