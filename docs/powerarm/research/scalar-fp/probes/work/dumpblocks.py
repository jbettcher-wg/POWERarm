# gdb -batch -ex "set disable-randomization on" -x dumpblocks.py --args POWERarm <guest> ...
# with env POWERARM_BLOCKJITNAMING=1 DUMP_BLOCKS="nbody+0x624 nbody+0x700" DUMP_DIR=<dir> DUMP_SECS=4
#
# Runs the emulator for DUMP_SECS seconds, interrupts it, reads the JIT's
# /tmp/perf-<pid>.map, and dumps the host code of every block whose name is
# listed in DUMP_BLOCKS (guest symbol+offset as perf reports it) to
# DUMP_DIR/<name>.bin, plus a listing of block sizes. Disassemble with
# objdump -D -b binary -m powerpc:common64 -EL -M power9 <file>.
import gdb, os

blocks = os.environ.get("DUMP_BLOCKS", "").split()
outdir = os.environ.get("DUMP_DIR", ".")
secs = float(os.environ.get("DUMP_SECS", "4"))
os.makedirs(outdir, exist_ok=True)

gdb.execute("handle SIGINT stop print nopass")
gdb.execute("starti")
pid = gdb.selected_inferior().pid
# a shell-side timer delivers SIGINT to exactly this inferior pid
os.system("(sleep %s; kill -INT %d) > /dev/null 2>&1 &" % (secs, pid))
try:
    gdb.execute("continue")
except gdb.error as e:
    print("continue:", e)
mapfile = "/tmp/perf-%d.map" % pid
entries = []
with open(mapfile) as f:
    for line in f:
        parts = line.rstrip("\n").split(" ", 2)
        if len(parts) < 3: continue
        start, size, name = int(parts[0], 16), int(parts[1], 16), parts[2]
        entries.append((start, size, name))
print("map entries:", len(entries))
inf = gdb.selected_inferior()
with open(os.path.join(outdir, "blocks.txt"), "w") as lst:
    for start, size, name in entries:
        base_name = name.split(" (")[0]
        if base_name in blocks:
            data = inf.read_memory(start, size).tobytes()
            fn = os.path.join(outdir, os.path.basename(base_name).replace("+", "_") + ".bin")
            with open(fn, "wb") as o: o.write(data)
            lst.write("%s %x %d %s\n" % (name, start, size, fn))
            print("dumped", name, hex(start), size, fn)
gdb.execute("kill")
