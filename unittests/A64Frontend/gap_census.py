#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Lists the instruction encodings in AArch64 binaries that the frontend has
no translator for.

Every word in the executable sections (objdump -d) is decoded against a64.inc
in DecodeTable.cpp's order (see simd_census.py). A word counts as a gap when
its entry has no handler in IRBuilder.cpp's name table, or doesn't decode at
all. Handlers that reject some operand combinations at translation time are
not visible here. glibc IFUNC variants the presented CPU never selects are
listed separately.

Usage: gap_census.py [--markdown] BINARY...
"""

import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import simd_census  # noqa: E402

IRBUILDER = os.path.join(HERE, "../../FEXCore/Source/Interface/Core/A64Frontend/IRBuilder.cpp")


def handled_names():
    return set(re.findall(r'\{"(\w+)",\s*&IRBuilder::', open(IRBUILDER).read()))


def main():
    args = sys.argv[1:]
    markdown = args[:1] == ["--markdown"]
    if markdown:
        args = args[1:]
    entries = simd_census.load_table()
    handled = handled_names()
    counts = collections.Counter()
    example = {}
    funcs = collections.defaultdict(set)
    bins = collections.defaultdict(set)
    import shutil
    objdump = os.environ.get("OBJDUMP", "llvm-objdump" if shutil.which("llvm-objdump") else "objdump")
    for path in args:
        out = subprocess.run([objdump, "-d", path], capture_output=True, text=True, check=True).stdout
        func = "?"
        cache = {}
        for line in out.splitlines():
            m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
            if m:
                func = m.group(1)
                continue
            m = re.match(r"^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)$", line)
            if not m:
                continue
            word = int(m.group(2), 16)
            if word not in cache:
                cache[word] = simd_census.decode(entries, word)
            e = cache[word]
            name = e[0] if e else "(undecoded)"
            if e and name in handled:
                continue
            if m.group(3).startswith((".inst", "udf")):
                continue
            reach = not simd_census.UNREACHABLE_VARIANT.search(func)
            key = (reach, name)
            counts[key] += 1
            funcs[key].add(func)
            bins[key].add(os.path.basename(path))
            example.setdefault(key, (m.group(2), m.group(3).split("//")[0].strip(), os.path.basename(path), func))
    rows = sorted(counts.items(), key=lambda kv: (not kv[0][0], -kv[1]))
    if markdown:
        print("| Entry | Count | Example word | Disassembly | Binaries | Example function |")
        print("|---|---:|---|---|---|---|")
    for (reach, name), n in rows:
        w, dis, b, f = example[(reach, name)]
        tag = "" if reach else " (unselected IFUNC variants only)"
        if markdown:
            print(f"| {name}{tag} | {n} | `{w}` | `{dis}` | {', '.join(sorted(bins[(reach, name)])[:4])} | {f} |")
        else:
            print(f"{name + tag:40} {n:7}  {w} {dis:40} {b} {f}")


if __name__ == "__main__":
    main()
