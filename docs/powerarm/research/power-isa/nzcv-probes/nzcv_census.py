#!/usr/bin/env python3
"""NZCV liveness census over a POWERarm post-optimisation IR dump.

Usage: nzcv_census.py IRDUMP CC1.DIS [--label NAME]

For every kept flag producer (SubNZCV, AddNZCV, SubWithFlags, AddWithFlags,
TestNZ, AndWithFlags; CondSubNZCV/CondAddNZCV and FCmp reported separately)
walk forward through the unit's CFG until every NZCV bit it wrote is
overwritten, and record what its flags reach: in-unit readers, and unit exits
by kind (constant None/Call, Return, indirect, Syscall/Break/other).

For every constant exit reached with live flags, classify the target's guest
code from the disassembly (a bounded scan, "the peek"): DEAD when every path
from the target reaches a full NZCV writer before any reader, LIVE when some
path reads first, UNRESOLVED when a path ends in BL/RET/BR/SVC/budget first.
When the target was itself compiled, also compute the target unit's live-in
per DFCE's rules (exits are FLAG_ALL) as a cross-check.
"""
import re
import sys
from collections import Counter, defaultdict

N, Z, C, V = 8, 4, 2, 1
ALL = 15

COND_FLAGS = {
    'AL': 0, 'ALWAYS': 0, 'MI': N, 'PL': N, 'EQ': Z, 'NEQ': Z, 'UGE': C, 'ULT': C,
    'VS': V, 'VC': V, 'FU': V, 'FNU': V, 'UGT': Z | C, 'ULE': Z | C,
    'SGE': N | V, 'SLT': N | V, 'FLU': N | V, 'FGE': N | V,
    'SGT': N | Z | V, 'SLE': N | Z | V, 'FLEU': N | Z | V, 'FGT': N | Z | V,
}

PRODUCERS = ('SubNZCV', 'AddNZCV', 'SubWithFlags', 'AddWithFlags', 'TestNZ', 'AndWithFlags')
COND_PRODUCERS = ('CondSubNZCV', 'CondAddNZCV')

# op -> (read, write); None entries are resolved per op (cond)
FLAG_OPS = {
    'SubNZCV': (0, ALL), 'AddNZCV': (0, ALL), 'TestNZ': (0, ALL), 'FCmp': (0, ALL), 'StoreNZCV': (0, ALL),
    'AndWithFlags': (0, ALL), 'AddWithFlags': (0, ALL), 'SubWithFlags': (0, ALL), 'RdRand': (0, ALL),
    'AdcWithFlags': (C, ALL), 'AdcZeroWithFlags': (C, ALL), 'SbbWithFlags': (C, ALL),
    'AdcNZCV': (C, ALL), 'SbbNZCV': (C, ALL),
    'ShiftFlags': (ALL, ALL), 'RotateFlags': (C | V, C | V), 'AXFlag': (Z | C | V, ALL),
    'CmpPairZ': (0, Z), 'CarryInvert': (C, C), 'SetSmallNZV': (0, N | Z | V),
    'LoadNZCV': (ALL, 0), 'Adc': (C, 0), 'AdcZero': (C, 0), 'Sbb': (C, 0),
}
COND_READERS = ('NZCVSelect', 'NZCVSelectV', 'NZCVSelectIncrement', 'Neg')
FUSABLE_READERS = ('CondJump', 'NZCVSelect')

def cond_of(args):
    for tok in args.replace(',', ' ').split():
        if tok in COND_FLAGS:
            return tok
    return None

def classify_op(op, args):
    """Return (read, write, kind) for an op; kind is a label for readers."""
    if op in FLAG_OPS:
        r, w = FLAG_OPS[op]
        return r, w, op
    if op in COND_READERS:
        c = cond_of(args)
        return (COND_FLAGS.get(c, ALL) if c else ALL), 0, op
    if op == 'CondJump':
        # CondJump Cmp1, Cmp2, True, False, Cond, Size, #FromNZCV, VCmp
        parts = [p.strip() for p in args.split(',')]
        from_nzcv = parts[6] == '#0x1'
        if not from_nzcv:
            return 0, 0, None
        return COND_FLAGS.get(parts[4], ALL), 0, 'CondJump'
    if op in COND_PRODUCERS:
        c = cond_of(args)
        return (COND_FLAGS.get(c, ALL) if c else ALL), ALL, op
    if op == 'RmifNZCV':
        return 0, ALL, op  # mask not parsed; conservative as a writer, ignored as a reader
    return 0, 0, None

OP_RE = re.compile(r'^\t\t(?:\(%(\d+) \w+\)|%(\d+)\(\w+\) \w+ =) (\w+)(?: (.*))?$')
BLOCK_RE = re.compile(r'^\t\(%(\d+)\) CodeBlock ')
HDR_RE = re.compile(r'^\t\(%0\) IRHeader %\d+, #0x([0-9a-f]+), #(\d+), #(\d+)')

class Block:
    __slots__ = ('id', 'ops', 'succ', 'exit', 'livein')
    def __init__(self, id_):
        self.id = id_
        self.ops = []      # (id, op, args)
        self.succ = []     # block ids
        self.exit = None   # ('const', addr, hint) | ('indirect', hint) | ('syscall',) | ('other', op)
        self.livein = ALL

def parse_units(path):
    """Yield (rip, blocks:dict id->Block, order:list)."""
    rip = None
    blocks = {}
    order = []
    consts = {}
    cur = None
    with open(path, 'r', errors='replace') as f:
        for line in f:
            if line.startswith('IR-post'):
                continue
            if line.startswith('@@@@@'):
                if rip is not None:
                    finish(rip, blocks, consts)
                    yield rip, blocks, order
                rip, blocks, order, consts, cur = None, {}, [], {}, None
                continue
            if line.startswith('\t\t'):
                m = OP_RE.match(line.rstrip('\n'))
                if not m:
                    continue
                nid = int(m.group(1) or m.group(2))
                op = m.group(3)
                args = m.group(4) or ''
                if op in ('BeginBlock', 'EndBlock', 'GuestOpcode'):
                    continue
                if op in ('InlineEntrypointOffset', 'InlineConstant', 'EntrypointOffset', 'Constant'):
                    mm = re.match(r'#0x([0-9a-f]+)', args)
                    if mm:
                        v = int(mm.group(1), 16)
                        if v >= 1 << 63:
                            v -= 1 << 64
                        consts[nid] = (op, v)
                    if op in ('InlineEntrypointOffset', 'InlineConstant'):
                        continue
                cur.ops.append((nid, op, args))
            elif line.startswith('\t('):
                m = BLOCK_RE.match(line)
                if m:
                    cur = Block(int(m.group(1)))
                    blocks[cur.id] = cur
                    order.append(cur.id)
                    continue
                m = HDR_RE.match(line)
                if m:
                    rip = int(m.group(1), 16)
    if rip is not None:
        finish(rip, blocks, consts)
        yield rip, blocks, order

def finish(rip, blocks, consts):
    for b in blocks.values():
        if not b.ops:
            b.exit = ('other', 'empty')
            continue
        nid, op, args = b.ops[-1]
        if op == 'CondJump':
            parts = [p.strip() for p in args.split(',')]
            b.succ = [int(parts[2][1:]), int(parts[3][1:])]
        elif op == 'Jump':
            b.succ = [int(args.strip()[1:])]
        elif op == 'ExitFunction':
            parts = [p.strip() for p in args.split(',')]
            tgt, hint = parts[0], parts[1]
            if tgt.startswith('%') and int(tgt[1:]) in consts:
                kind, v = consts[int(tgt[1:])]
                if kind in ('InlineEntrypointOffset', 'EntrypointOffset'):
                    b.exit = ('const', rip + v, hint)
                else:
                    b.exit = ('const', v, hint)
            else:
                b.exit = ('indirect', hint)
        elif op == 'Syscall':
            b.exit = ('syscall',)
        else:
            b.exit = ('other', op)

def unit_livein(blocks, order):
    """DFCE-style backward liveness with exits seeded FLAG_ALL. Returns entry block live-in."""
    for b in blocks.values():
        b.livein = ALL
    changed = True
    it = 0
    while changed and it < 50:
        changed = False
        it += 1
        for bid in reversed(order):
            b = blocks[bid]
            if b.succ:
                live = 0
                for s in b.succ:
                    live |= blocks[s].livein
            else:
                live = ALL
            for nid, op, args in reversed(b.ops):
                r, w, kind = classify_op(op, args)
                if r or w:
                    live = (live & ~w) | r
            if live != b.livein:
                b.livein = live
                changed = True
    return blocks[order[0]].livein

def linear_scan_unit(blocks, order):
    """First flag op along the entry block only: 'LIVE' / 'DEAD' / 'UNRESOLVED'."""
    b = blocks[order[0]]
    for nid, op, args in b.ops:
        r, w, kind = classify_op(op, args)
        if r:
            return 'LIVE'
        if w == ALL:
            return 'DEAD'
        if w:
            return 'UNRESOLVED'
    return 'UNRESOLVED'

def forward_reach(blocks, start_bid, start_idx, mask):
    """From op index start_idx (exclusive) in block start_bid, find readers and exits reached with live bits."""
    readers = Counter()
    exits = []   # (kind tuple, remaining mask)
    seen = set()
    stack = [(start_bid, start_idx + 1, mask)]
    while stack:
        bid, idx, m = stack.pop()
        key = (bid, idx, m)
        if key in seen:
            continue
        seen.add(key)
        b = blocks[bid]
        i = idx
        while i < len(b.ops):
            nid, op, args = b.ops[i]
            r, w, kind = classify_op(op, args)
            if r & m:
                readers[kind] += 1
            m &= ~w
            if m == 0:
                break
            i += 1
        if m == 0:
            continue
        if b.succ:
            for s in b.succ:
                stack.append((s, 0, m))
        else:
            exits.append((b.exit, m))
    return readers, exits

# ---------------------------------------------------------------------------
# Disassembly scan
# ---------------------------------------------------------------------------
COND_BRANCH = {'b.eq', 'b.ne', 'b.cs', 'b.hs', 'b.cc', 'b.lo', 'b.mi', 'b.pl', 'b.vs', 'b.vc', 'b.hi', 'b.ls', 'b.ge', 'b.lt', 'b.gt', 'b.le',
               'bc.eq', 'bc.ne', 'bc.cs', 'bc.hs', 'bc.cc', 'bc.lo', 'bc.mi', 'bc.pl', 'bc.vs', 'bc.vc', 'bc.hi', 'bc.ls', 'bc.ge', 'bc.lt', 'bc.gt', 'bc.le'}
READERS = {'csel', 'csinc', 'csinv', 'csneg', 'cset', 'csetm', 'cinc', 'cinv', 'cneg', 'adc', 'adcs', 'sbc', 'sbcs', 'ngc', 'ngcs',
           'fcsel', 'ccmp', 'ccmn', 'fccmp', 'fccmpe'}
WRITERS = {'cmp', 'cmn', 'subs', 'adds', 'ands', 'bics', 'tst', 'negs', 'fcmp', 'fcmpe'}
PARTIAL = {'setf8', 'setf16', 'rmif', 'cfinv', 'axflag', 'xaflag'}
TERM_UNKNOWN = {'bl', 'ret', 'br', 'blr', 'svc', 'brk', 'hlt', 'eret', 'udf', 'retaa', 'retab', 'braa', 'brab', 'blraa', 'blrab', 'braaz', 'brabz', 'blraaz', 'blrabz'}
TWO_WAY = {'cbz', 'cbnz', 'tbz', 'tbnz'}

class Dis:
    def __init__(self, path):
        self.cls = {}    # addr -> ('W'|'R'|'N'|'X'|'B'|'CB'|'TW'|'T'|'BL', target)
        self.mn = {}
        self.span = [1 << 62, 0]
        self.symbols = {}
        line_re = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)(?:\s+(.*))?$')
        with open(path, 'r', errors='replace') as f:
            for line in f:
                m = line_re.match(line)
                if not m:
                    continue
                addr = int(m.group(1), 16)
                mn = m.group(2)
                ops = m.group(3) or ''
                tgt = None
                if mn in COND_BRANCH or mn == 'b' or mn in TWO_WAY or mn == 'bl':
                    mt = re.search(r'0x([0-9a-f]+)', ops)
                    if mt:
                        tgt = int(mt.group(1), 16)
                if mn in COND_BRANCH:
                    c = ('CB', tgt)
                elif mn == 'b':
                    c = ('B', tgt)
                elif mn in TWO_WAY:
                    c = ('TW', tgt)
                elif mn == 'bl':
                    c = ('BL', tgt)
                elif mn in READERS:
                    c = ('R', None)
                elif mn in WRITERS:
                    c = ('W', None)
                elif mn == 'mrs' and 'nzcv' in ops:
                    c = ('R', None)
                elif mn == 'msr' and 'nzcv' in ops:
                    c = ('W', None)
                elif mn in PARTIAL:
                    c = ('X', None)
                elif mn in TERM_UNKNOWN:
                    c = ('T', None)
                else:
                    c = ('N', None)
                self.cls[addr] = c
                if c[0] == 'T':
                    self.mn[addr] = mn


    def scan(self, target, max_insns=48, max_nodes=96, follow_bl=True, abi_ret=False, depth_bl=0):
        """Bounded scan from target. Returns (verdict, insns_to_decide, reason).
        verdict: DEAD / LIVE / RET (a path reaches RET with flags live; only when abi_ret is False and
        depth_bl > 0, i.e. inside a followed call) / UNRESOLVED."""
        seen = set()
        stack = [(target, 0)]
        nodes = 0
        worst = 'DEAD'
        reason = ''
        max_depth = 0
        while stack:
            pc, depth = stack.pop()
            path = 0
            while True:
                if pc in seen:
                    break
                seen.add(pc)
                nodes += 1
                if pc < self.span[0]: self.span[0] = pc
                if pc > self.span[1]: self.span[1] = pc
                if nodes > max_nodes or path > max_insns:
                    return 'UNRESOLVED', max(max_depth, depth), 'budget'
                c = self.cls.get(pc)
                if c is None:
                    return 'UNRESOLVED', max(max_depth, depth), 'no-code'
                kind, tgt = c
                depth += 1
                path += 1
                max_depth = max(max_depth, depth)
                if kind == 'R' or kind == 'CB':
                    return 'LIVE', depth, 'reader'
                if kind == 'W':
                    break
                if kind == 'X':
                    worst, reason = 'UNRESOLVED', 'partial-writer'
                    break
                if kind == 'T':
                    mn = self.mn.get(pc, '?')
                    if mn == 'svc':
                        pc += 4
                        continue
                    if mn.startswith('ret'):
                        if depth_bl > 0:
                            if worst == 'DEAD':
                                worst, reason = 'RET', 'ret'
                            break
                        if abi_ret:
                            break  # ABI assumption: the caller's continuation does not read NZCV
                        worst, reason = 'UNRESOLVED', 'ret'
                        break
                    worst, reason = 'UNRESOLVED', mn
                    break
                if kind == 'BL':
                    if not follow_bl or tgt is None or depth_bl >= 2:
                        worst, reason = 'UNRESOLVED', 'bl'
                        break
                    v, d, r = self.scan(tgt, max_insns, max_nodes, follow_bl, abi_ret, depth_bl + 1)
                    if v == 'DEAD':
                        break
                    if v == 'LIVE':
                        return 'LIVE', depth + d, 'callee-reader'
                    if v == 'RET':
                        pc += 4
                        continue
                    worst, reason = 'UNRESOLVED', 'bl:' + r
                    break
                if kind == 'B':
                    if tgt is None:
                        worst, reason = 'UNRESOLVED', 'b-notarget'
                        break
                    pc = tgt
                    continue
                if kind == 'TW':
                    if tgt is None:
                        worst, reason = 'UNRESOLVED', 'tw-notarget'
                        break
                    stack.append((tgt, depth))
                    pc += 4
                    continue
                pc += 4
        return worst, max_depth, reason

def main():
    args = sys.argv[1:]
    label = 'census'
    if '--label' in args:
        i = args.index('--label')
        label = args[i + 1]
        del args[i:i + 2]
    irpath, dispath = args[0], args[1]
    print(f'# {label}: {irpath}')
    dis = Dis(dispath)
    print(f'disassembly: {len(dis.cls)} instructions')

    units = {}
    per_unit_entry = {}
    nunits = 0
    producers = Counter()
    kept_total = 0
    # classification counters
    cls = Counter()
    reader_kinds = Counter()
    exit_kinds = Counter()
    const_targets = Counter()          # addr -> count of (producer, exit) reaching it
    const_target_hint = {}
    entry_livein_units = 0
    entry_livein_by_scan = Counter()
    inunit_reason = Counter()
    # per producer detail for the "droppable under policy" estimate
    policy = Counter()
    cond_producers = Counter()
    fcmp_reach = Counter()
    hint_by_exit = Counter()

    all_units = list(parse_units(irpath))
    for rip, blocks, order in all_units:
        nunits += 1
        li = unit_livein(blocks, order)
        lin = linear_scan_unit(blocks, order)
        rd, ex = forward_reach(blocks, order[0], -1, ALL)
        entry_livein_by_scan['sound: reader reachable from entry'] += 1 if rd else 0
        entry_livein_by_scan['sound: reader in a later block only'] += 1 if (rd and lin != 'LIVE') else 0
        per_unit_entry[rip] = (li, lin, bool(rd))
        entry_livein_by_scan[lin] += 1
        if li & ALL:
            entry_livein_units += 1

    for rip, blocks, order in all_units:
        for bid in order:
            b = blocks[bid]
            for idx, (nid, op, args) in enumerate(b.ops):
                is_prod = op in PRODUCERS
                is_cond = op in COND_PRODUCERS
                if not (is_prod or is_cond or op == 'FCmp'):
                    continue
                readers, exits = forward_reach(blocks, bid, idx, ALL)
                if op == 'FCmp':
                    fcmp_reach['exit' if exits else 'inunit'] += 1
                    continue
                if is_cond:
                    cond_producers[op] += 1
                producers[op] += 1
                kept_total += 1
                cls['(region) ' + ('cc1' if rip < 0x3000000 else 'other (libc/ld.so)')] += 1
                for k, v in readers.items():
                    reader_kinds[k] += 1
                ek = set()
                targets = []
                for ex, m in exits:
                    if ex[0] == 'const':
                        ek.add('const-' + ex[2])
                        targets.append((ex[1], ex[2]))
                        hint_by_exit['const-' + ex[2]] += 1
                    elif ex[0] == 'indirect':
                        ek.add('indirect-' + ex[1])
                    elif ex[0] == 'syscall':
                        ek.add('syscall')
                    else:
                        ek.add('other:' + ex[1])
                for k in ek:
                    exit_kinds[k] += 1
                unfusable = [k for k in readers if k not in FUSABLE_READERS]
                if not exits:
                    if not readers:
                        cls['in-unit: no visible reader (fused readers; kept by the signal-barrier rule or a partial overwrite)'] += 1
                    elif unfusable:
                        cls['in-unit: unfusable reader (' + ','.join(sorted(set(unfusable))) + ')'] += 1
                    else:
                        cls['in-unit: only fusable readers visible'] += 1
                    continue
                # exits reached
                if ek <= {'const-None', 'const-Call'}:
                    cls['exit: constant only'] += 1
                    for mode, kw in (('simple', dict(follow_bl=False, abi_ret=False)), ('follow-bl', dict(follow_bl=True, abi_ret=False)), ('follow-bl+abi-ret', dict(follow_bl=True, abi_ret=True))):
                        verdicts = []
                        dis.span = [1 << 62, 0]
                        for t, h in targets:
                            v, d, r = dis.scan(t, **kw)
                            verdicts.append((v, d, r, h))
                            if mode == 'simple':
                                const_targets[(t, v)] += 1
                        if all(v == 'DEAD' for v, d, r, h in verdicts):
                            if unfusable:
                                policy[mode + ': all targets DEAD, but unfusable in-unit reader'] += 1
                            else:
                                policy[mode + ': all targets DEAD, readers fusable -> droppable'] += 1
                                dmax = max(d for v, d, r, h in verdicts)
                                policy[mode + ':   depth<=8'] += dmax <= 8
                                policy[mode + ':   depth<=16'] += dmax <= 16
                                policy[mode + ':   depth<=32'] += dmax <= 32
                                dist = max(abs(dis.span[0] - rip), abs(dis.span[1] - rip))
                                for lim, name in ((4096, '4K'), (65536, '64K'), (1 << 20, '1M'), (1 << 24, '16M')):
                                    if dist <= lim:
                                        policy[mode + ':   span within ' + name] += 1
                                        break
                                else:
                                    policy[mode + ':   span beyond 16M'] += 1
                        elif any(v == 'LIVE' for v, d, r, h in verdicts):
                            policy[mode + ': some target LIVE -> keep'] += 1
                        else:
                            policy[mode + ': UNRESOLVED -> keep'] += 1
                            rs = sorted(set(r for v, d, r, h in verdicts if v != 'DEAD'))
                            policy[mode + ':   unresolved because ' + '+'.join(rs)] += 1
                    kinds = tuple(sorted(ek))
                    policy['exit hint set ' + '+'.join(kinds)] += 1
                elif 'const-None' in ek or 'const-Call' in ek:
                    cls['exit: constant and ' + '+'.join(sorted(k for k in ek if not k.startswith('const')))] += 1
                else:
                    cls['exit: ' + '+'.join(sorted(ek))] += 1

    print(f'units: {nunits}; units with NZCV live-in at entry (DFCE rule, exits=ALL): {entry_livein_units}')
    print(f'units by first flag op in the entry block (EntryNZCVLiveIn as computed today = LIVE): {dict(entry_livein_by_scan)}')
    print(f'kept producers: {kept_total}  by op: {dict(producers)}')
    print(f'FCmp: {dict(fcmp_reach)}')
    print('\n## classification of kept producers (why the flags are live)')
    for k, v in cls.most_common():
        print(f'{v:8d}  {100.0*v/kept_total:5.1f}%  {k}')
    print('\n## visible in-unit readers (per producer that has one)')
    for k, v in reader_kinds.most_common():
        print(f'{v:8d}  {k}')
    print('\n## exit kinds reached with live flags (per producer)')
    for k, v in exit_kinds.most_common():
        print(f'{v:8d}  {k}')
    print('\n## policy estimate: constant-exit-only producers, by the peek verdict on their targets')
    for k, v in sorted(policy.items()):
        print(f'{v:8d}  {k}')
    # verdict distribution over distinct targets
    vd = Counter()
    for (t, v), n in const_targets.items():
        vd[v] += 1
    print('\n## distinct constant-exit targets reached with live flags, by peek verdict')
    for k, v in vd.most_common():
        print(f'{v:8d}  {k}')
    # cross-check peek against compiled target units
    agree = Counter()
    for (t, v), n in const_targets.items():
        if t in per_unit_entry:
            li, lin, rd = per_unit_entry[t]
            unit_v = 'LIVE' if li else 'DEAD'
            agree[(v, 'unit-' + unit_v, 'linear-' + lin)] += 1
            agree[('CONTRADICTION: peek DEAD but the compiled target reads NZCV from entry' if (v == 'DEAD' and rd) else 'consistent', )] += 1
            if v == 'DEAD' and rd:
                print(f'  contradiction at target 0x{t:x}')
        else:
            agree[(v, 'target-not-compiled')] += 1
    print('\n## cross-check: peek verdict vs the compiled target unit (DFCE live-in with exits=ALL; linear = first flag op in the entry block)')
    for k, v in agree.most_common():
        print(f'{v:8d}  {k}')

if __name__ == '__main__':
    main()
