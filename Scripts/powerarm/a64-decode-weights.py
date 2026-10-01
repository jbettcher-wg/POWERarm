#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Produce (and check) the A64 decode table's bucket-order weights.

    a64-decode-weights.py <a64.inc> <out-weights.txt> <guest-binary>...

Classifies every executable word of each AArch64 guest binary through the
decode table and writes, per a64.inc entry, how often it wins -- averaged
over the binaries so one large program does not dictate the order.
FEXCore/Scripts/a64_decode_table_generator.py reads the result and uses it to
order each bucket; see that script for why the order cannot change which
entry a word decodes to.

Also prints the expected scan depth of the current and candidate tables, and
with --leave-one-out, the depth each binary gets from weights computed without
it -- the only honest check that the order generalises past these binaries.

Needs numpy.  Run by hand when the reference set changes, not by the build.
"""
import argparse
import os
import struct
import sys
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'FEXCore', 'Scripts'))
import a64_decode_table_generator as G  # noqa: E402


def index12(w):
    """The index DecodeTable.cpp used before this work: bits [29:22], [13:10]."""
    return ((w >> 10) & 0x00F) | ((w >> 18) & 0xFF0)


def index14(w):
    """Bits [31:22] and [13:10], which is what the old comment claimed."""
    return ((w >> 10) & 0x00F) | ((w >> 18) & 0x3FF0)


VARIANTS = {12: (index12, 1 << 12), 14: (index14, 1 << 14)}


def build_buckets(order, bits):
    index, count = VARIANTS[bits]
    buckets = [[] for _ in range(count)]
    for e in order:
        fixed = index(e.mask)
        want = index(e.expect)
        free = ~fixed & (count - 1)
        sub = free
        while True:
            buckets[want | sub].append(e)
            if sub == 0:
                break
            sub = (sub - 1) & free
    return buckets


def exec_words(path):
    """The 4-byte words of every SHF_EXECINSTR section of an AArch64 ELF."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        raise SystemExit(f'{path}: not a little-endian 64-bit ELF')
    if struct.unpack_from('<H', data, 18)[0] != 0xb7:
        raise SystemExit(f'{path}: not AArch64')
    e_shoff, = struct.unpack_from('<Q', data, 0x28)
    e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x3a)
    parts = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        sh_type, sh_flags, _addr, sh_offset, sh_size = struct.unpack_from('<IQQQQ', data, o + 4)
        if sh_type == 8 or not (sh_flags & 0x4):  # SHT_NOBITS / not executable
            continue
        n = sh_size & ~3
        if n:
            parts.append(np.frombuffer(data, dtype='<u4', count=n // 4, offset=sh_offset))
    return np.concatenate(parts)


def scan(words, counts, buckets, bits):
    """Winner entry index and scan depth for each distinct word.

    Mirrors DecodeInstruction: first entry of the bucket whose mask/expect the
    word satisfies.  Words matching nothing cost the whole bucket.
    """
    index, count = VARIANTS[bits]
    idx = index(words)
    perm = np.argsort(idx, kind='stable')
    sidx = idx[perm]
    swords = words[perm]
    bounds = np.searchsorted(sidx, np.arange(count + 1))
    winner = np.full(words.size, -1, np.int32)
    depth = np.zeros(words.size, np.int32)
    for b in range(count):
        lo, hi = bounds[b], bounds[b + 1]
        if lo == hi:
            continue
        rest = np.arange(lo, hi)
        for d, e in enumerate(buckets[b], 1):
            if rest.size == 0:
                break
            hit = (swords[rest] & e.mask) == e.expect
            if hit.any():
                where = rest[hit]
                winner[where] = e.raw
                depth[where] = d
                rest = rest[~hit]
        depth[rest] = len(buckets[b])
    out_w = np.empty_like(winner)
    out_d = np.empty_like(depth)
    out_w[perm] = winner
    out_d[perm] = depth
    return out_w, out_d


def summarise(name, counts, winner, depth):
    total = int(counts.sum())
    matched = int(counts[winner >= 0].sum())
    mean = float((counts * depth).sum()) / total
    def frac(k):
        return 100.0 * float(counts[depth <= k].sum()) / total
    return (f'{name:<28} {total/1e6:8.2f}M {100.0*matched/total:6.2f}% '
            f'{mean:6.3f} {frac(1):6.2f}% {frac(2):6.2f}% {frac(4):6.2f}% {int(depth.max()):4d}')


HEAD = f'{"binary":<28} {"words":>9} {"matched":>7} {"E[d]":>6} {"d=1":>7} {"d<=2":>7} {"d<=4":>7} {"max":>4}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inc')
    ap.add_argument('out')
    ap.add_argument('binaries', nargs='+')
    ap.add_argument('--leave-one-out', action='store_true')
    args = ap.parse_args()

    entries, commented = G.parse_inc(args.inc)
    order = G.priority_order(entries)
    print(f'a64.inc: {len(entries)} active entries, {commented} commented out', file=sys.stderr)

    plain = {b: build_buckets(order, b) for b in (12, 14)}

    # Distinct words and their multiplicity, per binary.
    words, counts = {}, {}
    for path in args.binaries:
        t = time.time()
        w = exec_words(path)
        u, c = np.unique(w, return_counts=True)
        words[path] = u
        counts[path] = c.astype(np.int64)
        print(f'{os.path.basename(path):<28} {w.size/1e6:8.2f}M words, '
              f'{u.size/1e6:6.2f}M distinct ({100.0*u.size/w.size:.1f}%) [{time.time()-t:.1f}s]',
              file=sys.stderr)

    # Winners, and so weights.  The winner is the same under every candidate
    # index and order, so the 14-bit priority-order table decides it.
    hits = {}
    rates = {}
    for path in args.binaries:
        winner, _ = scan(words[path], counts[path], plain[14], 14)
        h = np.zeros(len(entries), np.int64)
        np.add.at(h, winner[winner >= 0], counts[path][winner >= 0])
        hits[path] = h
        rates[path] = h / max(1, h.sum())

    weight = {}
    mean_rate = sum(rates.values()) / len(rates)
    for e in entries:
        w = int(round(mean_rate[e.raw] * 1e6))
        if w:
            weight[e.key] = w

    with open(args.out, 'w') as f:
        f.write('# Weights for FEXCore/Scripts/a64_decode_table_generator.py: how often each\n'
                '# a64.inc entry wins a decode, in parts per million of the matched words of a\n'
                '# reference binary, averaged over the binaries below.  They decide only which\n'
                '# of the valid bucket orders is used, never what a word decodes to.\n'
                '#\n'
                '# Regenerate with Scripts/powerarm/a64-decode-weights.py.  Entries missing\n'
                '# here weigh nothing and keep their most-specific-first position.\n'
                '#\n')
        for path in args.binaries:
            f.write(f'#   {os.path.basename(path):<24} {int(counts[path].sum())/1e6:8.2f}M words  {path}\n')
        f.write('#\n# <ppm> <a64.inc name> <bitstring>\n')
        for e in sorted(entries, key=lambda e: (-weight.get(e.key, 0), e.name)):
            if e.key in weight:
                f.write(f'{weight[e.key]} {e.name} {e.bits}\n')
    print(f'{args.out}: {len(weight)} of {len(entries)} entries weighted', file=sys.stderr)

    tables = {
        'current 12-bit': (plain[12], 12),
        '12-bit + frequency': ([G.reorder_by_weight(b, weight) for b in plain[12]], 12),
        '14-bit': (plain[14], 14),
        '14-bit + frequency': ([G.reorder_by_weight(b, weight) for b in plain[14]], 14),
    }
    for label, (buckets, bits) in tables.items():
        slots = sum(len(b) for b in buckets)
        print(f'\n== {label}: {slots} slots, {slots * 12 / 1024:.0f} KiB at 12 B/slot, '
              f'{sum(1 for b in buckets if b)} non-empty buckets, max {max(len(b) for b in buckets)}')
        print(HEAD)
        for path in args.binaries:
            winner, depth = scan(words[path], counts[path], buckets, bits)
            print(summarise(os.path.basename(path), counts[path], winner, depth))

    if args.leave_one_out:
        print('\n== 14-bit + frequency, weights from the other binaries only')
        print(HEAD)
        for held in args.binaries:
            others = [p for p in args.binaries if p != held]
            mr = sum(rates[p] for p in others) / len(others)
            w = {e.key: int(round(mr[e.raw] * 1e6)) for e in entries if round(mr[e.raw] * 1e6)}
            buckets = [G.reorder_by_weight(b, w) for b in plain[14]]
            winner, depth = scan(words[held], counts[held], buckets, 14)
            print(summarise(os.path.basename(held), counts[held], winner, depth))


if __name__ == '__main__':
    main()
