import sys, os, re, glob
# For every <pc>-post.ir file: per GuestOpcode marker, count the IR ops until the next marker.
# Output: guest_pc_hex,n_ir_ops,n_vector_ops
d = sys.argv[1]
out = []
for fn in glob.glob(os.path.join(d, "*-post.ir")):
    base = int(os.path.basename(fn).split("-")[0], 16)
    cur = None; n = 0; nv = 0
    for line in open(fn):
        m = re.search(r"GuestOpcode #0x([0-9a-f]+)", line)
        if m:
            if cur is not None: out.append((base + cur, n, nv))
            cur = int(m.group(1), 16); n = 0; nv = 0; continue
        if cur is None: continue
        s = line.strip()
        if not s or s.startswith("(%") and ("InlineConstant" in s or "EndBlock" in s or "BeginBlock" in s): 
            if "InlineConstant" in s: n += 0
            continue
        if "=" in s or s.startswith("(%"):
            n += 1
            if re.search(r"\bV[A-Z][A-Za-z0-9]*\b", s.split("=")[-1] if "=" in s else s): nv += 1
    if cur is not None: out.append((base + cur, n, nv))
for pc, n, nv in sorted(out): print("%x,%d,%d" % (pc, n, nv))
