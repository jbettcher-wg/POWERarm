#!/bin/bash
# aot-translate.sh: translate RootFS binaries into the POWERarm code cache
# ahead of time, so their first run loads blocks instead of compiling them.
#
#   aot-translate.sh [-j JOBS] [-m all|entries] [-s MAXBYTES] <POWERarm> [guest path ...]
#
# Paths are guest paths (/usr/bin/bash) or directories under the RootFS;
# default: /usr/bin /usr/lib. The RootFS is $POWERARM_ROOTFS. Every other
# POWERARM_* variable (cache location, codegen options) is passed through, and
# it must match the later runs: the cache file name hashes the emulator build
# and every codegen option, so translate with the same POWERarm binary and
# settings you run with.
#
# Each ELF is one POWERarm process (POWERARM_AOTTRANSLATE=<mode>) at nice 19,
# JOBS at a time (default: CPUs in this process's affinity mask). Pin the
# script with taskset to keep it off guest cores. Mode "all" (default) seeds
# function entries and call return points; "entries" only function entries,
# about a tenth of the cache size. Already cached blocks are skipped, so a
# rerun only adds what is new.
#
# -s MAXBYTES skips ELFs larger than MAXBYTES. That is the cold-run policy
# that pays: the short-lived tools a build spawns (sh, the gcc driver, as,
# ar, ld, collect2, coreutils, libc) spend a large share of every run in
# translation and their cached code is small, while one giant binary such as
# cc1 (39 MB; 1.2 GiB of cache in mode "all") is re-run often enough inside a
# build to be warm anyway. See CODE-CACHE.md, "Pre-translating the tools a
# build spawns".
set -u
JOBS=$(nproc)
MODE=all
MAXSIZE=0
while getopts "j:m:s:" opt; do
  case $opt in
    j) JOBS=$OPTARG ;;
    m) MODE=$OPTARG ;;
    s) MAXSIZE=$OPTARG ;;
    *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))
if [ $# -lt 1 ] || [ -z "${POWERARM_ROOTFS:-}" ]; then
  echo "usage: POWERARM_ROOTFS=<rootfs> $0 [-j JOBS] [-m all|entries] [-s MAXBYTES] <POWERarm> [guest path ...]" >&2
  exit 2
fi
case $MAXSIZE in *[!0-9]*) echo "aot-translate: -s wants a byte count" >&2; exit 2 ;; esac
EMU=$(realpath "$1"); shift
ROOT=$(realpath "$POWERARM_ROOTFS")
[ $# -gt 0 ] || set -- /usr/bin /usr/lib

LIST=$(mktemp)
trap 'rm -f "$LIST"' EXIT

# AArch64 ET_EXEC or ET_DYN: bytes 0-3 magic, 16 e_type, 18-19 e_machine (183).
is_elf() {
  local h
  h=$(od -An -tx1 -N20 "$1" 2>/dev/null | tr -d ' \n')
  [ "${h:0:8}" = 7f454c46 ] && { [ "${h:32:2}" = 02 ] || [ "${h:32:2}" = 03 ]; } && [ "${h:36:4}" = b700 ]
}

for p in "$@"; do
  h=$ROOT/${p#/}
  if [ -d "$h" ]; then
    find "$h" -xdev -type f \( -perm -u+x -o -name '*.so*' \) -print
  else
    realpath -e "$h" 2>/dev/null || echo "aot-translate: $p: not found" >&2
  fi
done | while read -r f; do
  f=$(realpath -e "$f") || continue
  case $f in "$ROOT"/*) ;; *) continue ;; esac
  sz=$(stat -c %s "$f") || continue
  [ "$MAXSIZE" -eq 0 ] || [ "$sz" -le "$MAXSIZE" ] || continue
  is_elf "$f" && printf '%s %s\n' "$sz" "/${f#"$ROOT"/}"
done | sort -u -k2,2 | sort -rn -k1,1 | cut -d' ' -f2- > "$LIST"

N=$(wc -l < "$LIST")
echo "aot-translate: $N files, $JOBS jobs, mode $MODE, size cap $MAXSIZE" >&2
s=$(date +%s.%N)
tr '\n' '\0' < "$LIST" | POWERARM_AOTTRANSLATE=$MODE xargs -0 -r -P "$JOBS" -I{} \
  nice -n 19 timeout 1800 "$EMU" {}
rc=$?
printf "aot-translate: done rc=%s in %.1fs\n" "$rc" "$(echo "$(date +%s.%N) - $s" | bc)" >&2
exit $rc
