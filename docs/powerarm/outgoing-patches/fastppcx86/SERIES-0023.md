# fastppcx86 series 0023: code cache fixes and shared-code performance

Ported from POWERarm to `fastppcx86/daedalao-wt` (9da3ccf9f). Base for
applying and building: daedalao-wt plus the landed patches 0001–0008, 0010,
0011, 0014 and 0016, in that order. None of the patches needs the unlanded
v2 patches 0018–0021 or 0022. Each patch was built (clang, Release,
`-DBUILD_THUNKS=OFF -DBUILD_TESTING=OFF`, LTO off for turnaround) and checked
with static x86-64 programs under the built `FEX` (`FEX_HOSTPAGEMODE=force`,
private cache, data and server socket locations).

| Patch | Type | Depends on | Evidence |
|---|---|---|---|
| 0023 Config: add the missing separator after FEX_APP_CACHE_LOCATION | correctness | — | `FEX_APP_CACHE_LOCATION=/tmp/fxpp-cache/x`: `xcodemap/` before, `x/codemap/` after |
| 0024 FEXInterpreter: turn the code cache off when SMC store backpatching is on | correctness | — | before: "SMC store backpatching enabled" with "code caching on" (every stub allocation refused); after: "code caching off" |
| 0025 SMCTracking: hash mapped executables only when the code cache is enabled | performance | — | cache off: no whole-file XXH3 per mapped executable; cache on: id unchanged (`hello-5f2b88f93d485c2f`) |
| 0026 FEXInterpreter: write code maps only when server cache generation is requested | performance | — | cache on: code map file written before, none after |
| 0027 Telemetry: write the exit counters with one write and no fsync | performance | — | 50 × static exit program, data dir on tmpfs: 114.5 → 112.7 ms/run; POWERarm measured 6–16 ms per process for the fsync on disk |
| 0028 ScalarSplatChain: return early without a producer | performance | — | builds, programs run |
| 0029 PPC64LE JIT: fold the high-zero walk into the mask elision walk; skip spin analysis without a backedge | performance | — | builds, programs run (incl. a branchy loop with SSE, same output as base) |
| 0030 PPC64LE JIT: skip the FPR live-mask scan in units without FPR values | performance | 0029 | builds, programs run |

0024 and 0026 touch different places in `FEXInterpreter.cpp`; the series
applies in order with `git am` on the base above.

## Skipped

- **Block linking forced off with the code cache** (POWERarm 17673ac6a,
  892c0a2c9). The problem exists here: `JIT.cpp` sets
  `BlockLinkingEnabled = BlockLinking() && !ENABLECODECACHINGWIP`. It is not
  separable. This tree's cache serializes the whole code buffer and relocates
  it on load, and linked exits hold absolute host addresses the format cannot
  carry. POWERarm keeps linking on only through its v4 format (per-block
  entries with link records whose unlinked words are derived after
  relocation). Porting needs the v4 format; no patch.
- **Write-protect a block's pages before checking its guest bytes**
  (d3a9714da). The race is in v4's per-block load (`TryLoadBlock` hashes the
  guest bytes, then registers the pages). This tree's cache has no per-block
  guest-byte check: it loads a whole file keyed by a content hash of the
  mapped file and registers the pages afterwards. Not applicable.
- **Identify mapped files by `stat`** (78e3583bf). Only the cache-off half is
  ported (0025). The `stat` id is safe in POWERarm because v4 checks each
  block against the guest bytes; here the content hash is the only validity
  check (see the comment in `ComputeCodeMapId`), so replacing it would allow
  stale code.
- **Code cache load timer only with stats** (1b1235ead). The timer is a v4
  addition; this tree has no per-block load timer.
- **One taken branch per in-unit conditional branch** (bd36bfe32). It builds
  on `NextBlockID`/`BlockEmissionIDs` from POWERarm 310ebcd2d (direct
  conditional exits, not to be ported), and `DEF_OP(CondJump)` here has
  diverged (spin-edge hints, `FallthroughBlockID`). This tree already has the
  fall-into-next-block half as opt-in `FEX_FALLTHROUGH=1`, turned off after a
  measured 4.7% regression from loop-top alignment. The remaining idea (a
  single forward `bc` to a non-next leg, branch islands for range) should be
  re-derived on this tree and measured on its workloads, not ported.
- Host-feature cache key: already patch 0022.

## For reviewers

- **0024** changes which option wins: previously the cache stayed on and
  backpatching was silently ineffective. Only SMCStoreBackpatch is gated.
  POWERarm also turns the cache off for SMCSemanticPatch, SMCLazyInval,
  SMCCheapTier and SMCStoreEmulation; whether this tree's cache can serve
  those is open.
- **0029** is the large one (323+/230−): the producer step now runs one
  non-no-op behind the consumer step in the same walk. POWERarm verified
  that generated code is unchanged; here it was only built and run on small
  programs.
- **0030** fills the dynamic-FPR live masks with zeros for units with no FPR
  destination and no VF*ML*ScalarInsert op. A missed FPR class would drop a
  save across a helper call, so check the class test in the shared walk.
