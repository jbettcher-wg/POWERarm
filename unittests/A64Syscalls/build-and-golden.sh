#!/bin/sh
# SPDX-License-Identifier: MIT
# Build the A64Syscalls programs and capture their goldens. Run natively on
# aarch64 (the Raspberry Pi 5).
#
#   build-and-golden.sh OUTDIR [prog...]
#
# Builds every sys_*.c (or the named programs) with $CC -static -O2 -Wall
# -Werror into OUTDIR/bin, then runs each program three times with stdin from
# /dev/null: twice with stdout to a file and once through a pipe. It fails if
# a build fails, a run exits nonzero, the output doesn't end with "done", or
# the runs differ. Otherwise it writes golden/<prog>.txt (skipped with
# NO_GOLDEN=1). Keep OUTDIR off sshfs/NFS: the programs work in /tmp.
set -eu

[ $# -ge 1 ] || { sed -n '3,14p' "$0" >&2; exit 2; }
out=$1
shift
here=$(cd "$(dirname "$0")" && pwd)
cc=${CC:-gcc}
mkdir -p "$out/bin" "$out/runs"
out=$(cd "$out" && pwd)

if [ $# -gt 0 ]; then
  progs=$*
else
  progs=$(cd "$here" && ls sys_*.c | sed 's/\.c$//')
fi

fail=0
for p in $progs; do
  if ! $cc -static -O2 -Wall -Werror -o "$out/bin/$p" "$here/$p.c"; then
    echo "BUILD-FAIL $p"
    fail=1
    continue
  fi
  ok=1
  for run in 1 2 3; do
    r=$out/runs/$p.$run
    set +e
    if [ $run = 3 ]; then
      (cd /tmp && "$out/bin/$p" < /dev/null; echo $? > "$r.rc") | cat > "$r"
      rc=$(cat "$r.rc")
    else
      (cd /tmp && "$out/bin/$p" < /dev/null > "$r")
      rc=$?
    fi
    set -e
    if [ "$rc" != 0 ]; then
      echo "FAIL $p: run $run exited $rc"
      ok=0
    elif [ "$(tail -n 1 "$r")" != done ]; then
      echo "FAIL $p: run $run doesn't end with done"
      ok=0
    elif [ $run != 1 ] && ! cmp -s "$out/runs/$p.1" "$r"; then
      echo "FAIL $p: run $run differs from run 1"
      diff "$out/runs/$p.1" "$r" | head -n 10
      ok=0
    fi
  done
  if [ $ok = 1 ]; then
    if [ "${NO_GOLDEN:-0}" = 1 ]; then
      echo "OK   $p"
    else
      cp "$out/runs/$p.1" "$here/golden/$p.txt"
      echo "OK   $p -> golden/$p.txt"
    fi
  else
    fail=1
  fi
done
exit $fail
