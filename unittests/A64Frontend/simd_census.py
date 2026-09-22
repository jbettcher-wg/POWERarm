#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Counts the FP/SIMD encodings present in AArch64 binaries.

Every instruction word in the executable sections (objdump -d) is decoded
against the frontend's own decode table (a64.inc) with the same
specificity order as DecodeTable.cpp, so each word is attributed to exactly
the entry the frontend would dispatch to. Words that decode to an entry in a
SIMD/FP load/store group or a "FP and SIMD" data-processing group, or to
MRS/MSR of FPCR/FPSR, are counted.

Functions that are glibc IFUNC variants the presented CPU (HWCAP
fp|asimd|cpuid, MIDR Cortex-A76, no SVE, no MOPS) never selects are counted
separately, when the binary has symbols.

With --missing-only, only entries that have no translator in the frontend's
handler table (A64Frontend/IRBuilder.cpp) are counted, plus words that match
no decoder entry at all inside the FP/SIMD encoding space (op0 x111).

Usage: simd_census.py [--markdown] [--missing-only] NAME=BINARY...
"""

import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INC = os.path.join(HERE, "../../FEXCore/Source/Interface/Core/A64Frontend/a64.inc")
HANDLERS = os.path.join(HERE, "../../FEXCore/Source/Interface/Core/A64Frontend/IRBuilder.cpp")


def load_handlers():
    return set(re.findall(r'\{"(\w+)", &IRBuilder::', open(HANDLERS).read()))

# glibc aarch64 multiarch variants that the ifunc selectors never return for
# a non-SVE, non-MOPS Cortex-A76 (sysdeps/aarch64/multiarch/*.c).
UNREACHABLE_VARIANT = re.compile(r"_(sve|mops|a64fx|thunderx2?|falkor|emag|kunpeng|oryon1|zva64|nozva)\b|_sve_")

SIMD_LOADSTORE = {
    "STx_mult_1", "STx_mult_2", "LDx_mult_1", "LDx_mult_2", "LDR_lit_fpsimd", "STNP_LDNP_fpsimd",
    "STP_LDP_fpsimd", "STUR_fpsimd", "LDUR_fpsimd", "STR_imm_fpsimd_1", "STR_imm_fpsimd_2",
    "LDR_imm_fpsimd_1", "LDR_imm_fpsimd_2", "STR_reg_fpsimd", "LDR_reg_fpsimd",
}


HOISTED = {"MOVI, MVNI, ORR, BIC (vector, immediate)", "FMOV (vector, immediate)", "Unallocated SIMD modified immediate"}


def load_table():
    entries = []
    section = None
    for line in open(INC):
        if line.startswith("// "):
            section = line[3:].strip()
            continue
        m = re.match(r'INST\((\w+),\s*"([^"]*)",\s*"([01a-zA-Z-]{32})"\)', line)
        if not m:
            continue
        name, desc, bits = m.groups()
        mask = expect = 0
        for i, c in enumerate(bits):
            b = 1 << (31 - i)
            if c == "0":
                mask |= b
            elif c == "1":
                mask |= b
                expect |= b
        simd = "FP and SIMD" in section or "Advanced SIMD" in section or name in SIMD_LOADSTORE or "_sngl_" in name or name.startswith(("LD1R", "LD2R", "LD3R", "LD4R"))
        cls = section.replace("Data Processing - FP and SIMD - ", "").replace("Loads and stores - ", "")
        if name in SIMD_LOADSTORE:
            cls = "SIMD&FP register load/store"
        first = desc in HOISTED
        entries.append((name, mask, expect, simd, cls, first))
    # DecodeTable.cpp: stable sort, more fixed bits first, then dynarmic's
    # hoisted modified-immediate entries in front.
    entries.sort(key=lambda e: -bin(e[1]).count("1"))
    entries.sort(key=lambda e: not e[5])
    return entries


def decode(entries, word):
    for e in entries:
        if word & e[1] == e[2]:
            return e
    return None


FPCR_MRS = (0xD53B4400, 0xD53B4420)  # MRS FPCR / FPSR, Rt masked off
FPCR_MSR = (0xD51B4400, 0xD51B4420)


import shutil

OBJDUMP = os.environ.get("OBJDUMP", "llvm-objdump" if shutil.which("llvm-objdump") else "objdump")


def census(entries, path, missing_only=False, handlers=frozenset()):
    out = subprocess.run([OBJDUMP, "-d", path], capture_output=True, text=True, check=True).stdout
    func = "?"
    counts = collections.Counter()  # (reachable, cls, name, mnemonic) -> n
    funcs = collections.defaultdict(set)
    cache = {}
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if m:
            func = m.group(1)
            continue
        m = re.match(r"^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s+(\S+)", line)
        if not m:
            continue
        word = int(m.group(1), 16)
        mnem = m.group(2)
        reachable = not UNREACHABLE_VARIANT.search(func)
        if (word & ~0x1F) in FPCR_MRS or (word & ~0x1F) in FPCR_MSR:
            if missing_only:
                continue
            key = (reachable, "System register FPCR/FPSR", "MRS" if (word & ~0x1F) in FPCR_MRS else "MSR_reg", mnem)
            counts[key] += 1
            funcs[key].add(func)
            continue
        if word not in cache:
            cache[word] = decode(entries, word)
        e = cache[word]
        if e is None:
            # No decoder entry. Within the FP/SIMD space (op0 bits 28:25 == x111)
            # this is an instruction the table itself lacks (for example SVE).
            if missing_only and (word >> 25) & 0x7 == 0x7 and mnem not in (".inst", "udf"):
                key = (reachable, "No decoder entry (FP/SIMD space)", "-", mnem)
                counts[key] += 1
                funcs[key].add(func)
            continue
        if not e[3]:
            continue
        # Handlers that reject part of their entry's space count as missing
        # for that part: LD2-4/ST2-4 behind the LD1/ST1 multiple-structure entries.
        partial = e[0].startswith(("LDx_mult", "STx_mult")) and mnem not in ("ld1", "st1")
        if missing_only and e[0] in handlers and not partial:
            continue
        key = (reachable, e[4], e[0], mnem)
        counts[key] += 1
        funcs[key].add(func)
    return counts, funcs


def main():
    args = sys.argv[1:]
    markdown = False
    missing_only = False
    while args and args[0].startswith("--"):
        if args[0] == "--markdown":
            markdown = True
        elif args[0] == "--missing-only":
            missing_only = True
        args = args[1:]
    entries = load_table()
    handlers = load_handlers() if missing_only else frozenset()
    total = collections.Counter()
    per_bin = collections.defaultdict(collections.Counter)
    where = collections.defaultdict(set)
    for arg in args:
        name, path = arg.split("=", 1)
        counts, funcs = census(entries, path, missing_only, handlers)
        for k, v in counts.items():
            total[k] += v
            per_bin[k][name] = v
            for f in funcs[k]:
                where[k].add(f)
    names = [a.split("=", 1)[0] for a in args]

    by_class = collections.defaultdict(list)
    for k, v in total.items():
        by_class[(k[0], k[1])].append((k, v))

    classes = sorted(by_class, key=lambda c: (not c[0], -sum(v for _, v in by_class[c])))
    for reach, cls in classes:
        rows = sorted(by_class[(reach, cls)], key=lambda kv: -kv[1])
        n = sum(v for _, v in rows)
        tag = "" if reach else " (unreachable IFUNC variants only)"
        if markdown:
            print(f"\n### {cls}{tag}: {n}\n")
            print("| Entry | Mnemonic | Total | " + " | ".join(names) + " | Example functions |")
            print("|---|---|---:|" + "---:|" * len(names) + "---|")
            for k, v in rows:
                cells = " | ".join(str(per_bin[k].get(b, 0)) for b in names)
                ex = ", ".join(sorted(where[k])[:3])
                print(f"| {k[2]} | {k[3]} | {v} | {cells} | {ex} |")
        else:
            print(f"== {cls}{tag}: {n}")
            for k, v in rows:
                print(f"  {k[2]:24} {k[3]:10} {v:6}  " + " ".join(f"{b}={per_bin[k].get(b, 0)}" for b in names))


if __name__ == "__main__":
    main()
