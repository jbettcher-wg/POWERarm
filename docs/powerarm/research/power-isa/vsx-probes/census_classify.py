#!/usr/bin/env python3
# Static census of AArch64 FP/SIMD instructions: family x low-bank (V16-V31) mention.
# Reads `objdump -d --no-show-raw-insn` on stdin. Unweighted by execution.
import sys, re, json, collections
REG = re.compile(r'\b([vdsqhb])(\d+)\b')
ARR = re.compile(r'\.(16b|8b|8h|4h|4s|2s|2d|1d|[bhsd])\[?')
S1_SCALAR = set("fadd fsub fmul fdiv fnmul fmadd fmsub fnmadd fnmsub fsqrt fneg fabs fcmp fcmpe fccmp fccmpe fcsel fcvt fcvtzs fcvtzu fcvtns fcvtnu fcvtps fcvtpu fcvtms fcvtmu fcvtas fcvtau scvtf ucvtf fmov fmin fmax fminnm fmaxnm".split())
S1_VEC = set("fadd fsub fmul fdiv fmla fmls fabs fneg fsqrt fmin fmax fminnm fmaxnm scvtf ucvtf fmulx".split())
FCM = set("fcmeq fcmge fcmgt fcmle fcmlt facge facgt".split())
LOGIC = set("and orr eor bic orn not bsl bit bif mvn".split())
PERM = set("zip1 zip2 uzp1 uzp2 trn1 trn2".split())
VFCVT = set("fcvtzs fcvtzu fcvtns fcvtnu fcvtps fcvtpu fcvtms fcvtmu fcvtas fcvtau".split())
FRINT = set("frinta frinti frintm frintn frintp frintx frintz frint32z frint32x frint64z frint64x".split())
LDST = set("ldr str ldur stur ldp stp ldnp stnp ld1 st1 ld1r".split())
LDSTN = set("ld2 ld3 ld4 st2 st3 st4 ld2r ld3r ld4r".split())
fam = collections.Counter(); low = collections.Counter(); unknown = collections.Counter()
total = 0; fp = 0
def arrangement(ops):
    m = ARR.search(ops); return m.group(1) if m else ''
def wide(a):  # 32/64-bit lane?
    return a in ('4s','2s','2d','1d','s','d')
for line in sys.stdin:
    if '\t' not in line: continue
    parts = line.rstrip('\n').split('\t')
    if len(parts) < 2: continue
    total += 1
    if len(parts) >= 3: mn, ops = parts[-2].strip(), parts[-1]
    else: mn, ops = parts[-1].strip(), ''
    if ' ' in mn: mn, rest = mn.split(None, 1); ops = rest + ' ' + ops
    ops = re.sub(r'<[^>]*>', '', ops)
    ops = re.sub(r'//.*', '', ops)
    regs = REG.findall(ops)
    if not regs: continue
    # 'b' false positives: 'b' only as a reg in ldr b0 etc. Branch mnemonics have no reg operand.
    if mn.startswith('b.') or mn in ('b','bl','br','blr','cbz','cbnz','tbz','tbnz'): continue
    if mn.startswith(('crc32','prfm','ld64b','st64b')): continue
    vec = any(r[0]=='v' for r in regs)
    a = arrangement(ops)
    f = None
    if mn in LDSTN: f = 'ldst_multi'
    elif mn in LDST: f = 'ldst'
    elif not vec:
        if mn in S1_SCALAR: f = 'S1_scalar'
        elif mn in FRINT: f = 'S2_frint'
        elif mn in ('mov',): f = 'S2_dupscalar'  # mov s0, v1.s[1] is vec; plain mov d,d is fmov
        else: f = None
    else:
        base = mn
        if base in S1_VEC: f = 'S1_lanes'
        elif base in LOGIC or (base=='mov' and a in ('16b','8b')): f = 'S1_logic'
        elif base in FCM: f = 'S1_fcm64' if a in ('2d','1d','d') else 'S2_fcm32'
        elif base in PERM: f = 'S2_perm' if wide(a) else 'C_perm_bh'
        elif base == 'ext':
            m = re.search(r'#(\d+)', ops); n = int(m.group(1)) if m else 0
            f = 'S2_ext' if n % 4 == 0 else 'C_ext'
        elif base in ('ins','umov','smov','dup','mov','fmov'):
            # element access width
            e = re.search(r'\.([bhsd])\[', ops)
            w = e.group(1) if e else a
            if base in ('ins','mov') and e: f = 'S2_ins' if w in ('s','d') else 'C_ins_bh'
            elif base in ('umov','smov') or (base=='fmov' and e): f = 'S2_umov' if w in ('s','d') else 'C_umov_bh'
            elif base=='dup': f = 'S2_dup' if (w in ('s','d') or wide(a)) else 'C_dup_bh'
            elif base in ('fmov','movi','mvni'): f = 'S2_imm' if wide(a) or a in ('16b','8b') else 'C_imm_h'
            else: f = 'C_other'
        elif base in ('movi','mvni'): f = 'S2_imm' if (wide(a) or a in ('16b','8b')) else 'C_imm_h'
        elif base in ('fcvtn','fcvtl','fcvtn2','fcvtl2','fcvtxn','fcvtxn2'): f = 'S2_fcvt_sd' if a in ('2s','2d','4s') else 'C_f16'
        elif base in VFCVT: f = 'S2_vfcvt' if wide(a) else 'C_f16'
        elif base in FRINT: f = 'S2_frint' if wide(a) else 'C_f16'
        elif base.startswith(('rev',)): f = 'C_rev'
        else: f = 'C_int'
    if f is None:
        unknown[mn] += 1; f = 'C_unknown'
    fp += 1
    fam[f] += 1
    if any(int(r[1]) >= 16 for r in regs): low[f] += 1
out = {'total_insns': total, 'fp_simd_insns': fp, 'families': {k: [fam[k], low[k]] for k in sorted(fam)}, 'unknown_top': unknown.most_common(15)}
json.dump(out, sys.stdout, indent=1); print()
