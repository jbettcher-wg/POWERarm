import ctypes, os, sys, time, subprocess, re
emu, prog, offs = sys.argv[1], sys.argv[2], sys.argv[3:]
env = dict(os.environ, POWERARM_HOSTPAGEMODE="force", POWERARM_BLOCKJITNAMING="1")
p = subprocess.Popen(["taskset", "-c", "112", emu, prog, "4096", "3000000"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2.5)
# taskset execs the emulator, so p.pid is the emulator
libc = ctypes.CDLL(None, use_errno=True); libc.ptrace.restype = ctypes.c_long
if libc.ptrace(16, p.pid, None, None) != 0: sys.exit("attach failed errno %d" % ctypes.get_errno())
os.waitpid(p.pid, 0)
m = open("/tmp/perf-%d.map" % p.pid).read()
with open("/proc/%d/mem" % p.pid, "rb") as f:
  for off in offs:
    mm = re.search(r"^(0x[0-9a-f]+) ([0-9a-f]+) .*strloop\+%s " % off, m, re.M)
    if not mm: print("-- %s: not in map" % off); continue
    addr, size = int(mm.group(1), 16), int(mm.group(2), 16)
    f.seek(addr); data = f.read(size); open("/tmp/blk_%s.bin" % off, "wb").write(data)
    print("-- guest strloop+%s host %#x size %#x dumped" % (off, addr, size))
libc.ptrace(17, p.pid, None, None); p.kill()
