import re,sys,collections
OP=re.compile(r'^\s*(?:\(%\d+\s+i\d+\)\s*|%\d+(?:\(([A-Za-z0-9]+)\))?\s*i(\d+)\s*=\s*)?([A-Za-z0-9_]+)\s*(.*)$')
LBDST=re.compile(r'^V(1[6-9]|2[0-9]|3[01])$')
prod=collections.Counter(); lastdef={}; n=0
for line in open(sys.argv[1], errors='replace'):
    if 'BeginBlock' in line: lastdef={}; continue
    m=OP.match(line.strip())
    if not m: continue
    dst,sz,op,args=m.groups()
    if op=='StoreRegister' and dst and LBDST.match(dst):
        a=args.strip().split(',')[0].strip()
        prod[lastdef.get(a,'?')]+=1; n+=1
    if dst: lastdef[dst]=op
print(sys.argv[1].split('/')[-1], 'non-hoisted low-bank StoreRegister =', n, '; producers:', ', '.join(f"{k}={v}" for k,v in prod.most_common(12)))
