# One 2048-aligned leaf function per shape, 16 repetitions, registers varied
# across the repetitions so no two share operands; entered by an indirect call
# so the decoder never folds a function into its caller's unit.
import itertools
LO = list(range(0, 16)); HI = list(range(16, 32))
def regs(bank, k, n=3):
    base = bank
    return [base[(k * n + i) % 16] for i in range(n)]
shapes = []
def add(name, fmt, bank, n=3, prefix=""):
    body = []
    for k in range(16):
        r = regs(bank, k, n)
        body.append(prefix + fmt.format(*r))
    shapes.append((name, body))
for bank, tag in ((LO, "lo"), (HI, "hi")):
    add(f"fadd_d_{tag}",   "fadd d{0}, d{1}, d{2}", bank)
    add(f"faddacc_d_{tag}","fadd d{0}, d{0}, d{1}", bank, 2)
    add(f"fmul_d_{tag}",   "fmul d{0}, d{1}, d{2}", bank)
    add(f"fmadd_d_{tag}",  "fmadd d{0}, d{1}, d{2}, d{3}", bank, 4)
    add(f"fsqrt_d_{tag}",  "fsqrt d{0}, d{1}", bank, 2)
    add(f"fneg_d_{tag}",   "fneg d{0}, d{1}", bank, 2)
    add(f"fcmp_d_{tag}",   "fcmp d{0}, d{1}", bank, 2)
    add(f"fcsel_d_{tag}",  "fcsel d{0}, d{1}, d{2}, gt", bank)
    add(f"fcvt_sd_{tag}",  "fcvt s{0}, d{1}", bank, 2)
    add(f"scvtf_dx_{tag}", "scvtf d{0}, x1", bank, 1)
    add(f"fcvtzs_xd_{tag}","fcvtzs x1, d{0}", bank, 1)
    add(f"fmov_dx_{tag}",  "fmov d{0}, x1", bank, 1)
    add(f"fmov_xd_{tag}",  "fmov x1, d{0}", bank, 1)
    add(f"ldr_d_{tag}",    "ldr d{0}, [x0, #8]", bank, 1)
    add(f"str_d_{tag}",    "str d{0}, [x0, #8]", bank, 1)
    add(f"ldr_q_{tag}",    "ldr q{0}, [x0, #16]", bank, 1)
    add(f"fadd_v2d_{tag}", "fadd v{0}.2d, v{1}.2d, v{2}.2d", bank)
    add(f"fmla_v2d_{tag}", "fmla v{0}.2d, v{1}.2d, v{2}.2d", bank)
    add(f"fcmgt_v2d_{tag}","fcmgt v{0}.2d, v{1}.2d, v{2}.2d", bank)
    add(f"bsl_{tag}",      "bsl v{0}.16b, v{1}.16b, v{2}.16b", bank)
    add(f"eor_{tag}",      "eor v{0}.16b, v{1}.16b, v{2}.16b", bank)
    add(f"and_{tag}",      "and v{0}.16b, v{1}.16b, v{2}.16b", bank)
    add(f"dup_2d_{tag}",   "dup v{0}.2d, v{1}.d[1]", bank, 2)
    add(f"fabs_v2d_{tag}", "fabs v{0}.2d, v{1}.2d", bank, 2)
    add(f"add_v4s_{tag}",  "add v{0}.4s, v{1}.4s, v{2}.4s", bank)
    add(f"cmeq_v4s_{tag}", "cmeq v{0}.4s, v{1}.4s, v{2}.4s", bank)
    add(f"uzp1_v4s_{tag}", "uzp1 v{0}.4s, v{1}.4s, v{2}.4s", bank)
    add(f"movi0_{tag}",    "movi v{0}.2d, #0", bank, 1)
    add(f"fmov_v1_{tag}",  "fmov v{0}.2d, #1.0", bank, 1)
out = [".text", ".globl _start", "_start:",
       "  adr x0, buf", "  mov x1, #3", "  fmov d16, x1", "  fmov d17, x1", "  fmov d18, x1"]
for i in range(32): out.append(f"  fmov d{i}, x1")
for name, body in shapes:
    out += [f"  adr x9, f_{name}", "  blr x9"]
out += ["  adr x9, f_empty", "  blr x9",
        "  mov x0, #0", "  mov x8, #93", "  svc #0"]
out += [".balign 2048", "f_empty:", "  ret"]
for name, body in shapes:
    out += [".balign 2048", f"f_{name}:"] + ["  " + b for b in body] + ["  ret"]
out += [".data", ".balign 64", "buf: .space 4096"]
open("shapes.S", "w").write("\n".join(out) + "\n")
print(len(shapes), "shapes")
