#!/bin/bash
# SPDX-License-Identifier: MIT
# Download the pinned upstream source tarballs for the M2 builds (zlib, Lua) into a cache
# directory and check their sha256 against the "project" lines of alarm-m2.manifest.
# Doesn't build or unpack anything.
#
#   fetch-m2-projects.sh [--cache DIR] [--manifest FILE] [NAME...]
#
#   --cache DIR      default ${XDG_CACHE_HOME:-~/.cache}/powerarm/m2-sources
#   --manifest FILE  default alarm-m2.manifest next to this script
#   NAME...          only these projects (default: all)
#
# Prints "<name> <version> <path>" per project.  A sha256 mismatch is fatal and the bad
# download is kept as <file>.bad.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
cache="${XDG_CACHE_HOME:-$HOME/.cache}/powerarm/m2-sources"
manifest="$here/alarm-m2.manifest"
want=()
while [ $# -gt 0 ]; do
  case "$1" in
    --cache) cache=$2; shift 2 ;;
    --manifest) manifest=$2; shift 2 ;;
    -h|--help) sed -n '3,15p' "$0"; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) want+=("$1"); shift ;;
  esac
done
mkdir -p "$cache"

found=0
while read -r kind name version url sha; do
  [ "$kind" = project ] || continue
  if [ ${#want[@]} -gt 0 ] && ! printf '%s\n' "${want[@]}" | grep -qx "$name"; then continue; fi
  found=$((found + 1))
  [ "${#sha}" = 64 ] || { echo "error: $name: manifest has no sha256 pin" >&2; exit 1; }
  dest="$cache/${url##*/}"
  if [ ! -f "$dest" ] || [ "$(sha256sum < "$dest" | cut -d' ' -f1)" != "$sha" ]; then
    echo "  fetch $url" >&2
    curl -fsSL --retry 3 -o "$dest.part" "$url"
    mv "$dest.part" "$dest"
  fi
  got=$(sha256sum < "$dest" | cut -d' ' -f1)
  if [ "$got" != "$sha" ]; then
    mv "$dest" "$dest.bad"
    echo "error: $name $version: sha256 mismatch: manifest $sha, got $got" >&2
    exit 1
  fi
  echo "$name $version $dest"
done < "$manifest"
[ "$found" -gt 0 ] || { echo "error: no matching project lines in $manifest" >&2; exit 1; }
