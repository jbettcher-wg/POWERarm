#!/bin/bash
# SPDX-License-Identifier: MIT
# A64Diff compare side.  Run on the POWER9 host.
#
#   a64diff-run.sh 64k|4k-kvm POWERARM_BINARY [options]
#
#   --bundle PATH       bundle .tar.gz or directory from a64diff-golden.sh
#                       (default: newest in $A64DIFF_BUNDLES, ~/a64diff/bundles)
#   --work DIR          scratch (default: ~/.cache/a64diff)
#   --time-limit SEC    wall-clock limit for the whole run (default 1800)
#   --timeout SEC       per-test limit (default 30)
#   -j N                parallel tests (64k default 32; 4k-kvm default = guest cpus)
#   --skip CLASSES      comma list of instruction classes not to run
#   --env KEY=VALUE     pass to the emulator (repeatable), e.g. POWERARM_HOSTPAGEMODE=force
#   --suites LIST       insn,programs (default both)
#   4k-kvm only:
#   --kernel IMAGE      default /boot/vmlinuz-linux-power9 (the host's 4K kernel)
#   --cpus N --mem SIZE guest size (default 8, 4G)
#
# 64k runs the suites on the bare host.  4k-kvm builds an initramfs with the
# emulator, its shared libraries, a static a64diff and the bundle, boots the
# 4K kernel under KVM, lets the guest run and compare everything, and powers
# it off; the console log is kept and its reports extracted.
#
# Exit: 0 all required tests pass and every control fired; 1 required failures;
# 2 harness error or a control did not fire; 124 time limit.
set -euo pipefail

usage() { sed -n '3,25p' "$0" >&2; exit 2; }
[ $# -ge 2 ] || usage
mode=$1
emu=$(readlink -f "$2")
shift 2
case $mode in 64k|4k-kvm) ;; *) usage ;; esac
[ -x "$emu" ] || { echo "a64diff-run: $emu is not executable" >&2; exit 2; }

bundle=
work=${A64DIFF_WORK:-$HOME/.cache/a64diff}
limit=1800
timeout=30
jobs=
skip=
suites=insn,programs
kernel=/boot/vmlinuz-linux-power9
cpus=8
mem=4G
envs=()
while [ $# -gt 0 ]; do
  case $1 in
    --bundle) bundle=$2; shift 2 ;;
    --work) work=$2; shift 2 ;;
    --time-limit) limit=$2; shift 2 ;;
    --timeout) timeout=$2; shift 2 ;;
    -j) jobs=$2; shift 2 ;;
    --skip) skip=$2; shift 2 ;;
    --env) envs+=("$2"); shift 2 ;;
    --suites) suites=$2; shift 2 ;;
    --kernel) kernel=$2; shift 2 ;;
    --cpus) cpus=$2; shift 2 ;;
    --mem) mem=$2; shift 2 ;;
    *) usage ;;
  esac
done

t0=$(date +%s)
step() { echo "[$(( $(date +%s) - t0 ))s] $*"; }

if [ -z "$bundle" ]; then
  bundle=$(ls -t "${A64DIFF_BUNDLES:-$HOME/a64diff/bundles}"/a64diff-v*.tar.gz 2>/dev/null | head -n 1 || true)
  [ -n "$bundle" ] || { echo "a64diff-run: no bundle given and none in ${A64DIFF_BUNDLES:-$HOME/a64diff/bundles}" >&2; exit 2; }
fi
mkdir -p "$work"
if [ -d "$bundle" ]; then
  root=$(readlink -f "$bundle")
else
  name=$(basename "$bundle" .tar.gz)
  root=$work/$name
  if [ ! -f "$root/.unpacked" ] || [ "$bundle" -nt "$root/.unpacked" ]; then
    step "unpack $bundle"
    rm -rf "$root"
    tar -C "$work" -xzf "$bundle"
    touch "$root/.unpacked"
  fi
fi
[ -f "$root/manifest.tsv" ] || { echo "a64diff-run: $root is not an a64diff bundle" >&2; exit 2; }
name=$(basename "$root")
res=$work/results/$name-$mode-$(date +%Y%m%d-%H%M%S)
mkdir -p "$res"
step "bundle $name ($(sed -n 's/^tests //p' "$root/VERSION") tests), mode $mode, emulator $emu"
step "results in $res"

# Build the tool from this checkout when it's there (so comparator fixes apply
# to existing bundles; the manifest format check still guards the record
# layout), otherwise from the copy inside the bundle.
here=$(cd "$(dirname "$0")" && pwd)
tsrc=$here/../../unittests/A64Diff/tool
isrc=$here/../../unittests/A64Diff/vm
[ -f "$tsrc/a64diff.c" ] || { tsrc=$root/src; isrc=$root/src/vm; }
tooldir=$work/tool-$(cat "$tsrc/a64diff.c" "$tsrc/a64diff.h" "$isrc/a64diff-init.c" | sha256sum | cut -c1-12)
if [ ! -x "$tooldir/init" ]; then
  mkdir -p "$tooldir"
  gcc -O2 -static -o "$tooldir/a64diff" "$tsrc/a64diff.c"
  gcc -O2 -static -o "$tooldir/init" "$isrc/a64diff-init.c"
fi
tool=$tooldir/a64diff

summarize() { # LOG
  grep -E '^(CONTROL-FAIL|A64DIFF (INSN|PROGRAMS) SUMMARY)' "$1" || echo "(no summary in $1)"
}

worst() { # combine exit codes: 2 beats 1 beats 0
  local w=0
  for c in "$@"; do
    if [ "$c" -ge 2 ] || [ "$c" -lt 0 ]; then w=2; elif [ "$c" = 1 ] && [ $w = 0 ]; then w=1; fi
  done
  echo $w
}

if [ "$mode" = 64k ]; then
  ps=$(getconf PAGESIZE)
  [ "$ps" = 65536 ] || { echo "a64diff-run: 64k mode but the host page size is $ps" >&2; exit 2; }
  jobs=${jobs:-32}
  for e in "${envs[@]+"${envs[@]}"}"; do export "${e?}"; done
  # One wall-clock budget for both suites: each gets what is left of it.
  end=$((t0 + limit - 15))
  left() { local l=$((end - $(date +%s))); [ "$l" -gt 1 ] || l=1; echo "$l"; }
  rc_i=0 rc_p=0
  if [[ $suites == *insn* ]]; then
    step "instruction suite (-j $jobs)"
    set +e
    nice -n 10 "$tool" run --manifest "$root/manifest.tsv" --root "$root" --out "$res/insn" -j "$jobs" \
      --timeout "$timeout" --deadline "$(left)" --skip "$skip" -- "$emu" | grep -v '^a64diff run: [0-9]'
    "$tool" compare --manifest "$root/manifest.tsv" --golden "$root/golden" --actual "$res/insn" \
      --report "$res/insn.report" --max-detail 5 --skip "$skip" > "$res/insn.log" 2>&1
    rc_i=$?
    set -e
    grep -v '^CONTROL-FIRED' "$res/insn.log"
  fi
  if [[ $suites == *programs* ]]; then
    step "program suite"
    set +e
    nice -n 10 "$tool" run --jobs "$root/programs/programs.jobs" --root "$root" --out "$res/programs" -j "$jobs" \
      --timeout "$timeout" --deadline "$(left)" -- "$emu"
    "$tool" pcompare --jobs "$root/programs/programs.jobs" --golden "$root/golden-programs" --actual "$res/programs" \
      --report "$res/programs.report" --max-detail 5 > "$res/programs.log" 2>&1
    rc_p=$?
    set -e
    grep -v '^CONTROL-FIRED' "$res/programs.log"
  fi
  rc=$(worst $rc_i $rc_p)
  step "64k done: insn exit $rc_i, programs exit $rc_p -> $rc"
  [ ! -f "$res/insn.log" ] || summarize "$res/insn.log"
  [ ! -f "$res/programs.log" ] || summarize "$res/programs.log"
  exit "$rc"
fi

# ------------------------------------------------------------------ 4k-kvm
[ -r "$kernel" ] || { echo "a64diff-run: cannot read kernel $kernel" >&2; exit 2; }
[ -w /dev/kvm ] || { echo "a64diff-run: /dev/kvm is not usable" >&2; exit 2; }
jobs=${jobs:-$cpus}
step "initramfs"
ir=$res/initramfs
mkdir -p "$ir/a64diff/bin" "$ir/a64diff/emu" "$ir/usr/lib" "$ir/lib64" "$ir/usr/lib64" "$ir/tmp" "$ir/proc" "$ir/dev" "$ir/etc"
ln -s usr/lib "$ir/lib"
cp "$tooldir/init" "$ir/init"
cp "$tool" "$ir/a64diff/bin/a64diff"
emudir=$(dirname "$emu")
cp "$emu" "$ir/a64diff/emu/POWERarm"
[ ! -x "$emudir/POWERarmServer" ] || cp "$emudir/POWERarmServer" "$ir/a64diff/emu/"
echo "nobody:x:65534:65534:nobody:/tmp:/bin/false" > "$ir/etc/passwd"
echo "nobody:x:65534:" > "$ir/etc/group"
# Runtime dependencies of the emulator binaries, resolved by the host loader.
for b in "$ir/a64diff/emu/"*; do
  { ldd "$b" 2>/dev/null || true; } | awk '/=>/ { print $3 } /^\t\/.*ld64/ { print $1 }'
done | sort -u | while read -r lib; do
  [ -f "$lib" ] || continue
  cp -L "$lib" "$ir/usr/lib/"
  case $lib in */ld64.so.*) cp -L "$lib" "$ir/lib64/"; cp -L "$lib" "$ir/usr/lib64/" ;; esac
done
interp=$(readelf -lW "$emu" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
[ -z "$interp" ] || [ -f "$ir$interp" ] || { mkdir -p "$ir$(dirname "$interp")"; cp -L "$interp" "$ir$interp"; }
ls "$ir/usr/lib" | tr '\n' ' ' | sed 's/^/    libs: /'; echo
mkdir -p "$ir/a64diff/bundle"
cp -R "$root/manifest.tsv" "$root/VERSION" "$root/tests" "$root/golden" "$root/programs" "$root/golden-programs" "$ir/a64diff/bundle/"
(cd "$ir" && find . | cpio -o -H newc --quiet) > "$res/initramfs.cpio"
rm -rf "$ir"
step "initramfs $(du -h "$res/initramfs.cpio" | cut -f1)"

guest_limit=$((limit - $(date +%s) + t0 - 30))
[ "$guest_limit" -gt 60 ] || { echo "a64diff-run: time limit too small" >&2; exit 124; }
# The guest stops starting tests at its deadline, leaving time to compare and
# print; timeout(1) is the hard stop if the guest itself wedges.
gdeadline=$((guest_limit - 60))
[ "$gdeadline" -gt $((guest_limit / 2)) ] || gdeadline=$((guest_limit / 2))
# A64DIFF_TEST_NO_GUEST_DEADLINE=1 withholds it, to exercise the hard stop.
[ "${A64DIFF_TEST_NO_GUEST_DEADLINE:-0}" = 0 ] || gdeadline=0
append="console=hvc0 quiet A64DIFF_JOBS=$jobs A64DIFF_TIMEOUT=$timeout A64DIFF_DEADLINE=$gdeadline A64DIFF_SUITES=$suites"
[ -z "$skip" ] || append+=" A64DIFF_SKIP=$skip"
for e in "${envs[@]+"${envs[@]}"}"; do append+=" $e"; done
qemu=(qemu-system-ppc64 -M pseries,accel=kvm -cpu host -smp "$cpus" -m "$mem" -nographic -nodefaults
      -serial stdio -no-reboot -kernel "$kernel" -initrd "$res/initramfs.cpio" -append "$append")
echo "${qemu[*]}" > "$res/qemu-cmdline.txt"
step "boot: ${qemu[*]}"
set +e
timeout -k 20 "$guest_limit" "${qemu[@]}" < /dev/null > "$res/console.log" 2>&1
qrc=$?
set -e
pkill -f -- "-initrd $res/initramfs.cpio" 2>/dev/null || true
rm -f "$res/initramfs.cpio"
step "guest finished (qemu exit $qrc)"

tr -d '\r' < "$res/console.log" > "$res/console.txt"
for part in insn.log insn.report programs.log programs.report; do
  awk -v m="$part" '$0 == "A64DIFF-BEGIN " m { on = 1; next } $0 == "A64DIFF-END " m { on = 0 } on' "$res/console.txt" > "$res/$part"
done
grep -E '^A64DIFF-GUEST' "$res/console.txt" || true
exitline=$(grep -E '^A64DIFF-GUEST-EXIT ' "$res/console.txt" || true)
if [ -z "$exitline" ]; then
  echo "a64diff-run: guest did not report completion (qemu exit $qrc); console: $res/console.txt" >&2
  tail -n 20 "$res/console.txt" >&2
  [ "$qrc" = 124 ] && exit 124
  exit 2
fi
[ ! -s "$res/insn.log" ] || grep -v '^CONTROL-FIRED' "$res/insn.log"
[ ! -s "$res/programs.log" ] || grep -v '^CONTROL-FIRED' "$res/programs.log"
rc_i=$(sed -n 's/.*insn=\(-\?[0-9]*\).*/\1/p' <<< "$exitline")
rc_p=$(sed -n 's/.*programs=\(-\?[0-9]*\).*/\1/p' <<< "$exitline")
[[ $suites == *insn* ]] || rc_i=0
[[ $suites == *programs* ]] || rc_p=0
rc=$(worst "$rc_i" "$rc_p")
step "4k-kvm done: insn exit $rc_i, programs exit $rc_p -> $rc"
[ ! -s "$res/insn.log" ] || summarize "$res/insn.log"
[ ! -s "$res/programs.log" ] || summarize "$res/programs.log"
exit "$rc"
