#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sample POWERarm's live profile stats (ProfileStats) into a CSV time series.

POWERarm publishes per-thread counters in /dev/shm/powerarm-<pid>-stats when
POWERARM_PROFILESTATS=1 (FEXCore/include/FEXCore/Utils/SHMStats.h, stats
version 2; MangoHud's FEX panel reads the same layout). The segment is
unlinked at exit, so this polls it while the guest runs.

  shmstats.py [--interval S] [--out FILE.csv] -- COMMAND [ARGS...]
  shmstats.py [--interval S] [--out FILE.csv] --pid PID

With a command it sets POWERARM_PROFILESTATS=1, runs it, and samples the
child's segment (launch the guest directly or through binfmt: either way the
child's pid is the emulator's). Each row is cumulative for the process. Threads
that exit keep their last counts. Times are seconds of host timebase.
A summary goes to stderr at the end. The emulator's own exit line
("[POWERarm JIT] ...") reports JIT totals over live threads only.
"""
import argparse
import csv
import os
import struct
import subprocess
import sys
import time

HEADER = struct.Struct("<BBH48sIII")  # Version, app_type, ThreadStatsSize, fex_version, Head, Size, Pad
FIELDS = ("jit_time", "signal_time", "sigbus", "smc", "softfloat",
          "cache_miss", "cache_rlock_time", "cache_wlock_time", "jit_count")
SLOT = struct.Struct("<II" + "Q" * len(FIELDS))  # Next, TID, counters
TIMES = {"jit_time", "signal_time", "cache_rlock_time", "cache_wlock_time"}


def timebase_hz():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("timebase"):
                    return int(line.split(":")[1])
    except OSError:
        pass
    return 0


def read_segment(pid):
    try:
        with open(f"/dev/shm/powerarm-{pid}-stats", "rb") as f:
            data = f.read()
    except OSError:
        return None
    if len(data) < HEADER.size:
        return None
    version, _app, slot_size, _ver, _head, size, _pad = HEADER.unpack_from(data)
    if version != 2 or slot_size < SLOT.size:
        raise SystemExit(f"shmstats: unsupported stats version {version} / slot size {slot_size}")
    slots = {}
    for off in range(HEADER.size, min(size, len(data)) - slot_size + 1, slot_size):
        vals = SLOT.unpack_from(data, off)
        if vals[1]:
            slots[off] = (vals[1], vals[2:])
    return slots


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--interval", type=float, default=0.5)
    ap.add_argument("--out", help="CSV path (default: stdout)")
    ap.add_argument("--pid", type=int)
    ap.add_argument("command", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.command[1:] if a.command[:1] == ["--"] else a.command
    if bool(cmd) == bool(a.pid):
        ap.error("give either --pid or a command")

    hz = timebase_hz() or 1
    proc = None
    if cmd:
        proc = subprocess.Popen(cmd, env=dict(os.environ, POWERARM_PROFILESTATS="1"))
        pid = proc.pid
    else:
        pid = a.pid

    out = open(a.out, "w", newline="") if a.out else sys.stdout
    w = csv.writer(out)
    w.writerow(["t_s", "threads", *FIELDS, "jit_cores"])
    live = {}                     # slot offset -> (tid, counters)
    retired = [0] * len(FIELDS)   # counts of threads that have exited
    t0 = time.monotonic()
    prev_t, prev_jit = 0.0, 0
    last_row = None
    seen = False
    while True:
        slots = read_segment(pid)
        now = time.monotonic() - t0
        if slots is not None:
            seen = True
            for off, (tid, vals) in live.items():
                if slots.get(off, (None,))[0] != tid:
                    retired = [r + v for r, v in zip(retired, vals)]
            live = slots
            tot = [r + sum(v[i] for _, v in live.values()) for i, r in enumerate(retired)]
            row = [len(live)] + [t / hz if f in TIMES else t for f, t in zip(FIELDS, tot)]
            jit_s = row[1]
            cores = (jit_s - prev_jit) / (now - prev_t) if now > prev_t else 0.0
            prev_t, prev_jit = now, jit_s
            last_row = [f"{now:.3f}", *[f"{x:.6f}" if isinstance(x, float) else x for x in row], f"{cores:.3f}"]
            w.writerow(last_row)
            out.flush()
        if proc is not None:
            if proc.poll() is not None:
                break
        elif slots is None and seen:
            break
        # Poll fast until the segment appears: it is created at startup and short runs end quickly.
        time.sleep(a.interval if seen else min(a.interval, 0.01))

    wall = time.monotonic() - t0
    if last_row is None:
        print(f"shmstats: no /dev/shm/powerarm-{pid}-stats seen (ProfileStats off, or not a POWERarm process)", file=sys.stderr)
    else:
        named = dict(zip(["t_s", "threads", *FIELDS], last_row))
        print(f"shmstats: pid {pid} wall {wall:.1f}s | JIT {float(named['jit_time']):.2f}s "
              f"({float(named['jit_time']) / wall * 100:.1f}% of wall) over {named['jit_count']} blocks | "
              f"signal {float(named['signal_time']):.2f}s | SIGBUS {named['sigbus']} SMC {named['smc']} "
              f"softfloat {named['softfloat']} cache-miss {named['cache_miss']} (last sample, "
              f"{a.interval}s before exit at most)", file=sys.stderr)
    return proc.returncode if proc is not None else 0


if __name__ == "__main__":
    sys.exit(main())
