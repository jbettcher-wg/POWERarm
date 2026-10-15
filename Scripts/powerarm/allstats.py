#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Aggregate POWERarm ProfileStats across EVERY emulator process a command spawns.

shmstats.py samples ONE pid and writes a time series. That is the wrong
instrument for anything multi-process, and the case that forced this one is
Chrome: it puts the V8 work in a renderer child, every child is its own
emulator process with its own /dev/shm/powerarm-<pid>-stats, and the launcher
process does almost no guest work. Sampling the launcher reports nearly
nothing and looks like a result.

  allstats.py -- COMMAND [ARGS...]

Sets POWERARM_PROFILESTATS=1, runs the command, polls for new segments while it
runs, and prints one total per counter to stderr with the process and thread
counts it summed over. Only segments that did not exist at startup are counted,
so an unrelated POWERarm process on the machine -- a gate run, another desktop
app -- cannot contribute to the total.

Segments are unlinked at exit, so a process that starts and finishes entirely
between two polls is missed. The poll is 0.2s; a short-lived child is the known
blind spot, and the printed process count is how you notice.

Counters are cumulative per process and threads that exit keep their last
counts, exactly as in shmstats.py. Times are seconds of host timebase.
"""
import glob
import os
import struct
import subprocess
import sys
import time

HEADER = struct.Struct("<BBH48sIII")
FIELDS = ("jit_time", "signal_time", "sigbus", "smc", "softfloat",
          "cache_miss", "cache_rlock_time", "cache_wlock_time", "jit_count",
          "vma_lock_time", "vma_queries")
SLOT = struct.Struct("<II" + "Q" * len(FIELDS))


def read_segment(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if len(data) < HEADER.size:
        return None
    version, _app, slot_size, _ver, _head, size, _pad = HEADER.unpack_from(data)
    if version != 2 or slot_size < SLOT.size:
        return None
    totals = [0] * len(FIELDS)
    nthreads = 0
    for off in range(HEADER.size, min(size, len(data)) - slot_size + 1, slot_size):
        vals = SLOT.unpack_from(data, off)
        if vals[1]:
            nthreads += 1
            for i, v in enumerate(vals[2:]):
                totals[i] += v
    return totals, nthreads


def main():
    argv = sys.argv[1:]
    if argv[:1] == ["--"]:
        argv = argv[1:]
    if not argv:
        sys.exit("usage: allstats.py -- COMMAND [ARGS...]")

    preexisting = set(glob.glob("/dev/shm/powerarm-*-stats"))
    env = dict(os.environ)
    env["POWERARM_PROFILESTATS"] = "1"

    per_pid = {}
    t0 = time.time()
    proc = subprocess.Popen(argv, env=env)
    while True:
        for path in glob.glob("/dev/shm/powerarm-*-stats"):
            if path in preexisting:
                continue
            got = read_segment(path)
            if got:
                per_pid[path] = got
        if proc.poll() is not None:
            break
        time.sleep(0.2)
    # One last sweep for anything still alive.
    for path in glob.glob("/dev/shm/powerarm-*-stats"):
        if path in preexisting:
            continue
        got = read_segment(path)
        if got:
            per_pid[path] = got
    wall = time.time() - t0

    agg = [0] * len(FIELDS)
    threads = 0
    for totals, nthreads in per_pid.values():
        for i, v in enumerate(totals):
            agg[i] += v
        threads += nthreads

    print(f"--- allstats: rc={proc.returncode} wall={wall:.1f}s "
          f"procs={len(per_pid)} threads={threads}", file=sys.stderr)
    for name, v in zip(FIELDS, agg):
        print(f"{name} {v}", file=sys.stderr)
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
