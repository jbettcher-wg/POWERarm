# The rootfs path cache: what the syscalls really are, and why not to cache them

Measured 2026-10-01 on the POWER9 AC922, at `0caae5503`. The cache is declined;
the only thing that landed is §6's `O_DIRECTORY` fast path, measured at
`2c9542104` and written up in §6 with its own cost on the paths it does not
help. The unsound measurement probe behind §2 is reproduced in §7 so those
numbers can be re-derived without it living in the tree.

HANDOVER item 56 closed the kernel-mitigation question and left behind one
actionable finding: POWERarm issues 2.9 host syscalls per guest syscall, and
**53% of a `gcc -c` run's host syscalls are rootfs path resolution**. The
proposal was a guest-path → (tier, resolved fd) cache that also memoises
`ENOENT`, as "the largest single cut available".

Three things turned out to be true, in this order:

1. **The syscalls are not what the item says they are.** They are not two-tier
   overlay probing — the overlay is *inactive* for that workload. They are a
   three-syscall trampoline inside `FileManager::Readlink`/`Readlinkat`, and
   **55%** of a compile's host syscalls are that one call shape, every instance
   of it answering "not a symlink" (§1).
2. **A cache cannot be made sound at a price worth paying**, and the reason is
   structural rather than a matter of care: `openat2(RESOLVE_IN_ROOT)` validates
   the *entire* path prefix in one syscall, and any cache that skips it must
   re-validate that prefix, for which there is nothing cheaper than the
   `openat2` it was trying to avoid (§3, §4).
3. **It does not matter, because the whole thing is worth 0.00% of the
   compile.** An unsound probe that memoises with no invalidation at all cuts
   **every host syscall of the run by 34.4%** (81 418 → 53 425) and moves the
   warm slice by **+0.00%** (median of 7, interleaved) and the emulator's
   kernel CPU by **0.020 s out of 16.9 s = 0.12%** (§2).

So: **declined.** Item 56's own arithmetic already predicted this (4,383
syscalls/s × ~250 ns = 0.029% of the compile's CPU); the error was calling the
largest *syscall-count* cut "the largest cut available" in a workload where
syscall count had just been shown not to be a cost. §6 is the one sound
improvement that does exist; it has since landed, and it too measures at zero,
on one compile and on a 32-way parallel build.

---

## 1. What the syscalls are

`strace -c -f` on the slice (10 Lua objects at `-O2`, plus `ar` and `gcc -r`),
warm cache, `POWERARM_PORTABLE=1` so child `cc1`/`as` execs run the build under
test — see §8, this matters and is easy to get wrong:

| syscall | calls | errors |
|---|---|---|
| `close` | 13 744 | 0 |
| `openat2` | 13 537 | 2 735 |
| `readlinkat` | 12 864 | 10 679 |
| `mmap` | 6 082 | 2 333 |
| `openat` | 5 135 | 2 430 |
| `newfstatat` | 3 854 | 1 120 |
| *total* | **81 943** | 22 492 |

A raw trace of a single `gcc -c lapi.c` (4 279 traced path syscalls) resolves
the shape exactly. **799 of them are this triple:**

```
openat2(RootFSFD, "usr/local/include", {O_PATH|O_NOFOLLOW|O_CLOEXEC,
                                        resolve=RESOLVE_IN_ROOT}) = 4
readlinkat(4, "", ...)                  = -1 ENOENT   <- "not a symlink"
close(4)                                = 0
```

799 × 3 = 2 397 syscalls, **55.0% of the traced total**, and the
`readlinkat` returned `ENOENT` **799 times out of 799** — the kernel's
empty-pathname spelling of "not a symlink", which `FileManager` translates to
the `EINVAL` that `realpath(3)` needs.

The guest is `gcc`'s `lrealpath`, which `readlink()`s every prefix of every
include path it tries: `/usr`, `/usr/local`, `/usr/local/include`, then the
header, for each of ~50 candidate directories. The guest's redundancy is the
guest's business; POWERarm's contribution is multiplying each of those calls
by three.

It is worth being clear that a cache would *work*, in the hit-rate sense. The
799 trampolines name only **80 distinct paths**, `usr` alone 182 times:

```
182  usr                                              733 of 799 (91.7%) are directories
 62  usr/lib                                           66 of 799 ( 8.3%) are regular files
 62  usr/lib/gcc                                        0 of 799         are symlinks
 62  usr/lib/gcc/aarch64-unknown-linux-gnu
 62  usr/lib/gcc/aarch64-unknown-linux-gnu/16.1.1
 62  usr/lib/gcc/aarch64-unknown-linux-gnu/16.1.1/include
 59  usr/local
 59  usr/local/include
```

A 90% hit rate on an 80-entry table. That is why this looked like the obvious
win, and it is why the decline rests on §3/§4 (it cannot be made sound cheaply)
and §2 (it is worth nothing) rather than on the cache being ineffective.

**The overlay is not involved.** There is no `ArchLinuxARM-m2-overlay`, so
`RootFSOverlay::Active()` is false and `TRY_OVERLAY` returns immediately. The
two-tier probing item 56 attributes these syscalls to is not running. (It would
be for the vk rootfs — and note that when the overlay *is* active it answers
`Readlinkat` before the trampoline is reached, so a cache placed there would be
bypassed exactly in the configuration item 56 describes. `RootFSOverlay` has its
own caches, `OpaqueCache` and `Listings`, already mtime-validated.)

## 2. What removing them buys: nothing

Two probe levels (§7), both deliberately unsound, to bound any cache from above:

* **level 1** — collapse the trampoline to a single
  `readlinkat(RootFSFD, rel)`. Unsound (plain resolution; an intermediate
  absolute symlink escapes the rootfs) but strictly faster than any sound
  cache, which must do ≥ 1 syscall *plus* a hash of the path.
* **level 2** — plus an unbounded, never-invalidated in-process memo: zero
  syscalls on a repeat. The absolute floor of any cache whatsoever.

Traced path syscalls for one `gcc -c`: **4 279 → 2 684 → 1 851** (−37%, −57%).
Across the whole slice, **every** host syscall, by name
(`strace -c -f`, one run each, `POWERARM_PORTABLE=1`):

| syscall | level 0 | level 1 | level 2 | Δ1 | Δ2 |
|---|---|---|---|---|---|
| `openat2` | 13 537 | 2 919 | 2 919 | −10 618 | −10 618 |
| `close` | 13 686 | 4 418 | 4 418 | −9 268 | −9 268 |
| `readlinkat` | 12 864 | 14 214 | 4 734 | **+1 350** | −8 130 |
| `mmap` | 6 021 | 6 017 | 6 015 | −4 | −6 |
| `openat` | 5 046 | 5 046 | 5 047 | 0 | +1 |
| `fstat` | 3 886 | 3 886 | 3 886 | 0 | 0 |
| `newfstatat` | 3 811 | 3 803 | 3 834 | −8 | +23 |
| `read` | 2 885 | 2 885 | 2 885 | 0 | 0 |
| `access` | 1 966 | 1 966 | 1 966 | 0 | 0 |
| `readlink` | 1 120 | 1 120 | 1 120 | 0 | 0 |
| **total** | **81 418** | **62 869** | **53 425** | **−22.8%** | **−34.4%** |
| *of which errors* | 44 818 | 44 820 | 25 858 | | |

Level 1's `readlinkat` *rises* by 1 350: one direct `readlinkat` replaces the
`openat2`+`readlinkat`+`close` triple, and the paths that used to fail at the
`openat2` now fail at the `readlinkat` instead. Nothing outside the three
trampoline syscalls moves, which is the check that the probe changed only what
it claimed to.

Warm slice, interleaved 0/1/2 × 7 reps, CPU 104, **busy box** (load average
~8 from other work on the machine; the quiet-box figure for this compile is
0.06%, so treat anything under ~4% as noise):

| probe | median | min | max | spread | vs level 0 |
|---|---|---|---|---|---|
| 0 (today) | 16.87 s | 16.80 | 17.41 | 3.6% | — |
| 1 (1 syscall) | 16.88 s | 16.78 | 17.54 | 4.5% | **+0.06%** |
| 2 (0 syscalls) | 16.87 s | 16.74 | 17.42 | 4.0% | **+0.00%** |

Kernel CPU is the sharper instrument, and it agrees. Medians of 5, interleaved
(bash `TIMEFORMAT='%R %U %S'`; there is no `/usr/bin/time` on this box):

| probe | wall | user | sys | sys/wall |
|---|---|---|---|---|
| 0 | 16.86 s | 16.34 s | 0.474 s | 2.81% |
| 1 | 16.80 s | 16.30 s | 0.464 s | 2.76% |
| 2 | 16.77 s | 16.27 s | 0.454 s | 2.71% |

The emulator's **entire** kernel CPU for the slice is 2.8% of wall, and
removing 28 000 syscalls takes **0.020 s** off it — monotone across the three
levels, which is the only reason to believe it at all, since it sits inside the
level-0 `sys` spread of 0.452–0.491 s. 0.020 s on a 16.9 s compile is
**0.12%**, and 28 000 × ~250 ns = 7 ms is the right order. There is no 1%
hiding here, let alone 10%. **The path-resolution syscalls are not a cost on
this workload.**

Short, resolution-heavy workloads were checked in case the compile was hiding
something behind 16 s of translation — medians of 5, level 0 → level 2:

| workload | wall | sys |
|---|---|---|
| `/usr/bin/true` | 0.013 → 0.011 s | 0.008 → 0.007 s |
| `gcc -c empty.c` | 0.097 → 0.097 s | 0.028 → 0.029 s |
| `find /usr/include -type f` | 0.022 → 0.023 s | 0.010 → 0.011 s |

All inside a 1 ms timer's noise. `find` is the instructive one: a pure
path-walk workload that does **not** improve, because `find` walks with
`getdents64` and `fstatat` against directory fds it already holds, which never
enter the trampoline. The guest programs that do enter it are the ones calling
`readlink()` on absolute paths — `realpath(3)`, and `gcc`'s `lrealpath` above
all.

Chrome was not measured: `strace` cannot measure it (item 56's trap — 253×
dilation multiplying `futex`/`epoll_pwait`/`ppoll`), and its *ceiling* is
already known from item 56 to be 0.163% of its CPU with a 1.53% pessimistic
bound. A cache cannot beat its own ceiling, so there was nothing to learn
from spending a browser session on it.

## 3. The invalidation story, worked through

Written out in full because the conclusion is a structural one, not "we ran out
of care", and because the reasoning is what makes the decline reusable.

### 3.1 The guest writes — easy, and *demonstrably* necessary

Every guest mutation reaches `FileManager`/`RootFSOverlay`, so it can be seen
precisely. What a cache must do is drop every entry whose guest path is the
mutated path **or has it as a prefix**, which over a hash map means a trie or a
global epoch bump. A conservative "any guest path mutation under the rootfs
flushes the cache" is trivially sound and costs the slice nothing (gcc's writes
are to `/tmp` and the cwd); guest `pacman` would flush continuously and get no
benefit, which is fine.

This is the *easiest* class, and `invtest.c` + `invrun.sh` beside this document
show an uninvalidated cache getting all five of these wrong, inside one guest
process, against a throwaway fixture rootfs in `/tmp`:

| case | correct | uninvalidated cache |
|---|---|---|
| 1. memoised `ENOENT`, then the guest creates the symlink | `"tgt1"` | `ENOENT` |
| 2. memoised "not a symlink", then the guest replaces it with one | `"tgt2"` | `EINVAL` |
| 3. memoised target, then the guest deletes the symlink | `ENOENT` | `"tgt3"` |
| 4. symlink retargeted (`unlink` + `symlink`) | `"after"` | `"before"` |
| 5. an **intermediate** component becomes a symlink | `ENOENT` | `EINVAL` |

Case 5 is the one that decides the design (§4): `/pafix/mid/leaf`'s answer
changed although `leaf` itself was never touched.

### 3.2 The host writes — the one that cannot be made cheap

`sleeve` and the rootfs tooling modify the tree from outside the emulator, and
other POWERarm processes share it. Nothing notifies us. The options:

* **Revalidate per use.** Sound, and costs ≥ 1 syscall per lookup — which
  collapses the win to 3 → 1 at best, and §4 shows even that does not stand up.
* **inotify.** Precise, and **rejected on resource grounds**: `max_user_watches`
  is a finite per-user resource this box shares with the owner's live VS Code
  and Chrome, both heavy watch consumers; exhausting it breaks the owner's
  editor, not just the cache. The fd is also shared across `fork` (two
  processes draining one queue steal each other's events), watches are needed
  on *both* tiers per cached directory, and `IN_Q_OVERFLOW` forces a full flush
  anyway.
* **A TTL.** Bounds the damage but does not remove it: for the TTL window the
  guest gets a *wrong answer*, not a race.
* **Nothing.** Unacceptable. A long-lived guest — Chrome, VS Code, a `claude`
  session — would hold the wrong answer for its entire life after `sleeve`
  touched the tree. That is not a race window, it is a permanent lie, and it is
  the file-system-visible corruption class.

### 3.3 Deletions and whiteouts

A whiteout appearing is a directory write in the overlay: guest-side it is
§3.1, host-side it is §3.2. It also changes the answer *without touching the
leaf's own inode* (base symlink → `ENOENT`), so with the overlay active a
leaf-only validator is insufficient and the floor is two probes, not one.

### 3.4 Symlinks

`RESOLVE_IN_ROOT` re-roots absolute symlinks at *every* component, and
`Source/Common/RootFSCheck.cpp` relies on it. Symlink targets are immutable, so
a target "changing" is always `unlink` + `symlink` — a directory write, hence
§3.1/§3.2 again. The sharp edge is specific to caching a **resolved fd**: an fd
pins an *inode*, so if the guest path is later re-pointed, the cached entry
answers about the old inode **permanently and undetectably** — its `fstat` is
unchanged while the binding has moved. That makes fd caching strictly worse
than answer caching, which can at least be revalidated.

### 3.5 File descriptors

Dissolved by not caching fds — and there are three independent reasons not to:
the budget competes with the guest's own (Chrome runs near `RLIMIT_NOFILE`) in
a way invisible to the guest's own accounting; an `EMFILE` originating in the
cache would surface as a spurious failure in an unrelated guest `open`; and
decisively, **the cache's fds would appear in `/proc/self/fd`, which
Chromium/CEF sweeps and closes** — `FileManager::IsProtectedFile` exists
precisely because guests close FEX's descriptors. The cache would have its
fds closed out from under it by one of the three target applications.

### 3.6 Threads

This sits on every guest thread's syscall path, up to 176 of them. The existing
precedent is `RootFSOverlay::OpaqueMutex`/`ListingMutex`, plain `std::mutex`,
already there. A new global lock here is a shared cache line per path syscall;
the declined L3 item is the analogy — measure first. With §2 showing nothing to
win, adding synchronisation to this path is pure risk.

### 3.7 fork and exec

Answer caching across `fork` is correct and free: the child inherits a copy and
the same filesystem view. fd caching is not — the child shares the fds, and the
emulator's own re-exec paths (`execveat("/proc/self/exe")`, and the binfmt path
of §8) need every cached fd `CLOEXEC` or it leaks into the guest's fd space,
where §3.5 then applies.

## 4. Why the cache cannot beat the code that is already there

Put §3.1 case 5 next to §3.2 and the floor appears:

* The answer to `readlink(P)` depends on **every component of P's prefix**, not
  just on P's leaf (case 5). So a validator must cover the whole prefix.
* `openat2(RootFSFD, rel, RESOLVE_IN_ROOT)` **validates the entire prefix,
  re-rooting every absolute symlink, in exactly one syscall.** That is the
  cheapest complete prefix validation the kernel offers; there is no
  `readlinkat2` and no resolve flag on `statx`.
* Therefore a sound cache's best case is: one syscall to revalidate, which is
  the `openat2` it was trying to skip — and once you have performed it you are
  one `readlinkat` and one `close` from the answer anyway.

The existing code is already at the floor for a *sound* answer. The only way to
1 syscall is to give up the scoping, which is the containment breach
`OpenPathInRootFS` was written to prevent (its comment names the case: a rootfs
`libtinfo.so.6` resolving through to the host's PowerPC `libncursesw`).

The structural alternative that *would* collapse all of this to one syscall is
to stop needing `RESOLVE_IN_ROOT` at all — make the rootfs the process's
resolution root via a mount namespace and `pivot_root`. That is a change to the
whole process model, it needs user namespaces, and `CLONE_NEWUSER` collides
with Chromium's sandbox (HANDOVER item 16). Not now; recorded because it is the
only thing that changes the floor.

## 5. What a cache would deliberately not cover

Recorded for whoever revisits this with a workload where the syscalls do cost:

* **Anything with the overlay active** — `TRY_OVERLAY` answers first, so the
  cache would never see those paths (§1).
* **`FollowSymlink = true` entry points** (`Stat`, `Access`, `Statx`, `Chmod`,
  `Truncate`): the leaf is followed, so an absolute leaf symlink needs
  re-rooting and `openat2` is unavoidable.
* **Paths containing `..`** — plain resolution and `RESOLVE_IN_ROOT` differ
  (`..` above the root is clamped), so the fast path must be refused for them.
  Cheap to check in-process.
* **Thunk-overlay paths** (`Path.FD == AT_FDCWD`) and host-only paths — a
  different namespace with different rules.
* **`/proc` and magic symlinks** — `EXDEV` already has a documented fallback
  that must not be memoised.
* **Negative entries for a path whose parent is missing from the rootfs** — the
  host fallback is part of the answer, and the host's view is §3.2's problem
  twice over.

## 6. The one sound improvement — done

There is a sound way to take one syscall off the common case, with no cache, no
staleness and no invalidation. **91.7%** of the 799 trampolines ask about a
**directory** and not one of them about a symlink (§1), because the guest
readlinks every prefix component. Add `O_DIRECTORY`:

```c
openat2(RootFSFD, rel, {O_PATH|O_NOFOLLOW|O_CLOEXEC|O_DIRECTORY,
                        resolve = RESOLVE_IN_ROOT})
```

succeeds only if the leaf is a directory — hence not a symlink, hence
`readlink` is `EINVAL` — so the `readlinkat` is skipped and only the `close`
remains: **2 syscalls instead of 3**, with the scoping intact.

This section originally read "recorded, not done", on the grounds that §2
values it at zero. It has since been **done**, in `Readlink` and `Readlinkat`,
because "we removed a third of the system calls and then kept them because the
removal did not move a compile time" is not a defensible place to end up: the
syscalls go whether or not they were costing anything. §6.1 is what it costs on
the paths that are *not* directories, §6.2 is why nothing cheaper exists, §6.3
is what it bought (approximately nothing, as predicted), and §6.4 is where it
does not apply. All measured on the same box at the same settings as §1 and §2.

### 6.1 `ENOTDIR` is the only errno that has to re-ask

`O_DIRECTORY` adds exactly one failure to the open: the `d_can_lookup()` test
at the end of `path_lookupat()`, reported as `ENOTDIR`. It changes nothing
about the resolution, so every other errno is the one the probe would have
produced without the flag, is final, and costs nothing extra. `ENOENT` is the
important member of that set — a guest path the rootfs does not have, which the
host fallback then answers, 2 735 of the slice's 13 537 `openat2`s.

Only `ENOTDIR` is ambiguous, because it conflates "the leaf exists and is not a
directory" with "an intermediate component is not a directory", so it re-probes
without the flag and the original three syscalls follow. Classified from a
trace of one `gcc -c lapi.c` against the built change:

| the `O_DIRECTORY` probe | count | syscalls before | after |
|---|---|---|---|
| succeeded — the leaf is a directory | 733 | 3 | **2** |
| `-1 ENOTDIR` — re-probed, and that second open then succeeded 66/66 | 66 | 3 | **4** |
| `-1 ENOENT` — not in the rootfs; the host fallback follows, as before | 116 | 1 | 1 |
| *total probes* | **915** | | |

**So a non-directory costs one extra syscall**: 733 saved against 66 added,
**−667** per compile, and a path that fails `ENOTDIR` at an intermediate
component likewise pays 2 where it paid 1. By name, one compile's traced path
syscalls: `readlinkat` 1 100 → 367, `openat2` 1 153 → 1 219, `close` 1 156 →
1 146, total **4 267 → 3 598 (−15.7%)**.

This is a bet on the shape §1 measured, not a universal improvement. A guest
whose readlinks were mostly of *files* would lose by the same arithmetic, up to
+33% of the trampoline's syscalls in the limit. The shape holds for the reason
§1 gives — the guests that readlink at all are walking a path one component at
a time, and all but the last component of a path is a directory by
construction — so the ratio is a property of `realpath(3)`, not of gcc.

A **symlink to a directory** is in the `ENOTDIR` group and not in the success
group, which is the property the whole thing rests on: `O_NOFOLLOW` with
`O_PATH` opens the symlink *itself*, which cannot be looked up, so the open
fails and the full probe runs and returns the target. Confirmed in the trace
rather than assumed — `openat2(..., "rlfix/to_dir", O_PATH|O_NOFOLLOW|
O_DIRECTORY, RESOLVE_IN_ROOT) = -1 ENOTDIR`, followed by the probe without the
flag and a `readlinkat` returning `"dir"` — and pinned by
`unittests/A64Frontend/readlinkerr.c`, 26 checks over the errno matrix
(§6.5).

One semantic difference, for completeness: the `ENOTDIR` path now resolves the
guest path **twice**, so a concurrent rename between the two can make the
second resolution land on a different inode, where before the `openat2` pinned
one inode and the `readlinkat` asked about that. The answer is still one a
single correctly-timed resolution could have produced — `readlink(2)` offers no
atomicity against a concurrent rename — and `RESOLVE_IN_ROOT` is in force on
both probes, so the containment property is never weakened. It is a wider
window on a race that was already there, not a new class of answer.

### 6.2 Nothing cheaper exists

The question worth asking before accepting the extra probe is whether the
`O_PATH` fd the code already has can answer "is this a symlink" without a
second syscall. **It cannot.** `openat2` hands back a descriptor and nothing
else; an `O_PATH` fd yields no attribute to userspace without a syscall on it,
and the candidates are `readlinkat(fd, "")`, which is what is already there and
also produces the target, or `fstatat(fd, "", AT_EMPTY_PATH)`, which gives the
type but not the target and so would need a third syscall for a symlink. The
existing code is therefore already using the best available second syscall, and
the only way to remove it is to fold the test into the open — which is what
`O_DIRECTORY` does.

The one alternative fold is `RESOLVE_NO_SYMLINKS` (with `O_NOFOLLOW` dropped,
so a leaf symlink is `ELOOP` rather than silently opened): it succeeds when *no
component* of the path is a symlink, so it would answer regular files as well
as directories. On this workload it is strictly better on paper — all 799 of
the trampoline paths are symlink-free end to end, so it would save 799 rather
than 667 — and it was **not** taken:

* It is worth 132 syscalls out of 80 601, 0.16% of the run, against §6.3's
  measurement of zero. There is nothing to buy.
* It makes the fast path's success depend on the **whole prefix** again, which
  is the exact coupling §4 identifies as what defeats caching here. `ENOTDIR`
  comes out of a check performed *after* resolution, so `O_DIRECTORY` leaves
  the resolution bit-identical to today's; `RESOLVE_NO_SYMLINKS` changes what
  resolution is attempted, in the most correctness-sensitive file in the tree.
* The rootfs's own top level has `bin`, `lib` and `sbin` as symlinks, so every
  guest path through them — `/lib/ld-linux-aarch64.so.1`, `/bin/sh` — would
  take the slow path and pay the extra syscall. gcc's include paths are all
  under `/usr`, which is why they are clean; nothing guarantees that for the
  next guest.

### 6.3 What it bought: nothing, as predicted

Whole slice, `strace -c -f`, warm, `POWERARM_PORTABLE=1`, two independent
interleaved pairs:

| syscall | before | after | Δ | second pair |
|---|---|---|---|---|
| `readlinkat` | 12 864 | 4 358 | **−8 506** | **−8 506** |
| `openat2` | 13 537 | 14 299 | **+762** | **+762** |
| `close` | 13 543 | 13 613 | +70 | +150 |
| `newfstatat` | 3 513 | 3 638 | +125 | +165 |
| `openat` | 4 975 | 5 052 | +77 | +123 |
| **total** | **80 601** | **73 383** | **−7 218 (−9.0%)** | −6 849 (−8.4%) |
| *of which errors* | 22 424 | 14 726 | −7 698 | −7 781 |

The two deltas that are the change are **exact and reproduce to the call**:
`openat2` +762 (the 66-per-compile `ENOTDIR` re-probes, ten compiles plus `ar`
and `gcc -r`) and `readlinkat` −8 506. The rest is a common-mode block of
+70…+165 that also moves between two runs of the *same* build — baseline
`close` came back 13 543, 13 658 and 13 657 on three runs, and baseline `mmap`
23 712 and 5 985 on two — so it is not attributable to this. `openat2` and
`readlinkat` themselves were identical to the call on every baseline run, which
is why the −8 506 can be believed.

Kernel CPU, the sharper instrument. Single compile, medians of 7 interleaved,
quiet box (load average ~2.3, `TIMEFORMAT='%R %U %S'`, CPU 100):

| | wall | user | sys |
|---|---|---|---|
| before | 16.840 s | 16.310 s | 0.486 s |
| after | 16.795 s | 16.260 s | 0.475 s |
| Δ | −0.27% | −0.31% | −0.011 s, −2.3% |

The direction is consistent — the patched run was lower in 6 of the 7
interleaved pairs on wall and on user — and the magnitude is **not resolved**:
the `sys` spread is 9% (0.465–0.509 s before) and 7 218 syscalls at ~250 ns is
1.8 ms, an order of magnitude below the 11 ms the medians moved. A −9% syscall
cut cannot beat the −34% cut of §2, which bought 0.020 s; this is the same
answer, which is 0.0%.

The aggregate case, which is the one worth caring about: a **32-way parallel
build**, 136 `gcc -c` jobs, each its own POWERarm process, on CPUs 0-87.
`%S` on the waiting pipeline is the whole reaped descendant tree, so it is the
job's total kernel CPU. Two independent series, 5 and 8 reps, interleaved:

| series | before `sys` | after `sys` | Δ |
|---|---|---|---|
| 1 (5 reps) | 8.126 s | 8.248 s | **+1.5%** |
| 2 (8 reps) | 8.244 s | 8.160 s | **−1.0%** |
| pooled (13) | 8.242 s | 8.172 s | −0.85% |

**The two series disagree in sign**, both inside their own 2–7% spread, against
199 s of user CPU for the job. So: **it does not move there either.** Fewer
syscalls each did not become less kernel time or less contention across the box
at 32-way concurrency, and the honest reading is that 100 000 fewer syscalls
spread over 136 processes is 0.025 s of kernel work in a job that spends 8 s in
the kernel and 199 s in userspace. §2's conclusion stands at every scale
measured: **the path-resolution syscalls are not a cost on this workload.**

### 6.4 Where it does not apply

With the overlay active, `RootFSOverlay::Readlinkat` answers from its own
`Lookup` before `FileManager`'s trampoline is reached (§1), so this never runs
for a configuration with an overlay — the vk rootfs. It is also bypassed for
`/proc/self/exe` and friends, which `Readlink` answers directly, and for the
`EXDEV`/`ENOSYS` fallback, where the plain `openat` carries `O_DIRECTORY` too
and a magic link therefore fails it and takes the full probe.

### 6.5 The test

`unittests/A64Frontend/readlinkerr.c` is the errno matrix, 26 checks: a
directory, a regular file, a nested file, a dangling symlink, a symlink to a
directory, a symlink to a file, a symlink to a symlink, a self-referential
symlink, a missing path, intermediate components that are each of a symlink,
two symlinks, a regular file, a dangling symlink and a self-referential one,
in-tree `..` through both a real directory and a symlink, the dirfd and
empty-pathname forms of `readlinkat`, and the degenerate `bufsiz` cases the
shortcut must not change (`bufsiz == 0` on a directory is `EINVAL` for a second
reason, and the shortcut answers `EINVAL` without reaching the kernel's check).

`run.sh` runs it twice: once with its fixture as `POWERARM_ROOTFS`, so the
trampoline answers every path, and once against a host path the rootfs lacks,
so the host fallback does. Requiring those two to be byte-identical is the real
check, because it compares POWERarm's reconstruction of `readlink` against the
kernel's own answer for the same tree. They were identical before the change
and identical after it, and identical to each other in both.

## 7. Reproducing

The probe is **not in the tree**. It was two edits to
`Source/Tools/LinuxEmulation/LinuxSyscalls/FileManagement.cpp`, gated on
`POWERARM_RLPROBE`, inserted in `Readlink` and `Readlinkat` immediately after
`GetEmulatedFDPath` and before `OpenPathInRootFS`:

* level ≥ 1: when `Path.FD` is the rootfs fd, answer with a single
  `readlinkat(Path.FD, Path.Path, buf, bufsiz)`; `ENOENT` falls through to the
  host path exactly as an `openat2` `ENOENT` does, every other errno is final.
* level ≥ 2: in front of that, an unbounded `unordered_map<path, {errno,
  target}>` under a `std::mutex`, never invalidated.

Measurement scripts and the invalidation program are reproduced from this
session's scratch in the paragraph below; none of them touch the owner's
rootfs — the invalidation fixture is a throwaway tree in `/tmp` holding a
statically linked `invtest` plus five fixture paths, used as
`POWERARM_ROOTFS`, and the mutations land in it because
`FileManager::Symlinkat` is host-first and falls back to the rootfs on
`ENOENT` (so the fixture paths are under `/pafix`, which the host lacks).

* syscall histogram: `strace -c -f` around the slice command, `POWERARM_PORTABLE=1`.
* trampoline census: `trampoline-census.py <trace>` beside this document.
* timing: the slice interleaved across probe levels, medians of 7.
* kernel CPU: bash `TIMEFORMAT='%R %U %S'` (there is no `/usr/bin/time` on this box).

## 8. Trap: the slice measures the *stable* emulator unless you stop it

Worth more than the rest of this document.

`gcc` spawns `cc1` and `as`, which carry ~97% of the slice's CPU. POWERarm's
`execve` handler, when `IsBinfmtCompatible`, execs the guest binary **directly**
and lets `binfmt_misc` pick the interpreter — and the registered interpreter is
`~/.local/opt/powerarm-stable/Bin/POWERarm`, not the build under test. So a
plain `slice.sh <build>/Bin/POWERarm` measures the build for the `sh` and `gcc`
driver processes and **the stable install for everything that matters**. This
cost an hour here: the first probe run showed `openat2`/`readlinkat` counts
unchanged to the call, because the probe was only ever running in the driver.

`POWERARM_INTERPRETER_INSTALLED=0` does **not** fix it —
`FEXInterpreter.cpp:627` `Config::Set`s that option unconditionally, overriding
the environment layer. The lever is **`POWERARM_PORTABLE=1`**, which makes
`QueryInterpreterInstalled` return false so children exec `/proc/self/exe`.
Pass `POWERARM_ROOTFS=<absolute>` with it (the portable + named-rootfs trap).
Verify, don't assume:

```
strace -f -e trace=execveat … | grep execveat
  # binfmt:   execveat(AT_FDCWD, "<rootfs>/usr/lib/gcc/…/cc1", …)
  # own build: execveat(AT_FDCWD, "/proc/self/exe", …)
```

This is survivable in the owner's day-to-day because `powerarm-stable` is
promoted often (it was refreshed the morning of this measurement), so stable ≈
main and the slice's numbers happen to be about the right code. For a worktree
change it is never the right code.

## 9. Conclusion

* The 53% is real, is `Readlink`/`Readlinkat`'s three-syscall trampoline rather
  than two-tier probing, and is **worth 0.00%** of the reference compile.
* A sound cache cannot do better than one `openat2` per lookup, which is the
  syscall it exists to avoid; an unsound one is wrong in all five guest-write
  cases and permanently wrong under a host write.
* §6's `O_DIRECTORY` fast path **landed**: 2 syscalls instead of 3 for the
  91.7% whose leaf is a directory, 4 instead of 3 for the 8.3% whose leaf
  exists and is not, −9.0% of every host syscall in the slice, and — measured
  both on one compile and on a 32-way parallel build — **no resolvable change
  in kernel time at either scale**. It is there because the work is smaller,
  not because it is faster.
* **Closed.** Do not revisit for the compile or the browser. Revisit only with
  a workload whose kernel CPU share is an order of magnitude higher than the
  2.8% measured here, and when it is, §4 is the reason there is no 55% left
  and §6.2 is the reason there is nothing cheaper than what is now there.
