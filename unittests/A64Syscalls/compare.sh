#!/usr/bin/env bash
# Run the A64Syscalls binaries under an emulator (or natively) and diff each
# against golden/<prog>.txt.
#
# Usage: compare.sh "<emulator command prefix>" <bindir> [prog...]
#   prefix  e.g. "build-syscalls/Bin/POWERarm"; pass "" to run natively.
#   bindir  directory holding the static sys_* binaries (from
#           build-and-golden.sh, i.e. <OUTDIR>/bin).
#   prog    optional subset (default: every golden/*.txt).
# Env:  RESULTS (dir for outputs, default a fresh mktemp dir),
#       RUN_TIMEOUT (seconds, default 300),
#       XFAIL (space-separated programs whose failure is expected; default
#              none).  XFAIL failures don't fail the run.
set -u

if [ $# -lt 2 ]; then
	sed -n '2,14p' "$0" >&2
	exit 2
fi
prefix=$1
bindir=$(cd "$2" && pwd) || exit 2
shift 2
here=$(cd "$(dirname "$0")" && pwd)
results=${RESULTS:-$(mktemp -d "${TMPDIR:-/tmp}/a64sys-results.XXXXXX")}
mkdir -p "$results"
timeout_s=${RUN_TIMEOUT:-300}
xfail=" ${XFAIL-} "

if [ $# -gt 0 ]; then
	progs=("$@")
else
	progs=()
	for g in "$here"/golden/sys_*.txt; do
		progs+=("$(basename "$g" .txt)")
	done
fi

pass=0 failn=0 xfailn=0 xpass=0
failed=()
for prog in "${progs[@]}"; do
	golden="$here/golden/$prog.txt"
	outf="$results/$prog.txt"
	if [ ! -f "$golden" ]; then
		echo "FAIL  $prog (no golden)"
		failn=$((failn + 1)); failed+=("$prog")
		continue
	fi
	if [ ! -x "$bindir/$prog" ]; then
		echo "FAIL  $prog (no binary in $bindir)"
		failn=$((failn + 1)); failed+=("$prog")
		continue
	fi
	rc=0
	# shellcheck disable=SC2086  # prefix is intentionally word-split
	timeout "$timeout_s" $prefix "$bindir/$prog" </dev/null >"$outf" 2>"$outf.stderr" || rc=$?
	why=""
	if ! cmp -s "$golden" "$outf"; then
		diff -u "$golden" "$outf" >"$outf.diff"
		why="output differs ($(grep -c '^[-+][^-+]' "$outf.diff") changed lines)"
	fi
	if [ "$rc" != 0 ]; then
		[ "$rc" = 124 ] && why="${why:+$why; }timeout" || why="${why:+$why; }exit status $rc"
	fi
	case $xfail in *" $prog "*) isx=1 ;; *) isx=0 ;; esac
	if [ -z "$why" ]; then
		if [ "$isx" = 1 ]; then
			echo "XPASS $prog"; xpass=$((xpass + 1))
		else
			echo "PASS  $prog"; pass=$((pass + 1))
		fi
	elif [ "$isx" = 1 ]; then
		echo "XFAIL $prog: $why"; xfailn=$((xfailn + 1))
	else
		echo "FAIL  $prog: $why"
		[ -f "$outf.diff" ] && head -n 40 "$outf.diff" | sed 's/^/      /'
		failn=$((failn + 1)); failed+=("$prog")
	fi
done

echo "summary: $pass passed, $failn failed, $xfailn xfail, $xpass xpass (outputs in $results)"
[ "$failn" = 0 ] || { echo "failed: ${failed[*]}"; exit 1; }
exit 0
