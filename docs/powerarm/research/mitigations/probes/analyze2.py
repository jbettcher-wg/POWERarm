#!/usr/bin/env python3
import sys, collections
rows = []
for line in open(sys.argv[1]):
    if not line.startswith("kernel="): continue
    d = {}
    for kv in line.split():
        if "=" not in kv: continue
        k, v = kv.split("=", 1)
        if v.endswith("/unit"): k, v = k + "_per_unit", v[:-5]
        try: d[k] = float(v)
        except ValueError: d[k] = v
    rows.append(d)
by = collections.defaultdict(dict)
for r in rows: by[(r["kernel"], r["param"], r["R"])][r["sys"]] = r

print("=== link stack: the syscall taken at the bottom of a depth-D bl/blr chain ===")
print(f"{'depth':>6} {'warm cyc/descent':>16} | {'getppid dcyc_u':>14} {'dmpred_lstack':>13} {'dpred_lstack':>12} | {'vdso dcyc_u':>11} {'vdso dmpred':>11}")
for (k, p, R), m in sorted(by.items()):
    if k != "lstackdeep" or "none" not in m: continue
    n = m["none"]
    g, v = m.get("getppid"), m.get("vdso")
    gc = g["cyc_per_syscall"] - n["cyc_per_syscall"] if g else float('nan')
    gm = g["0x48ac_per_sys"] - n["0x48ac_per_sys"] if g else float('nan')
    gp = g["0x40a8_per_sys"] - n["0x40a8_per_sys"] if g else float('nan')
    vc = v["cyc_per_syscall"] - n["cyc_per_syscall"] if v else float('nan')
    vm = v["0x48ac_per_sys"] - n["0x48ac_per_sys"] if v else float('nan')
    print(f"{int(p):6d} {n['cyc_per_unit']:16.1f} | {gc:14.1f} {gm:13.2f} {gp:12.2f} | {vc:11.1f} {vm:11.2f}")

print()
print("=== count cache: distinct monomorphic bctr sites, 64-byte spacing ===")
print(f"{'sites':>6} {'warm cyc/bctr':>13} {'warm mpred/bctr':>15} | {'getppid dcyc_u':>14} {'dmpred_ccache':>13} | "
      f"{'vdso dcyc_u':>11} {'vdso dmpred':>11} | {'ctrl dcyc_u':>11} {'net cyc':>8}")
for (k, p, R), m in sorted(by.items(), key=lambda x: x[0][1]):
    if k != "ccache" or "none" not in m: continue
    n, g, v = m["none"], m.get("getppid"), m.get("vdso")
    if not g: continue
    gc = g["cyc_per_syscall"] - n["cyc_per_syscall"]
    gm = g["0x40ac_per_sys"] - n["0x40ac_per_sys"]
    vc = v["cyc_per_syscall"] - n["cyc_per_syscall"] if v else float('nan')
    vm = v["0x40ac_per_sys"] - n["0x40ac_per_sys"] if v else float('nan')
    cm = by.get(("ctrl", p, R), {})
    cd = (cm["getppid"]["cyc_per_syscall"] - cm["none"]["cyc_per_syscall"]) if ("none" in cm and "getppid" in cm) else float('nan')
    print(f"{int(p):6d} {n['cyc_per_unit']:13.2f} {n['0x40ac_per_unit']:15.5f} | {gc:14.1f} {gm:13.2f} | "
          f"{vc:11.1f} {vm:11.2f} | {cd:11.1f} {gc-cd:8.1f}")
