#!/usr/bin/env python3
# check_linked_callee.py OBJDUMP OBJECT
#
# Disassembles the linked_callee_probe object and checks every
# CallHostFunction instantiation: the first instruction that touches X17 must
# read it, and no call, branch or write to X16/X17 may come before it. Exits
# nonzero, naming the offender, otherwise. See common/Guest.h.
import re
import subprocess
import sys

WRITES = {'mov', 'movz', 'movk', 'movn', 'add', 'sub', 'adr', 'adrp', 'ldr', 'ldur', 'ldp', 'orr', 'and', 'eor', 'lsl', 'lsr', 'asr',
          'madd', 'mul', 'csel', 'ubfx', 'sbfx', 'ubfiz', 'bfi', 'ldrb', 'ldrh', 'ldrsw'}


def check(lines):
    results = []
    func = None
    verdict = None
    for line in lines:
        m = re.match(r'^[0-9a-f]+ <(.*)>:', line)
        if m:
            if func is not None and verdict is None:
                results.append((func, 'no X17 read at all'))
            func = m.group(1) if 'CallHostFunction' in m.group(1) else None
            verdict = None
            continue
        if func is None or verdict is not None:
            continue
        parts = line.rstrip('\n').split('\t')
        if len(parts) < 2 or not parts[0].strip().endswith(':'):
            continue
        mnem = parts[1].strip()
        args = parts[2].split('//')[0].strip() if len(parts) > 2 else ''
        operands = [a.strip() for a in args.split(',')]
        dest = operands[0] if operands else ''
        if mnem in ('bl', 'blr', 'b', 'br', 'ret') or mnem.startswith('b.'):
            verdict = 'BAD: "%s %s" before X17 is read' % (mnem, args)
        elif mnem in WRITES and re.fullmatch(r'[xw]1[67]', dest):
            verdict = 'BAD: "%s %s" writes IP0/IP1 before X17 is read' % (mnem, args)
        elif re.search(r'\b[xw]17\b', args):
            verdict = 'ok'
        if verdict is not None:
            results.append((func, verdict))
    if func is not None and verdict is None:
        results.append((func, 'no X17 read at all'))
    return results


def main():
    if len(sys.argv) != 3:
        print('usage: check_linked_callee.py OBJDUMP OBJECT')
        return 2
    disassembly = subprocess.run([sys.argv[1], '-d', '--no-show-raw-insn', sys.argv[2]], check=True, capture_output=True,
                                 text=True).stdout
    results = check(disassembly.splitlines())
    if len(results) < 4:
        print('linked-callee check: expected 4 CallHostFunction instantiations, found %d' % len(results))
        return 1
    bad = [(f, v) for f, v in results if v != 'ok']
    for f, v in bad:
        print('linked-callee check: %s: %s' % (f, v))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
