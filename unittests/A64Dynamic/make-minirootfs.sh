#!/bin/sh
# SPDX-License-Identifier: MIT
# Pi side: assemble a minimal Debian aarch64 rootfs with dynamically linked
# test programs, as an a64diff program bundle with native goldens.
#
#   make-minirootfs.sh OUTDIR
#
# OUTDIR/programs/rootfs holds the Pi's ld-linux-aarch64.so.1, libc.so.6 and
# libm.so.6 at their Debian paths, /usr/bin/{hello,fpprint,dltest} and
# /usr/lib/libplugin.so. OUTDIR/golden-programs are the native runs (through
# the copied ld.so and libraries). On the POWER9:
#   Scripts/powerarm/a64diff-run.sh {64k|4k-kvm} POWERARM --bundle OUTDIR --suites programs \
#     --env POWERARM_ROOTFS=<bundle>/programs/rootfs
# where <bundle> is OUTDIR on the 64K host and /a64diff/bundle in the KVM guest.
set -eu
[ $# = 1 ] || { sed -n '3,15p' "$0" >&2; exit 2; }
[ "$(uname -m)" = aarch64 ] || { echo "make-minirootfs: run on aarch64" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$1
rm -rf "$out"
multi=/usr/lib/aarch64-linux-gnu
mkdir -p "$out/programs/rootfs$multi" "$out/programs/rootfs/usr/bin" "$out/tests" "$out/golden" "$out/work"
out=$(cd "$out" && pwd)
r=$out/programs/rootfs
for l in ld-linux-aarch64.so.1 libc.so.6 libm.so.6; do
  cp -L "$multi/$l" "$r$multi/$l"
done
ln -s usr/lib "$r/lib"
# Debian: /lib/ld-linux-aarch64.so.1 -> aarch64-linux-gnu/ld-linux-aarch64.so.1
ln -s aarch64-linux-gnu/ld-linux-aarch64.so.1 "$r/usr/lib/ld-linux-aarch64.so.1"
cc=${CC:-gcc}
$cc -O2 -Wall -o "$r/usr/bin/hello" "$here/src/hello.c"
$cc -O2 -Wall -o "$r/usr/bin/fpprint" "$here/src/fpprint.c" -lm
$cc -O2 -Wall -shared -fPIC -o "$r/usr/lib/libplugin.so" "$here/src/plugin.c"
$cc -O2 -Wall -o "$r/usr/bin/dltest" "$here/src/dltest.c"

tab=$(printf '\t')
jobs() { # PREFIX...: the jobs, with each argv prefixed
  echo "# A64Dynamic minirootfs jobs. TAB-separated: id class required stdin argv..."
  p=$(printf '%s\t' "$@")
  echo "dynamic.hello${tab}dynamic${tab}1${tab}-${tab}${p}/usr/bin/hello${tab}one${tab}two words"
  echo "dynamic.fpprint${tab}dynamic${tab}1${tab}-${tab}${p}/usr/bin/fpprint"
  echo "dynamic.dltest${tab}dynamic${tab}1${tab}-${tab}${p}/usr/bin/dltest${tab}/usr/lib/libplugin.so"
}
# Under POWERarm the paths resolve inside POWERARM_ROOTFS.
jobs | sed "s|${tab}${tab}/usr|${tab}/usr|" > "$out/programs/programs.jobs"
# Natively, through the copied loader and libraries.
jobs "$r$multi/ld-linux-aarch64.so.1" --library-path "$r$multi" |
  sed "s|${tab}/usr/bin/|${tab}$r/usr/bin/|; s|${tab}/usr/lib/libplugin.so|${tab}$r/usr/lib/libplugin.so|" > "$out/work/native.jobs"
printf '# a64diff manifest format=1\n# id\tclass\tsub\trequired\texpect\tinsn_addr\tencodings\tdisasm\tinit(x0..x30,sp,nzcv)\n' > "$out/manifest.tsv"

cc -O2 -Wall -o "$out/work/a64diff" "$repo/unittests/A64Diff/tool/a64diff.c"
"$out/work/a64diff" run --jobs "$out/work/native.jobs" --root "$out" --out "$out/golden-programs" -j 1 --timeout 60
rm -rf "$out/golden-programs/"*.cwd
"$out/work/a64diff" run --jobs "$out/work/native.jobs" --root "$out" --out "$out/work/native2" -j 1 --timeout 60 > /dev/null
"$out/work/a64diff" pcompare --jobs "$out/work/native.jobs" --golden "$out/golden-programs" --actual "$out/work/native2" \
  --max-detail 5 > "$out/work/native2.log" || true
grep -q 'RESULT=PASS$' "$out/work/native2.log" || { cat "$out/work/native2.log"; echo "make-minirootfs: native re-run differs" >&2; exit 1; }
{
  echo "name a64dynamic-minirootfs"
  echo "created $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "golden-host $(uname -srm)"
  echo "glibc $(ldd --version | head -n 1)"
  echo "gcc $($cc --version | head -n 1)"
  echo "tests 0"
  echo "program-jobs 3"
} > "$out/VERSION"
(cd "$r" && find . -type f | LC_ALL=C sort | xargs sha256sum) > "$out/MANIFEST"
rm -rf "$out/work"
echo "minirootfs bundle: $out"
