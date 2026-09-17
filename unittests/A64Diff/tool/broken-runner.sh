#!/bin/sh
# SPDX-License-Identifier: MIT
# Negative control for the A64Diff harness: a deliberately wrong "emulator".
#
#   broken-runner.sh TEST_ELF | PROGRAM [ARGS...]
#
# Instruction tests: copies the test, damages the first instruction under test,
# and runs the copy natively.  A correct harness must report these tests as
# mismatches.
#   A64DIFF_BREAK=nop   replace the instruction with NOP (the "skipped insn" bug)
#   A64DIFF_BREAK=bit0  flip bit 0 of the instruction (usually the destination register)
#   A64DIFF_BREAK=none  run unmodified (must pass: proves the wrapper itself is neutral)
#   A64DIFF_BREAK=trunc program jobs: keep only the first 64 bytes of stdout
# Only tests whose id matches A64DIFF_BREAK_MATCH (a grep -E pattern, default all)
# are damaged.
set -eu
elf=$1
shift
id=$(basename "$elf")
manifest=$(dirname "$elf")/../manifest.tsv
mode=${A64DIFF_BREAK:-nop}
match=${A64DIFF_BREAK_MATCH:-.}
if [ "$mode" = none ] || ! printf '%s\n' "$id" | grep -Eq -- "$match"; then
  exec "$elf" "$@"
fi
if [ "$mode" = trunc ]; then
  "$elf" "$@" | head -c 64
  exit 0
fi
line=$(awk -F'\t' -v id="$id" '$1 == id { print $6, $7; exit }' "$manifest")
addr=${line%% *}
enc=${line#* }
enc=${enc%%,*}
[ -n "$addr" ] || { echo "broken-runner: $id not in manifest" >&2; exit 125; }
[ "$enc" != "-" ] || exec "$elf"   # no instruction under test (identity class)
# Text is the first PT_LOAD: file offset 0 at vaddr 0x400000 (checked by the golden script).
off=$((addr - 0x400000))
case $mode in
  nop) word=0xd503201f ;;
  bit0) word=$((0x$enc ^ 1)) ;;
  *) echo "broken-runner: unknown A64DIFF_BREAK=$mode" >&2; exit 125 ;;
esac
tmp=$(mktemp "${TMPDIR:-/tmp}/a64diff-broken.XXXXXX")
trap 'rm -f "$tmp"' EXIT
cp "$elf" "$tmp"
chmod 700 "$tmp"
printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $((word & 255)) $((word >> 8 & 255)) $((word >> 16 & 255)) $((word >> 24 & 255)))" |
  dd of="$tmp" bs=1 seek="$off" conv=notrunc status=none
"$tmp"
