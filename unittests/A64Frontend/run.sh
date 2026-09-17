#!/bin/sh
# SPDX-License-Identifier: MIT
#
# POWER host side: run every test built by golden.sh under POWERarm and
# compare stdout and exit status with the Pi.
#   run.sh POWERARM_BINARY OUTDIR
#
# Differential tests (a .golden file) must match byte for byte. sysreg is
# self-checking: its golden is not used (the presented ID registers differ
# from the Pi's by design); every line must be PASS except the positive
# control `control-deliberately-wrong`, which must be FAIL.
#
# A second positive control corrupts one character of a copy of a golden and
# requires the comparison to report it.
set -u
emu=${1:?usage: run.sh POWERARM_BINARY OUTDIR}
out=${2:?usage: run.sh POWERARM_BINARY OUTDIR}
cd "$out" || exit 2

pass=0
fail=0
report() {
  if [ "$1" = PASS ]; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
  echo "$1 $2${3:+ ($3)}"
}

run_emu() {
  POWERARM_HOSTPAGEMODE=${POWERARM_HOSTPAGEMODE-force} "$emu" "./$1" > "$1.powerarm" 2> "$1.stderr"
  echo $? > "$1.powerarm.rc"
}

for golden in *.golden; do
  t=${golden%.golden}
  run_emu "$t"
  if [ "$t" = sysreg ]; then
    fails=$(grep -c '^FAIL' sysreg.powerarm)
    control=$(grep -c '^FAIL control-deliberately-wrong' sysreg.powerarm)
    passes=$(grep -c '^PASS' sysreg.powerarm)
    if [ "$(cat sysreg.powerarm.rc)" = 0 ] && [ "$fails" = 1 ] && [ "$control" = 1 ] && [ "$passes" -gt 0 ]; then
      report PASS sysreg "$passes checks, positive control FAILed as required"
    else
      report FAIL sysreg "rc=$(cat sysreg.powerarm.rc) fails=$fails control=$control; see $out/sysreg.powerarm"
    fi
    continue
  fi
  if ! cmp -s "$golden" "$t.powerarm"; then
    line=$(cmp "$golden" "$t.powerarm" 2>&1 | head -1)
    report FAIL "$t" "stdout differs: $line"
  elif [ "$(cat "$t.rc")" != "$(cat "$t.powerarm.rc")" ]; then
    report FAIL "$t" "exit status $(cat "$t.powerarm.rc"), Pi $(cat "$t.rc")"
  else
    report PASS "$t" "$(wc -l < "$golden") lines, exit $(cat "$t.rc")"
  fi
done

# Positive control for the comparison itself.
if [ -f addsub.powerarm ]; then
  sed '5s/^#\(.\)/#X/' addsub.golden > control.corrupted
  if cmp -s control.corrupted addsub.powerarm; then
    report FAIL comparison-control "a corrupted golden compared equal"
  else
    report PASS comparison-control "corrupted golden reported as a mismatch"
  fi
fi

echo "passed $pass, failed $fail"
[ "$fail" = 0 ]
