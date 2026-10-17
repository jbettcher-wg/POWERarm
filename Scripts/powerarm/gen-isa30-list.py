#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Classify every PPC64 emitter instruction by ISA level, using the host assembler.

   gen-isa30-list.py OUT_ISA30 OUT_UNCLASSIFIED

The oracle is /mnt/arch/usr/bin/as, the HOST's ppc64le GNU assembler, which does
enforce `-mpower8` vs `-mpower9`. Nothing else on this box does: clang assembles
`lxv` at -mcpu=power8 without complaint and llvm-objdump decodes it at
--mcpu=pwr8, so both are useless for this. The rootfs's /usr/bin/as is the
AArch64 one, hence the absolute path -- /mnt/arch is the host root as seen from a
shell running under POWERarm.

Method: take every emitter method name as a mnemonic (trailing `_` is the ISA's
trailing `.`), find an operand form that assembles at -mpower9 by trying a list
of templates, then re-assemble those forms at -mpower8. Rejected at power8 and
accepted at power9 means ISA 3.0. Two `as` invocations over one generated file,
because `as` reports every bad line rather than stopping at the first.

This replaces classifying by hand or by comment-mining, both of which I got wrong:
comment-mining missed lxv/setb/cnttzd and wrongly flagged mfocrf/lxsdx, and by
hand I wrongly called xvrspic, xvcvspuxws and vabsdub ISA 3.0 -- the assembler
accepts all three at power8, and the FP16 POWER8 path uses xvrspic deliberately.

The 8 names that no template matches are not instructions: and__/or__/xor__ are
record-form aliases, dc64 emits data, iselcc wraps isel (ISA 2.06), and the
smt_*_priority helpers are `or Rx,Rx,Rx` nops. Reviewed; none emits ISA 3.0.
"""
import re, io, subprocess, sys, os
AS = "/mnt/arch/usr/bin/as"
em = io.open("CodeEmitter/PPC64LE/Emitter.h", encoding="utf-8").read()
methods = sorted(set(re.findall(r'^\s*(?:\[\[[^\]]*\]\]\s*)?void\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(', em, re.M)))
def mnem(m): return m[:-1] + "." if m.endswith("_") else m
# Operand templates, tried in order until one assembles at power9.
T = ["3,4,5", "3,4", "32,33,34", "32,33", "2,3,4", "2,3", "32,0(3)", "3,0(4)",
     "3,4,5,6", "2,3,4,5", "32,3,4", "3,32", "32,3", "3,4,5,6,7", "2,3,4,1",
     "0", "3,0", "3", "", "2,3,4,0", "32,33,34,0", "32,33,0", "1,3,4",
     "3,4,0,0", "3,4,0,31", "2,0,3", "32,0,3", "3,0,4", "2,3,0"]
cands = []  # (mnemonic, text)
for M in methods:
    mn = mnem(M)
    if not re.match(r'^[a-z][a-z0-9_.]*$', mn):
        continue
    for t in T:
        cands.append((mn, (mn + " " + t).strip()))

def run(level, lines):
    src = "\n".join(lines) + "\n"
    p = subprocess.run([AS, "-m" + level, "-o", "/dev/null", "-"],
                       input=src.encode(), capture_output=True)
    bad = set()
    for L in p.stderr.decode(errors="replace").split("\n"):
        m = re.search(r':(\d+): (?:Error|Fatal)', L)
        if m:
            bad.add(int(m.group(1)))
    return bad

# Pass 1: which (mnemonic, form) assemble at power9
bad9 = run("power9", [c[1] for c in cands])
ok9 = {}
for i, (mn, txt) in enumerate(cands, start=1):
    if i not in bad9 and mn not in ok9:
        ok9[mn] = txt
# Pass 2: of those, which are rejected at power8 -> ISA 3.0
items = sorted(ok9.items())
bad8 = run("power8", [t for _, t in items])
isa30 = sorted(mn for i, (mn, _) in enumerate(items, start=1) if i in bad8)
unresolved = sorted(set(mnem(M) for M in methods
                        if re.match(r'^[a-z][a-z0-9_.]*$', mnem(M))) - set(ok9))
print("emitter methods with a mnemonic: %d" % len(set(mnem(M) for M in methods if re.match(r'^[a-z][a-z0-9_.]*$', mnem(M)))))
print("classified (a form assembled at power9): %d" % len(ok9))
print("ISA 3.0 (rejected at power8): %d" % len(isa30))
print("no operand template matched (unclassified): %d" % len(unresolved))
io.open(sys.argv[1], "w").write("\n".join(isa30) + "\n")
io.open(sys.argv[2], "w").write("\n".join(unresolved) + "\n")
print()
print("ISA 3.0:", " ".join(isa30))
