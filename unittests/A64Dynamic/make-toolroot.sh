#!/bin/sh
# SPDX-License-Identifier: MIT
# Pi side: a Debian aarch64 toolchain rootfs (gcc 14, binutils, make, dash,
# coreutils) copied from the Pi's own installed packages, as an a64diff
# program bundle for the M2 ladder rungs 2-4 (tool --version, gcc -c, link).
#
#   make-toolroot.sh OUTDIR
#
# The rootfs is the files of the packages below plus every shared library
# their ELF files need (resolved with ldd). Goldens come from running the
# same commands inside that rootfs natively with bwrap, so both sides use
# byte-identical files. Stand-in for the Arch Linux ARM sysroot until it
# exists; the jobs don't depend on Debian.
set -eu
[ $# = 1 ] || { sed -n '3,14p' "$0" >&2; exit 2; }
[ "$(uname -m)" = aarch64 ] || { echo "make-toolroot: run on aarch64" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$1
rm -rf "$out"
mkdir -p "$out/programs/rootfs" "$out/tests" "$out/golden" "$out/work"
out=$(cd "$out" && pwd)
r=$out/programs/rootfs

pkgs="gcc gcc-14 gcc-14-aarch64-linux-gnu cpp cpp-14 cpp-14-aarch64-linux-gnu binutils binutils-aarch64-linux-gnu
      libbinutils libgcc-14-dev libc6 libc6-dev linux-libc-dev make dash coreutils"
for p in $pkgs; do
  dpkg -L "$p"
done | grep -v '^/usr/share/\(doc\|man\|info\|locale\|lintian\)' | LC_ALL=C sort -u > "$out/work/files"
# Copy files and symlinks (not directories) preserving the path. Debian's
# /bin, /lib and /sbin are symlinks into /usr.
while read -r f; do
  if [ -L "$f" ] || [ -f "$f" ]; then
    mkdir -p "$r$(dirname "$f")"
    cp -P "$f" "$r$f"
  fi
done < "$out/work/files"
for d in bin lib sbin; do [ -e "$r/$d" ] || ln -s "usr/$d" "$r/$d"; done
mkdir -p "$r/tmp" "$r/src" "$r/etc"
chmod 1777 "$r/tmp"
cp "$here/src/hello.c" "$r/src/"
# Inputs for the rungs that must not need cc1: the assembler on a .s and the
# driver's link step (collect2, ld) on a .o, both made here by the Pi's gcc.
(cd "$r/src" && gcc -O2 -S hello.c -o hello.s && gcc -O2 -c hello.c -o hello.o)
ln -sf dash "$r/usr/bin/sh"

# Shared-library closure.
while :; do
  find "$r" -type f -perm -u+x -o -type f -name '*.so*' | while read -r e; do
    file -b "$e" | grep -q '^ELF' || continue
    ldd "$e" 2>/dev/null | awk '/=> \// { print $3 } /^\t\/.*ld-linux/ { print $1 }'
  done | LC_ALL=C sort -u > "$out/work/libs"
  added=0
  while read -r l; do
    # Canonical path (merged /usr): copy the file there, and give it the
    # SONAME-style name ldd reported as a relative symlink beside it.
    real=$(readlink -f "$l")
    dir=$(dirname "$real")
    name=$dir/$(basename "$l")
    if [ ! -e "$r$real" ]; then
      mkdir -p "$r$dir"
      cp "$real" "$r$real"
      added=1
    fi
    if [ "$name" != "$real" ] && [ ! -e "$r$name" ]; then
      ln -s "$(basename "$real")" "$r$name"
      added=1
    fi
  done < "$out/work/libs"
  [ $added = 1 ] || break
done

tab=$(printf '\t')
cc1=/usr/libexec/gcc/aarch64-linux-gnu/14/cc1
{
  echo "# A64Dynamic toolroot jobs. TAB-separated: id class required stdin argv..."
  echo "tool.gcc-version${tab}tool${tab}1${tab}-${tab}/usr/bin/gcc${tab}--version"
  echo "tool.cc1-version${tab}tool${tab}1${tab}-${tab}$cc1${tab}-version"
  echo "tool.as-version${tab}tool${tab}1${tab}-${tab}/usr/bin/as${tab}--version"
  echo "tool.ld-version${tab}tool${tab}1${tab}-${tab}/usr/bin/ld${tab}--version"
  echo "tool.make-version${tab}tool${tab}1${tab}-${tab}/usr/bin/make${tab}--version"
  echo "gcc.as-hello${tab}gcc${tab}1${tab}-${tab}/bin/sh${tab}-c${tab}cp /src/hello.s . && as hello.s -o hello.o && cat hello.o"
  echo "gcc.link-obj${tab}gcc${tab}1${tab}-${tab}/bin/sh${tab}-c${tab}cp /src/hello.o . && gcc hello.o -o hello && cat hello"
  echo "gcc.run-linked-obj${tab}gcc${tab}1${tab}-${tab}/bin/sh${tab}-c${tab}cp /src/hello.o . && gcc hello.o -o hello && ./hello x y"
  echo "gcc.c-hello${tab}gcc${tab}1${tab}-${tab}/bin/sh${tab}-c${tab}cp /src/hello.c . && gcc -O2 -c hello.c -o hello.o && cat hello.o"
  echo "gcc.link-hello${tab}gcc${tab}1${tab}-${tab}/bin/sh${tab}-c${tab}cp /src/hello.c . && gcc -O2 hello.c -o hello && cat hello"
  echo "gcc.run-hello${tab}gcc${tab}1${tab}-${tab}/bin/sh${tab}-c${tab}cp /src/hello.c . && gcc -O2 hello.c -o hello && ./hello x y"
} > "$out/programs/programs.jobs"
# Native: the same argv inside the rootfs through bwrap (unprivileged). The
# job's cwd is bound at the same path so relative paths behave the same.
sed "s|^\([^#][^$tab]*$tab[^$tab]*$tab[^$tab]*$tab[^$tab]*\)$tab|\1${tab}$out/work/inroot$tab|" \
  "$out/programs/programs.jobs" > "$out/work/native.jobs"
cat > "$out/work/inroot" <<INROOT
#!/bin/sh
exec bwrap --bind "$r" / --dev /dev --proc /proc --tmpfs /tmp --bind "\$PWD" "\$PWD" --chdir "\$PWD" \
  --setenv PATH /usr/bin:/bin --unsetenv LANG --unsetenv LC_ALL "\$@"
INROOT
chmod +x "$out/work/inroot"
printf '# a64diff manifest format=1\n# id\tclass\tsub\trequired\texpect\tinsn_addr\tencodings\tdisasm\tinit(x0..x30,sp,nzcv)\n' > "$out/manifest.tsv"

cc -O2 -Wall -o "$out/work/a64diff" "$repo/unittests/A64Diff/tool/a64diff.c"
"$out/work/a64diff" run --jobs "$out/work/native.jobs" --root "$out" --out "$out/golden-programs" -j 1 --timeout 120
rm -rf "$out/golden-programs/"*.cwd
"$out/work/a64diff" run --jobs "$out/work/native.jobs" --root "$out" --out "$out/work/native2" -j 1 --timeout 120 > /dev/null
"$out/work/a64diff" pcompare --jobs "$out/work/native.jobs" --golden "$out/golden-programs" --actual "$out/work/native2" \
  --max-detail 5 > "$out/work/native2.log" || true
grep -q 'RESULT=PASS$' "$out/work/native2.log" || { cat "$out/work/native2.log"; echo "make-toolroot: native re-run differs" >&2; exit 1; }
{
  echo "name a64dynamic-toolroot"
  echo "created $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "golden-host $(uname -srm)"
  for p in $pkgs; do echo "package $p $(dpkg-query -W -f='${Version}' "$p")"; done
  echo "tests 0"
  echo "program-jobs $(grep -vc '^#' "$out/programs/programs.jobs")"
} > "$out/VERSION"
rm -rf "$out/work"
du -sh "$r"
echo "toolroot bundle: $out"
