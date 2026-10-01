# What the four kernel entry/exit mitigations cost POWERarm

Measured 2026-10-01 on the POWER9 AC922 (`omarchy-power9`), at `d225363aa`.
Nothing was changed: no mitigation was disabled, no reboot, no debugfs write.
This prices the exposure so the decision can be made on evidence.

HANDOVER item 55 flagged this as an unmeasured finding and framed a hypothesis:
that emulation is hit harder than a database was, because POWERarm loses the
lookup cache's 2 MiB L1 table on every block exit, making the mitigations a
per-transition tax rather than a per-query one.

**The hypothesis is wrong, and it is wrong for a reason worth writing down.**
A 2 MiB table cannot be lost from a 32 KiB cache. At any realistic hot-block
count the L1 lookup table is already not L1D-resident, so the L1D flush takes
away nothing it still had — the extra L1D misses per syscall return are 5–7, not
thousands. And block transitions never enter the kernel at all: the kernel is
entered on *guest syscalls*, whose rate emulation barely changes.

The answer, measured end to end:

| | per-syscall-return tax | syscall rate | share of the workload's CPU |
|---|---|---|---|
| `gcc -c` × 10 under POWERarm | 160–250 cyc (42–66 ns) | 4 383 / s | **0.029%** |
| headless Chrome, JS-heavy page | 160–250 cyc (42–66 ns) | 1.9 × 10⁴ / s | **0.163%** |

Stacking every pessimistic assumption — the highest syscall count and charging
the *entire* syscall round trip to the mitigations — the ceiling is 0.11% for the
compile and 1.5% for the browser. **The exposure is small. This closes the
question; there is nothing here that argues for disabling a mitigation.**

Of the four, the L1D flush dominates, the `eieio` barrier is next, the count
cache flush costs one mispredict, and the link stack flush costs nothing
measurable at all (§4).

## 1. The host's configuration

```
meltdown           Mitigation: RFI Flush, L1D private per thread
l1tf               Mitigation: RFI Flush, L1D private per thread
spec_store_bypass  Mitigation: Kernel entry/exit barrier (eieio)
spectre_v2         Software count cache flush (hardware accelerated), Software link stack flush
spectre_v1         Mitigation: __user pointer sanitization, ori31 speculation barrier enabled
```

## 2. Instrument

`probes/mitprobe.c`, native ppc64le, built with the host toolchain from the
emulated shell (item 55's method):

```
/mnt/arch/usr/bin/gcc -O2 -mcpu=power9 -fno-pie -no-pie -o mitprobe mitprobe.c
```

It opens `perf_event_open` counters on itself (`exclude_kernel=1`, so cycles are
*user-side only* — which is exactly the right lens for "what does the return
cost the code that resumes"), reads `CLOCK_MONOTONIC` for total wall, pins to
one SMT thread, and reports best-of-7 after a discarded warm-up. Raw POWER9
events used: `PM_LD_MISS_L1` (`r3e054`), `pm_br_mpred_ccache` (`r40ac`),
`pm_br_pred_ccache` (`r40a4`), `pm_br_mpred_lstack` (`r48ac`),
`pm_br_pred_lstack` (`r40a8`).

Frequency calibration, from the dependent-`addi` kernel: 8.0004 cyc / 2.1250 ns
= **3.769 GHz**. All cycle figures below are at that clock.

Five kernels, each a loop of "units" with a chosen syscall injected every R units:

| kernel | what it depends on |
|---|---|
| `null` | nothing — 4 dependent `addi`. Gives the syscall's own cost with no state to lose. |
| `probe` | the emitted L1 lookup probe: `ld` L1Pointer, `rlwinm` index, `add`, `ld` key at +8, `cmpd`, not-taken `bne` to the miss leg, `xor` dependency, `ldx` HostCode. Copied instruction for instruction from `BranchOps.cpp`. The loaded HostCode *is* the next guest PC, so the loop is one dependent chain through a 2 MiB, 128 K-entry, 16-byte-per-entry table indexed `((PC >> 2) & 0x1FFFF) * 16`, exactly `LookupCache::L1Slot`. |
| `ccache` | a ring of up to 512 monomorphic `bctr` sites, 64 bytes apart (clear of §4.4's 32-byte branch-density artifact). |
| `lstackdeep` | the syscall taken at the *bottom* of a depth-D `bl`/`blr` chain, so D returns are pending on the link stack across the kernel entry/exit. |
| `eieio` / `eieiost` | an `eieio` against `null`'s identical `addi` chain, quiet and with a pending store. |

Syscall modes: `none`, `vdso` (`clock_gettime` through the guest-less vDSO — a
call of similar user-side cost that *never enters the kernel*, and the control
that matters most here), `getppid`, `madvise(MADV_NORMAL)`,
`madvise(MADV_DONTNEED)`, `mprotect`.

**Validation.** The `ccache` kernel reproduces
`POWER9-VS-A76-PIPELINE.md` §4.4's `B_bctr_next` exactly: **3.05 cyc/`bctr`,
0.00000 `pm_br_mpred_ccache` per `bctr`**. The instrument agrees with the
existing pipeline study where they overlap.

### A correction to item 55's probe figure

Item 55 records the L1 probe as "3.8 cycles with 64 hot blocks rising to 7.3 at
16K, warm". Running the emitted sequence here gives **3.84 ns / 14.43 cyc** at
64 hot blocks and **11.08 ns / 41.63 cyc** at 16 K. The 3.8 matches to three
digits in *nanoseconds*, and item 55's 7.3 lands on this sweep's ~1 K-hot-block
row (6.89 ns). Item 55's figures were almost certainly nanoseconds labelled as
cycles. The probe is a 2-deep dependent load chain — `ld` L1Pointer (3) + `add`
(1) + `ld` key (3) + `xor` (1) + `ldx` (3) is already 11 cycles of latency
before loop overhead — so 14 cycles is the floor, not 4. Nothing in this
document's conclusion depends on it, but the number is quoted elsewhere and
should be read as ns.

## 3. The cost of a syscall return

### 3.1 With nothing to lose (`null` kernel)

Per call, against the no-syscall arm at the same R. "user cyc" is
`exclude_kernel=1` cycles — the part that lands on the resuming user code.

| syscall | ns | total cyc | user cyc | extra L1D misses |
|---|---|---|---|---|
| `clock_gettime` (vDSO, no kernel entry) | 20 | 77 | 76–80 | 0.00 |
| `getppid` | 252–265 | 949–997 | 67–116 | 3.2–4.0 |
| `madvise(MADV_NORMAL)` | 442–454 | 1664–1710 | 74–83 | 3.0–4.1 |
| `madvise(MADV_DONTNEED)` | 515–528 | 1939–1989 | 68–80 | 3.0–4.1 |
| `mprotect(PROT_READ\|PROT_WRITE)` | 546–557 | 2057–2099 | 80–84 | 3.0–4.0 |

**Only ~4 L1D lines of the returning user code are lost.** That is the first
structural result. The RFI flush discards L1D (32 KiB), but L2 (512 KiB per
core) is untouched, so everything the flush threw away is one L2 access away.

### 3.2 With the lookup table to lose (`probe` kernel)

`Δ` is per syscall return, against the same configuration with `sys=none`;
`vdso` is the no-kernel-entry control at the same R, and "net" is their
difference — the part attributable to entering the kernel.

| hot blocks | warm cyc/probe | warm ns/probe | Δcyc_u `getppid` | Δcyc_u `vdso` | net | ΔL1D miss/sys |
|---|---|---|---|---|---|---|
| 64 | 14.43 | 3.83 | 862 | 77 | **~785** | **70.0** |
| 1 024 | 25.99 | 6.91 | 118–150 | 80–84 | ~40–70 | 5.2–7.3 |
| 4 096 | 32.21 | 8.56 | 126–188 | 80–105 | ~45–83 | 5.4–7.2 |
| 16 384 | 41.74 | 11.10 | 117–138 | 81–88 | ~36–50 | 5.8–7.2 |
| 65 536 | 41.75 | 11.10 | 118–143 | 83–90 | ~35–53 | 5.1–6.4 |

(`R` = 8…128 for the `Δ` columns. At `R` ≥ 512 the `vdso` control drifts upward
by the same amount as `getppid`, so that drift is not syscall-related and those
rows say nothing about the mitigations. The `ΔL1D miss/sys` column is flat
across all R and is the number to trust.)

**The tax collapses the moment the hot-block count exceeds what L1D can hold.**
At 64 hot blocks, the 64 touched entries land in 64 distinct 128-byte lines =
8 KiB, which fits the 32 KiB L1D; the flush therefore costs a full re-warm of
70 lines, ~790 cycles. At 1 024 hot blocks the touched lines are already 128 KiB
and at 16 K they are the whole 2 MiB table — far past L1D — so the probes miss
L1D in steady state anyway (which is why warm cost rises 14 → 42 cycles) and the
flush takes away nothing. The extra 5–7 misses per return are the state frame
line, the stack, and the syscall wrapper's own footprint, not the table.

This is the inverse of the hypothesis: **the structure is safe precisely because
it is too big to be in the cache that gets flushed.**

## 4. Separating the four

### 4.1 `meltdown` / `l1tf` — RFI flush, L1D private per thread

The dominant one. Its user-visible cost is the L1D re-warm measured above:
**4 misses with no state to lose, 5–7 with a realistic lookup-table working set,
70 in the toy 64-hot-block case** — roughly 40–70 cycles typical and ~790
cycles worst case, at ~10 cycles per L1→L2 upgrade.

Its kernel-side *execution* cost is not separable with `exclude_kernel=1`.

### 4.2 `spec_store_bypass` — kernel entry/exit barrier (`eieio`)

Priced directly in user mode, against `null`'s identical `addi` chain:

| | cyc/unit | minus baseline |
|---|---|---|
| `null` (4 dependent `addi`) | 8.00 | — |
| `eieio` + the same chain | 59.03 | **51.0 cyc** |
| `std` + 4 `addi` | 8.78 | — |
| `std` + `eieio` + the same chain | 69.37 | **60.6 cyc** |

Two per syscall (entry and exit) → **~100–120 cycles**. Treat it as an upper
bound: back-to-back barriers in a loop cannot overlap, and one in a syscall path
has less to drain.

### 4.3 `spectre_v2` — software count cache flush (hardware accelerated)

**~1 extra count-cache mispredict per syscall return, ≈15 cycles**, and it does
*not* scale with the number of distinct indirect exit sites:

| monomorphic `bctr` sites | warm cyc/`bctr` | warm mpred/`bctr` | Δmpred_ccache `getppid` | Δmpred_ccache `vdso` |
|---|---|---|---|---|
| 16 | 3.42 | 0.00000 | 0.00 | 0.00 |
| 64 | 3.11 | 0.00000 | **1.02** | 0.00 |
| 128 | 3.06 | 0.00000 | **1.03** | 0.03 |
| 192 | 3.04 | 0.00000 | 9.64 | 15.72 |
| 256 | 3.04 | 0.00060 | 21.73 | 23.54 |
| 320 | 11.61 | 0.40060 | −9.29 | −9.97 |
| 512 | 24.64 | 1.00010 | −20.47 | −19.59 |

The `vdso` control is what makes this readable. At 64–128 sites `getppid` adds
one mispredict and `vdso` adds none: that difference *is* the flush, and it is
one mispredict. From 192 sites up, `vdso` shows the same or larger delta as
`getppid` — so that is capacity pressure, not the flush. Above ~300 sites the
ring has saturated the count cache and mispredicts 0.4–1.0 times per `bctr`
*warm*, at which point the flush is irrelevant because there was no prediction
to lose.

A first pass without the `vdso` control read 20 mispredicts per syscall at 256
sites and looked like the dominant term. It is not; it is the ring hitting the
count cache's capacity. Worth recording as the trap.

### 4.4 `spectre_v2` — software link stack flush

**Zero.** The syscall taken at the bottom of a depth-D `bl`/`blr` chain, so D
returns are pending across the kernel entry and exit:

| depth | warm cyc/descent | Δcyc_u `getppid` | Δmpred_lstack | Δpred_lstack |
|---|---|---|---|---|
| 1 | 28.6 | 154.3 | 0.00 | 1.00 |
| 8 | 128.8 | 108.7 | 0.00 | 1.00 |
| 24 | 353.7 | 120.1 | 0.01 | 1.00 |
| 48 | 710.2 | 102.9 | 0.02 | 1.00 |

48 pending returns across a kernel entry/exit cost no mispredicts at all.
`pm_br_pred_lstack` rises by exactly 1.00 — the syscall wrapper's own return —
and `pm_br_mpred_lstack` does not move. Item 55's worry that "syscall-dense
guest phases get no branch prediction on indirect exits at all" is half right:
the count cache loses one entry's worth, and the link stack loses nothing.

### 4.5 Sum, and what cannot be separated

| mitigation | per syscall return |
|---|---|
| RFI flush / L1D (user-side re-warm) | 40–70 cyc typical; ~790 cyc if the hot set fits L1D |
| `eieio` × 2 | ~100–120 cyc (upper bound) |
| count cache flush | ~15 cyc (1 mispredict) |
| link stack flush | 0 |
| **measurable total** | **~160–250 cyc ≈ 42–66 ns** |

Hard ceiling, charging the *entire* `getppid` round trip to the mitigations:
**953 cyc / 253 ns**.

**What I could not separate:** the no-mitigation baseline. That needs
`/sys/kernel/debug/powerpc/{rfi_flush,entry_flush,count_cache_flush,stf_barrier}`
written as root, which is a mitigation change. Not done; debugfs is not readable
from here either. So "how much of the 953 cycles is mitigation and how much is
just a syscall" is **unanswered, deliberately**, and the numbers above are the
parts that could be isolated without touching anything.

## 5. The emulator's syscall rate

Counts from native `strace -c -f` (accurate), wall times from separate untraced
runs (median of 3; `strace` inflates wall by 20–100× under emulation and its own
wall is not comparable).

### `gcc -c` × 10 (Lua 5.4.9, `-O2 -fPIC`), the slice workload

| | value |
|---|---|
| untraced wall, box busy | 19.38 / 17.16 / 16.67 s → median 17.16 s, spread 16% |
| untraced wall, box quiet | 16.755 / 16.759 / 16.765 s → median **16.76 s**, spread 0.06% |
| host syscalls | **73 461** |
| **host syscall rate** | **4 383 / s** |
| same compile native ppc64le | 25 275 syscalls / 6.58 s = 3 841 / s |
| host syscalls per guest syscall | **2.9** |

(The 16% spread is what this box does when the owner's desktop is working; 0.06%
is what it does quiet. Both medians are quoted because the first set is what the
strace run shared the machine with.)

Top of the host mix: 13 420 `openat2`, 13 028 `close`, 12 613 `readlinkat`,
5 161 `mmap`, 4 500 `openat`, 2 982 `newfstatat`, 2 323 `read`, 2 317 `pread64`,
2 206 `rt_sigaction`, 1 612 `access`, 1 039 `madvise`, 781 `mprotect`.
**53% of all host syscalls (`openat2` + `readlinkat` + `close`) are rootfs path
resolution the guest never asked for** — `RESOLVE_IN_ROOT` probing across the
two-tier rootfs.

### Headless Chrome on a local JS-heavy page

`probes/jsbench.html` — tree build/walk, polymorphic dispatch over three shapes,
`Map` and string churn, `Float64Array` math. No network, no GUI:
`--headless=old --dump-dom`, `POWERARM_PORTABLE=1` with an absolute
`POWERARM_ROOTFS` so the 17 child processes stay on this build.

| | value |
|---|---|
| untraced wall | 7.34 / 7.65 / 7.48 s → median **7.48 s** |
| untraced CPU | user 4.92 s + sys 0.89 s = **5.81 CPU-seconds** |
| work host syscalls | **143 133** |
| all host syscalls incl. polling | 351 698 |
| **host syscall rate** | **1.9 × 10⁴ / s**, upper bound 4.7 × 10⁴ / s |

**`strace` cannot measure a timer-driven workload's syscall rate, and this is
the trap to record.** The straced session took 1 892 s against 7.48 s untraced —
253× — and `futex` (190 464), `epoll_pwait` (13 349) and `ppoll` (4 752) fire on
real-time deadlines, so ptrace's slowdown multiplies the polling syscalls by the
same factor it divides the progress. 59% of the straced total is that artifact.
The 143 133 "work" syscalls (the rest) are caused by process startup and page
loads, not timers, and are session-length invariant. Cross-check: 0.887 s of
system CPU over 143 133 work syscalls is 6.2 µs each, against 6.3 µs each for
`gcc` — two very different workloads agreeing to 2%.

Top of the work mix: 17 687 `newfstatat`, 13 373 `faccessat`, 9 017
`readlinkat`, 8 839 `read`, 8 098 `openat`, 6 002 `mmap`, 5 470 `sendto`,
5 097 `madvise`, 4 975 `recvmsg`, 4 346 `write`, 1 358 `clone`, 1 282
`execveat`. Path resolution again leads.

### Kernel CPU time, measured directly (no tracing at all)

The assumption-free version of the same question. `sys` is an upper bound on all
host syscall time, page-fault handling included.

| workload | real | user | sys | **kernel share of CPU** |
|---|---|---|---|---|
| `gcc -c` × 10, POWERarm | 16.76 s (16.755–16.765) | 16.28 s | 0.464 s | **2.8%** |
| same compile, native ppc64le | 6.58 s | 6.49 s | 0.082 s | **1.2%** |
| headless Chrome, full JS page | 7.48 s | 4.92 s | 0.887 s | **15.3%** |
| headless Chrome, blank page (startup only) | 3.39 s | 4.26 s | 0.84 s | **16.5%** |

Even if every cycle the kernel spent were mitigation — which it is not, by two
orders of magnitude — the whole exposure would be 2.8% on `gcc` and 15.3% on
Chrome. The measurable mitigation parts are 42–66 ns of a syscall that averages
6.2–6.3 µs of kernel time, so **~1% of kernel time**, hence ~0.03% and ~0.16% of
those workloads' CPU.

## 6. Multiplying out

Per CPU-second of the workload's own time, on a 3.769 GHz core
(`gcc`: 73 461 syscalls / 16.74 CPU-s = 4 388 per CPU-second;
Chrome: 143 133 / 5.81 CPU-s = 24 640 per CPU-second):

| workload | charged at | cycles per CPU-second | share of CPU |
|---|---|---|---|
| `gcc -c` × 10 | measurable mitigation parts, 250 cyc | 1.10 M | **0.029%** |
| `gcc -c` × 10 | the entire `getppid` round trip, 953 cyc | 4.18 M | 0.111% |
| Chrome, JS page | measurable mitigation parts, 250 cyc | 6.16 M | **0.163%** |
| Chrome, JS page | upper-bound syscall count (60 533/CPU-s) at 250 cyc | 15.1 M | 0.40% |
| Chrome, JS page | upper count × the entire round trip, 953 cyc | 57.7 M | **1.53% (ceiling)** |

So: **0.03% on a compile, 0.16% on a JS-heavy browser session, and 1.5% if every
pessimistic assumption is stacked at once.**

Cross-check against the workload's own counters, so this does not rest on the
microbenchmark alone. `perf stat`, user mode, the whole `gcc` run (6.57
CPU-seconds, 24.35 G `cycles:u`, 32.03 G `instructions:u`, IPC 1.32):

| counter | workload total | syscall-attributable | share |
|---|---|---|---|
| `PM_LD_MISS_L1` | 247.2 M | 73 461 × 4–7 = 0.29–0.51 M | **0.12–0.21%** |
| `pm_br_mpred_ccache` | 15.20 M (of 97.43 M predicted — 15.6% already miss) | 73 461 × 1 = 0.073 M | **0.48%** |
| `pm_br_mpred_lstack` | 6.94 M (of 535.1 M — 1.3%) | 0 | **0%** |

The mitigations' effect on the dispatch structures is a fraction of a percent of
those structures' own miss and mispredict traffic. The emulator is already
missing L1D 247 million times and mispredicting 15 million count-cache branches
on this run; 73 thousand syscall returns do not move either number.

## 7. Against the database framing

When this class of mitigation was being argued about, the workloads that lost
double digits were the ones at 10⁵–10⁶ syscalls per second per core: OLTP with
network and storage round trips, where a ~1 µs per-entry penalty becomes 10–30%
of throughput. At 10⁶ syscalls/s, 953 cycles each is 25% of a core.

POWERarm is at **4.4 × 10³ per CPU-second on a compile and 2.5 × 10⁴ on a
JS-heavy browser session** — one to two orders of magnitude below the database
regime, and the browser figure is mostly Chrome's own IPC and startup rather
than anything the emulator adds.

The premise of the hypothesis was that block transitions make an emulator
syscall-sensitive. They do not: **a block transition never enters the kernel.**
The kernel is entered only on guest syscalls, and emulation barely changes their
*rate*: on the same program, 3 841/s native against 4 383/s emulated, 14%
higher. The emulator's 2.9:1 host-syscall amplification is almost exactly
cancelled by its 2.55× wall-time dilation. Emulation stretches the user-mode work
between syscalls by the same factor that it multiplies the syscalls, so a
per-syscall-return tax lands on POWERarm at very nearly the rate it lands on the
native program — and on neither does it matter.

A database loses data it would partly have lost to I/O anyway. POWERarm does not
lose the thing it was feared to lose, because that thing was never in the cache
that gets flushed.

**Is this shape unusually exposed? No — it is unusually un-exposed.** If the
owner is weighing whether to disable any of these four, this measurement says
POWERarm is not a reason to. The decision should be made on whatever *else* runs
on the box; the emulator stands to gain 0.03% of a core on a compile and 0.16%
on a browser session, with a 1.5% ceiling only if every pessimistic assumption
is stacked.

If one were disabled anyway, the order by payoff-to-risk from these numbers is:
the `eieio` entry/exit barrier (~100–120 cyc, the largest single measured term,
and `spec_store_bypass` is the narrowest of the four vulnerabilities), then the
link stack flush (costs nothing, so disabling it gains nothing — do not bother),
then the count cache flush (~15 cyc), and the RFI/L1D flush last, since it is
both the costliest to give up security-wise and the one whose cost here is
bounded by a 32 KiB cache. **This is a ranking of a rounding error; it is not a
recommendation to change anything.**

## 8. What the emulator could do anyway, mitigations aside

- **Fewer host syscalls per guest syscall.** 2.9:1 today, and 53% of the `gcc`
  run's host syscalls are rootfs path resolution: `openat2(RESOLVE_IN_ROOT)`
  probes across the overlay and base tiers, a `readlinkat`, and a `close` per
  probe. A guest-path → (tier, resolved fd) cache that also memoises `ENOENT`
  would be the single largest cut available. This is worth doing for syscall
  *cost* — it is not a mitigation lever, and it should not be justified as one.
- **A smaller or denser L1 is the wrong direction for this.** Item 55 measured
  1–2 cycles per probe saved in the 4 K–8 K hot-block range, which is a genuine
  steady-state gain. But shrinking the table toward L1D-residency makes the
  mitigation exposure *worse*, not better: the hot-block-64 row — the only row in
  the whole sweep where the flush costs real cycles (70 misses, ~790 cycles) — is
  the row where the working set fits L1D. Re-warming is not an argument for a
  smaller table; if anything it is a mild argument against.
- **Prefetching the probe is not available.** The probe's address is derived from
  the guest PC the previous block just computed, so there is no earlier point at
  which the address exists. Nothing to prefetch from.

## 9. Reproducing

From `docs/powerarm/research/mitigations/`:

```
/mnt/arch/usr/bin/gcc -O2 -mcpu=power9 -fno-pie -no-pie -o probes/mitprobe probes/mitprobe.c

CPU=120 bash probes/sweep.sh  probes/sweep.txt    # probe x hot x R x syscall; null; ccache; lstack
CPU=164 bash probes/sweep2.sh probes/sweep2.txt   # lstackdeep by depth; ccache by site count
python3 probes/analyze.py  probes/sweep.txt
python3 probes/analyze2.py probes/sweep2.txt

# the eieio figures in 4.2
for k in null eieio nullst eieiost; do
  taskset -c 164 probes/mitprobe $k none 20000000 1000000 1 7
done

EMU=<host path to build-powerarm/Bin/POWERarm>          # no /mnt/arch prefix: it is a native binary
EMU=$EMU bash probes/wl-cputime.sh                      # user vs sys CPU, both workloads, untraced
EMU=$EMU bash probes/wl-gcc.sh                          # gcc slice: walls + strace -c -f count
EMU=$EMU LD_LIBRARY_PATH=$HOME/.local/lib/perfshim bash probes/wl-perf.sh
EMU=$EMU bash probes/wl-chrome2.sh js probes/jsbench.html 96-111
```

`probes/sweep.txt` and `probes/sweep2.txt` are the raw runs this document was
written from, and `probes/{gcc,chrome}.strace.txt` the raw syscall summaries.

Traps, all of which cost time here:

- **Pin to an SMT thread nothing else is on** (`120`, `164` were free; `120-123`
  is one core). Keep the box quiet — the `gcc` wall spread is 16% with the
  owner's desktop working and 0.06% without.
- **The `vdso` control column is the whole instrument.** Any delta that `vdso`
  shows as well as `getppid` is drift or capacity, not the kernel entry. The
  first pass of §4.3 read 20 count-cache mispredicts per syscall and was wrong
  for exactly this reason.
- **`perf` needs `LD_LIBRARY_PATH=$HOME/.local/lib/perfshim`** and raw event
  syntax (`r3e054:u`). Tracepoints (`raw_syscalls:sys_enter`) need root and are
  not available, which is why syscall counts come from `strace`.
- **Host paths have no `/mnt/arch` prefix.** The shell is emulated and sees
  `/mnt/arch/...`; `POWERarm` and `POWERARM_ROOTFS` are native and see
  `/home/jbettcher/...`. Passing the emulated form to the emulator gets
  "unusable RootFS: does not exist".
- **`strace` cannot measure a timer-driven workload** (§5, Chrome). It can
  measure `gcc`.
