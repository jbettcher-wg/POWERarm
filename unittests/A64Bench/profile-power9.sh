#!/bin/sh
# Profile and stall/miss comparison for selected workloads on POWER9.
# Usage: profile-power9.sh bench [bench...]    output: results/profile/<bench>.*
# env: EMU, CPU=88, NODE=0, PERF=perf (with libpfm reachable), REPS=3
#
# 1. rusage (user/sys CPU, faults) for POWERarm and native -mcpu=power9.
# 2. perf record (user space; kernel.perf_event_paranoid=2 hides kernel samples) with
#    POWERARM_BLOCKJITNAMING=1, which makes the JIT write /tmp/perf-<pid>.map so samples in
#    generated code resolve to guest block addresses.
# 3. perf stat, a few events per run (ungrouped; POWER9 PMC constraints make most groups
#    unschedulable, so perf may multiplex and scales counts; the % running is recorded).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
EMU=${EMU:-$root/build-bench/Bin/POWERarm}
CPU=${CPU:-88}
NODE=${NODE:-0}
REPS=${REPS:-3}
PERF=${PERF:-perf}
out=$here/results/profile
mkdir -p "$out"
pin() { numactl --membind="$NODE" taskset -c "$CPU" "$@"; }
groups="cycles:u,instructions:u,branches:u,branch-misses:u
pm_br_mpred_cmpl,pm_br_pred_ccache,pm_br_mpred_ccache
pm_br_pred_lstack,pm_br_mpred_lstack,pm_br_mpred_pcache
pm_cmplu_stall,pm_cmplu_stall_bru,pm_cmplu_stall_lhs
pm_cmplu_stall_store_data,pm_cmplu_stall_st_fwd,pm_cmplu_stall_exec_unit
pm_flush,pm_flush_mpred,pm_ld_miss_l1
pm_lsu_flush_lhs,pm_cmplu_stall_dmiss_l2l3,pm_cmplu_stall_fxu"
for b in "$@"; do
  emu="env POWERARM_HOSTPAGEMODE=force $EMU $here/out/aarch64/$b"
  nat="$here/out/ppc64le-power9/$b"
  {
    for cfg in powerarm native-power9; do
      [ $cfg = powerarm ] && cmd=$emu || cmd=$nat
      # shellcheck disable=SC2086
      python3 -c 'import resource, subprocess, sys
subprocess.run(sys.argv[2:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
r = resource.getrusage(resource.RUSAGE_CHILDREN)
print(f"rusage {sys.argv[1]} user_s={r.ru_utime:.3f} sys_s={r.ru_stime:.3f} minflt={r.ru_minflt} majflt={r.ru_majflt} nvcsw={r.ru_nvcsw} nivcsw={r.ru_nivcsw}")' \
        $cfg numactl --membind="$NODE" taskset -c "$CPU" $cmd "" "$REPS"
    done
  } > "$out/$b.rusage"
  # shellcheck disable=SC2086
  POWERARM_BLOCKJITNAMING=1 pin $PERF record -q -F 4999 -e cycles:u -o "$out/$b.perf.data" -- $emu "" "$REPS" > /dev/null 2>&1
  $PERF report -q -i "$out/$b.perf.data" --sort dso,sym --percent-limit 0.3 --stdio 2>/dev/null > "$out/$b.report"
  $PERF report -q -i "$out/$b.perf.data" --sort dso --stdio 2>/dev/null > "$out/$b.report-dso"
  : > "$out/$b.stat"
  rm -f "$out/$b.stat.tmp"
  echo "$groups" | while read -r g; do
    for cfg in powerarm native-power9; do
      [ $cfg = powerarm ] && cmd=$emu || cmd=$nat
      # shellcheck disable=SC2086
      pin $PERF stat -x, -o "$out/$b.stat.tmp" -e "$g" -- $cmd "" "$REPS" > /dev/null 2>&1
      grep -v '^#' "$out/$b.stat.tmp" | grep . | sed "s/^/$cfg,/" >> "$out/$b.stat"
    done
  done
  rm -f "$out/$b.stat.tmp"
  echo "profiled $b -> $out/$b.*"
done
