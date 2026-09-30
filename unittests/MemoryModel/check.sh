#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Formal check of POWERarm's memory-ordering lowering, by model rather than by
# hardware. unittests/A64Frontend/litmus.c can only see reordering POWER9
# actually performs, and POWER9 never showed store->store reordering there; this
# asks the architecture models instead, so it covers everything the Power ISA
# permits, including what a future core might do.
#
#   check.sh [MAP...]            default: current acqrel none
#   check.sh trailing            the rejected trailing-sync proposal (see trailing.known)
#
# 1. diy7 generates a family of AArch64 litmus tests: every shape up to 4 threads
#    and 6 edges (MP, SB, LB, R, S, 2+2W, IRIW, WRC, ISA2, RWC, WWC, Z6, ...)
#    containing at least one of the ordering edges POWERarm has to lower --
#    acquire, release and acquirePC accesses and the three DMB flavours -- plus a
#    second family (names suffixed "Lx") whose ordering edge is an LDXR..STXR
#    pair in each of its four annotations, for the native lwarx/stwcx. lowering.
# 2. herd7 with Arm's own aarch64.cat says which outcomes AArch64 forbids.
# 3. jingle7 rewrites each test into Power code through MAP.map, which is a
#    lowering written down as rules -- current.map is what the JIT emits today.
# 4. herd7 with ppc.cat says which outcomes that Power code allows.
#
# A test AArch64 forbids but the Power translation allows is a lowering bug:
# the emulator would let a guest observe something real hardware never shows.
# The opposite (AArch64 allows, Power forbids) is only a missed optimisation.
#
# none.map and acqrel.map are negative controls. They must report unsound
# tests, or the check has no teeth: none.map drops every fence, and acqrel.map
# is the C/C++ acquire/release mapping, which is too weak for AArch64's RCsc
# LDAR/STLR (a release followed by an acquire must stay ordered).
#
# Needs herdtools7 (herd7, diy7, jingle7) on PATH and its model library, found
# through HERDLIB (default ~/.local/share/herdtools7/herd).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
: "${HERDLIB:=$HOME/.local/share/herdtools7/herd}"
work=${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/powerarm-memmodel.XXXXXX")}
mkdir -p "$work/gen"

PO="PodWW PodWR PodRW PodRR"
# Pod edges annotated P (plain), L (release, STLR), A (acquire, LDAR) or
# Q (acquirePC, LDAPR) at their source and target.
ANN="PodWWPL PodWWLP PodWWLL PodWRLA PodWRLP PodWRPA PodWRLQ PodRRAP PodRRAA PodRRQP PodRRQA PodRRPA PodRWAP PodRWAL PodRWQP PodRWPL"
DMB=""
for k in SY LD ST; do for p in WW WR RW RR; do DMB="$DMB DMB.${k}d$p"; done; done

diy7 -arch AArch64 -size 6 -nprocs 4 -relax "$ANN $DMB" -safe "Rfe Fre Wse $PO" -o "$work/gen" >/dev/null

# Exclusive pairs. LxSx is diy7's read-modify-write edge; the two annotation
# letters are the load's and the store's, so these four are LDXR/STXR,
# LDAXR/STXR, LDXR/STLXR and LDAXR/STLXR. They go in -relax so every test in
# this family contains one, with the DMB edges and plain program order as the
# safe set around it (adding the annotated pods as well grows the family from
# 576 tests to 1454 and finds nothing the smaller one misses -- checked).
#
#   -ua 0   emit the pair straight-line instead of inside a retry loop. jingle7
#           cannot parse a label in a source pattern, and a single attempt is
#           the right thing to model anyway: see the comment in current.map.
#   -oneloc the pair's load and store are the same location, so some shapes are
#           single-location and diy7 otherwise rejects them.
#   -sufname keeps the two families' generated names apart -- both number their
#           shapes from 000, and they share one output directory.
LX="LxSxPP LxSxAP LxSxPL LxSxAL"
diy7 -oneloc -ua 0 -sufname Lx -arch AArch64 -size 5 -nprocs 4 -relax "$LX" \
     -safe "Rfe Fre Wse $PO $DMB" -o "$work/gen" >/dev/null
# IRIW with release (STLR) writers. The family above cannot produce these, because an
# IRIW writer has no Pod edge to annotate. SC-IRIW1 is the all-seq_cst IRIW that C++
# std::atomic compiles to on AArch64, and is the case that rejects trailing-sync.
n=0
for cyc in "RfeLA PodRRAA FreAL RfeLA PodRRAA FreAL" "RfeLA PodRRAP FrePL RfeLA PodRRAP FrePL" \
           "RfeLQ PodRRQP FrePL RfeLQ PodRRQP FrePL" "RfeLP DMB.LDdRR FrePL RfeLP DMB.LDdRR FrePL" \
           "RfeLQ PodRRQA FreAL RfeLQ PodRRQA FreAL"; do
  n=$((n + 1))
  # diyone7 -name writes <name>.litmus into the current directory itself (stdout
  # stays empty), so run it from inside the generated-test directory.
  # shellcheck disable=SC2086
  (cd "$work/gen" && diyone7 -arch AArch64 -name "SC-IRIW$n" $cyc >/dev/null 2>&1)
done
# Two hand-built controls for the divergence current.known calls CO-R: the R
# shape with a coherence edge and a strong barrier on the reader. They contain no
# exclusive at all -- the writer is a plain store -- and they are unsound under
# current.map, which is what proves that R011Lx's unsoundness belongs to the
# `DMB ST -> lwsync` and `STLR -> sync; stw` mappings and not to the reservation
# lowering that made it visible. diy7's own family never produces this cycle
# (two barrier edges in a 4-edge cycle), which is why it went unnoticed.
n=0
for cyc in "DMB.STdWW Coe DMB.SYdWR Fre" "DMB.STdWW Coe PodWRLA FreAP"; do
  n=$((n + 1))
  # shellcheck disable=SC2086
  (cd "$work/gen" && diyone7 -arch AArch64 -name "CO-R$n" $cyc >/dev/null 2>&1)
done
total=$(ls "$work/gen"/*.litmus | wc -l)

# name -> forbidden|allowed, one line per test.
verdicts() {
  herd7 -I "$HERDLIB" -model "$1" "$2"/*.litmus 2>/dev/null |
    awk '/^Observation/ { print $2, ($3 == "Never") ? "forbidden" : "allowed" }' | sort
}

verdicts aarch64.cat "$work/gen" >"$work/aarch64.txt"
echo "$total AArch64 tests; Arm's model forbids $(grep -c forbidden "$work/aarch64.txt") of them"

status=0
for m in ${*:-current acqrel none}; do
  out="$work/out-$m"
  mkdir -p "$out"
  jingle7 -I "$HERDLIB" -theme "$here/$m.map" -o "$out" "$work/gen"/*.litmus >/dev/null
  verdicts ppc.cat "$out" >"$work/$m.txt"
  join "$work/aarch64.txt" "$work/$m.txt" | awk '$2 == "forbidden" && $3 == "allowed" { print $1 }' >"$work/$m.unsound"
  n=$(wc -l <"$work/$m.unsound")
  printf '%-9s %4d translated, %3d UNSOUND\n' "$m" "$(wc -l <"$work/$m.txt")" "$n"
  for t in $(head -12 "$work/$m.unsound"); do
    f=$(grep -l "^AArch64 $t\$" "$work/gen"/*.litmus | head -1)
    printf '            %-26s %s\n' "$t" "$(sed -n 's/^Cycle=//p' "$f")"
  done
  [ "$n" -gt 12 ] && echo "            ... $((n - 12)) more in $work/$m.unsound"
  case $m in
  none | acqrel) [ "$n" -gt 0 ] || { echo "            control FAILED: expected unsound tests"; status=1; } ;;
  *)
    # A MAP.known file lists divergences accepted on purpose (see its comments).
    # Anything unsound and not listed there is a regression.
    known="$here/$m.known"
    new=$(grep -vxF -f "$( [ -f "$known" ] && grep -v '^#' "$known" | awk '{print $1}' >"$work/$m.k" && echo "$work/$m.k" || echo /dev/null)" "$work/$m.unsound" || true)
    if [ -n "$new" ]; then
      echo "            NEW unsound, not in $m.known:"; echo "$new" | sed 's/^/              /'; status=1
    elif [ "$n" -gt 0 ]; then
      echo "            all $n are known divergences ($m.known)"
    fi ;;
  esac
done
echo "work dir: $work"
exit $status
