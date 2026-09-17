# A64Bench: time integer AArch64 workloads under POWERarm

A64Bench is a small set of freestanding, integer-only AArch64 workloads, timed three ways:

- natively on a real ARM core (the reference is a Raspberry Pi 5, Cortex-A76)
- natively on the POWER9, built from the same C sources for ppc64le
- under POWERarm on the POWER9, running the identical AArch64 binaries

Comparing POWERarm with the Pi mixes CPU speed with emulation cost. Comparing POWERarm
with native ppc64le on the same core isolates the emulation overhead. The first baseline
and its findings are in [`docs/powerarm/BENCH-M1-BASELINE.md`](../../docs/powerarm/BENCH-M1-BASELINE.md).

## Workloads

Every binary is static, links no libc, and makes only three syscalls: `write`,
`exit_group` and `clock_gettime`. Each one prints a checksum.

| name | what it does | pipeline behaviour it stresses |
|---|---|---|
| `crc32` | table-driven CRC-32 over 1 MiB, 64 passes | byte loads, table lookups, short loop-carried chain |
| `sha256` | SHA-256 of 1 MiB, 48 chained passes | 32-bit rotate/add dependency chains, little memory |
| `bst` | unbalanced BST: 400k random inserts, 400k lookups, in-order walk | data cache misses, data-dependent branches |
| `vm` | switch-dispatched bytecode interpreter (Collatz with CALL/RET) | indirect branches (count cache) |
| `sort` | recursive quicksort of 2.5M u32 through a comparator pointer | unpredictable branches, indirect calls, BL/RET pairs |

Usage: `<binary> [scale] [reps]`. Each repetition prints `ns=` (measured in the program
with `CLOCK_MONOTONIC`) and `sum=`. The final line gives `parity=ok` only if every
repetition produced the same checksum. Under POWERarm, rep 0 includes translating the hot
code ("cold") and reps from 1 on are steady state ("warm").

## Integer-only constraint

The POWERarm frontend has no FP/SIMD support yet, so the workloads must not touch V
registers:

- AArch64 is built with `-mgeneral-regs-only -ffreestanding -fno-builtin`, and the runtime
  in `src/bench.h` provides byte-loop `memcpy`/`memset`.
- `build-aarch64.sh` disassembles every binary and fails if any `b/h/s/d/q/v<n>` register
  or FP/SIMD mnemonic appears. The check has a negative control: rebuilding without
  `-mgeneral-regs-only` is rejected.
- ppc64le is built with `-mno-altivec -mno-vsx -msoft-float`. `build-ppc64le.sh` rejects
  any `f<n>`/`v<n>`/`vs<n>` register use. The native side is therefore not auto-vectorised
  either, which keeps it the same program as the guest.

## Reproducing

```sh
# On the AArch64 reference machine (the Pi sees the worktree over sshfs):
unittests/A64Bench/build-aarch64.sh
unittests/A64Bench/run-pi.sh                # -> results/pi.csv (CPU=3 RUNS=3 REPS=3)

# On the POWER9, after building POWERarm into build-bench/:
unittests/A64Bench/build-ppc64le.sh         # -mcpu=power8 and -mcpu=power9
unittests/A64Bench/run-power9.sh            # -> results/power9.csv (CPU=88 NODE=0 RUNS=5 REPS=3)
unittests/A64Bench/summarize.py results/pi.csv results/power9.csv
PERF=perf unittests/A64Bench/profile-power9.sh vm sort   # the worst-ratio workloads
```

`results/` and `out/` are not committed.

## Method

- **Parity before timing.** `summarize.py` compares every configuration's checksums
  with the AArch64 reference. It prints no timing for a workload that mismatches.
- **Pinning.** On the Pi: `taskset -c 3`, one untimed warm-up run, runs strictly sequential.
  The `ondemand` governor is left alone and its state is recorded before and after. On the
  POWER9: one hardware thread on NUMA node 0 (`numactl --membind=0 taskset -c 88`). Other
  users are kept on node 8.
- **Load gate.** Before each process, `run-power9.sh` measures node 0 busy % over 1 s
  from `/proc/stat`. It waits while that exceeds `MAXBUSY` (default 10 %) and records the
  value in the CSV.
- **Interleaving and control.** The run index is the outermost loop, so drift lands on every
  configuration equally. A native `-mcpu=power9` `crc32` control runs first and last.
  `summarize.py` declares the whole run invalid if the control moves by more than 2 %
  (handbook: low spread is not low error).
- **Repetitions.** Five process runs × three in-process repetitions on the POWER9 (three × three
  on the Pi). Cells report the median over runs, with min and max. Wall time per process
  is recorded separately from in-program time. A `scale=1` startup probe measures
  process start plus translation.
- **Profiling.** `POWERARM_BLOCKJITNAMING=1` makes the JIT write `/tmp/perf-<pid>.map`.
  `perf record -e cycles:u` then attributes samples to JIT blocks, the dispatcher, or the
  emulator's own code. `kernel.perf_event_paranoid=2` hides kernel samples, so kernel time
  comes from `rusage` instead. `perf stat` runs one POWER9 event group per process
  (6 counters) so counts are not multiplexed.

## What to re-run at each milestone

Rebuild POWERarm, then run `run-power9.sh` and `summarize.py`. The AArch64 binaries and
`pi.csv` only need regenerating when `src/` changes. Once FP/NEON lands, glibc-static
programs start, and workloads that need libc (CoreMark, real compressors) can be added
next to these.
