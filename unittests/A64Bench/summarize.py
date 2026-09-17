#!/usr/bin/env python3
"""Summarise A64Bench CSVs into a markdown table, refusing timings where parity fails.

usage: summarize.py results/pi.csv results/power9.csv

cold = rep 0 (includes translation under POWERarm); warm = median of reps >= 1.
Each cell is the median over process runs, with [min-max] of the per-run values.
"""
import csv
import statistics
import sys
from collections import defaultdict


def load(paths):
    rows = []
    for p in paths:
        with open(p) as f:
            rows += list(csv.DictReader(line for line in f if not line.startswith("#")))
    return rows


def main():
    rows = load(sys.argv[1:])
    ref = {}  # bench -> checksum from the AArch64 reference at default scale
    per = defaultdict(lambda: defaultdict(dict))  # (config, bench) -> run -> rep -> ns
    wall = defaultdict(dict)
    sums = defaultdict(set)
    bad = []
    for r in rows:
        key = (r["config"], r["bench"])
        if r["rep"] == "summary":
            wall[key][r["run"]] = int(r["wall_ns"])
            if r["parity"] != "ok":
                bad.append(f"{key}: run {r['run']} parity={r['parity']}")
            continue
        per[key][r["run"]][int(r["rep"])] = int(r["ns"])
        sums[key].add(r["sum"])
        if r["config"] == "native-aarch64":
            ref[r["bench"]] = r["sum"]

    benches = sorted({b for _, b in per})
    print("## Parity\n")
    print("| bench | reference (Pi) | " + " | ".join(c for c in ("native-power8", "native-power9", "powerarm")) + " |")
    print("|---|---|---|---|---|")
    ok = {}
    for b in benches:
        cells = []
        good = b in ref
        for c in ("native-power8", "native-power9", "powerarm"):
            s = sums.get((c, b), set())
            match = s == {ref.get(b)}
            good &= match
            cells.append("match" if match else ("missing" if not s else "MISMATCH " + ",".join(sorted(s))))
        ok[b] = good
        print(f"| {b} | `{ref.get(b, '-')}` | " + " | ".join(cells) + " |")
    for line in bad:
        print(f"\nparity failure: {line}")

    def stat(key, which):
        vals = []
        for run, reps in per.get(key, {}).items():
            if which == "cold":
                if 0 in reps:
                    vals.append(reps[0])
            elif which == "warm":
                w = [v for k, v in reps.items() if k >= 1]
                if w:
                    vals.append(statistics.median(w))
            elif which == "wall":
                vals.append(wall[key][run])
        if not vals:
            return None
        return statistics.median(vals), min(vals), max(vals)

    def ms(s):
        return "-" if s is None else f"{s[0] / 1e6:.0f} [{s[1] / 1e6:.0f}-{s[2] / 1e6:.0f}]"

    print("\n## Timing (ms per repetition: median [min-max] over process runs)\n")
    print("| bench | Pi 5 warm | P9 native -mcpu=power8 | P9 native -mcpu=power9 | POWERarm cold (rep 0) | POWERarm warm | POWERarm / Pi | POWERarm / native P9 | startup wall POWERarm / native |")
    print("|---|---|---|---|---|---|---|---|---|")
    for b in benches:
        if not ok[b]:
            print(f"| {b} | parity failed: no timings reported | | | | | | | |")
            continue
        pi = stat(("native-aarch64", b), "warm")
        p8 = stat(("native-power8", b), "warm")
        p9 = stat(("native-power9", b), "warm")
        ec = stat(("powerarm", b), "cold")
        ew = stat(("powerarm", b), "warm")
        su_e = stat(("powerarm-startup", b), "wall")
        su_n = stat(("native-power9-startup", b), "wall")
        r_pi = f"{ew[0] / pi[0]:.2f}x" if ew and pi else "-"
        r_n = f"{ew[0] / p9[0]:.2f}x" if ew and p9 else "-"
        su = f"{su_e[0] / 1e6:.0f} / {su_n[0] / 1e6:.0f} ms" if su_e and su_n else "-"
        print(f"| {b} | {ms(pi)} | {ms(p8)} | {ms(p9)} | {ms(ec)} | {ms(ew)} | {r_pi} | {r_n} | {su} |")

    cs, ce = stat(("control-start", "crc32"), "warm"), stat(("control-end", "crc32"), "warm")
    if cs and ce:
        drift = 100 * (ce[0] - cs[0]) / cs[0]
        print(f"\ncontrol (native-power9 crc32, warm): start {cs[0] / 1e6:.1f} ms, end {ce[0] / 1e6:.1f} ms, drift {drift:+.2f}%"
              + ("  ** CONTROL MOVED >2%: run invalid **" if abs(drift) > 2 else ""))
    busy = [int(r["node_busy_pct"]) for r in rows if r.get("node_busy_pct")]
    if busy:
        print(f"node busy% before each process: median {statistics.median(busy)}, max {max(busy)}")


if __name__ == "__main__":
    main()
