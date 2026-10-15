# How to measure POWERarm, and the ways it has gone wrong

Every rule here is here because an experiment in this repository produced a wrong
number first. The worked failures are in §5, with the number each one reported
before it was withdrawn; they are kept because the retraction is the useful part.

Tags follow `BENCH-M1-BASELINE.md`: **[MEASURED]** is from a run on this box,
**[SPEC]** is an inference nothing here has isolated.

---

## 0. Who measures

The owner does. An implementation change is justified by what it does to the
emitted code — fewer instructions for the same semantics, a shape that was not
expressible before — and that argument is static and reviewable. A timing claim
needs a quiet box, a pinned configuration and a variance estimate, none of which
a parallel agent has. A bug-fix commit carries no timing numbers at all.

Build first. Do not front-load measurement: an optimisation that is obviously
fewer instructions does not need a benchmark to land behind a switch, and a
measurement taken to decide whether to *start* is the most likely to be wasted.

---

## 1. The acceptance metric is work, not time

The governor on this machine is `ondemand` over 2.3–3.8 GHz, and most cores sit
at the 2.3 GHz floor until something wakes them; idle states go to `stop5` at
200 µs exit latency; SMT is 4 (44 cores, 176 threads). **[MEASURED]** A short
interactive burst therefore spends part of itself on the clock ramp, and a
wall-clock delta under a few percent is as likely to be the ramp as the change.

So the metric is a count of work, not a duration:

- `instructions:u` (and `:u` variants generally — `perf_event_paranoid` is 2, so
  unprivileged user-space counting works without sudo) **[MEASURED]**
- guest blocks compiled (`jit_count`), faults taken, syscalls made, SMC
  invalidations — the ProfileStats counters, via `Scripts/powerarm/shmstats.py`
  or `allstats.py`

`cpupower frequency-set -g performance` is a diagnostic, not a benchmark
configuration: the product has to be fast on a machine whose clock is not
pinned, so a result that only exists at a locked clock is not a result.

**Count children.** `perf stat` without inherited counting measures the launcher
and not the work. Everything below assumes children are included.

---

## 2. Census the binary before choosing the workload

This is the most expensive mistake available, and it has been made here: three
separate instruments were built and aimed at a lane-test fusion using Chrome as
the workload, and Chrome's own `.text` contains **113 `umaxv` + 31 `uminv` = 144
sites** against libxul's 3,284. **[MEASURED]** No instrument can resolve a
change at 144 sites in 56.4 M instructions, and no amount of care in the harness
would have helped.

Before building a harness, disassemble the binary you intend to run and count
the instructions the change touches. `docs/powerarm/research/power-isa/census/`
has the scripts. If the count is small, either pick a different binary or accept
up front that the answer will be "no measurable difference" and spend the time
elsewhere. The same census is what turned the lane-test fusion toward `addv`:
**2,427 `addv` sites in the same Chrome binary**, 17× the arms the fuser
originally accepted. **[MEASURED]**

Also check *which rootfs* holds the binary you are reasoning about — Chrome lives
in the vk-overlay rootfs, not the base one.

---

## 3. Pin the configuration, and know what flipping it costs

### Cache identity

Most codegen-affecting options are hashed into the code-cache config id
(`CodeCache.cpp ComputeCodeCacheConfigId`). **Flipping one hands you a cold
cache.** A baseline sitting in a settled namespace against a switched side still
filling is not an A/B test, it is a warm run against a cold one, and it will
report the warm side as faster by whatever the compile cost is.

Two ways out, both acceptable:

- **Warm each side separately** and discard the warming run. State in the result
  that you did.
- **`EnableCodeCachingWIP=0`.** Runs become uniform, but counts roughly
  quadruple (≈20 G → ≈79 G `instructions:u` on the `gcc -O2 -c lvm.c`
  workload **[MEASURED]**) because every block is recompiled every run, which
  dilutes an execution-side difference into compile noise. Good for ruling out
  cache effects, bad for sizing a small win.

There is **no cache-directory option** to point a run at a scratch cache — only
`CodeCacheHotLocation`. Do not look for one.

### Variance first, delta second

Establish the within-configuration spread before quoting any delta, by running
the *same* configuration at least twice. If the spread is 4 % and the delta is
3 %, there is no result. The three JS failures in §5 all look different in
hindsight once the within-side spread is in the table — and in two of the three,
the tell was already in a table that had been written and then read past.

A worked example of a spread small enough to use: warm `gcc -O2 -c lvm.c`,
`instructions:u`, children included, `POWERARM_PORTABLE=1`, two runs per
configuration, **0.01–0.06 % spread** — which is what made a −0.26 % lowering
and a −1.63 % policy legible, and let their combination be identified as
superadditive (−2.00 %) rather than rounded together. **[MEASURED]**

---

## 4. Workload hygiene

- **Fixed work, not a time budget.** A benchmark that runs for N seconds and
  reports a score measures the score's own noise. A page that performs a fixed
  number of iterations and prints an accumulator can be compared by instruction
  count.
- **The workload must end its own process.** A harness that polls and then kills
  the process tree charges the counter for everything up to the kill. An idle
  headless Chrome burns **~2.5 G instructions/second** **[MEASURED]**, so a
  5-second poll interval is ±12 G of counted-but-unwanted work. Have the page
  call `window.close()` and have the harness wait for the self-exit.
- **Read `perf`'s enabled-time field.** It is the direct check on the point
  above: identical work whose enabled time ranges 12.2–21.2 s is not identical
  work. **[MEASURED]** Check it on every run before reading the counts.
- **One pid is the wrong instrument for a browser.** Chrome puts V8 in a
  renderer child, each child is its own emulator process with its own
  `/dev/shm/powerarm-<pid>-stats`, and the launcher does almost no guest work.
  Use `Scripts/powerarm/allstats.py`, which sums every segment that appeared
  after it started.
- **Keep a flat control in the matrix.** NavierStokes held to **0.4 %** across
  configurations while the other Octane subtests swung 3.8–30 % **[MEASURED]** —
  which is what proved the harness steady and the variance intrinsic to the
  suites. A flat control is the difference between "the instrument is broken"
  and "this workload cannot answer this question", and the two call for opposite
  responses.

---

## 5. Three failures, with what each one taught

**Octane subset (regexp, earley-boyer, typescript, pdfjs; navier-stokes as
control).** Fusion on 9752/9802, off 9893/9520. Inconclusive: within-configuration
swing 3.8 %, PdfJS 30 %. The control was flat to 0.4 %, so the harness was fine
and the subtests simply cannot resolve a few percent. *Lesson: §3's variance-first
rule, and that a flat control tells you which of two very different problems you
have.*

**Fixed-work page.** Reported **17 %** for the lane-test fusion, then withdrew
it. `perf`'s enabled time varied 12.2–21.2 s across identical work, because the
counter ran until the harness's 5-second poll killed the tree. *Lesson: §4's
self-exit and enabled-time rules.*

**`NZCVExitDead` on JS.** Reported **+12.6 %**, then withdrew that too. The
switch is hashed into the cache identity, so flipping it handed the switched side
a cold cache while the baseline sat in a settled namespace — and the tell was in
the published table, baseline spread 2.1 % against the switched side's 9 %.
Re-measured with per-side warmed caches and one discarded warming run: **+2.5–3.3 %**
against within-side spreads of 3.9/4.6 %, i.e. residual, not a regression.
**[MEASURED]**

That re-measurement also killed the mechanism that had been proposed for it:
`jit_count` went *down* 1.2 % across 16,581 dead verdicts taken, and **the SMC
counter cannot move in the default `icache` mode at all** — both increment sites
are inside the `mtrack` write-fault handler, nothing is write-protected, so the
counter is pinned at zero by construction. **[MEASURED, from the source]**
Residual ≈3 % is most consistent with the peek's own compile cost: 76 K constant
exits scanned per run, up to 96 guest words each. **[SPEC]**

*Lesson: when a counter does not move, check whether it is capable of moving in
the configuration you ran before building a story on it.*

---

## 6. Correctness evidence is not performance evidence, and has its own rules

- **No new goldens can be minted.** The Pi 5 is gone. The goldens on disk stay
  the reference; a new test must be self-checking and carry `// NO-GOLDEN`.
- **Never create a golden from POWERarm's own output.** It compares equal to a
  failing run, and that has shipped in this repository before.
- **A golden cannot close a hole it does not contain.** `simd_half` has 2,536
  cases goldened against real hardware, and the FP16 narrowing helper it covered
  was wrong for 16,777,214 inputs — every f32 strictly between 2⁻²⁵ and 2⁻²⁴,
  which the 2,536 cases happen not to include. **[MEASURED]**
- **Prefer exhaustive comparison against the host instruction the other arm
  emits.** Where a lowering has an ISA 3.0 arm and a 2.07 arm, the 3.0 arm's
  hardware instruction is a reference this box can run over the entire input
  domain: all 2¹⁶ halves and all 2³² singles in four rounding modes, which is
  what found the bug above and what certifies its replacement. That is stronger
  than any golden and it needs no second machine.
- **Run the mode the code is in.** A green default run proves nothing about a
  2.07-only path; `POWERARM_HOSTFEATURES=disableisa30` is the mode that executes
  it. The same applies to `VSXCLASSES`, `MAXINST=1` and the switch states of
  whatever is being added.
- **A hang is a failure, so bound every test that can hang.** Both teardown-race
  tests could stop the suite indefinitely rather than fail it, and did.
- **The memory model is evidence.** `litmus.c` cannot validate a release-side
  change, because POWER9 never exhibits store→store reordering; `herd7` over
  `unittests/MemoryModel` is what adjudicates ordering questions, and it is how
  the ctrl+`isync` acquire load was rejected.

---

## 7. What dominates, so you know what is worth attacking

Ranked on this host, cold run **[MEASURED]** except where marked:

1. **Emulation overhead itself** — ≈2.5 host instructions per guest instruction,
   and **82.6 % of user cycles in POWERarm's own code** against 9.2 % in
   translated guest code. This is why a build-flag fix outweighed a season of
   lowerings: `TUNE_CPU` had been silently ineffective, and the shipped binary
   contained **0 ISA 3.0 instructions and 750 `xxswapd`** out of 2,347 VSX
   instructions; fixing it reached **8,344** ISA 3.0 instructions with
   `xxswapd` down to 85.
2. **Clock ramp on interactive bursts** — see §1.
3. **SMT contention** — real but below the first two. **[SPEC]**

The ordering is the argument for spending effort on POWERarm's own code
(register allocation, block shape, the cost of the dispatcher) ahead of
individual guest-instruction lowerings, and for treating a lowering's value as
"it made a shape expressible" rather than "it moved a benchmark".
