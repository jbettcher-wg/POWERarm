#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generates the SIMD and floating-point differential test programs.

Same method as gen.py: each case loads operands (V registers from literal
data with `ldr qN`, X registers with `ldr xN, =`), sets FPCR and NZCV, runs
one instruction or a short sequence, then calls `dump` (every GPR, SP,
NZCV) and `vdump` (V0-V31, FPCR, FPSR without the cumulative exception
bits). The Pi's output is the golden.

Operands mix statically allocated V registers (V0-V15) and context-backed
ones (V16-V31). Floating-point operands come from an edge corpus (signed
zeros, infinities, quiet and signalling NaNs with payloads, denormals,
integer conversion boundaries) plus random values; the bits of a V register
above the operand are random, so every upper-bit clear is checked.

Usage: gen_simd.py OUTDIR
"""

import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen import Program, SEED, CONDS  # noqa: E402

M64 = (1 << 64) - 1

F64_EDGE = [
    0x0000000000000000, 0x8000000000000000,                      # +-0
    0x7FF0000000000000, 0xFFF0000000000000,                      # +-Inf
    0x7FF8000000000000, 0xFFF8000000000000, 0x7FF8000000000123,  # quiet NaNs
    0x7FF0000000000001, 0x7FF4000000000ABC, 0xFFF0000000000042,  # signalling NaNs
    0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x8000000000000001,  # denormals
    0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF,  # min normal, +-max
    0x3FF0000000000000, 0xBFF0000000000000, 0x3FE0000000000000,  # 1, -1, 0.5
    0x3FF8000000000000, 0x4004000000000000, 0xC004000000000000,  # 1.5, 2.5, -2.5
    0x3FE8000000000000, 0xBFE0000000000000, 0x400C000000000000,  # 0.75, -0.5, 3.5
    0x41DFFFFFFFC00000, 0x41E0000000000000, 0xC1E0000000000000,  # 2^31-1, 2^31, -2^31
    0xC1E0000000200000, 0x41EFFFFFFFE00000, 0x41F0000000000000,  # -2^31-1, 2^32-1, 2^32
    0x43DFFFFFFFFFFFFF, 0x43E0000000000000, 0xC3E0000000000000,  # 2^63-1024, 2^63, -2^63
    0xC3E0000000000001, 0x43EFFFFFFFFFFFFF, 0x43F0000000000000,  # -2^63-2048, 2^64-2048, 2^64
    0x41DFFFFFFFE00000, 0x41E0000000100000,                      # 2^31-0.5, 2^31+0.5
]

F32_EDGE = [
    0x00000000, 0x80000000, 0x7F800000, 0xFF800000,
    0x7FC00000, 0xFFC00000, 0x7FC00123, 0x7F800001, 0x7FA00ABC, 0xFF800042,
    0x00000001, 0x007FFFFF, 0x00800000, 0x7F7FFFFF, 0xFF7FFFFF,
    0x3F800000, 0xBF800000, 0x3F000000, 0x3FC00000, 0x40200000, 0xC0200000,
    0x4EFFFFFF, 0x4F000000, 0xCF000000, 0xCF000001, 0x4F7FFFFF, 0x4F800000,
    0x5EFFFFFF, 0x5F000000, 0xDF000000, 0x5F7FFFFF, 0x5F800000,
]

NAN_PAIRS64 = [
    (0x7FF8000000000003, 0x7FF0000000000002), (0x7FF0000000000001, 0x7FF8000000000004),
    (0x7FF8000000000003, 0x7FF8000000000004), (0x7FF0000000000001, 0x7FF0000000000002),
    (0x3FF0000000000000, 0x7FF0000000000002), (0x7FF0000000000001, 0x3FF0000000000000),
    (0x3FF0000000000000, 0x7FF8000000000004), (0x7FF8000000000003, 0x3FF0000000000000),
    (0x0000000000000000, 0x8000000000000000), (0x8000000000000000, 0x0000000000000000),
    (0x7FF0000000000000, 0xFFF0000000000000), (0x0000000000000000, 0x7FF0000000000000),
]

NAN_PAIRS32 = [
    (0x7FC00003, 0x7F800002), (0x7F800001, 0x7FC00004), (0x7FC00003, 0x7FC00004),
    (0x7F800001, 0x7F800002), (0x3F800000, 0x7F800002), (0x7F800001, 0x3F800000),
    (0x3F800000, 0x7FC00004), (0x7FC00003, 0x3F800000), (0x00000000, 0x80000000),
    (0x80000000, 0x00000000), (0x7F800000, 0xFF800000), (0x00000000, 0x7F800000),
]

VEC_EDGE = [
    0, M64, 0x8080808080808080, 0x7F7F7F7F7F7F7F7F, 0x0101010101010101,
    0x8000800080008000, 0x7FFF7FFF7FFF7FFF, 0x8000000080000000, 0x7FFFFFFF7FFFFFFF,
    0x8000000000000000, 0x7FFFFFFFFFFFFFFF, 0x00FF00FF00FF00FF, 0x0123456789ABCDEF,
    0xFEDCBA9876543210, 0xFFFFFFFF00000000, 0x00000000FFFFFFFF,
]

# Registers: statically allocated V0-V15 and context-backed V16-V31.
VREGS = [0, 1, 2, 3, 5, 7, 12, 15, 16, 17, 20, 23, 26, 29, 30, 31]
XREGS = [0, 1, 2, 3, 4, 7, 9, 10, 19, 22, 25, 28]

# FPCR RMode values: nearest, +Inf, -Inf, zero.
RMODES = [0x000000, 0x400000, 0x800000, 0xC00000]


class VProgram(Program):
    def __init__(self, name, rng):
        super().__init__(name, rng)
        self.lits = []

    def half(self):
        r = self.rng.random()
        if r < 0.35:
            return self.rng.choice(VEC_EDGE)
        if r < 0.45:
            return self.rng.getrandbits(8) * 0x0101010101010101
        return self.rng.getrandbits(64)

    def vec(self):
        return (self.half(), self.half())

    def f64(self):
        r = self.rng.random()
        if r < 0.5:
            return self.rng.choice(F64_EDGE)
        if r < 0.75:
            v = self.rng.uniform(-1e6, 1e6) * self.rng.choice([1, 1e-300, 1e10, 1e-5, 1e300])
            return struct.unpack("<Q", struct.pack("<d", v))[0]
        return self.rng.getrandbits(64)

    def f32(self):
        r = self.rng.random()
        if r < 0.5:
            return self.rng.choice(F32_EDGE)
        if r < 0.75:
            v = self.rng.uniform(-1e4, 1e4) * self.rng.choice([1, 1e-40, 1e10, 1e-3, 1e30])
            return struct.unpack("<I", struct.pack("<f", max(min(v, 3e38), -3e38)))[0]
        return self.rng.getrandbits(32)

    def fp(self, is64):
        """A V register holding a scalar operand, with random bits above it."""
        if is64:
            return (self.f64(), self.rng.getrandbits(64))
        return ((self.rng.getrandbits(32) << 32) | self.f32(), self.rng.getrandbits(64))

    def vreg(self):
        return self.rng.choice(VREGS)

    def vregs(self, n):
        return self.rng.sample(VREGS, n)

    def xreg(self):
        return self.rng.choice(XREGS)

    def vcase(self, body, v=None, x=None, nzcv=None, fpcr=0, dumpbuf=False):
        self.cases += 1
        self.emit(f"        // case {self.cases}: {' ; '.join(body)}")
        labels = []
        for reg, (lo, hi) in (v or {}).items():
            label = f"vlit_{self.cases}_{reg}"
            labels.append((label, lo & M64, hi & M64))
            self.emit(f"        ldr     q{reg}, {label}")
        for reg, value in (x or {}).items():
            self.load(reg, value)
        self.emit(f"        mov     x17, #0x{fpcr:x}")
        self.emit("        msr     fpcr, x17")
        self.flags(nzcv)
        for line in body:
            self.emit(f"        {line}")
        self.emit("        bl      dump")
        self.emit("        bl      vdump")
        if dumpbuf:
            self.emit("        bl      dumpbuf")
        self.emit("        b       1f")
        self.emit("        .balign 8")
        for label, lo, hi in labels:
            self.emit(f"{label}: .quad 0x{lo:x}, 0x{hi:x}")
        self.emit("1:")
        if self.cases % 16 == 0:
            self.emit("        b       1f")
            self.emit("        .ltorg")
            self.emit("1:")


def vtypes(p, allow_d=True):
    """(width suffix, element bits, Q) for a random vector type."""
    choices = [("8b", 8, 0), ("16b", 8, 1), ("4h", 16, 0), ("8h", 16, 1), ("2s", 32, 0), ("4s", 32, 1)]
    if allow_d:
        choices.append(("2d", 64, 1))
    return p.rng.choice(choices)


def narrow_wide(p):
    """(narrow type, wide type, Q-2 narrow type) triples by element size."""
    return p.rng.choice([
        ("8b", "8h", "16b", 8), ("4h", "4s", "8h", 16), ("2s", "2d", "4s", 32),
    ])


# ---------------------------------------------------------------------------
# Loads and stores
# ---------------------------------------------------------------------------

def gen_simd_loadstore(p):
    p.emit("        adrp    x19, buf")
    p.emit("        add     x19, x19, :lo12:buf")
    p.emit("        add     x19, x19, #128")
    regs = [("b", 1), ("h", 2), ("s", 4), ("d", 8), ("q", 16)]
    for _ in range(360):
        rt, size = p.rng.choice(regs)
        t = p.vreg()
        v = {t: p.vec()}
        is_load = p.rng.random() < 0.5
        op = "ldr" if is_load else "str"
        kind = p.rng.randrange(6)
        if kind == 0:
            off = p.rng.randrange(0, 8) * size
            p.vcase([f"{op} {rt}{t}, [x19, #{off}]"], v, dumpbuf=not is_load)
        elif kind == 1:
            off = p.rng.randrange(-40, 40)
            p.vcase([f"{op.replace('ldr', 'ldur').replace('str', 'stur')} {rt}{t}, [x19, #{off}]"], v, dumpbuf=not is_load)
        elif kind == 2:
            off = p.rng.randrange(-40, 40)
            p.vcase(["mov x20, x19", f"{op} {rt}{t}, [x20, #{off}]!"], v, dumpbuf=not is_load)
        elif kind == 3:
            off = p.rng.randrange(-40, 40)
            p.vcase(["mov x20, x19", f"{op} {rt}{t}, [x20], #{off}"], v, dumpbuf=not is_load)
        elif kind == 4:
            shift = size.bit_length() - 1
            ext = p.rng.choice(["uxtw", "sxtw", "lsl", "sxtx"])
            amount = p.rng.choice([0, shift])
            if ext == "uxtw":
                idx = p.rng.choice([0, 1, 3, 0xABCD000000000002])
            elif ext == "sxtw":
                idx = p.rng.choice([0, 1, 0xFFFFFFFF, 0xFFFFFFFE, 0x12345678FFFFFFFC])
            else:
                idx = p.rng.choice([0, 1, 3, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFC])
            idxreg = "w10" if ext in ("uxtw", "sxtw") else "x10"
            if ext == "lsl" and amount == 0:
                p.vcase([f"{op} {rt}{t}, [x19, {idxreg}]"], v, {10: idx}, dumpbuf=not is_load)
            else:
                p.vcase([f"{op} {rt}{t}, [x19, {idxreg}, {ext} #{amount}]"], v, {10: idx}, dumpbuf=not is_load)
        else:
            if size == 1 or size == 2:
                rt, size = p.rng.choice([("s", 4), ("d", 8), ("q", 16)])
            u = p.rng.choice([r for r in VREGS if r != t])
            v[u] = p.vec()
            pair = "ldp" if is_load else "stp"
            off = p.rng.randrange(-3, 3) * size
            mode = p.rng.randrange(3)
            if mode == 0:
                p.vcase(["mov x20, x19", f"{pair} {rt}{t}, {rt}{u}, [x20, #{off}]"], v, dumpbuf=not is_load)
            elif mode == 1:
                p.vcase(["mov x20, x19", f"{pair} {rt}{t}, {rt}{u}, [x20, #{off}]!"], v, dumpbuf=not is_load)
            else:
                p.vcase(["mov x20, x19", f"{pair} {rt}{t}, {rt}{u}, [x20], #{off}"], v, dumpbuf=not is_load)
    # LD1/ST1 with one to four registers, D and Q, offset and post-index.
    for _ in range(60):
        count = p.rng.randrange(1, 5)
        first = p.rng.randrange(32)
        width = p.rng.choice(["8b", "16b", "4h", "8h", "2s", "4s", "1d", "2d"])
        regs_ = [(first + i) % 32 for i in range(count)]
        v = {r: p.vec() for r in regs_}
        lst = ", ".join(f"v{r}.{width}" for r in regs_)
        is_load = p.rng.random() < 0.5
        op = "ld1" if is_load else "st1"
        mode = p.rng.randrange(3)
        pre = ["mov x20, x19", "sub x20, x20, #64"]
        if mode == 0:
            p.vcase(pre + [f"{op} {{{lst}}}, [x20]"], v, dumpbuf=not is_load)
        elif mode == 1:
            nbytes = count * (16 if width in ("16b", "8h", "4s", "2d") else 8)
            p.vcase(pre + [f"{op} {{{lst}}}, [x20], #{nbytes}"], v, dumpbuf=not is_load)
        else:
            p.vcase(pre + [f"{op} {{{lst}}}, [x20], x10"], v, {10: p.rng.choice([0, 3, 0xFFFFFFFFFFFFFFF0])}, dumpbuf=not is_load)
    # Literals.
    p.vcase(["ldr s1, flit32", "ldr d17, flit64", "ldr q30, qlit"], {1: p.vec(), 17: p.vec(), 30: p.vec()})
    p.emit("        b       2f")
    p.emit("        .balign 4")
    p.emit("flit32: .word   0x80706050")
    p.emit("flit64: .quad   0x8877665544332211")
    p.emit("qlit:   .quad   0x0123456789abcdef, 0xfedcba9876543210")
    p.emit("2:")


# ---------------------------------------------------------------------------
# Copies and immediates
# ---------------------------------------------------------------------------

def gen_simd_copy(p):
    elems = [("b", 8, 16), ("h", 16, 8), ("s", 32, 4), ("d", 64, 2)]
    for _ in range(420):
        d, n = p.vreg(), p.vreg()
        v = {d: p.vec(), n: p.vec()}
        x = p.xreg()
        xv = {x: p.rng.choice([p.rng.getrandbits(64), 0x80, 0xFFFFFFFFFFFFFF7F, 0x8000, 0x80000000])}
        kind = p.rng.randrange(7)
        e, bits, count = p.rng.choice(elems)
        idx = p.rng.randrange(count)
        if kind == 0:
            q = p.rng.random() < 0.5 or e == "d"
            vt = {"b": "16b" if q else "8b", "h": "8h" if q else "4h", "s": "4s" if q else "2s", "d": "2d"}[e]
            src = f"x{x}" if e == "d" else f"w{x}"
            p.vcase([f"dup v{d}.{vt}, {src}"], v, xv)
        elif kind == 1:
            q = p.rng.random() < 0.5 or e == "d"
            vt = {"b": "16b" if q else "8b", "h": "8h" if q else "4h", "s": "4s" if q else "2s", "d": "2d"}[e]
            p.vcase([f"dup v{d}.{vt}, v{n}.{e}[{idx}]"], v)
        elif kind == 2:
            src = f"x{x}" if e == "d" else f"w{x}"
            p.vcase([f"ins v{d}.{e}[{idx}], {src}"], v, xv)
        elif kind == 3:
            idx2 = p.rng.randrange(count)
            p.vcase([f"ins v{d}.{e}[{idx}], v{n}.{e}[{idx2}]"], v)
        elif kind == 4:
            if e == "d":
                p.vcase([f"umov x{x}, v{n}.d[{idx}]", f"mov d{d}, v{n}.d[{idx}]"], v, xv)
            else:
                p.vcase([f"umov w{x}, v{n}.{e}[{idx}]"], v, xv)
        elif kind == 5:
            if e == "d":
                e, bits, count, idx = "s", 32, 4, idx
            if e == "s":
                p.vcase([f"smov x{x}, v{n}.s[{idx}]"], v, xv)
            else:
                reg = p.rng.choice(["w", "x"])
                p.vcase([f"smov {reg}{x}, v{n}.{e}[{idx}]"], v, xv)
        else:
            imm8 = p.rng.choice([0, 0x80, 0xFF, 0x7F, 0x01, p.rng.getrandbits(8)])
            q = p.rng.random() < 0.5
            form = p.rng.randrange(9)
            if form == 0:
                p.vcase([f"movi v{d}.{'16b' if q else '8b'}, #0x{imm8:x}"], v)
            elif form == 1:
                sh = p.rng.choice([0, 8])
                op = p.rng.choice(["movi", "mvni", "orr", "bic"])
                p.vcase([f"{op} v{d}.{'8h' if q else '4h'}, #0x{imm8:x}, lsl #{sh}"], v)
            elif form == 2:
                sh = p.rng.choice([0, 8, 16, 24])
                op = p.rng.choice(["movi", "mvni", "orr", "bic"])
                p.vcase([f"{op} v{d}.{'4s' if q else '2s'}, #0x{imm8:x}, lsl #{sh}"], v)
            elif form == 3:
                sh = p.rng.choice([8, 16])
                op = p.rng.choice(["movi", "mvni"])
                p.vcase([f"{op} v{d}.{'4s' if q else '2s'}, #0x{imm8:x}, msl #{sh}"], v)
            elif form == 4:
                mask = 0
                for i in range(8):
                    if (imm8 >> i) & 1:
                        mask |= 0xFF << (8 * i)
                if q:
                    p.vcase([f"movi v{d}.2d, #0x{mask:x}"], v)
                else:
                    p.vcase([f"movi d{d}, #0x{mask:x}"], v)
            elif form == 5:
                fv = p.rng.choice(["1.5", "-0.25", "2.0", "0.125", "-16.0", "31.0"])
                p.vcase([f"fmov v{d}.{'4s' if q else '2s'}, #{fv}"], v)
            elif form == 6:
                fv = p.rng.choice(["1.5", "-0.25", "2.0", "0.125", "-16.0", "31.0"])
                p.vcase([f"fmov v{d}.2d, #{fv}"], v)
            else:
                p.vcase([f"movi v{d}.16b, #0", f"movi v{n}.2d, #0"], v)


# ---------------------------------------------------------------------------
# Three same, two-register misc, across lanes
# ---------------------------------------------------------------------------

def gen_simd_arith(p):
    three = ["add", "sub", "cmeq", "cmgt", "cmge", "cmhi", "cmhs", "cmtst", "umax", "umin", "smax", "smin",
             "addp", "umaxp", "uminp"]
    for _ in range(700):
        d, n, m = p.vreg(), p.vreg(), p.vreg()
        v = {d: p.vec(), n: p.vec(), m: p.vec()}
        kind = p.rng.randrange(8)
        if kind <= 2:
            op = p.rng.choice(three)
            allow_d = op not in ("umax", "umin", "smax", "smin", "umaxp", "uminp")
            vt, bits, q = vtypes(p, allow_d)
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, v{m}.{vt}"], v)
        elif kind == 3:
            op = p.rng.choice(["and", "bic", "orr", "orn", "eor", "bsl", "bit", "bif"])
            vt = p.rng.choice(["8b", "16b"])
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, v{m}.{vt}"], v)
        elif kind == 4:
            op = p.rng.choice(["add", "sub", "cmeq", "cmgt", "cmge", "cmhi", "cmhs", "cmtst"])
            p.vcase([f"{op} d{d}, d{n}, d{m}"], v)
        elif kind == 5:
            op = p.rng.choice(["cmeq", "cmge", "cmgt", "cmle", "cmlt"])
            if p.rng.random() < 0.3:
                p.vcase([f"{op} d{d}, d{n}, #0"], v)
            else:
                vt, bits, q = vtypes(p)
                p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, #0"], v)
        elif kind == 6:
            op = p.rng.choice(["cnt", "mvn", "neg", "abs", "rev64", "rev32"])
            if op in ("cnt", "mvn"):
                vt = p.rng.choice(["8b", "16b"])
            elif op == "rev64":
                vt = p.rng.choice(["8b", "16b", "4h", "8h", "2s", "4s"])
            elif op == "rev32":
                vt = p.rng.choice(["8b", "16b", "4h", "8h"])
            else:
                vt, bits, q = vtypes(p)
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}"], v)
        else:
            op = p.rng.choice(["addv", "umaxv", "uminv"])
            reg, vt = p.rng.choice([("b", "8b"), ("b", "16b"), ("h", "4h"), ("h", "8h"), ("s", "4s")])
            p.vcase([f"{op} {reg}{d}, v{n}.{vt}"], v)
    # Edge vectors: signed/unsigned boundaries in every lane.
    for a in VEC_EDGE[:8]:
        for b in VEC_EDGE[4:12]:
            op = p.rng.choice(["cmhi", "cmhs", "cmgt", "cmge", "umaxp", "addp", "umax", "smin"])
            vt = p.rng.choice(["16b", "8h", "4s"])
            p.vcase([f"{op} v1.{vt}, v2.{vt}, v18.{vt}"], {1: p.vec(), 2: (a, b), 18: (b, a)})


# ---------------------------------------------------------------------------
# Widening, narrowing, shifts, extract, permutes
# ---------------------------------------------------------------------------

def gen_simd_shape(p):
    for _ in range(700):
        d, n, m = p.vreg(), p.vreg(), p.vreg()
        v = {d: p.vec(), n: p.vec(), m: p.vec()}
        kind = p.rng.randrange(8)
        nt, wt, nt2, bits = narrow_wide(p)
        if kind == 0:
            op = p.rng.choice(["xtn", "xtn2"])
            p.vcase([f"{op} v{d}.{nt2 if op == 'xtn2' else nt}, v{n}.{wt}"], v)
        elif kind == 1:
            op = p.rng.choice(["shrn", "shrn2"])
            sh = p.rng.choice([1, bits // 2, bits, p.rng.randrange(1, bits + 1)])
            p.vcase([f"{op} v{d}.{nt2 if op == 'shrn2' else nt}, v{n}.{wt}, #{sh}"], v)
        elif kind == 2:
            op = p.rng.choice(["saddl", "uaddl", "ssubl", "usubl"])
            if p.rng.random() < 0.5:
                p.vcase([f"{op} v{d}.{wt}, v{n}.{nt}, v{m}.{nt}"], v)
            else:
                p.vcase([f"{op}2 v{d}.{wt}, v{n}.{nt2}, v{m}.{nt2}"], v)
        elif kind == 3:
            op = p.rng.choice(["saddw", "uaddw", "addhn", "subhn"])
            two = p.rng.random() < 0.5
            if op in ("saddw", "uaddw"):
                p.vcase([f"{op}{'2' if two else ''} v{d}.{wt}, v{n}.{wt}, v{m}.{nt2 if two else nt}"], v)
            else:
                p.vcase([f"{op}{'2' if two else ''} v{d}.{nt2 if two else nt}, v{n}.{wt}, v{m}.{wt}"], v)
        elif kind == 4:
            op = p.rng.choice(["sshll", "ushll", "sxtl", "uxtl"])
            two = p.rng.random() < 0.5
            src = nt2 if two else nt
            if op in ("sxtl", "uxtl"):
                p.vcase([f"{op}{'2' if two else ''} v{d}.{wt}, v{n}.{src}"], v)
            else:
                sh = p.rng.randrange(0, bits)
                p.vcase([f"{op}{'2' if two else ''} v{d}.{wt}, v{n}.{src}, #{sh}"], v)
        elif kind == 5:
            op = p.rng.choice(["sshr", "ushr", "shl"])
            if p.rng.random() < 0.3:
                sh = p.rng.choice([1, 63, 64] if op != "shl" else [0, 1, 63])
                p.vcase([f"{op} d{d}, d{n}, #{sh}"], v)
            else:
                vt, ebits, q = vtypes(p)
                if op == "shl":
                    sh = p.rng.choice([0, 1, ebits - 1, p.rng.randrange(ebits)])
                else:
                    sh = p.rng.choice([1, ebits, ebits - 1, p.rng.randrange(1, ebits + 1)])
                p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, #{sh}"], v)
        elif kind == 6:
            if p.rng.random() < 0.5:
                p.vcase([f"ext v{d}.16b, v{n}.16b, v{m}.16b, #{p.rng.randrange(16)}"], v)
            else:
                p.vcase([f"ext v{d}.8b, v{n}.8b, v{m}.8b, #{p.rng.randrange(8)}"], v)
        else:
            op = p.rng.choice(["uzp1", "uzp2", "zip1", "zip2", "trn1", "trn2"])
            vt, ebits, q = vtypes(p)
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, v{m}.{vt}"], v)


# ---------------------------------------------------------------------------
# Scalar floating point
# ---------------------------------------------------------------------------

def fp_pair(p, is64):
    if p.rng.random() < 0.25:
        a, b = p.rng.choice(NAN_PAIRS64 if is64 else NAN_PAIRS32)
        hi1, hi2 = p.rng.getrandbits(64), p.rng.getrandbits(64)
        if is64:
            return (a, hi1), (b, hi2)
        return ((p.rng.getrandbits(32) << 32) | a, hi1), ((p.rng.getrandbits(32) << 32) | b, hi2)
    return p.fp(is64), p.fp(is64)


def gen_fp_scalar(p):
    for _ in range(900):
        is64 = p.rng.random() < 0.5
        r = "d" if is64 else "s"
        d, n, m, a = p.vreg(), p.vreg(), p.vreg(), p.vreg()
        vn, vm = fp_pair(p, is64)
        v = {d: p.vec(), a: p.fp(is64), n: vn, m: vm}
        fpcr = p.rng.choice(RMODES)
        nzcv = p.rng.randrange(16)
        x = p.xreg()
        kind = p.rng.randrange(10)
        if kind == 0:
            op = p.rng.choice(["fadd", "fsub", "fmul", "fdiv", "fnmul"])
            p.vcase([f"{op} {r}{d}, {r}{n}, {r}{m}"], v, fpcr=fpcr)
        elif kind == 1:
            op = p.rng.choice(["fmin", "fmax", "fminnm", "fmaxnm"])
            p.vcase([f"{op} {r}{d}, {r}{n}, {r}{m}"], v, fpcr=fpcr)
        elif kind == 2:
            op = p.rng.choice(["fabs", "fneg", "fsqrt", "fmov"])
            p.vcase([f"{op} {r}{d}, {r}{n}"], v, fpcr=fpcr)
        elif kind == 3:
            op = p.rng.choice(["fmadd", "fmsub", "fnmadd", "fnmsub"])
            p.vcase([f"{op} {r}{d}, {r}{n}, {r}{m}, {r}{a}"], v, fpcr=fpcr)
        elif kind == 4:
            op = p.rng.choice(["fcmp", "fcmpe"])
            if p.rng.random() < 0.3:
                p.vcase([f"{op} {r}{n}, #0.0"], v, nzcv=nzcv)
            else:
                p.vcase([f"{op} {r}{n}, {r}{m}"], v, nzcv=nzcv)
        elif kind == 5:
            op = p.rng.choice(["fccmp", "fccmpe"])
            cond = p.rng.choice(CONDS)
            p.vcase([f"{op} {r}{n}, {r}{m}, #{p.rng.randrange(16)}, {cond}"], v, nzcv=nzcv)
        elif kind == 6:
            cond = p.rng.choice(CONDS)
            p.vcase([f"fcsel {r}{d}, {r}{n}, {r}{m}, {cond}"], v, nzcv=nzcv)
        elif kind == 7:
            p.vcase([f"fcvt {'s' if is64 else 'd'}{d}, {r}{n}"], v, fpcr=fpcr)
        elif kind == 8:
            imm = p.rng.choice(["1.5", "-0.25", "2.0", "0.125", "-16.0", "31.0", "0.1875", "-7.75"])
            p.vcase([f"fmov {r}{d}, #{imm}"], v)
        else:
            form = p.rng.randrange(3)
            gx = {x: p.rng.getrandbits(64)}
            if form == 0:
                p.vcase([f"fmov {'x' if is64 else 'w'}{x}, {r}{n}"], v, gx)
            elif form == 1:
                p.vcase([f"fmov {r}{d}, {'x' if is64 else 'w'}{x}"], v, gx)
            else:
                p.vcase([f"fmov x{x}, v{n}.d[1]", f"fmov v{d}.d[1], x{x}"], v, gx)
    # Flags from every compare outcome feed every condition.
    for a, b in [(0x3FF0000000000000, 0x4000000000000000), (0x4000000000000000, 0x3FF0000000000000),
                 (0x3FF0000000000000, 0x3FF0000000000000), (0x7FF8000000000000, 0x3FF0000000000000),
                 (0x8000000000000000, 0x0000000000000000)]:
        for cond in CONDS:
            setcond = cond if cond not in ("al", "nv") else "eq"
            p.vcase(["fcmp d1, d17", "mov x5, #1", f"b.{cond} 2f", "mov x5, #2", "2:"],
                    {1: (a, 0), 17: (b, 0)}, nzcv=p.rng.randrange(16))
            p.vcase(["fcmp d1, d17", f"cset w6, {setcond}", f"fcsel d2, d1, d17, {cond}"],
                    {1: (a, 0), 17: (b, 0), 2: p.vec()}, nzcv=p.rng.randrange(16))


def gen_fp_convert(p):
    to_int = ["fcvtzs", "fcvtzu", "fcvtns", "fcvtnu", "fcvtps", "fcvtpu", "fcvtms", "fcvtmu", "fcvtas", "fcvtau"]
    for _ in range(900):
        is64 = p.rng.random() < 0.5
        r = "d" if is64 else "s"
        d, n = p.vreg(), p.vreg()
        x = p.xreg()
        v = {d: p.vec(), n: p.fp(is64)}
        fpcr = p.rng.choice(RMODES)
        g = p.rng.choice(["w", "x"])
        kind = p.rng.randrange(6)
        if kind <= 1:
            p.vcase([f"{p.rng.choice(to_int)} {g}{x}, {r}{n}"], v, {x: p.rng.getrandbits(64)}, fpcr=fpcr)
        elif kind == 2:
            op = p.rng.choice(["scvtf", "ucvtf"])
            ival = p.rng.choice([0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000,
                                 M64, 0x20000000000001, 0x1000001, 0xFFFFFFFFFFFFFC01, p.rng.getrandbits(64),
                                 p.rng.getrandbits(32), 0x7FFFFFBF, 0xFFFFFF7F])
            p.vcase([f"{op} {r}{d}, {g}{x}"], v, {x: ival}, fpcr=fpcr)
        elif kind == 3:
            op = p.rng.choice(["scvtf", "ucvtf"])
            fbits = p.rng.choice([1, 8, 31, 32] + ([33, 63, 64] if g == "x" else []))
            p.vcase([f"{op} {r}{d}, {g}{x}, #{fbits}"], v, {x: p.rng.getrandbits(64)}, fpcr=fpcr)
        elif kind == 4:
            op = p.rng.choice(["fcvtzs", "fcvtzu"])
            fbits = p.rng.choice([1, 8, 31, 32] + ([33, 63, 64] if g == "x" else []))
            p.vcase([f"{op} {g}{x}, {r}{n}, #{fbits}"], v, {x: p.rng.getrandbits(64)}, fpcr=fpcr)
        else:
            op = p.rng.choice(["fcvtzs", "fcvtzu", "scvtf", "ucvtf"])
            if op in ("scvtf", "ucvtf"):
                ival = p.rng.choice([0, 1, M64, 0x8000000000000000, 0x7FFFFFFFFFFFFFFF, p.rng.getrandbits(64)])
                v[n] = (ival, p.rng.getrandbits(64))
            p.vcase([f"{op} {r}{d}, {r}{n}"], v, fpcr=fpcr)
    # Every edge value through every rounding mode and conversion width.
    for is64, edges in ((True, F64_EDGE), (False, F32_EDGE)):
        r = "d" if is64 else "s"
        for val in edges:
            for op in ("fcvtns", "fcvtzu", "fcvtas", "fcvtps", "fcvtms"):
                g = p.rng.choice(["w", "x"])
                p.vcase([f"{op} {g}3, {r}20", f"fcvtzs {'x' if g == 'w' else 'w'}4, {r}20"],
                        {20: (val, p.rng.getrandbits(64))}, fpcr=p.rng.choice(RMODES))


def gen_fp_fpcr(p):
    # FPCR round trip, and the rounding mode reaching arithmetic and conversions.
    values = [0x0, 0x400000, 0x800000, 0xC00000, 0x3000000, 0x7C00000, 0xFFFFFFFF]
    for val in values:
        p.vcase(["mrs x4, fpcr", f"ldr x3, =0x{val:x}", "msr fpcr, x3", "mrs x5, fpcr", "msr fpcr, x4"], {})
    third = (0x3FD5555555555555, 0)  # 1/3
    tie = (0x4338000000000001, 0)    # 2^52 * 1.5 + 1: halfway cases after scaling
    for rm in RMODES:
        p.vcase(["fdiv d1, d2, d3", "fdiv s4, s5, s6", "fsqrt d7, d8", "fcvt s9, d10", "scvtf s11, x12",
                 "scvtf d13, x14", "fadd d15, d16, d17", "fmul s18, s19, s20"],
                {2: (0x3FF0000000000000, 0), 3: (0x4008000000000000, 0), 5: (0x3F800000, 0), 6: (0x41300000, 0),
                 8: (0x4000000000000000, 0), 10: third, 16: tie, 17: (0x3FE0000000000000, 0),
                 19: (0x3EAAAAAB, 0), 20: (0x40E00000, 0)},
                {12: 0x7FFFFFBF, 14: 0x20000000000003}, fpcr=rm)


F16_EDGE = [
    0x0000, 0x8000, 0x7C00, 0xFC00, 0x7E00, 0xFE00, 0x7E01, 0x7C01, 0x7D55, 0xFC2A,
    0x0001, 0x8001, 0x03FF, 0x0400, 0x7BFF, 0xFBFF, 0x3C00, 0xBC00, 0x3E00, 0x4100,
    0xC100, 0x3BFF, 0x0200, 0x7A00, 0x4000, 0x3800,
]


def gen_fp_half(p):
    def f16(): 
        if p.rng.random() < 0.6:
            return p.rng.choice(F16_EDGE)
        return p.rng.getrandbits(16)

    def hreg():
        return ((p.rng.getrandbits(48) << 16) | f16(), p.rng.getrandbits(64))

    # FPCR: every rounding mode, with and without FZ16.
    fpcrs = [rm | fz for rm in RMODES for fz in (0, 0x80000)]
    to_int = ["fcvtzs", "fcvtzu", "fcvtns", "fcvtnu", "fcvtps", "fcvtpu", "fcvtms", "fcvtmu", "fcvtas", "fcvtau"]
    for _ in range(900):
        d, n, m, a = p.vreg(), p.vreg(), p.vreg(), p.vreg()
        x = p.xreg()
        v = {d: p.vec(), n: hreg(), m: hreg(), a: hreg()}
        fpcr = p.rng.choice(fpcrs)
        nzcv = p.rng.randrange(16)
        kind = p.rng.randrange(12)
        if kind == 0:
            op = p.rng.choice(["fadd", "fsub", "fmul", "fdiv", "fnmul", "fmin", "fmax", "fminnm", "fmaxnm"])
            p.vcase([f"{op} h{d}, h{n}, h{m}"], v, fpcr=fpcr)
        elif kind == 1:
            op = p.rng.choice(["fabs", "fneg", "fsqrt", "fmov"])
            p.vcase([f"{op} h{d}, h{n}"], v, fpcr=fpcr)
        elif kind == 2:
            op = p.rng.choice(["fmadd", "fmsub", "fnmadd", "fnmsub"])
            p.vcase([f"{op} h{d}, h{n}, h{m}, h{a}"], v, fpcr=fpcr)
        elif kind == 3:
            op = p.rng.choice(["fcmp", "fcmpe"])
            if p.rng.random() < 0.3:
                p.vcase([f"{op} h{n}, #0.0"], v, nzcv=nzcv, fpcr=fpcr)
            else:
                p.vcase([f"{op} h{n}, h{m}"], v, nzcv=nzcv, fpcr=fpcr)
        elif kind == 4:
            cond = p.rng.choice(CONDS)
            if p.rng.random() < 0.5:
                p.vcase([f"fccmp h{n}, h{m}, #{p.rng.randrange(16)}, {cond}"], v, nzcv=nzcv, fpcr=fpcr)
            else:
                p.vcase([f"fcsel h{d}, h{n}, h{m}, {cond}"], v, nzcv=nzcv, fpcr=fpcr)
        elif kind == 5:
            src = p.rng.choice(["s", "d"])
            v[n] = p.fp(src == "d")
            p.vcase([f"fcvt h{d}, {src}{n}"], v, fpcr=fpcr)
        elif kind == 6:
            dst = p.rng.choice(["s", "d"])
            p.vcase([f"fcvt {dst}{d}, h{n}"], v, fpcr=fpcr)
        elif kind == 7:
            g = p.rng.choice(["w", "x"])
            p.vcase([f"{p.rng.choice(to_int)} {g}{x}, h{n}"], v, {x: p.rng.getrandbits(64)}, fpcr=fpcr)
        elif kind == 8:
            g = p.rng.choice(["w", "x"])
            op = p.rng.choice(["scvtf", "ucvtf"])
            ival = p.rng.choice([0, 1, 65504, 65519, 65520, 0xFFFFFFFF, M64, 0x8000000000000000, 3, 0x7FFF, p.rng.getrandbits(17),
                                 p.rng.getrandbits(64)])
            if p.rng.random() < 0.5:
                p.vcase([f"{op} h{d}, {g}{x}"], v, {x: ival}, fpcr=fpcr)
            else:
                fbits = p.rng.choice([1, 8, 16, 31, 32] + ([33, 50, 64] if g == "x" else []))
                p.vcase([f"{op} h{d}, {g}{x}, #{fbits}"], v, {x: ival}, fpcr=fpcr)
        elif kind == 9:
            g = p.rng.choice(["w", "x"])
            fbits = p.rng.choice([1, 8, 16, 31, 32] + ([33, 63] if g == "x" else []))
            p.vcase([f"{p.rng.choice(['fcvtzs', 'fcvtzu'])} {g}{x}, h{n}, #{fbits}"], v, {x: p.rng.getrandbits(64)}, fpcr=fpcr)
        elif kind == 10:
            form = p.rng.randrange(3)
            g = p.rng.choice(["w", "x"])
            if form == 0:
                imm = p.rng.choice(["1.5", "-0.25", "2.0", "0.125", "-16.0", "31.0", "0.1875", "-7.75"])
                p.vcase([f"fmov h{d}, #{imm}"], v)
            elif form == 1:
                p.vcase([f"fmov {g}{x}, h{n}"], v, {x: p.rng.getrandbits(64)})
            else:
                p.vcase([f"fmov h{d}, {g}{x}"], v, {x: p.rng.getrandbits(64)})
        else:
            form = p.rng.randrange(4)
            half_vec = (sum(f16() << (16 * i) for i in range(4)), sum(f16() << (16 * i) for i in range(4)))
            single_vec = tuple(sum(p.f32() << (32 * i) for i in range(2)) for _ in range(2))
            double_vec = (p.f64(), p.f64())
            if form == 0:
                v[n] = half_vec
                p.vcase([p.rng.choice([f"fcvtl v{d}.4s, v{n}.4h", f"fcvtl2 v{d}.4s, v{n}.8h"])], v, fpcr=fpcr)
            elif form == 1:
                v[n] = single_vec
                p.vcase([p.rng.choice([f"fcvtn v{d}.4h, v{n}.4s", f"fcvtn2 v{d}.8h, v{n}.4s"])], v, fpcr=fpcr)
            elif form == 2:
                v[n] = single_vec
                p.vcase([p.rng.choice([f"fcvtl v{d}.2d, v{n}.2s", f"fcvtl2 v{d}.2d, v{n}.4s"])], v, fpcr=fpcr)
            else:
                v[n] = double_vec
                p.vcase([p.rng.choice([f"fcvtn v{d}.2s, v{n}.2d", f"fcvtn2 v{d}.4s, v{n}.2d"])], v, fpcr=fpcr)
    # Every half edge value through every conversion and rounding mode.
    for val in F16_EDGE:
        for fpcr in fpcrs:
            p.vcase(["fcvt d1, h20", "fcvt s2, h20", "fcvtns w3, h20", "fcvtau x4, h20", "fadd h5, h20, h21", "fmul h6, h20, h21"],
                    {20: (val, 0), 21: ((p.rng.choice(F16_EDGE)), 0)}, fpcr=fpcr)
    # Double -> half rounding and overflow boundaries.
    for bits in [0x40EFFC0000000000, 0x40EFFD0000000000, 0x40EFFE0000000000, 0x40EFFF0000000000, 0x40F0000000000000,
                 0x3F10000000000000, 0x3F0FFFFFFFFFFFFF, 0x3EF0000000000000, 0x3E70000000000000, 0x3E60000000000000,
                 0x3E68000000000000, 0x3E78000000000000, 0x3FF0080000000000, 0x3FF0180000000000, 0xC0EFFE0000000000,
                 0x7FF0000000000001, 0x7FF4000000000000, 0x800FFFFFFFFFFFFF, 0x0000000000000001, 0x7FEFFFFFFFFFFFFF]:
        for fpcr in fpcrs:
            p.vcase(["fcvt h1, d20", "fcvt h2, s21"], {20: (bits, 0), 21: (0x7F7FFFFF if bits == 0x7FEFFFFFFFFFFFFF else 0x3FFFFFFF, 0)},
                    fpcr=fpcr)


def gen_exclusive(p):
    # Exclusive monitor and atomic-width loads/stores. X19 points at buf+128.
    p.emit("        adrp    x19, buf")
    p.emit("        add     x19, x19, :lo12:buf")
    p.emit("        add     x19, x19, #128")
    pairs = [("ldxr", "stxr"), ("ldaxr", "stlxr")]
    for _ in range(80):
        ld, st = p.rng.choice(pairs)
        size = p.rng.choice([("x", ""), ("w", ""), ("w", "b"), ("w", "h")])
        r, suffix = size
        off = p.rng.randrange(0, 12) * 8
        val = p.value()
        kind = p.rng.randrange(5)
        pre = [f"add x20, x19, #{off}"]
        if kind == 0:
            # Load, modify, store: succeeds.
            p.case(pre + [f"{ld}{suffix} {r}1, [x20]", f"add {r}1, {r}1, #1", f"{st}{suffix} w2, {r}3, [x20]"], {3: val}, dumpbuf=True)
        elif kind == 1:
            # Store without a load: fails, memory unchanged.
            p.case(pre + ["msr nzcv, x17", f"{st}{suffix} w2, {r}3, [x20]"], {3: val, 2: 0x55}, dumpbuf=True)
        elif kind == 2:
            # A store to another address after a load is IMPLEMENTATION
            # DEFINED (the Pi succeeds within the same region), so it is not
            # tested; a load of a different size before the store is used
            # instead, and must leave the store to the loaded address working.
            p.case(pre + [f"ldxrb w6, [x20]", f"{ld}{suffix} {r}1, [x20]", f"{st}{suffix} w2, {r}3, [x20]"], {3: val}, dumpbuf=True)
        elif kind == 3:
            # Two stores after one load: the second fails.
            p.case(pre + [f"{ld}{suffix} {r}1, [x20]", f"{st}{suffix} w2, {r}3, [x20]", f"{st}{suffix} w4, {r}5, [x20]"],
                   {3: val, 5: p.value()}, dumpbuf=True)
        else:
            # clrex between load and store: fails.
            p.case(pre + [f"{ld}{suffix} {r}1, [x20]", "clrex", f"{st}{suffix} w2, {r}3, [x20]"], {3: val}, dumpbuf=True)
    # NZCV survives a successful store.
    for nzcv in range(16):
        p.case(["mov x20, x19", "ldxr x1, [x20]", "stxr w2, x3, [x20]"], {3: 0x1122334455667788}, nzcv, dumpbuf=True)
    for _ in range(40):
        r, suffix = p.rng.choice([("x", ""), ("w", ""), ("w", "b"), ("w", "h")])
        off = p.rng.randrange(0, 12) * 8
        op = p.rng.choice(["ldar", "stlr"])
        p.case([f"add x20, x19, #{off}", f"{op}{suffix} {r}1, [x20]"], {1: p.value()}, dumpbuf=op == "stlr")


def gen_fp_round(p):
    """FRINT{N,P,M,Z,A,X,I} on half, single and double, across rounding modes."""
    ops = ["frintn", "frintp", "frintm", "frintz", "frinta", "frintx", "frinti"]
    ties64 = [0x3FDFFFFFFFFFFFFF, 0xBFDFFFFFFFFFFFFF, 0x4330000000000000, 0x432FFFFFFFFFFFFF,  # 0.49999999999999994, 2^52, 2^52-0.5
              0xC32FFFFFFFFFFFFF, 0x4012000000000000, 0xC012000000000000, 0x3FB999999999999A,  # 4.5, -4.5, 0.1
              0xBFD3333333333333, 0x4341C37937E08000, 0x3FF0000000000001, 0xC00C000000000000]  # -0.3, 1e16, 1+ulp, -3.5
    ties32 = [0x3EFFFFFF, 0xBEFFFFFF, 0x4B000000, 0x4AFFFFFF, 0xCAFFFFFF, 0x41100000, 0xC1100000,
              0x3DCCCCCD, 0xBE99999A, 0xC0600000, 0x3F800001]
    ties16 = [0x3800, 0xB800, 0x3E00, 0xBE00, 0x4100, 0xC120, 0x4480, 0x3BFF, 0xB7FF, 0x6000, 0x0001]
    for rm in RMODES:
        for op in ops:
            for val in F64_EDGE + ties64:
                p.vcase([f"{op} d{p.vreg()}, d20"], {20: (val, p.rng.getrandbits(64))}, fpcr=rm)
            for val in F32_EDGE + ties32:
                p.vcase([f"{op} s{p.vreg()}, s21"], {21: ((p.rng.getrandbits(32) << 32) | val, p.rng.getrandbits(64))}, fpcr=rm)
            for val in F16_EDGE + ties16:
                p.vcase([f"{op} h{p.vreg()}, h22"], {22: ((p.rng.getrandbits(48) << 16) | val, p.rng.getrandbits(64))}, fpcr=rm)
    for _ in range(300):
        is64 = p.rng.random() < 0.5
        r = "d" if is64 else "s"
        n = p.vreg()
        p.vcase([f"{p.rng.choice(ops)} {r}{p.vreg()}, {r}{n}"], {n: p.fp(is64)}, fpcr=p.rng.choice(RMODES))


def reg_list(first, count, width):
    return ", ".join(f"v{(first + i) % 32}.{width}" for i in range(count))


def gen_simd_table(p):
    # TBL/TBX with 1-4 tables. Every index value 0..255 appears in every
    # table count, table registers wrap past V31, and Q=0 checks that the
    # upper half is cleared.
    for tables in (1, 2, 3, 4):
        for op in ("tbl", "tbx"):
            values = list(range(256))
            p.rng.shuffle(values)
            for start in range(0, 256, 16):
                idx = values[start:start + 16]
                lo = sum(b << (8 * i) for i, b in enumerate(idx[:8]))
                hi = sum(b << (8 * i) for i, b in enumerate(idx[8:]))
                first = p.rng.choice([1, 14, 29, 30, 31])
                d = p.rng.choice([r for r in VREGS if all(r != (first + i) % 32 for i in range(tables))])
                m = p.rng.choice([r for r in VREGS if r != d])
                v = {(first + i) % 32: p.vec() for i in range(tables)}
                v[d] = p.vec()
                v[m] = (lo, hi)
                q = p.rng.random() < 0.5
                w = "16b" if q else "8b"
                p.vcase([f"{op} v{d}.{w}, {{{reg_list(first, tables, '16b')}}}, v{m}.{w}"], v)
    # Index register aliasing a table register and the destination.
    for tables in (1, 2, 3, 4):
        p.vcase([f"tbx v1.16b, {{{reg_list(0, tables, '16b')}}}, v1.16b"], {i: p.vec() for i in range(tables)} | {1: (0x0F1E2D3C4B5A6978, 0x8796A5B4C3D2E1F0)})
        p.vcase([f"tbl v0.16b, {{{reg_list(0, tables, '16b')}}}, v0.16b"], {i: p.vec() for i in range(tables)} | {0: (0x0706050403020100, 0x3F2F1F0F302010FF)})


def gen_simd_struct(p):
    # Multiple and single structures at every width and lane, loads and
    # stores, offset and post-index (immediate and register), registers
    # wrapping past V31. X19 = buf+64 leaves room for 64 bytes either side.
    p.emit("        adrp    x19, buf")
    p.emit("        add     x19, x19, :lo12:buf")
    p.emit("        add     x19, x19, #64")
    widths_q = [("16b", 1), ("8h", 2), ("4s", 4), ("2d", 8)]
    widths_d = [("8b", 1), ("4h", 2), ("2s", 4)]
    for _ in range(260):
        n = p.rng.randrange(1, 5)
        interleave = p.rng.random() < 0.7
        q = p.rng.random() < 0.6
        width, eb = p.rng.choice(widths_q if q else widths_d + ([("1d", 8)] if not interleave or n == 1 else []))
        first = p.rng.choice([0, 3, 15, 16, 29, 30, 31])
        regs = {(first + i) % 32: p.vec() for i in range(n)}
        is_load = p.rng.random() < 0.5
        base = f"{'ld' if is_load else 'st'}{n if interleave else 1}"
        lst = reg_list(first, n, width)
        total = n * (16 if q else 8)
        mode = p.rng.randrange(3)
        body = ["mov x20, x19"]
        if mode == 0:
            body.append(f"{base} {{{lst}}}, [x20]")
        elif mode == 1:
            body.append(f"{base} {{{lst}}}, [x20], #{total}")
        else:
            body.append(f"{base} {{{lst}}}, [x20], x9")
        p.vcase(body, regs, {9: p.rng.choice([0, 5, 0xFFFFFFFFFFFFFFF0])}, dumpbuf=not is_load)
    lanes = [("b", 1, 16), ("h", 2, 8), ("s", 4, 4), ("d", 8, 2)]
    for _ in range(260):
        n = p.rng.randrange(1, 5)
        e, eb, count = p.rng.choice(lanes)
        idx = p.rng.randrange(count)
        first = p.rng.choice([0, 3, 15, 16, 29, 30, 31])
        regs = {(first + i) % 32: p.vec() for i in range(n)}
        is_load = p.rng.random() < 0.5
        lst = reg_list(first, n, e)
        mode = p.rng.randrange(3)
        op = f"{'ld' if is_load else 'st'}{n}"
        body = ["mov x20, x19"]
        if mode == 0:
            body.append(f"{op} {{{lst}}}[{idx}], [x20]")
        elif mode == 1:
            body.append(f"{op} {{{lst}}}[{idx}], [x20], #{n * eb}")
        else:
            body.append(f"{op} {{{lst}}}[{idx}], [x20], x9")
        p.vcase(body, regs, {9: p.rng.choice([0, 3, 0xFFFFFFFFFFFFFFF8])}, dumpbuf=not is_load)
    # Every lane of every width, one register.
    for e, eb, count in lanes:
        for idx in range(count):
            p.vcase(["mov x20, x19", f"ld1 {{v17.{e}}}[{idx}], [x20]", "add x21, x19, #16", f"st1 {{v2.{e}}}[{idx}], [x21]"],
                    {17: p.vec(), 2: p.vec()}, dumpbuf=True)
    # Replicated loads.
    for _ in range(80):
        n = p.rng.randrange(1, 5)
        q = p.rng.random() < 0.5
        width = p.rng.choice(["16b", "8h", "4s", "2d"] if q else ["8b", "4h", "2s"])
        eb = {"b": 1, "h": 2, "s": 4, "d": 8}[width[-1]]
        first = p.rng.choice([0, 15, 16, 30, 31])
        regs = {(first + i) % 32: p.vec() for i in range(n)}
        mode = p.rng.randrange(3)
        body = ["mov x20, x19"]
        lst = reg_list(first, n, width)
        if mode == 0:
            body.append(f"ld{n}r {{{lst}}}, [x20]")
        elif mode == 1:
            body.append(f"ld{n}r {{{lst}}}, [x20], #{n * eb}")
        else:
            body.append(f"ld{n}r {{{lst}}}, [x20], x9")
        p.vcase(body, regs, {9: p.rng.choice([0, 7])})
    # The Debian rtld instruction.
    p.vcase(["mov x2, x19", "ld1 {v31.d}[1], [x2]"], {31: p.vec()})


def gen_simd_gaps(p):
    counts = [0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 65, 127, 0x80, 0x81, 0xF9, 0xF8, 0xF1, 0xF0, 0xE1, 0xE0, 0xC1, 0xC0, 0xFF, 0xFE]

    def count_vec(bits):
        lanes = 128 // bits
        vals = [p.rng.choice(counts) if p.rng.random() < 0.8 else p.rng.getrandbits(bits) for _ in range(lanes)]
        # The count is the low byte; fill the rest of each lane with junk.
        full = 0
        for i, c in enumerate(vals):
            lane = (p.rng.getrandbits(bits) & ~0xFF) | c if bits > 8 else c
            full |= lane << (bits * i)
        return (full & M64, full >> 64)

    for _ in range(1000):
        d, n, m = p.vreg(), p.vreg(), p.vreg()
        v = {d: p.vec(), n: p.vec(), m: p.vec()}
        kind = p.rng.randrange(14)
        fpcr = p.rng.choice(RMODES)
        if kind <= 1:
            op = p.rng.choice(["ushl", "sshl"])
            if p.rng.random() < 0.2:
                v[m] = count_vec(64)
                p.vcase([f"{op} d{d}, d{n}, d{m}"], v)
            else:
                vt, bits, q = vtypes(p)
                v[m] = count_vec(bits)
                p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, v{m}.{vt}"], v)
        elif kind == 2:
            op = p.rng.choice(["sri", "sli", "usra", "ssra"])
            vt, bits, q = vtypes(p)
            if op == "sli":
                sh = p.rng.choice([0, 1, bits - 1, p.rng.randrange(bits)])
            else:
                sh = p.rng.choice([1, bits, bits - 1, p.rng.randrange(1, bits + 1)])
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, #{sh}"], v)
        elif kind == 3:
            op = p.rng.choice(["uaddlp", "saddlp", "uadalp", "sadalp"])
            src, dst = p.rng.choice([("8b", "4h"), ("16b", "8h"), ("4h", "2s"), ("8h", "4s"), ("2s", "1d"), ("4s", "2d")])
            p.vcase([f"{op} v{d}.{dst}, v{n}.{src}"], v)
        elif kind == 4:
            op = p.rng.choice(["ssubw", "usubw"])
            nt, wt, nt2, bits = narrow_wide(p)
            two = p.rng.random() < 0.5
            p.vcase([f"{op}{'2' if two else ''} v{d}.{wt}, v{n}.{wt}, v{m}.{nt2 if two else nt}"], v)
        elif kind == 5:
            op = p.rng.choice(["umull", "smull", "umlal", "smlal", "umlsl", "smlsl"])
            nt, wt, nt2, bits = narrow_wide(p)
            two = p.rng.random() < 0.5
            p.vcase([f"{op}{'2' if two else ''} v{d}.{wt}, v{n}.{nt2 if two else nt}, v{m}.{nt2 if two else nt}"], v)
        elif kind == 6:
            op = p.rng.choice(["mul", "mla", "mls"])
            vt, bits, q = vtypes(p, False)
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, v{m}.{vt}"], v)
        elif kind == 7:
            q = p.rng.random() < 0.5
            if p.rng.random() < 0.5:
                vt, lane, count = ("8h" if q else "4h"), "h", 8
                mreg = p.rng.choice([r for r in VREGS if r < 16])
            else:
                vt, lane, count = ("4s" if q else "2s"), "s", 4
                mreg = p.rng.choice(VREGS)
            v[mreg] = p.vec()
            p.vcase([f"mul v{d}.{vt}, v{n}.{vt}, v{mreg}.{lane}[{p.rng.randrange(count)}]"], v)
        elif kind == 8:
            op = p.rng.choice(["smaxp", "sminp"])
            vt, bits, q = vtypes(p, False)
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, v{m}.{vt}"], v)
        elif kind == 9:
            vt = p.rng.choice(["8b", "16b"])
            p.vcase([f"rev16 v{d}.{vt}, v{n}.{vt}"], v)
        elif kind == 10:
            op = p.rng.choice(["neg d{d}, d{n}", "abs d{d}, d{n}", "addp d{d}, v{n}.2d", "uqsub d{d}, d{n}, d{m}"])
            if op.startswith("uqsub") and p.rng.random() < 0.5:
                v[m] = (v[n][0] + p.rng.choice([0, 1, -1]) & M64, v[m][1])
            p.vcase([op.format(d=d, n=n, m=m)], v)
        elif kind == 11:
            is64 = p.rng.random() < 0.5
            vn, vm = fp_pair(p, is64)
            v[n], v[m] = vn, vm
            if p.rng.random() < 0.5:
                p.vcase([f"fabd {'d' if is64 else 's'}{d}, {'d' if is64 else 's'}{n}, {'d' if is64 else 's'}{m}"], v, fpcr=fpcr)
            else:
                vt = "2d" if is64 else p.rng.choice(["2s", "4s"])
                v[n] = (p.f64() if is64 else (p.f32() << 32) | p.f32(), p.f64() if is64 else (p.f32() << 32) | p.f32())
                p.vcase([f"fabd v{d}.{vt}, v{n}.{vt}, v{m}.{vt}"], v, fpcr=fpcr)
        elif kind == 12:
            op = p.rng.choice(["fneg", "fabs"])
            vt = p.rng.choice(["2s", "4s", "2d"])
            is64 = vt == "2d"
            v[n] = (p.f64() if is64 else (p.f32() << 32) | p.f32(), p.f64() if is64 else (p.f32() << 32) | p.f32())
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}"], v)
        else:
            op = p.rng.choice(["scvtf", "ucvtf"])
            vt = p.rng.choice(["2s", "4s", "2d"])
            ints = [0, 1, M64, 0x8000000000000000, 0x7FFFFFFFFFFFFFFF, 0x20000000000001, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x1000001]
            if vt == "2d":
                v[n] = (p.rng.choice(ints + [p.rng.getrandbits(64)]), p.rng.choice(ints + [p.rng.getrandbits(64)]))
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}"], v, fpcr=fpcr)


def gen_simd_struct1(p):
    """LD1/ST1 single-element structures: one lane, every element size and
    index, offset, post-index by the element size and by a register."""
    p.emit("        adrp    x19, buf")
    p.emit("        add     x19, x19, :lo12:buf")
    p.emit("        add     x19, x19, #128")
    elems = [("b", 1, 16), ("h", 2, 8), ("s", 4, 4), ("d", 8, 2)]
    for _ in range(400):
        name, size, lanes = p.rng.choice(elems)
        t = p.vreg()
        idx = p.rng.randrange(lanes)
        v = {t: p.vec()}
        is_load = p.rng.random() < 0.5
        op = "ld1" if is_load else "st1"
        pre = ["mov x20, x19", f"sub x20, x20, #{p.rng.randrange(0, 48)}"]
        mode = p.rng.randrange(3)
        if mode == 0:
            p.vcase(pre + [f"{op} {{v{t}.{name}}}[{idx}], [x20]"], v, dumpbuf=not is_load)
        elif mode == 1:
            p.vcase(pre + [f"{op} {{v{t}.{name}}}[{idx}], [x20], #{size}"], v, dumpbuf=not is_load)
        else:
            p.vcase(pre + [f"{op} {{v{t}.{name}}}[{idx}], [x20], x10"], v, {10: p.rng.choice([0, 5, 0xFFFFFFFFFFFFFFF8])},
                    dumpbuf=not is_load)


def gen_simd_struct2(p):
    """LD2/ST2 interleaved structures: every width, offset and post-index."""
    p.emit("        adrp    x19, buf")
    p.emit("        add     x19, x19, :lo12:buf")
    p.emit("        add     x19, x19, #128")
    widths = [("8b", 8), ("16b", 16), ("4h", 8), ("8h", 16), ("2s", 8), ("4s", 16), ("2d", 16)]
    for _ in range(300):
        width, nbytes = p.rng.choice(widths)
        first = p.rng.randrange(32)
        second = (first + 1) % 32
        v = {first: p.vec(), second: p.vec()}
        is_load = p.rng.random() < 0.5
        op = "ld2" if is_load else "st2"
        lst = f"v{first}.{width}, v{second}.{width}"
        pre = ["mov x20, x19", f"sub x20, x20, #{p.rng.randrange(0, 64)}"]
        mode = p.rng.randrange(3)
        if mode == 0:
            p.vcase(pre + [f"{op} {{{lst}}}, [x20]"], v, dumpbuf=not is_load)
        elif mode == 1:
            p.vcase(pre + [f"{op} {{{lst}}}, [x20], #{2 * nbytes}"], v, dumpbuf=not is_load)
        else:
            p.vcase(pre + [f"{op} {{{lst}}}, [x20], x10"], v, {10: p.rng.choice([0, 7, 0xFFFFFFFFFFFFFFE0])}, dumpbuf=not is_load)


def gen_simd_projects(p):
    """Instructions the M2 project builds (zlib, Lua) reached that the groups
    above don't cover: scalar DUP of every element size."""
    elems = [("b", 16), ("h", 8), ("s", 4), ("d", 2)]
    for _ in range(200):
        d, n = p.vreg(), p.vreg()
        v = {d: p.vec(), n: p.vec()}
        e, count = p.rng.choice(elems)
        p.vcase([f"mov {e}{d}, v{n}.{e}[{p.rng.randrange(count)}]"], v)


# ---------------------------------------------------------------------------
# Advanced SIMD floating point (vector and scalar SIMD forms)
# ---------------------------------------------------------------------------

def fvec_pair(p, is64):
    """Two V registers of float lanes whose lanes line up NaN pairs, edge values and random values."""
    lanes = 2 if is64 else 4
    bits = 64 if is64 else 32
    a, b = 0, 0
    for i in range(lanes):
        r = p.rng.random()
        if r < 0.25:
            x, y = p.rng.choice(NAN_PAIRS64 if is64 else NAN_PAIRS32)
        else:
            x, y = (p.f64(), p.f64()) if is64 else (p.f32(), p.f32())
            if r < 0.35:
                y = x
            elif r < 0.45:
                y = x ^ (1 << (bits - 1))
        a |= x << (bits * i)
        b |= y << (bits * i)
    return (a & M64, a >> 64), (b & M64, b >> 64)


def ftype(p, allow_scalar=False):
    """(register syntax, element syntax, is64, scalar) for a float vector or scalar operand."""
    choices = [("2s", "s", False, False), ("4s", "s", False, False), ("2d", "d", True, False)]
    if allow_scalar:
        choices += [("s", "s", False, True), ("d", "d", True, True)]
    return p.rng.choice(choices)


def gen_simd_float(p):
    to_int = ["fcvtns", "fcvtnu", "fcvtps", "fcvtpu", "fcvtms", "fcvtmu", "fcvtzs", "fcvtzu", "fcvtas", "fcvtau"]
    for _ in range(1400):
        d, n, m = p.vreg(), p.vreg(), p.vreg()
        t, e, is64, scalar = ftype(p, True)
        vn, vm = fvec_pair(p, is64)
        v = {d: p.vec(), n: vn, m: vm}
        fpcr = p.rng.choice(RMODES)
        rd = f"{e}{d}" if scalar else f"v{d}.{t}"
        rn = f"{e}{n}" if scalar else f"v{n}.{t}"
        rm = f"{e}{m}" if scalar else f"v{m}.{t}"
        kind = p.rng.randrange(12)
        if kind <= 1 and not scalar:
            op = p.rng.choice(["fadd", "fsub", "fmul", "fdiv", "fmin", "fmax", "fminnm", "fmaxnm"])
            p.vcase([f"{op} {rd}, {rn}, {rm}"], v, fpcr=fpcr)
        elif kind == 2:
            op = p.rng.choice(["fmul", "fmla", "fmls"])
            lanes = 2 if is64 else 4
            idx = p.rng.randrange(lanes)
            p.vcase([f"{op} {rd}, {rn}, v{m}.{e}[{idx}]"], v, fpcr=fpcr)
        elif kind == 3 and not scalar:
            op = p.rng.choice(["fmla", "fmls"])
            vd, _ = fvec_pair(p, is64)
            v[d] = vd
            p.vcase([f"{op} {rd}, {rn}, {rm}"], v, fpcr=fpcr)
        elif kind == 4:
            op = p.rng.choice(["faddp", "fmaxp", "fminp", "fmaxnmp", "fminnmp"])
            if scalar:
                p.vcase([f"{op} {e}{d}, v{n}.2{e}"], v, fpcr=fpcr)
            else:
                p.vcase([f"{op} {rd}, {rn}, {rm}"], v, fpcr=fpcr)
        elif kind == 5:
            op = p.rng.choice(["fcmeq", "fcmge", "fcmgt", "facge", "facgt"])
            p.vcase([f"{op} {rd}, {rn}, {rm}"], v)
        elif kind == 6:
            op = p.rng.choice(["fcmeq", "fcmge", "fcmgt", "fcmle", "fcmlt"])
            p.vcase([f"{op} {rd}, {rn}, #0.0"], v)
        elif kind == 7 and not scalar:
            op = p.rng.choice(["frintn", "frintp", "frintm", "frintz", "frinta", "frintx", "frinti", "fsqrt"])
            p.vcase([f"{op} {rd}, {rn}"], v, fpcr=fpcr)
        elif kind == 8:
            op = p.rng.choice(to_int)
            p.vcase([f"{op} {rd}, {rn}"], v, fpcr=fpcr)
        elif kind == 9:
            op = p.rng.choice(["scvtf", "ucvtf", "fcvtzs", "fcvtzu"])
            bits = 64 if is64 else 32
            fbits = p.rng.choice([1, 2, bits // 2, bits - 1, bits, p.rng.randrange(1, bits + 1)])
            if op in ("scvtf", "ucvtf"):
                v[n] = p.vec()
            p.vcase([f"{op} {rd}, {rn}, #{fbits}"], v, fpcr=fpcr)
        elif kind == 10:
            op = p.rng.choice(["fneg", "fabs"])
            ht = p.rng.choice(["4h", "8h"])
            p.vcase([f"{op} v{d}.{ht}, v{n}.{ht}"], v)
        else:
            op = p.rng.choice(["fadd", "fmul", "fdiv", "fsub"])
            t2, e2, is642, _ = ftype(p)
            vn2, vm2 = fvec_pair(p, is642)
            p.vcase([f"{op} v{d}.{t2}, v{n}.{t2}, v{m}.{t2}"], {d: p.vec(), n: vn2, m: vm2}, fpcr=fpcr)
    # Every edge value through the conversions and rounding in every mode.
    for is64, edges in ((True, F64_EDGE), (False, F32_EDGE)):
        e, t = ("d", "2d") if is64 else ("s", "4s")
        bits = 64 if is64 else 32
        for i in range(0, len(edges), 2 if is64 else 4):
            chunk = edges[i:i + (2 if is64 else 4)]
            val = 0
            for j, c in enumerate(chunk):
                val |= c << (bits * j)
            reg = (val & M64, val >> 64)
            for op in ["fcvtns", "fcvtzu", "fcvtas", "fcvtps", "fcvtms", "fcvtzs", "frintn", "frinta", "frintm"]:
                p.vcase([f"{op} v5.{t}, v20.{t}"], {5: p.vec(), 20: reg}, fpcr=p.rng.choice(RMODES))


# ---------------------------------------------------------------------------
# Saturating, rounding, halving and by-element integer families
# ---------------------------------------------------------------------------

def lane_vec(p, bits):
    """A 128-bit register whose lanes are drawn from the signed/unsigned boundaries."""
    mask = (1 << bits) - 1
    edges = [0, 1, 2, mask, mask - 1, 1 << (bits - 1), (1 << (bits - 1)) + 1, (1 << (bits - 1)) - 1,
             (1 << (bits - 1)) - 2, mask >> 2, (mask >> 2) + 1]
    full = 0
    for i in range(128 // bits):
        v = p.rng.choice(edges) if p.rng.random() < 0.6 else p.rng.getrandbits(bits)
        full |= v << (bits * i)
    return (full & M64, full >> 64)


def gen_simd_sat(p):
    scal = [("b", 8), ("h", 16), ("s", 32), ("d", 64)]
    vt_of = {8: ("8b", "16b"), 16: ("4h", "8h"), 32: ("2s", "4s"), 64: (None, "2d")}
    nar = {8: ("8b", "16b", "8h"), 16: ("4h", "8h", "4s"), 32: ("2s", "4s", "2d")}
    clr = "msr fpsr, xzr"
    for _ in range(2200):
        d, n, m = p.vreg(), p.vreg(), p.vreg()
        kind = p.rng.randrange(19)
        if kind <= 1:
            op = p.rng.choice(["sqadd", "sqsub", "uqadd", "uqsub"])
            if p.rng.random() < 0.3:
                r, bits = p.rng.choice(scal)
                v = {d: p.vec(), n: lane_vec(p, bits), m: lane_vec(p, bits)}
                p.vcase([clr, f"{op} {r}{d}, {r}{n}, {r}{m}"], v)
            else:
                vt, bits, q = vtypes(p)
                v = {d: p.vec(), n: lane_vec(p, bits), m: lane_vec(p, bits)}
                p.vcase([clr, f"{op} v{d}.{vt}, v{n}.{vt}, v{m}.{vt}"], v)
        elif kind == 2:
            op = p.rng.choice(["sqabs", "sqneg"])
            if p.rng.random() < 0.3:
                r, bits = p.rng.choice(scal)
                p.vcase([clr, f"{op} {r}{d}, {r}{n}"], {d: p.vec(), n: lane_vec(p, bits)})
            else:
                vt, bits, q = vtypes(p)
                p.vcase([clr, f"{op} v{d}.{vt}, v{n}.{vt}"], {d: p.vec(), n: lane_vec(p, bits)})
        elif kind == 3:
            op = p.rng.choice(["sqxtn", "uqxtn", "sqxtun"])
            bits = p.rng.choice([8, 16, 32])
            nt, nt2, wt = nar[bits]
            v = {d: p.vec(), n: lane_vec(p, bits * 2)}
            form = p.rng.randrange(3)
            if form == 0:
                p.vcase([clr, f"{op} v{d}.{nt}, v{n}.{wt}"], v)
            elif form == 1:
                p.vcase([clr, f"{op}2 v{d}.{nt2}, v{n}.{wt}"], v)
            else:
                rn = {8: "b", 16: "h", 32: "s"}[bits]
                rw = {8: "h", 16: "s", 32: "d"}[bits]
                p.vcase([clr, f"{op} {rn}{d}, {rw}{n}"], v)
        elif kind <= 5:
            op = p.rng.choice(["rshrn", "sqshrn", "sqrshrn", "uqshrn", "uqrshrn", "sqshrun", "sqrshrun"])
            bits = p.rng.choice([8, 16, 32])
            nt, nt2, wt = nar[bits]
            sh = p.rng.choice([1, 2, bits - 1, bits, p.rng.randrange(1, bits + 1)])
            v = {d: p.vec(), n: lane_vec(p, bits * 2)}
            form = p.rng.randrange(3)
            if form == 2 and op in ("sqshrn", "uqshrn", "sqshrun"):
                rn = {8: "b", 16: "h", 32: "s"}[bits]
                rw = {8: "h", 16: "s", 32: "d"}[bits]
                p.vcase([clr, f"{op} {rn}{d}, {rw}{n}, #{sh}"], v)
            elif form == 1:
                p.vcase([clr, f"{op}2 v{d}.{nt2}, v{n}.{wt}, #{sh}"], v)
            else:
                p.vcase([clr, f"{op} v{d}.{nt}, v{n}.{wt}, #{sh}"], v)
        elif kind == 6:
            op = p.rng.choice(["srshr", "urshr", "srsra", "ursra"])
            if p.rng.random() < 0.25:
                sh = p.rng.choice([1, 2, 63, 64, p.rng.randrange(1, 65)])
                p.vcase([f"{op} d{d}, d{n}, #{sh}"], {d: lane_vec(p, 64), n: lane_vec(p, 64)})
            else:
                vt, bits, q = vtypes(p)
                sh = p.rng.choice([1, 2, bits - 1, bits, p.rng.randrange(1, bits + 1)])
                p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, #{sh}"], {d: lane_vec(p, bits), n: lane_vec(p, bits)})
        elif kind == 7:
            op = p.rng.choice(["sqshl", "uqshl", "sqshlu"])
            if p.rng.random() < 0.3:
                r, bits = p.rng.choice(scal)
                sh = p.rng.choice([0, 1, bits - 1, p.rng.randrange(bits)])
                p.vcase([clr, f"{op} {r}{d}, {r}{n}, #{sh}"], {d: p.vec(), n: lane_vec(p, bits)})
            else:
                vt, bits, q = vtypes(p)
                sh = p.rng.choice([0, 1, bits - 1, p.rng.randrange(bits)])
                p.vcase([clr, f"{op} v{d}.{vt}, v{n}.{vt}, #{sh}"], {d: p.vec(), n: lane_vec(p, bits)})
        elif kind == 8:
            op = p.rng.choice(["uhadd", "shadd", "urhadd", "srhadd", "uhsub", "shsub"])
            vt, bits, q = vtypes(p, False)
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, v{m}.{vt}"], {d: p.vec(), n: lane_vec(p, bits), m: lane_vec(p, bits)})
        elif kind == 9:
            op = p.rng.choice(["uabd", "sabd", "uaba", "saba"])
            vt, bits, q = vtypes(p, False)
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, v{m}.{vt}"], {d: lane_vec(p, bits), n: lane_vec(p, bits), m: lane_vec(p, bits)})
        elif kind == 10:
            op = p.rng.choice(["uabdl", "sabdl", "uabal", "sabal"])
            bits = p.rng.choice([8, 16, 32])
            nt, nt2, wt = nar[bits]
            v = {d: lane_vec(p, bits * 2), n: lane_vec(p, bits), m: lane_vec(p, bits)}
            if p.rng.random() < 0.5:
                p.vcase([f"{op} v{d}.{wt}, v{n}.{nt}, v{m}.{nt}"], v)
            else:
                p.vcase([f"{op}2 v{d}.{wt}, v{n}.{nt2}, v{m}.{nt2}"], v)
        elif kind == 11:
            op = p.rng.choice(["raddhn", "rsubhn"])
            bits = p.rng.choice([8, 16, 32])
            nt, nt2, wt = nar[bits]
            v = {d: p.vec(), n: lane_vec(p, bits * 2), m: lane_vec(p, bits * 2)}
            if p.rng.random() < 0.5:
                p.vcase([f"{op} v{d}.{nt}, v{n}.{wt}, v{m}.{wt}"], v)
            else:
                p.vcase([f"{op}2 v{d}.{nt2}, v{n}.{wt}, v{m}.{wt}"], v)
        elif kind <= 13:
            op = p.rng.choice(["sqdmulh", "sqrdmulh"])
            bits = p.rng.choice([16, 32])
            e = "h" if bits == 16 else "s"
            vt = p.rng.choice(vt_of[bits])
            mreg = p.rng.choice([r for r in VREGS if r < 16]) if bits == 16 else p.vreg()
            v = {d: p.vec(), n: lane_vec(p, bits), m: lane_vec(p, bits), mreg: lane_vec(p, bits)}
            idx = p.rng.randrange(128 // bits)
            form = p.rng.randrange(4)
            if form == 0:
                p.vcase([clr, f"{op} v{d}.{vt}, v{n}.{vt}, v{m}.{vt}"], v)
            elif form == 1:
                p.vcase([clr, f"{op} {e}{d}, {e}{n}, {e}{m}"], v)
            elif form == 2:
                p.vcase([clr, f"{op} v{d}.{vt}, v{n}.{vt}, v{mreg}.{e}[{idx}]"], v)
            else:
                p.vcase([clr, f"{op} {e}{d}, {e}{n}, v{mreg}.{e}[{idx}]"], v)
        elif kind == 14:
            op = p.rng.choice(["mla", "mls", "smull", "umull", "smlal", "umlal", "smlsl", "umlsl"])
            bits = p.rng.choice([16, 32])
            e = "h" if bits == 16 else "s"
            mreg = p.rng.choice([r for r in VREGS if r < 16]) if bits == 16 else p.vreg()
            idx = p.rng.randrange(128 // bits)
            v = {d: lane_vec(p, bits), n: lane_vec(p, bits), mreg: lane_vec(p, bits)}
            if op in ("mla", "mls"):
                vt = p.rng.choice(vt_of[bits])
                p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}, v{mreg}.{e}[{idx}]"], v)
            else:
                nt, nt2, wt = {16: ("4h", "8h", "4s"), 32: ("2s", "4s", "2d")}[bits]
                if p.rng.random() < 0.5:
                    p.vcase([f"{op} v{d}.{wt}, v{n}.{nt}, v{mreg}.{e}[{idx}]"], v)
                else:
                    p.vcase([f"{op}2 v{d}.{wt}, v{n}.{nt2}, v{mreg}.{e}[{idx}]"], v)
        elif kind == 15:
            op = p.rng.choice(["uaddlv", "saddlv"])
            r, vt, bits = p.rng.choice([("h", "8b", 8), ("h", "16b", 8), ("s", "4h", 16), ("s", "8h", 16), ("d", "4s", 32)])
            p.vcase([f"{op} {r}{d}, v{n}.{vt}"], {d: p.vec(), n: lane_vec(p, bits)})
        elif kind == 16:
            op = p.rng.choice(["smaxv", "sminv"])
            r, vt, bits = p.rng.choice([("b", "8b", 8), ("b", "16b", 8), ("h", "4h", 16), ("h", "8h", 16), ("s", "4s", 32)])
            p.vcase([f"{op} {r}{d}, v{n}.{vt}"], {d: p.vec(), n: lane_vec(p, bits)})
        elif kind == 17:
            op = p.rng.choice(["clz", "cls"])
            vt, bits, q = vtypes(p, False)
            full = 0
            for i in range(128 // bits):
                full |= (p.rng.getrandbits(bits) >> p.rng.randrange(bits + 1)) << (bits * i)
            vals = (full & M64, full >> 64) if p.rng.random() < 0.6 else lane_vec(p, bits)
            p.vcase([f"{op} v{d}.{vt}, v{n}.{vt}"], {d: p.vec(), n: vals})
        else:
            q = p.rng.random() < 0.5
            p.vcase([f"udot v{d}.{'4s' if q else '2s'}, v{n}.{'16b' if q else '8b'}, v{m}.{'16b' if q else '8b'}"],
                    {d: p.vec(), n: lane_vec(p, 8), m: lane_vec(p, 8)})
    # QC stays set across instructions until cleared.
    p.vcase(["msr fpsr, xzr", "sqadd v1.16b, v2.16b, v3.16b", "add v4.16b, v2.16b, v3.16b"],
            {1: p.vec(), 2: (0x7F7F7F7F7F7F7F7F, 0), 3: (0x0101010101010101, 0)})


# ---------------------------------------------------------------------------
# Cryptographic extension and CRC32
# ---------------------------------------------------------------------------

AES_SBOX = None


def aes_key_expand(key):
    """AES-128 round keys (11 x 16 bytes) for the FIPS-197 vector."""
    global AES_SBOX
    if AES_SBOX is None:
        # Multiplicative inverse in GF(2^8) then the affine map.
        def gmul(a, b):
            r = 0
            while b:
                if b & 1:
                    r ^= a
                a = ((a << 1) ^ 0x11B) if a & 0x80 else a << 1
                b >>= 1
            return r
        inv = [0] * 256
        for a in range(1, 256):
            for b in range(1, 256):
                if gmul(a, b) == 1:
                    inv[a] = b
                    break
        AES_SBOX = []
        for a in range(256):
            x = inv[a]
            y = x
            for _ in range(4):
                x = ((x << 1) | (x >> 7)) & 0xFF
                y ^= x
            AES_SBOX.append(y ^ 0x63)
    words = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        t = list(words[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [AES_SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = ((rcon << 1) ^ 0x11B) if rcon & 0x80 else rcon << 1
        words.append([words[i - 4][j] ^ t[j] for j in range(4)])
    return [bytes(sum(words[r * 4:r * 4 + 4], [])) for r in range(11)]


def bytes_reg(b):
    v = int.from_bytes(b, "little")
    return (v & M64, v >> 64)


def words_reg(ws):
    v = 0
    for i, w in enumerate(ws):
        v |= w << (32 * i)
    return (v & M64, v >> 64)


def gen_simd_crypto(p):
    for _ in range(700):
        d, n, m = p.vregs(3)
        v = {d: p.vec(), n: p.vec(), m: p.vec()}
        kind = p.rng.randrange(9)
        if kind == 0:
            op = p.rng.choice(["aese", "aesd"])
            p.vcase([f"{op} v{d}.16b, v{n}.16b"], v)
        elif kind == 1:
            op = p.rng.choice(["aesmc", "aesimc"])
            p.vcase([f"{op} v{d}.16b, v{n}.16b"], v)
        elif kind == 2:
            if p.rng.random() < 0.5:
                v[n] = p.rng.choice([(M64, M64), (0x8000000000000001, 0x8000000000000001), p.vec()])
                p.vcase([f"pmull v{d}.1q, v{n}.1d, v{m}.1d"], v)
            else:
                p.vcase([f"pmull2 v{d}.1q, v{n}.2d, v{m}.2d"], v)
        elif kind == 3:
            if p.rng.random() < 0.5:
                p.vcase([f"pmull v{d}.8h, v{n}.8b, v{m}.8b"], v)
            else:
                p.vcase([f"pmull2 v{d}.8h, v{n}.16b, v{m}.16b"], v)
        elif kind == 4:
            op = p.rng.choice(["sha1c", "sha1m", "sha1p"])
            p.vcase([f"{op} q{d}, s{n}, v{m}.4s"], v)
        elif kind == 5:
            op = p.rng.choice(["sha1h s{d}, s{n}", "sha1su0 v{d}.4s, v{n}.4s, v{m}.4s", "sha1su1 v{d}.4s, v{n}.4s"])
            p.vcase([op.format(d=d, n=n, m=m)], v)
        elif kind == 6:
            op = p.rng.choice(["sha256h q{d}, q{n}, v{m}.4s", "sha256h2 q{d}, q{n}, v{m}.4s",
                               "sha256su0 v{d}.4s, v{n}.4s", "sha256su1 v{d}.4s, v{n}.4s, v{m}.4s"])
            p.vcase([op.format(d=d, n=n, m=m)], v)
        else:
            x, y, z = p.rng.sample(XREGS, 3)
            op = p.rng.choice(["crc32b", "crc32h", "crc32w", "crc32x", "crc32cb", "crc32ch", "crc32cw", "crc32cx"])
            src = "x" if op.endswith("x") else "w"
            vals = {x: p.rng.choice([0, M64, 0xFFFFFFFF, p.rng.getrandbits(64)]), y: p.rng.getrandbits(64), z: p.rng.getrandbits(64)}
            p.vcase([f"{op} w{z}, w{x}, {src}{y}"], v, vals)
    # Aliased operands.
    p.vcase(["aese v3.16b, v3.16b", "aesd v17.16b, v17.16b", "sha1su1 v20.4s, v20.4s", "sha256su1 v21.4s, v21.4s, v21.4s",
             "pmull v22.1q, v22.1d, v22.1d", "crc32x w9, w9, x9"],
            {3: p.vec(), 17: p.vec(), 20: p.vec(), 21: p.vec(), 22: p.vec()}, {9: p.rng.getrandbits(64)})

    # FIPS-197 C.1: AES-128 encrypt 00112233.. under 000102..0f, then decrypt.
    key = bytes(range(16))
    rks = aes_key_expand(key)
    pt = bytes.fromhex("00112233445566778899aabbccddeeff")
    v = {16 + r: bytes_reg(rks[r]) for r in range(11)}
    v[0] = bytes_reg(pt)
    body = []
    for r in range(9):
        body += [f"aese v0.16b, v{16 + r}.16b", "aesmc v0.16b, v0.16b"]
    body += ["aese v0.16b, v25.16b", "eor v0.16b, v0.16b, v26.16b", "mov v1.16b, v0.16b"]
    # Decryption keys: InvMixColumns of rk9..rk1.
    for r in range(1, 10):
        body.append(f"aesimc v{16 + r}.16b, v{16 + r}.16b")
    body += ["aesd v1.16b, v26.16b", "aesimc v1.16b, v1.16b"]
    for r in range(9, 1, -1):
        body += [f"aesd v1.16b, v{16 + r}.16b", "aesimc v1.16b, v1.16b"]
    body += ["aesd v1.16b, v17.16b", "eor v1.16b, v1.16b, v16.16b"]
    p.vcase(body, v)

    k256 = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ]
    block = b"abc" + b"\x80" + bytes(52) + (24).to_bytes(8, "big")

    # SHA-256 compression of the padded "abc" block with the ARM intrinsics
    # schedule (state words A..D in v0, E..H in v1, message words in v4-v7).
    iv256 = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]
    v = {0: words_reg(iv256[:4]), 1: words_reg(iv256[4:])}
    for i in range(4):
        v[4 + i] = bytes_reg(block[4 * i:4 * i + 4 * 4][:16]) if False else bytes_reg(block[16 * i:16 * i + 16])
    body = ["mov v28.16b, v0.16b", "mov v29.16b, v1.16b"] + [f"rev32 v{4 + i}.16b, v{4 + i}.16b" for i in range(4)]
    for g in range(16):
        msg = 4 + g % 4
        v[8 + g] = words_reg(k256[4 * g:4 * g + 4]) if 8 + g < 16 else None
        body += [f"add v3.4s, v{msg}.4s, v27.4s".replace("v27", f"v{8 + g}") if 8 + g < 16 else f"adr x9, sha256k_{g}", ]
        if 8 + g >= 16:
            body += ["ld1 {v27.4s}, [x9]", f"add v3.4s, v{msg}.4s, v27.4s"]
        body += ["mov v2.16b, v0.16b", "sha256h q0, q1, v3.4s", "sha256h2 q1, q2, v3.4s"]
        body += [f"sha256su0 v{msg}.4s, v{4 + (g + 1) % 4}.4s",
                 f"sha256su1 v{msg}.4s, v{4 + (g + 2) % 4}.4s, v{4 + (g + 3) % 4}.4s"]
    body += ["add v0.4s, v0.4s, v28.4s", "add v1.4s, v1.4s, v29.4s", "b 3f"]
    for g in range(8, 16):
        body.append(f"sha256k_{g}: .word " + ", ".join(f"0x{k:08x}" for k in k256[4 * g:4 * g + 4]))
    body.append("3:")
    v = {r: val for r, val in v.items() if val is not None}
    p.vcase(body, v)

    # SHA-1 compression of the same block (ABCD in v0, E in w/s5 and s6).
    iv1 = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0]
    k1 = [0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6]
    v = {0: words_reg(iv1[:4]), 5: (iv1[4], 0)}
    for i in range(4):
        v[8 + i] = bytes_reg(block[16 * i:16 * i + 16])
    for i in range(4):
        v[12 + i] = words_reg([k1[i]] * 4)
    body = ["mov v28.16b, v0.16b", "mov v29.16b, v5.16b"] + [f"rev32 v{8 + i}.16b, v{8 + i}.16b" for i in range(4)]
    for g in range(20):
        msg = 8 + g % 4
        fn = ["sha1c", "sha1p", "sha1m", "sha1p"][g // 5]
        body += [f"add v3.4s, v{msg}.4s, v{12 + g // 5}.4s", "sha1h s6, s0", f"{fn} q0, s5, v3.4s", "mov v5.16b, v6.16b",
                 f"sha1su0 v{msg}.4s, v{8 + (g + 1) % 4}.4s, v{8 + (g + 2) % 4}.4s",
                 f"sha1su1 v{msg}.4s, v{8 + (g + 3) % 4}.4s"]
    body += ["add v0.4s, v0.4s, v28.4s", "add v5.4s, v5.4s, v29.4s"]
    p.vcase(body, v)

    # CRC-32 and CRC-32C check values of "123456789" (0xCBF43926, 0xE3069283).
    for op in ("crc32b", "crc32cb"):
        body = ["adr x9, 4f", "mov w0, #-1", "mov x10, #9"]
        body += ["5:", "ldrb w1, [x9], #1", f"{op} w0, w0, w1", "subs x10, x10, #1", "b.ne 5b", "mvn w0, w0", "b 6f",
                 '4: .ascii "123456789"', ".balign 4", "6:"]
        p.vcase(body, {})
    # Wider words over the same bytes: "12345678" as one x, two w, four h.
    for op, width in (("crc32", "x"), ("crc32c", "x")):
        p.vcase(["adr x9, 7f", "mov w0, #-1", "ldr x1, [x9]", f"{op}x w0, w0, x1",
                 "mov w2, #-1", "ldr w3, [x9]", f"{op}w w2, w2, w3", "ldr w3, [x9, #4]", f"{op}w w2, w2, w3",
                 "mov w4, #-1", "ldrh w5, [x9]", f"{op}h w4, w4, w5", "ldrh w5, [x9, #2]", f"{op}h w4, w4, w5",
                 "b 8f", '7: .ascii "12345678"', ".balign 4", "8:"], {})


GROUPS = {
    "simd_loadstore": gen_simd_loadstore,
    "simd_copy": gen_simd_copy,
    "simd_arith": gen_simd_arith,
    "simd_shape": gen_simd_shape,
    "fp_scalar": gen_fp_scalar,
    "fp_convert": gen_fp_convert,
    "fp_fpcr": gen_fp_fpcr,
    "fp_half": gen_fp_half,
    "fp_round": gen_fp_round,
    "simd_struct1": gen_simd_struct1,
    "simd_struct2": gen_simd_struct2,
    "exclusive": gen_exclusive,
    "simd_table": gen_simd_table,
    "simd_struct": gen_simd_struct,
    "simd_gaps": gen_simd_gaps,
    "simd_projects": gen_simd_projects,
    "simd_float": gen_simd_float,
    "simd_sat": gen_simd_sat,
    "simd_crypto": gen_simd_crypto,
}


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for name, fn in GROUPS.items():
        rng = random.Random(f"{SEED}-{name}")
        p = VProgram(name, rng)
        fn(p)
        p.write(outdir)
        print(f"{name}: {p.cases} cases")


if __name__ == "__main__":
    main()
