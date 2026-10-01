#!/bin/bash
# sweep2.sh <outfile> -- link stack at depth, and a finer indirect-site sweep
set -u
S=$(cd "$(dirname "$0")" && pwd)
P=$S/mitprobe
CPU=${CPU:-164}
OUT=${1:-$S/sweep2.txt}
REPS=${REPS:-7}
: > "$OUT"
echo "# host=$(hostname) cpu=$CPU date=$(date -Is) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT"

run() { local k=$1 sy=$2 u=$3 r=$4 p=$5; shift 5
  taskset -c $CPU "$P" "$k" "$sy" "$u" "$r" "$p" "$REPS" "$@" >> "$OUT" 2>&1; }

# lstackdeep: the syscall is taken at the bottom of a depth-D bl/blr chain.
for d in 1 2 4 8 16 24 32 48; do
  for sy in none vdso getppid; do
    run lstackdeep $sy 60000 1 $d 0x48ac 0x40a8
  done
done

# ccache: finer site sweep, 64-byte spacing, 512 sites max (32 KiB of ring)
for sites in 16 64 128 192 256 320 384 448 512; do
  r=$(( sites * 8 ))
  outer=$(( 4000000 / r )); [ $outer -lt 2000 ] && outer=2000; [ $outer -gt 30000 ] && outer=30000
  u=$(( outer * r ))
  for sy in none vdso getppid; do
    run ccache $sy $u $r $sites 0x40ac 0x40a4
    run ctrl   $sy $u $r $sites 0x40ac 0x3e054
  done
done
echo "DONE" >> "$OUT"
