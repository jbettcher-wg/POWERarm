# Code cache leaks its whole history into XDG_RUNTIME_DIR

Observed 2026-09-23 on `omarchy-power9` (AC922, POWER9 DD2.3, 446G RAM),
powerarm-stable build dated Sep 23. Written from a live system; every number
below is reproducible with the commands in §5.

## 1. Symptom

`/run/user/1000` (tmpfs, 45G = 10% of RAM) reached 100% full, 0 bytes
available. Everything that writes to `XDG_RUNTIME_DIR` then fails with
`ENOSPC`:

```
uwsm_app-daemon[3647473]: [Errno 28] No space left on device
```

User-visible effect: Hyprland keybindings silently launch nothing, and
quickshell reports "invalid argument". The desktop looks broken with no
obvious cause, because `df /` shows 1.4T free and `du /run/user/1000` shows
only 2.5G of files.

## 2. The discrepancy

| | |
|---|---|
| `df /run/user/1000` | 45G size, 45G used, **0 available** |
| `du -sh /run/user/1000` | **2.5G** |
| `free` "shared" column | 42 GiB |
| deleted-but-open files via `/proc/*/fd` | none significant |

`du` and `/proc/*/fd` both miss it. The space is in files that were
**unlinked while still mmap'd**: the fd is closed, the directory entry is
gone, and the pages stay charged to the tmpfs until the last mapping is
dropped.

## 3. Mechanism

Eviction removes the directory entry. It does not unmap. So a long-lived,
multi-process guest accumulates its entire cache history in RAM while the
on-disk cache stays small and the cap stays satisfied.

Measured on one `code` process (VS Code, the arm64 build, pid 2544983):

```
cache mappings held:        589
  still linked on disk:      56
  already deleted:          533     <- pinning tmpfs pages
code-* files on disk:          9
```

Across six `code` processes: 533, 839, 340, 234, 74, 34 deleted mappings
each — roughly 1,500 dead generations pinned by one application. Total
deleted-but-mapped address space under `/run/user/1000`: **51.1 GiB**.

Attribution by program (deleted-but-mapped bytes, virtual, pages shared
between processes):

```
  379.27 GiB   18 procs   code
    1.12 GiB    3 procs   Isolated Web Co
    0.65 GiB    2 procs   claude
    0.10 GiB    ~         bash and assorted command binaries
```

VS Code maps **its own** generations — 494 entries for the `code` binary
plus its libraries. It maps none of the short-lived command binaries, and
those processes exit and release, which is why nothing else on the box shows
the problem. The requirement is simply: long-lived + heavily JIT'd +
multi-process.

## 4. Why the 8G cap does not catch it

The cap is enforced against what is linked. Eviction unlinks. So the act of
evicting is what makes the consumption invisible to the cap — the accounting
goes to zero at the exact moment the pages become unreclaimable. On disk at
the time of measurement: 9 `code-*` files. Resident: ~45G.

## 5. Reproducing the measurement

```sh
# the discrepancy
df -h /run/user/1000; du -sh /run/user/1000

# what is actually pinning the pages (this is the one that finds it --
# /proc/*/fd does NOT, because the fd is already closed)
for m in /proc/[0-9]*/maps; do p=${m#/proc/}; p=${p%%/*}
  awk -v pid="$p" '/\/run\/user\/1000\/.*\(deleted\)/ {
    split($1,a,"-"); s+=strtonum("0x"a[2])-strtonum("0x"a[1])
  } END{ if(s>0) print pid"\t"s }' "$m" 2>/dev/null
done | sort -k2 -rn | head

# linked vs deleted for one process
p=<pid>
grep -cE '/run/user/1000/powerarm/cache/' /proc/$p/maps
grep  -E '/run/user/1000/powerarm/cache/' /proc/$p/maps | grep -c '(deleted)'
```

## 6. Secondary observation: generation forking

Cache files fork per concurrent writer rather than being shared. For a single
binary and context hash:

```
ls-0d51c98b64ee3bab-ee2bbc0f1769898e        499 KiB  17:04
ls-0d51c98b64ee3bab-ee2bbc0f1769898e.1       26 KiB  12:36
ls-0d51c98b64ee3bab-ee2bbc0f1769898e.2 ... .7
ls-0d51c98b64ee3bab-ee2bbc0f1769898e.lock
```

1264 files in the cache directory at measurement time, with 17 entries for
`ls`, 15 for `sed`, 15 for `grep`, 14 for `sort`. Two distinct binary hashes
for `ls` are legitimate here (a sleeve `/usr/bin/ls` and a native
`/mnt/arch/usr/bin/ls`), but the numbered chain under each is not. The
`.lock` file beside each chain suggests fork-on-contention. `Config.json` on
this machine has `"CodeCacheForkWriter": "0"`.

This multiplies the number of generations, which multiplies what §3 leaks.

## 7. Directions

Not prescriptions — you know the code.

- **Unmap on rotation.** Drop the superseded generation's mapping in each
  process that holds it before unlinking, so eviction actually reclaims.
- **`ftruncate(fd, 0)` before `unlink()`.** Releases the pages even where a
  mapping survives; consumers fault on a hole rather than stale code, which
  may or may not be acceptable depending on how generations are referenced.
- **Account the cap against resident bytes, not linked bytes.** As it stands
  the cap measures the one quantity eviction is guaranteed to reduce.
- **Consider a disk-backed cache directory.** `XDG_RUNTIME_DIR` is tmpfs and
  sized at 10% of RAM; on a 446G machine that is 45G, which sounds generous
  and is not. A leak there takes the desktop down rather than filling a disk.

## 8. Not implicated

This was initially suspected to be a 775-package distro rebuild running on
the same box. It is not: that build runs natively over ssh (outside the
emulator), and its `TMPDIR` and `CCACHE_TEMPDIR` point into
`/var/tmp/omarchy-bq-ppc64le`. Claude Code's own command execution accounts
for 0.65 GiB across two processes — those binaries are short-lived and
release on exit.

Immediate mitigation used, for the record, which restores the desktop without
restarting anything:

```sh
sudo mount -o remount,size=64G /run/user/1000
```

Restarting the guest application (VS Code here) is what actually reclaims.
