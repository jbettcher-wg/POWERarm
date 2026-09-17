# POWERarm code cache

Workstream OPT-CODECACHE, 2026-09-17. Branch `powerarm-opt/codecache`.

POWERarm translates every guest block the first time a process reaches it. A
build runs hundreds of short processes (`sh`, `sed`, the `gcc` driver, `cc1`,
`as`) that translate the same code again and again. The code cache keeps
translated blocks on disk and installs them in later processes.

The cache is on by default (`EnableCodeCachingWIP=1`, `CodeCacheScope=rootfs`).
Turn it off with:

```
POWERARM_ENABLECODECACHINGWIP=0
```

`rootfs` writes caches for files under the configured RootFS, and `all` for
every executable mapping. The cache lives in `$POWERARM_APP_CACHE_LOCATION/cache/`
(default `$XDG_CACHE_HOME/powerarm/cache/`) and is capped at `CodeCacheMaxSize`
MiB (default 2048, `POWERARM_CODECACHEMAXSIZE`). `POWERARM_CODECACHESTATS=1`
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
or 60 s, a process writes the blocks it compiled that no segment holds yet. It
writes a temp file and publishes it with `link(2)` under a shared `flock`. When
all eight names are taken, the writer merges them into `<name>` under an
exclusive `flock` (`LOCK_NB`, so it skips if busy). Readers take no lock: a
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
   namespaces in least-recently-used order until they fit in 90% of it.

A namespace is removed under an exclusive `LOCK_NB` flock of its `.lock`. A
busy namespace is skipped, so the sweep never runs during an append or a
compaction. Segments go highest index first, then the lock file. Running
processes keep valid data in their mapped segments. A writer that races the
lock file's removal can at worst lose its own segment to a concurrent
compaction. That costs recompiles, never wrong code, because every block is
checked on install.

**Processes.** A fork child forgets the parent's unsaved compiles. SMC modes
whose per-block metadata is not stored disable loading: semantic patch, lazy
invalidation, cheap tier, and store emulation or backpatch. The code map writer
now runs only for `POWERARM_SERVERCODECACHE=1`.

## Correctness

`Scripts/powerarm/check-code-cache.sh BUILD_DIR` runs these checks (75 s):

- **Parallel:** 16 parallel `gcc -O2 -c` sharing one cold cache, then warm.
  The objects are identical to cache-off objects, 2.7M blocks load, no entry
  fails its hash, and no temp file or ninth segment is left.
- **ISA 3.0:** `disableisa30` on the same directory loads nothing from the
  ISA 3.0 cache (checked with the directory read-only), writes a second config
  id, and produces identical objects.
- **Replace:** a binary rewritten in place (same inode and size) runs its new
  code.
- **Forged:** the old binary's cache is renamed to the new identity, and its
  block is rejected on the guest bytes.
- **Corrupt:** one code byte is flipped in every `cc1` block. 2.3M entries are
  rejected and the objects are still identical.
- **SMC:** a guest patches its own code after first use, and before first use,
  in cold and warm runs.

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

**Asynchronous cache writes: not implemented.** A cold Lua build spends
1.5 s of 91.5 s writing segments, summed over all 116 processes
(`save-ms`). A writer cannot outlive `exit_group`, so moving the write off the
guest thread would need a forked helper per process, costing more than 1.3%.

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

On since OPT2-CACHEDEFAULT (`CodeCacheScope=rootfs`). The earlier blockers:

1. **Unbounded disk use:** fixed by the size cap, LRU eviction and the sweep of
   other builds' namespaces (see Design).
2. **Cache-mode codegen cost steady-state speed:** fixed by variable-width
   relocatable loads. On the slice workload (10 Lua objects at `-O2` plus
   `ar`/`ld`, CPU 100), with the cache off at 45.02 s, a cold run went from
   31.18 s to 30.28 s and a warm run from 28.28 s to 27.22 s.
3. **First runs are slower:** still true for one long process that reuses
   nothing (a cold `cc1 -O2 lvm.c`). Builds come out ahead because later
   processes of the same binary load what earlier ones wrote.

## Next targets

1. **Install cost.** A warm block costs about 1.2 µs to install, register and
   link on first use: 7.7 s of an 86 s warm Lua build. The rwlock, memcpy and
   registration costs dominate now that hashing is gated.

