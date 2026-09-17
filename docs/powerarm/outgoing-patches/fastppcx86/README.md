# fastppcx86 review package (follows the review of 0001–0017)

Everything here is new since your review of 0001–0017. All patches are `git format-patch` files
authored against `daedalao-wt` (9da3ccf9f).

## Contents

| Patches | What | Base to apply on |
|---|---|---|
| 0018–0021 | v2 replacements for the four held patches (0009, 0012, 0013, 0015); `HELD-0009-0012-0013-0015.md` maps each review comment to its fix | daedalao-wt + landed 0001–0008, 0010, 0011, 0014, 0016. Apply 0018–0021 as a set (built and tested as a stack) |
| 0022 | Code cache: hash the detected host features (ISA 3.0 etc.) into the cache config id | same base |
| 0023–0030 | Code cache fixes and shared-code performance; `SERIES-0023.md` has the type, dependencies, evidence and skipped items for each | same base; independent of 0018–0022; 0030 depends on 0029 |
| 0031–0033 | Three FP/SIMD backend correctness bugs found by POWERarm's scalar-FP research: `VFNMLA`/`VFNMLS` negating after rounding, `Float_FromGPR_S` double-rounding i64→f32, and 32-bit `VAddV` saturating. Independent of everything above; apply in order (0031 touches `Emitter.h` too) | `daedalao-wt` directly |

`CODE-CACHE-GAPS.md` compares this tree's code cache with POWERarm's rewrite, and says which gaps
these patches close and which need the new design.

## Apply

```
git switch -c review daedalao-wt
git am <landed 0001-0008 0010 0011 0014 0016>   # if not already on your branch
git am 0018-*.patch 0019-*.patch 0020-*.patch 0021-*.patch
git am 0022-*.patch
git am 0023-*.patch … 0030-*.patch
```

## Worth a close look

- **0018–0021:** each v2 addresses the specific review comment (SMC-protected granules during vfork
  copy-back; the per-path byte loop; the fd table sized to NOFILE; early-log buffering hiding errors).
- **0024:** SMC store backpatching now turns the code cache off, instead of the cache silently
  disabling backpatch stubs. Only this SMC mode is gated.
- **0029:** a large rewrite of the backend analysis walk. It was verified in POWERarm (generated code
  unchanged, all gates), but only built and run on small x86-64 programs here.
- **0030:** check the FPR class test; a missed class would drop an FPR save across a helper call.
