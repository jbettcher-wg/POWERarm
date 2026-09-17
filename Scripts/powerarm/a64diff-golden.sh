#!/bin/sh
# SPDX-License-Identifier: MIT
# A64Diff golden side.  Run on the aarch64 reference machine (Raspberry Pi 5).
#
#   a64diff-golden.sh [--seed N] [--scale N] [--out DIR] [-j N] [--selftest] [--push USER@HOST:DIR]
#                     [--rootfs NAME=DIR]...
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
#
# Rootfs jobs (programs/*.jobs blocks with "rootfs NAME") run natively through
# a64diff-rootfs-exec.sh.  The prototype rootfs "minimal-debian" is assembled
# here and shipped inside the bundle.  --rootfs NAME=DIR adds an external
# rootfs (e.g. the Arch Linux ARM sysroot): it is not copied, only its content
# hash is recorded, and the compare side must find the same tree.
set -eu
umask 022

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../unittests/A64Diff" && pwd)
seed=1
scale=2
out=${A64DIFF_OUT:-$HOME/a64diff}
jobs=$(nproc)
selftest=0
push=
extra_rootfs=

while [ $# -gt 0 ]; do
  case $1 in
    --seed) seed=$2; shift 2 ;;
    --scale) scale=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    -j) jobs=$2; shift 2 ;;
    --selftest) selftest=1; shift ;;
    --push) push=$2; shift 2 ;;
    --rootfs) extra_rootfs="$extra_rootfs $2"; shift 2 ;;
    *) echo "usage: $0 [--seed N] [--scale N] [--out DIR] [-j N] [--selftest] [--push USER@HOST:DIR]" >&2; exit 2 ;;
  esac
done

[ "$(uname -m)" = aarch64 ] || { echo "a64diff-golden: run this on the aarch64 golden machine" >&2; exit 2; }

t0=$(date +%s)
step() { echo "[$(( $(date +%s) - t0 ))s] $*"; }

format=$(sed -n 's/^FORMAT_VERSION = //p' "$src/gen/a64gen.py")
hash=$(cd "$src" && find gen tool programs rootfs -type f ! -name '*.pyc' | LC_ALL=C sort | xargs sha256sum | sha256sum | cut -c1-12)
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
cp "$src/rootfs/a64diff-rootfs.py" "$root/src/"

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

step "rootfs"
mkdir -p "$root/rootfs"
record_rootfs() { # NAME DIR LOCATION
  python3 "$src/rootfs/a64diff-rootfs.py" hash "$2" --contents-out "$root/rootfs/$1.contents" > "$work/rootfs-$1.hash"
  { echo "name $1"; cat "$work/rootfs-$1.hash"; echo "location $3"; } > "$root/rootfs/$1.id"
  echo "  $1: $(grep content-hash "$root/rootfs/$1.id") ($3)"
}
A64DIFF_BUSYBOX=$root/programs/bin/busybox sh "$src/rootfs/mkrootfs-minimal.sh" "$root/rootfs/minimal-debian"
record_rootfs minimal-debian "$root/rootfs/minimal-debian" bundle
# Isolation check: through the native runner, the rootfs's multiarch directory
# must hold exactly what the builder put there (the golden machine's own
# /usr/lib/aarch64-linux-gnu has hundreds of files).  A runner that leaked the
# host tree would make every rootfs golden meaningless.
iso=$(mktemp -d)
seen=$("$here/a64diff-rootfs-exec.sh" "$root/rootfs/minimal-debian" "$iso" -- /usr/bin/busybox ls /usr/lib/aarch64-linux-gnu | tr '\n' ' ')
rm -rf "$iso"
[ "$seen" = "ld-linux-aarch64.so.1 libc.so.6 " ] ||
  { echo "a64diff-golden: rootfs runner is not isolated: sees '$seen'" >&2; exit 1; }
echo "  isolation: runner sees only the rootfs ($seen)"
rootfs_args="--rootfs minimal-debian=$root/rootfs/minimal-debian"
for spec in $extra_rootfs; do
  n=${spec%%=*} d=$(cd "${spec#*=}" && pwd -P)
  record_rootfs "$n" "$d" external
  rootfs_args="$rootfs_args --rootfs $n=$d"
done
rootfs_args="$rootfs_args --rootfs-exec $here/a64diff-rootfs-exec.sh"

suites=$(cd "$root/programs" && ls *.jobs | sed 's/\.jobs$//')
for suite in $suites; do
  step "golden run: $suite"
  # shellcheck disable=SC2086
  "$tool" run --jobs "$root/programs/$suite.jobs" --root "$root" --out "$root/golden-$suite" -j "$jobs" --timeout 600 $rootfs_args
  rm -rf "$root/golden-$suite/"*.cwd
done

step "determinism: second native run must match the goldens"
rm -rf "$work/native"
"$tool" run --manifest "$root/manifest.tsv" --root "$root" --out "$work/native/insn" -j "$jobs" --timeout 10 >/dev/null
"$tool" compare --manifest "$root/manifest.tsv" --golden "$root/golden" --actual "$work/native/insn" \
  --report "$work/native/insn.report" --max-detail 5 > "$work/native/insn.log" || true
grep -v '^CONTROL-FIRED' "$work/native/insn.log"
grep -q 'RESULT=PASS$' "$work/native/insn.log" || { echo "a64diff-golden: native re-run does not match the goldens" >&2; exit 1; }
for suite in $suites; do
  # shellcheck disable=SC2086
  "$tool" run --jobs "$root/programs/$suite.jobs" --root "$root" --out "$work/native/$suite" -j "$jobs" --timeout 600 $rootfs_args >/dev/null
  "$tool" pcompare --jobs "$root/programs/$suite.jobs" --golden "$root/golden-$suite" --actual "$work/native/$suite" \
    --report "$work/native/$suite.report" --max-detail 5 > "$work/native/$suite.log" || true
  grep -v '^CONTROL-FIRED' "$work/native/$suite.log"
  grep -q 'RESULT=PASS$' "$work/native/$suite.log" || { echo "a64diff-golden: native $suite re-run does not match the goldens" >&2; exit 1; }
done

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
  echo "job-suites $(echo $suites)"
  for f in "$root"/rootfs/*.id; do echo "rootfs $(sed -n 's/^name //p' "$f") $(sed -n 's/^content-hash //p' "$f") $(sed -n 's/^location //p' "$f")"; done
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
