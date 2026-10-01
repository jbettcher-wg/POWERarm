#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# invrun.sh <POWERarm binary> [cpu]
#
# Produces the table in ROOTFS-PATH-CACHE.md section 3.1: what a guest-path ->
# readlink-answer cache gets wrong when it is not invalidated on the guest's
# own writes. It needs the measurement probe from section 7 of that document;
# without it both runs are the uncached path and agree trivially, which is the
# correct answer for a tree that has no such cache.
#
# Builds a throwaway fixture rootfs in /tmp -- never the owner's -- holding a
# statically linked invtest plus five fixture paths under /pafix. That prefix
# is deliberately one the host lacks: FileManager::Symlinkat is host-first and
# only falls back to creating inside the rootfs on ENOENT, so a fixture under
# /usr would have its mutations land on (or be refused by) the host instead.
#
# Every step runs inside ONE guest process, because a per-process cache is
# only wrong within a process's lifetime: a shell script spawning `readlink`
# per step gets a fresh cache each time and can never see the bug.
set -u
EMU=${1:?usage: invrun.sh <POWERarm binary> [cpu]}
CPU=${2:-0}
SR=${POWERARM_ROOTFS:-$HOME/.local/share/powerarm/RootFS/ArchLinuxARM-m2}
SRC=$(cd "$(dirname "$0")" && pwd)
B=/tmp/pa-inv-build
rm -rf "$B"; mkdir -p "$B"; cp "$SRC/invtest.c" "$B/"
cd "$B"
taskset -c "$CPU" env LC_ALL=C POWERARM_ROOTFS="$SR" "$EMU" \
  /usr/bin/gcc -O1 -static -o invtest invtest.c || { echo "BUILD FAILED"; exit 1; }

mkfixture() {
  local F=$1
  rm -rf "$F"; mkdir -p "$F/pafix/mid"
  cp "$B/invtest" "$F/invtest"
  echo plain > "$F/pafix/plain"
  echo leaf  > "$F/pafix/mid/leaf"
  ln -s tgt3   "$F/pafix/link"
  ln -s before "$F/pafix/moving"
}

for L in 0 2; do
  F=/tmp/pa-inv-fixture-$L
  mkfixture "$F"
  echo "########## POWERARM_RLPROBE=$L ##########"
  taskset -c "$CPU" env LC_ALL=C POWERARM_ROOTFS="$F" POWERARM_RLPROBE=$L \
    "$EMU" /invtest 2>&1
done
