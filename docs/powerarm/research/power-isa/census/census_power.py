#!/usr/bin/env python3
# Static A64 census for POWER-STRENGTHS.md: counts the guest shapes that each POWER lead
# would touch. Input: `llvm-objdump -d --no-show-raw-insn --no-leading-addr` on stdin.
# Lines look like "<addr>:\t<mnem>\t<ops>" or "\t<mnem>\t<ops>".
import sys, re, collections

c = collections.Counter()
bl = collections.Counter()
SYM = re.compile(r'<([^>]+)>')
VEC = re.compile(r'\bv\d+\.')
REG = re.compile(r'\b([wxvdsqhb]\d+|wzr|xzr|sp)\b')

def regs(ops):
    return REG.findall(ops)

def dst(ops):
    r = regs(ops)
    return r[0] if r else None

def srcs(ops):
    r = regs(ops)
    return r[1:] if len(r) > 1 else []

def base(r):
    # x5 / w5 / v5 / d5 / s5 -> 5 with class letter stripped so fmov d0,x0 ; cnt v0 matches
    m = re.match(r'[wxvdsqhb](\d+)', r)
    return m.group(1) if m else r

win = collections.deque(maxlen=8)   # (mnem, ops, dst, srcs)

CMPS = ('cmeq', 'cmhi', 'cmhs', 'cmgt', 'cmge', 'cmtst', 'cmlt', 'cmle', 'fcmeq', 'fcmgt', 'fcmge', 'fcmlt', 'fcmle')
REDUCE = ('umaxv', 'uminv', 'addv', 'smaxv', 'sminv', 'uaddlv', 'saddlv')
DOT = ('sdot', 'udot', 'usdot', 'sudot', 'smlal', 'smlal2', 'umlal', 'umlal2', 'smlsl', 'smlsl2', 'umlsl', 'umlsl2',
       'sqdmull', 'sqdmull2', 'sqdmlal', 'sqdmlal2', 'sqdmlsl', 'sqdmlsl2', 'sqdmulh', 'sqrdmulh', 'sqrdmlah', 'sqrdmlsh',
       'pmull', 'pmull2', 'pmul', 'sadalp', 'uadalp', 'saddlp', 'uaddlp', 'saddlv', 'uaddlv', 'saddl', 'saddl2', 'uaddl',
       'uaddl2', 'ssubl', 'ssubl2', 'usubl', 'usubl2', 'sabal', 'uabal', 'sabdl', 'uabdl', 'saddw', 'uaddw', 'ssubw', 'usubw')
WIDEN = ('sxtl', 'sxtl2', 'uxtl', 'uxtl2', 'sshll', 'sshll2', 'ushll', 'ushll2')
LDS = ('ldr', 'ldrh', 'ldur', 'ldurh', 'ldrb', 'ldurb', 'ldp')
STS = ('str', 'strh', 'stur', 'sturh', 'strb', 'sturb', 'stp')
TF = ('__addtf3', '__subtf3', '__multf3', '__divtf3', '__extenddftf2', '__extendsftf2', '__trunctfdf2', '__trunctfsf2',
      '__floatsitf', '__floatditf', '__floatunsitf', '__floatunditf', '__fixtfsi', '__fixtfdi', '__fixunstfsi',
      '__fixunstfdi', '__eqtf2', '__netf2', '__lttf2', '__letf2', '__gttf2', '__getf2', '__unordtf2', '__negtf2',
      '__extendhftf2', '__trunctfhf2', '__floattitf', '__fixtfti')
LIBML = re.compile(r'^(sqrt|fma|exp|exp2|expm1|log|log2|log10|log1p|pow|sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|'
                   r'cbrt|hypot|fmod|remainder|ldexp|frexp|scalbn|scalbln|nextafter|nexttoward|rint|nearbyint|round|'
                   r'trunc|floor|ceil|fabs|copysign|fmin|fmax|fdim|erf|erfc|lgamma|tgamma|modf|ilogb|logb|'
                   r'strtold|__strtold_internal|wcstold|qecvt|qfcvt|__printf_fp)l?$')

for line in sys.stdin:
    f = line.rstrip('\n').split('\t')
    if len(f) < 2:
        continue
    m = f[1].strip()
    ops = f[2].strip() if len(f) > 2 else ''
    if not m or m.startswith('<') or m.startswith('.') or re.match(r'^[0-9a-f]+:$', m):
        continue
    c['total'] += 1
    vec = VEC.search(ops) is not None
    d = dst(ops)
    s = srcs(ops)

    if m.startswith('crc32'):
        c[m] += 1
    elif m == 'prfm':
        c['prfm'] += 1
        c['prfm_' + ops.split(',')[0].strip()] += 1
    elif m in ('rev', 'rev16', 'rev32') and d and d[0] in 'wx':
        c['rev_gpr_' + m] += 1
        c['rev_gpr'] += 1
        if win:
            pm, pops, pd, ps = win[-1]
            if pm in LDS and pd and s and base(pd) == base(s[0]):
                c['ld_then_rev'] += 1
    elif m in ('rev16', 'rev32', 'rev64') and vec:
        c['rev_vec'] += 1
    elif m in STS and win:
        pm, pops, pd, ps = win[-1]
        if pm in ('rev', 'rev16', 'rev32') and pd and pd[0] in 'wx' and d and base(pd) == base(d):
            c['rev_then_st'] += 1
    if m in ('ldrsw', 'ldrsh', 'ldrsb', 'ldursw', 'ldursh', 'ldursb', 'smulh', 'umulh', 'fjcvtzs', 'sli', 'sri',
             'ror', 'extr', 'cls', 'clz', 'rbit', 'bsl', 'bit', 'bif', 'ushl', 'sshl', 'urshl', 'srshl', 'ext',
             'csel', 'csinc', 'csinv', 'csneg', 'cset', 'csetm', 'cinc', 'cinv', 'cneg', 'ccmp', 'ccmn', 'fcsel',
             'xtn', 'xtn2', 'shrn', 'shrn2', 'sqxtn', 'uqxtn', 'sqxtun', 'rshrn', 'tbl', 'tbx', 'blr', 'ret', 'br',
             'fcvtl', 'fcvtl2', 'fcvtn', 'fcvtn2', 'frecpe', 'frsqrte', 'frecps', 'frsqrts', 'frecpx', 'fsqrt',
             'fdiv', 'sdiv', 'udiv', 'fnmul', 'fnmadd', 'fnmsub', 'fmadd', 'fmsub', 'fmla', 'fmls', 'fabd',
             'facgt', 'facge', 'fcvtzs', 'fcvtzu', 'scvtf', 'ucvtf', 'fcvtas', 'fcvtau', 'fcvtms', 'fcvtmu',
             'fcvtns', 'fcvtnu', 'fcvtps', 'fcvtpu', 'frinta', 'frinti', 'frintm', 'frintn', 'frintp', 'frintx',
             'frintz', 'fabs', 'fneg', 'fcvt', 'fmov', 'ins', 'umov', 'smov', 'dup', 'movi', 'mvni', 'fmul',
             'fadd', 'fsub', 'fcmp', 'fcmpe', 'fccmp', 'fccmpe', 'cnt', 'addv', 'umaxv', 'uminv', 'smaxv', 'sminv',
             'fmaxv', 'fminv', 'fmaxnmv', 'fminnmv', 'fmax', 'fmin', 'fmaxnm', 'fminnm', 'fmaxp', 'fminp',
             'fmaxnmp', 'fminnmp', 'addp', 'neg', 'abs', 'sqabs', 'sqneg', 'not', 'mvn', 'orn', 'bic', 'eon',
             'and', 'orr', 'eor', 'ushr', 'sshr', 'shl', 'sshr', 'usra', 'ssra', 'ursra', 'srsra', 'urshr',
             'srshr', 'uqshl', 'sqshl', 'sqshlu', 'uqadd', 'sqadd', 'uqsub', 'sqsub', 'uhadd', 'shadd', 'urhadd',
             'srhadd', 'uhsub', 'shsub', 'umax', 'umin', 'smax', 'smin', 'uabd', 'sabd', 'uaba', 'saba', 'mul',
             'mla', 'mls', 'zip1', 'zip2', 'uzp1', 'uzp2', 'trn1', 'trn2', 'fcmeq', 'fcmgt', 'fcmge', 'fcmlt',
             'fcmle', 'cmeq', 'cmhi', 'cmhs', 'cmgt', 'cmge', 'cmtst', 'cmlt', 'cmle', 'dc', 'ic', 'dmb', 'dsb',
             'isb', 'mrs', 'msr', 'madd', 'msub', 'smaddl', 'umaddl', 'smsubl', 'umsubl', 'smull', 'umull',
             'ldar', 'ldarb', 'ldarh', 'stlr', 'stlrb', 'stlrh', 'ldaxr', 'ldxr', 'stlxr', 'stxr', 'ldapr',
             'ldaxp', 'ldxp', 'stlxp', 'stxp', 'cas', 'casa', 'casl', 'casal', 'ldadd', 'ldadda', 'ldaddl',
             'ldaddal', 'swp', 'swpa', 'swpl', 'swpal', 'bl', 'b', 'cbz', 'cbnz', 'tbz', 'tbnz', 'ld1r', 'ld1',
             'st1', 'ld2', 'st2', 'ld3', 'st3', 'ld4', 'st4', 'sha256h', 'sha256h2', 'sha256su0', 'sha256su1',
             'sha1c', 'sha1p', 'sha1m', 'sha1h', 'sha1su0', 'sha1su1', 'aese', 'aesd', 'aesmc', 'aesimc',
             'sqdmulh', 'sqrdmulh', 'rev', 'rev16', 'rev32', 'rev64', 'sxtb', 'sxth', 'sxtw', 'uxtb', 'uxth',
             'ubfx', 'sbfx', 'ubfiz', 'sbfiz', 'bfi', 'bfxil', 'bfc', 'lsl', 'lsr', 'asr', 'tst', 'ands',
             'cmp', 'cmn', 'adds', 'subs', 'adc', 'adcs', 'sbc', 'sbcs', 'ngc', 'ngcs', 'adrp', 'adr', 'movz',
             'movk', 'movn', 'mov', 'nop', 'hint', 'yield', 'wfe', 'sev', 'svc', 'brk', 'hlt', 'udf', 'pacia',
             'autia', 'paciasp', 'autiasp', 'bti', 'xpaclri', 'ldraa', 'ldrab', 'sdot', 'udot', 'smlal', 'umlal',
             'smlal2', 'umlal2', 'sadalp', 'uadalp', 'saddlp', 'uaddlp', 'uaddlv', 'saddlv', 'pmull', 'pmull2',
             'pmul', 'smlsl', 'umlsl', 'sqdmull', 'sqdmlal', 'usdot', 'sxtl', 'sxtl2', 'uxtl', 'uxtl2', 'sshll',
             'sshll2', 'ushll', 'ushll2'):
        tag = m + ('_v' if vec else '')
        c[tag] += 1
    if m == 'addp':
        # vector addp has three vector operands; scalar addp d0, v1.2d has two
        n = len(regs(ops))
        c['addp_vec' if n == 3 else 'addp_scalar'] += 1
        if n == 3 and len(win) >= 2 and win[-1][0] == 'addp' and win[-2][0] == 'addp':
            c['addp_run3'] += 1
        if n == 3 and win and win[-1][0] == 'and' and VEC.search(win[-1][1]):
            c['and_then_addp'] += 1
    if m in REDUCE:
        for i in range(1, min(4, len(win) + 1)):
            if win[-i][0] in CMPS:
                c['cmp_then_reduce'] += 1
                c['cmp_then_' + m] += 1
                break
    if m == 'shrn' and win:
        for i in range(1, min(4, len(win) + 1)):
            if win[-i][0] in CMPS:
                c['cmp_then_shrn'] += 1
                break
    if m in WIDEN:
        arr = ''
        mm = re.match(r'v\d+\.(\w+)', ops)
        if mm:
            arr = mm.group(1)
        c['widen_' + m + '_' + arr] += 1
        c['widen_all'] += 1
    if m in DOT:
        c['dot_mac_' + m] += 1
        c['dot_mac_all'] += 1
    if m == 'fmov' and win:
        # popcount idiom: fmov d, x ; cnt v.8b ; addv b ; fmov w, s
        if d and d[0] == 'w' and s and s[0][0] == 's':
            seq = [w[0] for w in list(win)[-3:]]
            if seq == ['fmov', 'cnt', 'addv'] or (len(win) >= 3 and 'cnt' in seq and 'addv' in seq):
                c['popcnt_idiom'] += 1
    if m == 'cnt' and vec and win and win[-1][0] == 'fmov':
        c['fmov_then_cnt'] += 1
    if m in ('uaddlp', 'uaddlv', 'addv') and win and win[-1][0] == 'cnt':
        c['cnt_then_' + m] += 1
    if m == 'orr' and vec and len(win) >= 2:
        a, b = win[-1], win[-2]
        if {a[0], b[0]} == {'ushr', 'shl'} or {a[0], b[0]} == {'usra', 'shl'}:
            c['vrot_idiom'] += 1
    if m == 'usra' and vec and win and win[-1][0] == 'shl':
        c['vrot_idiom_usra'] += 1
    if m in ('fmax', 'fmin', 'fmaxnm', 'fminnm', 'fcmeq', 'fcmgt', 'fcmge', 'fcmlt', 'fcmle', 'fabs', 'fneg',
             'fsqrt', 'fdiv', 'fmul', 'fadd', 'fsub', 'fmadd', 'fmsub', 'fnmadd', 'fnmsub', 'fcvt', 'fcvtzs',
             'fcvtzu', 'scvtf', 'ucvtf', 'frecpe', 'frsqrte', 'fcmp', 'fcmpe'):
        if d and d[0] in 'sdh' and not vec:
            c['scalarfp_' + m] += 1
            c['scalarfp_all'] += 1
    if m == 'fmov' and d and s:
        if d[0] == 'd' and s[0][0] == 'x':
            c['fmov_d_from_x'] += 1
        elif d[0] == 'x' and s[0][0] == 'd':
            c['fmov_x_from_d'] += 1
        elif d[0] == 'w' and s[0][0] == 's':
            c['fmov_w_from_s'] += 1
        elif d[0] == 's' and s[0][0] == 'w':
            c['fmov_s_from_w'] += 1
    if m == 'dc' and 'zva' in ops:
        c['dc_zva'] += 1
    if m in ('dmb', 'dsb', 'isb'):
        c[m + '_' + (ops.strip() or 'sy')] += 1
    if m == 'mrs' and s is not None:
        c['mrs_' + ops.split(',')[-1].strip().lower()] += 1
    if m == 'msr':
        c['msr_' + ops.split(',')[0].strip().lower()] += 1
    if m == 'neg' and vec:
        c['neg_vec'] += 1
    if m == 'cnt' and vec:
        c['cnt_vec'] += 1
    if m == 'bl':
        mm = SYM.search(ops)
        if mm:
            sym = mm.group(1).split('+')[0].replace('@plt', '')
            bl[sym] += 1
            if sym in TF:
                c['bl_tf_' + sym] += 1
                c['bl_tf_all'] += 1
            elif LIBML.match(sym) and sym.endswith('l'):
                c['bl_libml_' + sym] += 1
                c['bl_libml_all'] += 1
    win.append((m, ops, d, s))

for k in sorted(c):
    print(f'{k}\t{c[k]}')
print('--- bl targets, top 120')
for sym, n in bl.most_common(120):
    print(f'BL\t{sym}\t{n}')
