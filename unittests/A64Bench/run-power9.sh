#!/bin/sh
# POWER9 timings: native ppc64le (-mcpu=power8/power9) and POWERarm on the AArch64 binaries.
# Usage: run-power9.sh [csv]
# env: EMU=<path to POWERarm>  CPU=88  NODE=0  RUNS=5  REPS=3  BENCHES="..."
#      CONFIGS="native-power8 native-power9 powerarm"  MAXBUSY=10 (percent, node's CPUs)
# Runs are interleaved (run index outermost) so drift lands on every config equally.
# A startup probe (scale=1, reps=1) measures process start + translation of the hot code.
# The native-power9 crc32 control is run first and last; compare the two to validate the run.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
EMU=${EMU:-$root/build-bench/Bin/POWERarm}
CPU=${CPU:-88}
NODE=${NODE:-0}
RUNS=${RUNS:-5}
REPS=${REPS:-3}
MAXBUSY=${MAXBUSY:-10}
BENCHES=${BENCHES:-"crc32 sha256 bst vm sort"}
CONFIGS=${CONFIGS:-"native-power8 native-power9 powerarm"}
csv=${1:-$here/results/power9.csv}
mkdir -p "$(dirname "$csv")"
nodecpus=$(cat /sys/devices/system/node/node$NODE/cpulist)
pin() {
  if command -v numactl >/dev/null; then numactl --membind="$NODE" taskset -c "$CPU" "$@"
  else taskset -c "$CPU" "$@"; fi
}
# Busy percentage across the node's CPUs over one second, from /proc/stat.
node_busy() {
  snap() { awk -v list="$nodecpus" '
    BEGIN { n = split(list, r, ","); for (i = 1; i <= n; i++) { if (split(r[i], ab, "-") == 1) ab[2] = ab[1]; for (c = ab[1]; c <= ab[2]; c++) want["cpu" c] = 1 } }
    ($1 in want) { t = 0; for (i = 2; i <= NF; i++) t += $i; idle += $5 + $6; tot += t }
    END { print tot, idle }' /proc/stat; }
  set -- $(snap); t0=$1 i0=$2
  sleep 1
  set -- $(snap)
  echo $(( (100 * (($1 - t0) - ($2 - i0))) / ($1 - t0 + 1) ))
}
wait_quiet() {
  tries=0
  while b=$(node_busy); [ "$b" -gt "$MAXBUSY" ]; do
    tries=$((tries + 1))
    echo "node$NODE busy ${b}% > ${MAXBUSY}%, waiting ($tries)" >&2
    [ $tries -ge 60 ] && { echo "giving up on a quiet node; recording busy=$b" >&2; break; }
    sleep 10
  done
  echo "$b"
}
fq=/sys/devices/system/cpu/cpu$CPU/cpufreq
freq() { echo "# cpufreq cpu$CPU $1: governor=$(cat $fq/scaling_governor) cur=$(cat $fq/scaling_cur_freq) max=$(cat $fq/scaling_max_freq)"; }
cmd_for() { # config bench -> command
  case $1 in
    native-power8) echo "$here/out/ppc64le-power8/$2" ;;
    native-power9) echo "$here/out/ppc64le-power9/$2" ;;
    powerarm) echo "env POWERARM_HOSTPAGEMODE=force $EMU $here/out/aarch64/$2" ;;
  esac
}
one() { # config bench run scale reps label
  cmd=$(cmd_for "$1" "$2")
  busy=$(wait_quiet)
  t0=$(date +%s%N)
  # shellcheck disable=SC2086
  outp=$(pin $cmd "$4" "$5" 2>/dev/null) || true
  t1=$(date +%s%N)
  echo "$outp" | awk -v cfg="$6" -v run="$3" -v wall=$((t1 - t0)) -v busy="$busy" -v host=power9 '
    function kv(k,  i) { for (i = 1; i <= NF; i++) if (index($i, k "=") == 1) return substr($i, length(k) + 2); return "" }
    / rep=/ { printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,,%s\n", host, cfg, kv("bench"), run, kv("rep"), kv("scale"), kv("ns"), wall, kv("sum"), busy }
    /parity=/ { printf "%s,%s,%s,%s,summary,%s,%s,%s,%s,%s,%s\n", host, cfg, kv("bench"), run, kv("scale"), kv("total_ns"), wall, kv("sum"), kv("parity"), busy; seen = 1 }
    END { if (!seen) printf "%s,%s,%s,%s,summary,,,%s,,DID_NOT_FINISH,%s\n", host, cfg, "'"$2"'", run, wall, busy }' >> "$csv"
}
{
  echo "# host=$(uname -n) kernel=$(uname -r) date=$(date -Iseconds) cpu=$CPU node=$NODE runs=$RUNS reps=$REPS"
  echo "# emulator=$EMU commit=$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "# smt: $(cat /sys/devices/system/cpu/smt/control 2>/dev/null) load: $(cat /proc/loadavg)"
  freq before
  echo "host,config,bench,run,rep,scale,ns,wall_ns,sum,parity,node_busy_pct"
} > "$csv"
taskset -c "$CPU" "$here/out/ppc64le-power9/sha256" >/dev/null # warm-up
one native-power9 crc32 0 "" "$REPS" control-start
for run in $(seq 1 "$RUNS"); do
  for b in $BENCHES; do
    for c in $CONFIGS; do
      one "$c" "$b" "$run" "" "$REPS" "$c"
    done
    one powerarm "$b" "$run" 1 1 powerarm-startup
    one native-power9 "$b" "$run" 1 1 native-power9-startup
  done
done
one native-power9 crc32 0 "" "$REPS" control-end
freq after >> "$csv"
echo "# load after: $(cat /proc/loadavg)" >> "$csv"
echo "wrote $csv"
