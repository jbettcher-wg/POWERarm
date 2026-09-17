#!/bin/sh
# SPDX-License-Identifier: MIT
# A64Diff golden side.  Run on the aarch64 reference machine (Raspberry Pi 5).
#
#   a64diff-golden.sh [--seed N] [--scale N] [--out DIR] [-j N] [--selftest] [--push USER@HOST:DIR]
#
# Generates the instruction tests, builds them, runs them natively, builds the
# program corpus, captures all goldens, checks them, re-runs everything
# natively and requires a clean compare (tests that aren't deterministic would
# fail here), and packs a bundle for the compare side:
#
#   $OUT/a64diff-v<format>-s<seed>-x<scale>-<hash>/            bundle root
#   $OUT/a64diff-v<format>-s<seed>-x<scale>-<hash>.tar.gz      same, packed
#
# <hash> covers the generator, the tool and the program corpus sources, so a
# bundle name identifies exactly what produced it.  --selftest additionally
# runs the negative controls (a64diff-selftest.sh).  --push scps the tarball.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../unittests/A64Diff" && pwd)
seed=1
scale=2
out=${A64DIFF_OUT:-$HOME/a64diff}
jobs=$(nproc)
selftest=0
push=

while [ $# -gt 0 ]; do
  case $1 in
    --seed) seed=$2; shift 2 ;;
    --scale) scale=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    -j) jobs=$2; shift 2 ;;
    --selftest) selftest=1; shift ;;
    --push) push=$2; shift 2 ;;
    *) echo "usage: $0 [--seed N] [--scale N] [--out DIR] [-j N] [--selftest] [--push USER@HOST:DIR]" >&2; exit 2 ;;
  esac
done

[ "$(uname -m)" = aarch64 ] || { echo "a64diff-golden: run this on the aarch64 golden machine" >&2; exit 2; }

t0=$(date +%s)
step() { echo "[$(( $(date +%s) - t0 ))s] $*"; }

format=$(sed -n 's/^FORMAT_VERSION = //p' "$src/gen/a64gen.py")
hash=$(cd "$src" && find gen tool programs -type f ! -name '*.pyc' | LC_ALL=C sort | xargs sha256sum | sha256sum | cut -c1-12)
name=a64diff-v$format-s$seed-x$scale-$hash
root=$out/$name
work=$out/work-$name
mkdir -p "$out"
rm -rf "$root" "$work"
mkdir -p "$root/tests" "$root/src" "$work/obj"

step "bundle $name"
cc -O2 -Wall -o "$work/a64diff" "$src/tool/a64diff.c"
tool=$work/a64diff
cp "$src/tool/a64diff.c" "$src/tool/a64diff.h" "$src/tool/broken-runner.sh" "$root/src/"
cp -R "$src/vm" "$root/src/vm"

step "generate (seed $seed, scale $scale)"
python3 "$src/gen/a64gen.py" gen --seed "$seed" --scale "$scale" --out "$work"
cp "$work/manifest.pre.tsv" "$root/"

step "assemble and link $(wc -l < "$work/manifest.pre.tsv") tests (-j $jobs)"
cut -f1 "$work/manifest.pre.tsv" | xargs -P "$jobs" -n 64 sh -c '
  w=$1; r=$2; shift 2
  for t; do
    as -o "$w/obj/$t.o" "$w/src/$t.s" && ld -static -e _start -o "$r/tests/$t" "$w/obj/$t.o" || { echo "BUILD-FAIL $t" >&2; exit 255; }
  done' sh "$work" "$root"
python3 "$src/gen/a64gen.py" finalize --dir "$root"
rm "$root/manifest.pre.tsv"

# The broken runner and the disassembly addresses assume text is the first
# PT_LOAD at file offset 0, vaddr 0x400000.
first=$(head -n 3 "$root/manifest.tsv" | tail -n 1 | cut -f1)
readelf -lW "$root/tests/$first" | grep -m1 LOAD | grep -q '0x000000 0x0000000000400000' ||
  { echo "a64diff-golden: unexpected ELF layout in $first" >&2; exit 1; }

step "golden run (native)"
"$tool" run --manifest "$root/manifest.tsv" --root "$root" --out "$root/golden" -j "$jobs" --timeout 10
"$tool" check --manifest "$root/manifest.tsv" --golden "$root/golden"

step "program corpus"
sh "$src/programs/build-programs.sh" "$root"
"$tool" run --jobs "$root/programs/programs.jobs" --root "$root" --out "$root/golden-programs" -j "$jobs" --timeout 60
rm -rf "$root/golden-programs/"*.cwd

step "determinism: second native run must match the goldens"
rm -rf "$work/native"
"$tool" run --manifest "$root/manifest.tsv" --root "$root" --out "$work/native/insn" -j "$jobs" --timeout 10 >/dev/null
"$tool" compare --manifest "$root/manifest.tsv" --golden "$root/golden" --actual "$work/native/insn" \
  --report "$work/native/insn.report" --max-detail 5 > "$work/native/insn.log" || true
grep -v '^CONTROL-FIRED' "$work/native/insn.log"
grep -q 'RESULT=PASS$' "$work/native/insn.log" || { echo "a64diff-golden: native re-run does not match the goldens" >&2; exit 1; }
"$tool" run --jobs "$root/programs/programs.jobs" --root "$root" --out "$work/native/programs" -j "$jobs" --timeout 60 >/dev/null
"$tool" pcompare --jobs "$root/programs/programs.jobs" --golden "$root/golden-programs" --actual "$work/native/programs" \
  --report "$work/native/programs.report" --max-detail 5 > "$work/native/programs.log" || true
grep -v '^CONTROL-FIRED' "$work/native/programs.log"
grep -q 'RESULT=PASS$' "$work/native/programs.log" || { echo "a64diff-golden: native program re-run does not match the goldens" >&2; exit 1; }

{
  echo "name $name"
  echo "format $format"
  echo "seed $seed"
  echo "scale $scale"
  echo "source-hash $hash"
  echo "created $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "golden-host $(uname -srm)"
  echo "golden-cpu $(grep -m1 'CPU part' /proc/cpuinfo | awk '{print $NF}') $(grep -m1 Features /proc/cpuinfo | cut -d: -f2)"
  echo "golden-pagesize $(getconf PAGESIZE)"
  echo "tests $(grep -vc '^#' "$root/manifest.tsv")"
  echo "program-jobs $(grep -vc '^#' "$root/programs/programs.jobs")"
} > "$root/VERSION"

if [ "$selftest" = 1 ]; then
  step "selftest (negative controls)"
  sh "$here/a64diff-selftest.sh" "$root" "$tool"
fi

step "pack"
tar -C "$out" -czf "$out/$name.tar.gz" "$name"
ls -l "$out/$name.tar.gz"
if [ -n "$push" ]; then
  step "push to $push"
  scp -q "$out/$name.tar.gz" "$push/"
fi
step "done: $root"
