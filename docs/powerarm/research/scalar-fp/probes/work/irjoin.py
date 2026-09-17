import sys, subprocess, re, collections
binf, csvf = sys.argv[1], sys.argv[2]
dis = {}
for line in subprocess.run(["objdump","-d","--no-show-raw-insn",binf],capture_output=True,text=True).stdout.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)", line)
    if m: dis[int(m.group(1),16)] = (m.group(2), m.group(3))
agg = collections.defaultdict(lambda: [0,0,0])  # count, sum ir, sum vector ir
fpre = re.compile(r"^(f[a-z]+|scvtf|ucvtf|fcvt[a-z]*)$")
for line in open(csvf):
    pc, n, nv = line.strip().split(",")
    pc = int(pc,16); n=int(n); nv=int(nv)
    if pc not in dis: continue
    mn, ops = dis[pc]
    key = mn
    if fpre.match(mn):
        # width from the first operand register letter
        m = re.match(r"([sdhwx])\d+", ops)
        key = "%s.%s" % (mn, m.group(1) if m else "?")
    a = agg[key]; a[0]+=1; a[1]+=n; a[2]+=nv
rows = sorted(agg.items(), key=lambda kv: -kv[1][1])
print("mnemonic,count,avg_ir_ops,avg_vector_ir_ops")
for k,(c,s,sv) in rows:
    print("%s,%d,%.1f,%.1f" % (k,c,s/c,sv/c))
