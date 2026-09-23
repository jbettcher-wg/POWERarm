# POWERarm code cache

Workstream OPT-CODECACHE, 2026-09-17. Branch `powerarm-opt/codecache`.

POWERarm translates every guest block the first time a process reaches it. A
build runs hundreds of short processes (`sh`, `sed`, the `gcc` driver, `cc1`,
`as`) that translate the same code again and again. The code cache keeps
translated blocks on disk and installs them in later processes.

The cache is on by default (`EnableCodeCachingWIP=1`, `CodeCacheScope=home`).
Turn it off with:

```
POWERARM_ENABLECODECACHINGWIP=0
```

`rootfs` writes caches for files under the configured RootFS and its overlay,
`home` (the default) for those and every file under `$HOME`, and `all` for
every executable mapping. See [Scope](#scope-which-files-are-cached) for why
`home` is the default. The durable cache lives in
`$POWERARM_APP_CACHE_LOCATION/cache/` (default `$XDG_CACHE_HOME/powerarm/cache/`)
and is capped at `CodeCacheMaxSize` MiB (default 8192,
`POWERARM_CODECACHEMAXSIZE`). The working copy is a hot tier in RAM over it
(`$XDG_RUNTIME_DIR/powerarm/cache/`); see [Two tiers](#two-tiers-a-hot-copy-in-ram). `POWERARM_CODECACHESTATS=1`
prints each process's counters to stderr. The SMC modes that the cache cannot
serve (`SMCSemanticPatch`, `SMCLazyInval`, `SMCCheapTier`, `SMCStoreEmulation`,
`SMCStoreBackpatch`) turn it off. See [Default-on](#default-on).

## Where cold-process time goes

`gcc -c empty.c` runs the driver, `cc1` and `as`. The table shows the share of
user CPU time for the process tree. The data comes from `perf record -e cycles:u`
over 6 runs on one pinned core. Kernel time is `rusage` sys time, which is
8–10% of wall time in every case. The categories group symbols:
translation means the A64 decoder, IR builder, IR passes, register allocation
and PPC64 emission.

| Category | f470d805f, cache off | branch, cache off | branch (merged), cache warm |
|---|---|---|---|
| guest code (translated) | 7.1% | 7.4% | 43.9% |
| translation: decode + IR build | 14.2% | 12.9% | 0.1% |
| translation: IR passes + RA | 18.3% | 18.9% | 0.1% |
| translation: backend emission | 25.4% | 26.2% | 0.2% |
| lookup, link, dispatch | 8.6% | 8.0% | 23.7% |
| block install + registration | 2.4% | 3.3% | 20.1% |
| xxhash (file ids; cache entry hashes) | 2.1% | 0.0% | 1.1% |
| host libc (locks, memcpy, malloc) | 2.5% | 2.6% | 11.0% |
| other emulator | 7.0% | 7.7% | 7.2% |

The branch cache-off column was measured before the OPT-BRANCHES merge. In the
warm column, link-first exits (merged from OPT-BRANCHES) make every exit link
on its first execution, which is part of the larger lookup/link share.
vfork copy-back is not in the table: strace puts all `process_vm_readv` calls
at 1.4 ms of `gcc -c empty.c` and 25 ms of a 12 s zlib `configure`, so it was
left alone. The 152 forks of that `configure` take 218 ms in the kernel.

Per process, cache off, on CPU 100 at f470d805f: `POWERarm /usr/bin/true` took
31–44 ms, `gcc --version` 50 ms, `cc1` on an empty file 245 ms and `as` 67 ms.
Startup was dominated by translating `ld.so` and libc initialisation. The
A64 decode-table build alone was 15% of `true` (C1).

## The "stall"

M2 reported that a Lua build with the cache enabled stalled for over 10
minutes. It was not a hang:

- The stalled run's objects (`/tmp/pa-m2p-timing/emu-cache1/lua`) kept
  appearing until 00:38:03, after its agent's tool call had been interrupted
  at 00:37:02. The processes were then killed by hand.
- The same script, with the same binary (`bin-843ec5be6`) and a fresh cache,
  completes. zlib configure took 10.1 s, zlib make 183.4 s and Lua make
  177.0 s, where zlib make and Lua make each take about 120 s without the
  cache. The current tree behaves the same way.
- The cause of the slowdown: enabling the cache forced block linking off
  (`BlockLinkingEnabled = ... && !ENABLECODECACHINGWIP`). `cc1 -O2 lvm.c` goes
  from 11.9 s to 21.3 s without linking. The four-pass timing sequence
  therefore ran past the agent's 10-minute tool limit and looked stuck.

## Design (format v4)

`FEXCore/Source/Interface/Core/CodeCache.cpp` has the authoritative comment.
In short:

**Keys.** Each file's cache is named `<basename>-<FileId>-<ConfigId>`.

- FileId hashes the file's `(dev, inode, size, mtime, ctime)`.
- ConfigId hashes:
  - `GIT_HASH` and the executable's `NT_GNU_BUILD_ID` (changes on any rebuild);
  - every `HostFeatures` field, including `SupportsISA30` and the cache line
    sizes (a size assert catches a new field); the host MIDRs as a set of
    distinct values, so the CPU affinity does not split the cache;
  - the host page size;
  - every codegen-affecting option and environment switch, including
    `BlockLinking`.

  So a different emulator build, host ISA level, page size or codegen option
  is a different file name.

**Soundness does not rest on the key.** A segment block is installed only if
all of these hold:

1. its entry hash (entry fields, code, relocations) verifies;
2. the guest bytes it was translated from are executable in this process and
   hash to the stored value, checked after its pages are write-protected;
3. its relocations apply.

A replaced binary, a forged or colliding id, or code patched before the block
is first reached all fail check 2 and are compiled normally. Code patched
after install is caught by normal SMC tracking: the pages are registered
exactly like a compile's.

**Format (version 5).** A file's cache is a set of self-contained segments:
`<name>`, `<name>.1` … `<name>.7`. Each segment has a header (build hash,
emulator build id, config id, file id, writer's boot id), a guest-offset-sorted
block index, relocations (offsets relative to the block) and code. Blocks are stored exactly as the JIT laid them
out, but unlinked (`RELOC_LINK_RECORD` undoes links, and the record's saved
words are recomputed after relocation), with host symbols zeroed and guest RIPs
relative to the file's load base.

**Guest-address loads.** With the cache alone on, a guest RIP (exit target,
call return address) is the ordinary variable-width `LoadConstant`, and
`RELOC_GUEST_RIP_MOVE` records how many instructions it took. The segment
stores the site as nops. On install the loader emits `LoadConstant` for the
rebased value into that width, padded with nops, and rejects the block if it
needs more. Load bases are page-aligned, so the low bits that decide the width
rarely change (`reloc-failed` stays 0 on `cc1`). The last-constant delta form
applies between two guest RIPs of the same block, whose difference the load
base does not change, but never between a guest RIP and a plain constant.
`SMCSemanticPatch` still uses the fixed 5-instruction window.

**Loading is lazy, per block.** `CompileBlock` asks `CodeCache::TryLoadBlock`
before compiling. Nothing is read at `mmap` time, so a process pays only for
the blocks it reaches. Blocks relink on first use, so block linking stays on
with the cache.

**Writing appends.** At `exit_group`, before `execve`, and every 50000 blocks
or 60 s, a process writes the blocks it compiled that no segment holds yet.
Before the guest `munmap`s an executable file mapping (a `dlclose`), it writes
that file's new blocks, since a pass only reaches files still mapped. A
periodic pass skips a file with fewer than 8 new blocks and keeps them for a
later pass. At exit, `execve` and unmap the minimum is waived for a file with no
cache yet and in a process that has run a periodic pass. Files are tracked as
code when their mode is executable or they are ELF files (shared libraries are
often installed 0644). A segment is written to a temp file and published with
`link(2)` under a shared `flock`. When
all eight names are taken, the writer merges them into `<name>` under an
exclusive `flock`. Readers take no lock: a
mapped segment stays valid if a compaction unlinks it. There is no `fsync`.
Entry hashes are checked when the reading boot differs from the writer's
(`/proc/sys/kernel/random/boot_id`), which is when a crash could have torn the
file. `POWERARM_CODECACHEVERIFY=1` forces the check.

**Size cap and eviction.** After a process publishes a segment, it sweeps the
cache directory. Across all processes this happens at most once a minute:
the mtime of `.sweep` records the last sweep, and a process claims the next
one under `.sweep.lock` with `LOCK_NB`. A namespace is all files of one
`<name>-<FileId>-<ConfigId>`. Its last use is the newest mtime of its files.
A process opening a namespace's first segment refreshes that mtime when it is
older than 10 minutes. The sweep:

1. removes namespaces unused for an hour whose segment header names another
   emulator build (GIT_HASH plus executable build id) or format, and temp
   files older than an hour;
2. if the remaining namespaces exceed `CodeCacheMaxSize`, removes whole
   namespaces until they fit in 90% of it: namespaces of another build first,
   whatever their mtime, then this build's own, each least-recently-used
   first.

A namespace is removed under an exclusive `LOCK_NB` flock of its `.lock`. A
busy namespace is skipped, so the sweep never runs during an append or a
compaction. Segments go highest index first, then the lock file. Running
processes keep valid data in their mapped segments. A writer that races the
lock file's removal can at worst lose its own segment to a concurrent
compaction. That costs recompiles, never wrong code, because every block is
checked on install.

Why 8 GiB, and why another build goes first. One desktop app's namespace is
the size of the code its session reaches, not of its binary: VS Code's is
726 MB (604,598 unique blocks, 9x host expansion, 43% relocation records),
the Claude CLI's 200-258 MB, and Firefox's libxul would be of that order
again. Four of the owner's apps under one build are 1.35 GB, so the old
2 GiB cap was already being swept every minute with two builds present, and
Firefox never kept a namespace at all. Rows 1 and 2 answer different
questions: row 1 removes another build's namespaces only after an hour
unused, because its processes may still be running (a promote does not stop
them -- HANDOVER "Traps"), while row 2 runs while a process of that build is
still writing. Under the cap the two builds' namespaces are worth the same
per byte; over it they are not, because nothing started after the promote can
ever read the old build's, so those are evicted first.

Test harnesses use a cache directory of their own
(`POWERARM_APP_CACHE_LOCATION`): `check-code-cache.sh`,
`check-code-cache-contend.sh` and `unittests/A64Frontend/run.sh`. Every gate
run is a new build, so a new ConfigId, and with the default directory the
suites' namespaces competed with the apps the cache exists for until the
hour-old sweep took them.

### Two tiers: a hot copy in RAM

The working cache is the hot tier: every segment a process maps, appends to or
compacts is there, so a warm session reads and writes a tmpfs and the NVMe is
touched only to seed a namespace and to write one back. The durable tier is the
copy that survives a reboot.

| | |
|---|---|
| Durable | `$POWERARM_APP_CACHE_LOCATION/cache/`, default `$XDG_CACHE_HOME/powerarm/cache/`. Capped by `CodeCacheMaxSize` (8192 MiB). |
| Hot | `$XDG_RUNTIME_DIR/powerarm/cache/`, or `/tmp/powerarm-<uid>/cache/` with no runtime directory -- and `hot/` beside the `cache/` of a caller that chose its own cache location, so a test harness never shares one. `CodeCacheHotLocation` overrides it. Capped by `CodeCacheHotMaxSize` (8192 MiB), swept the same way and separately, because this one is RAM. |

**Seeding is lazy, per namespace.** The first time a process opens a namespace,
the segments the durable tier has and the hot tier has not are copied up,
through a temp file and `link(2)`: two processes racing cost one wasted copy and
never half a file. Nothing is copied at startup, and a namespace no process
opens is never copied at all. That copy is the one place the hot tier costs
something, and it happens once per namespace per boot.

**Write-back is off the guest thread.** After a segment is published or a
namespace compacted, the forked writer (below) mirrors what changed: one segment
for an append, the whole namespace after a compaction, which is where the names
the durable tier must lose are decided. A segment is copied only if it validates
-- magic, version, header hash, and a file long enough for the extent its header
declares -- and it lands with `rename(2)`. So a torn or truncated hot copy cannot
replace a good durable one, and a reader of the durable tier never sees a partial
file.

**What it costs when it goes wrong.** Losing the hot tier -- a reboot, a wipe,
its own size sweep -- costs a re-seed and nothing else. The tiers are allowed to
disagree, the hot one being the newer: a block the durable tier is missing is
compiled once more in the session after the reboot. Every block either tier holds
is validated on install exactly as before, so neither tier can make a process run
wrong code. `POWERARM_CODECACHEHOTTIER=0` leaves one tier, the durable one,
behaving exactly as it did before this existed.

**The writing is forked off the guest thread.** Collecting the blocks needs the
process (the code buffer walk under `CodeBufferWriteMutex`, and the guest's own
bytes); writing them out does not. So a periodic or unmap pass builds its
segments and then forks a writer, which writes the temp files, takes the
namespace locks, compacts and sweeps while the guest runs on. Without it that
work was on whichever guest thread reached the trigger inside `mmap`, `munmap`
or `mprotect`: ~3 us per block, and once a namespace has all eight segment
names, a whole-namespace rewrite (726 MB for VS Code's) per pass. The exit save
is forked already (cold G4). `POWERARM_CODECACHEFORKWRITER=0` puts the writing
back on the guest thread.

The writer is a fork of a live multi-threaded guest, so it only writes: it takes
no lock of the emulator's, reads no guest memory and never returns to emulation.
It comes from `fork(3)` (whose `pthread_atfork` handlers leave the allocator
consistent, which the raw `clone(2)` of a guest fork does not do), the parent
forks twice and reaps the intermediate so the writer is init's child rather than
the guest's -- invisible to the guest's `wait(2)`, and never a zombie -- and the
writer drops every inherited descriptor: a shell's `$(guest ...)` reads the
guest's stdout until every writer closes it, so a writer parked on a lock would
otherwise hang the command substitution. It says nothing (`LogMan`'s handler can
want a lock another thread held at the fork); what it wrote is in the counters,
which it reports through a page shared with its parent. An `alarm(2)` bounds its
life at a minute, and at most two run at once.

Because the writer waits for the namespace lock (up to 20 s) instead of giving
the segment up the moment `flock` says busy, a pass no longer loses what it
compiled. It used to: nothing re-kept the records of a dropped segment, so with
eight segments present -- where every periodic pass wants the exclusive lock --
two processes of the same app saving in the same minute cost one of them
everything since its last pass, for good. A pass that still publishes on the
guest thread (the writer fork refused, or turned off) hands its records back to
the next pass instead.

**Processes.** A fork child forgets the parent's unsaved compiles. SMC modes
whose per-block metadata is not stored disable loading: semantic patch, lazy
invalidation, cheap tier, and store emulation or backpatch. The code map writer
now runs only for `POWERARM_SERVERCODECACHE=1`.

## Correctness

`Scripts/powerarm/check-code-cache.sh BUILD_DIR` runs these checks (75 s):

- **Parallel:** 16 parallel `gcc -O2 -c` sharing one cold cache, then warm.
  The objects are identical to cache-off objects, 1.3M blocks load, no entry
  fails its hash, and no temp file or ninth segment is left.
- **ISA 3.0:** `disableisa30` on the same directory loads nothing from the
  ISA 3.0 cache (checked with the directory read-only), writes a second config
  id, and produces identical objects.
- **Replace:** a binary rewritten in place (same inode and size) runs its new
  code.
- **Forged:** the old binary's cache is renamed to the new identity, and its
  block is rejected on the guest bytes.
- **Corrupt:** one code byte is flipped in every `cc1` block. 1.2M entries are
  rejected and the objects are still identical.
- **SMC:** a guest patches its own code after first use, and before first use,
  in cold and warm runs.
- **Small libraries:** a warm run of a program that throws a C++ exception
  (`libgcc_s.so.1`, installed 0644), `dlopen`s and `dlclose`s a library and
  links libraries it only initialises compiles fewer than 10 blocks. The
  program closes its stderr before exiting, and the counters still reach the
  log.
- **Reseed:** with a hot tier, what a cold run published is in both tiers, and
  a wiped hot tier (a reboot) is seeded back from the durable one: the guest
  loads its blocks again, `no-file` is 0, and the hot copy is back.
- **Torn:** a hot segment truncated to half its length must not reach the
  durable tier -- the durable copy is byte-identical afterwards -- the guest
  must still run correctly, and the durable copy must still load once the hot
  tier is wiped.
- **Lock busy:** another process holds `LOCK_EX` on a library's namespace lock
  for the whole guest run. The guest's `dlclose` pass cannot publish, and its
  blocks must not be lost: the forked writer is parked on the lock (its temp
  file is there, no segment is), and once the holder lets go the segment
  appears and a later run loads it. Skipped without host `flock(1)`.
- **Evict:** 20 MiB of another build's cache (a namespace this build's header
  check rejects), written last so a pure LRU would keep it, next to this
  build's namespaces under a cap just below the total. The other build's goes
  and this build's stays. Then the same against the hot tier's own cap.
- **lld:** two lld-linked programs, built with the host's `clang` and `ld.lld`
  against the rootfs, compile none of their own blocks warm: one with lld's
  default layout (text 64K-congruent at a file offset whose 4K rounding is not
  64K-aligned, the Claude CLI's case) and one linked `-z max-page-size=4096`
  (only 4K-congruent). Their own functions sit 128K from any other segment,
  and each prints their address range so the check counts only those. Before
  G1 all 20 of those blocks were compiled again by every warm run on a 64K
  host. Skipped when `clang` or `ld.lld` is missing.
- **Home links:** with `CodeCacheScope=home`, a program reached through a
  symlink directly in `$HOME` whose target is outside it (a temporary
  directory in `BUILD_DIR`) gets a cache, and one behind a symlink into the
  check's `/tmp` directory does not.

Every process runs with `POWERARM_PORTABLE=1`, so the `cc1` and `as` that
`gcc` execs run on the build under test rather than on whatever build binfmt
has registered (until 2026-09-18 they ran on the stable install).

Gates on the merged tree (892c0a2c9):

- A64Frontend: 45/45 in default, `POWERARM_MAXINST=1` and `disableisa30`
  modes, with the cache off, cold and warm.
- `callret` (OPT-BRANCHES golden): passes with the cache off, cold, warm and
  `disableisa30`.
- a64diff bundle `41c1f1e4dd1e`, on 64K (cache off, cold, warm) and 4k-kvm
  (cache off, and cache on within the run): insn 3713/3731 with
  required-fail=0, programs 25/25, alarm 3/3, projects 2/2 (zlib and Lua
  byte-identical), rootfs 6/6.
- `check-code-cache.sh`, `check-user-strings.sh` and `check-rootfs-server.sh`
  all pass.

## Timings

CPU 100, one guest core, `m2time` script (tar + configure + make), private
cache directory. "Cold" starts from an empty cache and writes it during the
run. "Warm" repeats the run with that cache. f470d805f is the M2 tree
(measured at the start of this work). powerarm is f0a9da187, with OPT-BRANCHES
merged. "Branch" is this branch after merging powerarm. There is no separate
"cold with background cores" column: nothing in the shipped cache runs off the
guest thread (see below), so it equals cold.

| Step | f470d805f off | powerarm off | branch off | branch cold | branch warm | warm vs f470 | warm vs powerarm |
|---|---|---|---|---|---|---|---|
| `gcc -c empty.c` (via `sh -c`) | 0.418 s | 0.404 s | 0.378 s | 0.409 s | 0.157 s | -62% | -61% |
| zlib `./configure` | 13.62 s | 12.94 s | 12.51 s | 6.58 s | 5.15 s | -62% | -60% |
| zlib build | 133.1 s | 117.7 s | 115.6 s | 74.7 s | 71.3 s | -46% | -39% |
| Lua build | 118.9 s | 103.3 s | 102.7 s | 70.2 s | 66.1 s | -44% | -36% |

For reference, the Pi 5 natively takes 0.043 s, 0.55 s, 12.6 s and 12.6 s. A
cold build is already much faster than no cache, because every process after
the first of each binary reuses what earlier ones wrote. `cc1 -O2 lvm.c` alone
is the worst case: one long process that reuses nothing, 14.9 s cold, 11.9 s
warm and 11.9 s with the cache off (measured before the merge).

## Background work on spare cores

The owner asked for three evaluations. Each was measured with the guest on
CPU 100 and background work allowed on the physical cores 112/116/120/124.

**Asynchronous cache writes: not implemented** -- overturned, twice. A cold Lua
build spends 1.5 s of 91.5 s writing segments, summed over all 116 processes
(`save-ms`), and the conclusion rested on "a writer cannot outlive
`exit_group`". It can: cold G4 forks the exit save, and the periodic and unmap
passes fork theirs (above). The premise was measured on a build's short
processes, which is not where the cost is; a long-lived app pays the same write
over and over, plus a compaction.

**Background pre-translation and install: implemented, measured slower,
removed.** A per-process `SCHED_IDLE` thread was placed outside the guest's
SMT siblings. It installed every cached block of a file in the order the
writer compiled them, as soon as the guest first touched the file. Warm Lua
took 90.5 s with it against 86.9 s without. It installed 8.7M blocks where the
guest used 6.6M, and its per-block lookup-cache and code-buffer writes
contended with the guest thread's lookups and links. Translating
never-executed binaries ahead of time, for example from `POWERarmServer`,
cannot help more than the cold-to-warm gap:
3.4 s of a 74.7 s cold zlib build, 4.1 s of 70.2 s for Lua, 1.4 s for zlib `configure`. On a build, only the first process of each binary pays that
gap, because later processes load what it wrote.

**Tiered compilation: evaluated only.** This means a quick first translation,
then an optimised one compiled in the background and swapped in by relinking.
The pieces exist: `BlockLinks` delinking, `LookupCache` replacement, and the
cache's guest-byte check. The cost is correctness surface. The swap must
delink every inbound link and every return-stack and BR compare-cache entry
naming the old block, under the exclusive invalidation lock, while guest
threads run. The gain has no ceiling to measure yet: this JIT has no cheaper
tier, and steady-state translated code (75% of a build) would only improve if
the optimised tier is materially better than today's code. Next step if
pursued: a `POWERARM_MAXINST`-style fast tier with per-block execution
counters, and a measured quality delta on `cc1 -O2 lvm.c`.

## Default-on

On since OPT2-CACHEDEFAULT (`CodeCacheScope=rootfs`; `home` since G1, see
the next section). The earlier blockers:

1. **Unbounded disk use:** fixed by the size cap, LRU eviction and the sweep of
   other builds' namespaces (see Design).
2. **Cache-mode codegen cost steady-state speed:** fixed by variable-width
   relocatable loads. On the slice workload (10 Lua objects at `-O2` plus
   `ar`/`ld`, CPU 100), with the cache off at 45.02 s, a cold run went from
   31.18 s to 30.28 s and a warm run from 28.28 s to 27.22 s.
3. **First runs are slower:** still true for one long process that reuses
   nothing (a cold `cc1 -O2 lvm.c`). Builds come out ahead because later
   processes of the same binary load what earlier ones wrote.

## Scope: which files are cached

`CodeCacheScope=home` is the default since G1 (cold research,
`research/cold-translation/`, 2026-09-18); it was `rootfs`. The apps POWERarm
exists for are installed by the user, under `$HOME`: the Claude CLI in
`~/.local/share/claude/versions/<version>`, code-server in
`~/.cache/powerarm/code-server`, a VS Code tarball in `~/.local`. Under
`rootfs` their own code, 84% of what code-server translates and nearly all of
Claude's, was compiled again at every launch. `home` is the RootFS, its
overlay (where guest pacman installs; `rootfs` includes it now too),
everything below `$HOME`, and everything below the target of a symlink
directly in `$HOME` (next paragraphs).

Cache names come from `/proc/self/fd`, so a file's path is its resolved one,
and "below `$HOME`" alone misses trees a user keeps behind a link. On the
POWER9, `~/Development` is a link to `/mnt/arch/home/jbettcher/Development`,
and VS Code arm64 lives at `~/Development/vscode-arm64/<version>`. So `home`
also covers the resolved target of each symlink directly in `$HOME`:

- only links directly in `$HOME`, read once per process (a `getdents64` of
  `$HOME` and a `realpath` per link, on the first scope question about a file
  outside the rootfs, so build tools that run only rootfs code never pay it).
  Links deeper down resolve inside trees already covered;
- not a target that is `/`, under `/tmp`, `/var/tmp`, `/dev`, `/proc`,
  `/sys` or `/run`, or on a tmpfs or ramfs: a link into scratch space would
  let `/tmp`'s one-off binaries back in;
- only a directory or a regular file.

A simpler rule was weighed and rejected: "every file the user owns on a
persistent filesystem" covers the same layout, but needs an `fstat` and a
`statfs` per file, sweeps in whatever the user owns under `/opt` or `/srv`,
and is harder to predict than "`$HOME` and what it links to".

This takes in the build trees under `~/Development` too, the POWERarm build
directories and test binaries included. That is bounded: a rebuilt binary is
a new namespace written once, at the size of the code its run reached, and it
ages out of the LRU; a new emulator build's namespaces replace the old
build's within the hour (the sweep). Measured on the A64Frontend gate, whose
goldens live in `~/Development/.powerarm-golden`: one plain run of the suite
writes about 20 MB, 18 MB of it the test binaries' own; the
`POWERARM_MAXINST=1` mode (one block per instruction) 95 MB; the three gate
modes together 175 MB, under six config ids. That is why `run.sh` now makes
its own cache directory (above). Build trees in `/tmp`, the a64diff work
directory and check-code-cache.sh's own stay out.

Measured on the G1 tree against its parent (f518d94a3, whose code is the
current stable, c632e0bca), one cold/warm pair each, CPUs 40-47, private cache
directories, code-server's child processes kept on the build under test with
`POWERARM_PORTABLE=1`:

| Workload | before, cold / warm | after, cold / warm |
|---|---|---|
| `claude --version` (2.1.276), whole process | 507 / 470 ms | 576 / **90** ms |
| same, blocks compiled | 25.7k / 24.2k | 25.7k / **41** |
| same, `map` phase | 31.8 / 49.5 ms | 0.8 / 0.9 ms |
| same, max RSS | 335 / 313 MiB | 245 / **179** MiB |
| code-server, launch to first `/healthz` 200 | 3.93 / 3.97 s | 3.88 / **2.19** s |
| code-server, then `/` | 1.14 / 1.20 s | 1.36 / 1.07 s |
| cache directory after the pair | 2.9 MB (Claude), 6.7 MB (code-server) | 47 MB, 179 MB |

The cold Claude run pays 79 ms to save 25.6k blocks (12 ms before). The `map`
row is the loader half of G1 (next section), and so is most of the RSS drop:
with the loader change alone a cold run peaks at 177 MiB, and the cold run
above adds the segments it builds to save.

What was weighed:

- **Size.** The cap (C16: `CodeCacheMaxSize`, 8 GiB, whole namespaces evicted
  another build's first and then least recently used) bounds it. One launch of each app writes the sizes
  above; a long Claude session writes more, in the same append-only segments.
  Each Claude update is a new file, so a new namespace, and the old version's
  ages out of the LRU. Apps used daily stay.
- **Runtime-generated code is never cached, in any scope.** JSC's and V8's
  JITs write anonymous memory; the cache only names file-backed code. That
  code (V8's 14% of code-server's units, Bun's JIT tiers) still compiles at
  every launch, which is G5's territory. Self-modifying file-backed code is
  handled as for any cached file: every block's guest bytes are hashed when it
  is installed, and normal SMC tracking covers it afterwards.
- **Security does not change.** A block is installed only when the guest
  bytes it was translated from hash to the stored value in this process, so a
  cache file can only ever supply a translation of the bytes actually mapped.
  The cache directory is per user (`$XDG_CACHE_HOME`), and whoever can write it
  is already that user. Widening the scope adds that user's own files, not
  new writers.
- **Why not `all`.** It also takes `/tmp`, build trees and memfd-backed
  code. A rebuilt test binary is a new namespace every time, and a memfd
  (`/memfd:... (deleted)`, a new inode per process, as in .NET's double-mapped
  JIT) can never be reused, yet each costs save time and pushes useful
  namespaces out of the LRU. `all` stays available for apps installed
  system-wide, under `/opt` for example.

## 64K hosts: lld-linked ELFs (G1)

AArch64 linkers (lld, GNU ld) default to a 64K max-page-size, so an ELF's
segments are 64K-congruent (`p_vaddr = p_offset mod 64K`), but their offsets
are only 4K-aligned. The ELF loader (`ELFCodeLoader.h`, `MapFile`) rounded
each segment to the 4K guest page, and on a 64K host the resulting offset
could not be mapped, so the segment went through the anon+`pread` fallback.
That was every lld-linked executable whose text starts past the first 64K of
the file, the Claude CLI's 63 MB text and 146 MB data included. Its text was
anonymous memory: no file for the cache to name, no load, no save, 32-50 ms of
`pread` per launch and ~200 MB of private RSS.

Now, on a host page larger than 4K:

- a **host-congruent** segment is a real file mapping from the host page that
  holds `p_vaddr` and the matching host-aligned offset, which is what a 64K
  kernel's own binfmt_elf does. Host pages an earlier segment of the same ELF
  already materialised are kept (their bytes and protections), and this
  segment's own bytes are read into them;
- a segment that is **only 4K-congruent** (`-z max-page-size=4096`) still
  gets the anon+`pread` copy, and the copy is then tracked as the private file
  mapping it stands in for (`TrackFileBackedCopy`), the same tracking
  `GranuleMemory::Mmap` gives the guest's own sub-granule file mappings. The
  cache keys blocks by guest offset from the file's load base and hashes the
  guest bytes of every block it installs, so how the bytes got there does not
  matter. Code in host pages it shares with a writable segment is still
  recompiled when those writes invalidate it (the check's 4k program: 8
  blocks of PLT and `.fini`).

`InferMappingBaseAddress` accepts host-rounded mapping offsets, so both
mappings, and the guest ld.so's own (it rounds to `AT_PAGESZ`, the host page),
find their file's resource directly. On a 4K host none of this runs: the
`MatchesGuest()` path of `MapFile` is the old one.

## AOT for apps (G1c): plan, not built

Measured once: `POWERARM_AOTTRANSLATE=entries` over the Claude ELF finds 3,636
seeds in 0.14 s (6 MB), and the first `--version` after it loads 136 of them
and still compiles 25.5k blocks. Bun ships stripped: only `.dynsym` and the
`.eh_frame_hdr` FDEs are left to seed from, and neither reaches the code a
launch runs. Seeding from symbols is the wrong tool for these apps.

What would make an app's first launch warm is a warm-up run at install:

1. The port's packaging runs one representative launch under the emulator the
   user runs (the stable install, through binfmt) and the default cache: the
   Claude installer after writing `versions/<v>` runs `claude --version`; the
   VS Code or code-server unpack starts the server, waits for `/healthz` and
   stops it with SIGTERM (a process killed by SIGKILL saves nothing).
2. A promote gives the emulator a new ConfigId and makes every app cold again,
   so `promote-powerarm-stable.sh` re-runs the registered warm-ups (a list in
   the config directory, one command per app) after re-registering binfmt.
3. Still worth measuring: mode `entries` over code-server's `node`, which
   keeps a large `.dynsym`, as a complement to the warm-up.

## Ahead-of-time translation (Q1)

`Scripts/powerarm/aot-translate.sh [-j JOBS] [-m all|calls|entries] [-s MAXBYTES] <POWERarm> [guest paths or dirs]`
fills the cache before a binary first runs. The default paths are `/usr/bin`
and `/usr/lib` of `$POWERARM_ROOTFS`. The script runs one `POWERarm <file>` per
ELF with `POWERARM_AOTTRANSLATE=<mode>` at nice 19. That process starts up
exactly like a normal run, so the cache name (FileId, and the ConfigId of this
executable and its options) matches the runs that follow. It compiles the
seeds and saves them through `SaveCodeCaches`. It then folds the namespace
into one segment (`CompactAllSegments`). The guest is never executed.
Translate with the same build and the same `POWERARM_*` codegen settings as
the later runs.

The seeds (`Source/Tools/FEXInterpreter/AOT/AOTGenerator.cpp`) are the ELF
entry, the `.eh_frame_hdr` FDE starts and the function symbols. Mode `calls`
adds the return points after BL and BLR. Mode `all` (the default) also adds
direct-branch targets beyond the decoder's 128-byte region window. Jump-table
targets are not found and are still compiled at run time. Libraries load at the
main-ELF base rather than where ld.so would put them. Cached addresses are
relative to the base, so a different base can only cause a `reloc-failed`
reject, and none were seen. Every block is validated on install as usual.

The cost is size. `cc1` in mode `all` takes 1.2 GiB of the 8 GiB default
`CodeCacheMaxSize` (2 GiB when this was measured). Pre-translating all of `/usr/lib` therefore needs a larger
cap, or mode `calls` or `entries`. The numbers are in the checklist's Queue
section.

### Pre-translating the tools a build spawns (Q4, cold runs)

The AOT policy that pays for cold runs is the opposite of the obvious one.
A build spawns the same few short-lived tools over and over -- `sh`, the `gcc`
driver, `as`, `ar`, `ld`, `collect2` -- and each of those runs is mostly
translation: cold, the `gcc` driver burns 40 ms of its own user time against
7.5 ms warm, and `as` 36 ms against 14 ms. Their cached code is small. The one
giant binary, `cc1` (39 MB), is the opposite: pre-translating it in mode `all`
costs 1.2 GiB, and a build re-runs it per source file, so it is warm by the
second file anyway.

The measured recipe, against the Arch Linux ARM rootfs (one cold and one warm
slice run each, CPU 108, `POWERARM_PORTABLE=1`, private cache directory):

```
POWERARM_ROOTFS=<rootfs> POWERARM_APP_CACHE_LOCATION=<dir>/ POWERARM_PORTABLE=1 \
  taskset -c 0-87 Scripts/powerarm/aot-translate.sh -j 22 -m entries <POWERarm> \
  /usr/bin/bash /usr/bin/gcc /usr/bin/as /usr/bin/ar /usr/bin/ld /usr/bin/ld.bfd \
  /usr/lib/gcc/aarch64-unknown-linux-gnu/*/collect2 \
  /usr/lib/libc.so.6 /usr/lib/ld-linux-aarch64.so.1 \
  /usr/lib/libbfd-*.so /usr/lib/libopcodes-*.so
```

| Pre-translation | AOT wall | cache | slice cold | slice warm |
|---|---|---|---|---|
| none (empty cache) | -- | 0 | 24.42 s | 21.30 s |
| 11 tools, mode `entries` | 0.14 s | 28 MiB | 23.73 s | 21.20 s |
| 11 tools, mode `all` | 0.59 s | 268 MiB | 23.65 s | 21.25 s |
| 11 tools + `cc1`, mode `entries` | 1.05 s | 115 MiB | 23.70 s | 21.24 s |
| `/usr/bin` + `/usr/lib` under 4 MiB (877 files), mode `entries` | 2.5 s | 466 MiB | not run | -- |

Mode `entries` is the one to use: it keeps 90% of the cold win of mode `all`
at a tenth of the size, and adding `cc1` on top buys nothing. `-s MAXBYTES`
skips ELFs over a size, so `-m entries -s 4194304 /usr/bin /usr/lib` is the
whole-rootfs form of the same policy; it costs 466 MiB, and was not measured on the slice because the
slice only reaches the eleven binaries above.

This is not the answer to cold runs, only a third of a second of the 3.1 s
cold-to-warm gap. The rest is `cc1` translating code no earlier run reached,
and that is translation throughput, not cache policy: see the X series.

## Next targets

1. **Install cost.** A warm block costs about 1.2 µs to install, register and
   link on first use: 7.7 s of an 86 s warm Lua build. The rwlock, memcpy and
   registration costs dominate now that hashing is gated.

