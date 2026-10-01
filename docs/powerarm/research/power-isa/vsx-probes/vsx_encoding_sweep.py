#!/usr/bin/env python3
"""The VSX-form encoding sweep (VSX-REGISTER-CLASSES.md 7.5, Stage 0 gate).

Every VSX-form emitter method now takes VSXR and derives its TX/AX/BX/CX
extension bits from bit 5 of the register number instead of hardcoding them.
A wrong derivation is a WRONG REGISTER, not a SIGILL: the instruction still
decodes and still runs, it just reads or writes the other half of the file.
The emitted-code identity gate cannot see it, because nothing in the suite
emits a low-bank operand yet. So each method is assembled over registers from
both halves and compared, word for word, against llvm-mc.

  python3 docs/powerarm/research/power-isa/vsx-probes/vsx_encoding_sweep.py \
      [--cxx g++] [--mc llvm-mc] [--mcpu pwr9]

Writes nothing to the tree; prints one line per mnemonic plus a summary, and
exits non-zero on any mismatch.
"""
import argparse, collections, os, re, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', '..'))
EMITTER = os.path.join(ROOT, 'CodeEmitter', 'PPC64LE', 'Emitter.h')

# Registers the sweep draws from. The cross product covers every combination
# of extension bits; the per-operand walk covers every register number in both
# halves with the other operands pinned low and high.
CROSS = [0, 1, 31, 32, 33, 63]
ALL64 = list(range(64))
PIN = [0, 63]
GPRS = [0, 3, 4, 31]
DQS = [0, 16, -32768, 32752]

# How to spell each method as assembly, and which of its arguments are VSRs.
#   shape: a format string over named holes
#   vsx:   the holes that take a VSR
SHAPES = {
    'xx3':    ('{m} {t}, {a}, {b}',            ('t', 'a', 'b')),
    'xx3rec': ('{m}. {t}, {a}, {b}',           ('t', 'a', 'b')),
    'xx2':    ('{m} {t}, {b}',                 ('t', 'b')),
    'xx3imm': ('{m} {t}, {a}, {b}, {i}',       ('t', 'a', 'b')),
    'xx4':    ('{m} {t}, {a}, {b}, {c}',       ('t', 'a', 'b', 'c')),
    'xx2imm': ('{m} {t}, {b}, {i}',            ('t', 'b')),
    'bf':     ('{m} {f}, {a}, {b}',            ('a', 'b')),
    'xto':    ('{m} {t}, {ra}',                ('t',)),
    'xtodd':  ('{m} {t}, {ra}, {rb}',          ('t',)),
    'xfrom':  ('{m} {ra}, {b}',                ('b',)),
    'xmem':   ('{m} {t}, {ra}, {rb}',          ('t',)),
    'dq':     ('{m} {t}, {d}({ra})',           ('t',)),
}

# The method inventory, by shape. Names are the emitter's; the assembler
# mnemonic is the same except for the record forms, whose emitter name carries
# a trailing underscore where the mnemonic carries a dot.
def inventory():
    src = open(EMITTER).read()
    inv = collections.defaultdict(list)

    def names(pattern):
        return sorted(set(re.findall(pattern, src)))

    inv['xx3']    = names(r'void (\w+)\s*\(VSXR \w+, VSXR \w+, VSXR \w+\) \{ EmitXX3VSX\(')
    inv['xx2']    = names(r'void (\w+)\s*\(VSXR \w+, VSXR \w+\)\s*\{ EmitXX2VSX\(')
    inv['xx2']   += names(r'void (\w+)\(VSXR \w+, VSXR \w+\) \{ EmitXX2VSXWithRA\(')
    inv['bf']     = names(r'void (\w+)\(uint32_t bf, VSXR \w+, VSXR \w+\)')
    inv['xto']    = names(r'void (mtvsr(?:d|wa|wz|ws))\(VSXR \w+, GPR \w+\)')
    inv['xtodd']  = names(r'void (mtvsrdd)\(VSXR \w+, GPR \w+, GPR \w+\)')
    inv['xfrom']  = names(r'void (mfvsr\w*)\(GPR \w+, VSXR \w+\)')
    inv['xmem']   = names(r'void (\w+)\s*\(VSXR \w+, GPR ra, GPR rb\)')
    inv['dq']     = names(r'void (\w+)\s*\(VSXR \w+, int16_t dq, GPR ra\)')
    inv['xx4']    = ['xxsel']
    inv['xx2imm'] = ['xxspltw']
    inv['xx3imm'] = ['xxpermdi', 'xxsldwi']
    # Rc=1 twins: emitter name ends in '_', mnemonic ends in '.'
    inv['xx3rec'] = [n[:-1] for n in names(r'void (\w+_)\s*\(VSXR \w+, VSXR \w+, VSXR \w+\)')]
    # xxbr* carry their width in the RA field, so they are raw Emit32 in the
    # emitter rather than an EmitXX2VSX call; pick them up by name.
    inv['xx2'] += [n for n in names(r'void (xxbr[hwdq])\(VSXR') if n not in inv['xx2']]
    inv['xx2'] += [n for n in names(r'void (xvcvs?phps?p?|xvcvhpsp)\(VSXR \w+, VSXR \w+\) \{') if n not in inv['xx2']]
    # Drop the shapes handled explicitly above out of the generic buckets.
    for n in ('xxspltw',):
        if n in inv['xx2']:
            inv['xx2'].remove(n)
    for n in ('xxsel',):
        if n in inv['xx3']:
            inv['xx3'].remove(n)
    # The Rc=1 twins are their own shape; keep their underscore-suffixed
    # emitter names out of the plain buckets.
    for grp in ('xx3', 'xx2'):
        inv[grp] = [n for n in inv[grp] if not n.endswith('_')]
    for grp in inv.values():
        grp[:] = sorted(set(grp))
    return inv


def cases(shape, name):
    """Yield dicts of hole -> value for one mnemonic."""
    fmt, vsx = SHAPES[shape]
    holes = re.findall(r'\{(\w+)\}', fmt)
    imms = {'i': 0, 'f': 0, 'ra': GPRS[1], 'rb': GPRS[2], 'd': 0}
    out = []

    def emit(vals):
        c = dict(imms)
        c.update(vals)
        out.append(c)

    # Full cross product over the boundary registers.
    def cross(i, acc):
        if i == len(vsx):
            emit(dict(acc))
            return
        for r in CROSS:
            acc[vsx[i]] = r
            cross(i + 1, acc)
    cross(0, {})

    # Every register number in each operand position, others pinned.
    for pos in vsx:
        for pin in PIN:
            for r in ALL64:
                vals = {h: pin for h in vsx}
                vals[pos] = r
                emit(vals)

    # The non-register fields each get their own walk.
    if 'i' in holes:
        lim = 4
        for v in range(lim):
            for r in CROSS:
                emit({h: r for h in vsx} | {'i': v})
    if 'f' in holes:
        for v in range(8):
            emit({h: CROSS[0] for h in vsx} | {'f': v})
    if 'd' in holes:
        for v in DQS:
            for r in CROSS:
                emit({h: r for h in vsx} | {'d': v})
    if 'ra' in holes or 'rb' in holes:
        for g in GPRS:
            emit({h: CROSS[0] for h in vsx} | {'ra': g, 'rb': g})
    return out


CXX_TEMPLATE = r'''// Generated by vsx_encoding_sweep.py. Prints "<asm>\t<word>" per case.
#include <PPC64LE/Emitter.h>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace PPC64Emitter;

static uint8_t Buf[1 << 20];

int main() {
  Emitter E(Buf, sizeof(Buf));
  auto W = [&](const char* Text) {
    const uint32_t* Words = reinterpret_cast<const uint32_t*>(Buf);
    printf("%s\t%08x\n", Text, Words[0]);
    E.SetBuffer(Buf, sizeof(Buf));
  };
  (void)W;
@BODY@
  return 0;
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cxx', default='g++')
    ap.add_argument('--mc', default='llvm-mc')
    ap.add_argument('--mcpu', default='pwr9')
    args = ap.parse_args()

    inv = inventory()
    body = []
    expected = []
    for shape in sorted(inv):
        fmt, vsx = SHAPES[shape]
        for name in inv[shape]:
            method = name + '_' if shape == 'xx3rec' else name
            for c in cases(shape, name):
                text = fmt.format(m=name, **c)
                cargs = []
                if shape in ('xx3', 'xx3rec'):
                    cargs = ['vsx(%d)' % c['t'], 'vsx(%d)' % c['a'], 'vsx(%d)' % c['b']]
                elif shape == 'xx2':
                    cargs = ['vsx(%d)' % c['t'], 'vsx(%d)' % c['b']]
                elif shape == 'xx3imm':
                    cargs = ['vsx(%d)' % c['t'], 'vsx(%d)' % c['a'], 'vsx(%d)' % c['b'], str(c['i'])]
                elif shape == 'xx4':
                    cargs = ['vsx(%d)' % c[h] for h in ('t', 'a', 'b', 'c')]
                elif shape == 'xx2imm':
                    cargs = ['vsx(%d)' % c['t'], 'vsx(%d)' % c['b'], str(c['i'])]
                elif shape == 'bf':
                    cargs = [str(c['f']), 'vsx(%d)' % c['a'], 'vsx(%d)' % c['b']]
                elif shape == 'xto':
                    cargs = ['vsx(%d)' % c['t'], 'r(%d)' % c['ra']]
                elif shape == 'xtodd':
                    cargs = ['vsx(%d)' % c['t'], 'r(%d)' % c['ra'], 'r(%d)' % c['rb']]
                elif shape == 'xfrom':
                    cargs = ['r(%d)' % c['ra'], 'vsx(%d)' % c['b']]
                elif shape == 'xmem':
                    cargs = ['vsx(%d)' % c['t'], 'r(%d)' % c['ra'], 'r(%d)' % c['rb']]
                elif shape == 'dq':
                    cargs = ['vsx(%d)' % c['t'], str(c['d']), 'r(%d)' % c['ra']]
                body.append('  E.%s(%s); W("%s");' % (method, ', '.join(cargs), text))
                expected.append(text)

    with tempfile.TemporaryDirectory() as td:
        srcp = os.path.join(td, 'sweep.cpp')
        open(srcp, 'w').write(CXX_TEMPLATE.replace('@BODY@', '\n'.join(body)))
        binp = os.path.join(td, 'sweep')
        cmd = [args.cxx, '-std=c++20', '-O0', '-I', os.path.join(ROOT, 'CodeEmitter'),
               '-I', os.path.join(ROOT, 'FEXCore', 'include'), srcp, '-o', binp]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[-6000:]); return 2
        ours = subprocess.run([binp], capture_output=True, text=True, check=True).stdout.splitlines()

        asm = '\n'.join(l.split('\t')[0] for l in ours) + '\n'
        mc = subprocess.run([args.mc, '-triple=powerpc64le', '-mcpu=' + args.mcpu, '--show-encoding'],
                            input=asm, capture_output=True, text=True)
        if mc.returncode:
            print(mc.stderr[-6000:]); return 2
        theirs = []
        for line in mc.stdout.splitlines():
            m = re.search(r'encoding: \[([^\]]+)\]', line)
            if m:
                b = [int(x, 16) for x in m.group(1).replace('0x', '').split(',')]
                theirs.append('%08x' % (b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24))

    if len(theirs) != len(ours):
        print('FAIL: %d emitted, %d assembled' % (len(ours), len(theirs))); return 1

    bad = collections.Counter()
    total = collections.Counter()
    for line, want in zip(ours, theirs):
        text, got = line.split('\t')
        mn = text.split()[0]
        total[mn] += 1
        if got != want:
            if bad[mn] == 0:
                print('MISMATCH %-12s %-34s ours %s llvm %s' % (mn, text, got, want))
            bad[mn] += 1

    for mn in sorted(total):
        print('%-5s %-12s %d cases' % ('FAIL' if bad[mn] else 'ok', mn, total[mn]))
    print('%d mnemonics, %d cases, %d mismatched' % (len(total), sum(total.values()), sum(bad.values())))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
