#!/bin/bash
# Run every fptime kernel under perf stat, pinned, and emit a CSV:
#   kernel,ns_per_iter,cycles_per_iter,insns_per_iter,flush_per_iter,mtfpscr_stall_per_iter,brmpred_per_iter
# env: BIN=<fptime binary> CPU=92 PERF=<perf wrapper> OUT=<csv> OPS=<ops per chain>
set -u
BIN=${BIN:?}; CPU=${CPU:-92}; PERF=${PERF:-perf}; OUT=${OUT:?}; OPS=${OPS:-20000000}
echo "kernel,ns_per_iter,cycles_per_iter,insns_per_iter,flush_per_iter,mtfpscr_per_iter,brmpred_per_iter,iters" > "$OUT"
for k in $("$BIN" list); do
  # The binary runs 5 timed repetitions + warm-up; perf counts all of them, so
  # counts are divided by the total iterations executed (5 + 1/8 passes).
  line=$(taskset -c "$CPU" "$PERF" stat -x, -e cycles:u,instructions:u,pm_flush:u,pm_cmplu_stall_mtfpscr:u,pm_br_mpred_cmpl:u -o /tmp/fptime.$$.stat "$BIN" "$k" lat "$OPS" 2>/dev/null)
  ns=$(echo "$line" | sed -n 's/.*ns_per_iter=\([0-9.]*\).*/\1/p')
  iters=$(echo "$line" | sed -n 's/.*iters=\([0-9]*\).*/\1/p')
  total=$(awk -v i="$iters" 'BEGIN{printf "%.0f", i*5.125}')
  cyc=$(awk -F, '$3=="cycles:u"{print $1}' /tmp/fptime.$$.stat)
  ins=$(awk -F, '$3=="instructions:u"{print $1}' /tmp/fptime.$$.stat)
  fl=$(awk -F, '$3=="pm_flush:u"{print $1}' /tmp/fptime.$$.stat)
  mt=$(awk -F, '$3=="pm_cmplu_stall_mtfpscr:u"{print $1}' /tmp/fptime.$$.stat)
  br=$(awk -F, '$3=="pm_br_mpred_cmpl:u"{print $1}' /tmp/fptime.$$.stat)
  awk -v k="$k" -v ns="$ns" -v t="$total" -v c="$cyc" -v i="$ins" -v f="$fl" -v m="$mt" -v b="$br" -v it="$iters" \
    'BEGIN{printf "%s,%s,%.2f,%.2f,%.4f,%.3f,%.4f,%s\n", k, ns, c/t, i/t, f/t, m/t, b/t, it}' >> "$OUT"
done
rm -f /tmp/fptime.$$.stat
