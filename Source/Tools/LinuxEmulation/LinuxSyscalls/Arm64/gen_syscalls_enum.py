#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate SyscallsEnum.h (AArch64 guest syscall numbers) from a Linux tree.

The numbers come from include/uapi/asm-generic/unistd.h, evaluated the way an
arm64 64-bit userspace build sees it (the __ARCH_WANT_* set arm64 selects).
The result is cross-checked against arch/arm64/tools/syscall_64.tbl filtered
by the ABIs arm64 enables in arch/arm64/kernel/Makefile.syscalls; any
difference is an error.

Usage: gen_syscalls_enum.py <linux-source-dir> <output SyscallsEnum.h>
"""

import os
import re
import sys

# 64-bit arm64 userspace view of asm-generic/unistd.h.
ARM64_DEFINES = {
    "__ARCH_WANT_NEW_STAT",
    "__ARCH_WANT_RENAMEAT",
    "__ARCH_WANT_SET_GET_RLIMIT",
    "__ARCH_WANT_MEMFD_SECRET",
}
BITS_PER_LONG = 64
# ABIs listed for arm64 in syscall_64.tbl (common + 64 + arch-selected extras).
ARM64_TBL_ABIS = {"common", "64", "renameat", "rlimit", "memfd_secret"}


def eval_condition(expr, defines):
    expr = expr.split("/*")[0].strip()
    expr = re.sub(r"defined\s*\(\s*(\w+)\s*\)", lambda m: str(m.group(1) in defines), expr)
    expr = re.sub(r"defined\s+(\w+)", lambda m: str(m.group(1) in defines), expr)
    expr = expr.replace("__BITS_PER_LONG", str(BITS_PER_LONG))
    expr = expr.replace("||", " or ").replace("&&", " and ")
    expr = re.sub(r"!(?!=)", " not ", expr)
    return bool(eval(expr, {"__builtins__": {}}, {}))


def parse_unistd(path):
    numbers = {}
    aliases = {}
    stack = []  # (taking_this_branch, any_branch_taken)
    defines = set(ARM64_DEFINES)

    def active():
        return all(s[0] for s in stack)

    for raw in open(path):
        line = raw.strip()
        if line.startswith("#ifdef"):
            cond = line.split()[1] in defines
            stack.append([cond, cond])
        elif line.startswith("#ifndef"):
            cond = line.split()[1] not in defines
            stack.append([cond, cond])
        elif line.startswith("#if"):
            cond = eval_condition(line[3:], defines)
            stack.append([cond, cond])
        elif line.startswith("#elif"):
            top = stack[-1]
            cond = (not top[1]) and eval_condition(line[5:], defines)
            top[0] = cond
            top[1] = top[1] or cond
        elif line.startswith("#else"):
            top = stack[-1]
            top[0] = not top[1]
            top[1] = True
        elif line.startswith("#endif"):
            stack.pop()
        elif not active():
            continue
        elif line.startswith("#define"):
            m = re.match(r"#define\s+__NR(3264)?_(\w+)\s+(\S+)", line)
            if not m:
                continue
            is3264, name, value = m.group(1), m.group(2), m.group(3)
            # __NR_syscalls is the table size; __NR_arch_specific_syscall marks the
            # start of the per-arch range (244), not a syscall.
            if name in ("syscalls", "arch_specific_syscall"):
                continue
            key = ("3264_" if is3264 else "") + name
            if re.fullmatch(r"\d+", value):
                numbers[key] = int(value)
            else:
                aliases[key] = value
        elif line.startswith("#undef"):
            m = re.match(r"#undef\s+__NR(3264)?_(\w+)", line)
            if m:
                key = ("3264_" if m.group(1) else "") + m.group(2)
                numbers.pop(key, None)
                aliases.pop(key, None)

    result = {}
    for key, number in numbers.items():
        if not key.startswith("3264_"):
            result[key] = number
    for key, target in aliases.items():
        if key.startswith("3264_"):
            continue
        m = re.fullmatch(r"__NR(3264)?_(\w+)", target)
        if not m:
            raise SystemExit(f"unresolvable alias {key} -> {target}")
        target_key = ("3264_" if m.group(1) else "") + m.group(2)
        result[key] = numbers[target_key]
    return result


def parse_tbl(path):
    result = {}
    for raw in open(path):
        line = raw.split("#")[0].strip()
        if not line:
            continue
        fields = line.split()
        number, abi, name = int(fields[0]), fields[1], fields[2]
        if abi in ARM64_TBL_ABIS:
            result[name] = number
    return result


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    linux, output = sys.argv[1], sys.argv[2]
    unistd = os.path.join(linux, "include/uapi/asm-generic/unistd.h")
    table = parse_unistd(unistd)

    tbl_path = os.path.join(linux, "arch/arm64/tools/syscall_64.tbl")
    if os.path.exists(tbl_path):
        tbl = parse_tbl(tbl_path)
        if tbl != table:
            only_h = sorted(set(table.items()) - set(tbl.items()))
            only_t = sorted(set(tbl.items()) - set(table.items()))
            raise SystemExit(f"unistd.h and syscall_64.tbl disagree:\n  unistd.h only: {only_h}\n  tbl only: {only_t}")

    version = "unknown"
    makefile = os.path.join(linux, "Makefile")
    if os.path.exists(makefile):
        fields = {}
        for raw in open(makefile):
            m = re.match(r"(VERSION|PATCHLEVEL|SUBLEVEL)\s*=\s*(\d+)", raw)
            if m:
                fields[m.group(1)] = m.group(2)
            if len(fields) == 3:
                break
        version = f"{fields.get('VERSION')}.{fields.get('PATCHLEVEL')}.{fields.get('SUBLEVEL')}"

    by_number = sorted(table.items(), key=lambda kv: (kv[1], kv[0]))
    with open(output, "w") as out:
        out.write("// SPDX-License-Identifier: MIT\n")
        out.write("// Generated by gen_syscalls_enum.py from Linux " + version + "\n")
        out.write("// include/uapi/asm-generic/unistd.h (arm64 64-bit view, cross-checked against\n")
        out.write("// arch/arm64/tools/syscall_64.tbl). Do not edit by hand.\n")
        out.write("#pragma once\n\n")
        out.write("namespace FEX::HLE::Arm64 {\n")
        out.write("enum Syscalls_Arm64 {\n")
        for name, number in by_number:
            out.write(f"  SYSCALL_Arm64_{name} = {number},\n")
        out.write(f"  SYSCALL_Arm64_MAX = {by_number[-1][1] + 1},\n")
        out.write("};\n")
        out.write("} // namespace FEX::HLE::Arm64\n")
    print(f"{len(table)} syscalls, max {by_number[-1][1]}")


if __name__ == "__main__":
    main()
