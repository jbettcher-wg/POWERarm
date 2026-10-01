#!/bin/bash
# wl-gcc.sh -- host syscall rate of a gcc -c compile under POWERarm.
# Three untraced runs for wall time, then one strace -c -f run for the count.
set -u
S=$(cd "$(dirname "$0")" && pwd)
EMU=${EMU:?set EMU to a POWERarm build binary, host path (no /mnt/arch prefix)}
SR=/home/jbettcher/.local/share/powerarm/RootFS/ArchLinuxARM-m2
STRACE=/mnt/arch/usr/bin/strace
CPU=${CPU:-96-103}
W=/tmp/pa-mit-gcc
FILES="lapi.c lcode.c ldebug.c ldo.c lgc.c llex.c lparser.c lstrlib.c ltable.c lvm.c"

rm -rf "$W"; mkdir -p "$W"
tar xzf "$HOME/.cache/powerarm/m2-sources/lua-5.4.9.tar.gz" -C "$W" || exit 1
SRC=$W/lua-5.4.9/src
CMD="for f in $FILES; do gcc -std=gnu99 -O2 -Wall -fPIC -DLUA_COMPAT_5_3 -DLUA_USE_LINUX -c \$f || exit 1; done"

echo "== gcc -c x10 (Lua 5.4.9) under POWERarm, cpus $CPU =="
for i in 1 2 3; do
  rm -f "$SRC"/*.o
  s=$(date +%s.%N)
  ( cd "$SRC" && taskset -c $CPU env SOURCE_DATE_EPOCH=0 LC_ALL=C TZ=UTC \
      POWERARM_PORTABLE=1 POWERARM_ROOTFS="$SR" "$EMU" /usr/bin/sh -c "$CMD" ) >/dev/null 2>&1
  rc=$?
  e=$(date +%s.%N)
  echo "run$i rc=$rc wall=$(echo "$e - $s" | bc)"
done

echo "-- strace -c -f (count only; wall under strace is not comparable) --"
rm -f "$SRC"/*.o
s=$(date +%s.%N)
( cd "$SRC" && taskset -c $CPU env SOURCE_DATE_EPOCH=0 LC_ALL=C TZ=UTC \
    POWERARM_PORTABLE=1 POWERARM_ROOTFS="$SR" \
    $STRACE -c -f -o "$S/gcc.strace" "$EMU" /usr/bin/sh -c "$CMD" ) >/dev/null 2>&1
rc=$?
e=$(date +%s.%N)
echo "straced rc=$rc wall=$(echo "$e - $s" | bc)"
tail -40 "$S/gcc.strace"

echo "== same compile NATIVE ppc64le (guest-syscall proxy) =="
rm -rf "$W/native"; mkdir -p "$W/native"
tar xzf "$HOME/.cache/powerarm/m2-sources/lua-5.4.9.tar.gz" -C "$W/native" || exit 1
NSRC=$W/native/lua-5.4.9/src
NCMD="for f in $FILES; do /mnt/arch/usr/bin/gcc -std=gnu99 -O2 -Wall -fPIC -DLUA_COMPAT_5_3 -DLUA_USE_LINUX -c \$f || exit 1; done"
s=$(date +%s.%N)
( cd "$NSRC" && taskset -c $CPU /mnt/arch/bin/sh -c "$NCMD" ) >/dev/null 2>&1
e=$(date +%s.%N)
echo "native wall=$(echo "$e - $s" | bc)"
rm -f "$NSRC"/*.o
( cd "$NSRC" && taskset -c $CPU $STRACE -c -f -o "$S/gcc-native.strace" /mnt/arch/bin/sh -c "$NCMD" ) >/dev/null 2>&1
tail -8 "$S/gcc-native.strace"
