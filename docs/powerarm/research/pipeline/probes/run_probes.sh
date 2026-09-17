#!/bin/bash
# run_probes.sh <cpu> <iters> <outfile> [probe ...]   -- parity, wall time, then PMC groups
# Output: CSV lines  probe,metric,value,per_iter
cpu=$1; iters=$2; out=$3; shift 3
PERF=${PERF:-/mnt/arch/usr/bin/perf}
export LD_LIBRARY_PATH=$HOME/.local/lib/perfshim
here=$(cd "$(dirname "$0")" && pwd)
bin=$here/pipeprobe
probes="$@"; [ -z "$probes" ] && probes=$($bin list | awk '{print $1}')
G1="cycles:u,instructions:u,pm_cmplu_stall:u,pm_cmplu_stall_exec_unit:u,pm_flush_mpred:u,pm_cmplu_stall_st_fwd:u"
G2="pm_cmplu_stall_lhs:u,pm_br_mpred_cmpl:u,pm_br_pred_ccache:u,pm_br_mpred_ccache:u"
G3="pm_br_pred_lstack:u,pm_br_mpred_lstack:u,pm_cmplu_stall_bru:u,pm_cmplu_stall_lsu:u"
G4="pm_st_fwd:u,pm_flush_lsu:u,pm_lsu_flush_lhl_shl:u,pm_cmplu_stall_fxu:u"
echo "# host=$(hostname) cpu=$cpu iters=$iters date=$(date -Is) load=$(cut -d' ' -f1-3 /proc/loadavg) freq=$(cat /sys/devices/system/cpu/cpu$cpu/cpufreq/scaling_cur_freq)" >> "$out"
for p in $probes; do
  t=$(taskset -c $cpu $bin time $p $iters) || { echo "$p,PARITY_FAIL,,"; echo "$p,PARITY_FAIL,," >> "$out"; continue; }
  ns=$(echo "$t" | sed -n 's/.*ns\/iter=\([0-9.]*\).*/\1/p')
  echo "$p,ns_per_iter,$ns,$ns" >> "$out"
  for g in "$G1" "$G2" "$G3" "$G4"; do
    $PERF stat -x, -e "$g" -- taskset -c $cpu $bin once $p $iters 2>&1 >/dev/null | \
      awk -F, -v p=$p -v it=$iters '$3!="" && $1 ~ /^[0-9]/ {printf "%s,%s,%s,%.4f,%s%%\n", p, $3, $1, $1/it, $5}' >> "$out"
  done
  printf "%-22s %s\n" "$p" "$ns ns/iter"
done
