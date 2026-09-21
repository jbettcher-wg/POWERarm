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
#
# hoststack runs with ASLR off and an unlimited stack rlimit. That is the
# layout in which the ELF loader used to put the guest's main stack flush
# against the host stack every time, so its nested signal handlers drove host
# frames into the guest stack. With a hard stack limit below unlimited the
# layout is not forced and the result says so.
#
# forkexec runs with POWERARM_PORTABLE, the code cache on and a fresh cache
# directory of its own (see forkexec.c).
hoststack_forced=yes
[ "$(ulimit -Hs)" = unlimited ] || hoststack_forced=no
run_emu() {
  bin=$1
  args=
  envs=
  [ -f "$1.bin" ] && bin=$(cat "$1.bin")
  [ -f "$1.args" ] && args=$(cat "$1.args")
  [ -f "$1.env" ] && envs=$(cat "$1.env")
  # shellcheck disable=SC2086
  {
    (
      ulimit -c 0 2> /dev/null
      [ -n "$envs" ] && export $envs
      if [ "$1" = hoststack ]; then
        ulimit -s unlimited 2> /dev/null
        exec setarch -R "$emu" "./$bin" $args
      fi
      if [ "$1" = forkexec ]; then
        # Its children exec it again, which must stay on this build
        # (POWERARM_PORTABLE), and save, fill and compact a code cache of their
        # own on every run.
        fecache=$(mktemp -d "${TMPDIR:-/tmp}/forkexec-cache.XXXXXX")
        POWERARM_PORTABLE=1 POWERARM_ENABLECODECACHINGWIP=1 POWERARM_CODECACHESCOPE=all POWERARM_APP_CACHE_LOCATION="$fecache/" \
          "$emu" "./$bin" $args
        rc=$?
        rm -rf "$fecache"
        exit $rc
      fi
      exec "$emu" "./$bin" $args
    ) > "$1.powerarm" 2> "$1.stderr"
  } 2> /dev/null
  echo $? > "$1.powerarm.rc"
}

# A tree's names, types, modes, sizes, mtimes, link targets and file hashes.
snapshot() {
  (cd "$1" && find . -printf '%y %m %s %T@ %p %l\n' | LC_ALL=C sort && find . -type f -exec sha256sum {} + | LC_ALL=C sort)
}

# rootfs_overlay runs with its own fixture as the base rootfs and an empty
# overlay, once as an ordinary program and once sealed. Both stdouts must
# match the Pi's (the same operations on a plain directory there), and the
# host must see: the base unchanged, the changes in the overlay and none on
# the host, a host tool the base lacks still reachable except from a sealed
# process (the package manager, by default), a host file changed and deleted
# only in the overlay, and with the overlay disabled or absent the old
# fallthrough to the host's pacman state.
run_rootfs_overlay() {
  ovt=$(mktemp -d "${TMPDIR:-/tmp}/rootfs_overlay.XXXXXX")
  # ovt_emu OVERLAY SEAL PROGRAM ARGS...: SEAL is on, off, or - for the default.
  ovt_emu() {
    (
      export POWERARM_ROOTFS="$ovt/base" POWERARM_ROOTFSOVERLAY="$1"
      if [ "$2" = - ]; then unset POWERARM_ROOTFSOVERLAYSEAL; else export POWERARM_ROOTFSOVERLAYSEAL="$2"; fi
      shift 2
      exec "$emu" "$@"
    )
  }
  # What the host itself has at a path the fixture base lacks (the shell
  # running this script may see a rootfs of its own there).
  host_stat() {
    ovt_emu 0 - ./rootfs_overlay --stat "$1" 2>&1
  }
  "$emu" ./rootfs_overlay --make-fixture "$ovt/base" > rootfs_overlay.stderr 2>&1
  mkdir "$ovt/overlay" "$ovt/sealed" "$ovt/pm" "$ovt/hostfile" "$ovt/install"
  cp ./rootfs_overlay "$ovt/pacman"
  snapshot "$ovt/base" > "$ovt/before"
  hostdir_before=$(host_stat /usr/bin/ovtest.d)
  ovt_emu "$ovt/overlay" - ./rootfs_overlay > rootfs_overlay.powerarm 2>> rootfs_overlay.stderr
  echo $? > rootfs_overlay.powerarm.rc
  ovt_emu "$ovt/sealed" on ./rootfs_overlay > rootfs_overlay.sealed 2>> rootfs_overlay.stderr
  echo $? > rootfs_overlay.sealed.rc
  snapshot "$ovt/base" > "$ovt/after"
  hostdir_after=$(host_stat /usr/bin/ovtest.d)
  tool=$(ovt_emu "$ovt/overlay" - ./rootfs_overlay --probe /usr/bin/env 2>&1)
  hidden=$(ovt_emu "$ovt/overlay" - ./rootfs_overlay --probe /var/lib/pacman 2>&1)
  # Sealed: on for every program, auto only for pacman.
  seal_on=$(ovt_emu "$ovt/pm" on ./rootfs_overlay --probe /usr/bin/env 2>&1)
  seal_pm=$(ovt_emu "$ovt/pm" - "$ovt/pacman" --probe /usr/bin/env 2>&1)
  seal_pm_off=$(ovt_emu "$ovt/pm" off "$ovt/pacman" --probe /usr/bin/env 2>&1)
  knob=$(ovt_emu 0 - ./rootfs_overlay --probe /var/lib/pacman 2>&1)
  absent=$(env -u POWERARM_ROOTFSOVERLAY -u POWERARM_ROOTFSOVERLAYSEAL POWERARM_ROOTFS="$ovt/base" "$emu" ./rootfs_overlay --probe /var/lib/pacman 2>&1)
  # getcwd inside the base reads as the guest path, with or without the
  # overlay; a host directory the base lacks reads as itself.
  cwd_on=$(ovt_emu "$ovt/overlay" - ./rootfs_overlay --cwd /usr/share/ovtest/sub 2>&1)
  cwd_off=$(ovt_emu 0 - ./rootfs_overlay --cwd /usr/share/ovtest/sub 2>&1)
  cwd_host=$(ovt_emu 0 - ./rootfs_overlay --cwd /usr/bin 2>&1)
  host_pacman=ENOENT
  [ -d /var/lib/pacman ] && host_pacman=exists
  # A host file the base lacks, under a guest-owned prefix: changed through a
  # read-only descriptor and deleted by an ordinary program, installed over
  # and removed by a sealed one; the host copy never changes. Not as root,
  # where a regression would change the host's own file.
  hostfile=/usr/lib/os-release
  hf_before=$(host_stat "$hostfile")
  hf=skipped
  if [ "$(id -u)" != 0 ] && [ "$hf_before" != ENOENT ]; then
    hf=$(ovt_emu "$ovt/hostfile" off ./rootfs_overlay --host-file "$hostfile" 2>&1)
    hf_conflict=$(ovt_emu "$ovt/hostfile" off ./rootfs_overlay --install /usr/bin/env 2>&1)
    hf_install=$(ovt_emu "$ovt/install" on ./rootfs_overlay --install "$hostfile" 2>&1)
    hf_installed=$(cat "$ovt/install$hostfile" 2>&1)
    hf_remove=$(ovt_emu "$ovt/install" on ./rootfs_overlay --remove "$hostfile" 2>&1)
    hf_back=$(ovt_emu "$ovt/install" off ./rootfs_overlay --probe "$hostfile" 2>&1)
    hf_after=$(host_stat "$hostfile")
  fi
  hf_name=${hostfile##*/}
  hf_dir=${hostfile%/*}
  if ! cmp -s rootfs_overlay.golden rootfs_overlay.powerarm; then
    report FAIL rootfs_overlay "stdout differs: $(cmp rootfs_overlay.golden rootfs_overlay.powerarm 2>&1 | head -1)"
  elif ! cmp -s rootfs_overlay.golden rootfs_overlay.sealed; then
    report FAIL rootfs_overlay "sealed stdout differs: $(cmp rootfs_overlay.golden rootfs_overlay.sealed 2>&1 | head -1)"
  elif [ "$(cat rootfs_overlay.rc)" != "$(cat rootfs_overlay.powerarm.rc)" ] || [ "$(cat rootfs_overlay.rc)" != "$(cat rootfs_overlay.sealed.rc)" ]; then
    report FAIL rootfs_overlay "exit status $(cat rootfs_overlay.powerarm.rc), sealed $(cat rootfs_overlay.sealed.rc), Pi $(cat rootfs_overlay.rc)"
  elif ! cmp -s "$ovt/before" "$ovt/after"; then
    report FAIL rootfs_overlay "the base rootfs changed: $(diff "$ovt/before" "$ovt/after" | head -3 | tr '\n' ' ')"
  elif [ ! -f "$ovt/overlay/usr/share/ovtest/.wh.rename.txt" ] || [ ! -f "$ovt/overlay/usr/share/ovtest/modify.txt" ]; then
    report FAIL rootfs_overlay "the overlay lacks the whiteout or the copied-up file"
  elif [ ! -f "$ovt/overlay/usr/bin/ovtest.d/kept" ] || [ ! -f "$ovt/sealed/usr/bin/ovtest.d/kept" ] || [ "$hostdir_after" != ENOENT ]; then
    report FAIL rootfs_overlay "a file made in a host-only directory missed the overlay (host now: $hostdir_after, before: $hostdir_before)"
  elif [ "$tool" != exists ]; then
    report FAIL rootfs_overlay "host /usr/bin/env unreachable through the overlay: $tool"
  elif [ "$seal_on" != ENOENT ] || [ "$seal_pm" != ENOENT ] || [ "$seal_pm_off" != exists ]; then
    report FAIL rootfs_overlay "host /usr/bin/env through the seal: on [$seal_on] pacman [$seal_pm] pacman, seal off [$seal_pm_off]"
  elif [ "$hidden" != ENOENT ]; then
    report FAIL rootfs_overlay "/var/lib/pacman fell through to the host: $hidden"
  elif [ "$cwd_on" != "/usr/share/ovtest/sub ERANGE" ] || [ "$cwd_off" != "/usr/share/ovtest/sub ERANGE" ] || [ "$cwd_host" != "/usr/bin ERANGE" ]; then
    report FAIL rootfs_overlay "getcwd leaked a host path: overlay [$cwd_on] no overlay [$cwd_off] host dir [$cwd_host]"
  elif [ "$knob" != "$host_pacman" ] || [ "$absent" != "$host_pacman" ]; then
    report FAIL rootfs_overlay "disabled ($knob) or absent ($absent) overlay changed the host fallthrough ($host_pacman)"
  elif [ "$hf" != skipped ] && { [ "$hf" != "fchmod=1 futimens=1 seen=1 unlink=1" ] || [ ! -f "$ovt/hostfile$hf_dir/.wh.$hf_name" ] || [ "$hf_conflict" != EEXIST ]; }; then
    report FAIL rootfs_overlay "host $hostfile through a descriptor: [$hf], whiteout $(ls -a "$ovt/hostfile$hf_dir" 2>&1 | tr '\n' ' '), install unsealed [$hf_conflict]"
  elif [ "$hf" != skipped ] && { [ "$hf_install" != installed ] || [ "$hf_installed" != guest ] || [ "$hf_remove" != removed ] || [ "$hf_back" != exists ] || [ -e "$ovt/install$hostfile" ] || [ -e "$ovt/install$hf_dir/.wh.$hf_name" ]; }; then
    report FAIL rootfs_overlay "sealed install over host $hostfile: [$hf_install] content [$hf_installed] remove [$hf_remove] host visible again [$hf_back]"
  elif [ "$hf" != skipped ] && [ "$hf_after" != "$hf_before" ]; then
    report FAIL rootfs_overlay "the host's $hostfile changed: [$hf_before] -> [$hf_after]"
  else
    note=
    [ "$hf" = skipped ] && note=", host-file checks skipped (root, or no $hostfile)"
    report PASS rootfs_overlay "$(grep -c '^PASS' rootfs_overlay.powerarm) checks unsealed and sealed, base and host unchanged$note"
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
    note=
    [ "$t" = hoststack ] && [ "$hoststack_forced" = no ] && note=", layout not forced (hard stack limit $(ulimit -Hs))"
    report PASS "$t" "$(wc -l < "$golden") lines, exit $(cat "$t.rc")$note"
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

# A guest started under an address-space limit (glycin runs GTK's image loaders
# under bwrap with RLIMIT_AS; `ulimit -v` does it for a shell). POWERarm's 48-bit
# reservation used to fail there and crash startup before the guest ran.
if [ -f hello.golden ]; then
  (ulimit -v 4000000 && "$emu" ./hello) > rlimit_as.powerarm 2> rlimit_as.stderr
  echo $? > rlimit_as.powerarm.rc
  if [ "$(cat rlimit_as.powerarm.rc)" = 0 ] && cmp -s hello.golden rlimit_as.powerarm; then
    report PASS rlimit_as "hello under RLIMIT_AS=4 GB"
  else
    report FAIL rlimit_as "hello under RLIMIT_AS=4 GB: rc=$(cat rlimit_as.powerarm.rc)"
  fi
fi

# The fatal host-fault report survives a fault of its own. hostfault makes
# POWERarm fault in its own syscall body (POWERARM_HOSTFAULT_INJECT, 173 is
# getppid) under a return address the unwinder cannot read, once with fault
# handlers like a crash reporter's (SA_NODEFER) and once without. Each run
# must print the report's line first and once, and the unwinder's fault note,
# never run the guest's handler, and end with the original SIGSEGV. The report
# used to re-enter itself through the unwinder's fault, or die in it.
if [ -f hostfault.golden ]; then
  hf_fail=
  for variant in handlers --no-handlers; do
    set --
    [ "$variant" = --no-handlers ] && set -- --no-handlers
    # (The braces keep the shell's own "Segmentation fault" notice off the output.)
    { (ulimit -c 0 && POWERARM_HOSTFAULT_INJECT=173,segv,unwind exec "$emu" ./hostfault "$@") > hostfault_report.powerarm 2> hostfault_report.stderr; } 2> /dev/null
    rc=$?
    lines=$(grep -c 'FATAL host fault' hostfault_report.stderr)
    first=$(head -1 hostfault_report.stderr | cut -c1-37)
    if [ "$rc" != 139 ] || [ "$lines" != 1 ] || [ "$first" != "POWERarm: FATAL host fault: signal 11" ] ||
      ! grep -q 'host backtrace faulted in the unwinder after' hostfault_report.stderr ||
      grep -q -e 'guest handler' -e 'getppid' hostfault_report.powerarm; then
      hf_fail="$hf_fail $variant: rc=$rc report lines=$lines first=[$first];"
    fi
  done
  if [ -z "$hf_fail" ]; then
    report PASS hostfault_report "one report line first, unwinder fault survived, with and without SA_NODEFER handlers"
  else
    report FAIL hostfault_report "${hf_fail% ;}"
  fi
fi

if [ "$skip" -gt 0 ]; then
  echo "passed $pass, failed $fail, skipped $skip"
else
  echo "passed $pass, failed $fail"
fi
[ "$fail" = 0 ]
