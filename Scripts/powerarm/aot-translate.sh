#!/bin/bash
# aot-translate.sh: translate RootFS binaries into the POWERarm code cache
# ahead of time, so their first run loads blocks instead of compiling them.
#
#   aot-translate.sh [-j JOBS] [-m all|entries] <POWERarm> [guest path or dir ...]
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
# about a seventh of the cache size. Already cached blocks are skipped, so a
# rerun only adds what is new.
set -u
JOBS=$(nproc)
MODE=all
while getopts "j:m:" opt; do
  case $opt in
    j) JOBS=$OPTARG ;;
    m) MODE=$OPTARG ;;
    *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))
if [ $# -lt 1 ] || [ -z "${POWERARM_ROOTFS:-}" ]; then
  echo "usage: POWERARM_ROOTFS=<rootfs> $0 [-j JOBS] [-m all|entries] <POWERarm> [guest path ...]" >&2
  exit 2
fi
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
  is_elf "$f" && printf '%s %s\n' "$(stat -c %s "$f")" "/${f#"$ROOT"/}"
done | sort -u -k2,2 | sort -rn -k1,1 | cut -d' ' -f2- > "$LIST"

N=$(wc -l < "$LIST")
echo "aot-translate: $N files, $JOBS jobs, mode $MODE" >&2
s=$(date +%s.%N)
tr '\n' '\0' < "$LIST" | POWERARM_AOTTRANSLATE=$MODE xargs -0 -r -P "$JOBS" -I{} \
  nice -n 19 timeout 1800 "$EMU" {}
rc=$?
printf "aot-translate: done rc=%s in %.1fs\n" "$rc" "$(echo "$(date +%s.%N) - $s" | bc)" >&2
exit $rc
