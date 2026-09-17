#!/usr/bin/env python3
"""dump_hot_block.py -- dump the host code of named JIT blocks from a running POWERarm.

Usage: dump_hot_block.py <emulator> <guest-binary> <cpu> <outprefix> <block-name> [block-name ...]

Runs the emulator as a child (so /proc/<pid>/mem is readable under
kernel.yama.ptrace_scope=1), waits for the perf map (POWERARM_BLOCKJITNAMING=1)
to contain the named blocks, reads their bytes, and writes <outprefix>.<name>.bin
plus an objdump listing. The block names are the guest symbols the JIT writes,
e.g. "out/aarch64/crc32+0x624".
"""
import os, sys, time, subprocess, signal

emu, guest, cpu, outprefix = sys.argv[1:5]
names = sys.argv[5:]
env = dict(os.environ, POWERARM_BLOCKJITNAMING="1", POWERARM_HOSTPAGEMODE="force")
child = subprocess.Popen(["taskset", "-c", cpu, emu, guest, "", "3"], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
# taskset execs the emulator in place, so the pid is the emulator's.
pid = child.pid
mapfile = f"/tmp/perf-{pid}.map"
found = {}
for _ in range(200):
    time.sleep(0.05)
    if not os.path.exists(mapfile):
        continue
    with open(mapfile) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            addr, size, name = int(parts[0], 16), int(parts[1], 16), " ".join(parts[2:])
            for want in names:
                if want in name:
                    found[want] = (addr, size)
    if len(found) == len(names):
        break
if not found:
    child.kill(); sys.exit("no named block found in " + mapfile)
with open(f"/proc/{pid}/mem", "rb") as mem:
    for want, (addr, size) in found.items():
        mem.seek(addr)
        data = mem.read(size)
        safe = want.replace("/", "_")
        binpath = f"{outprefix}.{safe}.bin"
        with open(binpath, "wb") as f:
            f.write(data)
        lst = subprocess.run(["objdump", "-D", "-b", "binary", "-m", "powerpc:common64", "-EL",
                              f"--adjust-vma={addr:#x}", binpath], capture_output=True, text=True).stdout
        with open(f"{outprefix}.{safe}.objdump", "w") as f:
            f.write(f"# block {want} host {addr:#x} size {size:#x}\n")
            f.write(lst)
        print(f"dumped {want}: host {addr:#x} size {size} -> {outprefix}.{safe}.objdump")
child.kill()
try:
    child.wait(timeout=5)
except Exception:
    pass
