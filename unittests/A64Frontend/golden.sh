#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Pi side: generate, build and run the A64Frontend tests natively.
#   golden.sh OUTDIR
# Writes OUTDIR/<test> binaries, OUTDIR/<test>.golden (stdout) and
# OUTDIR/<test>.rc (exit status). Run on an AArch64 Linux machine with gcc.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: golden.sh OUTDIR}
mkdir -p "$out"

python3 "$here/gen.py" "$out"
cp "$here"/*.S "$out"/

cd "$out"
for src in *.S; do
  [ "$src" = common.S ] && continue
  gcc -nostdlib -static -o "${src%.S}" "$src"
done
gcc -static -O2 -o hello "$here/hello.c"

for src in *.S; do
  [ "$src" = common.S ] && continue
  t=${src%.S}
  set +e
  "./$t" > "$t.golden" 2>/dev/null
  echo $? > "$t.rc"
  set -e
done
set +e
./hello > hello.golden 2>/dev/null
echo $? > hello.rc
set -e
echo "golden outputs written to $out"
