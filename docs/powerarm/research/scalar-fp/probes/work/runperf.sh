#!/bin/bash
# Profile the scalar-FP workloads under POWERarm and natively on the POWER9.
#   env: EMU=<POWERarm> WORK=<dir with aarch64 binaries> NATIVE=<dir with -p9 binaries>
#        OUT=<log dir> CPU=88 NODE=0 PERF=<perf with libpfm reachable>
# Each perf stat pass uses at most four programmable events so nothing is
# multiplexed (POWER9 has four programmable counters plus fixed ones).
set -u
EMU=${EMU:?}; WORK=${WORK:?}; NATIVE=${NATIVE:?}; OUT=${OUT:?}; CPU=${CPU:-88}; NODE=${NODE:-0}
PERF=${PERF:-perf}
mkdir -p "$OUT"
pin() { numactl --membind="$NODE" taskset -c "$CPU" "$@"; }
busy() { # node-0 busy % over 1 s, from /proc/stat, for the CPUs in $1
  local a b; a=$(grep -E "^cpu($1) " /proc/stat | awk '{u+=$2+$3+$4;t+=$2+$3+$4+$5+$6+$7+$8} END{print u, t}')
  sleep 1; b=$(grep -E "^cpu($1) " /proc/stat | awk '{u+=$2+$3+$4;t+=$2+$3+$4+$5+$6+$7+$8} END{print u, t}')
  echo "$a $b" | awk '{printf "%.1f", ($3-$1)*100/($4-$2+0.001)}'
}
GROUPS_="cycles,instructions,branches,branch-misses
pm_cmplu_stall,pm_cmplu_stall_st_fwd,pm_cmplu_stall_lhs,pm_flush
pm_cmplu_stall_exec_unit,pm_cmplu_stall_dp,pm_cmplu_stall_vdp,pm_cmplu_stall_fxu
pm_cmplu_stall_dplong,pm_cmplu_stall_vdplong,pm_cmplu_stall_mtfpscr,pm_flush_mpred
pm_br_mpred_cmpl,pm_cmplu_stall_bru,pm_cmplu_stall_lsu,pm_cmplu_stall_dcache_miss
pm_cmplu_stall_ntc_flush,pm_cmplu_stall_flush_any_thread,pm_vsu_fsqrt_fdiv,pm_cmplu_stall_other_cmpl"
declare -A SCALE=([nbody]=2000000 [lu]=40 [fmix32]=300 [cvtmix]=150 [cmpsel]=2000)
for w in nbody lu fmix32 cvtmix cmpsel; do
  sc=${SCALE[$w]}
  echo "== $w busy=$(busy "$CPU|$((CPU+1))|$((CPU+2))|$((CPU+3))")%" | tee -a "$OUT/summary.txt"
  for cfg in emu native; do
    if [ $cfg = emu ]; then cmd=(env POWERARM_HOSTPAGEMODE=force "$EMU" "$WORK/$w" "$sc" 1); else cmd=("$NATIVE/$w-p9" "$sc" 1); fi
    pin "${cmd[@]}" > /dev/null 2>&1   # warm-up
    # plain timing, 3 process runs, 2 reps each
    : > "$OUT/$w.$cfg.time"
    for r in 1 2 3; do
      if [ $cfg = emu ]; then pin env POWERARM_HOSTPAGEMODE=force "$EMU" "$WORK/$w" "$sc" 2 2>&1 | grep -E "ns=|parity" >> "$OUT/$w.$cfg.time"
      else pin "$NATIVE/$w-p9" "$sc" 2 2>&1 | grep -E "ns=|parity" >> "$OUT/$w.$cfg.time"; fi
    done
    echo "$GROUPS_" | while read -r ev; do
      if [ $cfg = emu ]; then pin "$PERF" stat -x, -e "$ev" -o "$OUT/$w.$cfg.stat" --append env POWERARM_HOSTPAGEMODE=force "$EMU" "$WORK/$w" "$sc" 2 > /dev/null 2>&1
      else pin "$PERF" stat -x, -e "$ev" -o "$OUT/$w.$cfg.stat" --append "$NATIVE/$w-p9" "$sc" 2 > /dev/null 2>&1; fi
    done
  done
  # profile the emulated run with the JIT map (user space only)
  pin env POWERARM_HOSTPAGEMODE=force POWERARM_BLOCKJITNAMING=1 "$PERF" record -q -F 4999 -e cycles:u -o "$OUT/$w.perf.data" -- "$EMU" "$WORK/$w" "$sc" 2 > /dev/null 2>&1
  "$PERF" report -q -i "$OUT/$w.perf.data" --sort dso,sym --percent-limit 0.5 --stdio 2>/dev/null > "$OUT/$w.report"
  "$PERF" report -q -i "$OUT/$w.perf.data" --sort dso --stdio 2>/dev/null > "$OUT/$w.report-dso"
  cp /tmp/perf-*.map "$OUT/" 2>/dev/null; rm -f /tmp/perf-*.map
done
echo done >> "$OUT/summary.txt"
