#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# check-rootfs-server.sh BUILD_DIR [STATIC_AARCH64_BUSYBOX]
#
# Two POWERarm processes with different directory rootfses must each run in
# their own, even when the second one connects to the server the first one
# started. The server answers a rootfs request with the rootfs of the client
# that started it; a client that took that answer for a directory rootfs ran
# in the other rootfs. The a64diff 4K guest hit this when the rootfs suite
# (minimal-debian) started within a second of the projects suite
# (ArchLinuxARM-m2): all six jobs ran against the Arch Linux ARM tree.
#
# The check builds two throwaway rootfses holding the same static busybox and
# a different /marker, keeps a process running in rootfs A, then reads
# /marker from rootfs B. It uses a private HOME, TMPDIR and server socket, so
# it neither sees nor disturbs any other POWERarmServer.
#
# Exit 0: OK. 1: rootfs B saw A's marker (or the check's own control failed).
# 2: could not run (bad BUILD_DIR, no busybox).
set -u

build=${1:?usage: check-rootfs-server.sh BUILD_DIR [BUSYBOX]}
emu=$build/Bin/POWERarm
[ -x "$emu" ] && [ -x "$build/Bin/POWERarmServer" ] || { echo "check-rootfs-server: no POWERarm/POWERarmServer in $build/Bin" >&2; exit 2; }

bb=${2:-}
if [ -z "$bb" ]; then
  for c in "${XDG_CACHE_HOME:-$HOME/.cache}"/a64diff/*/rootfs/minimal-debian/usr/bin/busybox; do
    [ -f "$c" ] && { bb=$c; break; }
  done
fi
[ -n "$bb" ] && [ -f "$bb" ] || { echo "check-rootfs-server: no static aarch64 busybox (pass one as the second argument)" >&2; exit 2; }

w=$(mktemp -d "${TMPDIR:-/tmp}/check-rootfs-server.XXXXXX")
holder=
cleanup() {
  [ -z "$holder" ] || kill "$holder" 2> /dev/null
  wait 2> /dev/null
  rm -rf "$w"
}
trap cleanup EXIT

for r in a b; do
  mkdir -p "$w/$r/bin"
  cp "$bb" "$w/$r/bin/busybox"
  echo "$r" > "$w/$r/marker"
done
mkdir -p "$w/home-control" "$w/home-shared" "$w/run"

# A private server: its socket name and lock folder (under HOME) are this
# check's own, and each phase gets its own, so a server lingering from the
# control phase is neither reused nor holding the lock.
run_in() { # PHASE ROOTFS ARGS...
  local phase=$1 r=$2
  shift 2
  exec env -i PATH=/usr/bin:/bin HOME="$w/home-$phase" TMPDIR="$w/run" \
    POWERARM_SERVERSOCKETPATH="check-rootfs-server.$$.$phase.Socket" POWERARM_ROOTFS="$w/$r" "$emu" "$@"
}

# Control: rootfs B on its own reads its own marker.
got=$(run_in control b /bin/busybox cat /marker 2>&1)
if [ "$got" != b ]; then
  echo "check-rootfs-server: FAIL control: rootfs b alone read '$got', want 'b'"
  exit 1
fi

# Hold a server started by a client in rootfs A. The ready file is a host path
# outside the rootfs, so the guest writes it where this script can see it.
(run_in shared a /bin/busybox sh -c "echo up > $w/ready; exec /bin/busybox sleep 30" > /dev/null 2>&1) &
holder=$!
for _ in $(seq 200); do
  [ -s "$w/ready" ] && break
  sleep 0.05
done
[ -s "$w/ready" ] || { echo "check-rootfs-server: FAIL: the rootfs a holder never started"; exit 1; }

got=$(run_in shared b /bin/busybox cat /marker 2>&1)
if [ "$got" != b ]; then
  echo "check-rootfs-server: FAIL: rootfs b read '$got' while a rootfs a client held the server, want 'b'"
  exit 1
fi
echo "check-rootfs-server: OK"
exit 0
