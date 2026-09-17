#!/bin/sh
# SPDX-License-Identifier: MIT
# Build the program-level corpus on the aarch64 golden machine.
#
#   build-programs.sh BUNDLE_ROOT
#
# Produces BUNDLE_ROOT/programs/{bin/hello-glibc,bin/hello-musl,bin/busybox,
# corpus/,applets.sh,*.jobs,SOURCES.txt}.  No root needed: third-party
# binaries come from pinned Debian packages, verified by sha256 and unpacked
# with dpkg-deb -x.  Downloads are cached in $A64DIFF_CACHE (default
# ~/.cache/a64diff).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$1
out=$root/programs
cache=${A64DIFF_CACHE:-$HOME/.cache/a64diff}
mkdir -p "$out/bin" "$cache"

[ "$(uname -m)" = aarch64 ] || { echo "build-programs: must run on aarch64 (golden machine)" >&2; exit 1; }

# name|primary URL|snapshot.debian.org URL (by sha1)|sha256
PKGS="
busybox-static_1.37.0-6+b9_arm64.deb|http://deb.debian.org/debian/pool/main/b/busybox/busybox-static_1.37.0-6%2bb9_arm64.deb|https://snapshot.debian.org/file/6d31276d7d9ae8fd1fd27b9b368bef89e7677d62|c833be48abfa16bc19c4966ec93e289ff1ce5d2f1476cad3a57bd105378cd15c
musl-dev_1.2.5-3.1~deb13u1_arm64.deb|http://deb.debian.org/debian/pool/main/m/musl/musl-dev_1.2.5-3.1%7edeb13u1_arm64.deb|https://snapshot.debian.org/file/e0c0f9fdda5053d869b1fd8ba27f7f926a79499e|384ea029e3694baac696d3993d93246f9a4b3741c493697e1ceb7256d8820748
"

fetch() {
  name=$1 url=$2 snap=$3 sum=$4
  f=$cache/$name
  if [ ! -f "$f" ] || ! echo "$sum  $f" | sha256sum -c --status; then
    curl -fsSL -o "$f.tmp" "$url" || curl -fsSL -o "$f.tmp" "$snap"
    mv "$f.tmp" "$f"
  fi
  echo "$sum  $f" | sha256sum -c --status || { echo "build-programs: sha256 mismatch for $name" >&2; exit 1; }
}

echo "$PKGS" | while IFS='|' read -r name url snap sum; do
  [ -n "$name" ] || continue
  fetch "$name" "$url" "$snap" "$sum"
done

x=$(mktemp -d)
trap 'rm -rf "$x"' EXIT
dpkg-deb -x "$cache/busybox-static_1.37.0-6+b9_arm64.deb" "$x/bb"
dpkg-deb -x "$cache/musl-dev_1.2.5-3.1~deb13u1_arm64.deb" "$x/musl"

install -m 755 "$x/bb/usr/bin/busybox" "$out/bin/busybox"

# glibc: the system toolchain's static libc.
gcc -O2 -static -o "$out/bin/hello-glibc" "$here/hello.c"

# musl: link by hand against the unpacked musl-dev (musl-gcc's spec file has
# absolute /usr paths).  libgcc supplies the long double helpers printf needs.
M=$x/musl
gcc -O2 -nostdinc -isystem "$M/usr/include/aarch64-linux-musl" -isystem "$(gcc -print-file-name=include)" \
  -c "$here/hello.c" -o "$x/hello-musl.o"
gcc -static -nostdlib -o "$out/bin/hello-musl" \
  "$M/usr/lib/aarch64-linux-musl/crt1.o" "$M/usr/lib/aarch64-linux-musl/crti.o" "$x/hello-musl.o" \
  "$M/usr/lib/aarch64-linux-musl/libc.a" "$(gcc -print-libgcc-file-name)" "$M/usr/lib/aarch64-linux-musl/crtn.o"

# Positive checks that each binary is what its name says.
file "$out/bin/hello-glibc" "$out/bin/hello-musl" "$out/bin/busybox" | grep -v 'statically linked' && {
  echo "build-programs: a corpus binary is not static" >&2; exit 1; }
nm "$out/bin/hello-musl" | grep -q ' __init_libc$' || { echo "build-programs: hello-musl is not musl" >&2; exit 1; }
nm "$out/bin/hello-glibc" | grep -q ' __libc_start_main$' || { echo "build-programs: hello-glibc is not glibc" >&2; exit 1; }
nm "$out/bin/hello-glibc" | grep -q ' __init_libc$' && { echo "build-programs: hello-glibc contains musl" >&2; exit 1; }

rm -rf "$out/corpus"
cp -R "$here/corpus" "$out/corpus"
cp "$here/applets.sh" "$here/"*.jobs "$out/"

{
  echo "busybox: Debian busybox-static 1:1.37.0-6+b9 arm64"
  echo "musl:    Debian musl-dev 1.2.5-3.1~deb13u1 arm64"
  echo "$PKGS" | while IFS='|' read -r name url snap sum; do
    if [ -n "$name" ]; then printf '  %s\n    %s\n    %s\n    sha256 %s\n' "$name" "$url" "$snap" "$sum"; fi
  done
  echo "glibc:   $(ldd --version | head -n 1)"
  echo "gcc:     $(gcc --version | head -n 1)"
  (cd "$out/bin" && sha256sum *)
} > "$out/SOURCES.txt"
echo "build-programs: ok"
