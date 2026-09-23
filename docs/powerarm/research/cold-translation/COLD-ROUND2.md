# Cold, round 2: what is left after G1-G5

Design research, 2026-09-22. No code, no builds, no runs. Companion to
`COLD-TRANSLATION-RESEARCH.md` (round 1), `CODE-CACHE.md` and HANDOVER's
"Next optimizations" rows 6-10. Every citation was re-read in the tree at
`b4e5f3dcd`; the cache-directory numbers were taken from the live
`~/.cache/powerarm/cache` at 18:33 on 2026-09-22, read-only.

**One-paragraph answer.** Firefox translates libxul while browsing for three
reasons, in this order of evidence: (1) it has no cache to hit -- on this
machine right now there is no Firefox namespace at all, the directory sits at
1829 MB of a 2048 MiB cap with two emulator builds' namespaces competing for
it, and every promote (two today) gives every app a fresh ConfigId; (2) the
cache loses blocks that multi-process apps compile: a save whose namespace lock
is busy drops its blocks for good (`CodeCache.cpp:2273-2298`, `:2326-2348`),
and once a namespace has eight segments every periodic pass needs the
exclusive lock for a whole-namespace compaction, which is where VS Code's
726 MB `code` namespace lives today; (3) genuine first touch, which only a
warm-up run or a survived cache can pre-pay. Separately, the periodic save
itself runs on a guest thread inside an mmap/mprotect syscall, holding the
code-buffer lock across the block walk and doing the compaction and the
directory sweep synchronously: a hitch of its own, ~150 ms per 50k blocks and
of the order of a second per compaction, on every app that keeps translating.
G4 already proved the fix shape (fork the writer); it covers only the exit
path. The VS Code cache is not duplicated across its processes (0.2%
duplicates); it is 600k unique blocks of which 43% of the bytes are 48-byte
relocation records. AOT at install is not the answer for these apps (Bun is
stripped, `entries` bought nothing on `cc1`, libxul has ~274k FDEs); a
warm-up run after every promote is. Lazy per-block install is still right. A
cheaper G5 exists -- soft-invalidation at the W^X `PROT_EXEC` flip, which no
current option reaches -- but it is fourth, not first.

## 1. Why cacheable text is translated during a browsing session

### 1.1 The cache directory, now

`~/.cache/powerarm/cache` at 18:33, 2026-09-22 (read-only `ls`/`du`; the
namespace parser is in the session scratchpad, not the tree):

| Fact | Value |
|---|---|
| Size / cap | **1829 MB** of `CodeCacheMaxSize` 2048 MiB (`Config.json.in:59-67`); the sweep trims to 90% = 1843 MB, and `.sweep` was stamped 18:32:03, i.e. sweeps run every minute now |
| ConfigIds present | two: `1819329788bf0c78` (1351 MB, 423 segment files) and `932f48bcb1941dee` (476 MB, 122), both written to between 18:28 and 18:34 |
| Stable promoted | 14:37:02 today; `.prev` from 13:47:29 today |
| Oldest namespace | 14:15 today: everything older has been evicted or swept |
| Firefox / libxul namespaces | **none** (`ls | grep -ic 'xul\|firefox\|mozilla'` = 0) |
| Factorio namespace | none |
| Largest namespaces | `code` (VS Code) 726 MB, `2.1.280` (Claude CLI, old ConfigId) 258 MB, `claude` 200 MB, `agy` 170 MB + 76 MB |

Reading it. The second ConfigId is the previous stable: long-lived guests keep
the emulator they started with (HANDOVER "Traps"), so the Claude and agy
sessions started before the 14:37 promote are still writing 476 MB of cache
nobody new can read. The sweep removes another build's namespaces only after
an hour unused (`CodeCache.cpp:866`, `:2070-2078`), but the size
eviction is by mtime across builds (`:2090-2100`), so the dead build's
namespaces and the live build's compete on equal terms. Firefox is 100% cold
on its next launch on this machine: libxul is cacheable (64K-congruent,
`p_offset 0x30b18e0` / `p_vaddr 0x30c18e0`, `p_align 0x10000`; 115.2 MB of
`.text`), it just is not there.

The G5 census (HANDOVER row 10, line 200) says 88% of the translation in a
Firefox hitch window was libxul text. It was taken during the G2 work on a
build of that day. Every build is a new ConfigId (`CodeCache.cpp:355-376`
hashes `GIT_HASH` and the executable's build id), so unless that census was
the *second* Firefox run on that exact binary, the libxul it translated had no
cache to hit. The row does not say which; `POWERARM_CODECACHESTATS=1` would
have (section 4). "Cold translation of its own code" is the right diagnosis;
the missing half is *why it was cold*, and the directory says: because it
always is, on a machine that promotes twice a day.

### 1.2 Mechanisms in the code that lose or delay blocks for a multi-process app

These are read from the code, not measured. Each is specific to an app that
runs many processes for a long time, which is the shape the round-1 design
never had to face (it was built for a build's hundreds of short processes).

**(a) A busy namespace lock drops the pass's blocks for good.**
`SaveNewBlocks` marks every candidate record `KeepRecord = false`
(`CodeCache.cpp:2201`) and only `Defer()` (`:2213-2218`) sets it back, on the
two too-few-blocks paths (`:2221`, `:2259`). The publish takes
`flock(LOCK_SH|LOCK_NB)` and, when all eight names are taken,
`flock(LOCK_EX|LOCK_NB)` for a compaction (`:2280-2296`). If either fails
nothing re-keeps the records, the temp file is unlinked (`:2298`) and the
records are replaced by the kept set (`:2326-2348`). The comment at
`:2276-2279` says the segment "is simply deferred ... and written by a later
pass"; it is not. On a Final pass (exit) there is no later pass either. When
does the lock fail? `LOCK_SH` fails only against a compaction or a namespace
removal; `LOCK_EX` fails against any sibling's `LOCK_SH` or `LOCK_EX`. With
eight segments present, every process's periodic pass wants `LOCK_EX`, so two
processes saving in the same minute means one of them loses its blocks.

**(b) Eight segments, then a compaction per pass.** `MaxSegments = 8`
(`:852`). A periodic pass writes one segment per file per process
(`:2280-2287`); a process with more than seven new blocks in a file saves
every 60 s or 50k blocks (`:1257-1258`, `:455-460` of `Syscalls.h`). So a
multi-process app fills eight names within minutes and every later pass by
any process compacts: `CompactSegments` (`:1821-1900`) opens every segment,
re-hashes every block (`:1867`), and rewrites the whole namespace through a
`BufferedWriter`. For `code` that is 607,597 blocks and 726 MB rewritten,
synchronously, in the guest process that happened to hit its 60 s trigger.
Today `code`, `2.1.280` (old ConfigId) and two more are at eight segments.

**(c) The periodic save runs on a guest thread, inside a syscall, under the
code-buffer lock.** `MaybeSaveCodeCaches` fires from `FinishTrackedMmap`
(`SyscallsSMCTracking.cpp:2339`), `GuestMunmap` (`:2435`) and
`FinishTrackedMprotect` (`:2755`, also reached from the sub-granule path
`GranuleMemory.cpp:886`). `SaveCodeCaches` (`:2114-2149`) then runs
`SaveNewBlocks` on that thread under `SaveIOLock`; `CollectLiveBlocks` holds
`CodeBufferWriteMutex` and the lookup-cache read lock for the whole
serialisation walk (`CodeCache.cpp:1593-1596`), so every other thread's
publish (`JIT.cpp` staging publish, G2) and every install (`TryLoadBlock`
takes the same mutex, `:1485`) waits. The measured save cost is ~3 us per
block: code-server `save-ms 259` for 86,896 blocks (round 1 §7, line 402) and
Claude 79 ms for 25.6k (`CODE-CACHE.md:362`). A 50k-block pass is therefore
~150 ms on the calling thread, plus the compaction in (b), plus the directory
sweep (`:2315`; a `readdir` of 783 files, an `fstatat` each and a header
`pread` per namespace, `:2011-2101`). For a SpiderMonkey content process the
calling thread is the JS thread, because its W^X `mprotect`s are the syscalls
that reach `FinishTrackedMprotect`. G4 moved only the *exit* save into a
forked child (`FEXInterpreter.cpp:914-925`, `Thread.cpp:867-890`); the
periodic and unmap passes still run inline.

**(d) A signal-killed process saves nothing** (`WantsSave`'s own comment,
`CodeCache.cpp:1251-1258`; HANDOVER item 10, line 283). Firefox's parent kills
a content process that misses its shutdown timeout with SIGKILL, and a crash
saves nothing. With G4 the exit save is a separate pid, so a normal exit is
safe from the parent's timeout; a hard kill is not.

**(e) A fork child forgets the parent's unsaved compiles**
(`ResetAfterFork`, `:1290-1310`; `CODE-CACHE.md:173`). Firefox's content
processes are forked from a fork server (HANDOVER item 21, line 376-379);
what the fork server compiled after its last pass is compiled again by every
child and saved by none of them until a child's own pass. Minor.

**(f) Evicting a namespace an app is using loses its future saves.** Readers
take no lock; a running process keeps its (now unlinked) segments mapped, and
`FileCache::Contains` still answers "on disk" for their blocks (`:1188-1195`),
so that process never re-saves them. Edge case, but it is the LRU's normal
behaviour when a dev build's test run pushes a running Firefox out.

**(g) Not a cause: code-buffer rotation.** `CodeBufferMaxSize` defaults to
1024 MiB and, with the cache on, is allocated up front so no growth step ever
discards code (`Config.json.in:19-29`, `CPUBackend.cpp:760-779`). The "128 MiB"
in the comment at `JIT.cpp:1758` is stale. `FEX_BUFSTATS=<file>`
(`CPUBackend.cpp:603-611`) logs every rotation with a timestamp; expect none
for Firefox.

**(h) Not a cause: rejected loads.** `guest-mismatch`, `not-exec` (a demoted
mixed granule, `:1446-1462`) and `reloc-failed` are counted separately in the
stats line (`:1311-1330`); nothing in the round-1 or G1 measurements showed
them non-zero on file-backed code (`reloc-failed` 0 on `cc1`,
`CODE-CACHE.md` C9). Firefox has not been measured; the stats line is the
check.

### 1.3 What the VS Code cache actually contains

The orchestrator's question: does each of VS Code's ~14 processes save its
own copy? **No.** Parsing the `code-c12ca674f10168bb-1819329788bf0c78`
namespace (header and index formats at `CodeCache.cpp:868-945`):

| | |
|---|---|
| Segments | 8 (base 720 MB compacted, plus seven appends of 12 to 2,802 blocks); a compaction ran between two of my reads, 602,663 -> 607,597 base blocks, 720 -> 726 MB |
| Blocks | 605,990 total, **604,598 unique**, 1,392 duplicates (0.2%) |
| Guest bytes covered | 41.9 MB of the binary's 155.2 MB `.text` (27%) |
| Host code | 376 MB: **8.97x** the guest bytes; 652 host bytes per block for 73 guest bytes (18 instructions) |
| Relocations | 6,888,750 = **11.3 per block**, 48 bytes each = 315 MB, **43% of the file**; the index is 32 MB |

Dedup works: a pass skips blocks any on-disk segment holds (`File->Contains`,
`:2227-2256`) and a compaction keeps one entry per guest offset
(`:1853-1855`). The uncompacted Claude namespace shows the pre-compaction
duplication: 8.1% across seven appends. The 1.12 GB figure in HANDOVER
(line 213) was that namespace before its compaction, or two of them. The
other namespaces agree: Claude CLI 199,542 blocks, 133 MB code, 12.5 relocs
per block, 44% of the file; `claude` 152,038 / 104 MB / 12.8 / 44%; `agy`
135,805 / 86 MB / 12.4 / 45%. Two consequences: the cap is undersized for the
owner's app set by 2-3x (one build's four apps are 1.35 GB before Firefox is
added), and the relocation record (`RelocGuestRIP`, `Relocations.h:70-88`:
a 12-byte header, 4 bytes of payload, an 8-byte RIP and 24 bytes of `pad2`)
is half padding.

### 1.4 The install side, and Factorio's 38 s

Lazy per-block install is still right for a 150k-block app. The arithmetic:
`lookup-ms` gives 2.9 us per installed block (round 1 §4.7 item 5, line
328-333) and the compile side ~12 us (G2 commit message); 150k installs are
0.45 s spread over a run, against C12's eager install of 8.7M blocks for
6.6M used (`CODE-CACHE.md:264-270`) and translate-ahead's lost races. The
install path's remaining cost is per block (rwlock, memcpy, registration:
`CODE-CACHE.md:519-522`) and the relocation walk over 11-13 records per block
(`ApplyCodeRelocationsSplit`, `:2485`), which section 2 R5 shrinks.

Factorio's warm launch at 38 s against 27 s cold (HANDOVER item 24, lines
422-423) cannot be install cost: even 300k blocks at 2.9 us is under a
second. Its binary is cacheable (64K-congruent: `p_offset 0x116889c`,
`p_vaddr 0x117889c`), 41.7 MB of `.text`, unstripped. The candidates the code
offers are (b) above -- a several-minute cold benchmark writes five or six
periodic segments, the warm run's own passes fill the eighth and every later
pass compacts a ~300 MB namespace inline -- and (c), periodic saves landing on
its loader threads. Neither reaches 11 s on paper; that number was one run
each on a box the head-to-head note calls contended. It is not a lever until
one instrumented launch (section 4, M3) says what it is.

## 2. Ranked: what is left on cold

Ranked by expected effect on the owner's real workloads, discounted by
confidence, effort and risk. "Builds" means the zlib/Lua builds and the slice;
none of these items moves them (their processes are short and their exit save
is already forked; cold slice is within 0.04 s of warm, HANDOVER row 7).

### R1. Periodic and unmap saves off the guest thread, and no lost blocks

**What.** Extend G4's pattern to the periodic and unmap passes. Cheapest
first step, same shape as G4's child: the guest thread still builds the
`SegmentBuilder` (the `CollectLiveBlocks` walk under the locks, ~1 us per
block), then forks; the child does the temp write, the `flock`s, the
compaction and the sweep, and may *block* on the lock instead of dropping
(`:2276-2296`), which also fixes 1.2(a). Second step, if the walk itself shows
up: fork before the walk under the existing `LockBeforeFork` discipline
(`Core.cpp:837-846` takes `CodeInvalidationMutex` and `SaveIOLock`;
`ForkableUniqueMutex`, `SignalScopeGuards.h:44`) and let the child walk its
copy-on-write snapshot with no lock held in the parent at all. Either way,
fix the comment at `:2276-2279` and re-keep records on `!Written` for the
in-process path that remains (a fork that fails).

**Evidence.** 1.2(a)-(c); the measured 3 us/block save cost; the live
directory (four namespaces at eight segments; `code` at 726 MB); G4's result
that a forked writer works and outlives `exit_group` (HANDOVER row 7, C11's
premise overturned); `check-code-cache-contend.sh`'s history (issue #1: the
save path already once starved an invalidation for 4 s).

**Expected effect.** Firefox browsing, VS Code, Claude sessions, Factorio
in-game: removes a ~150 ms stall per 50k compiled blocks and a compaction of
the order of a second (726 MB rewritten) from live app threads, and stops the
lock-busy loss so blocks compiled in one session are there for the next. No
effect on launch time, code-server start or builds. Magnitude on Firefox is
unmeasured (section 4, M1 gives `save-ms` and `compactions` per process).

**Risk.** Medium. A fork inside a live multi-threaded guest at a syscall
point: the child must only write. HANDOVER item 21 open (a) -- compaction
SIGSEGV in a forked child's pre-exec save (`SaveNewBlocks -> CompactSegments
-> CacheSegment::Open`) -- is this exact path and must be reproduced and fixed
first; it is the gate, not a side issue. Chromium's single-thread sandbox check
is on `/proc/self/task` (G2 commit), so a child *process* is invisible to it;
guests under `RLIMIT_AS` fork at the parent's size, so the `rlimit_as` and
`forkexec` tests are gates. Fork cost is ~1.4 ms for a large process (C17).

**Effort.** Small-medium (a day for the first step, given G4).

**Gate.** `check-code-cache.sh` and `check-code-cache-contend.sh`; a new
case: two writers against a namespace with all eight names taken while a
third holds `LOCK_EX`, and every block appears in a later segment; the
`rlimit_as`/`forkexec`/`hostfault` tests; the three-mode A64Frontend suite;
one VS Code session with `POWERARM_CODECACHESTATS=1` showing `saved` close to
the compiled count and `save-ms` gone from the parent.

### R2. Size the cap for the app set, and evict dead builds first

**What.** `CodeCacheMaxSize` 2048 -> 8192 MiB (`Config.json.in:59-67`);
in `SweepCacheDirectory` (`CodeCache.cpp:2090-2100`) evict other builds'
namespaces before any of this build's, whatever their mtime; make the test
harnesses that write to the default directory (A64Frontend's three modes
write 175 MB under six ConfigIds, `CODE-CACHE.md:340-344`) use a private
`POWERARM_APP_CACHE_LOCATION` like `check-code-cache.sh` does.

**Evidence.** 1.1: 1829 MB of 2048, sweeps every minute, no Firefox
namespace, one build's four apps at 1.35 GB, a dead build's 476 MB
competing on mtime. Adding Firefox (libxul at VS Code's density: a quarter of
115 MB of text touched is ~29 MB of guest bytes, ~400k blocks, ~300 MB of
cache) cannot fit beside VS Code and Claude under this cap.

**Expected effect.** Prevents whole-app cold sessions: for Firefox that is
the entire libxul translation (~400k blocks x 12 us = ~5 s of compile time,
delivered as hitches across the session, extrapolated from VS Code's block
density); for VS Code its 600k blocks. No effect on a session that was going
to be warm anyway, none on builds.

**Risk.** None. Disk: the directory is already 1.8 GB; 8 GiB is an upper
bound the LRU keeps it under.

**Effort.** Trivial (a default, a sort key, a few env lines).

**Gate.** `check-code-cache.sh`'s 20 MiB cap case still passes; a directory
with two builds' namespaces evicts the other build's first.

### R3. Warm-up after promote and at install (G1(c) in its warm-up form)

**What.** `CODE-CACHE.md:427-449` already specifies it and it is still not
built: a registered warm-up per app (`claude --version`; code-server to
`/healthz` then SIGTERM; `firefox --headless --screenshot` of a real page;
VS Code `--version` or a headless start; Factorio `--version` or a benchmark
save load), run niced in the background by `promote-powerarm-stable.sh`
(`~/Development/promote-powerarm-stable.sh`, which today only copies and
prints) after `register-powerarm-binfmt.sh`, and by the port's package hook
at app install/update. sleeve already owns per-app records
(`~/.config/powerarm/sleeve/apps/*.json`, HANDOVER item 25) and is the natural
place for the warm-up command.

**Evidence.** Every promote is a new ConfigId (`CodeCache.cpp:355-376`),
two promotes today; the measured warm numbers are the ceiling: code-server
first response 3.88 -> 2.19 s and `claude --version` 576 -> 90 ms
(`CODE-CACHE.md:354-358`). AOT is ruled out for these apps by evidence, not
taste: Bun ships stripped and `entries` seeded 136 of 25.5k blocks
(`CODE-CACHE.md:429-433`); `cc1` in mode `entries` bought nothing on the slice
(`:506-507`, Q4-A1) and is the closest analogue to libxul, which has ~274k
`.eh_frame_hdr` entries (2.19 MB) and a 67 KB `.dynsym`, so mode `entries`
would cost ~5x `cc1`'s 87 MiB for the same nothing; mode `all` was 1.2 GiB
for `cc1` alone.

**Expected effect.** App launch after a promote or update: code-server
-44%, Claude -84% on `--version`, VS Code and Firefox startup share
unmeasured. Firefox *browsing* beyond what the warm-up page exercises stays
first-touch cold; the warm-up cannot reach the GPU process, media or the
sites the owner visits. This is why R1 and R2 rank above it: they keep what a
real session compiled.

**Risk.** Low. A warm-up that crashes must not block the promote; a
SIGKILLed warm-up saves nothing (1.2(d)), so stop with SIGTERM.

**Effort.** Small (scripts, a sleeve record field).

**Gate.** After a promote plus warm-ups, the first launch of each app prints
`no-file 0` and `loaded` far above `not-in-index` in its stats line; the
code-server and `claude --version` first launches match their warm numbers.

### R4. Soft-invalidation at the W^X flip (the cheaper G5)

**What.** SpiderMonkey and V8 flip JIT pages RW <-> RX with `mprotect`.
Today every such flip goes through `GuestMprotect ->
InvalidateCodeRangeIfNecessary` (`SyscallsSMCTracking.cpp:2718`;
`GranuleMemory.cpp:882` on the sub-granule path), which is
`TM.InvalidateGuestCodeRange` -- a hard invalidation
(`Syscalls.h:548-552`). `SMCSoftInvalidate` (hash-validated relink instead of
recompile, `Config.json.in:667-677`) is reachable only from the write-fault
handler (`:835`) and the granule re-arm path (`:1389`, `:1435`), never from
`mprotect`; `SMCMprotectDefer` (`Config.json.in:828-838`, code at
`:2653-2690`) defers the RW-side invalidation to the `PROT_EXEC` side but
still hard-invalidates there. The design item: at the `PROT_EXEC` transition
of a deferred range, re-hash each block on the page and relink the unchanged
ones, recompiling only blocks whose bytes moved. Both options are
cache-compatible (`LoadEnabled` excludes only the five listed modes,
`CodeCache.cpp:1217-1219`), so the first evidence costs one env var (M4).

**Evidence.** HANDOVER row 10: anonymous units are 12-13% of Firefox and
code-server translation, 33% of Claude's, ~88% of them invalidated, ~22% run
once. The invalidated share is exactly what a page flip after a small IC
patch does to the other blocks on the page. What is unknown is the fraction
whose bytes did *not* change, which bounds the win.

**Expected effect.** Up to ~11% of Firefox's and code-server's translation
and ~29% of Claude's, times the unchanged fraction. Small today; it becomes
the majority of what a *warm* session translates once R1-R3 keep file-backed
code cached, which is why it is on the list and why it is fourth.

**Risk.** Medium: SMC soundness on the W^X path, on a 64K host where the
`mprotect` quantum is the granule (`SMCHostGranule.h`).

**Effort.** Medium.

**Gate.** `check-code-cache.sh`'s SMC case plus a W^X test (a guest that
patches one word on a page holding several blocks through RW -> RX and
checks every block's result), the three-mode suite, and M4's before/after
`smc` and `jit_count` on a real Firefox session.

### R5. Halve the relocation records on disk

**What.** A compact on-disk encoding for `RelocGuestRIP` and
`RelocLinkRecord` (format v6): a 32-bit block-relative offset, a type byte,
register and width bytes, and a 32-bit guest delta, ~16 bytes instead of 48.
Decode into the in-memory struct at load.

**Evidence.** 1.3: 11-13 relocations per block, 43-45% of every large
namespace; `pad2[6]` is 24 of the 48 bytes (`Relocations.h:70-88`).

**Expected effect.** -35% cache size, so R2's cap goes further; -35% of the
bytes every save, compaction and sweep touches (R1's child does less work);
a shorter `ApplyCodeRelocationsSplit` walk per installed block, i.e. some of
the 2.9 us `lookup-ms` on every warm app start (VS Code, Firefox, Factorio,
code-server). Magnitude unmeasured; the reloc walk's share of install has
never been profiled.

**Risk.** Low-medium (a format bump; every block is still hash-checked).

**Effort.** Medium.

**Gate.** `check-code-cache.sh` (corrupt, forged, replace, lld cases),
`reloc-failed` stays 0 on `cc1` and on VS Code, a64diff 64k and 4k-kvm.

### R6. Factorio's warm launch: measure before touching

Not a lever until M3 says what it is (section 1.4). If it is (b)/(c), R1
covers it; if `lookup-ms` is large, R5 and the per-block install cost
(`CODE-CACHE.md:519-522`) are the follow-up; if neither, it is not the cache.

### R7. De-churn the ConfigId: not recommended

Hashing FEXCore's *contents* instead of `GIT_HASH` would keep app caches
across promotes that touch only the syscall layer, thunks, docs or tests.
Against it: the last forty commits are almost all codegen (N7, N9, N13, F7,
G2, ...), so the win is limited to the rare non-codegen promote, and the
failure is silent stale translations after a lowering fix -- the kind of trap
HANDOVER's "Traps" section exists to prevent. R3 buys the safe version of
the same thing. Say no unless promote cadence stays at two a day *and* most
of them stop being codegen.

### Not worth doing (say it plainly)

- **Persisting runtime-generated code keyed by content hash.** JIT output
  embeds heap addresses and IC targets, so identical bytes recur only for
  identical code at identical addresses; 22% of anonymous units run once and
  88% are invalidated within the session (row 10). A hit rate too low to pay
  for the key.
- **A second compiler tier** (G5 proper): closed on evidence, row 10; R4 is
  the part of that territory that does not carry the tier's swap surface.
- **Code-buffer rotation work**: 1 GiB up front (1.2(g)).
- **Eager or background install** (C12, Q2's eager form) and
  **translate-ahead** (G2): the install still takes `CodeBufferWriteMutex`
  per block (`CodeCache.cpp:1485`), so C12's contention finding stands after
  the staging buffer; translate-ahead needs a predictor that knows the taken
  arm (G2 commit) and breaks Chromium's zygote check.
- **AOT `entries`/`all` over the apps**: evidence in R3.

## 3. What round 1 called "not a lever" that the landed work changed

- **C11, asynchronous cache writes** ("a writer cannot outlive
  `exit_group`", `CODE-CACHE.md:259-262`): the premise fell with G4's forked
  writer. The same tool applies to the periodic passes, which round 1 never
  priced because its workloads were short processes. That is R1.
- **C12, background install**: unchanged conclusion, sharpened. G2's staging
  buffer took *emission* out of the global lock, not install
  (`TryLoadBlock`, `:1485`), so a background installer would still contend
  exactly as C12 measured.
- **"Lazy install" (Q2)**: confirmed right by the arithmetic in 1.4 and by
  translate-ahead's races.
- **G1(c), AOT at install**: was "trivial, packaging hook"; the Claude
  measurement (`CODE-CACHE.md:429-433`) and the `cc1` `entries` result turned
  it into R3's warm-up form.
- **Block formation**: still not a compile-cost lever (round 1 §4.5, warm G5's
  sweep), but the cache data gives it a new face: 73 guest bytes per block and
  9x host expansion set the block count, the relocation count and the
  install count for every warm app start. Not on this list; worth a line in
  the warm docs.

## 4. The numbers that are missing, and how to take them from real runs

No harness, no build. The stable through binfmt, the owner's launchers, one
env var each; the counters print per process at exit, so a multi-process app
gives one line per pid. `FEX_CODECACHESTATS=1` also disables the exit fork so
the line includes the final save (`FEXInterpreter.cpp:914-925`,
`Thread.cpp:874-878`).

- **M1, Firefox browsing.** `POWERARM_CODECACHESTATS=1 firefox <site>`,
  browse ten minutes across a few sites, quit normally; repeat once so the
  second session is the warm one. Per process: `loaded`, `not-in-index`
  (first touch or lost saves), `no-file` (no namespace: post-promote or
  evicted), `guest-mismatch`/`not-exec`/`reloc-failed` (rejected loads),
  `saved`, `compactions`, `save-ms`, `lookup-ms`. With
  `POWERARM_PROFILESTATS=1` and `Scripts/powerarm/shmstats.py` alongside:
  `jit_count` per thread, so `saved` against compiled gives the loss of
  1.2(a), and `smc` gives the invalidation rate for R4. This one run splits
  1.1 from 1.2 from first touch and decides R1's magnitude. Twenty minutes.
- **M2, VS Code start and code-server start.** Same line; code-server's warm
  numbers exist (`CODE-CACHE.md:358`), VS Code's do not.
- **M3, one warm Factorio launch** with `POWERARM_STARTUPTIMES=1
  POWERARM_CODECACHESTATS=1` and shmstats: `lookup-ms`, `compactions`,
  `save-ms`, the cache lock times, against the 27/38 s pair.
- **M4, W^X.** One Firefox (or VS Code) session with
  `POWERARM_SMCMPROTECTDEFER=1` against one without, reading shmstats's `smc`
  and `jit_count`: the invalidation count the flip costs today and the share
  a deferral removes. Two sessions.
- **M5, rotation.** `FEX_BUFSTATS=/tmp/bufstats.log` on the M1 session; the
  expected answer is no rotation, which retires 1.2(g) for good.

## 5. Gates, summarised

| Item | Gate |
|---|---|
| R1 | HANDOVER item 21 open (a) reproduced and fixed first; `check-code-cache.sh`, `check-code-cache-contend.sh`, new busy-lock case; `rlimit_as`, `forkexec`, `hostfault`; three-mode A64Frontend; M1/M2 `saved` vs compiled |
| R2 | cap case in `check-code-cache.sh`; two-build eviction order |
| R3 | first launch after promote+warm-up: `no-file 0`, code-server 2.19 s, `claude --version` 90 ms |
| R4 | SMC case plus a W^X test; three-mode suite; M4 before/after |
| R5 | `check-code-cache.sh` corrupt/forged/replace/lld; `reloc-failed` 0; a64diff 64k and 4k-kvm |
