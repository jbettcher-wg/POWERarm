# fastppcx86 code cache vs POWERarm's rewrite

POWERarm started from this tree's code cache (fork point 9da3ccf9f, the same as `daedalao-wt`) and
rewrote it (format v4/v5). Design, measurements and correctness tests are in POWERarm's
`docs/powerarm/CODE-CACHE.md`, `FEXCore/Source/Interface/Core/CodeCache.cpp` and
`Scripts/powerarm/check-code-cache.sh`.

## How this tree's cache works (at 9da3ccf9f)

- It serializes a file's whole code buffer and relocates it on load.
- The file is keyed by a content hash of the mapped executable. That hash is the cache's only
  validity check (see `ComputeCodeMapId`).
- It's off by default (`EnableCodeCachingWIP`).

## Gaps, and what closes them

| # | Gap in this tree | Effect | Closed by |
|---|---|---|---|
| 1 | Block linking is forced off whenever the cache is enabled: `JIT.cpp` `BlockLinkingEnabled = BlockLinking() && !ENABLECODECACHINGWIP` | every block exit goes through the dispatcher; in POWERarm this made `cc1` about 1.8× slower with the cache on (11.9 → 21.3 s) | **needs the new design.** Linked exits hold absolute host addresses that a whole-buffer format can't carry; POWERarm stores blocks unlinked with link records and relinks on use |
| 2 | Exit guest addresses are forced to fixed width with the cache on: `JIT.cpp` `ExitRIPFixedWidth` | every guest-address load is the 5-instruction form (about 3.8% steady-state in POWERarm) | **needs the new design** (variable-width relocations recorded per site) |
| 3 | Whole-buffer, eager loading | a process pays for code it never reaches | **needs the new design** (per-block lazy install) |
| 4 | No size cap, eviction or cleanup of old builds' caches | unbounded growth across rebuilds | **needs the new design** (namespace LRU with a size cap, stale-build sweep) |
| 5 | No safe concurrent writers (no append-only segments, no `link(2)`/`flock` publishing) | parallel processes (make -j, Wine process trees) can race on the same cache | **needs the new design** |
| 6 | No torn-write detection across crashes or reboots | a torn file after a crash may be loaded | **needs the new design** (entry hashes checked when boot id differs) |
| 7 | Host features (ISA 3.0) not in the cache key | ISA 3.0 code can be served to a run with ISA 3.0 disabled or on POWER8 | **0022** |
| 8 | `FEX_APP_CACHE_LOCATION` without a trailing slash creates a sibling directory | cache written to the wrong place | **0023** |
| 9 | Cache on silently disables SMC store backpatch stubs | degraded SMC handling with no message | **0024** (cache off in that SMC mode) |
| 10 | Whole-file content hash of every mapped executable even with the cache off | startup cost with no benefit | **0025** (cache-off half; with the cache on the hash stays, since it's the only validity check here) |
| 11 | Code maps written unconditionally | extra I/O per process | **0026** |

## The new design in brief (POWERarm v5)

- **Keys:** per-file namespace keyed on file identity (`stat`), emulator build, every host-feature field,
  page size and codegen options.
- **Validation:** every block is validated on install (entry hash; guest bytes hashed after the pages
  are write-protected; relocations apply), so a replaced, forged, corrupted or self-modifying binary
  recompiles instead of running stale code.
- **Storage:** append-only segments published with `link(2)` under `flock` and compacted at eight; a
  size cap with LRU eviction.
- **Linking and relocations:** blocks are stored unlinked and relinked on first use, so linking stays
  on; guest-address loads use variable-width relocations.
- **Measured in POWERarm:** warm zlib and Lua builds about 40% faster than with the cache off.
- **Tested** by `check-code-cache.sh`: 16 parallel compiles, ISA 3.0 on/off on one cache directory,
  in-place binary replacement, a forged identity, corrupted bytes, and self-modifying code before
  and after caching.

Porting it means taking `CodeCache.cpp` and its JIT hooks (link records, relocation recording)
largely as a unit. It isn't a small patch series.
