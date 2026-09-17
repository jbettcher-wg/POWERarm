#!/bin/bash
# emu_stat.sh <emulator> <benchdir> <cpu> <outfile> bench...
# Completion-stall breakdown of POWERarm vs native ppc64le, one PMC group per run,
# grouped so that each event lands on a distinct POWER9 PMC (no multiplexing).
emu=$1; bd=$2; cpu=$3; out=$4; shift 4
PERF=${PERF:-/mnt/arch/usr/bin/perf}
export LD_LIBRARY_PATH=$HOME/.local/lib/perfshim
REPS=${REPS:-3}
groups="cycles:u,instructions:u,pm_cmplu_stall:u,pm_cmplu_stall_exec_unit:u,pm_cmplu_stall_other_cmpl:u,pm_cmplu_stall_st_fwd:u
pm_ict_noslot_cyc:u,pm_cmplu_stall_fxu:u,pm_disp_held:u,pm_cmplu_stall_fxlong:u
pm_cmplu_stall_lsu_fin:u,pm_cmplu_stall_lhs:u,pm_flush_mpred:u,pm_cmplu_stall_load_finish:u
pm_cmplu_stall_lrq_other:u,pm_cmplu_stall_store_finish:u,pm_cmplu_stall_spec_finish:u,pm_cmplu_stall_bru:u
pm_disp_held_issq_full:u,pm_cmplu_stall_lsu:u,pm_disp_held_hb_full:u,pm_ict_noslot_br_mpred:u
pm_1plus_ppc_cmpl:u,pm_cmplu_stall_ntc_flush:u,pm_cmplu_stall_dp:u,pm_br_mpred_cmpl:u
pm_run_cyc:u,pm_cmplu_stall_dcache_miss:u,pm_flush_lsu:u,pm_ld_miss_l1:u
pm_cmplu_stall_flush_any_thread:u,pm_cmplu_stall_store_data:u,pm_lsu_flush_lhl_shl:u,pm_lsu_flush_lhs:u
pm_cmplu_stall_any_sync:u,pm_st_fwd:u,pm_br_pred_ccache:u,pm_br_mpred_ccache:u
pm_ict_noslot_disp_held_issq:u,pm_br_pred_lstack:u,pm_br_mpred_lstack:u,pm_ict_noslot_disp_held_hb_full:u"
echo "# emu=$emu cpu=$cpu reps=$REPS date=$(date -Is) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$out"
for b in "$@"; do
  for cfg in powerarm native; do
    if [ $cfg = powerarm ]; then cmd="env POWERARM_HOSTPAGEMODE=force $emu $bd/out/aarch64/$b"; else cmd="$bd/out/ppc64le-power9/$b"; fi
    echo "$groups" | while read -r g; do
      # shellcheck disable=SC2086
      $PERF stat -x, -e "$g" -- taskset -c $cpu $cmd "" $REPS 2>&1 >/dev/null | \
        awk -F, -v b=$b -v c=$cfg '$1 ~ /^[0-9]/ {printf "%s,%s,%s,%s,%s%%\n", b, c, $3, $1, $5}' >> "$out"
    done
    echo "$b $cfg done"
  done
done
