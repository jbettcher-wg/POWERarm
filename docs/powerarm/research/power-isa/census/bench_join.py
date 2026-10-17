#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rank lowering candidates by measured cycles times real site counts.

The two halves of the evidence have existed separately for a while and were
never joined:

  * research/neon/probes/neon_bench_p9.c measures what a HOST sequence costs on
    this POWER9, in core cycles, latency and throughput.
  * this directory's *.strengths.census counts how often each GUEST shape
    appears in real binaries (cc1, the Claude CLI, libxul, VS Code, Factorio).

Ranking by static instruction count -- which is what ISA-OPPORTUNITIES.md and
POWER-STRENGTHS.md do -- is a proxy that measurement has now shown can invert:
`setb` replaces six instructions with two and is slower on both columns, and
`stp_preindex_stdu` replaces three with two and is slower on throughput.

WHAT THIS DOES NOT TELL YOU. The censuses here are STATIC site counts, not
execution counts (only cc1-nzcv-exec.census and libxul-nzcv-exec.census are
execution-weighted, and only for exit sites). A site inside a loop runs millions
of times and a site in an error path runs never, and this script cannot tell
them apart. So read the output as a RANKING SIGNAL -- "which of these is worth
implementing first" -- and never as a predicted speedup.

Ranked on throughput, because the question is the aggregate cost of executing
many independent sites; latency only governs a dependency chain. Both are shown.

Usage:
  bench_join.py BENCH_OUTPUT [census-prefix ...]      # default: all five binaries
"""
import sys, os, collections

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BINARIES = ["cc1", "claude", "libxul", "code", "factorio"]

# Each row names the two benched sequences and the guest shapes the swap bears
# on. `shapes` is empty where the census has no matching shape -- then only the
# per-site delta is meaningful. `note` records every known over-count, because a
# shape count is an upper bound on idiom occurrences unless the idiom IS the
# shape.
ITEMS = [
    dict(item="ISA-OPP 3.9", what="SDIV+MSUB remainder -> modsd",
         cur="rem_divd_mulld_subf", cand="rem_modsd", shapes=["sdiv", "udiv"],
         note="gated on a divide being present; msub counts are higher and include non-remainder uses"),
    dict(item="ISA-OPP 3.12", what="three-way compare -> setb",
         cur="threeway_isel", cand="threeway_setb", shapes=["csinv"],
         note="upper bound: not every csinv is a three-way compare"),
    dict(item="ISA-OPP 3.13", what="pre-index STP -> stdu",
         cur="stp_preindex_addi", cand="stp_preindex_stdu", shapes=[],
         note="no stp_preindex shape in these censuses; per-site delta only"),
    dict(item="STRENGTHS 2.1", what="lane test -> vcmpequb. + bc  (LANDED)",
         cur="umaxv16_fold10", cand="vcmpequb_rc_bc", shapes=["umaxv_v", "uminv_v"],
         note="landed; kept as the method's check against a known-good decision"),
    dict(item="STRENGTHS 2.1b", what="addv 3-op -> 4-op rotate form",
         cur="addv16_3", cand="addv32_rot4", shapes=["addv_v"],
         note="more instructions, less time: the clearest case against static counts"),
    dict(item="ISA-OPP 3.14a", what="LSE RMW -> ISA 3.0 AMO (ldat)",
         cur="rmw_fetchadd_larx", cand="amo_ldat_add", shapes=[],
         note="REJECTED: 70 -> 796 cycles. Present on this POWER9 but unpipelined, i.e. assisted"),
    dict(item="ISA-OPP 3.14b", what="CR0 save mfocrf -> mcrf  (LANDED)",
         cur="cr0_save_mfocrf", cand="cr0_save_mcrf", shapes=[],
         note="landed; 15% of an unfenced atomic, and the AMO row above is what is rejected"),
    dict(item="FP16 narrowing", what="GPR-built word splat -> pooled constant",
         cur="const_gpr_splat", cand="lxv_const", shapes=[],
         note="internal to one lowering, not a guest shape; the FP16 narrowing builds eight"),
]


def read_bench(path):
    """name -> (insns, lat_cyc, tp_cyc) from the probe's table."""
    out = {}
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.split()
            if len(p) != 6:
                continue
            try:
                out[p[0]] = (int(p[1]), float(p[4]), float(p[5]))
            except ValueError:
                continue
    return out


def read_census(prefix):
    """shape -> static site count, from <prefix>.strengths.census."""
    path = os.path.join(HERE, prefix + ".strengths.census")
    if not os.path.exists(path):
        return None
    out = {}
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) >= 2:
                try:
                    out[p[0]] = int(p[1])
                except ValueError:
                    pass
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    bench = read_bench(sys.argv[1])
    if not bench:
        sys.exit("no table rows parsed from %s" % sys.argv[1])
    names = sys.argv[2:] or DEFAULT_BINARIES

    censuses = {}
    for n in names:
        c = read_census(n)
        if c is None:
            print("# no census for %r, skipped" % n)
        else:
            censuses[n] = c

    print("# Measured host cycles joined with static guest site counts.")
    print("# STATIC sites: a ranking signal, NOT a predicted speedup. See the module docstring.")
    print("# delta = current - candidate, in core cycles; positive means the candidate is cheaper.")
    print()
    print("%-15s %-44s %9s %9s %10s %12s" % ("item", "change", "d lat", "d tp", "sites", "tp*sites"))

    rows = []
    for it in ITEMS:
        cur, cand = bench.get(it["cur"]), bench.get(it["cand"])
        if not cur or not cand:
            missing = it["cur"] if not cur else it["cand"]
            rows.append((None, it, "not benched (%s)" % missing, 0, 0, 0))
            continue
        d_lat = cur[1] - cand[1]
        d_tp = cur[2] - cand[2]
        sites = 0
        for sh in it["shapes"]:
            for c in censuses.values():
                sites += c.get(sh, 0)
        rows.append((d_tp * sites if it["shapes"] else None, it, None, d_lat, d_tp, sites))

    # Items with a site count rank by expected cycles; the rest follow, since an
    # unweighted per-site delta cannot be compared against a weighted total.
    rows.sort(key=lambda r: (r[0] is None, -(r[0] or 0)))

    for score, it, err, d_lat, d_tp, sites in rows:
        if err:
            print("%-15s %-44s %s" % (it["item"], it["what"], err))
            continue
        print("%-15s %-44s %+9.2f %+9.2f %10s %12s" % (
            it["item"], it["what"], d_lat, d_tp,
            sites if it["shapes"] else "-",
            ("%.3g" % score) if score is not None else "-"))

    print()
    print("# Notes (every over-count is recorded; a shape count bounds idiom occurrences)")
    for it in ITEMS:
        print("#   %-15s %s" % (it["item"], it["note"]))
    print("#")
    print("# Censuses joined: %s" % (", ".join(sorted(censuses)) or "none"))


if __name__ == "__main__":
    main()
