# Latency, round 3: the x86 inheritance, the cold path after R1/R2, and decode as a table lookup

Design research, 2026-09-23, tree at `761657a4d`. No code, no builds, no
instrumented runs. Everything measured here is static: `a64.inc` parsed the
way `DecodeTable.cpp` parses it, and the `.text` words of the real guest
binaries on this box (VS Code's `code`, Firefox's `libxul.so`, Factorio,
the Claude CLI, libc, libstdc++) classified through that table. The two
scripts are in the session scratchpad (`decode_table.py`, `decode_alt.py`),
not the tree. Companion to `COLD-ROUND2.md`; R1 and R2 have landed
(`93eae4d9b`, then `5740ba7c8` replaced the forked writer with a spawned
one after HANDOVER item 44; `c3c66319b`; the hot tier `d1296ca9a`) and
nothing here re-proposes R3-R7.

**One-paragraph answer.** Yes, we are still paying for x86, and the bill has
one large line and two small ones. The large one: mtrack write-protection is
the x86 coherency model -- an x86 store to code is architecturally visible to
the next fetch, so an x86 emulator has no signal from the guest and must trap
every write. AArch64 is the opposite: modified code is *not* visible until
the guest executes `IC IVAU` + `DSB` + `ISB`, and every JIT in the target set
does exactly that -- and POWERarm translates `IC IVAU` to a NOP
(`IRBuilder.cpp:66`) while advertising `CTR_EL0.DIC = 0` so that the guest
issues it. That NOP is the whole V8/JSC churn: each write into a 64K granule
that holds translated code costs a signal round trip, the exclusive
invalidation lock, the erasure of every block on 16 guest pages, a per-thread
lookup-cache scrub and `madvise`, an `mprotect`, and then the recompilation of
everything the granule held, followed by a re-arm at the next compile that
guarantees the next write faults again. Hanging invalidation on `IC IVAU`
instead is architecturally exact, strictly sounder than the arm-after-decode
window mtrack has today, and makes every SMC option in the tree dead code.
The two small ones: the L1 lookup hash indexes by `RIP & (N-1)` on a guest
whose PC has two zero low bits, so three quarters of the 2 MiB per-thread L1
can never hold an entry (a one-instruction fix, 4x the effective capacity),
and the SMC Idea 4 semantic-patch residue carries 96 bytes of empty x86
vectors on every block entry. On the cold path itself, after R1/R2 and G2-G4,
what is left is per-unit fixed cost: two to four `VMATracking` lock descents
per compile unit, six to eight heap allocations, and one IR node per guest
instruction that every pass walks; together perhaps 5-10% of a compile,
each item small and gated on `instructions:u`. Decode is already within one
mask-compare of a pure table lookup (expected scan depth 1.7-2.0 on the real
binaries, ~0.5% of translation); a two-bit index fix the comment already
claims to have and a frequency-aware bucket order make it one compare 95% of
the time for 60 KiB more; a memo cache cannot beat that; and going further --
threaded code -- is the cheap tier that row 10 closed, because it gives up
exactly the cross-instruction work (fusion, the GPR cache across edges, RA
regions, DFCE) the last month's wins came from.

## 1. Half one: what x86 still costs a fixed-length, aligned guest

Ranked by expected value. "Delete" means dead weight; "leave" means
conservative but cheap; "change" means a real per-block, per-write or
per-fault cost.

### X1. mtrack write-protection is the x86 coherency model; AArch64 announces every code write and we discard the announcement -- CHANGE

**The architectural fact this rests on.** The Arm ARM (DDI 0487, B2.2.5
"Concurrent modification and execution of instructions", and the caches
chapter's requirements for instruction-to-data coherence) does not make
instruction fetch coherent with stores. A PE may keep executing the old
instruction indefinitely after the bytes changed; the modification is
guaranteed visible only after the writer performs `DC CVAU` (unless
`CTR_EL0.IDC`), `DSB ISH`, `IC IVAU` (unless `CTR_EL0.DIC`), `DSB ISH`, and
the executing PE performs `ISB`. The concurrent-modification exemption
(B, BL, BRK, HVC, ISB, NOP, SMC, SVC) only bounds *what* a racing fetch may
see -- old or new -- it does not remove the maintenance requirement for the
new instruction to become visible. A translation cache is an instruction
cache with a bigger line; the architecture lets it hold stale translations
until `IC IVAU`. That is the whole argument.

POWERarm already relies on the guest honouring this: `SystemRegisters.h:38-41`
advertises the Pi 5's `CTR_EL0 = 0x9444C004` (IDC=1, DIC=0, 64-byte lines),
so libgcc's `__aarch64_sync_cache_range`, V8's `FlushInstructionCache`
(the string is in `~/Development/vscode-arm64/current/code`), JSC's
`cacheFlush`, Mono, .NET, OpenJDK and LuaJIT all skip `DC CVAU` and issue
one `IC IVAU` per 64 bytes they wrote, then `DSB ISH; ISB`. And then
`IRBuilder.cpp:66` maps `IC_IVAU` to `CacheMaintenanceNop`
(`TranslateBranchSystem.cpp:263-273`: "SMC tracking (mtrack) already
invalidates translations of written pages"), and `:62` maps `ISB` to `HINT`.
The guest tells us exactly which 64 bytes changed and when they must become
visible; we throw both away and reconstruct the information with page faults.

**What it costs, per new code page** (`Core.cpp:1279-1300` ->
`SyscallsSMCTracking.cpp:1103-1335`): the VMA map version sample, the
`VMATracking.Mutex` shared lock, a walk of the VMAs overlapping the page,
and -- for a writable mapping, which is every JIT code page --
`mprotect(granule, PROT_READ)` at `:1287` widened by
`SMCGranule::Cover` (`SMCHostGranule.h:107-118`) to the 64K host granule.
Nothing checks whether the granule is already armed, so the first compile on
each of a granule's 16 guest pages issues the same 64K `mprotect` again: 16
syscalls per 64K of JIT code, each a hash-page-table walk on this kernel
(`GranuleTable.h:143-145` says why that is not free). The memo at `:1130`
(`IsMarkNoOp`) only remembers pages that needed *no* action.

**What it costs, per guest write into a granule that holds translated
code** (`HandleSegfault`, `SyscallsSMCTracking.cpp:235-1010`, default
options): the SIGSEGV round trip; `ReleaseAllPendingSharedLocks` (`:291`);
`VMATracking.Mutex` + `FindVMAEntry` (`:295-315`); `NoteFault` (`:620`);
then `TM.InvalidateGuestCodeRange` over the whole granule
(`ThreadManager.h:411-434`): `TakeCodeInvalidationWriteLockOrSteal` -- the
write-priority mutex blocks every new `CompileBlock` and
`ExitFunctionLink` in the process and waits out the in-flight ones, dying at
4 s (`:379-387`); `InvalidateCodeBuffersCodeRange` (`Core.cpp:1432-1456`)
-> `GuestToHostMap::InvalidateRange` (`LookupCache.h:731-754`), which
erases *every* block registered on the granule's 16 pages and runs the
delinker of every inbound link of each (`SeverLinks`, `:229-258`: a word
patch plus `sync; icbi; sync; isync` per link); then, per thread in the
process, `InvalidateThreadCachedCodeRange` (`Core.cpp:1580-1600`): the
thread's lookup write lock, a `CachedCodePages` walk, L1 entry zeroing, and
-- with `ShadowRetStack` on by default -- one `madvise(DONTNEED)` of that
thread's callret stack (`:1597`); then `mprotect(granule, RW)` (`:435-460`);
then, if the faulting store sits in a block on the same page,
`IsAddressInCurrentBlock` -> `SpillSRA` and a redirect that abandons the
block and recompiles the store as a one-instruction unit
(`:919-994`, `Core.cpp:1404-1430`). The handler alone is of the order of
20 us (the tree's own smcstorm numbers, `SMCSoftInvalidate.h:16-18`); the
real bill comes after: every block the granule held is recompiled at ~12 us
when next executed, and the first of those recompiles re-arms the granule
(`Core.cpp:1292-1295`), so the JIT's next write to it faults again. V8
writes its code space continuously (Sparkplug/Maglev/TurboFan compiles, GC
relocation of embedded pointers); whether that space is RWX (every write
faults on the granule) or W^X-flipped with `mprotect` (every flip invalidates
the page -- the `write-protect-code-memory` flag is in the `code` binary, its
default is not readable from there) both doors lead to the same
`InvalidateGuestCodeRange`, and the same goes for JSC in the Claude binary
and for SpiderMonkey's W^X flips (COLD-ROUND2 R4). HANDOVER row 10's census
("~88% of anonymous units are invalidated") is this loop seen from the
block side.

**What it becomes: `SMCChecks=icache`.** The guest's cache-maintenance
sequence *is* the invalidation protocol:

- `IC IVAU Xt` lowers to a helper call (the shape `SVC` and `_ValidateCode`
  already use to leave JIT code) that first asks the SMC Idea 3 granule
  bitmap `ProvablyClear(VA & ~63, 64)` (`SMCCodeGranules.h:287-331`; its
  64-byte granule is exactly the `IminLine` we advertise, and it is
  lock-free) and returns immediately when no translated block lies in that
  line -- the common case, since a JIT flushes the code it just wrote into
  fresh memory. Otherwise it takes the exclusive invalidation path for
  `[line, line+64)` with *byte* precision: `InvalidateRange` filters the
  page's entries by the extent every block already carries
  (`JITCodeTail::RIP/GuestSize`, `CPUBackend.h:70-100`; the same test
  `RangeOverlapsCompiledCode` does at `LookupCache.h:419`) and erases only
  the overlapping ones, leaving the page registered when entries remain.
  The bitmap bits stay set (per-block clearing is forbidden,
  `SMCCodeGranules.h:73-78`; a stale set bit only costs the locked walk on
  the next flush of that line). The bitmap is enabled unconditionally in
  this mode (`Core.cpp:294` gates it on the store-emulation flags today);
  its cost is `SetPageRange` per code page per compile under a lock already
  held (`LookupCache.h:817`) and 8 KiB of leaf per 4 MiB of code VA.
- `ISB` ends the block (add it to `EndsBlock`, `Decoder.cpp:81-103`) and
  exits to the dispatcher: after a context synchronisation event the thread
  must re-fetch, which in this model means re-look-up. This is what makes
  "modify my own block, then `ISB`, then continue" correct: the continuation
  PC misses (or hits a block whose entry was erased) and is recompiled from
  the new bytes. Before the `ISB` the old translation may run, exactly as
  the architecture permits.
- `MarkGuestExecutableRange` is a no-op in this mode: no `mprotect`, no VMA
  walk, no `NoteGranulesArmed`; `HandleSegfault`'s SMC half is unreachable
  (every `SEGV_ACCERR` is then the guest's own).
- The syscall-side invalidations stay exactly as they are: `mmap` over,
  `munmap`, `mremap`, `shmdt`, `madvise(DONTNEED|FREE|REMOVE)`
  (`SyscallsSMCTracking.cpp:3071-3105`) all change bytes without a guest
  store. They mirror what the kernel does on hardware, and the kernel's own
  rule is the guide for `mprotect`: arm64 Linux syncs the I-cache for a
  page only the first time a PTE for it becomes executable
  (`PG_dcache_clean` in `arch/arm64/mm/flush.c`); a page flipped RX -> RW
  -> RX is *not* re-synced, which is why SpiderMonkey flushes after every
  patch. So `mprotect` that adds `PROT_EXEC` to pages that were never
  executable in this mapping invalidates (bytes may have arrived by
  `read(2)`); a flip back to `PROT_EXEC` on pages that were executable before
  needs nothing, because the guest's `IC IVAU` already did the precise
  work. That is COLD-ROUND2's R4 for free, without re-hashing a page of
  blocks at the flip.
- Aliases: `IC IVAU` on a PIPT instruction cache (we advertise
  `ICachePOL = 0b11`) invalidates by physical address, so a JIT that writes
  through a writable alias and flushes *that* VA is correct on hardware.
  The helper must therefore fan out to every mirror of a shared mapping's
  page -- the same `MappedResource` walk `HandleSegfault` does in
  `CollectMirrors` (`:374-393`) and `MarkGuestExecutableRange` does for
  `PROTECT-mirror` (`:1196-1230`). Private anonymous pages, which is what
  V8 and JSC use, need no walk.
- Batching (second step, only if the helper shows up): `__clear_cache` is a
  loop of `IC IVAU` then one `DSB ISH` then `ISB`. A per-thread ring of
  pending lines drained at the `DSB` and at the `ISB` takes the exclusive
  lock once per flush instead of once per line that hit. Not needed for the
  first version: the bitmap answers "clear" for fresh code without any lock.

**Why this is sound, and sounder than today.** `CompileBlock` holds
`CodeInvalidationMutex` *shared* from before decode to after publish
(`Core.cpp:1166`; the decode happens inside `CompileCode` at `:1090`), and
the `IC IVAU` helper takes it *exclusive*. So a write that lands mid-decode
is followed by an `IC IVAU` that blocks until the stale unit is published and
then erases it; a write that lands after the flush is by definition before
the next flush. This closes the window `SMCSoftInvalidate.h:141-147` calls
the "fresh-compile half" residual risk: mtrack arms the page *after* the
decode read the bytes, and a store in between is invisible to both the hash
and the protection. Under `icache` the guest's own ordering (store, then
`IC IVAU`) is what serialises against the compiler, and a guest that stores
without flushing is a guest that is broken on a Cortex-A76. Cross-thread: the
invalidation is process-global at the writer's `IC IVAU`; a thread already
inside an old translation finishes it and re-looks-up at its next exit,
which is the same in-flight semantics legacy invalidation has
(`SMCSoftInvalidate.h:60-69`) and stronger than the architecture, which lets
that thread run old code until *its* `ISB`.

**What could break, and what catches it.**

- A guest that rewrites bytes it has already executed and does not flush.
  Broken on the reference hardware too (DIC=0, and a PIPT I-cache will hold
  the old line); every runtime in the target set flushes;
  `check-code-cache.sh`'s SMC guest (`Scripts/powerarm/check-code-cache.sh:129-149`)
  uses `__builtin___clear_cache`. Bytes written into memory that was *never*
  executed (a loader `read(2)`-ing a plugin into RWX memory) have no stale
  translation to hit. `SMCChecks=full` remains the paranoid fallback, and
  `HostPageMode=degrade` still forces it.
- Dual-mapped JIT memory flushed through the writable alias: covered by the
  mirror fan-out above; a new test must exercise it.
- LDR (literal) constant pools inside code pages, which V8 patches during GC
  *without* a flush (it is data): safe only because the frontend translates
  the literal load as a real load (`TranslateLoadStore.cpp:63-74`,
  `LDR_lit_gen` -> `LoadStoreSingle`). The design must forbid folding
  code-page reads into immediates; `_ValidateCode`-style snapshots of the
  decoded word are the only code-page bytes a translation may bake in.
- The code cache's install path arms pages before hashing
  (`CodeCache.cpp:1775-1783`, with its ordering comment); with no arm the
  argument becomes the one above (the install runs under the shared lock).
- Gates: `unittests/A64Frontend/run.sh` in the three modes (90 tests), which
  includes `dcmaint` (DC only today) and the signal tests; the
  `check-code-cache.sh` SMC case; and a new `icache` test set with Pi goldens:
  same-thread patch + flush + `ISB`; patch of a block the *other* thread is
  executing, flush from the writer, executor observes new after its next
  entry; modify-own-block then `ISB` then continue; a writable-alias flush;
  and a documented "patch without flush" case that must not crash the
  emulator (either result is architecturally allowed). Real-app gate: one
  VS Code session with `POWERARM_PROFILESTATS=1` and `shmstats.py`: the `smc`
  counter per process (the fault count) should go to ~0 and `jit_count`
  should drop by the invalidated share; the HANDOVER row 10 census
  (invalidated fraction of anonymous units) re-run. An upper bound on the win
  costs nothing today: a *measurement-only* session with
  `POWERARM_SMCCHECKS=none` (unsound; it may misbehave) shows what removing
  every fault is worth before any code is written.

**Size.** Medium: a config mode, the `IC IVAU` helper and the bitmap gate,
`ISB` as a block end, byte-precise `InvalidateRange`, the mirror fan-out, the
`mprotect` rule with a per-page "was executable" bit, the mark/fault gating,
and the tests. Confidence: high that it is sound and that it removes the
fault loop; medium on magnitude until the `smc` counter has been read on a
real session, which is one env var away.

**What it retires** (all of it x86-motivated, all of it default-off, and all
of it costing a predictable branch on a hot path today): `SMCStoreEmulation`
and `SMCStoreBackpatch` (the PPC store decoder at
`SyscallsSMCTracking.cpp:146-212`, `SMCStoreBackpatch.cpp`),
`SMCLazyInval`/`Scrub`/`CrossPoke`/`Link` (the `LazySMCDirtyCount` test at
`Core.cpp:1154`), `SMCMprotectDefer`, `SMCFileImmutable`,
`SMCGranulePolicy=rearm` and `SMCGranuleMixed` (`GuestCodePageValidateOnly`
per code page per compile, `Core.cpp:936-944` -> `SMCHostGranule.h:401-410`),
the S4c arming table, and `SMCSoftInvalidate` (`Core.cpp:1179`), whose
hash-and-relink has no remaining customer once a W^X flip invalidates
nothing. Leave them until `icache` is the default and proven, then delete
them with `HandleSegfault`'s SMC half.

**If X1 is not done first**, the interim fix for the 16-per-granule arming
is one check: skip the `mprotect` at `:1287` when
`SMCGranule::Table().Armed(granule)` (`SMCHostGranule.h:434-441`; a non-zero
mask means the kernel already has `PROT_READ`, because `NoteFault` clears
the mask when it unprotects and `RematerialiseIfNeeded` honours `Armed`).
Small; medium confidence; gated by `dcmaint`, the SMC case and the three
modes. Under X1 it is moot.

### X2. The L1 lookup hash assumes any byte can start a block -- CHANGE

x86 blocks start at any byte, so `LookupCache::FindBlock` indexes the L1 by
`Address & L1PointerMask` (`LookupCache.h:876`, and the same expression in
`CacheBlockMapping` `:1243` and `InvalidateCache` `:1061`), and the inlined
probe does the same in one instruction, `rldic(TMP4, RIP, 4, L1MB)` = `(RIP
<< 4) & ((N*16)-1)` (`BranchOps.cpp:1008-1009`, `PPC64Dispatcher.cpp:354-355`).
An AArch64 PC has `PC[1:0] == 0` -- the decoder already turns any other PC
into a `BUS_ADRALN` block (`Decoder.cpp:153-166`) -- so entry indices
congruent to 1, 2 or 3 mod 4 are never written and never probed. The
128k-entry L1 (`:1328`) behaves as a 32k-entry one; of the 2 MiB that S4
prefaults per thread (`LookupCache.cpp:98-104`), 512 KiB can ever hold an
entry, and every 128-byte cache line the probe touches holds two usable
entries of eight. The L1 is per thread; an Electron app has of the order of
a hundred threads across its processes.

**Becomes:** index by `(RIP >> 2) & (N-1)`. C++: a `>> 2` in the three sites.
JIT: `rldic(TMP4, RIP, 2, L1MB)` -- still one instruction: the rotate by two
clears the two low bits, the mask clears everything above `log2(N)+4`, and for
an aligned PC the result is `((RIP >> 2) & (N-1)) * 16`. For the unaligned
PC the dispatcher can be handed on the way to its SIGBUS block, the probe
reads a 4-byte-misaligned slot and compares a value made of half a host
code pointer and half a neighbouring guest key against the PC; for that
value to equal a canonical (< 2^48) unaligned PC, either a guest code
address or a host code address would have to lie below 64 KiB, and neither
is mapped there, so an unaligned PC can only miss, which is what it does
today. The dynamic-mask leg (`:1011-1013`, `L1Mask` pre-scaled by 16)
changes `sldi 4` to `sldi 2`; the mask's zero low nibble clears bits
`[3:2]`. Keep `MAX_L1_ENTRIES` at 128k: the same 2 MiB then holds four times
the entries, which is what a 600k-block VS Code renderer wants; or halve it
twice for the same effective capacity at a quarter of the per-thread RSS
and prefault -- a choice for the measurement, not the design.

**Could break:** nothing outside the three C++ sites and two probe sites;
the `DynamicL1Cache` resize path (`:940-985`) flushes the whole table on a
resize either way. **Gate:** the three-mode suite; shmstats `cache_miss`
per thread on a VS Code session before and after (direct-mapped miss rate
scales with working set over capacity, so expect a visible drop on the
renderer and extension host); `instructions:u` on cold `gcc -c empty.c`
unchanged. **Size:** trivial. **Confidence:** high on correctness, medium-high
on effect.

The L2 has the same shape (`PageOffset = Address & 0xFFF` into a 4096-entry,
64 KiB page of which 1024 entries are reachable, `:1246-1250`,
`SIZE_PER_PAGE` `:1332`; and `CODE_SIZE` 128 MiB `:1331` is 2048 such pages
before `ClearL2Cache` at `:1260` wipes it), but `DisableL2Cache` defaults
to true (`Config.json.in:356-358`), so it costs nothing today. Apply the
same shift if it is ever turned on.

### X3. SMC Idea 4's rel32 / mov-imm semantic patcher: dead by its own TODO, still 96 bytes per block -- DELETE

`SMCSemanticPatch.h:348` says it: the x86 decoders (`DecodeRel32BranchSite`
`:366-401`, `DecodeMovImmSite` `:427-466`) have no caller since the x86
frontend went, and `GenerateIR` threads two empty site tables through
`CompileCode` and `CompileBlock` (`Core.cpp:918-921`, `:1079-1134`,
`:1327-1347`). What that costs per block: `GuestToHostMap::BlockEntry`
(`LookupCache.h:117-155`) carries four `fextl::vector`s -- `BranchImmSites`,
`ExitRIPSites`, `MovImmSites`, `MovImmWindows` -- that are always empty: 96
of its 160 bytes, copied by `insert_or_assign` on every `AddBlockMapping`
(`:194-197`) and resident for the life of the block (a VS Code renderer with
a few hundred thousand blocks carries tens of MB of zeroed vector headers in
`BlockList`). Per constant exit the backend tests `SMCSemanticPatch()`
(`JIT.cpp:831`); per constant the RA tests `IROp_Constant::PatchSite`
(`RegisterAllocationPass.cpp:128, 405, 563`) and so does `ALUOps.cpp:196`.
And the option, if anyone sets it, force-disables block linking
(`SMCSemanticPatch.h:73-83`) for a patcher that can never fire.

**Becomes:** delete the option, the x86 half of the header, `PatchSite`, the
four vectors and the plumbing; keep the PPC64 window helpers only if
something else uses `SynthesizeRIPWindow` (the code-cache relocation
re-emits the same window through the emitter, so probably not). An A64
version -- `B`/`BL` imm26 and `MOVZ`/`MOVK` patching -- is not worth writing:
V8 on arm64 patches targets through the constant pool and `BL` with a flush,
which under X1 is one line invalidated and one block recompiled.

**Gate:** it builds, the three modes, the SMC case. **Size:** small-medium,
mechanical. **Value:** memory and hygiene; performance effect small.

### X4. Re-running the faulting store as a one-instruction block -- LEAVE until X1, then dead

`HandleSegfault`'s tail (`SyscallsSMCTracking.cpp:919-994`) exists because
x86 promises that a store into the *currently executing* block's later bytes
is seen by the very next instruction, so the block must be abandoned and the
store re-executed in isolation (`CompileSingleStep`, `Core.cpp:1404-1430`, a
`MaxInst = 1` compile per redirect, plus the frame-pop dance the ppc64le
comment describes). AArch64 makes no such promise: until the thread's `ISB`
the old instructions may execute. Under mtrack today it is one extra compile
per self-page fault; under X1 the path is unreachable. Leave it, then delete
it with the handler.

### X5. What is already clean, and the residue that costs nothing

- The A64 decoder (`Decoder.cpp:134-409`) has no length decode, no resync
  and no unaligned fetch: an instruction is the aligned word at its PC
  (`:263-264`), the region walk steps by 4, and the only per-instruction
  check besides decode is the cached executable-range compare (`:260`,
  `:55-72`). `SetExternalBranches`, `DelayedDisownBuffer`,
  `IsCheapTierBlock` (`Decoder.h:121-133`) are interface stubs the x86
  `Core.cpp` contract needs. Leave.
- `FullSMCValidation`'s 16-byte `CodeOriginal` (`Core.cpp:992-995`) is the
  x86 15-byte maximum; it only runs under `SMCChecks=full` or a demoted
  granule. Leave.
- The suspect-RIP filter (`JIT.cpp:1986-2000`) is three compares per link
  miss; the `ThunkCallbackRet` stack walk behind it is inert for AArch64 by
  its own comment (`:2020-2025`) and the `0xCCCC...` test is x86 poison.
  Delete the walk for hygiene; no cost either way. `State.pc` is an
  AArch64 frame already (`CoreState.h:99-160`): the "RIP" names in
  `GuestRIP`, `RIPEntries`, `RIPFallback` are vocabulary, not layout.
- `RangeOverlapsCompiledCode` and the code-granule bitmap are keyed by byte
  range and 64-byte granule, which is the right shape for AArch64 too; X1
  reuses them. Leave.

## 2. Half two: where the time goes before a guest instruction runs, after R1/R2

The cold path for one unit, in order, with what is fixed per unit
regardless of size. `[V8]` marks costs an anonymous, never-cacheable V8
block pays; `[xul]` those a cacheable libxul block pays on a warm start.

1. **Exit -> `ExitFunctionLink`** (`JIT.cpp:1980-2075`): suspect filter;
   lazy-drain check; `FindBlock` = L1 probe, then the lookup *read* lock and
   the L3 `robin_map` (`LookupCache.h:870-937`; L2 is off); then
   `CompileBlock`.
2. **`CompileBlock`** (`Core.cpp:1136-1372`): `PreCompile` (a no-op,
   `SyscallHandler.h:99`); the shared `CodeInvalidationMutex`; `FindBlock`
   *again* (`:1170`); the `SMCSoftInvalidate` test; then `TryLoadBlock`
   (`CodeCache.cpp:1688-1830`): `LookupExecutableFileSection` =
   `VMATracking.Mutex` shared + a `std::map` descent + a section-info copy
   (`SyscallsSMCTracking.cpp:1714-1724`, `:1706-1711`), which for `[V8]`
   answers "no file" after the lock and descent; for `[xul]` the file
   registry, `Find` in each of up to eight loaded segments, `Validate`,
   `QueryGuestExecutableRange` (`VMATracking.Mutex` again, `:1894-1904`),
   page registration under the lookup write lock plus
   `MarkGuestExecutableRange` (a third `VMATracking` descent unless the
   no-op memo hits), `XXH3` over the guest bytes, `CodeBufferWriteMutex`, two
   `memcpy`s, the relocation walk (R5), two `FlushICacheRange`s, and
   `RegisterCachedBlock` (`:1374-1402`: a `CodePages` vector and
   `AddBlockMapping` under the lookup write lock).
3. **`GenerateIR`** (`Core.cpp:910-1077`): `DecodeInstructionsAtEntry`
   (`Decoder.cpp:134-409`): `EntryPoints = {PC}` and `CodePages = {page}`
   (`:139-140`: two set assignments, i.e. two node allocations and two frees
   per unit), `QueryGuestExecutableRange` for the entry (a `VMATracking`
   descent, `:56`), then per instruction the word copy, `DecodeInstruction`
   (section 3) and slot bookkeeping, the layout pass, and the predecessor
   census (`:371-395`, quadratic in a leader count capped at 8). Then
   `GuestCodePageValidateOnly` per page; `BeginFunction` (`IRBuilder.cpp:316-345`,
   one `JumpTargets` insert per block); per instruction the `_GuestOpcode`
   marker (`Core.cpp:978`) and the handler; `Finalize`; the pass manager
   (DFCE with the `HasFlags` skip, ScalarSplatChain, RA); and the raced
   `FindBlock` (`:1101-1113`).
4. **Backend `CompileCode`** (`JIT.cpp:5015-6420`): the per-thread staging
   buffer (reused, `:2383-2409`), the prepasses, emission, the vl64pair RIP
   table (`:6022-6042`, one entry per instruction), and publish under
   `CodeBufferWriteMutex` (`:6091-6195`: `memcpy`, fixups, tail, two
   `FlushICacheRange`s `:6372-6376`).
5. **Back in `CompileBlock`:** `DebugData` (`:1115`, heap, freed at the end
   of the call), symbols (off), `ClearRelocations` or `AbsorbRelocations`,
   the `CodePages` vector (`:1277-1283`), `AddBlockExecutableRange` under the
   lookup write lock per page (memoised, `LookupCache.h:790-822`) and
   `MarkGuestExecutableRange` for new pages (X1), the soft-invalidate hash
   (off), `AddBlockMapping` (lookup write lock; the 160-byte `BlockEntry`
   including a copy of `CodePages`; `CacheBlockMapping` `:1224-1245`: a
   `CachedCodePages` set insert per page and the L1 publish).
6. **`LinkAndPatch`** (`JIT.cpp:1800-1958`, once per exit edge): lookup
   write lock, re-validate, `FindBlockHeader` for the target's
   `EntryNZCVLiveIn`, `AddBlockLink`, one or two word patches each followed
   by `sync; icbi; sync; isync`, then `bctr`.

Ranked, excluding what R1-R7 and G2-G4 already own:

### L1. X1 is also the largest cold-path item for JIT code

It removes, per new JIT code page, the `VMATracking` walk and the 64K
`mprotect` (16 per granule); per write into live code, the fault, the
exclusive lock everyone waits on, the per-thread lookup scrub and `madvise`,
and the granule's recompilation; and per W^X flip, the whole-page
invalidation. For `[V8]` this is the difference between compiling a block
once and compiling it every time its neighbour is written.

### L2. X2: four times the L1 for the same 2 MiB

An L1 miss on this port is the ~62-instruction spill, two locks and the fill
(`Config.json.in:364-372`), on every exit that is not linked (calls, returns,
indirect branches -- the majority of exits in JIT'd JS code). Working sets
of hundreds of thousands of blocks against an effective 32k-entry
direct-mapped table is the wrong ratio.

### L3. One `VMATracking` descent per compile unit, not two to four -- CHANGE

Today a unit takes `VMATracking.Mutex` shared and walks the VMA map in the
decoder (`QueryGuestExecutableRange`), in `TryLoadBlock`
(`LookupExecutableFileSection`), for an install again in
`QueryGuestExecutableRange` and in `MarkGuestExecutableRange`, and for a
compile in `MarkGuestExecutableRange` per new page. Each is a `std::map`
lower-bound over every VMA in the process (Firefox and VS Code carry
thousands) and the same mutex is what every guest `mmap`/`mprotect`/`munmap`
takes exclusively -- and V8 and SpiderMonkey are `mmap`-heavy. Nothing in
the stats measures this lock: `cache_rlock_time`/`cache_wlock_time`
(`shmstats.py:29-30`) are the lookup cache's.

**Becomes:** one lookup per unit that returns `{range, prot, resource}`,
cached on the decoder's existing `ExecutableRangeBase/End`
(`Decoder.h:141-142`) and handed to `TryLoadBlock`, `RegisterCachedBlock`
and the page registration. **Effect:** one to three fewer lock-plus-descent
per unit, ~100-300 ns each uncontended, i.e. 1-5% of a ~12 us compile and
more whenever a syscall holds the mutex. **Confidence:** low-medium; the
instrumentation that decides it is an `AccumulatedVMALockTime` counter in
`ThreadStats` next to the cache lock times, printed by `shmstats.py` -- a
follow-up spec, not this document. **Size:** small-medium. **Gate:** the
three modes, `check-code-cache.sh` (its `not-exec` and `guest-mismatch`
counters must stay 0).

### L4. Six to eight heap allocations per unit -- CHANGE

`DebugData` (`Core.cpp:1115`) and its `GuestOpcodes` vector, which grows
by one entry per instruction (`JIT.cpp:3071`, `:5494`) and is freed with
the `DebugData` at the end of `CompileBlock`; `Subblocks`; the decoder's two
sets (`Decoder.cpp:139-140`); the `CodePages` vector (`Core.cpp:1277-1283`)
and its copy into `BlockEntry` (`LookupCache.h:194-197`); the
`CachedCodePages` set node per page (`:1232-1240`); `HeapEntries` for any
unit over ~120 instructions (`JIT.cpp:6027-6033`, 17 bytes per entry
against a 2048-byte stack buffer). G3 already removed the `JumpTargets`
map for the same reason.

**Becomes:** `DebugData` as a per-thread member cleared per unit; the decoder
sets as small inline vectors (one entry point, a handful of pages);
`CodePages` as a one-inline-element small vector inside `BlockEntry`.
**Effect:** ~0.3-0.6 us per unit at jemalloc's ~50-80 ns per operation, 3-5%
of a compile. **Confidence:** medium. **Size:** small. **Gate:** G3's:
`perf stat -e instructions:u` on cold `gcc -c empty.c` (1,295 M today), the
three modes.

### L5. One IR node per guest instruction that every pass walks -- CHANGE, medium

`Core.cpp:978` emits `_GuestOpcode` before every instruction. It is a real
IR node: DFCE, `CompareBranchFusion` (`:231`), the RA (`:777`) and six
backend loops (`JIT.cpp:3070`, `:3772`, `:4236`, `:4292`, `:4506`, `:4828`,
`:5339`) each test and skip it, and the vl64pair encoder re-walks the list
it produced. At three to six IR ops per AArch64 instruction the markers
are 15-20% of the nodes in every walk. On a fixed-length guest the marker
carries one bit of information -- "an instruction boundary starts here";
the guest delta is +4 inside a block and only changes at block edges,
which the block header already records.

**Becomes:** the boundary as a flag on the first IR op of each instruction
(or a per-unit side vector of `(first SSA id, guest offset)` the backend
consults as it emits); the RIP table format and `RestoreRIPFromHostPC`
(`Core.cpp:609-652`) unchanged. **Effect:** 2-4% of compile (round 1 measured
`Op_GuestOpcode` plus its re-walk at ~1.2% before counting the passes'
skips). **Confidence:** medium-low. **Size:** medium (IR.json, frontend,
every pass's skip case, the backend). **Gate:** `sigpreempt`, `sigedit`,
`sigqueued` (signal-resume PC must stay instruction-granular), the three
modes, `instructions:u`.

### L6. Not levers, and why

- **First-touch faults on the code buffer.** 1 GiB is reserved up front
  (`CPUBackend.cpp:760-779`), THP-hinted (warm G7); with 2 MiB huge pages
  that is one fault per 2 MiB of emitted code -- ~190 for VS Code's 376 MB of
  host code per process, ~5 for `cc1`. Prefaulting a chunk would trade a
  few hundred microseconds at startup for the same at first use.
- **The staging buffer** is per thread and reused (`JIT.cpp:2383-2409`).
- **Block-link patching** is once per exit edge and G3 already made it one
  lock hold; the two `icbi` sequences are the price of a link that then runs
  as a single `b`.
- **The dispatcher round trip** has no x86 residue that executes.
- **RA's per-unit resizes** (`RegisterAllocationPass.cpp:632-634`, and
  `NextUses.assign` per spilling region at `:305`) are round 1 §4.1, not
  AArch64-specific; G6's regions changed the walk, not the resets.
- **The relocation walk per install** is R5.

## 3. Decode and dispatch: how close to a pure table lookup

### 3.1 What it is today

`DecodeTable.cpp:28-30` computes `((W >> 10) & 0xF) | ((W >> 18) & 0xFF0)`,
which is word bits `[29:22]` and `[13:10]` -- twelve bits, 4096 buckets. The
comment on `:27` says "op1 bits [31:22]"; the arithmetic dropped bits 31
(`sf`) and 30 (the opc bit that separates `ADD` from `SUB`, `MOVN` from
`MOVZ`, `ADR` from `ADRP`). The bucket is scanned linearly in priority order
(`:133-141`), sixteen bytes per entry (`Mask`, `Expect`, matcher pointer),
and the winner's `Handler` is a pointer-to-member on `IRBuilder` called once
per instruction (`IRBuilder.cpp:492-502`). The decoder calls
`DecodeInstruction` once per decoded instruction and stores the matcher in
`DecodedInst` (`Decoder.cpp:269-274`), so the IR builder never re-decodes.

Static facts from parsing `a64.inc` the way `BuildTable` does (774 active
`INST` entries, 101 commented out; 736 handler registrations in
`IRBuilder.cpp:35-280`, ~740 entries with a handler): 2559 of the 4096
buckets are non-empty; 6461 bucket entries in total (101 KiB); non-empty
buckets hold 2.52 entries on average, median 2, maximum 31 (bucket `0x780`,
the FP data-processing 1-source cluster: `FMOV`, `FABS`, `FNEG`, `FSQRT`, the
`FRINT*` and `FCVT*` forms); 583 of the 774 matchers are reachable from more
than one bucket (mean 8.3 copies; `B_uncond` from 256 buckets, because it
fixes only bits `[30:26]`).

### 3.2 What real code does to it

Classifying every `.text` word of the guest binaries on this box through the
same table (static, so it is the instruction *mix of the file*, not of a run;
the two agree well enough for a decoder question):

| Binary | words | matched | E[scan depth] | depth 1 | depth <= 2 | depth <= 4 | max |
|---|---|---|---|---|---|---|---|
| VS Code `code` (Electron) | 38.8 M | 99.94% | **1.94** | 63.9% | 81.0% | 92.5% | 30 |
| Firefox `libxul.so` | 28.8 M | 100.00% | **1.74** | 65.2% | 84.5% | 96.1% | 30 |
| Factorio | 10.4 M | 99.78% | **2.02** | 60.8% | 78.3% | 94.3% | 31 |
| Claude CLI 2.1.280 | 15.9 M | 97.2% | **1.95** | 63.0% | 80.0% | 93.8% | 27 |
| libc.so.6 / libstdc++.so.6 | 0.29 / 0.39 M | 99.9 / 100% | 1.73 / 1.64 | 63 / 69% | 85 / 87% | 95 / 97% | 27 |

The unmatched remainder is literal pools and unallocated words; the Claude
binary's 2.8% is Bun's embedded data. Five matchers hit by VS Code and 18 by
Factorio have no handler (0.001-0.004% of words; feature probes, per
HANDOVER).

The deep-but-frequent entries, and why they are deep:

- `MOVZ` (3.6-6.7% of words) at depth 2 behind `MOVN`; `SUBS_imm` behind
  `ADDS_imm`; `SUB_imm` behind `ADD_imm`; `ADRP` (2-8%) behind `ADR`: bit 30
  (or 31) is the discriminator and is not in the index.
- `BL` (5-7%) at depth 2-13 and `B_uncond` (3-4%) at depth 1-12: in bucket
  `0x54d` they sit behind `IC IALLU`, `CLREX`, `DSB`, `DMB`, `ISB`, `DC IVAC`,
  `DC ISW`, `DC ZVA`, `IC IVAU`, `MSR` -- the system-instruction cluster has
  more fixed bits and sorts first, and shares the bucket because `B` fixes
  none of the bits that would separate them.
- `B_cond` (5-6%) at depth 3 behind `SVC` and `BRK`; `SUBS_shift` (2-4%) at
  depth 4 behind the extended-register forms; `STURx_LDURx` (2.7%) at depth
  10; `HINT` (2.8% in VS Code -- BTI landing pads) at depth 7 behind
  `NOP`/`YIELD`/`WFE`/`WFI`/`SEV`/`SEVL`, all of which map to the same
  handler.

### 3.3 What decode costs, in proportion

Per instruction today: three ALU ops for the index, two loads for the bucket
bounds, then 1.7-2.0 iterations of a 16-byte load, `and`, `cmp`, branch --
of the order of 10-15 ns, against the ~1,000 host cycles per guest
instruction the round-1 profile puts on translation as a whole
(`COLD-TRANSLATION-RESEARCH.md` §3). Decode is ~0.5% of translation. The
handler dispatch is one indirect call through a PMF; it is not where the
frontend's 15% goes -- the handler bodies (IR construction) are. So the
honest framing of Jordan's question is: decode is already within one
compare of a table lookup, and the items below are worth doing because they
are cheap and remove a comment/arithmetic mismatch, not because they move
the cold number.

### 3.4 The three questions

**(1) The table built at every process start.** `BuildTable`
(`DecodeTable.cpp:68-125`) does, per process: 774 32-character parses; 774
`FindHandler` binary searches over 736 names (`IRBuilder.cpp:283-302`, itself
built once per process by a stable sort of the handler table); a
`stable_sort` by mask popcount (`:97`); a `stable_partition` that compares
every entry's description string against three literals (`:103-109`); then
6461 bucket visits to count and 6461 to fill (`:113`, `:122`); and it
allocates `Matchers` (774 x 32 B), `BucketEntries` (6461 x 16 B) and two
16 KiB arrays -- ~150 KiB of private heap, of the order of 100 us. Forked
children inherit it; exec'd ones rebuild: a 1000-TU build is ~4000 execs,
~0.4 s over minutes; an Electron session is a few tens of exec'd helpers.
The time is not the reason to do it; the shared pages and determinism are.
**Verdict: do it**, as a generated source (the tree already generates from
`IR.json`; a C++20 `constexpr` build is also possible -- `std::sort` is
constexpr, `stable_sort` is not, so write the insertion sort) that emits the
sorted matcher array and the CSR buckets into `.rodata` using *indices*
(handler index, name offset) rather than pointers, so the pages need no
dynamic relocation and stay shared across every guest process on the box;
the 736-entry PMF array stays a separate, tiny, relocated table. Small.

**(2) The in-bucket scan.** Expected scan depth over the real words for the
candidate indices, and for a frequency-aware bucket order (stable; an entry
only moves ahead of another when their `(Mask, Expect)` pairs are disjoint,
so "more fixed bits first" is preserved exactly where two entries could both
match a word):

| Index | bits | buckets | entries (KiB) | E[depth] code / xul / factorio / libc | depth 1 |
|---|---|---|---|---|---|
| current `[29:22]+[13:10]` | 12 | 4096 | 6461 (101) | 1.94 / 1.74 / 2.02 / 1.73 | 61-65% |
| **`[31:22]+[13:10]`** (what the comment says) | 14 | 16384 | 12292 (192) | 1.32 / -- / 1.25 / 1.14 | 88-91% |
| greedy 14 `[31:23,21,15,13,12,10]` | 14 | 16384 | 10301 (160) | 1.31 / -- / 1.21 / 1.12 | 90-94% |
| `[31:22]+[15:10]` | 16 | 65536 | 39537 (617) | 1.24 / -- / 1.14 / 1.11 | 90-91% |
| greedy 16 | 16 | 65536 | 32378 (505) | 1.17 / -- / 1.05 / 1.02 | 97-99% |
| current + frequency order | 12 | 4096 | 6461 (101) | 1.44 / -- / 1.26 / 1.27 | 80-83% |
| **greedy 14 + frequency order** | 14 | 16384 | 10301 (160) | **1.17 / -- / 1.03 / 1.02** | 95-98% |

(libxul was run only under the current index; the greedy selections
minimise the sum of squared bucket sizes over the table.) Two things fall
out. Restoring bits 31:30 -- `0xFF0` to `0x3FF0` and a 16385-entry
`BucketStart` -- halves the expected depth by itself for 91 KiB. A wider
index costs table size exponentially in the free index bits, because an
entry is copied into every bucket it can be reached from: 16-17 bits is
half a megabyte to a megabyte of L2-resident table for a few percent of
depth. The second-level discriminator that does make the common bucket size
1 is not more bits but the order: frequency-ordered buckets on the 14-bit
index give E[depth] 1.02-1.17 and a maximum of 21, at 160 KiB. The order
is derived from these same static counts at build time; no runtime
profiling. **Verdict: do it**, bundled with (1): 14-bit index, disjoint-aware
frequency order, constant table. Expected effect on translation time
<= 0.5%; the gate is the three-mode suite plus a static self-check, free in
Python, that the new order returns the same matcher as the old one for
every one of the ~10 M distinct words in the six binaries.

**(3) Memoising decode.** Distinct words are 9-14% of all words (VS Code:
4.17 M distinct of 38.8 M; the top 4096 words cover 55%, the top 65536 78%).
A direct-mapped word -> matcher cache over the static stream hits 34-37%
at 256 entries, 47-52% at 1k, 58-63% at 4k, 68-72% at 16k (a multiplicative
hash; the sequential stream is an optimistic proxy for the region-ordered
decode). A hit saves one bucket scan -- 1.7-2.0 compares, 5-10 ns -- and
costs a hash, a load and a compare (3-5 ns) plus 4-256 KiB competing with
the bucket table for L1d/L2. Net a few nanoseconds per instruction at best,
and after (2) the scan it replaces is one compare 95% of the time. The
decoder already memoises within a unit (`DecodedInst::Matcher`). **Verdict:
not worth doing.**

### 3.5 Where the table-driven idea stops paying

It stops at `TranslateInstruction`. Everything after the handler call --
IR construction, DFCE, splat chains, compare-branch fusion, RA, the backend
prepasses, emission -- is 95%+ of the cold cost, and it *is* the
cross-instruction machinery: G2's fusion of `CMP`/`SUBS` into `B.cond` and
the `CSEL` family (`CompareBranchFusion.cpp`), N1's vector-scan idiom, the
frontend GPR value cache carried across single-predecessor edges
(`4814f2da3`), RA regions across chains of blocks (`694af3345`), DFCE's
dead-flag elimination across instructions, `Mask32Tail` elision, the alias
folds. A per-opcode host stub or threaded-code translator would keep the
decode and replace all of that with a fixed expansion per instruction: cold
cost per instruction down perhaps 3-5x, steady-state code 2-3x slower and
never re-tiered, plus the tier-swap surface -- which is the cheap tier
HANDOVER row 10 closed on the census evidence (12-33% of translation is
anonymous code, 88% of Firefox's hitch window was cacheable libxul). The
numbers in this section sharpen that: decode is ~0.5% of translation, so
there is no middle ground between "make decode a table lookup" (worth a
day) and "make translation a table lookup" (the closed tier). X1 removes
most of the *re*-translation of anonymous code, which was the only argument
the tier ever had.

## 4. Ranking across the three sections

| # | Item | Kind | Effect | Size | Confidence |
|---|---|---|---|---|---|
| 1 | X1 / L1: `SMCChecks=icache` -- invalidate on `IC IVAU`, `ISB` ends the block, no mtrack arming | change | the V8/JSC fault-invalidate-recompile loop and the W^X flip cost; 16 `mprotect`s per JIT granule; every SMC option retired | medium | high (sound), medium (magnitude: read `smc` on one VS Code session first) |
| 2 | X2 / L2: L1 hash by `RIP >> 2` | change | 4x effective L1 per thread for the same 2 MiB; fewer spill/fill misses on JIT'd JS | trivial | high / medium-high |
| 3 | L4: per-unit heap traffic | change | 3-5% of a compile | small | medium |
| 4 | Section 3 (1)+(2): constant decode table, 14-bit index, frequency order | change | ~100 us and ~150 KiB per exec; decode one compare 95% of the time; <= 0.5% of translation | small | high (static), low on wall time |
| 5 | L3: one `VMATracking` descent per unit | change | 1-5% of a compile, more under `mmap` churn | small-medium | low-medium; needs the lock-time counter |
| 6 | X3: delete SMC Idea 4 residue | delete | 96 B per block, a few branches per constant; hygiene | small-medium | high |
| 7 | L5: `GuestOpcode` markers out of the IR | change | 2-4% of a compile | medium | medium-low |
| 8 | X4, X5, the option zoo, the suspect walk | leave / delete later | none measurable | -- | -- |
| -- | Section 3 (3): decode memo | no | none | -- | high |
| -- | L6 items | no | none | -- | -- |

## 5. Gates

| Item | Gate |
|---|---|
| X1 | three-mode `unittests/A64Frontend/run.sh` (incl. `dcmaint`, `sigpreempt`, `sigedit`); `check-code-cache.sh` SMC case; new `icache` tests (same-thread, cross-thread, own-block + `ISB`, alias flush, no-flush must-not-crash) with Pi goldens; a64diff 64k and 4k-kvm; VS Code `shmstats` `smc` ~0 and `jit_count` down; the row-10 census re-run |
| X2 | three modes; `cache_miss` per thread before/after on VS Code; `instructions:u` on cold `gcc -c empty.c` unchanged |
| X3 | build; three modes; SMC case |
| L3 | three modes; `check-code-cache.sh` counters (`not-exec`, `guest-mismatch` stay 0); the new VMA lock-time counter as the measurement |
| L4 | `perf stat -e instructions:u`, cold `gcc -c empty.c` (1,295 M baseline); three modes |
| L5 | `sigpreempt`, `sigedit`, `sigqueued`; three modes; `instructions:u` |
| Section 3 | three modes; the static same-winner check over the six binaries' distinct words; `POWERARM_STARTUPTIMES=1` gains a `decodetable` phase if anyone wants the microseconds |

## 6. Follow-up specs for an implementation agent (the measurements this document could not take)

- **M-A, the ceiling of X1 with no code:** one VS Code session (and one
  `claude` session) with `POWERARM_PROFILESTATS=1` and `shmstats.py`, then
  the same with `POWERARM_SMCCHECKS=none` -- unsound, measurement only. The
  `smc` counter of the first is the fault count X1 removes; the `jit_count`
  difference between the two is the recompilation it removes.
- **M-B, X2:** `cache_miss` per thread from the same sessions, before and
  after the shift.
- **M-C, L3:** add `AccumulatedVMALockTime` to `ThreadStats`, print it in
  `shmstats.py`, read it on VS Code and Firefox.
- **M-D, L4/L5/section 3:** `instructions:u` on cold `gcc -c empty.c`, one
  run per item, no wall-time claims (HANDOVER "Traps": layout noise).
