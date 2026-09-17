#!/bin/sh
# Native AArch64 timings (reference: Raspberry Pi 5). Sequential, one pinned core.
# Usage: run-pi.sh [csv]   env: CPU=3 RUNS=3 REPS=3 BENCHES="crc32 sha256 bst vm sort"
# The governor is left as found (ondemand on the reference Pi); its state is recorded
# before and after, and one untimed warm-up run brings the core to speed first.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
CPU=${CPU:-3}
RUNS=${RUNS:-3}
REPS=${REPS:-3}
BENCHES=${BENCHES:-"crc32 sha256 bst vm sort"}
csv=${1:-$here/results/pi.csv}
bin=$here/out/aarch64
mkdir -p "$(dirname "$csv")"
fq=/sys/devices/system/cpu/cpu$CPU/cpufreq
freq() { echo "# cpufreq cpu$CPU $1: governor=$(cat $fq/scaling_governor) cur=$(cat $fq/scaling_cur_freq) min=$(cat $fq/scaling_min_freq) max=$(cat $fq/scaling_max_freq)"; }
{
  echo "# host=$(uname -n) kernel=$(uname -r) date=$(date -Iseconds) cpu=$CPU runs=$RUNS reps=$REPS"
  echo "# load: $(cat /proc/loadavg)"
  freq before
  echo "host,config,bench,run,rep,scale,ns,wall_ns,sum,parity"
} > "$csv"
# Warm-up: bring the pinned core out of its idle frequency.
taskset -c "$CPU" "$bin/sha256" >/dev/null
freq after-warmup >> "$csv"
for run in $(seq 1 "$RUNS"); do
  for b in $BENCHES; do
    t0=$(date +%s%N)
    outp=$(taskset -c "$CPU" "$bin/$b" "" "$REPS")
    t1=$(date +%s%N)
    echo "$outp" | awk -v run="$run" -v wall=$((t1 - t0)) -v host=pi5 '
      function kv(k,  i) { for (i = 1; i <= NF; i++) if (index($i, k "=") == 1) return substr($i, length(k) + 2); return "" }
      / rep=/ { printf "%s,native-aarch64,%s,%s,%s,%s,%s,%s,%s,\n", host, kv("bench"), run, kv("rep"), kv("scale"), kv("ns"), wall, kv("sum") }
      /parity=/ { printf "%s,native-aarch64,%s,%s,summary,%s,%s,%s,%s,%s\n", host, kv("bench"), run, kv("scale"), kv("total_ns"), wall, kv("sum"), kv("parity") }' >> "$csv"
  done
done
freq after >> "$csv"
echo "# load after: $(cat /proc/loadavg)" >> "$csv"
echo "wrote $csv"
