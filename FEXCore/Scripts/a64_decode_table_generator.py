#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the A64 decode table's constant bucket arrays from a64.inc.

    a64_decode_table_generator.py <a64.inc> <weights> <out.inc>

DecodeTable.cpp used to do this work in every process: parse 774 bitstrings,
sort them by mask popcount, hoist the SIMD modified-immediate entries, and
then fill ~12K bucket slots into ~250 KiB of private heap.  None of it depends
on anything but a64.inc, so it happens here instead and the result lands in
.rodata, shared by every guest process on the box.

The order inside a bucket is the one thing this script adds.  Entries are laid
out most-specific-first, as before, and then the frequently decoded ones are
moved forward -- but an entry only ever moves ahead of another when no 32-bit
word can match both of them, so any two entries that could both match a word
keep their relative order.  The scan therefore returns the same entry for
every word it did before; the weights only decide which of the orders that
satisfy that constraint is used.  See <weights> for where they come from.

This module is also the shared model of the table: Scripts/powerarm/
a64-decode-weights.py imports it to produce the weights file.
"""
import re
import sys

# Descriptions dynarmic's table hoists ahead of everything else: without this
# MOVI/MVNI/BIC (vector) words decode as the shift-by-immediate entries whose
# immh=0000 space they occupy.  Kept in sync with DecodeTable.cpp's ComesFirst.
COMES_FIRST = (
    'MOVI, MVNI, ORR, BIC (vector, immediate)',
    'FMOV (vector, immediate)',
    'Unallocated SIMD modified immediate',
)

# INST(fn, "description", "bitstring"), commented-out entries included so the
# caller can report them.
INST_RE = re.compile(r'^(//)?\s*INST\(\s*([A-Za-z0-9_]+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"([^"]*)"\s*\)')

INDEX_BITS = 14
INDEX_COUNT = 1 << INDEX_BITS


def fast_lookup_index(word):
    """DecodeTable.cpp's FastLookupIndex: word bits [31:22] and [13:10]."""
    return ((word >> 10) & 0x00F) | ((word >> 18) & 0x3FF0)


class Entry:
    """One active INST entry, with the mask/expect DecodeTable.cpp derives."""

    __slots__ = ('raw', 'name', 'desc', 'bits', 'mask', 'expect')

    def __init__(self, raw, name, desc, bits):
        if len(bits) != 32:
            raise SystemExit(f'a64.inc entry {name} is not 32 bits: {bits!r}')
        self.raw = raw
        self.name = name
        self.desc = desc
        self.bits = bits
        mask = expect = 0
        for i, c in enumerate(bits):
            bit = 1 << (31 - i)
            if c == '0':
                mask |= bit
            elif c == '1':
                mask |= bit
                expect |= bit
        self.mask = mask
        self.expect = expect

    @property
    def key(self):
        """Stable identity across a64.inc edits.  The name alone is not one:
        nine entries are called UnallocatedEncoding."""
        return (self.name, self.bits)


def parse_inc(path):
    """(active entries in a64.inc order, number of commented-out entries)."""
    active = []
    commented = 0
    with open(path) as f:
        for line in f:
            m = INST_RE.match(line)
            if not m:
                continue
            if m.group(1):
                commented += 1
                continue
            active.append(Entry(len(active), m.group(2), m.group(3), m.group(4)))
    return active, commented


def priority_order(entries):
    """DecodeTable.cpp's matcher order before this script existed: a stable
    sort by mask popcount, descending, then a stable partition hoisting
    COMES_FIRST.  More fixed bits is more specific, so it wins."""
    by_specificity = sorted(entries, key=lambda e: -bin(e.mask).count('1'))
    first = [e for e in by_specificity if e.desc in COMES_FIRST]
    rest = [e for e in by_specificity if e.desc not in COMES_FIRST]
    return first + rest


def reachable_buckets(entry):
    """Every index an entry can be reached from: the index bits it fixes have
    to agree, the rest are free."""
    fixed = fast_lookup_index(entry.mask)
    want = fast_lookup_index(entry.expect)
    free = ~fixed & (INDEX_COUNT - 1)
    sub = free
    out = []
    while True:
        out.append(want | sub)
        if sub == 0:
            return out
        sub = (sub - 1) & free


def build_buckets(order):
    """Bucket i's entries, in scan order."""
    buckets = [[] for _ in range(INDEX_COUNT)]
    for e in order:
        for i in reachable_buckets(e):
            buckets[i].append(e)
    return buckets


def disjoint(a, b):
    """True when no 32-bit word can match both entries."""
    return ((a.expect ^ b.expect) & a.mask & b.mask) != 0


def reorder_by_weight(bucket, weight):
    """Move the heavier entries forward by swapping adjacent pairs that cannot
    both match a word.  Every pair that could both match keeps its relative
    order, so the first entry matching any given word does not change."""
    b = list(bucket)
    swapped = True
    while swapped:
        swapped = False
        for i in range(len(b) - 1):
            x, y = b[i], b[i + 1]
            if weight.get(y.key, 0) > weight.get(x.key, 0) and disjoint(x, y):
                b[i], b[i + 1] = y, x
                swapped = True
    return b


def read_weights(path):
    """<weight> <name> <bitstring> per line; '#' comments."""
    weight = {}
    header = []
    with open(path) as f:
        for line in f:
            if line.startswith('#'):
                header.append(line.rstrip())
                continue
            parts = line.split()
            if not parts:
                continue
            if len(parts) != 3:
                raise SystemExit(f'{path}: cannot parse {line!r}')
            weight[(parts[1], parts[2])] = int(parts[0])
    return weight, header


def format_array(values, per_line=24):
    out = []
    for i in range(0, len(values), per_line):
        out.append('  ' + ' '.join(f'{v},' for v in values[i:i + per_line]))
    return '\n'.join(out)


def main(argv):
    if len(argv) != 4:
        raise SystemExit(__doc__)
    inc_path, weights_path, out_path = argv[1:]

    entries, commented = parse_inc(inc_path)
    weight, weights_header = read_weights(weights_path)
    buckets = [reorder_by_weight(b, weight) for b in build_buckets(priority_order(entries))]

    starts = [0]
    slots = []
    for b in buckets:
        slots.extend(e.raw for e in b)
        starts.append(len(slots))
    if len(slots) > 0xFFFF:
        raise SystemExit(f'{len(slots)} bucket slots does not fit a uint16_t')

    unweighted = sum(1 for e in entries if e.key not in weight)
    with open(out_path, 'w') as f:
        f.write(f'''// SPDX-License-Identifier: MIT
//
// Generated by FEXCore/Scripts/a64_decode_table_generator.py from
// a64.inc ({len(entries)} active INST entries, {commented} commented out) and
// a64-decode-weights.txt ({len(entries) - unweighted} of {len(entries)} entries weighted).
// Do not edit; the build regenerates it.
//
// Buckets are indexed by FastLookupIndex (word bits [31:22] and [13:10],
// {INDEX_BITS} bits, {INDEX_COUNT} buckets) and hold a64.inc entry indices in scan
// order: most specific first, then frequency-ordered among entries that
// cannot both match the same word.

#define A64_DECODE_ENTRY_COUNT {len(entries)}
#define A64_DECODE_INDEX_BITS {INDEX_BITS}
#define A64_DECODE_INDEX_COUNT {INDEX_COUNT}
#define A64_DECODE_SLOT_COUNT {len(slots)}

// Bucket i is A64DecodeSlot[A64DecodeBucketStart[i] .. [i + 1]).
constexpr uint16_t A64DecodeBucketStart[A64_DECODE_INDEX_COUNT + 1] = {{
{format_array(starts)}
}};

constexpr uint16_t A64DecodeSlot[A64_DECODE_SLOT_COUNT] = {{
{format_array(slots)}
}};
''')

    nonempty = sum(1 for b in buckets if b)
    print(f'{out_path}: {len(entries)} entries, {len(slots)} slots in '
          f'{nonempty}/{INDEX_COUNT} buckets, max depth {max(len(b) for b in buckets)}, '
          f'{unweighted} entries unweighted', file=sys.stderr)


if __name__ == '__main__':
    main(sys.argv)
