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
skip=0
report() {
  case $1 in
  PASS) pass=$((pass + 1)) ;;
  SKIP) skip=$((skip + 1)) ;;
  *) fail=$((fail + 1)) ;;
  esac
  echo "$1 $2${3:+ ($3)}"
}

# The guest vDSO (vdso, vdso_syscalls) is libVDSO-a64-guest.so, which only a
# -DBUILD_THUNKS=ON build produces, in <build>/Guest. Point POWERarm at the one
# next to the emulator under test unless the caller already chose, so a
# thunk build gates itself; a build without one SKIPs those two tests,
# visibly and with the reason, instead of failing them.
if [ -z "${POWERARM_THUNKGUESTLIBS:-}" ]; then
  if guest=$(cd "$(dirname "$emu")/../Guest" 2>/dev/null && pwd) && [ -f "$guest/libVDSO-a64-guest.so" ]; then
    export POWERARM_THUNKGUESTLIBS="$guest"
  fi
fi
have_vdso=0
[ -n "${POWERARM_THUNKGUESTLIBS:-}" ] && [ -f "$POWERARM_THUNKGUESTLIBS/libVDSO-a64-guest.so" ] && have_vdso=1

# <test>.bin and <test>.args, when present, name the binary and its
# arguments (the busybox applet tests); otherwise ./<test> runs bare.
# <test>.env holds environment one test needs under POWERarm (NAME=value
# words, e.g. POWERARM_NEEDSSECCOMP=1 for vdso_syscalls); the guest sees it too.
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

# A tree's names, types, modes, sizes, mtimes, link targets and file hashes.
snapshot() {
  (cd "$1" && find . -printf '%y %m %s %T@ %p %l\n' | LC_ALL=C sort && find . -type f -exec sha256sum {} + | LC_ALL=C sort)
}

# rootfs_overlay runs with its own fixture as the base rootfs and an empty
# overlay. Its stdout must match the Pi's (the same operations on a plain
# directory there), and the host must see: the base unchanged, the changes in
# the overlay, a host tool the base lacks still reachable, and with the
# overlay disabled or absent the old fallthrough to the host's pacman state.
run_rootfs_overlay() {
  ovt=$(mktemp -d "${TMPDIR:-/tmp}/rootfs_overlay.XXXXXX")
  "$emu" ./rootfs_overlay --make-fixture "$ovt/base" > rootfs_overlay.stderr 2>&1
  mkdir "$ovt/overlay"
  snapshot "$ovt/base" > "$ovt/before"
  POWERARM_ROOTFS=$ovt/base POWERARM_ROOTFSOVERLAY=$ovt/overlay "$emu" ./rootfs_overlay > rootfs_overlay.powerarm 2>> rootfs_overlay.stderr
  echo $? > rootfs_overlay.powerarm.rc
  snapshot "$ovt/base" > "$ovt/after"
  tool=$(POWERARM_ROOTFS=$ovt/base POWERARM_ROOTFSOVERLAY=$ovt/overlay "$emu" ./rootfs_overlay --probe /usr/bin/env 2>&1)
  hidden=$(POWERARM_ROOTFS=$ovt/base POWERARM_ROOTFSOVERLAY=$ovt/overlay "$emu" ./rootfs_overlay --probe /var/lib/pacman 2>&1)
  knob=$(POWERARM_ROOTFS=$ovt/base POWERARM_ROOTFSOVERLAY=0 "$emu" ./rootfs_overlay --probe /var/lib/pacman 2>&1)
  absent=$(env -u POWERARM_ROOTFSOVERLAY POWERARM_ROOTFS="$ovt/base" "$emu" ./rootfs_overlay --probe /var/lib/pacman 2>&1)
  host_pacman=ENOENT
  [ -d /var/lib/pacman ] && host_pacman=exists
  if ! cmp -s rootfs_overlay.golden rootfs_overlay.powerarm; then
    report FAIL rootfs_overlay "stdout differs: $(cmp rootfs_overlay.golden rootfs_overlay.powerarm 2>&1 | head -1)"
  elif [ "$(cat rootfs_overlay.rc)" != "$(cat rootfs_overlay.powerarm.rc)" ]; then
    report FAIL rootfs_overlay "exit status $(cat rootfs_overlay.powerarm.rc), Pi $(cat rootfs_overlay.rc)"
  elif ! cmp -s "$ovt/before" "$ovt/after"; then
    report FAIL rootfs_overlay "the base rootfs changed: $(diff "$ovt/before" "$ovt/after" | head -3 | tr '\n' ' ')"
  elif [ ! -f "$ovt/overlay/usr/share/ovtest/.wh.rename.txt" ] || [ ! -f "$ovt/overlay/usr/share/ovtest/modify.txt" ]; then
    report FAIL rootfs_overlay "the overlay lacks the whiteout or the copied-up file"
  elif [ "$tool" != exists ]; then
    report FAIL rootfs_overlay "host /usr/bin/env unreachable through the overlay: $tool"
  elif [ "$hidden" != ENOENT ]; then
    report FAIL rootfs_overlay "/var/lib/pacman fell through to the host: $hidden"
  elif [ "$knob" != "$host_pacman" ] || [ "$absent" != "$host_pacman" ]; then
    report FAIL rootfs_overlay "disabled ($knob) or absent ($absent) overlay changed the host fallthrough ($host_pacman)"
  else
    report PASS rootfs_overlay "$(grep -c '^PASS' rootfs_overlay.powerarm) checks, base unchanged"
  fi
  rm -rf "$ovt"
}

for golden in *.golden; do
  t=${golden%.golden}
  if [ "$t" = rootfs_overlay ]; then
    run_rootfs_overlay
    continue
  fi
  case $t in
  vdso | vdso_syscalls)
    if [ "$have_vdso" = 0 ]; then
      report SKIP "$t" "no guest vDSO: build with -DBUILD_THUNKS=ON or set POWERARM_THUNKGUESTLIBS"
      continue
    fi
    ;;
  esac
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

if [ "$skip" -gt 0 ]; then
  echo "passed $pass, failed $fail, skipped $skip"
else
  echo "passed $pass, failed $fail"
fi
[ "$fail" = 0 ]
