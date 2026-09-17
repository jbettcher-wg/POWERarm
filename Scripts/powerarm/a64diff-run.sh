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
#   --suites LIST       comma list: insn and/or job suites (programs/<suite>.jobs); default all
#   --rootfs NAME=DIR   where an external rootfs lives (default:
#                       ${XDG_DATA_HOME:-~/.local/share}/powerarm/RootFS/NAME); bundled
#                       rootfs trees are used from the bundle.  Content hashes are checked.
#   4k-kvm only:
#   --kernel IMAGE      default /boot/vmlinuz-linux-power9 (the host's 4K kernel)
#   --cpus N --mem SIZE guest size (default 8, 4G)
#   --rootfs-transport ext4|squashfs
#                       how a rootfs reaches the guest: a read-only virtio-blk image
#                       (default ext4; images are cached by content hash)
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
suites=all
transport=ext4
rootfs_over=()
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
    --rootfs) rootfs_over+=("$2"); shift 2 ;;
    --rootfs-transport) transport=$2; shift 2 ;;
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
    tar -C "$work" -xpzf "$bundle"   # -p: keep modes (rootfs content hashes include them)
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

rsrc=$here/../../unittests/A64Diff/rootfs/a64diff-rootfs.py
[ -f "$rsrc" ] || rsrc=$root/src/a64diff-rootfs.py

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

wanted() { [ "$suites" = all ] || [[ ",$suites," == *",$1,"* ]]; }

# Suites in this bundle: insn (if it has a manifest) and one per jobs file.
all_suites=()
[ ! -f "$root/manifest.tsv" ] || all_suites+=(insn)
for f in "$root"/programs/*.jobs; do [ -f "$f" ] && all_suites+=("$(basename "$f" .jobs)"); done
run_suites=()
for su in "${all_suites[@]}"; do wanted "$su" && run_suites+=("$su"); done
[ ${#run_suites[@]} -gt 0 ] || { echo "a64diff-run: no suite of '${suites}' in this bundle (has: ${all_suites[*]})" >&2; exit 2; }

# Rootfs trees the bundle's jobs use: rootfs/<name>.id records the content hash.
rootfs_names=() rootfs_dirs=() rootfs_hashes=()
for idf in "$root"/rootfs/*.id; do
  [ -f "$idf" ] || continue
  n=$(sed -n 's/^name //p' "$idf")
  h=$(sed -n 's/^content-hash //p' "$idf")
  loc=$(sed -n 's/^location //p' "$idf")
  d=
  for ov in "${rootfs_over[@]+"${rootfs_over[@]}"}"; do [ "${ov%%=*}" = "$n" ] && d=${ov#*=}; done
  if [ -z "$d" ]; then
    if [ "$loc" = bundle ]; then d=$root/rootfs/$n; else d=${XDG_DATA_HOME:-$HOME/.local/share}/powerarm/RootFS/$n; fi
  fi
  [ -d "$d" ] || { echo "a64diff-run: rootfs $n ($h) not found at $d" >&2; exit 2; }
  d=$(readlink -f "$d")
  if [ "${A64DIFF_TEST_ROOTFS_UNVERIFIED:-0}" = 1 ]; then
    # Measurement aid only: use a tree whose hash doesn't match the bundle.
    echo "a64diff-run: TEST MODE: rootfs $n at $d is NOT verified; results are not comparable" >&2
    h=unverified-$(date +%s%N)
  else
    python3 "$rsrc" verify "$d" "$h" || exit 2
  fi
  step "rootfs $n: $d ($h)"
  rootfs_names+=("$n") rootfs_dirs+=("$d") rootfs_hashes+=("$h")
done

suite_summary=()
if [ "$mode" = 64k ]; then
  ps=$(getconf PAGESIZE)
  [ "$ps" = 65536 ] || { echo "a64diff-run: 64k mode but the host page size is $ps" >&2; exit 2; }
  jobs=${jobs:-32}
  for e in "${envs[@]+"${envs[@]}"}"; do export "${e?}"; done
  rargs=()
  for i in "${!rootfs_names[@]}"; do rargs+=(--rootfs "${rootfs_names[$i]}=${rootfs_dirs[$i]}"); done
  # One wall-clock budget for every suite: each gets what is left of it.
  end=$((t0 + limit - 15))
  left() { local l=$((end - $(date +%s))); [ "$l" -gt 1 ] || l=1; echo "$l"; }
  rcs=()
  for su in "${run_suites[@]}"; do
    set +e
    if [ "$su" = insn ]; then
      step "instruction suite (-j $jobs)"
      nice -n 10 "$tool" run --manifest "$root/manifest.tsv" --root "$root" --out "$res/insn" -j "$jobs" \
        --timeout "$timeout" --deadline "$(left)" --skip "$skip" -- "$emu" | grep -v '^a64diff run: [0-9]'
      "$tool" compare --manifest "$root/manifest.tsv" --golden "$root/golden" --actual "$res/insn" \
        --report "$res/insn.report" --max-detail 5 --skip "$skip" > "$res/insn.log" 2>&1
    else
      step "job suite $su"
      nice -n 10 "$tool" run --jobs "$root/programs/$su.jobs" --root "$root" --out "$res/$su" -j "$jobs" \
        --timeout "$timeout" --deadline "$(left)" "${rargs[@]+"${rargs[@]}"}" -- "$emu"
      "$tool" pcompare --jobs "$root/programs/$su.jobs" --golden "$root/golden-$su" --actual "$res/$su" \
        --report "$res/$su.report" --max-detail 5 > "$res/$su.log" 2>&1
    fi
    rc=$?
    set -e
    grep -v '^CONTROL-FIRED' "$res/$su.log"
    rcs+=("$rc")
    suite_summary+=("$su exit $rc")
  done
  rc=$(worst "${rcs[@]}")
  step "64k done: ${suite_summary[*]} -> $rc"
  for su in "${run_suites[@]}"; do echo "[$su]"; summarize "$res/$su.log"; done
  exit "$rc"
fi

# ------------------------------------------------------------------ 4k-kvm
[ -r "$kernel" ] || { echo "a64diff-run: cannot read kernel $kernel" >&2; exit 2; }
[ -w /dev/kvm ] || { echo "a64diff-run: /dev/kvm is not usable" >&2; exit 2; }
jobs=${jobs:-$cpus}

# Rootfs transport.  The 4K kernel has no 9p or virtiofs support (and there is
# no virtiofsd on the host), so each rootfs goes in as a read-only virtio-blk
# disk image, built from the verified tree without root and cached by content
# hash.  virtio_pci/virtio_blk (and squashfs) are modules of that kernel: they
# are copied from its /lib/modules directory into the initramfs.
drives=() rootfs_spec=
if [ ${#rootfs_names[@]} -gt 0 ]; then
  case $transport in ext4|squashfs) ;; *) echo "a64diff-run: unknown --rootfs-transport $transport" >&2; exit 2 ;; esac
  imgdir=$work/rootfs-img
  mkdir -p "$imgdir"
  for i in "${!rootfs_names[@]}"; do
    n=${rootfs_names[$i]} d=${rootfs_dirs[$i]} h=${rootfs_hashes[$i]#sha256:}
    img=$imgdir/$h.$transport
    [[ $h != unverified-* ]] || img=$res/rootfs-$n.$transport
    if [ ! -f "$img" ]; then
      t1=$(date +%s.%N)
      if [ "$transport" = ext4 ]; then
        kb=$(du -sk --apparent-size "$d" | cut -f1)
        files=$(find "$d" | wc -l)
        size=$(( kb + kb / 4 + files * 4 + 16384 ))
        mke2fs -q -F -t ext4 -O ^has_journal -m 0 -N $((files + 1024)) -L "$n" -d "$d" "$img.tmp" "${size}k"
      else
        mksquashfs "$d" "$img.tmp" -noappend -all-root -comp "${A64DIFF_SQUASHFS_COMP:-lzo}" -quiet >/dev/null
      fi
      mv "$img.tmp" "$img"
      step "rootfs $n: built $transport image $(du -h "$img" | cut -f1) in $(echo "$(date +%s.%N) - $t1" | bc)s"
    fi
    drives+=(-drive "file=$img,if=none,id=rootfs$i,format=raw,readonly=on" -device "virtio-blk-pci,drive=rootfs$i")
    rootfs_spec+="${rootfs_spec:+,}$n:$transport"
  done
fi

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
if [ -n "$rootfs_spec" ]; then
  # The module directory whose vmlinuz is the kernel being booted.
  moddir=
  for m in /lib/modules/*; do cmp -s "$m/vmlinuz" "$kernel" && moddir=$m; done
  [ -n "$moddir" ] || { echo "a64diff-run: no /lib/modules directory matches $kernel" >&2; exit 2; }
  want=(virtio_ring virtio virtio_pci_legacy_dev virtio_pci_modern_dev virtio_pci virtio_blk)
  [ "$transport" = squashfs ] && want+=(squashfs)
  mkdir -p "$ir/a64diff/modules"
  for m in "${want[@]}"; do
    # modules.builtin: nothing to load; otherwise take it from modules.dep.
    grep -q "/$m.ko" "$moddir/modules.builtin" && continue
    rel=$(grep -oE "^[^:]*/$m\.ko(\.(zst|xz|gz))?:" "$moddir/modules.dep" | head -n 1 | tr -d :)
    [ -n "$rel" ] || { echo "a64diff-run: module $m not found for $moddir" >&2; exit 2; }
    case $rel in
      *.ko) cp "$moddir/$rel" "$ir/a64diff/modules/$m.ko" ;;
      *.zst) zstd -dq "$moddir/$rel" -o "$ir/a64diff/modules/$m.ko" ;;
      *.xz) xz -dc "$moddir/$rel" > "$ir/a64diff/modules/$m.ko" ;;
      *.gz) gzip -dc "$moddir/$rel" > "$ir/a64diff/modules/$m.ko" ;;
    esac
    echo "$m.ko" >> "$ir/a64diff/modules/order"
  done
  echo "    modules ($moddir): $(tr '\n' ' ' < "$ir/a64diff/modules/order")"
fi
mkdir -p "$ir/a64diff/bundle"
parts=(VERSION programs)
wanted insn && parts+=(manifest.tsv tests golden)
for part in "${parts[@]}"; do
  [ ! -e "$root/$part" ] || cp -R "$root/$part" "$ir/a64diff/bundle/"
done
for su in "${run_suites[@]}"; do
  [ "$su" = insn ] || cp -R "$root/golden-$su" "$ir/a64diff/bundle/"
done
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
guest_suites=$(IFS=,; echo "${run_suites[*]}")
append="console=hvc0 quiet A64DIFF_JOBS=$jobs A64DIFF_TIMEOUT=$timeout A64DIFF_DEADLINE=$gdeadline A64DIFF_SUITES=$guest_suites"
[ -z "$skip" ] || append+=" A64DIFF_SKIP=$skip"
[ -z "$rootfs_spec" ] || append+=" A64DIFF_ROOTFS=$rootfs_spec"
for e in "${envs[@]+"${envs[@]}"}"; do append+=" $e"; done
qemu=(qemu-system-ppc64 -M pseries,accel=kvm -cpu host -smp "$cpus" -m "$mem" -nographic -nodefaults
      -serial stdio -no-reboot -kernel "$kernel" -initrd "$res/initramfs.cpio" "${drives[@]+"${drives[@]}"}" -append "$append")
echo "${qemu[*]}" > "$res/qemu-cmdline.txt"
step "boot: ${qemu[*]}"
set +e
timeout -k 20 "$guest_limit" "${qemu[@]}" < /dev/null > "$res/console.log" 2>&1
qrc=$?
set -e
pkill -f -- "-initrd $res/initramfs.cpio" 2>/dev/null || true
rm -f "$res/initramfs.cpio"
[[ ${rootfs_hashes[*]-} != *unverified-* ]] || rm -f "$res"/rootfs-*."$transport"
step "guest finished (qemu exit $qrc)"

tr -d '\r' < "$res/console.log" > "$res/console.txt"
for su in "${run_suites[@]}"; do
  for part in "$su.log" "$su.report"; do
    awk -v m="$part" '$0 == "A64DIFF-BEGIN " m { on = 1; next } $0 == "A64DIFF-END " m { on = 0 } on' "$res/console.txt" > "$res/$part"
  done
done
grep -E '^A64DIFF-GUEST' "$res/console.txt" || true
exitline=$(grep -E '^A64DIFF-GUEST-EXIT' "$res/console.txt" || true)
if [ -z "$exitline" ]; then
  echo "a64diff-run: guest did not report completion (qemu exit $qrc); console: $res/console.txt" >&2
  tail -n 20 "$res/console.txt" >&2
  [ "$qrc" = 124 ] && exit 124
  exit 2
fi
rcs=()
[[ $exitline != *" error="* ]] || rcs+=(2)
[ -z "$rootfs_spec" ] || ! grep -q '^A64DIFF-GUEST-ERROR' "$res/console.txt" || rcs+=(2)
for su in "${run_suites[@]}"; do
  [ ! -s "$res/$su.log" ] || grep -v '^CONTROL-FIRED' "$res/$su.log"
  c=$(sed -n "s/.* $su=\(-\?[0-9]*\).*/\1/p" <<< "$exitline")
  rcs+=("${c:--1}")
  suite_summary+=("$su exit ${c:-missing}")
done
rc=$(worst "${rcs[@]}")
step "4k-kvm done: ${suite_summary[*]} -> $rc"
for su in "${run_suites[@]}"; do echo "[$su]"; [ -s "$res/$su.log" ] && summarize "$res/$su.log" || echo "(no log)"; done
exit "$rc"
