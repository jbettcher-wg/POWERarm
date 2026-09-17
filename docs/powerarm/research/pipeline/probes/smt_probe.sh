#!/bin/bash
# smt_probe.sh <cpu> <outfile> -- probes and POWERarm crc32 with 0/1/3 SMT siblings busy,
# plus a control with 3 spinners on a different core. cpu must be the first thread of a core.
cpu=$1; out=$2
PERF=${PERF:-/mnt/arch/usr/bin/perf}; export LD_LIBRARY_PATH=$HOME/.local/lib/perfshim
here=$(cd "$(dirname "$0")" && pwd)
E=$HOME/Development/POWERarm-wt/research-pipeline/build-rp/Bin/POWERarm
B=$HOME/Development/POWERarm-wt/research-pipeline/unittests/A64Bench
probes="A_indep8 A_chain1 S_ctx_rt1 S_ctx_crcshape D_bctr_rand4 D_bctr_rep8 B_r2_bl_blr B_bctr_next"
iters=4000000
pids=""
spin() { for c in "$@"; do taskset -c $c $here/spin & pids="$pids $!"; done; sleep 0.5; }
stop() { [ -n "$pids" ] && kill $pids 2>/dev/null; pids=""; sleep 0.3; }
echo "# cpu=$cpu date=$(date -Is) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$out"
for cfg in solo sib1 sib3 far3; do
  case $cfg in
    solo) ;;
    sib1) spin $((cpu+1)) ;;
    sib3) spin $((cpu+1)) $((cpu+2)) $((cpu+3)) ;;
    far3) spin $((cpu+9)) $((cpu+10)) $((cpu+11)) ;;
  esac
  for p in $probes; do
    r=$($PERF stat -x, -e cycles:u,instructions:u -- taskset -c $cpu $here/pipeprobe once $p $iters 2>&1 >/dev/null | awk -F, '$1 ~ /^[0-9]/ {printf "%s=%s ", $3, $1}')
    t=$(taskset -c $cpu $here/pipeprobe time $p $iters | sed -n 's/.*ns\/iter=\([0-9.]*\).*/\1/p')
    echo "$cfg,$p,$t,$r" >> "$out"
  done
  for b in crc32 vm; do
    r=$(POWERARM_HOSTPAGEMODE=force taskset -c $cpu $E $B/out/aarch64/$b "" 2 2>/dev/null | grep "rep=1" | sed -n 's/.*ns=\([0-9]*\).*/\1/p')
    echo "$cfg,powerarm_$b,$r," >> "$out"
    r=$(taskset -c $cpu $B/out/ppc64le-power9/$b "" 2 2>/dev/null | grep "rep=1" | sed -n 's/.*ns=\([0-9]*\).*/\1/p')
    echo "$cfg,native_$b,$r," >> "$out"
  done
  stop
  echo "$cfg done"
done
