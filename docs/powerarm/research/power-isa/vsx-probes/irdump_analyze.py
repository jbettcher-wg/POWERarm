import re,sys,collections
VSXCLEAN=set("NZCVSelectV FCmp VMov VNot VFAbs VFNeg VFSqrt VAnd VAndn VOrn VOr VXor VFAdd VFSub VFMul VFDiv VBSL VFMLA VFMLS VFNMLA VFNMLS VCastFromGPR VLoadTwoGPRs A64FloatToGPR A64FloatFromGPR A64VecIntToFloat A64FToF A64FArith A64FMinMax A64FMulAdd".split())
FPOPS=set("A64FArith A64FMulAdd A64FMinMax VFAdd VFSub VFMul VFDiv VFMLA VFMLS A64FloatToGPR A64FloatFromGPR FCmp A64FToF VFSqrt".split())
OP=re.compile(r'^\s*(?:\(%\d+\s+i\d+\)\s*|%\d+(?:\(([A-Za-z0-9]+)\))?\s*i(\d+)\s*=\s*)?([A-Za-z0-9_]+)\s*(.*)$')
LOADLB=re.compile(r'^#0x(1[0-9a-f]), FPR'); LBDST=re.compile(r'^V(1[6-9]|2[0-9]|3[01])$'); LBARG=re.compile(r'\bV(1[6-9]|2[0-9]|3[01])\b')
blocks=[]; cur=None
for line in open(sys.argv[1], errors='replace'):
    if 'BeginBlock' in line: cur=[]; blocks.append(cur); continue
    if cur is None: continue
    s=line.strip()
    if s: cur.append(s)
def stats(sel):
    st=collections.Counter(); dist=[]; opmix=collections.Counter(); cons=collections.Counter()
    for b in sel:
        ops=[]; g=0
        for s in b:
            m=OP.match(s)
            if not m: continue
            dst,sz,op,args=m.groups()
            if op=='GuestOpcode': g+=1; continue
            ops.append((g,dst,sz,op,args))
        seen={}
        for idx,(g,dst,sz,op,args) in enumerate(ops):
            opmix[op]+=1
            if op=='VMov' and sz in ('8','16'): st['VMov_8_16']+=1
            if op=='LoadRegister':
                ml=LOADLB.match(args)
                if not ml: continue
                st['loads']+=1; r=int(ml.group(1),16)
                if r in seen:
                    st['reusable']+=1; d=g-seen[r]; dist.append(d)
                    if d<=8: st['reusable_w8']+=1
                seen[r]=g
                # trace consumers via the physical dest register until it is redefined
                if dst and dst[0]=='v':
                    pat=re.compile(r'\b'+dst+r'\b'); kinds=set()
                    for g2,d2,s2,o2,a2 in ops[idx+1:]:
                        if pat.search(a2): kinds.add('clean' if o2 in VSXCLEAN or o2=='StoreRegister' else 'vmx'); cons[o2]+=1
                        if d2==dst: break
                    if kinds=={'clean'}: st['cons_allclean']+=1
                    elif kinds=={'vmx'}: st['cons_allvmx']+=1
                    elif kinds: st['cons_mixed']+=1
                    else: st['cons_none']+=1
            elif op=='StoreRegister' and dst and LBDST.match(dst):
                st['stores']+=1; seen[int(dst[1:])]=g
            else:
                if LBARG.search(args): st['coalesced_reads']+=1
                if dst and LBDST.match(dst): st['hoisted_writes']+=1
    return st,dist,opmix,cons
def lb(b): return any(('LoadRegister #0x1' in s and 'FPR' in s) or re.search(r'\(V(1[6-9]|2[0-9]|3[01])\)',s) or LBARG.search(s) for s in b)
def fp(b): return any(re.search(r'= ('+'|'.join(FPOPS)+r')\b',s) for s in b)
for name,sel in (("all low-bank blocks",[b for b in blocks if lb(b)]),("FP low-bank blocks (contain a scalar/lane FP op)",[b for b in blocks if lb(b) and fp(b)])):
    st,dist,opmix,cons=stats(sel)
    print(f"== {name}: n={len(sel)}")
    print("  ", dict(st))
    if dist: dist.sort(); print(f"   reuse distance guest insns: median={dist[len(dist)//2]} p90={dist[int(len(dist)*.9)]}")
    vec=[k for k in opmix if k.startswith('V') or k.startswith('A64F') or k in ('FCmp','NZCVSelectV','Vector_FToF','Vector_FToI')]
    print("   top vector ops:", ', '.join(f"{k}={opmix[k]}" for k in sorted(vec,key=lambda k:-opmix[k])[:18]))
    print("   consumers of non-coalesced loads:", ', '.join(f"{k}={v}" for k,v in cons.most_common(14)))
