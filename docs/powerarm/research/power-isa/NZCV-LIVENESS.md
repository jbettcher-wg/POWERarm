# NZCV at compile-unit exits: who can see the flags, what can be proven, and what it is worth

Written 2026-09-29 for `ISA-OPPORTUNITIES.md` §3.0 (ranked item 1 of 17), against the tree at
`53b600451` and the promoted stable emulator `cffa7b4de`. Everything here comes from four
sources, and each number says which: (1) source reading (`RedundantFlagCalculationElimination.cpp`,
`CompareBranchFusion.cpp`, `JIT/PPC64LE/JIT.cpp` and `BranchOps.cpp`, `PPC64Emitter.cpp`, the A64
`Decoder.cpp`, `SignalDelegator.cpp`, `GuestFramesManagement.cpp`, `CodeCache.cpp`,
`LookupCache.h`); (2) probe programs built static on the Pi 5 (Cortex-A76, kernel
`6.18.39+rpt-rpi-2712`, glibc 2.41) and run there and under the stable emulator on the POWER9,
sources in `nzcv-probes/`, raw output in `nzcv-probes/RESULTS.txt`; (3) an IR census of
`cc1 -O2 lvm.c` taken from the stable binary's own post-optimisation IR dump
(`POWERARM_DUMPIR=stderr`, code cache off), 110,706 units, classified by
`nzcv-probes/nzcv_census.py` against the host `llvm-objdump` disassembly of the rootfs `cc1`;
(4) the history in `git log` (`5558f8ae8`, `41e867c9a`, `eaf03c95d`, `09ed0e84d`, `2f8013325`).
No build. No source file changed.

**One-paragraph answer.** The item is worth doing, but not in the shape §3.0 proposes. Link time
is the wrong time and the successor's IR is the wrong witness: only 27% of `cc1`'s units write
all four flags in their first block, 73% end that block before touching a flag, so a
successor-IR check refuses (or, in §3.0's protocol, recompiles) most of the time and proves
almost nothing; and the existing `EntryNZCVLiveIn` bit is not merely toothless, it is unsound as
a guard (first block only, §4.2). What can be proven is a property of *guest code*, available
when the predecessor is compiled: scan forward from the constant exit target until the first
instruction that reads or fully writes NZCV. On `cc1` that scan, bounded at 16 guest
instructions and following one level of `BL`, marks the flags dead at 56,743 of the 102,222
compares that today survive only because a leg reaches a constant exit (55%; 44% of all
128,968 kept compares), never contradicts a compiled successor that actually reads flags
(40,609 cross-checked), and needs no link-time protocol, no recompile cycle and no cache-image
coordination: its inputs are guest bytes, so the code cache re-runs it at load and SMC tracking
covers it by adding the scanned pages to the unit's `CodePages`. The observers that the IR
cannot see are the kernel's signal frame and ptrace, and POWERarm *already* shows them stale
flags at every drain point and after every dropped compare (§3, probes 1 and 3, measured
against the A76); the policy widens the set of points where that happens, it does not create a
new kind of divergence. Every other boundary (SVC, BL/RET, sigreturn, a synchronous fault, the
dispatcher) carries NZCV exactly on the A76 and under POWERarm today, and the design keeps them
exact. Expected value on `cc1`: 2-4% of executed host instructions for the simple scan, 3-6% with
`BL` following, plus the multiplier on items 2, 4 and 5; the executed weighting of the sites is
the one number this document could not measure (§10). The risky part is the classification table
and the SMC/cache coverage, and §9 puts a canary mode in front of both so that a wrong
assumption flips branches loudly in the suites instead of silently in Firefox.

## 0. What this document had to decide

1. Who can observe NZCV that the unit's IR does not show (§3), with three columns per boundary:
   what the architecture and the kernel ABI define, what the A76 does, what POWERarm does.
2. What can be proven at compile time versus link time, given block linking, the L1 probe, the
   dispatcher and the persistent code cache (§4, §6).
3. Where the 54% actually comes from (§5), so the design fixes the right third of it.
4. A staged plan with a gate per stage that has teeth (§9).

## 1. Method and its limits

- **Reading.** Line numbers are from `53b600451`. The flag contract is `IRBuilder.h:17-20`
  (N,Z in CR0.LT/EQ, C,V in XER.CA/OV); the exit seed is
  `RedundantFlagCalculationElimination.cpp:635` (`FlagsRead = FLAG_ALL`) replaced only for
  `CondJump`/`Jump` at `:656`; fusion is `CompareBranchFusion.cpp:345` (`CompareFusion::Run`),
  block-local, closed with the block's `NZCVLiveOut`; the gate is `JIT.cpp:1905-1909`; the exit
  lowering is `BranchOps.cpp:388` with link-first at `:585-592`; the spill/fill of NZCV is
  `PPC64Emitter.cpp:84-116` and `:198-220`; the signal reconstruction is
  `SignalDelegator.cpp:1069-1070` through `MContext_ppc64le.h:306-325`; frame build and restore
  are `GuestFramesManagement.cpp:29-51,119` and `:200-225`; the unit shape is `Decoder.cpp:211`
  (128-byte region window, 8 leaders, `DecodeInstructionsAtEntry`).
- **Probes.** Three programs, built `gcc -static -O2` on the Pi (so the same glibc code runs on
  both sides), run natively and under `~/.local/opt/powerarm-stable/Bin/POWERarm`, with
  compare fusion on (default) and off (`POWERARM_DISABLECMPBRANCHFUSION=1`). They observe
  `uc_mcontext.pstate` in signal handlers, `MRS NZCV` after boundaries, and control flow that
  depends on flags produced in another unit. They are observation, not a test suite.
- **Census.** The IR dump is post-optimisation, so a compare that fusion dropped is absent and a
  compare that survived is present with whatever readers it still has. Two dumps: fusion on and
  off. For every surviving producer (`SubNZCV`, `AddNZCV`, `SubWithFlags`, `AddWithFlags`,
  `TestNZ`, `AndWithFlags`; `CondSubNZCV`/`CondAddNZCV` and `FCmp` counted apart) the script
  walks forward through the unit's CFG until every bit is overwritten and records what the flags
  reach: readers by kind, and exits by kind (constant with hint None/Call, indirect Return/Call/
  None, Syscall, Break). For each constant exit reached, it scans the target's guest code from
  the disassembly, exactly as the proposed peek would (§7), and records the verdict, the depth at
  which it was reached, and how far the scan strayed from the unit.
- **Limits.** Everything is per translated site, weighted once; nothing is weighted by execution
  (§10). The census is `cc1` only. The classification tables in the script are mine, checked
  against the frontend's `ClassifyConst` table and against the compiled units (no contradiction
  in 40,609 targets, §5.4), not against every A64 encoding.

## 2. Where the flags live and where they cross

The mechanics decide which observers exist, so they come first.

- **Inside a unit** NZCV is CR0/XER between producer and consumer; every non-flag op is
  forbidden the record and OE/CA forms (`ISA-OPPORTUNITIES.md` §2). A fused consumer compares
  SSA values into cr7 and leaves CR0/XER alone.
- **At a constant exit** (`ExitFunction` with an `InlineEntrypointOffset`, hint None or Call),
  the linked form is `[ResetStack]; [li r0,0]; b HostCode` (`BranchOps.cpp:388-600`): CR0/XER
  flow into the target untouched. The target is entered *after* its prologue's
  `FillStaticRegs`, so nothing reloads them. The unlinked link-first form goes to
  `ExitFunctionLinkWithRecord` (`JIT.cpp:1771`) through the miss-leg spill: `SpillStaticRegs`
  packs CR0+XER into `State.nzcv` (`PPC64Emitter.cpp:84-116`), the dispatcher enters the target
  through `FillStaticRegs`, which unpacks it (`:198-220`). The thunk link (`bcl 20,31,$+4`,
  load `HostCode`, `mtctr`, `bctr`) touches neither CR0 nor XER. **Every arrival delivers the
  predecessor's exit-time CR0/XER to the target's first guest instruction**, whether the link is
  direct, thunked, an L1 probe hit (`POWERARM_NOLINKFIRST=1`, and call exits under
  `POWERARM_SMCLAZYLINK`), a dispatcher miss, or a unit loaded from the code cache (stored
  unlinked, relinked at run time through the same linker). This is the fact §3.0's "catch it at
  link time" has to survive, and §6 says why it does not.
- **At an indirect exit** (`BR`, `BLR`, `RET`) the same holds, target unknown at compile time.
  A paired `RET` (`EmitA64PairedCall`) returns through a link-first constant exit to the
  caller's continuation.
- **At a syscall** `DEF_OP(Syscall)` spills (packing NZCV), calls C++, refills. Thunk crossings
  do the same.
- **At an asynchronous signal** the host signal is deferred and the interrupt fault page is
  armed; the unit takes the `stb` poke at its next drain point, which sits at every unit entry
  and on the backward leg of every branch (`JIT.cpp:3025-3083`, `BranchOps.cpp:1372-1424`). The
  poke faults inside JIT code, so the guest frame is built from the *host* ucontext:
  `GetArmPState` packs CR0.LT, CR0.EQ, XER.CA, XER.OV into the PSTATE layout
  (`MContext_ppc64le.h:306-325`) and `SetupFrame_Arm64` writes it to `uc_mcontext.pstate`
  (`GuestFramesManagement.cpp:119`). A back-edge drain reports the branch target, not the
  branch (G2's second signal fix).
- **At a synchronous fault** the same reconstruction runs at the faulting instruction.
- **On `rt_sigreturn`** (`RestoreFrame_Arm64`, `:200-225`): if the handler changed nothing the
  host context resumes in place and CR0/XER are whatever they were. If it changed registers or
  the PC, `LoadFrame` copies `pstate & 0xF0000000` into `State.nzcv` and the thread resumes
  through the dispatcher at the frame's PC, which unpacks it into CR0/XER before entering a
  translation that starts at that instruction.
- **In the code cache** a unit is validated by `GuestHash` over `[entry, entry+GuestLength)`
  (`CodeCache.cpp:954-956`) and by its config id; nothing outside that range is hashed.
- **For SMC** a unit is found by its `CodePages` set and its `[ExtentStart, ExtentLength)`
  (`LookupCache.h:128,152,284`); `IC IVAU` in the default `icache` mode consults a has-code
  bitmap and invalidates the line (`Core.cpp:1841-1850`).

## 3. Observers of NZCV, in three columns

Column 1 is what is *required*: the Arm ARM for the instruction set, the arm64 Linux kernel for
the signal and syscall ABI (the architecture does not define `uc_mcontext`). Column 2 is one
implementation, the A76 under Debian's 6.18 kernel, which is what every golden in this project
is built against. Column 3 is the stable emulator. Where 2 and 3 disagree the row says which
kind of disagreement it is. Probe numbers refer to `nzcv-probes/RESULTS.txt`.

| Boundary | 1. Defined | 2. A76 observed | 3. POWERarm observed | Verdict |
|---|---|---|---|---|
| Next guest instruction after a taken/not-taken branch, in another unit | Flags are architectural state; the next instruction sees the last writer's value. | Exact (entry probes: `bl`→`cset eq`, far `b.cond`→`b.ge`/`cset`/`ccmp`/`adc`, all match). | Exact today, because the exit seed keeps the producer (`FLAG_ALL`). Same results on every entry probe. | Agree. The policy must keep this exact: it may only drop a producer when the target provably writes before it reads. |
| `SVC` (syscall) | Exception entry saves PSTATE to SPSR, `ERET` restores it; Linux preserves `pt_regs.pstate` across a syscall (kernel, not architecture). | `msr nzcv, 0xB; svc getpid; mrs` → B; 0x5 → 5. Continuation-as-new-unit variant identical. | Identical (spill packs, refill unpacks). | Agree. Keep `FLAG_ALL` at `Syscall`, as §3.0 proposes. |
| `BL` / `RET` (flags across a call that does not touch them) | Architecture preserves them; AAPCS64 says "The N, Z, C and V flags are undefined on entry to and return from a public interface" (Machine Registers, fetched 2026-09-29). | `msr; bl leaf; mrs` exact; `cmp; bl leaf; cset eq` exact; `bl (cmp; ret); cset lo` exact. | Identical. | Agree. The ABI permits assuming them dead at `RET`; the hardware does not enforce it, and hand-written code may rely on the hardware. §7.4 keeps this behind a separate switch, default off. |
| Signal handler reading `uc_mcontext.pstate` after an **asynchronous** signal | Kernel ABI: `pstate` is the interrupted context's PSTATE, exact at any instruction boundary. | Exact per instruction: samples at +0/+8/+16 of `loop_dropped` read `--C-`/`-ZC-`/`----`, the values of the last executed compare (probe 1). | **Stale.** All 655 samples land at the back-edge drain point (+0) and read `----`: the value of the last *kept* producer (`cmp x0,x2` at +8, kept because its taken leg exits), not the `subs` result `--C-` that hardware shows. Fusion off: 583 of 584 samples at +0, all `--C-`, exact. | **POWERarm diverges from the kernel ABI today** whenever a compare has been dropped or the drain point follows a dropped compare. This is G2's documented trade ("NZCV in a signal frame taken after the last reader is not exact"), now measured. The policy widens where it happens (§7.6). |
| Signal handler reading `pstate` after a **synchronous** fault | Kernel ABI: exact at the faulting instruction. | Exact: 1993×`--C-` + 1×`-ZC-` across 1994 faults, result 7 (probe 1 `sync`); `-ZC-` ×1000 in probes 2 and 3. | Exact when the producer was kept: probe 2 `kept_fault` and `dropped_fault` match hardware bit for bit (the latter's compare is kept by its exit leg — the 54% shape). **Stale when the compare was dropped:** probe 3 (`cmp x0,x0; b.ne` both legs in-unit, then a faulting load) reads the sentinel the unit *inherited at entry* (`N-C-`, `-Z-V`, `----` for sentinels A, 5, 0) where hardware reads `-ZC-`. Fusion off: exact. | Same divergence class as the row above. The `MayRaiseSignal` rule (`CompareBranchFusion.cpp:229`) keeps the compare only when a fused *reader* follows the fault, which is what correctness needs; the frame's value at a point where nothing reads it is already not defended. |
| Handler **edits** `pstate` NZCV and returns | Kernel restores `pstate` (`restore_sigframe`), the resumed instruction reads the edited flags. | `loop_edit`: the handler clears Z on its third sample; the loop's `b.ne` takes it at once, 99,285,233 iterations remaining. | Honoured at once: 99,336,504 remaining (fusion off 99,365,089). Path: `FrameEdited` → `LoadFrame` → dispatcher resume at the frame PC → `FillStaticRegs`. | Agree. Any policy keeps this: the resumed unit starts at the frame PC and reads what was unpacked. |
| Handler resumes at a **different PC** with edited flags (Go's preemption trampoline, `sigpreempt.c`) | As above. | Exact. | Exact; covered by `sigpreempt` in the suite. The trampoline saves NZCV with `MRS` and restores with `MSR`, so a stale frame value round-trips to a point where nothing reads it. | Agree. |
| Signal delivered **and returned without edits** during a syscall (`raise`, `kill`) | Preserved. | `msr 0xB; svc kill(self, USR1); mrs` → B; 0x5 → 5 (probe 2). | Identical (delivered in the syscall's spill window from `State.nzcv`). | Agree. |
| `swapcontext`, `siglongjmp`, `raise()`, `nanosleep()` from C | glibc saves no NZCV (not in `ucontext`'s callee-saved set; `jmp_buf` has none). | Flags read after each are the last compare *glibc's own code* executed (6, 2, 6, 6, 0). | Bit-identical to the A76 in every case. | Agree, and informative: no libc boundary carries NZCV to user code, so user-level context switching is not an observer. |
| ptrace (`PTRACE_GETREGSET`), gdbserver | Kernel: exact at a stop. | Exact. | `GdbServer.cpp:306` reports `State.nzcv`, marked TODO for reconstruction inside JIT code. Not probed. | Fidelity only; no guest program's correctness depends on it. |
| Fault-page poke at a **unit entry** reached by a link | n/a (emulator internal). | n/a | The poke runs on linked arrivals too (`JIT.cpp:2222`); the frame packs the predecessor's exit-time CR0/XER. | Under the policy this is where a "dead at exit" assumption becomes visible in a frame: the value is stale iff the compare was dropped, and the target provably does not read it. |
| Code cache reload of a unit compiled elsewhere | n/a | n/a | The code is byte-identical; its assumptions are whatever it was compiled with. | The assumption must be a pure function of the guest bytes so the loader can re-derive it (§7.5). |

Two things this table settles. First, the "wall" the brief feared is real but already behind us:
a handler *can* observe NZCV at any interruptible boundary, and POWERarm has been showing it
stale flags at drain points and after dropped compares since G2 landed; the probes make the
divergence concrete (probe 3 shows an arbitrary inherited value where hardware shows `-ZC-`).
The choice §3.0's point 3 makes ("frames taken at a unit entry may carry the flags of the last
kept producer") is the same trade at more points, not a new kind of trade. Second, the boundaries
that *do* carry flags to guest code (successor instruction, SVC, BL/RET, sigreturn, fault) are
exact on both machines today, and no design may weaken them; the peek is built so that it never
does (§7).

What would make the frame divergence matter: a handler that reads `pstate` NZCV to *decide*
something at a point where the interrupted code itself does not read it. The known readers of
`pstate` in the reference set's runtimes are crash reporters (Breakpad/Crashpad minidumps, Go's
panic traces), which print it, and Go's asynchronous preemption, which round-trips it (row 7).
None of the probes, suites or reference apps has found a decision made on it; §10 lists this as
unprovable rather than proven.

## 4. History: why G2 delivered 3% and why `EntryNZCVLiveIn` has no teeth

### 4.1 G2's arithmetic reproduces exactly

The fusion-off dump has 141,707 surviving producers, the fusion-on dump 128,968: fusion dropped
12,739, **9.0%**, which is the 9% `5558f8ae8` reported. Of the survivors, 102,222 (79.3%) are
kept *only* because their flags reach a constant exit (§5.1). G2's "54% have a leg that leaves
the unit" and "37% have an in-unit leg that reaches an exit without a full writer" are the same
population split by whether the exit is the branch's own leg or further down; for the design
they are one class. The 6-10% estimate assumed all compares could go; 9% could; 3% came.

### 4.2 `EntryNZCVLiveIn` (2f8013325) is three things, none of them a safety net

1. **A conservative default nothing refines.** DFCE seeds `FLAG_ALL` at every exit
   (`:635`), so no unit ever assumes dead flags at an exit and the gate at `JIT.cpp:1905` has
   nothing to protect. It costs a thunk link instead of a direct one for targets that read at
   entry: 286 of `cc1`'s 110,706 units (0.26%). Negligible, but pure cost.
2. **Not a link-time mechanism even if it were needed.** The refused direct link falls back to
   the thunk link, which delivers the same CR0/XER (§2). If a predecessor ever did assume dead
   flags, refusing the direct link would change nothing observable. A mechanism with teeth would
   have to refuse *all* arrivals or recompile the predecessor, which is §3.0's point 2 and §6's
   subject.
3. **Unsound as a guard.** It is computed from block 0 only, as reads before writes
   (`RedundantFlagCalculationElimination.cpp:856-871`). A unit whose entry block has no flag op
   and ends in `cbz`, with `b.ne` in its second block, reads NZCV from entry and is marked
   false. The census finds 303 `cc1` units with a reader reachable from entry by the CFG and
   286 by the block-0 rule: 17 units are misclassified today. Harmless while nothing relies on
   the bit; a latent miscompile the moment something does. The fix is one forward reach from
   the entry block (the census's rule), and stage 1 does it (§9).

### 4.3 What the Firefox miscompile teaches

`41e867c9a`/`eaf03c95d`: fusion rewrote a CSEL chain with operands copied at discovery time;
the second compare read the first CSEL's SSA value, which the rewrite had replaced. The bug was
in the rewrite, not in liveness, and the 1954-case `cmpbranch` sweep missed it because it did
not chain compares through a context-backed register. Two lessons carry over: the sweep must be
generated from the *shapes the policy creates* (a successor that reads at entry, at every
distance and through every arrival path), and there has to be a mode in which a wrong assumption
fails deterministically rather than by luck (§9's canary), because a stale flag value is usually
the right value.

## 5. Where the 54% comes from: the `cc1` census

`cc1 -O2 lvm.c`, stable binary, cache off, fusion on unless stated. 110,706 units, 1,582,860
translated guest instructions (14.3 per unit), 287,667 `ExitFunction`s (187,645 None, 79,967
Call, 20,055 Return), 227,861 `CondJump`s. 98.2% of surviving producers are in `cc1`'s own text;
the rest are glibc and ld.so.

### 5.1 Why each surviving producer is kept (128,968)

| Reason the flags are live | Count | Share |
|---|---|---|
| Reach a **constant exit only** (None and/or Call) | 102,222 | 79.3% |
| Reach a constant exit and a **Return** | 9,409 | 7.3% |
| In-unit **CCMP/CCMN** reader (`CondSubNZCV`, unfusable today: item 2) | 5,963 | 4.6% |
| Reach a **Return** only | 5,291 | 4.1% |
| In-unit readers, all fusable, but in **another block** (fusion is block-local) | 2,525 | 2.0% |
| Reach a constant exit and an indirect Call | 1,650 | 1.3% |
| Reach an indirect Call only | 729 | 0.6% |
| No visible reader (readers fused; kept by the signal-barrier rule or a partial overwrite) | 332 | 0.3% |
| `CondAddNZCV` reader, `Break`, indirect None, ADC/SBC, combinations | 847 | 0.7% |

The exit hint sets inside the first row: None only 71,445; Call only 11,644; both 19,133. So
**86.6% of kept compares are kept by a constant exit** and 11.4% involve a `RET`; the in-unit
remainder belongs to items 2 (CCMP) and to a cross-block fusion extension (2%), not to this item.

### 5.2 What the peek can prove about those constant targets

For the 102,222 constant-exit-only producers, every reached target was scanned from the
disassembly with three rule sets (§7.2 defines them):

| Scan | DEAD (droppable) | UNRESOLVED (keep) | LIVE (keep) |
|---|---|---|---|
| simple: straight line, follow `b`/`cbz`/`tbz` legs, stop at `bl`/`ret`/`br`/`svc`-trap | 37,509 (36.7%) | 64,118 | 587 |
| follow-`bl`: scan the callee, continue after it if the callee returns without writing | 56,743 (55.5%) | 44,885 | 586 |
| follow-`bl` + AAPCS `ret` assumption (§7.4) | 65,562 (64.1%) | 36,066 | 586 |

Unresolved reasons, simple scan: a `bl` before any flag op 31,188 (plus 7k in combinations),
target outside the disassembly (a glibc address, i.e. calls into libc) 10,899, `ret` before any
flag op 7,601, `blr` 2,614, `br` 1,474, `brk` 543, scan budget 125. With `bl` following: libc
targets 13,269 (+ 2,982 through a call), `ret` 9,079, PLT stubs (`bl` then `br x17`) 4,398,
`blr` 2,988, `br` 1,773, three-deep calls 1,844, `brk` 619. Two thirds of what the simple scan
cannot resolve is a call, and two thirds of what `bl`-following cannot resolve is code the
census did not disassemble (glibc) or a return. Disassembling glibc would move a share of the
"no-code" rows to DEAD; the implementation reads real memory and does not have that limit.

Depth at which DEAD was decided (follow-`bl`, 56,743): ≤8 instructions 41,023 (72%), ≤16 51,482
(91%), ≤32 56,288 (99%). Simple: 29,195 / 34,809 / 37,248 of 37,509. **A 16-instruction bound
captures 91-93% of the value; 32 captures 99%.**

How far the scan strays from the unit (matters for SMC and the cache, §7.5), follow-`bl` DEAD:
within 4 KiB 26,215 (46%), 64 KiB 6,809, 1 MiB 4,182, 16 MiB 14,917, beyond 4,620 (libc). Simple
DEAD: within 4 KiB 25,378 (68%), 64 KiB 5,025, 1 MiB 1,771, 16 MiB 4,496, beyond 839. The far
spans are followed calls; the branch targets themselves are almost all within 4 KiB.

### 5.3 What the successor's IR can prove, for comparison

Over all 110,706 units, the first block's first flag op is: a full writer 29,434 (26.6%), a
reader 286 (0.26%), nothing before the block ends 80,986 (73.2%). DFCE's own rule over the whole
unit with exits seeded `FLAG_ALL` says NZCV is live-in for 75,683 units (68%). So a link-time
check against the successor's IR, as §3.0 proposes, would find the successor "reads flags" or
"might" for roughly 70% of targets, and the recompile-with-conservative-seed protocol would
fire for most of the 79% it set out to recover. The 128-byte region window that makes units
cheap to translate is exactly what makes their IR useless as a liveness witness.

### 5.4 Soundness cross-check of the scan

For every constant target that was itself compiled (40,609 distinct targets), the scan's verdict
was compared with the target unit's IR. Peek DEAD while the unit reads NZCV from entry (reader
reachable by the CFG): **0 cases**. Peek DEAD with the unit's first block also DEAD: 14,578;
peek DEAD where the unit's own IR could not decide (first block ends first): 6,183; peek LIVE
where the unit reads: 271 (+5 with the reader in a later block); peek UNRESOLVED where the unit
is in fact DEAD in its first block: 239 (the scan stopped at a `bl` or `ret` that the unit's own
shape did not reach; conservative, not wrong). The classification table used is in
`nzcv_census.py` (`READERS`, `WRITERS`, `PARTIAL`, `TERM_UNKNOWN`).

## 6. Why link time is the wrong time

§3.0's mechanism: seed dead at constant exits, record `ExitsAssumeNZCVDead`, and at link time,
if the target has `EntryNZCVLiveIn`, invalidate and recompile the source with the conservative
seed. Against the mechanics of §2 and the numbers of §5:

1. **A link is not the only arrival.** Link-first exits (P12) route the *first* traversal
   through the linker, but call exits under `POWERARM_SMCLAZYLINK` and every exit under
   `POWERARM_NOLINKFIRST=1` probe L1 and jump straight in; the dispatcher's miss path enters
   through `FillStaticRegs` with the packed stale value; a shadow-RET arrival is a constant exit
   from the callee, which never sees the caller's continuation. The gate would have to be
   replicated on every path, and each replica is a place to forget.
2. **The witness is the successor's compile, not the successor's code.** `EntryNZCVLiveIn` is a
   property of one unit shape at one region window; the same target compiled as an interior
   leader of another unit has no bit at all. And it is unsound as computed (§4.2). Repaired, it
   still says "live or unknown" for 70% of targets (§5.3).
3. **The recompile cycle is the most delicate path in the emulator.** Invalidating a unit that
   other threads may be executing, under the code-buffer and invalidation locks, from inside the
   linker, then relinking: the SMC machinery does it, and the HANDOVER's open items on cache
   compaction in forked children and the thread bring-up race are what that class of code costs.
   Adding a second reason to run it, on every link into a flag-reading target, is not a 3-5 day
   change.
4. **The cache image.** A source unit's assumption would depend on a successor that may not
   exist in the loading process. The only assumptions the cache can carry are ones the loader
   can re-derive from guest bytes.
5. **It would not deliver much.** Even with all of the above built, the successor-IR witness
   proves deadness for 27% of targets (§5.3) against 55-64% for a guest-code scan (§5.2).

The conclusion is not "the item is not worth its risk". It is that the item's value lives in a
compile-time scan of guest code, which has a smaller trusted base than a link-time protocol and
more coverage. §7 is that design.

## 7. Design: the compile-time peek at constant exit targets

### 7.1 Placement

The A64 frontend knows every constant exit target when it emits the exit
(`TranslateBranchSystem.cpp:17-97`, `IRBuilder.cpp:445-480` `ExitToPC`/`EmitConditionalExit`).
At that point it calls `NZCVPeek(Target)`, which reads guest words through the decoder's
`CheckRangeExecutable` (`Decoder.cpp:45-63`; the same rule the translate-ahead helper uses so
the read cannot fault) and returns DEAD, LIVE or UNRESOLVED. The verdict is stored on the
`ExitFunction` op (a new `i1:$NZCVDeadAtTarget{false}` in `IR.json`; only ever true for a
constant target with hint None or Call, never Return, never `Syscall`/`Break`/indirect/thunk).
DFCE's `ProcessBlock` seeds `FlagsRead` with the NZCV bits cleared for a block whose exit carries
the bit (`:635`); everything downstream (fusion's `NZCVLiveOut`, the `StoreNZCV`/`MSR` drop, item
2/4/5's future record forms) follows without change. `EntryNZCVLiveIn` is recomputed by a
forward reach from the entry block (sound), and the unit's tail additionally records
`ExitsAssumeNZCVDead` (set only when DFCE actually dropped a producer on the strength of a
dead seed).

### 7.2 The scan

From `Target`, with a budget of 16 instructions per path and 48 visited words in all (32/96
buys the last 8%, §5.2), depth-first over paths:

- a **full writer** (`CMP`, `CMN`, `SUBS`, `ADDS`, `ANDS`, `BICS`, `TST`, `NEGS`, `FCMP`,
  `FCMPE`, `MSR NZCV`) ends the path DEAD;
- a **reader** (`B.cond`/`BC.cond`, `CSEL`/`CSINC`/`CSINV`/`CSNEG` and aliases, `CCMP`/`CCMN`,
  `ADC`/`ADCS`/`SBC`/`SBCS`/`NGC`/`NGCS`, `FCSEL`, `FCCMP`/`FCCMPE`, `MRS NZCV`) ends the scan
  LIVE (readers that also write, like `CCMP`, are readers first);
- a **partial writer** (`SETF8`/`SETF16`, `RMIF`, `CFINV`, `AXFLAG`/`XAFLAG`) ends the scan
  UNRESOLVED;
- `B` follows its target; `CBZ`/`CBNZ`/`TBZ`/`TBNZ` continue on both legs; `SVC` continues
  (§3: the kernel preserves NZCV, and the `Syscall` op's spill/fill does the same);
- `BL`, `BLR`, `BR`, `RET`, `BRK`, `HLT` (including the thunk marker), `UDF`, an unhandled word,
  a non-executable word, or the budget end the scan UNRESOLVED in the first stage; stage 3 follows
  `BL` one level (scan the callee; DEAD ends the path, LIVE ends the scan, a `RET` reached with
  the flags still live resumes the scan after the call, anything else is UNRESOLVED);
- the verdict is DEAD only if every path ended DEAD.

Everything not listed is neutral. The table is mechanically checked, not trusted (§9 stage 1).
Instructions the frontend does not implement (`Matcher->Handler == nullptr`) are UNRESOLVED.

### 7.3 What stays conservative

Indirect exits, `Syscall`, `Break`, thunks, the ISB exit in `icache` mode, exits whose target
is not executable or not 4-aligned, and any exit the decoder emitted for a block it did not
decode (`Finalize`, `IRBuilder.cpp:420-427`, is a constant exit too and is peeked like any
other). `MAXINST=1` units exercise the peek at every instruction: a `cmp` unit's exit goes to
the `b.cond` unit, the scan sees a reader first, the compare stays.

### 7.4 The `RET` assumption, kept separate

11.4% of kept compares reach a `RET` and 9,079 scans stop at a callee's `ret` (§5.2). AAPCS64
allows treating NZCV as dead there. The A76 does not enforce it and the probes show hand-written
code can read across a `ret` correctly on both machines. Treating `RET` as dead is a program
property, not an architecture property: it belongs behind its own switch
(`POWERARM_NZCVRETDEAD`), default off, with its risk stated as "any guest that reads flags across
a return in hand-written code miscompiles silently". Its value is 8,819 more droppable compares
on `cc1` (6 points). The document does not recommend turning it on in this round.

### 7.5 SMC and the code cache

The peek reads bytes outside the unit's extent. Two consumers must see that:

- **SMC.** Add every page the scan touched to the unit's `CodePages`
  (`Decoder.h` `DecodedBlockInformation::CodePages`, already "start addresses of all pages
  touching the block", already what `mtrack` invalidation and the has-code bitmap index), and
  widen `[ExtentStart, ExtentLength)` to the hull of unit and witnesses *only when the hull is
  within 64 KiB*; otherwise refuse the DEAD verdict for that exit. On `cc1` the branch-target
  scans are within 4 KiB for 68% of DEAD verdicts and within 64 KiB for 81% (simple), so the
  bound costs a fifth of the simple scan's value and keeps a single extent. A followed `BL`
  target 16 MiB away (stage 3) needs a second extent or a per-page witness index: that is stage
  3's design cost and the reason it is a separate stage. A JIT guest (V8, JSC) that rewrites a
  page some other unit peeked into invalidates that unit too; the cost is a recompile, never a
  stale assumption.
- **Cache.** The verdict is a pure function of guest bytes, so the loader re-runs the scan for
  every exit whose stored bit is set (a `NZCVDeadExits` bitmask over the unit's constant exits,
  in `JITCodeTail` where `EntryNZCVLiveIn` sits, plus the exit's target which the relocation
  records already carry) and refuses the block when any verdict differs. Cost: a few dozen word
  reads per cached unit. No witness hashes, no variable-length records, no change to
  `SegmentBlock`; the tail already travels with the code. The config id gets the switch and the
  scan parameters, like `POWERARM_MAXLEADERS` (`CodeCache.cpp:704-706`).

### 7.6 Signal frames under the policy

At a drain point following a dropped producer, `pstate` carries the last kept producer's value
(today's behaviour, probes 1 and 3). The policy adds the entry drain of a DEAD target to the set
of such points. What stays exact: every synchronous fault whose faulting instruction precedes a
reader (the `MayRaiseSignal` rule is untouched), every `rt_sigreturn` edit (the resumed unit
starts at the frame PC and reads the unpacked value; if that PC is a DEAD target the edit is
overwritten by the first writer, as on hardware), and every boundary that carries flags to
guest code. What is not exact and cannot be made exact without keeping the producer: a handler
that *reads* `pstate` at a DEAD entry. There is no rematerialisation option: the producer's
operands are dead SSA values by then. If a guest that decides on `pstate` at such a point ever
appears, the switch turns the policy off for that guest; `sigpreempt` and a new frame-reading
test (§9) pin the round-trip case.

### 7.7 The tripwire

The question is whether the scan's word table and walk can disagree with the frontend's own
translation about one sequence of guest instructions. **The check is therefore local and
immediate, and holds no state at all:** when the frontend finishes a unit whose `EntryNZCVLiveIn`
is set, the scan is run on that unit's entry right there, before control returns to the guest, on
the bytes the decoder has just read. `SCAN_DEAD` from that scan is the contradiction. It is
counted always and aborts under `POWERARM_NZCVEXITDEAD=strict`. It catches only entries that read
within their own unit, so it is a detector for table bugs, not a proof; the canary below is the
proof.

Two earlier shapes of this check were wrong for the same reason, and the reason is worth stating
because it applies to any cross-unit assumption recorded by guest address. The first put the gate
on the link (a link from a unit with `ExitsAssumeNZCVDead` into a target with `EntryNZCVLiveIn`);
the second moved it to compile time but kept the link version's structure -- a process-lifetime,
address-keyed record of every DEAD verdict, checked against every compiled unit's entry. **A guest
address does not name a fixed sequence of instructions.** A JIT frees the code at an address and
emits different code there; so do `dlclose`/`dlopen`, plugin loaders and trampoline patchers. The
record outlives all of it, so what it eventually reports is a verdict about instructions that no
longer exist set against a translation of the instructions that replaced them: two correct
observations of two different programs. On Octane 2.0 under Firefox that is not a corner case, it
is the *only* thing the record reported, while every architectural check kept passing --
`unittests/A64Frontend/exitdeadsmc.S` is that failure reduced to twelve lines of guest code.

The immediate check is also a wider net than the record was: it fires at every compiled unit whose
entry reads NZCV, peeked or not, rather than only where some unit happened to have peeked. What it
gives up is an address that is peeked but never translated -- an address the guest never executes,
where no assumption can be observed -- and units served whole from the code cache, which are never
translated.

## 8. What it is worth

The value is a multiplier on G2's mechanism, so G2's numbers are the calibration: 9.0% of
compares dropped gave -3.0% instructions and -4.5% cycles on warm `cc1`, with the dropped set
biased to loop-internal compares. The simple scan makes another 26.5% of all compares droppable
(37,509 of 141,707), `BL`-following 40% (56,743). Scaling linearly would give -9% and -13%
instructions; the sites are colder than G2's on average, and a `cmp`+`B.cond` at a unit exit is
by construction the compare that decides which unit runs next, so it is not cold either. The
estimate carried into the plan is **2-4% of executed host instructions for stage 2 and 3-6%
for stage 3**, on compiled C/C++, with the honest caveat that the per-site execution weight was
not measured (§10). Independently of the number, the policy is what lets items 2, 4 and 5 fuse
*instead of* the packed producer rather than in addition to it: at 55% of exit sites their
"after" columns become reachable; today they are reachable at 9%.

Cost: a scan of ≤16 words per constant exit at compile time (287k exits in `cc1`, tens of
milliseconds against a 13 s compile) and a re-scan per cached unit at load; the code-size
change is negative.

## 9. Staged plan, with the gate that would catch each stage being wrong

The floor for every stage is the three-mode `unittests/A64Frontend/run.sh` (default,
`POWERARM_MAXINST=1`, `POWERARM_HOSTFEATURES=disableisa30`) at its current count with zero
failures, `a64diff-run.sh 64k` with the latest bundle, `check-code-cache.sh`,
`check-rootfs-server.sh`, and the Firefox headless screenshot and VS Code launch that found and
retired G2's miscompile. G2's lesson is that this floor passed while Firefox failed, so each
stage adds an instrument that turns a wrong assumption into a deterministic failure.

**Stage 0 — census in tree.** Commit `nzcv_census.py` and the probes (this commit), and add an
exit-site traversal counter to the link record so that a later run can weight §5 by execution
(the P12 branch census did this for probes; `LinkOutcome*` in `JIT.cpp:1765` is the place).
Gate: none; it changes no codegen.
**Done, §13.** It went into `DEF_OP(ExitFunction)` rather than the link record, because a link
record counts first traversals and a linked exit never reaches it again; the counter is
`POWERARM_NZCVEXITCENSUS`, default off, and §13.4 is the answer it gave.

**Stage 1 — the table, proven against the frontend.** `NZCVPeek` as a pure function over words,
plus `EntryNZCVLiveIn` recomputed by forward reach. A generated test enumerates every `a64.inc`
entry with a handler, synthesises words for it (fixed bits set, fields random, several per
entry), translates each alone, classifies the resulting IR with DFCE's `ClassifyFast` (reads
NZCV / writes all of NZCV / writes some / neither / terminator) and asserts the peek's table
gives the same class. Any handler the table calls neutral that the frontend translates into a
flag reader or writer fails the build. Gate: that test, at 100% of handled entries, run in
CI with the suite. Nothing in codegen changes; ship it alone.

**Stage 2 — the simple scan, behind a switch, with a canary.** `POWERARM_NZCVEXITDEAD` in
`{off, on, canary, strict}`, hashed into the config id, default `off`. `on` seeds dead at DEAD
exits within the 64 KiB hull bound; `canary` does the same *and* at every such exit writes a
deliberately wrong NZCV (all four bits inverted relative to the last kept value, or a fixed
`N-C-`) into CR0/XER before the branch, so that any successor, signal handler or cache-loaded
unit that reads flags there takes the wrong direction every time instead of the right one by
luck. `strict` is `on` plus the tripwire abort. Gates: (a) the floor in `on`; (b) the floor in
`canary`, byte-identical outputs, which is the gate with teeth: a successor that reads flags at a
DEAD target flips a branch in `cmpbranch`, `cmpchain`, `callret`, `sigpreempt`, `sigedit`, the
zlib and Lua M2 builds (byte-identical outputs) and the Firefox/VS Code runs; (c) a new
generated test `exitdead` (gen.py): for each reader kind (`b.cond` all 14 conditions, CSEL
family, CCMP, ADC/SBC, `MRS`) and each writer kind, successors at 0, 8, 16, 17, 33 instructions
from a far exit (past the region window, past 4 KiB, past 64 KiB), reached by B, B.cond taken
and not-taken, CBZ/TBZ legs, BL, and by fallthrough at the cap; with the same successors
reached through the dispatcher (cache off, `MAXINST=1`) and through the cache (cold and warm,
`check-code-cache.sh`); plus a signal-frame case: a handler that records `pstate` at an async
delivery and an `MSR`/`MRS` round-trip at a DEAD entry, golden from the Pi, with the frame's
NZCV value *excluded* from the golden and its round-trip *included*; (d) `check-code-cache.sh`
with the cache built under `on` and loaded under `canary` and vice versa (the config id must
separate them). Then default `on`.

**Stage 3 — `BL` following.** The callee scan and the second extent (or per-page witness
index) for far callees. Gates: everything in stage 2 in all four modes, `callret.c` (recursion,
longjmp out of calls, `ret x1`, tail calls, self-modifying caller) in `canary`, and `exitdead`
extended with callees that write, read, return without writing, and are modified after the
caller was compiled (`icfresh`/`icpartial` shapes).

**Stage 4 — not recommended now.** The AAPCS `RET` assumption (§7.4), off by default forever
unless a workload shows it matters; and cross-block fusion for the 2% of compares whose fusable
readers sit in another block, which is a fusion change, not a liveness change.

The order puts the provable part first (stage 1 changes no codegen and its test is exhaustive
over the decoder), the reversible part second (one env var, hashed, with the canary in front of
the floor), and the part that touches SMC extents last.

## 10. What this could not measure

- **Execution weight.** Every number in §5 is per translated site. G2's 9% of sites were worth
  3% of instructions; the constant-exit sites may be worth proportionally more or less. Stage 0's
  counter answers it; without it the estimate in §8 is a bracket, not a number.
  **Closed 2026-09-29: §13.** The counter is `POWERARM_NZCVEXITCENSUS`; it says the DEAD sites are
  the hotter half of the population the peek scans but that the population itself is the cold end of
  the exit distribution, and §8's bracket comes down by about a factor of two. The in-unit classes,
  and therefore a calibration against G2 itself, are still unweighted (§13.5 item 6).
- **Other workloads.** Only `cc1` was censused. Firefox, VS Code and Factorio are C++ with
  exceptions and virtual calls (more `blr`, more `br` through jump tables: both UNRESOLVED),
  V8/JSC-generated code has different shapes and an SMC exposure the `cc1` census cannot show.
  The census script runs on any IR dump; running it on `libxul` under the headless screenshot is
  a two-command job and should precede stage 2's default-on.
- **glibc targets.** 10,899 (simple) to 13,269 (follow-`bl`) unresolved verdicts are calls into
  glibc, which the census did not disassemble. The implementation reads memory and has no such
  limit; the true DEAD share is higher than §5.2 says by some part of those.
- **The cost of witness pages on JIT guests.** How often V8's or JSC's code-space writes hit a
  page some unit only peeked into, forcing recompiles, is unmeasured; the hull bound limits it
  and the census cannot size it.
- **ptrace and gdbserver.** Not probed; fidelity only.
- **A guest that decides on `pstate`.** The claim "none in the reference set" is the absence of
  evidence after probes, suites, and reading the runtimes' known handlers; it is not a proof.
- **The A76 sampling artefact.** Timer interrupts landed only at three of the seven PCs in
  `loop_dropped`; that is the core's interrupt-taking behaviour, not a property of NZCV, and it
  does not affect any conclusion.
- **Host instructions per dropped compare.** Taken from `ISA-OPPORTUNITIES.md` §3.0 (3 for a W
  compare plus the projection consumers pay); not re-measured.

## 11. Findings to carry out of this document, independent of the item

1. `EntryNZCVLiveIn` is computed from block 0 only and misclassifies 17 `cc1` units that read
   NZCV from entry in a later block (§4.2). It is harmless today and must not be used as a
   guard until recomputed by forward reach.
2. The direct-link refusal for `EntryNZCVLiveIn` targets buys nothing (the thunk delivers the
   same flags) and costs a thunk link on 0.26% of units. It can stay as the tripwire's hook.
3. POWERarm's `uc_mcontext.pstate` diverges from the kernel ABI at drain points and after
   dropped compares (probes 1 and 3), as G2 accepted; it is exact at syscalls, calls, returns,
   sigreturn and synchronous faults preceding a reader. Column 3 of §3 records it; nothing in
   the reference set is known to depend on it.
4. The native `lwarx`/`stwcx.` work (item 3, in flight) removes a `LoadNZCV`/`StoreNZCV`
   bracket: `stwcx.` records into CR0, so whatever replaces the bracket must be classified in
   DFCE as writing NZCV (or preserve CR0 itself); the peek's guest-level table sees `STXR` as
   neutral, correctly, and relies on the backend to keep the host clobber out of the guest's
   view. The two changes are adjacent but independent.

## 12. Files

- `nzcv-probes/nzcv_probe.c` — async sampling, handler edit, synchronous fault, libc and
  syscall boundaries, cross-unit entry readers.
- `nzcv-probes/nzcv_probe2.c` — raw-`svc` signal boundary, syscall continuation entry, a kept
  compare with an exit leg, the signal-barrier shape.
- `nzcv-probes/nzcv_probe3.c` — a dropped in-unit compare followed by a fault: the inherited
  sentinel is what the frame shows.
- `nzcv-probes/RESULTS.txt` — outputs, Pi and POWERarm (fusion on and off), verbatim.
- `nzcv-probes/nzcv_census.py` — the IR-dump census and the scan; `census/cc1-nzcv.census`
  is its output for `cc1 -O2 lvm.c` at `cffa7b4de`.
- `nzcv-probes/nzcv_exit_weight.py` — joins the in-emulator census's static rows with its executed
  traversals (§13); `census/cc1-nzcv-exec.census` and `census/libxul-nzcv-exec.census` are its
  output, with the raw per-process dumps appended.
- `FEXCore/Source/Interface/Core/JIT/PPC64LE/NZCVExitCensus.{h,cpp}` — the census itself:
  §5.1's classification and §7.2's scan as C++ over the live IR and real guest words, plus the
  per-thread traversal counters the JIT bumps under `POWERARM_NZCVEXITCENSUS`.

## 13. Execution weight, measured

Added 2026-09-29, against the tree at `ad49fb534` plus the exit-site census this section
describes. This closes §10's first bullet and is stage 0 of §9. Nothing above is rewritten: §10
still records what could not be measured when the document was written, and now says where the
answer is. Two workloads, `cc1` and `libxul`, both with `POWERARM_NZCVEXITCENSUS=1`; raw output in
`census/cc1-nzcv-exec.census` and `census/libxul-nzcv-exec.census`, joined by
`nzcv-probes/nzcv_exit_weight.py`.

**Answer in one line.** The executed weight *raises* the DEAD share within the population the
peek scans (45.1% of constant-exit-only traversals against 39.4% of its sites on `cc1`), and
*lowers* the value of the item, because the constant-exit population is the cold end of the exit
distribution: §8's estimate of 2-4% for stage 2 and 3-6% for stage 3 does not survive as written.
At the 3 host instructions per dropped compare §8 itself prices with, the measured executed
counts give **1.7% for the simple scan and 2.2% with `BL` following on `cc1`**, and about 0.7× that
on `libxul`. The item is still worth roughly what G2 was worth; the brackets were about twice too
high.

### 13.1 The instrument

`POWERARM_NZCVEXITCENSUS=1` (`NZCVExitCensus` in `Config.json.in`, default off) turns on two
tables over one slot space, so a static row and a dynamic row are the same row:

- **Static**, filled at compile time by
  `FEXCore/Source/Interface/Core/JIT/PPC64LE/NZCVExitCensus.cpp`. For every surviving NZCV
  producer it walks the unit's CFG forward until every bit it wrote is overwritten — the same walk
  `nzcv_census.py`'s `forward_reach` does, over `IROpNZCVRead`/`IROpNZCVWrite`, which are derived
  from DFCE's own `ClassifyFast` table so the census and the pass whose seed the policy would
  change cannot disagree. It then classifies the producer by the exits it reaches, using §5.1's
  terms verbatim, and for the constant-exit-only population runs §7.2's scan over real guest words
  (through `QueryGuestExecutableRange`, so a read cannot fault) in both the simple and the
  follow-`BL` shape.
- **Dynamic**: one 64-bit counter per slot per guest thread, and three instructions appended to
  every `ExitFunction`'s lowering per counter it bumps — `ld TMP2, slot*8(TMP1); addi TMP2,TMP2,1;
  std TMP2, slot*8(TMP1)`, after one `ld TMP1, Pointers.PPC64_NZCVExitCounters(STATE)`. Each site
  bumps its own exit-kind counter plus one per distinct (class, verdict) pair whose producer's
  flags reach it, capped at four; `site_slot_overflow` was **0** on both workloads, so nothing was
  dropped by the cap.

Three properties of the bump matter. It uses `ld`/`addi`/`std` only, so it touches no CR field and
no XER and therefore cannot perturb the flags it is measuring, and it cannot move the r0-dirty
state `DEF_OP(ExitFunction)` snapshots. It is per thread, so it needs no atomic — an atomic
increment here would be an `lwarx`/`stwcx.` loop and `stwcx.` records into CR0, which is where N
and Z live. And it reaches its array through a frame slot rather than an absolute address, so a
block saved to the code cache in one process bumps the *loading* process's array; the slot index is
a pure function of guest bytes plus the config id, which now hashes the option
(`CodeCache.cpp`), so a census run can never load blocks compiled without the bump.

**Why a compare is priced by the traversals of the exits it reaches.** A compare kept only by its
exits executes once per execution of its block, and on any one execution exactly one of the exits
its flags reach is traversed, because those exits are alternative legs. Summing their traversals is
therefore the number of times that compare executed, which is what the saving scales with. The sum
over exit-kind counters is the total `ExitFunction` traversal count, since every instrumented site
bumps exactly one of them.

**Off changes nothing.** One `if (NZCVExitCensusEnabled)` at the top of `DEF_OP(ExitFunction)`,
false unless the option is set; the analysis is not run and `NZCVCensusSlots` stays empty. The one
structural change, the counter pointer, is the **last** member of `JITPointers` and lands in
padding the frame already had: `sizeof(CpuStateFrame)` is 2176 before and after and
`offsetof(Pointers)` is 976 before and after, so not one STATE-relative d-form displacement in the
backend moves. The staging-buffer allowance the census adds is `0` when it is off. Nothing in the
census feeds a codegen decision in either state: it does not seed flags dead, does not change a
link, does not touch DFCE. Gates: `unittests/A64Frontend/run.sh` **101 pass, 0 fail** in all three
modes (default, `POWERARM_MAXINST=1`, `POWERARM_HOSTFEATURES=disableisa30`) with the census off and
again with it on — six runs, same 101 — and `check-code-cache.sh` **35 ok, 0 fail**.

### 13.2 `cc1 -O2 lvm.c`: the static census reproduced, then weighted

Same input as §5, code cache off, fusion on. 110,592 units, 287,389 instrumented exit sites,
128,877 kept producers, **618,683,967 executed exit traversals**. The static columns are an
independent reimplementation of §5 inside the emulator and land on §5's numbers: kept producers
128,877 against 128,968, constant-exit-only 79.3% against 79.3%, `constant and indirect-Return`
9,402 against 9,409, in-unit `CondSubNZCV` 5,960 against 5,963, `indirect-Return` 5,288 against
5,291, in-unit fusable-only 2,523 against 2,525. With fusion off it finds 141,608 kept producers
against §4.1's 141,707, i.e. fusion drops 12,731 against 12,739.

| §5.1 class | peek verdict | producers | of kept | exit sites | traversals | of all traversals | per site |
|---|---|---|---|---|---|---|---|
| exit: constant only | droppable, simple scan | 40,209 | 31.2% | 52,528 | 121,704,571 | 19.7% | **2,317** |
| exit: constant only | droppable, follow-`BL` only | 15,050 | 11.7% | 23,490 | 30,695,232 | 5.0% | 1,307 |
| exit: constant only | UNRESOLVED (keep) | 46,268 | 35.9% | 83,104 | 114,640,761 | 18.5% | **1,379** |
| exit: constant only | LIVE (keep) | 620 | 0.5% | 1,203 | 2,809,625 | 0.5% | 2,336 |
| exit: constant only | DEAD but unfusable reader | 9 | 0.0% | 9 | 439 | 0.0% | 49 |
| exit: constant and indirect-Return | n/a | 9,402 | 7.3% | 19,416 | 75,890,484 | 12.3% | 3,909 |
| exit: indirect-Return | n/a | 5,288 | 4.1% | 4,379 | 43,744,712 | 7.1% | **9,990** |
| exit: constant and indirect-Call | n/a | 1,649 | 1.3% | 4,105 | 5,023,852 | 0.8% | 1,224 |
| exit: constant and other | n/a | 334 | 0.3% | 714 | 2,151,974 | 0.3% | 3,014 |
| exit: indirect-Call | n/a | 727 | 0.6% | 733 | 1,605,209 | 0.3% | 2,190 |
| exit: other | n/a | 322 | 0.2% | 515 | 3,080,019 | 0.5% | 5,981 |
| in-unit: unfusable reader (`CondSubNZCV`) | n/a | 5,960 | 4.6% | — | — | — | — |
| in-unit: only fusable readers visible | n/a | 2,523 | 2.0% | — | — | — | — |
| in-unit: unfusable reader (other) | n/a | 184 | 0.1% | — | — | — | — |
| in-unit: no visible reader | n/a | 332 | 0.3% | — | — | — | — |

An in-unit class has no traversal weight by construction: its flags reach no exit, so this
instrument cannot price it (see 13.5). A repeat of the run moves the raw counts by 0.003% and none
of the percentages in this section at one decimal place, so nothing here rests on a single run.

Exit traversals by kind, which is where the item's real discount comes from:

| exit kind | sites | traversals | share | per site |
|---|---|---|---|---|
| const-None | 186,739 | 323,078,762 | 52.2% | 1,730 |
| indirect-Return | 20,012 | 143,931,283 | 23.3% | 7,192 |
| const-Call | 74,020 | 130,594,543 | 21.1% | 1,764 |
| indirect-Call | 5,867 | 13,194,789 | 2.1% | 2,249 |
| indirect-None | 751 | 7,884,590 | 1.3% | 10,499 |

**The side-by-side §10 asked for.**

| | static share | executed share |
|---|---|---|
| simple scan DEAD, of constant-exit-only | **39.4%** (40,209 of 102,156; §5.2 said 36.7%) | **45.1%** (121.70 M of 269.85 M) |
| simple scan DEAD, of all kept compares | 31.2% | 19.7% of all exit traversals, 30.3% of flag-carrying ones |
| follow-`BL` DEAD, of constant-exit-only | **54.1%** (55,259; §5.2 said 55.5%) | **56.5%** (152.40 M of 269.85 M) |
| follow-`BL` DEAD, of all kept compares | 42.9% | 24.6% of all exit traversals, 38.0% of flag-carrying ones |
| constant-exit-only, of all | 79.3% of kept compares | 43.6% of exit traversals |

Two things read off that. Within its own population the peek finds the *hotter* half: 2,317
traversals per DEAD site against 1,379 per UNRESOLVED site, so the executed share (45.1%) is six
points above the static share (39.4%). But the population itself is cold: a constant exit is
traversed 1,730-1,764 times per site against 7,192 for an `indirect-Return` and 10,499 for an
`indirect-None`, and constant-exit-only compares are 79.3% of the kept compares but only 43.6% of
the executed exit traversals.

### 13.3 `libxul`: the workload §10 said to check before defaulting this on

Firefox 156 `--headless --screenshot` of a local page with DOM, string, array, JSON and canvas
work, code cache on in a directory of its own, `POWERARM_PORTABLE=1` with an absolute
`POWERARM_ROOTFS` so the content processes ran on this build rather than the promoted stable. Nine
processes, summed: 521,966 units, 1,108,043 exit sites, 389,082 kept producers, **556,880,760
executed exit traversals**.

| | `cc1` | `libxul` |
|---|---|---|
| constant-exit-only, of kept compares | 79.3% | **61.1%** |
| simple scan DEAD, of constant-exit-only: static / executed | 39.4% / 45.1% | **34.4% / 39.5%** |
| follow-`BL` DEAD, of constant-exit-only: static / executed | 54.1% / 56.5% | **43.7% / 47.4%** |
| simple scan DEAD, of all exit traversals | 19.7% | **14.4%** |
| follow-`BL` DEAD, of all exit traversals | 24.6% | **17.3%** |
| indirect-Call exit sites | 5,867 (2.0%) | **203,192 (18.3%)** |
| traversals per const-None site | 1,730 | 496 |
| traversals per indirect-Return site | 7,192 | 1,351 |

The shape §10 predicted is there and is now sized: C++ with virtual calls moves 18.3% of exit
sites to `indirect-Call` against `cc1`'s 2.0%, and constant-exit-only drops from 79.3% to 61.1% of
kept compares. It is a discount, not a cliff — `libxul` keeps 87% of `cc1`'s static DEAD share and
73% of its executed share — and the direction of the static-to-executed correction is the same
(39.5% executed against 34.4% static). `libxul`'s exits are also far colder per site (496 per
const-None site against `cc1`'s 1,730), which is what a one-shot page load rather than a compile
looks like; it is the per-traversal shares, not the per-site ones, that carry across.

### 13.4 What this does to §8's estimate

The measured quantity is compare *executions*: 121,704,571 for the simple scan and 152,399,803
with `BL` following, on `cc1`. Turning that into a share of executed host instructions needs one
number this census does not measure, and which §10's last bullet already lists as not re-measured:
the host cost of one dropped compare. From `DEF_OP(SubNZCV)` (`ALUOps.cpp`) that is 1 instruction
for a 64-bit register compare (`subfco.`), 3 for a W-size one (two `sldi` plus `subfco.`), plus a
1-5 instruction `LoadConstant` when an operand is an immediate; §8 prices it at 3, from
`ISA-OPPORTUNITIES.md` §3.0. Against the recorded warm `cc1` figure of 21.25 G `instructions:u`
(post-G2, `OPTIMIZATION-CHECKLIST.md` P5):

| host instructions per dropped compare | stage 2 (simple) | stage 3 (follow-`BL`) |
|---|---|---|
| 1 | 0.57% | 0.72% |
| **3 (§8's own figure)** | **1.72%** | **2.15%** |
| 6 | 3.44% | 4.30% |

**Verdict: the estimate does not survive execution weighting; it comes down by roughly a factor of
two, and it does not collapse.** §8's 2-4% for stage 2 needs at least 3.5 host instructions per
dropped compare, and its 3-6% for stage 3 needs at least 4.2; at the 3 the same section quotes, the
answers are 1.7% and 2.2%. The bracket that should be carried into the plan is **0.6-3.4% for stage
2 and 0.7-4.3% for stage 3 on `cc1`, centred on 1.7% and 2.2%**, with about 0.7× those shares on
`libxul` — and the whole remaining width now belongs to one unmeasured quantity, the host cost of a
dropped compare, rather than to the execution weighting. Against G2's delivered -3.0%, stage 2 at
1.7% is still the same order of lever, and §8's separate argument (that the policy is what lets
items 2, 4 and 5 fuse *instead of* the packed producer at 55% of exit sites rather than 9%) is
untouched by any of this.

### 13.5 What the dynamic data contradicts, and what it cannot say

1. **§8 is wrong that these sites are not cold.** "A `cmp`+`B.cond` at a unit exit is by
   construction the compare that decides which unit runs next, so it is not cold either" — but the
   constant exits are the *cold end* of the exit distribution on both workloads: 1,730 and 1,764
   traversals per site (const-None, const-Call) against 7,192 for `indirect-Return` and 10,499 for
   `indirect-None` on `cc1`, and 496/346 against 1,351/1,413 on `libxul`. The hot exits are the
   returns and the indirect jumps, which this policy leaves alone. That, not the scan's choice of
   targets, is where the factor of two goes.
2. **§8 is right to worry, but wrong about the mechanism.** Its hedge was that the scan's sites are
   colder *on average*; in fact the scan picks the hotter half of the population it scans (2,317
   traversals per DEAD site against 1,379 per UNRESOLVED one), so the executed share is six points
   above the static share. The discount is entirely the population, not the peek.
3. **§5.2's 36.7% was low, as it suspected.** Reading real memory instead of a partial `cc1`
   disassembly resolves part of the 10,899 "target outside the disassembly" rows and moves the
   simple scan's DEAD share from 36.7% to 39.4%.
4. **§5.2's 55.5% is slightly high for a conservative implementation.** This census does not resume
   a scan after a callee that returns with the flags still live, which the script does; its
   follow-`BL` DEAD share is 54.1%.
5. **§10's "other workloads" bullet is now answered for `libxul`** (13.3), including the `blr`/`br`
   effect it named. It is still unanswered for V8/JSC-generated code, where the SMC exposure — not
   the liveness — is the open question.
6. **This instrument cannot price the in-unit classes, and therefore cannot calibrate against G2.**
   A compare whose flags reach no exit has no exit traversal to be weighted by, and the 12,731
   compares fusion drops are exactly that population: with fusion off the producer-weighted
   traversal total is 401,280,286 against 401,346,878 with it on, a 0.02% difference. So G2's own
   instructions-per-executed-compare, which would remove the last unknown in 13.4, needs a
   block-execution counter rather than an exit-traversal one. That is the one thing stage 0 should
   have measured and did not.
7. **Unchanged from §10:** the cost of witness pages on JIT guests, ptrace fidelity, whether any
   guest decides on `pstate`, and the host cost of a dropped compare.

### 13.6 Reproducing

```
# cc1, the same input as §5, under the conditions census/cc1-nzcv.census used
cd <lua-5.4.9>/src
TMPDIR=<dumpdir> POWERARM_NZCVEXITCENSUS=1 POWERARM_ENABLECODECACHINGWIP=0 \
  <build>/Bin/POWERarm <rootfs>/usr/lib/gcc/aarch64-unknown-linux-gnu/*/cc1 -quiet -O2 lvm.c -o /dev/null
python3 nzcv-probes/nzcv_exit_weight.py <dumpdir>/powerarm-nzcv-exits-*.txt

# libxul: headless, cache ON (item 21 open (c)) in a fresh directory, children on this build
TMPDIR=<dumpdir> POWERARM_NZCVEXITCENSUS=1 POWERARM_PORTABLE=1 \
  POWERARM_ROOTFS=$HOME/.local/share/powerarm/RootFS/ArchLinuxARM-vk \
  POWERARM_APP_CACHE_LOCATION=<fresh> MOZ_ENABLE_WAYLAND=0 \
  <build>/Bin/POWERarm <vk-overlay>/usr/lib/firefox/firefox --headless --no-remote \
  --profile <fresh> --screenshot <out.png> file://<page.html>
python3 nzcv-probes/nzcv_exit_weight.py <dumpdir>/powerarm-nzcv-exits-*.txt
```

A fresh cache directory per run is not optional: a run that loaded blocks compiled under a
different configuration would under-count exactly the sites it is measuring. The config id hashes
the option, so the two populations cannot mix by accident, but the option is a diagnostic and the
cheapest safe habit is a directory per run.
