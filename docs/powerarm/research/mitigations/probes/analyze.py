#!/usr/bin/env python3
import sys, re, collections

rows = []
for line in open(sys.argv[1]):
    if not line.startswith("kernel="):
        continue
    d = {}
    for kv in line.split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            if v.endswith("/unit"):
                k, v = k + "_per_unit", v[:-5]
            try: d[k] = float(v)
            except ValueError: d[k] = v
    rows.append(d)

def key(r): return (r["kernel"], r["param"], r["R"])
by = collections.defaultdict(dict)
for r in rows:
    by[key(r)][r["sys"]] = r

FREQ = 3.769e9   # measured: cyc_u / ns from the null kernel

print("=== 1. probe kernel: L1 lookup probe (2 MiB table, 2 dependent loads) ===")
print("hot = distinct hot blocks; R = probes between syscalls")
print(f"{'hot':>6} {'R':>6} {'cyc/probe':>10} {'warm_ns':>8} | "
      f"{'getppid dcyc_u/sys':>18} {'dL1miss/sys':>12} {'dns/sys':>9} | {'vdso dcyc_u':>12}")
for (k, p, R), m in sorted(by.items()):
    if k != "probe" or "none" not in m: continue
    n = m["none"]
    out = f"{int(p):6d} {int(R):6d} {n['cyc_per_unit']:10.2f} {n['ns_per_unit']:8.2f} | "
    for sy in ("getppid",):
        if sy in m:
            s = m[sy]
            dc = s["cyc_per_syscall"] - n["cyc_per_syscall"]
            dm = s["0x3e054_per_sys"] - n["0x3e054_per_sys"]
            dns = (s["ns_per_unit"] - n["ns_per_unit"]) * R
            out += f"{dc:18.1f} {dm:12.2f} {dns:9.1f} | "
    if "vdso" in m:
        out += f"{m['vdso']['cyc_per_syscall'] - n['cyc_per_syscall']:12.1f}"
    print(out)

print()
print("=== 2. null kernel: the syscall itself (no cache/predictor state to lose) ===")
print(f"{'R':>6} {'sys':>10} {'dcyc_u/sys':>11} {'dL1miss/sys':>12} {'dns/sys':>9} {'total_cyc/sys':>13}")
for (k, p, R), m in sorted(by.items()):
    if k != "null" or "none" not in m: continue
    n = m["none"]
    for sy in ("vdso", "getppid", "madvise", "mprotect", "dontneed"):
        if sy not in m: continue
        s = m[sy]
        dc = s["cyc_per_syscall"] - n["cyc_per_syscall"]
        dm = s["0x3e054_per_sys"] - n["0x3e054_per_sys"]
        dns = (s["ns_per_unit"] - n["ns_per_unit"]) * R
        print(f"{int(R):6d} {sy:>10} {dc:11.1f} {dm:12.2f} {dns:9.1f} {dns*FREQ/1e9:13.1f}")

print()
print("=== 3. ccache kernel: monomorphic bctr sites (count cache) ===")
print(f"{'sites':>6} {'R':>6} {'warm cyc/bctr':>13} {'warm mpred':>10} | "
      f"{'dcyc_u/sys':>10} {'dmpred_ccache/sys':>17} {'dns/sys':>9} | {'ctrl dcyc_u/sys':>15} {'net':>8}")
for (k, p, R), m in sorted(by.items()):
    if k != "ccache" or "none" not in m: continue
    n, s = m["none"], m.get("getppid")
    if not s: continue
    dc = s["cyc_per_syscall"] - n["cyc_per_syscall"]
    dmp = s["0x40ac_per_sys"] - n["0x40ac_per_sys"]
    dns = (s["ns_per_unit"] - n["ns_per_unit"]) * R
    cm = by.get(("ctrl", p, R), {})
    cd = (cm["getppid"]["cyc_per_syscall"] - cm["none"]["cyc_per_syscall"]) if ("none" in cm and "getppid" in cm) else float('nan')
    print(f"{int(p):6d} {int(R):6d} {n['cyc_per_unit']:13.2f} {n['0x40ac_per_unit']:10.4f} | "
          f"{dc:10.1f} {dmp:17.2f} {dns:9.1f} | {cd:15.1f} {dc-cd:8.1f}")

print()
print("=== 4. lstack kernel: nested bl/blr, depth 24 (link stack) ===")
print(f"{'R':>6} {'warm cyc/descent':>16} {'warm mpred_lstack':>17} | {'dcyc_u/sys':>10} {'dmpred_lstack/sys':>17} {'dns/sys':>9}")
for (k, p, R), m in sorted(by.items()):
    if k != "lstack" or "none" not in m: continue
    n, s = m["none"], m.get("getppid")
    if not s: continue
    dc = s["cyc_per_syscall"] - n["cyc_per_syscall"]
    dmp = s["0x48ac_per_sys"] - n["0x48ac_per_sys"]
    dns = (s["ns_per_unit"] - n["ns_per_unit"]) * R
    print(f"{int(R):6d} {n['cyc_per_unit']:16.2f} {n['0x48ac_per_unit']:17.4f} | {dc:10.1f} {dmp:17.2f} {dns:9.1f}")
