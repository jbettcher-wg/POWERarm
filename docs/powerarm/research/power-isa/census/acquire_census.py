#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""How many guest acquire loads could drop their leading hwsync?

An AArch64 LDAR lowers to Fence(LoadStore) + the load + Fence(Acquire), i.e.
`hwsync` (53.40 cycles on this POWER9) + load + `lwsync` (12.09). The leading
hwsync is what forbids store-then-load reordering and makes the Dekker handoff in
glibc, JSC's ParkingLot and Bun's thread pool safe; it cannot simply go
(TranslateExclusive.cpp:217-236, and herd7 rejected both trailing-sync and
ctrl+isync). But it is REDUNDANT when an earlier hwsync in the same basic block
already ordered every store that precedes this load and no store has happened
since: the stores it would order were ordered already.

This counts that, statically, on real binaries. It is a LOWER BOUND by
construction, three times over:

  * basic blocks only. A dominating acquire in an earlier block would also serve,
    but that needs a dominator tree over the guest CFG and the emulator could
    only use it inside one compile unit anyway (up to 8 blocks).
  * blocks are split at EVERY control-flow instruction, so a fallthrough pair
    that the emulator would see in one compile unit is counted as two blocks.
  * a call (bl/blr) ends the window, since the callee may store.

It also reports an UPPER BOUND -- any earlier acquire in the block, ignoring
intervening stores -- so the real figure is bracketed rather than guessed.

The distance distribution is the part that speaks to whether this is worth
anything: a redundant hwsync two instructions after its predecessor is sitting in
a tight dependency chain where 53 cycles is the critical path, while one 40
instructions later is likely hidden.

Usage:  llvm-objdump -d --no-show-raw-insn BINARY | acquire_census.py NAME
"""
import sys, re, collections

# Instructions whose lowering emits a FULL hwsync, so they order prior stores.
# ldar/ldarb/ldarh/ldapr: Fence(LoadStore) before the load.
# dmb ish / dmb ishst / dmb sy: Fence(LoadStore) or Fence(Store), both hwsync.
# dmb ishld is Fence(Acquire) -> lwsync, which does NOT order prior stores.
ACQ_LOAD = re.compile(r'^lda(r[bh]?|pr[bh]?)$')
def emits_hwsync(mnem, ops):
    if ACQ_LOAD.match(mnem):
        return True
    if mnem.startswith('ldaxr') or mnem.startswith('ldaxp'):
        return True   # LoadExclusive's leading hwsync
    if mnem == 'dmb':
        return ops.strip() not in ('ishld', 'oshld', 'nshld', 'ld')
    # An LSE RMW or CAS with A or R ordering keeps the full bracket.
    if re.match(r'^(ld(add|clr|eor|set|smax|smin|umax|umin)|swp|cas)', mnem):
        return 'a' in mnem[-2:] or 'l' in mnem[-2:]
    return False

def is_store(mnem):
    # Anything that writes memory, including the release store and the
    # store-exclusive, and the store half of an RMW.
    if mnem.startswith('st'):
        return True
    if re.match(r'^(ld(add|clr|eor|set|smax|smin|umax|umin)|swp|cas)', mnem):
        return True   # an RMW stores
    return False

def ends_window(mnem):
    # A call may store anything; a return or indirect branch ends the block too.
    return mnem in ('bl', 'blr', 'ret', 'br', 'brk', 'svc', 'hvc', 'smc', 'eret')

CF = re.compile(r'^(b|bl|blr|br|ret|cbz|cbnz|tbz|tbnz|b\.[a-z]+|eret|svc|hvc|smc|brk)$')

def main():
    name = sys.argv[1] if len(sys.argv) > 1 else '?'
    total = 0
    elidable = 0            # lower bound: prior hwsync, no store since
    upper = 0               # upper bound: prior acquire in block, stores ignored
    dist = collections.Counter()
    blocked_by_store = 0
    # Per-block state
    last_hwsync_idx = None  # index within the block of the last hwsync emitter
    store_since = False
    any_acq_idx = None
    idx = 0

    def new_block():
        nonlocal last_hwsync_idx, store_since, any_acq_idx, idx
        last_hwsync_idx = None
        store_since = False
        any_acq_idx = None
        idx = 0

    for line in sys.stdin:
        if not line.startswith((' ', '\t')):
            # a symbol header or blank: new block
            new_block()
            continue
        s = line.strip()
        if not s or s.startswith('...'):
            continue
        # "<addr>: mnem ops"  (--no-show-raw-insn)
        p = s.split(':', 1)
        body = p[1].strip() if len(p) == 2 else s
        if not body:
            continue
        f = body.split(None, 1)
        mnem = f[0].lower()
        ops = f[1] if len(f) > 1 else ''

        if ACQ_LOAD.match(mnem):
            total += 1
            if any_acq_idx is not None:
                upper += 1
            if last_hwsync_idx is not None and not store_since:
                elidable += 1
                dist[min(idx - last_hwsync_idx, 64)] += 1
            elif last_hwsync_idx is not None and store_since:
                blocked_by_store += 1
            any_acq_idx = idx

        if emits_hwsync(mnem, ops):
            last_hwsync_idx = idx
            store_since = False
        elif is_store(mnem):
            store_since = True

        if ends_window(mnem) or CF.match(mnem):
            idx += 1
            new_block()
            continue
        idx += 1

    pct = (100.0 * elidable / total) if total else 0.0
    upct = (100.0 * upper / total) if total else 0.0
    print("%-10s acquire loads %8d   elidable(lower) %7d = %5.2f%%   "
          "any-prior-acquire(upper) %7d = %5.2f%%   blocked by a store %6d"
          % (name, total, elidable, pct, upper, upct, blocked_by_store))
    if dist:
        print("%-10s distance from the hwsync it would reuse (instructions):" % "")
        tot = sum(dist.values())
        for d in sorted(dist):
            n = dist[d]
            print("%-12s %3s%s  %7d  %5.1f%%" % ("", d, "+" if d == 64 else " ", n, 100.0 * n / tot))

main()
