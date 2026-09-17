#!/bin/bash
# SPDX-License-Identifier: MIT
# Build the pinned Arch Linux ARM aarch64 GCC sysroot for POWERarm M2.  No root needed.
# Runs on the POWER9 host and on the Raspberry Pi 5; both produce the same content hash.
#
#   build-alarm-sysroot.sh [options]
#
#   --dest DIR            sysroot directory
#                         (default ${XDG_DATA_HOME:-~/.local/share}/powerarm/RootFS/ArchLinuxARM-m2)
#   --cache DIR           package cache (default ${XDG_CACHE_HOME:-~/.cache}/powerarm/alarm-pkgs)
#   --manifest FILE       pin manifest (default: alarm-m2.manifest next to this script)
#   --mirror URL          repo base ending in /aarch64; repeatable, tried in order
#   --force               replace an existing non-empty --dest
#   --no-verify-signatures  sha256 pins only (signatures are checked by default)
#   --base-tarball        lay the pinned official ArchLinuxARM rootfs tarball down first
#                         (experimental comparison mode, see README)
#   --no-align            skip the p_align report
#   --resolve             re-resolve the closure against today's repo databases and
#                         REWRITE the manifest pins (then review and commit the diff)
#
# Prints "content-hash sha256:<hex>" for the finished tree and writes the per-entry
# listing to <cache>/../<sysroot-name>.contents for diffing between machines.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
py=(python3 "$here/alarm_sysroot.py")
common=() extract=() align=1 resolve=0
dest="${XDG_DATA_HOME:-$HOME/.local/share}/powerarm/RootFS/ArchLinuxARM-m2"
cache="${XDG_CACHE_HOME:-$HOME/.cache}/powerarm/alarm-pkgs"

while [ $# -gt 0 ]; do
  case "$1" in
    --dest) dest=$2; shift 2 ;;
    --cache) cache=$2; shift 2 ;;
    --manifest|--mirror) common+=("$1" "$2"); shift 2 ;;
    --force|--no-verify-signatures|--base-tarball) extract+=("$1"); shift ;;
    --no-align) align=0; shift ;;
    --resolve) resolve=1; shift ;;
    -h|--help) sed -n '3,22p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; sed -n '3,22p' "$0" >&2; exit 2 ;;
  esac
done
common+=(--cache "$cache")

if [ "$resolve" = 1 ]; then
  "${py[@]}" resolve "${common[@]}"
  exit 0
fi

mkdir -p "$(dirname "$dest")" "$cache"
contents="$(dirname "$cache")/$(basename "$dest").contents"
"${py[@]}" extract "${common[@]}" "${extract[@]}" --dest "$dest" --contents-out "$contents"
echo "contents-listing $contents"
if [ "$align" = 1 ]; then
  echo "--- PT_LOAD p_align"
  "${py[@]}" align "$dest"
fi
