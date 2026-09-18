#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Pi side: generate, build and run the A64Frontend tests natively.
#   golden.sh OUTDIR
# Writes OUTDIR/<test> binaries, OUTDIR/<test>.golden (stdout) and
# OUTDIR/<test>.rc (exit status). Run on an AArch64 Linux machine with gcc.
#
# Optional corpus inputs (skipped when unset):
#   MUSL_ROOT  a directory holding usr/include/aarch64-linux-musl and
#              usr/lib/aarch64-linux-musl, e.g. Debian's musl and musl-dev
#              packages unpacked with `dpkg-deb -x`; builds the musl variants.
#   BUSYBOX    a static AArch64 busybox; adds the applet tests.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: golden.sh OUTDIR}
mkdir -p "$out"

python3 "$here/gen.py" "$out"
python3 "$here/gen_simd.py" "$out"
cp "$here"/*.S "$out"/

cd "$out"
# A test may carry extra link flags on a `// LDFLAGS:` line.
for src in *.S; do
  [ "$src" = common.S ] && continue
  gcc -march=armv8.2-a+fp16+crypto+crc+dotprod -nostdlib -static $(sed -n 's|^// LDFLAGS: ||p' "$src") -o "${src%.S}" "$src"
done

# A large file-backed RW segment, for the destructive-madvise regression test.
python3 "$here/gen_blob.py" "$out/madvfile_blob.bin" $((24 * 1024 * 1024))
gcc -O2 -o madvfile "$here/madvfile.c" "$here/madvfile.S"

# Static libc programs.
corpus="hello printf_float strmem fpmath lse lseminmax lsecasp litmus nosve cntvct madvfile"
for t in $corpus; do
  gcc -static -O2 -o "$t" "$here/$t.c" -lm
done
if [ -n "${MUSL_ROOT:-}" ]; then
  sed "s#/usr/include/aarch64-linux-musl#$MUSL_ROOT/usr/include/aarch64-linux-musl#g; s#/usr/lib/aarch64-linux-musl#$MUSL_ROOT/usr/lib/aarch64-linux-musl#g" \
    "$MUSL_ROOT/usr/lib/aarch64-linux-musl/musl-gcc.specs" > musl.specs
  for t in $corpus; do
    gcc -specs="$out/musl.specs" -static -O2 -o "musl_$t" "$here/$t.c" -lm
  done
  corpus="$corpus musl_hello musl_printf_float musl_strmem musl_fpmath"
fi
# glibc only (ucontext): guest call/return shapes for the link-stack pairing.
gcc -static -O2 -o callret "$here/callret.c"
corpus="$corpus callret"
# The rootfs overlay test: natively it runs inside a scratch copy of its own
# fixture (OVT_ROOT); run.sh runs it under POWERarm with that fixture as the
# base rootfs.
gcc -static -O2 -o rootfs_overlay "$here/rootfs_overlay.c"

run_golden() {
  set +e
  "$@" > "$t.golden" 2>/dev/null
  echo $? > "$t.rc"
  set -e
}

for src in *.S; do
  [ "$src" = common.S ] && continue
  t=${src%.S}
  # A test that cannot run on this kernel states its golden instead: its
  # `// EXPECT:` lines are the stdout and `// EXPECT-RC:` the exit status.
  if grep -q '^// EXPECT-RC: ' "$src"; then
    sed -n 's|^// EXPECT: ||p' "$src" > "$t.golden"
    sed -n 's|^// EXPECT-RC: ||p' "$src" > "$t.rc"
    continue
  fi
  run_golden "./$t"
done
for t in $corpus; do
  run_golden "./$t"
done
t=rootfs_overlay
rm -rf "$out/ovt-native"
./rootfs_overlay --make-fixture "$out/ovt-native"
run_golden env OVT_ROOT="$out/ovt-native" ./rootfs_overlay
rm -rf "$out/ovt-native"

# Busybox applets on a fixed input. <test>.bin names the binary and
# <test>.args its arguments for run.sh.
if [ -n "${BUSYBOX:-}" ]; then
  cp "$BUSYBOX" busybox
  python3 "$here/gen_applet_input.py" applet-input.txt
  while read -r t args; do
    echo busybox > "$t.bin"
    echo "$args" > "$t.args"
    run_golden ./busybox $args
  done <<APPLETS
bb_echo echo hello world
bb_cat cat applet-input.txt
bb_wc wc applet-input.txt
bb_sort sort applet-input.txt
bb_sort_n sort -n -k2 applet-input.txt
bb_sha256sum sha256sum applet-input.txt
bb_md5sum md5sum applet-input.txt
APPLETS
fi
echo "golden outputs written to $out"
