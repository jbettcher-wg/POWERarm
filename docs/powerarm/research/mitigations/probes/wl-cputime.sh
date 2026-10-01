#!/bin/bash
# wl-cputime.sh -- user vs system CPU time for both workloads, untraced.
# stime is an upper bound on all host syscall time, so it bounds the mitigation
# share without needing strace (which cannot measure a timer-driven workload:
# its 100x slowdown multiplies the polling syscalls by 100x too).
set -u
S=$(cd "$(dirname "$0")" && pwd)
EMU=${EMU:?set EMU to a POWERarm build binary, host path (no /mnt/arch prefix)}
SRM2=/home/jbettcher/.local/share/powerarm/RootFS/ArchLinuxARM-m2
SRVK=/home/jbettcher/.local/share/powerarm/RootFS/ArchLinuxARM-vk
TIMEFORMAT='TIME real=%3R user=%3U sys=%3S'
FILES="lapi.c lcode.c ldebug.c ldo.c lgc.c llex.c lparser.c lstrlib.c ltable.c lvm.c"

echo "=== gcc -c x10 under POWERarm (cpus 96-103) ==="
W=/tmp/pa-ct-gcc; rm -rf "$W"; mkdir -p "$W"
tar xzf "$HOME/.cache/powerarm/m2-sources/lua-5.4.9.tar.gz" -C "$W" || exit 1
SRC=$W/lua-5.4.9/src
CMD="for f in $FILES; do gcc -std=gnu99 -O2 -Wall -fPIC -DLUA_COMPAT_5_3 -DLUA_USE_LINUX -c \$f || exit 1; done"
for i in 1 2 3; do
  rm -f "$SRC"/*.o
  time ( cd "$SRC" && taskset -c 96-103 env SOURCE_DATE_EPOCH=0 LC_ALL=C TZ=UTC \
      POWERARM_PORTABLE=1 POWERARM_ROOTFS="$SRM2" "$EMU" /usr/bin/sh -c "$CMD" >/dev/null 2>&1 )
done
echo "--- same compile native ppc64le ---"
W2=/tmp/pa-ct-gccnat; rm -rf "$W2"; mkdir -p "$W2"
tar xzf "$HOME/.cache/powerarm/m2-sources/lua-5.4.9.tar.gz" -C "$W2" || exit 1
NSRC=$W2/lua-5.4.9/src
NCMD="for f in $FILES; do /mnt/arch/usr/bin/gcc -std=gnu99 -O2 -Wall -fPIC -DLUA_COMPAT_5_3 -DLUA_USE_LINUX -c \$f || exit 1; done"
for i in 1 2; do
  rm -f "$NSRC"/*.o
  time ( cd "$NSRC" && taskset -c 96-103 /mnt/arch/bin/sh -c "$NCMD" >/dev/null 2>&1 )
done

echo "=== headless Chrome, full JS page, under POWERarm (cpus 96-111) ==="
C=/tmp/pa-ct-chrome; rm -rf "$C"; mkdir -p "$C/page"
cp "$S/jsbench.html" "$C/page/p.html"
ARGS="--headless=old --no-sandbox --disable-gpu --disable-dev-shm-usage --no-first-run
      --disable-extensions --disable-background-networking --disable-sync
      --no-default-browser-check --disable-component-update --disable-features=Translate
      --user-data-dir=$C/profile --virtual-time-budget=120000 --dump-dom"
for i in 1 2 3; do
  rm -rf "$C/profile"; mkdir -p "$C/profile"
  time ( taskset -c 96-111 env POWERARM_PORTABLE=1 POWERARM_ROOTFS="$SRVK" \
    "$EMU" /opt/google/chrome/chrome $ARGS "file://$C/page/p.html" > "$C/dom$i.txt" 2>/dev/null )
  echo "   js=$(grep -o 'ms=[0-9]*' "$C/dom$i.txt" | head -1)"
done
echo "--- blank page (startup only, same flags) ---"
printf '<!doctype html><html><head><title>b</title></head><body>b</body></html>' > "$C/page/b.html"
for i in 1 2 3; do
  rm -rf "$C/profile"; mkdir -p "$C/profile"
  time ( taskset -c 96-111 env POWERARM_PORTABLE=1 POWERARM_ROOTFS="$SRVK" \
    "$EMU" /opt/google/chrome/chrome $ARGS "file://$C/page/b.html" > /dev/null 2>&1 )
done
