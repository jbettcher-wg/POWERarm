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


GROUPS = {
    "simd_loadstore": gen_simd_loadstore,
    "simd_copy": gen_simd_copy,
    "simd_arith": gen_simd_arith,
    "simd_shape": gen_simd_shape,
    "fp_scalar": gen_fp_scalar,
    "fp_convert": gen_fp_convert,
    "fp_fpcr": gen_fp_fpcr,
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
