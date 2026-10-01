#!/bin/bash
# wl-chrome2.sh <tag> <page.html> <cpus> -- one straced + three untraced runs of
# headless Chrome on a local page under POWERarm.  Two different JS workloads
# differenced give the steady-state JS-phase host syscall rate.
set -u
S=$(cd "$(dirname "$0")" && pwd)
EMU=${EMU:?set EMU to a POWERarm build binary, host path (no /mnt/arch prefix)}
SR=/home/jbettcher/.local/share/powerarm/RootFS/ArchLinuxARM-vk
STRACE=/mnt/arch/usr/bin/strace
TAG=$1; PAGEF=$2; CPU=$3
W=/tmp/pa-mit-c-$TAG
rm -rf "$W"; mkdir -p "$W/page"
cp "$S/$PAGEF" "$W/page/p.html"
PAGE="file://$W/page/p.html"
ARGS="--headless=old --no-sandbox --disable-gpu --disable-dev-shm-usage --no-first-run
      --disable-extensions --disable-background-networking --disable-sync
      --no-default-browser-check --disable-component-update --disable-features=Translate
      --user-data-dir=$W/profile --virtual-time-budget=600000 --dump-dom"

echo "== chrome $TAG ($PAGEF) cpus $CPU =="
for i in 1 2 3; do
  rm -rf "$W/profile"; mkdir -p "$W/profile"
  s=$(date +%s.%N)
  taskset -c $CPU env POWERARM_PORTABLE=1 POWERARM_ROOTFS="$SR" \
    "$EMU" /opt/google/chrome/chrome $ARGS "$PAGE" > "$W/dom$i.txt" 2>/dev/null
  rc=$?; e=$(date +%s.%N)
  echo "$TAG run$i rc=$rc wall=$(echo "$e - $s" | bc) js=$(grep -o 'ms=[0-9]*' "$W/dom$i.txt" | head -1)"
done
rm -rf "$W/profile"; mkdir -p "$W/profile"
s=$(date +%s.%N)
taskset -c $CPU env POWERARM_PORTABLE=1 POWERARM_ROOTFS="$SR" \
  $STRACE -c -f -o "$S/chrome-$TAG.strace" \
  "$EMU" /opt/google/chrome/chrome $ARGS "$PAGE" > "$W/dom-st.txt" 2>/dev/null
rc=$?; e=$(date +%s.%N)
echo "$TAG straced rc=$rc wall=$(echo "$e - $s" | bc) js=$(grep -o 'ms=[0-9]*' "$W/dom-st.txt" | head -1)"
echo "$TAG total: $(tail -1 "$S/chrome-$TAG.strace")"
head -14 "$S/chrome-$TAG.strace"
