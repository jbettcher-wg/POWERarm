#!/usr/bin/env python3
"""Static host-instruction counts for the SIMD load/store shapes (§4.1 of
VSX-STAGE2-AND-VECTOR-CACHE.md), per register bank and per ISA level.

gen_shapes2.py measured a handful of these but left out `str s`, `str q`,
every pair form, the register-offset forms and the displacements either side
of the DQ bound -- which is most of what §4.1 changes. This generates the
whole set, assembles it, runs it under an emulator with POWERARM_CODEHASHLOG
and prints one row per shape:

    python3 gen_shapes_ldst.py <emu-binary> [<emu-binary-b>]

Each shape is its own 2 KiB-aligned guest block holding 16 copies of the
instruction over 16 different registers of one bank, so a row is
(block bytes - empty block bytes) / 4 / 16 host instructions per guest
instruction. Addresses are symbol-resolved from the guest ELF, so a row is
keyed by name rather than by compile order.

Needs a host clang that can target aarch64 (`--target=aarch64-linux-gnu`,
-nostdlib -static, lld); no guest toolchain and no Pi.
"""
import os
import re
import subprocess
import sys
import tempfile

LO = list(range(0, 16))
HI = list(range(16, 32))


def regs(bank, k, n=1):
    return [bank[(k * n + i) % 16] for i in range(n)]


def build_shapes():
    shapes = []

    def add(name, fmt, bank, n=1):
        shapes.append((name, [fmt.format(*regs(bank, k, n)) for k in range(16)]))

    for bank, tag in ((LO, "lo"), (HI, "hi")):
        # --- single-register scaled-immediate forms, every width
        add(f"ldr_b_{tag}", "ldr b{0}, [x0, #1]", bank)
        add(f"ldr_h_{tag}", "ldr h{0}, [x0, #2]", bank)
        add(f"ldr_s_{tag}", "ldr s{0}, [x0, #4]", bank)
        add(f"ldr_d_{tag}", "ldr d{0}, [x0, #8]", bank)
        add(f"ldr_q_{tag}", "ldr q{0}, [x0, #16]", bank)
        add(f"str_b_{tag}", "str b{0}, [x0, #1]", bank)
        add(f"str_h_{tag}", "str h{0}, [x0, #2]", bank)
        add(f"str_s_{tag}", "str s{0}, [x0, #4]", bank)
        add(f"str_d_{tag}", "str d{0}, [x0, #8]", bank)
        add(f"str_q_{tag}", "str q{0}, [x0, #16]", bank)
        # --- zero displacement: no address arithmetic at all
        add(f"ldr_q0_{tag}", "ldr q{0}, [x0]", bank)
        add(f"str_q0_{tag}", "str q{0}, [x0]", bank)
        # --- unscaled (ldur/stur): a displacement the DQ-form cannot hold
        add(f"ldur_q_{tag}", "ldur q{0}, [x0, #4]", bank)
        add(f"stur_q_{tag}", "stur q{0}, [x0, #4]", bank)
        add(f"ldur_d_{tag}", "ldur d{0}, [x0, #3]", bank)
        add(f"stur_d_{tag}", "stur d{0}, [x0, #3]", bank)
        add(f"ldur_b_{tag}", "ldur b{0}, [x0, #3]", bank)
        add(f"stur_b_{tag}", "stur b{0}, [x0, #3]", bank)
        # --- register-offset forms
        add(f"ldr_q_reg_{tag}", "ldr q{0}, [x0, x1]", bank)
        add(f"str_q_reg_{tag}", "str q{0}, [x0, x1]", bank)
        add(f"ldr_q_sxtw_{tag}", "ldr q{0}, [x0, w1, sxtw #4]", bank)
        add(f"str_q_sxtw_{tag}", "str q{0}, [x0, w1, sxtw #4]", bank)
        add(f"ldr_d_reg_{tag}", "ldr d{0}, [x0, x1]", bank)
        add(f"str_d_reg_{tag}", "str d{0}, [x0, x1]", bank)
        add(f"ldr_b_reg_{tag}", "ldr b{0}, [x0, x1]", bank)
        add(f"str_b_reg_{tag}", "str b{0}, [x0, x1]", bank)
        # --- pairs: the largest low-bank family in the census
        add(f"ldp_q_{tag}", "ldp q{0}, q{1}, [x0, #32]", bank, 2)
        add(f"stp_q_{tag}", "stp q{0}, q{1}, [x0, #32]", bank, 2)
        add(f"ldp_q0_{tag}", "ldp q{0}, q{1}, [x0]", bank, 2)
        add(f"stp_q0_{tag}", "stp q{0}, q{1}, [x0]", bank, 2)
        add(f"ldp_d_{tag}", "ldp d{0}, d{1}, [x0, #16]", bank, 2)
        add(f"stp_d_{tag}", "stp d{0}, d{1}, [x0, #16]", bank, 2)
        add(f"ldp_s_{tag}", "ldp s{0}, s{1}, [x0, #8]", bank, 2)
        add(f"stp_s_{tag}", "stp s{0}, s{1}, [x0, #8]", bank, 2)
        # --- load-and-splat, which shares the LoadMem handler
        add(f"ld1r_s_{tag}", "ld1r {{v{0}.4s}}, [x0]", bank)
        add(f"ld1r_d_{tag}", "ld1r {{v{0}.2d}}, [x0]", bank)
    return shapes


def emit_asm(shapes, path):
    out = [".text", ".globl _start", "_start:", "  adr x0, buf", "  mov x1, #16"]
    # Poison every vector register so nothing can read an incidental zero.
    out += [f"  fmov d{i}, x1" for i in range(32)]
    out += ["  mov x1, #16"]
    for name, _ in shapes:
        out += [f"  adr x9, f_{name}", "  blr x9"]
    out += ["  adr x9, f_empty", "  blr x9",
            "  mov x0, #0", "  mov x8, #93", "  svc #0"]
    out += [".balign 2048", ".globl f_empty", "f_empty:", "  ret"]
    for name, body in shapes:
        out += [".balign 2048", f".globl f_{name}", f"f_{name}:"]
        out += ["  " + b for b in body]
        out += ["  ret"]
    out += [".data", ".balign 64", "buf: .space 8192"]
    open(path, "w").write("\n".join(out) + "\n")


def symbols(binary):
    txt = subprocess.run(["llvm-nm", binary], capture_output=True, text=True,
                         check=True).stdout
    syms = {}
    for line in txt.splitlines():
        m = re.match(r"^([0-9a-fA-F]+)\s+\S\s+(\S+)$", line.strip())
        if m:
            syms[m.group(2)] = int(m.group(1), 16)
    return syms


def measure(emu, binary, shapes, syms, env):
    with tempfile.NamedTemporaryFile(suffix=".hash", delete=False) as f:
        log = f.name
    e = dict(os.environ)
    e.update(env)
    e["POWERARM_CODEHASHLOG"] = log
    subprocess.run([emu, binary], env=e, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    sizes = {}
    for line in open(log):
        parts = line.split()
        if len(parts) >= 2:
            sizes[int(parts[0], 16)] = int(parts[1])
    os.unlink(log)
    base = sizes.get(syms["f_empty"])
    if base is None:
        raise SystemExit("f_empty block never compiled; CODEHASHLOG empty?")
    rows = {}
    for name, _ in shapes:
        addr = syms[f"f_{name}"]
        if addr not in sizes:
            rows[name] = None
            continue
        rows[name] = (sizes[addr] - base) / 4.0 / 16.0
    return rows


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    emus = sys.argv[1:]
    work = tempfile.mkdtemp(prefix="shapes-ldst-")
    asm = os.path.join(work, "shapes.S")
    binary = os.path.join(work, "shapes")
    shapes = build_shapes()
    emit_asm(shapes, asm)
    subprocess.run(["clang", "--target=aarch64-linux-gnu", "-nostdlib",
                    "-static", "-fuse-ld=lld", "-o", binary, asm], check=True)
    syms = symbols(binary)

    # Four configurations per emulator, as in shapes2-counts.txt: the VSX
    # register-class switch off and on, crossed with ISA 3.0 and the POWER8
    # fallbacks. `off` is the baseline every guest V16-V31 access pays today
    # (one xxlor out of the pinned low-bank register into a VMX one); `on` is
    # what a VSX-clean lowering gets.
    configs = [
        ("3.0/off", {"POWERARM_VSXCLASSES": "0"}),
        ("3.0/on", {"POWERARM_VSXCLASSES": "1"}),
        ("P8/off", {"POWERARM_VSXCLASSES": "0", "POWERARM_HOSTFEATURES": "disableisa30"}),
        ("P8/on", {"POWERARM_VSXCLASSES": "1", "POWERARM_HOSTFEATURES": "disableisa30"}),
    ]
    data = {}
    for emu in emus:
        for lvl, env in configs:
            data[(emu, lvl)] = measure(emu, binary, shapes, syms, env)

    names = sorted({n[:-3] for n, _ in shapes})
    print("# (block bytes - empty block bytes) / 4 / 16 = host instructions "
          "per guest instruction.")
    print("# lo = operands in V0-V15, hi = in V16-V31. off/on = "
          "POWERARM_VSXCLASSES=0/1.")
    for i, emu in enumerate(emus):
        print(f"# emu{i}: {emu}")
    hdr = "shape".ljust(14)
    for i in range(len(emus)):
        for lvl, _ in configs:
            hdr += f"|{('emu%d ' % i if len(emus) > 1 else '') + lvl:>8}: lo   hi"
    print(hdr)
    for base in names:
        row = base.ljust(14)
        for emu in emus:
            for lvl, _ in configs:
                d = data[(emu, lvl)]
                lo, hi = d.get(base + "_lo"), d.get(base + "_hi")
                row += "|{:>12} {:>4}".format(
                    "-" if lo is None else f"{lo:.2f}",
                    "-" if hi is None else f"{hi:.2f}")
        print(row)


if __name__ == "__main__":
    main()
