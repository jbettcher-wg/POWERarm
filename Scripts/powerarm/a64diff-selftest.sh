#!/bin/sh
# SPDX-License-Identifier: MIT
# A64Diff negative controls.  Run on the aarch64 golden machine against a
# bundle produced by a64diff-golden.sh:
#
#   a64diff-selftest.sh BUNDLE_ROOT [A64DIFF_TOOL]
#
# Each case states the verdict the harness must reach.  A harness that passes
# a broken runner, or a blind comparator whose controls stay quiet, fails here.
set -eu
root=$(cd "$1" && pwd)
tool=${2:-a64diff}
here=$(cd "$(dirname "$0")" && pwd)
broken=$root/src/broken-runner.sh
w=$(mktemp -d "${TMPDIR:-/tmp}/a64diff-selftest.XXXXXX")
trap 'rm -rf "$w"' EXIT
jobs=$(nproc)
fails=0

# expect NAME WANT_EXIT LOG: check the exit code and that controls behaved.
expect() {
  name=$1 want=$2 got=$3 log=$4 ctl=$5
  summary=$(grep -E '^A64DIFF (INSN|PROGRAMS) SUMMARY' "$log" || echo "(no summary)")
  ok=1
  [ "$got" = "$want" ] || ok=0
  case $ctl in
    all-fired) grep -q 'CONTROL-FAIL' "$log" && ok=0 ;;
    some-fail) grep -q 'CONTROL-FAIL' "$log" || ok=0 ;;
  esac
  if [ $ok = 1 ]; then v=OK; else v=WRONG; fails=$((fails + 1)); fi
  echo "SELFTEST $v $name: exit $got (want $want), controls $ctl"
  echo "    $summary"
}

insn() { # NAME BREAKMODE WANT CTL [compare args]
  name=$1 mode=$2 want=$3 ctl=$4
  shift 4
  A64DIFF_BREAK=$mode "$tool" run --manifest "$root/manifest.tsv" --root "$root" --out "$w/$name" -j "$jobs" -- "$broken" > /dev/null
  set +e
  "$tool" compare --manifest "$root/manifest.tsv" --golden "$root/golden" --actual "$w/$name" --max-detail 0 "$@" > "$w/$name.log" 2>&1
  rc=$?
  set -e
  expect "$name" "$want" "$rc" "$w/$name.log" "$ctl"
}

rootfs_args() {
  for idf in "$root"/rootfs/*.id; do
    [ -f "$idf" ] || continue
    n=$(sed -n 's/^name //p' "$idf")
    [ "$(sed -n 's/^location //p' "$idf")" = bundle ] && printf ' --rootfs %s=%s' "$n" "$root/rootfs/$n"
  done
  printf ' --rootfs-exec %s' "$here/a64diff-rootfs-exec.sh"
}

rootfsjobs() { # NAME WANT_EXIT CTL [run args]
  name=$1 want=$2 ctl=$3
  shift 3
  # shellcheck disable=SC2046
  "$tool" run --jobs "$root/programs/rootfs.jobs" --root "$root" --out "$w/$name" -j "$jobs" $(rootfs_args) "$@" > /dev/null 2>&1
  set +e
  "$tool" pcompare --jobs "$root/programs/rootfs.jobs" --golden "$root/golden-rootfs" --actual "$w/$name" --max-detail 0 \
    --report "$w/$name.report" > "$w/$name.log" 2>&1
  rc=$?
  set -e
  expect "$name" "$want" "$rc" "$w/$name.log" "$ctl"
}

prog() {
  name=$1 mode=$2 want=$3 ctl=$4
  A64DIFF_BREAK=$mode "$tool" run --jobs "$root/programs/programs.jobs" --root "$root" --out "$w/$name" -j "$jobs" -- "$broken" > /dev/null
  set +e
  "$tool" pcompare --jobs "$root/programs/programs.jobs" --golden "$root/golden-programs" --actual "$w/$name" --max-detail 0 > "$w/$name.log" 2>&1
  rc=$?
  set -e
  expect "$name" "$want" "$rc" "$w/$name.log" "$ctl"
}

insn neutral-wrapper none 0 all-fired
insn broken-nop nop 1 all-fired
insn broken-bit0 bit0 1 all-fired
insn blind-nzcv none 2 some-fail --blind-field nzcv
insn blind-x7 none 2 some-fail --blind-field x7
prog prog-neutral-wrapper none 0 all-fired
prog prog-broken-trunc trunc 1 all-fired
if [ -f "$root/programs/rootfs.jobs" ]; then
  rootfsjobs rootfs-neutral 0 all-fired
  rootfsjobs rootfs-broken-outputs 1 all-fired --break-outputs
  # It must fail on the output files, and only there.
  if grep -q '^    output ' "$w/rootfs-broken-outputs.report" && ! grep -qE '^    (status|steps|step [0-9]+ std)' "$w/rootfs-broken-outputs.report"; then
    echo "SELFTEST OK rootfs-broken-outputs: flagged on output files only ($(grep -c '^    output ' "$w/rootfs-broken-outputs.report") files)"
  else
    echo "SELFTEST WRONG rootfs-broken-outputs: not flagged on the output files alone"
    fails=$((fails + 1))
  fi
fi

grep -E '^(identity|dp-imm|bitfield|dp-reg|condsel|condcmp|muldiv|loadstore|pairs|branch|sysreg|hint|sysreg-id|tbi|signals) ' "$w/broken-nop.log" | sed 's/^/    nop: /'
if [ $fails = 0 ]; then
  echo "SELFTEST RESULT=PASS"
else
  echo "SELFTEST RESULT=FAIL ($fails cases gave the wrong verdict)"
  exit 1
fi
