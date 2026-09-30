#!/usr/bin/env python3
"""Join the NZCV exit-site census's static rows with its executed traversals.

Usage: nzcv_exit_weight.py DUMP [DUMP ...]

DUMPs are /tmp/powerarm-nzcv-exits-<pid>.txt files written by a run with
POWERARM_NZCVEXITCENSUS=1 (see FEXCore/Source/Interface/Core/JIT/PPC64LE/
NZCVExitCensus.h). Several are summed, which is what a multi-process guest
needs: Firefox writes one per content process and the session is their sum.

The class names are nzcv_census.py's, so a row here lines up with a row of
census/cc1-nzcv.census. Every percentage is printed against two denominators,
because they answer different questions:

  * of all kept producers / of all executed exit traversals -- the share of the
    whole workload;
  * of constant-exit-only producers / of their traversals -- the share of the
    population NZCV-LIVENESS.md SS5.2 scans, which is the row to compare with
    that section's 36.7% and 55.5%.
"""
import re
import sys
from collections import Counter

ROW = re.compile(r'^NZCV_ROW class="([^"]*)" verdict=(\S+) producers=(\d+) sites=(\d+) traversals=(\d+)')
KIND = re.compile(r'^NZCV_EXITKIND kind=(\S+) sites=(\d+) traversals=(\d+)')
META = re.compile(r'^NZCV_META pid=(\d+) units=(\d+) exit_sites=(\d+) producers=(\d+) site_slot_overflow=(\d+)')

DROPPABLE = ('droppable-simple', 'droppable-follow-bl')


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    prod, sites, trav = Counter(), Counter(), Counter()
    kprod, ksites, ktrav = Counter(), Counter(), Counter()
    units = exits = producers = overflow = 0
    procs = 0
    for path in sys.argv[1:]:
        with open(path) as f:
            for line in f:
                m = ROW.match(line)
                if m:
                    key = (m.group(1), m.group(2))
                    prod[key] += int(m.group(3))
                    sites[key] += int(m.group(4))
                    trav[key] += int(m.group(5))
                    continue
                m = KIND.match(line)
                if m:
                    ksites[m.group(1)] += int(m.group(2))
                    ktrav[m.group(1)] += int(m.group(3))
                    continue
                m = META.match(line)
                if m:
                    procs += 1
                    units += int(m.group(2))
                    exits += int(m.group(3))
                    producers += int(m.group(4))
                    overflow += int(m.group(5))

    all_trav = sum(ktrav.values())
    const_only = [k for k in prod if k[0] == 'exit: constant only']
    const_prod = sum(prod[k] for k in const_only)
    const_trav = sum(trav[k] for k in const_only)
    exit_prod = sum(v for k, v in prod.items() if k[0].startswith('exit:'))
    exit_trav = sum(v for k, v in trav.items() if k[0].startswith('exit:'))

    def pct(a, b):
        return f'{100.0 * a / b:5.1f}%' if b else '    -'

    print(f'processes: {procs}  units: {units}  exit sites: {exits}  kept producers: {producers}  '
          f'site-slot overflow: {overflow}')
    print(f'executed exit traversals: {all_trav}')
    print()
    print('## exit kinds: every instrumented ExitFunction bumps exactly one, so these sum to the total')
    print(f'{"kind":22} {"sites":>10} {"traversals":>14} {"share":>7} {"per site":>10}')
    for k, v in ktrav.most_common():
        print(f'{k:22} {ksites[k]:10d} {v:14d} {pct(v, all_trav)} {v / ksites[k]:10.0f}' if ksites[k]
              else f'{k:22} {ksites[k]:10d} {v:14d} {pct(v, all_trav)}')
    print()
    print('## kept producers by census class and peek verdict, static share vs executed share')
    print(f'{"class":40} {"verdict":26} {"producers":>9} {"stat%":>6} {"sites":>8} {"traversals":>14} {"exec%":>6} {"per site":>9}')
    for key in sorted(prod, key=lambda k: -trav[k]):
        c, v = key
        print(f'{c:40} {v:26} {prod[key]:9d} {pct(prod[key], producers)} {sites[key]:8d} '
              f'{trav[key]:14d} {pct(trav[key], all_trav)} '
              f'{(trav[key] / sites[key]) if sites[key] else 0:9.0f}')
    print()
    print('## the answer: compares the scan marks dead, static share vs executed share')
    for label, keys in (
            ('simple scan (SS5.2 row 1)', [('exit: constant only', 'droppable-simple')]),
            ('follow-bl scan (SS5.2 row 2)',
             [('exit: constant only', 'droppable-simple'), ('exit: constant only', 'droppable-follow-bl')])):
        p = sum(prod.get(k, 0) for k in keys)
        s = sum(sites.get(k, 0) for k in keys)
        t = sum(trav.get(k, 0) for k in keys)
        print(f'{label}:')
        print(f'  droppable producers {p} = {pct(p, const_prod)} of constant-exit-only, {pct(p, producers)} of all kept')
        print(f'  their exit sites    {s}')
        print(f'  their traversals    {t} = {pct(t, const_trav)} of constant-exit-only traversals, '
              f'{pct(t, exit_trav)} of flag-carrying exit traversals, {pct(t, all_trav)} of all exit traversals')
    print()
    print(f'constant-exit-only: producers {const_prod} ({pct(const_prod, producers)} of kept), '
          f'traversals {const_trav} ({pct(const_trav, all_trav)} of all)')
    print(f'producers whose flags reach any exit: {exit_prod} ({pct(exit_prod, producers)}), '
          f'traversals {exit_trav} ({pct(exit_trav, all_trav)})')


if __name__ == '__main__':
    main()
