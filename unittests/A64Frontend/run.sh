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

# <test>.bin and <test>.args, when present, name the binary and its
# arguments (the busybox applet tests); otherwise ./<test> runs bare.
# <test>.env holds POWERarm configuration that one test needs
# (NAME=value words, e.g. POWERARM_NEEDSSECCOMP=1 for vdso_syscalls).
run_emu() {
  bin=$1
  args=
  envs=
  [ -f "$1.bin" ] && bin=$(cat "$1.bin")
  [ -f "$1.args" ] && args=$(cat "$1.args")
  [ -f "$1.env" ] && envs=$(cat "$1.env")
  # shellcheck disable=SC2086
  (
    [ -n "$envs" ] && export $envs
    exec "$emu" "./$bin" $args
  ) > "$1.powerarm" 2> "$1.stderr"
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

# Positive controls for the comparison itself: one corrupted GPR line and
# one corrupted vector line (a single hex digit of V17 in the second case).
if [ -f addsub.powerarm ]; then
  sed '5s/^#\(.\)/#X/' addsub.golden > control.corrupted
  if cmp -s control.corrupted addsub.powerarm; then
    report FAIL comparison-control "a corrupted golden compared equal"
  else
    report PASS comparison-control "corrupted golden reported as a mismatch"
  fi
fi
if [ -f fp_scalar.powerarm ]; then
  awk 'NR == 4 { split($0, f, " "); d = substr(f[19], 32, 1); r = (d == "0") ? "1" : "0"; f[19] = substr(f[19], 1, 31) r; $0 = ""; for (i = 1; i <= length(f); i++) $0 = $0 (i > 1 ? " " : "") f[i] } { print }' \
    fp_scalar.golden > control.vcorrupted
  if cmp -s control.vcorrupted fp_scalar.powerarm || cmp -s control.vcorrupted fp_scalar.golden; then
    report FAIL vector-comparison-control "a corrupted vector golden compared equal"
  else
    report PASS vector-comparison-control "corrupted V17 digit reported as a mismatch"
  fi
fi

echo "passed $pass, failed $fail"
[ "$fail" = 0 ]
