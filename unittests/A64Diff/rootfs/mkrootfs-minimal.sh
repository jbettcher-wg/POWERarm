#!/bin/sh
# SPDX-License-Identifier: MIT
# Assemble the "minimal-debian" prototype rootfs on the aarch64 golden machine.
#
#   mkrootfs-minimal.sh OUT_DIR
#
# The smallest tree that runs a dynamically linked glibc program: the golden
# machine's own ld-linux-aarch64.so.1 and libc.so.6 in Debian's multiarch
# layout, plus hello linked against them (PIE and non-PIE) and a static
# busybox for multi-step jobs.  It stands in for the pinned Arch Linux ARM
# sysroot until that exists; a64diff only sees a directory and its content
# hash, so the real sysroot drops in under another name.  OUT_DIR.sources
# records where every file came from.
set -eu
umask 022
here=$(cd "$(dirname "$0")" && pwd)
out=$1
[ "$(uname -m)" = aarch64 ] || { echo "mkrootfs-minimal: run on aarch64" >&2; exit 1; }
multi=usr/lib/aarch64-linux-gnu
rm -rf "$out"
mkdir -p "$out/$multi" "$out/usr/bin"
ln -s usr/lib "$out/lib"
ln -s usr/bin "$out/bin"
ld=$(readlink -f /lib/ld-linux-aarch64.so.1)
libc=$(readlink -f "/$multi/libc.so.6")
install -m 755 "$ld" "$out/$multi/ld-linux-aarch64.so.1"
install -m 644 "$libc" "$out/$multi/libc.so.6"
# PT_INTERP is /lib/ld-linux-aarch64.so.1; Debian points it into the multiarch directory.
ln -s aarch64-linux-gnu/ld-linux-aarch64.so.1 "$out/usr/lib/ld-linux-aarch64.so.1"
[ -x "$out/lib/ld-linux-aarch64.so.1" ] || { echo "mkrootfs-minimal: PT_INTERP does not resolve inside the rootfs" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
gcc -O2 -o "$tmp/hello-dyn" "$here/../programs/hello.c"
gcc -O2 -no-pie -o "$tmp/hello-dyn-nopie" "$here/../programs/hello.c"
for b in hello-dyn hello-dyn-nopie; do
  readelf -lW "$tmp/$b" | grep -q 'Requesting program interpreter: /lib/ld-linux-aarch64.so.1' ||
    { echo "mkrootfs-minimal: $b has an unexpected interpreter" >&2; exit 1; }
  install -m 755 "$tmp/$b" "$out/usr/bin/$b"
done
# Only libc may be needed: the rootfs holds nothing else.
readelf -dW "$out/usr/bin/hello-dyn" | grep NEEDED | grep -v 'libc.so.6' && { echo "mkrootfs-minimal: unexpected NEEDED" >&2; exit 1; }
if [ -n "${A64DIFF_BUSYBOX:-}" ]; then install -m 755 "$A64DIFF_BUSYBOX" "$out/usr/bin/busybox"; fi
# The tree must not depend on the host: run hello through the rootfs's own loader.
"$out/$multi/ld-linux-aarch64.so.1" --library-path "$out/$multi" "$out/usr/bin/hello-dyn" >/dev/null 2>&1 || [ $? = 7 ] ||
  { echo "mkrootfs-minimal: hello-dyn does not run with the rootfs loader" >&2; exit 1; }
{
  echo "rootfs minimal-debian (prototype; replaced by the Arch Linux ARM sysroot)"
  echo "ld.so:   $ld"
  echo "libc:    $libc"
  echo "libc6:   $(dpkg-query -W -f='${Version}' libc6 2>/dev/null)"
  echo "gcc:     $(gcc --version | head -n 1)"
  echo "busybox: ${A64DIFF_BUSYBOX:-none}"
} > "$out.sources"
