#!/bin/bash
# sweep.sh <outfile>  -- raw lines from mitprobe across kernels/syscalls/R
set -u
S=$(cd "$(dirname "$0")" && pwd)
P=$S/mitprobe
CPU=${CPU:-120}
OUT=${1:-$S/sweep.txt}
REPS=${REPS:-7}
: > "$OUT"

echo "# host=$(hostname) cpu=$CPU date=$(date -Is) load=$(cut -d' ' -f1-3 /proc/loadavg) freq=$(cat /sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_cur_freq)" >> "$OUT"

run() { # kernel sys units R param [rawev...]
  local k=$1 sy=$2 u=$3 r=$4 p=$5; shift 5
  taskset -c $CPU "$P" "$k" "$sy" "$u" "$r" "$p" "$REPS" "$@" >> "$OUT" 2>&1
}

# ---- 1. probe: hot-block sweep x R sweep x syscall mode -------------------
for hot in 64 1024 4096 16384 65536; do
  for r in 8 32 128 512 2048 8192; do
    outer=$(( 6000000 / r )); [ $outer -lt 3000 ] && outer=3000; [ $outer -gt 40000 ] && outer=40000
    u=$(( outer * r ))
    for sy in none vdso getppid madvise; do
      run probe $sy $u $r $hot 0x3e054 0x40ac
    done
  done
done

# ---- 2. null: raw syscall cost -------------------------------------------
for r in 8 32 128 512 2048; do
  outer=$(( 6000000 / r )); [ $outer -lt 3000 ] && outer=3000; [ $outer -gt 40000 ] && outer=40000
  u=$(( outer * r ))
  for sy in none vdso getppid madvise mprotect dontneed; do
    run null $sy $u $r 1 0x3e054 0x40ac
  done
done

# ---- 3. ccache: monomorphic bctr sites ----------------------------------
for sites in 8 32 64 128 256; do
  for inner in 1 4 16 64; do
    r=$(( sites * inner ))
    outer=$(( 4000000 / r )); [ $outer -lt 2000 ] && outer=2000; [ $outer -gt 30000 ] && outer=30000
    u=$(( outer * r ))
    for sy in none getppid; do
      run ccache $sy $u $r $sites 0x40ac 0x40a4
      run ctrl   $sy $u $r $sites 0x40ac 0x3e054
    done
  done
done

# ---- 4. lstack: nested bl/blr depth 24 ----------------------------------
for r in 1 4 16 64 256; do
  outer=$(( 200000 / r )); [ $outer -lt 1000 ] && outer=1000; [ $outer -gt 20000 ] && outer=20000
  u=$(( outer * r ))
  for sy in none getppid; do
    run lstack $sy $u $r 1 0x48ac 0x40a8
  done
done
echo "DONE" >> "$OUT"
