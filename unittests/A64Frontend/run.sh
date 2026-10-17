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

# Re-entry, used only by the threadexit loop near the bottom of this file: it
# runs the whole loop through `setsid` so the runs -- and anything the
# emulator forks off them -- land in a process group of their own, which the
# caller then reaps. Nothing else uses this; cwd is already OUTDIR.
#
# $5 is a scratch directory of this run's own, and it has to be: OUTDIR is the
# golden directory, which is shared, and two suites running at once there used
# to write one another's exit codes into one file. Each truncates it and each
# appends 96 lines, so the count this loop checks became whatever the two runs
# happened to interleave -- 53 of 96 when the other run truncated mid-flight,
# 149 of 96 when its codes landed here -- with every recorded exit still 0.
# Both were read as a regression in process exit, and one of them cost a day
# (open item 51). The pgid file was shared the same way, so one run's reap
# killed the other's process group, or missed its own and left the hung
# emulators that then wrote into the next run's file.
if [ "${1:-}" = --threadexit-loop ]; then
  te_emu=$2
  te_total=$3
  te_batch=$4
  te_work=$5
  echo $$ > "$te_work/pgid"
  te_runs=0
  while [ "$te_runs" -lt "$te_total" ]; do
    te_i=0
    while [ "$te_i" -lt "$te_batch" ] && [ "$te_runs" -lt "$te_total" ]; do
      # (The braces keep the shell's own "Segmentation fault" notice off the output.)
      { (
        ulimit -c 0 2> /dev/null
        # Bounded per run so a hang in the process this loop waits on fails
        # the test instead of blocking the batch forever. 60s against a run
        # that normally takes well under a second, so a timeout means a hang
        # and not a loaded machine; it lands in codes as 124.
        #
        # It does NOT catch the leaked emulators this test leaves behind, and
        # cannot: those are the code cache's forked save writer, which
        # deadlocks inside fork(2) and reparents to init, so the process this
        # loop launched has already exited cleanly and nothing in its lifetime
        # bounds the orphan (HANDOVER item 42's last bullet; the `setsid'
        # above does not cover it either). Measured here 2026-10-15: 40 exits
        # leave 4 survivors with the writer on and 0 with
        # POWERARM_CODECACHEFORKWRITER=0. They accumulate for the box's
        # uptime -- 257 were cleared by hand that day -- so if this box is
        # suddenly full of ./threadexit processes, that is what they are and
        # they are not this test failing.
        timeout -k 5 60 "$te_emu" ./threadexit > /dev/null 2>&1
        echo $? >> "$te_work/codes"
      ) & } 2> /dev/null
      te_i=$((te_i + 1))
      te_runs=$((te_runs + 1))
    done
    wait
  done
  exit 0
fi

emu=${1:?usage: run.sh POWERARM_BINARY OUTDIR}
out=${2:?usage: run.sh POWERARM_BINARY OUTDIR}
# Absolute, so the re-entry above still resolves after the cd.
me=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
cd "$out" || exit 2

# A cache directory of this run's own. Every test here is a fresh binary of a
# fresh build, so with the default directory each suite run (three modes, three
# more per rebuild) wrote hundreds of MiB of namespaces under a new ConfigId
# into the user's cache, where they competed with the apps that cache is for
# until the hour-old sweep removed them. Unset POWERARM_APP_CACHE_LOCATION to
# keep an existing one: a caller that chose a directory keeps it.
if [ -z "${POWERARM_APP_CACHE_LOCATION:-}" ]; then
  suitecache=$(mktemp -d "${TMPDIR:-/tmp}/a64frontend-cache.XXXXXX") || exit 2
  POWERARM_APP_CACHE_LOCATION="$suitecache/"
  export POWERARM_APP_CACHE_LOCATION
  trap 'rm -rf "$suitecache"' EXIT INT TERM
fi

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
  elif guest=$(cd "$(dirname "$emu")/../GuestThunks" 2>/dev/null && pwd) && [ -f "$guest/libVDSO-a64-guest.so" ]; then
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
        POWERARM_PORTABLE=1 POWERARM_ROOTFS="" POWERARM_ENABLECODECACHINGWIP=1 POWERARM_CODECACHESCOPE=all POWERARM_APP_CACHE_LOCATION="$fecache/" \
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

# readlink(2)/readlinkat(2) errno and target fidelity (readlinkerr.c), run
# twice: once with its fixture as the rootfs, so FileManager::Readlink{,at}'s
# openat2(RESOLVE_IN_ROOT) trampoline answers every path, and once against a
# host path the rootfs lacks, so the host fallback does. Every answer is forced
# by POSIX plus Linux's documented behaviour, so the two must agree with each
# other and with the Pi. The O_DIRECTORY fast path answers a directory out of
# the openat2 alone and a symlink -- including a symlink to a directory -- must
# still fall through to the full probe and come back with its target, which is
# what these two runs together pin down.
#
# Self-checking as well as differential: with no Pi golden the matrix still has
# to be all-PASS, which is the threadleak/sysreg precedent.
run_readlinkerr() {
  rlt=$(mktemp -d "${TMPDIR:-/tmp}/readlinkerr.XXXXXX")
  "$emu" ./readlinkerr --make-fixture "$rlt/rootfs" > readlinkerr.stderr 2>&1
  "$emu" ./readlinkerr --make-fixture "$rlt/host" >> readlinkerr.stderr 2>&1
  POWERARM_ROOTFS="$rlt/rootfs" "$emu" ./readlinkerr > readlinkerr.powerarm 2>> readlinkerr.stderr
  echo $? > readlinkerr.powerarm.rc
  POWERARM_ROOTFS="$rlt/rootfs" RLFIX_ROOT="$rlt/host" "$emu" ./readlinkerr > readlinkerr.host 2>> readlinkerr.stderr
  echo $? > readlinkerr.host.rc
  rl_pass=$(grep -c '^PASS' readlinkerr.powerarm)
  rl_fail=$(grep -c '^FAIL' readlinkerr.powerarm)
  if [ "$(cat readlinkerr.powerarm.rc)" != 0 ] || [ "$rl_fail" != 0 ] || [ "$rl_pass" -lt 26 ]; then
    report FAIL readlinkerr "rootfs-answered: rc=$(cat readlinkerr.powerarm.rc) pass=$rl_pass fail=$rl_fail; see $out/readlinkerr.powerarm"
  elif [ "$(cat readlinkerr.host.rc)" != 0 ] || ! cmp -s readlinkerr.powerarm readlinkerr.host; then
    report FAIL readlinkerr "the rootfs and the host answer differently (host rc=$(cat readlinkerr.host.rc)): $(diff readlinkerr.powerarm readlinkerr.host | head -4 | tr '\n' ' ')"
  elif [ -f readlinkerr.golden ] && ! cmp -s readlinkerr.golden readlinkerr.powerarm; then
    report FAIL readlinkerr "stdout differs from the Pi: $(cmp readlinkerr.golden readlinkerr.powerarm 2>&1 | head -1)"
  else
    note=
    [ -f readlinkerr.golden ] || note=", no Pi golden (the answers are architecturally forced; self-checked)"
    report PASS readlinkerr "$rl_pass checks, the rootfs and the host answer identically$note"
  fi
  rm -rf "$rlt"
}
if [ -f readlinkerr ]; then
  run_readlinkerr
fi

for golden in *.golden; do
  t=${golden%.golden}
  if [ "$t" = rootfs_overlay ]; then
    run_rootfs_overlay
    continue
  fi
  # Handled above: it needs its fixture as the rootfs, not a plain run.
  if [ "$t" = readlinkerr ]; then
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

# EntryNZCVLiveIn, the per-unit IR-header bit meaning "some path from this
# compile unit's entry block reads an NZCV bit before writing it". Nothing
# guest-visible turns on it -- DFCE seeds FLAG_ALL at every unit exit, so the
# flags arrive whatever the bit says, which is why nzcvlate's own PASS lines
# cannot see this and this check exists. The JIT reads it to refuse a direct
# link, and the planned NZCV-exit-deadness work will read it for real.
#
# It was computed from the entry block alone, which is wrong for a unit whose
# reader sits in a LATER block (NZCV-LIVENESS.md 4.2, 17 of cc1's 110,706 units
# at -O2). nzcvlate has one unit of each kind and prints their entry addresses
# on its first line; required bits are 1 (late, the case an entry-block-only
# rule gets wrong), 1 (reads in the entry block) and 0 (entry block writes every
# bit first). The last two are the controls: without them a check that only
# demanded a 1 would pass an implementation that always answered 1.
#
# The code cache is off for this run: a cached unit is not compiled, so the pass
# never runs and dumps nothing.
if [ -f nzcvlate.golden ]; then
  a_late=$(awk '/^A /{print $2; exit}' nzcvlate.golden)
  a_early=$(awk '/^A /{print $3; exit}' nzcvlate.golden)
  a_writer=$(awk '/^A /{print $4; exit}' nzcvlate.golden)
  POWERARM_ENABLECODECACHINGWIP=0 POWERARM_DUMPIR=stderr POWERARM_PASSMANAGERDUMPIR=afteropt     "$emu" ./nzcvlate > nzcvlate.livein.out 2> nzcvlate.livein.irdump
  # Last dump of the unit whose OriginalRIP is $1 (a unit can be compiled more
  # than once); prints "<blockcount> <bit>", or nothing if it never compiled.
  unit() {
    awk -v want="#0x$(echo "$1" | sed 's/^0*//'), " '
      $2 == "IRHeader" && index($0, want) {
        blocks = $5; sub(/^#/, "", blocks); sub(/,$/, "", blocks)
        # The header line grew a second trailing bit (ExitsAssumeNZCVDead,
        # NZCV-LIVENESS.md 7.1), so take the digit and stop -- not the rest of
        # the line.
        bit = $0; sub(/.*EntryNZCVLiveIn=/, "", bit); sub(/[^0-9].*$/, "", bit)
        found = blocks " " bit
      }
      END { print found }' nzcvlate.livein.irdump
  }
  late=$(unit "$a_late")
  early=$(unit "$a_early")
  writer=$(unit "$a_writer")
  if [ -z "$late" ] || [ -z "$early" ] || [ -z "$writer" ]; then
    report FAIL nzcv_entry_livein "no IR dump for one of the three units: late=[$late] early=[$early] writer=[$writer]"
  elif [ "${late% *}" != 5 ]; then
    # The whole shape is one unit of five blocks: the entry cbz, its
    # not-taken leg, the block holding the b.ne, and that b.ne's two legs. With
    # the unit size capped (POWERARM_MAXINST) the b.ne is a unit of its OWN, so
    # the late entry reads nothing and 0 is the right answer there -- that mode
    # cannot test this and says so. Any other reason for the count to move means
    # the reader has left the unit and the check has lost its teeth, which is a
    # failure and not a skip.
    if [ -n "${POWERARM_MAXINST:-}" ]; then
      report SKIP nzcv_entry_livein "POWERARM_MAXINST=$POWERARM_MAXINST caps the unit at ${late% *} blocks; the later-block reader is a unit of its own"
    else
      report FAIL nzcv_entry_livein "nzcv_late compiled to ${late% *} blocks, want 5: the later-block reader is no longer inside this unit, so the check tests nothing"
    fi
  elif [ "${late#* }" = 1 ] && [ "${early#* }" = 1 ] && [ "${writer#* }" = 0 ]; then
    report PASS nzcv_entry_livein "late=1 (${late% *} blocks), early=1, writer=0"
  else
    report FAIL nzcv_entry_livein "EntryNZCVLiveIn late=${late#* } want 1, early=${early#* } want 1, writer=${writer#* } want 0; see $out/nzcvlate.livein.irdump"
  fi
fi

# ---------------------------------------------------------------------------
# NZCV exit-deadness (POWERARM_NZCVEXITDEAD; NZCV-LIVENESS.md 7, 9 stage 2).
# ---------------------------------------------------------------------------

# 1. The goldens themselves. exitdead and exitdeadsmc are self-checking AND
#    differential, which means a golden taken from hardware that was failing
#    would compare equal to a POWERarm run that was failing the same way, and
#    the suite would report PASS. One such golden has shipped here before. So
#    require each golden to be all PASS, independently of the comparison.
for ed_t in exitdead exitdeadsmc; do
  [ -f "$ed_t.golden" ] || continue
  ed_pass=$(grep -c '^PASS' "$ed_t.golden")
  ed_fail=$(grep -c '^FAIL' "$ed_t.golden")
  ed_other=$(grep -vc '^PASS' "$ed_t.golden")
  if [ "$ed_fail" = 0 ] && [ "$ed_pass" -gt 0 ] && [ "$ed_other" = 0 ] && [ "$(cat "$ed_t.rc")" = 0 ]; then
    report PASS "${ed_t}_golden" "$ed_pass hardware checks, none failing"
  else
    report FAIL "${ed_t}_golden" "the golden is not all-PASS: pass=$ed_pass fail=$ed_fail other=$ed_other rc=$(cat "$ed_t.rc"); see $out/$ed_t.golden"
  fi
done

# 2. The policy actually fires. With POWERARM_NZCVEXITDEAD=on the guest-code
#    peek must prove deadness at some constant exit of some unit of a program
#    full of compares, and DeadFlagCalculationElimination must act on it --
#    which is what ExitsAssumeNZCVDead in the IR header records. Without this,
#    every `on` and `canary` run in this suite could be a run in which the
#    policy silently did nothing, and a green result would mean nothing.
#
#    The code cache is off for this run: a cached unit is not compiled, so the
#    pass never runs and dumps nothing.
if [ -f cmpbranch.golden ]; then
  POWERARM_NZCVEXITDEAD=on POWERARM_ENABLECODECACHINGWIP=0 POWERARM_DUMPIR=stderr POWERARM_PASSMANAGERDUMPIR=afteropt \
    "$emu" ./cmpbranch > nzcv_exitdead.out 2> nzcv_exitdead.irdump
  ed_units=$(grep -c 'IRHeader' nzcv_exitdead.irdump)
  ed_assume=$(grep -c 'ExitsAssumeNZCVDead=1' nzcv_exitdead.irdump)
  # And with the option off, not one unit may claim it.
  POWERARM_NZCVEXITDEAD=off POWERARM_ENABLECODECACHINGWIP=0 POWERARM_DUMPIR=stderr POWERARM_PASSMANAGERDUMPIR=afteropt \
    "$emu" ./cmpbranch > /dev/null 2> nzcv_exitdead_off.irdump
  ed_off=$(grep -c 'ExitsAssumeNZCVDead=1' nzcv_exitdead_off.irdump)
  if [ "$ed_units" -gt 0 ] && [ "$ed_assume" -gt 0 ] && [ "$ed_off" = 0 ]; then
    report PASS nzcv_exit_dead "on: $ed_assume of $ed_units units assume NZCV dead at an exit; off: 0"
  else
    report FAIL nzcv_exit_dead "units=$ed_units assume_on=$ed_assume assume_off=$ed_off (want >0, >0, 0); see $out/nzcv_exitdead.irdump"
  fi
fi

# 3. Stage 1's gate: the peek's word table against the frontend. POWERARM_NZCVTABLECHECK=1
#    synthesises words for every a64.inc entry with a handler, translates each
#    one alone, classifies the IR with DeadFlagCalculationElimination's own
#    table and requires the peek to agree wherever a disagreement would be
#    unsound -- a word the scan walks through at which the frontend reads NZCV,
#    or a word the table calls a full writer that the frontend does not fully
#    write. Conservative disagreements are counted, not fatal.
if [ -f hello.golden ]; then
  POWERARM_NZCVTABLECHECK=1 "$emu" ./hello > /dev/null 2> nzcv_tablecheck.err
  tc=$(grep '^NZCV_TABLECHECK checked=' nzcv_tablecheck.err | tail -1)
  tc_checked=$(echo "$tc" | sed -n 's/.*checked=\([0-9]*\).*/\1/p')
  tc_unsound=$(echo "$tc" | sed -n 's/.*unsound=\([0-9]*\).*/\1/p')
  tc_entries=$(echo "$tc" | sed -n 's/.*handled_entries=\([0-9]*\).*/\1/p')
  if [ -n "$tc_checked" ] && [ "$tc_checked" -gt 0 ] && [ "$tc_unsound" = 0 ]; then
    report PASS nzcv_table_check "$tc_checked words over $tc_entries handled a64.inc entries, 0 unsound"
  else
    report FAIL nzcv_table_check "[$tc]; see $out/nzcv_tablecheck.err"
  fi
fi

# 4. The tripwire must be silent, on every test in the suite. Each test's stderr
#    is kept beside its stdout, and a contradiction line there means the peek's
#    scan and the frontend's translation disagreed about one fixed sequence of
#    guest words -- a table or walk bug, which is what 7.7 exists to catch.
#    stdout comparison alone cannot see this: under `on` a tripwire firing
#    changes nothing a test prints, and under `strict` it kills the run, which
#    the comparison reports as an exit-status difference and not as what it is.
#
#    exitdeadsmc is the test that used to print here: it rewrites the guest code
#    the verdict was taken from, the way a JIT does, and the detector that kept
#    a process-lifetime record keyed by address reported every such rewrite as a
#    contradiction. See its header and NZCV-LIVENESS.md 7.7.
ed_trip=$(grep -l 'NZCV exit-deadness contradiction' ./*.stderr 2> /dev/null | sed 's|^\./||; s|\.stderr$||' | tr '\n' ' ')
if [ -z "$ed_trip" ]; then
  report PASS nzcv_exit_dead_tripwire "no contradiction reported by any test"
else
  report FAIL nzcv_exit_dead_tripwire "tripwire fired in: ${ed_trip% }; see $out/<test>.stderr"
fi

# 5. No accepted verdict may rest on a witness BELOW its unit's entry.
#
#    The code cache validates a loaded block by hashing
#    [JITCodeTail::RIP, RIP + GuestSize) -- anchored at the unit's ENTRY, while
#    the witness hull is [DecodedMin, DecodedMax) and DecodedMin can be lower.
#    A verdict resting on a witness below the entry therefore loads out of the
#    cache after the guest has rewritten that witness, DEAD verdict intact and
#    wrong, with the pages below the entry not even armed for SMC. That is the
#    stale assumption 7.5 was written to make impossible, and no amount of
#    extent widening fixes it, because the window is not the extent.
#
#    The stats line is the only aggregate this option has (the contradiction
#    counter had no caller at all before), so both halves are asserted here:
#    refused_below_entry may be any number, accepted-below-entry must be zero,
#    and the way to prove the check has teeth is POWERARM_NZCVEXITDEADNOWINDOW=1,
#    which re-opens the hole and must make this same run report a nonzero count.
#    cmpbranch is used because check 2 above already establishes that it reaches
#    the policy at all.
if [ -f cmpbranch.golden ]; then
  # Not in OUTDIR: see the re-entry comment at the top of this file. These two
  # files are read back after they are written, so a second suite running in
  # the same shared golden directory truncates one mid-read and this check
  # fails for a reason that has nothing to do with the policy. Keep them per
  # run; the directory survives a failure so the file can still be read.
  nw_work=$(mktemp -d "${TMPDIR:-/tmp}/nzcv-window.XXXXXX") || exit 2
  POWERARM_NZCVEXITDEAD=on POWERARM_NZCVEXITDEADSTATS=1 POWERARM_ENABLECODECACHINGWIP=0 \
    "$emu" ./cmpbranch > /dev/null 2> "$nw_work/nzcv_window.err"
  POWERARM_NZCVEXITDEAD=on POWERARM_NZCVEXITDEADSTATS=1 POWERARM_NZCVEXITDEADNOWINDOW=1 POWERARM_ENABLECODECACHINGWIP=0 \
    "$emu" ./cmpbranch > /dev/null 2> "$nw_work/nzcv_window_nocheck.err"
  wl=$(grep '^NZCV_EXITDEAD ' "$nw_work/nzcv_window.err" | tail -1)
  wl_no=$(grep '^NZCV_EXITDEAD ' "$nw_work/nzcv_window_nocheck.err" | tail -1)
  w_dead=$(echo "$wl" | sed -n 's/.* dead=\([0-9]*\).*/\1/p')
  w_below=$(echo "$wl" | sed -n 's/.*refused_below_entry=\([0-9]*\).*/\1/p')
  w_below_no=$(echo "$wl_no" | sed -n 's/.*refused_below_entry=\([0-9]*\).*/\1/p')
  w_dead_no=$(echo "$wl_no" | sed -n 's/.* dead=\([0-9]*\).*/\1/p')
  if [ -z "$w_dead" ] || [ -z "$w_below" ] || [ -z "$w_below_no" ]; then
    report FAIL nzcv_exit_dead_window "no NZCV_EXITDEAD stats line: [$wl] [$wl_no]; see $nw_work/nzcv_window.err"
  elif [ "$w_dead" -le 0 ]; then
    report FAIL nzcv_exit_dead_window "no verdict was taken at all (dead=$w_dead), so this check tests nothing; see $nw_work/nzcv_window.err"
  elif [ "$w_below_no" != 0 ]; then
    report FAIL nzcv_exit_dead_window "the lever does not disable the check: refused_below_entry=$w_below_no with POWERARM_NZCVEXITDEADNOWINDOW=1"
  elif [ "$w_below" -gt 0 ] && [ "$w_dead_no" -le "$w_dead" ]; then
    report FAIL nzcv_exit_dead_window "the check refused $w_below verdicts but disabling it did not accept more (dead=$w_dead vs $w_dead_no): it has no teeth"
  else
    report PASS nzcv_exit_dead_window "dead=$w_dead, refused_below_entry=$w_below; lever off: dead=$w_dead_no"
    rm -rf "$nw_work"
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

# Thread teardown racing process exit. threadexit runs once above like any
# other test; that only proves it can pass. The race needs repetition and
# concurrency, so run it again in parallel batches and require every exit to
# be 0. A regression shows up as 139 (SIGSEGV/SI_KERNEL: the kernel could not
# write a signal frame onto an alt stack that had already been freed) or as
# 191 (128 + SIGNAL_FOR_PAUSE: the handler's dead-thread escape had set that
# real-time signal to SIG_DFL process-wide). Both came from the same window
# in SignalDelegator::UninstallTLSState.
#
# The code cache stays at its default (on), and that is load-bearing: with it
# off the race is there but essentially never fires, because nothing else
# holds FEX's allocator mutex long enough for the window to matter. On, the
# exit save's own allocation traffic contends it and roughly half of these
# runs died before the fix. Its cache directory is the suite's own (see
# POWERARM_APP_CACHE_LOCATION at the top of this file).
#
# The loop runs in a session of its own (setsid) and kills that process group
# when it is done. A guest whose threads are still running at exit makes the
# code cache fork a save writer, and such a writer can deadlock inside fork(2)
# itself, reparent to init and sit in futex_do_wait for good -- a separate,
# pre-existing bug that has nothing to do with this test but that the test
# would otherwise leave ~50 instances of behind on every suite run. The group
# is reaped only after every direct child has been waited for, so anything
# still in it is one of those. Without setsid the loop still runs; it just
# cannot clean up after the emulator.
# Golden-free checks.
#
# Both of these test a property that needs no reference implementation, so
# neither waits on the Pi, and their binaries may be built with any aarch64
# toolchain. That matters: the differential corpus pins the whole suite to one
# board being reachable, and when it is not, a test that needs no golden should
# still run.
#
#  * sigfploop is self-checking -- it takes a signal with live FP state in
#    D16-D31, reads and edits the frame's fpsimd_context, and prints its own
#    counters. A reference would add nothing a wrong answer here would not
#    already show as a FAIL line.
#  * fp_lowbank's property is self-referential: guest V16-V31 must behave
#    exactly as V0-V15 do, so the SAME binary run with POWERARM_VSXCLASSES off
#    and on must produce identical output. For the register-class work that is
#    a sharper gate than a golden, because a golden only says "matches the Pi"
#    while this says "the switch changed nothing observable".
#  * ld1r knows what a replicate must produce without being told, so it checks
#    its own 487 cases -- every element size, both register banks, all 16
#    offsets from an aligned base, LD2R-LD4R, post-index, and each element
#    flush against an unmapped page. It carries its own comparator control.
if [ -f sigfploop ]; then
  POWERARM_VSXCLASSES=on run_emu sigfploop 2> /dev/null || true
  sfl_rc=$(cat sigfploop.powerarm.rc 2> /dev/null || echo 1)
  # grep -c PRINTS 0 and EXITS 1 when it matches nothing, so '|| echo 0' would
  # append a second line and every test below would compare against "0\n0".
  sfl_fail=$(grep -c '^FAIL' sigfploop.powerarm 2> /dev/null)
  sfl_bad=$(grep -c 'mismatches [1-9]' sigfploop.powerarm 2> /dev/null)
  sfl_ok=$(grep -c 'frame mismatches 0' sigfploop.powerarm 2> /dev/null)
  if [ "$sfl_rc" = 0 ] && [ "$sfl_fail" = 0 ] && [ "$sfl_bad" = 0 ] && [ "$sfl_ok" -ge 2 ]; then
    report PASS sigfploop "signal in an FP loop: $sfl_ok frame checks clean, no mismatches"
  else
    report FAIL sigfploop "rc=$sfl_rc fails=$sfl_fail mismatched=$sfl_bad clean=$sfl_ok; see $out/sigfploop.powerarm"
  fi
fi

if [ -f fp_lowbank ]; then
  POWERARM_VSXCLASSES=off "$emu" ./fp_lowbank > fp_lowbank.vsxoff 2> /dev/null
  lb_off=$?
  POWERARM_VSXCLASSES=on "$emu" ./fp_lowbank > fp_lowbank.vsxon 2> /dev/null
  lb_on=$?
  lb_lines=$(wc -l < fp_lowbank.vsxoff)
  if [ "$lb_off" = 0 ] && [ "$lb_on" = 0 ] && cmp -s fp_lowbank.vsxoff fp_lowbank.vsxon && [ "$lb_lines" -gt 0 ]; then
    report PASS fp_lowbank "$lb_lines lines identical with VSXCLASSES off and on"
  else
    report FAIL fp_lowbank "rc off=$lb_off on=$lb_on; $(cmp fp_lowbank.vsxoff fp_lowbank.vsxon 2>&1 | head -1)"
  fi
fi

# sigvcache is the same kind of test one layer down: a signal taken BETWEEN two
# reads of one guest V register inside a single guest block, with the handler
# EDITING that register in the frame. sigfploop cannot cover this -- its
# synchronous delivery is an `svc`, which ends the block -- and nothing else in
# the suite edits a vector register from a mid-block signal at all. The
# delivery here is a SIGSEGV from a PROT_NONE page in the middle of a block
# that reads V20 on both sides of it, so it is deterministic rather than
# timing-dependent.
#
# What it pins is RestoreFrame_Arm64: an edited frame from a signal taken in
# JIT code is NOT resumed as the interrupted host context, it is resumed
# through the dispatcher at the frame's PC, which refills every static register
# from CPUState and enters a fresh translation. If that were ever changed to
# resume the host context, this is the test that fails.
#
# The run is worthless unless the positive control FAILs, so exactly one FAIL
# line is required and it has to be that one.
if [ -f sigvcache ]; then
  "$emu" ./sigvcache > sigvcache.powerarm 2> /dev/null
  svc_rc=$?
  svc_fail=$(grep -c '^FAIL' sigvcache.powerarm 2> /dev/null)
  svc_ctl=$(grep -c '^FAIL control-deliberately-wrong' sigvcache.powerarm 2> /dev/null)
  svc_pass=$(grep -c '^PASS' sigvcache.powerarm 2> /dev/null)
  if [ "$svc_rc" = 0 ] && [ "$svc_fail" = 1 ] && [ "$svc_ctl" = 1 ] && [ "$svc_pass" -ge 8 ]; then
    report PASS sigvcache "$svc_pass checks clean; handler edit of V20 from a mid-block signal observed, control FAILed"
  else
    report FAIL sigvcache "rc=$svc_rc fails=$svc_fail control=$svc_ctl passes=$svc_pass; see $out/sigvcache.powerarm"
  fi
fi

# A process-directed SIGCHLD generated while one of the guest's threads is
# inside a FEX thread-teardown window. Golden-free and self-checking for the
# same reason fcmp_denorm and memrev are: the expected answer is "every child
# this process reaped was announced to it", which sigchildteardown.c computes
# from its own round count, and a reference taken from the emulator under test
# is precisely what could not tell you that a signal went missing.
#
# Two gates, because the second costs nothing and is sharper than the first.
# The binary's own arithmetic has to come out -- every PASS line present, no
# FAIL line -- AND the emulator's bail report must not have named a single
# process-aimed drop. SignalHandlerThunk counts those apart from the
# thread-aimed ones (tkill/tgkill at a named TID, where dropping a signal that
# raced the thread's exit is correct), so `action=drop-process-aimed' on
# stderr is the emulator stating that it lost a signal POSIX required it to
# deliver -- true whether or not the guest's own count happened to catch it on
# this run, and the thing to watch if this ever starts failing again.
#
# Before the fix in SignalDelegator::UninstallTLSState and the guest `exit'
# syscall, this failed on the first handful of rounds: 4 of the first 5
# children went unannounced, 3 of them with that bail line on stderr and one
# swallowed silently by the deferred-signal queue.
#
# Wrapped in a timeout, because this run has a THIRD outcome besides pass and
# fail: on 2026-10-15 it stopped making progress for ten minutes with the child
# in futex_do_wait and the parent in do_wait, pids unchanging, and an unwrapped
# run makes that an indefinitely wedged suite rather than a failure. A hang IS
# a failure, so the timeout does not weaken the test; it is what lets the suite
# say so instead of stopping. 124 is timeout's own exit status for a kill.
#
# Most likely the parent-side face of the known code-cache fork-writer deadlock
# (HANDOVER, item 42's last bullet: "can deadlock inside fork(2), reparent to
# init and hang forever"), which has the same futex signature -- but unproven,
# because it has never reproduced on a quiet box: 0 hangs in 72 standalone runs
# and 48 parallel ones, against 2 in 13 full suite runs on a box that was also
# building. If this fires, re-run with POWERARM_CODECACHEFORKWRITER=0 before
# looking anywhere else.
if [ -f sigchildteardown ]; then
  timeout -k 10 300 "$emu" ./sigchildteardown > sigchildteardown.powerarm 2> sigchildteardown.stderr
  sct_rc=$?
  if [ "$sct_rc" = 124 ]; then
    report FAIL sigchildteardown "HUNG: no exit within 300s, killed. A thread stuck in teardown is the teardown-race hang, not a slow machine; see $out/sigchildteardown.powerarm"
  else
  sct_pass=$(grep -c '^PASS' sigchildteardown.powerarm 2> /dev/null)
  sct_fail=$(grep -c '^FAIL' sigchildteardown.powerarm 2> /dev/null)
  sct_drop=$(grep -c 'action=drop-process-aimed' sigchildteardown.stderr 2> /dev/null)
  sct_rounds=$(sed -n 's/^sigchildteardown: \([0-9]*\) rounds,.*/\1/p' sigchildteardown.powerarm)
  if [ "$sct_rc" = 0 ] && [ "$sct_fail" = 0 ] && [ "$sct_pass" -ge 3 ] && [ "$sct_drop" = 0 ]; then
    report PASS sigchildteardown "${sct_rounds:-0} children announced through a thread teardown, no process-aimed drop"
  else
    report FAIL sigchildteardown "rc=$sct_rc passes=$sct_pass fails=$sct_fail process-aimed-drops=$sct_drop; see $out/sigchildteardown.powerarm and .stderr"
  fi
  fi
fi

# ld1r runs in both switch states, because the lowering it exercises is the
# one the register classes change: a 4 or 8-byte LD1R into V16-V31 is a single
# VSX load only while VBroadcastFromMem is VSXClean, and the same instruction
# with the switch off goes through a copy out of the low bank. Both must give
# the same 487 clean checks. The negative control for this test is not in the
# file: widening the 4-byte arm to lxvdsx takes it to 82 value failures and a
# SIGSEGV on the at-page-end case.
if [ -f ld1r ]; then
  lr_fail=0
  lr_detail=""
  for lr_mode in on off; do
    POWERARM_VSXCLASSES=$lr_mode "$emu" ./ld1r > "ld1r.vsx$lr_mode" 2> /dev/null
    lr_rc=$?
    lr_bad=$(grep -c '^FAIL' "ld1r.vsx$lr_mode" 2> /dev/null)
    lr_checks=$(sed -n 's/^checks \([0-9]*\) failures 0$/\1/p' "ld1r.vsx$lr_mode")
    lr_ctl=$(grep -c '^PASS control-detects-corruption$' "ld1r.vsx$lr_mode" 2> /dev/null)
    # A missing summary line means it died before printing one, which an rc
    # check alone would miss if it died with status 0.
    if [ "$lr_rc" != 0 ] || [ "$lr_bad" != 0 ] || [ "$lr_ctl" != 1 ] || [ -z "$lr_checks" ] || [ "$lr_checks" -lt 400 ]; then
      lr_fail=1
      lr_detail="$lr_detail switch=$lr_mode rc=$lr_rc fails=$lr_bad checks=${lr_checks:-none} control=$lr_ctl;"
    fi
  done
  if [ "$lr_fail" = 0 ]; then
    report PASS ld1r "$lr_checks replicate checks clean with VSXCLASSES off and on"
  else
    report FAIL ld1r "$lr_detail see $out/ld1r.vsxon"
  fi
fi

# vins runs in both switch states for the same reason ld1r does: the 4 and
# 8-byte inserts are the ops the register classes moved, so with the switch on
# a word insert into V16-V31 is the same xxinsertw (or xxspltw/xxsel pre-3.0)
# that V0-V15 gets, and with it off the same instruction goes through a copy
# out of the low bank. Both must give the same clean checks, and the suite's
# disableisa30 mode supplies the other axis -- between the two suite runs this
# covers all four combinations of switch and ISA level, which matters because
# the pre-3.0 word insert is a different lowering (a splat and an xxsel
# against a register-built word mask) rather than a twin of the 3.0 one.
if [ -f vins ]; then
  vi_fail=0
  vi_detail=""
  for vi_mode in on off; do
    POWERARM_VSXCLASSES=$vi_mode "$emu" ./vins > "vins.vsx$vi_mode" 2> /dev/null
    vi_rc=$?
    vi_bad=$(grep -c '^FAIL' "vins.vsx$vi_mode" 2> /dev/null)
    vi_checks=$(sed -n 's/^checks \([0-9]*\) failures 0$/\1/p' "vins.vsx$vi_mode")
    vi_ctl=$(grep -c '^PASS control-detects-corruption$' "vins.vsx$vi_mode" 2> /dev/null)
    # A missing summary line means it died before printing one, which an rc
    # check alone would miss if it died with status 0.
    if [ "$vi_rc" != 0 ] || [ "$vi_bad" != 0 ] || [ "$vi_ctl" != 1 ] || [ -z "$vi_checks" ] || [ "$vi_checks" -lt 200 ]; then
      vi_fail=1
      vi_detail="$vi_detail switch=$vi_mode rc=$vi_rc fails=$vi_bad checks=${vi_checks:-none} control=$vi_ctl;"
    fi
  done
  if [ "$vi_fail" = 0 ]; then
    report PASS vins "$vi_checks insert checks clean with VSXCLASSES off and on"
  else
    report FAIL vins "$vi_detail see $out/vins.vsxon"
  fi
fi


# memredirect: the libc mem-routine redirect (POWERARM_MEMREDIRECT /
# POWERARM_MEMREDIRECTTHRESHOLD; docs/powerarm/THUNKS-DESIGN.md 4.2).
#
# The claim under test is that the redirect is INVISIBLE, so this does not just
# require the test to pass -- it requires the OUTPUT to be byte-identical with
# the switch off and with it on at four thresholds. Both sides of the gate are
# therefore checked against each other rather than against a recorded run, which
# is also why the test mints no golden.
#
# The thresholds are chosen so that every length the test uses lands on both
# sides of the gate across the five runs: 0 redirects every call the gate can
# see, 1 redirects all but the empty ones, the default (256) is the shipping
# configuration, and 65536 is above every length in the test so nothing is
# redirected at all and the run must match the switch-off run exactly.
#
# What is deliberately NOT compared: the guest PC reported for a fault taken
# inside a redirected copy. With the redirect on, the frame carries the hooked
# routine's own entry address (the gate stores State.pc before the host call);
# with it off it carries wherever inside the routine the faulting instruction
# was. The test asserts si_addr and that the guest's handler ran -- which must
# match -- and never looks at the PC.
if [ -f memredirect ]; then
  mr_fail=0
  mr_detail=""
  mr_base=""
  for mr_mode in off on t0 t1 tbig; do
    case $mr_mode in
    off) mr_env="" ;;
    on) mr_env="POWERARM_MEMREDIRECT=1" ;;
    t0) mr_env="POWERARM_MEMREDIRECT=1 POWERARM_MEMREDIRECTTHRESHOLD=0" ;;
    t1) mr_env="POWERARM_MEMREDIRECT=1 POWERARM_MEMREDIRECTTHRESHOLD=1" ;;
    tbig) mr_env="POWERARM_MEMREDIRECT=1 POWERARM_MEMREDIRECTTHRESHOLD=65536" ;;
    esac
    # shellcheck disable=SC2086
    env $mr_env "$emu" ./memredirect > "memredirect.$mr_mode" 2> /dev/null
    mr_rc=$?
    mr_bad=$(grep -c '^FAIL' "memredirect.$mr_mode" 2> /dev/null)
    mr_checks=$(sed -n 's/^checks \([0-9]*\) failures 0$/\1/p' "memredirect.$mr_mode")
    mr_ctl=$(grep -c '^PASS control-detects-corruption$' "memredirect.$mr_mode" 2> /dev/null)
    # A missing summary line means it died before printing one, which an rc
    # check alone would miss if it died with status 0.
    if [ "$mr_rc" != 0 ] || [ "$mr_bad" != 0 ] || [ "$mr_ctl" != 1 ] || [ -z "$mr_checks" ] || [ "$mr_checks" -lt 13 ]; then
      mr_fail=1
      mr_detail="$mr_detail switch=$mr_mode rc=$mr_rc fails=$mr_bad checks=${mr_checks:-none} control=$mr_ctl;"
      continue
    fi
    # Every run must agree with the switch-off run, byte for byte.
    if [ -z "$mr_base" ]; then
      mr_base="memredirect.$mr_mode"
    elif ! cmp -s "$mr_base" "memredirect.$mr_mode"; then
      mr_fail=1
      mr_detail="$mr_detail switch=$mr_mode output differs from switch-off;"
    fi
  done
  if [ "$mr_fail" = 0 ]; then
    report PASS memredirect "$mr_checks checks clean and output identical with MEMREDIRECT off and on at thresholds 0/1/default/65536"
  else
    report FAIL memredirect "$mr_detail see $out/memredirect.t0"
  fi
fi

# vneg runs in both switch states for the same reason vins does, and the
# suite's disableisa30 mode supplies the other axis: NEG.4S and NEG.2D are one
# `vnegw`/`vnegd` on an ISA 3.0 host and the `vspltisw 0 ; vsubu{w,d}m` pair
# below it, so between the two suite runs this covers both lowerings of the
# same IR op in both register banks. The byte and halfword arrangements have no
# `vneg` form at all and are here to show the swap did not reach them.
if [ -f vneg ]; then
  vn_fail=0
  vn_detail=""
  for vn_mode in on off; do
    POWERARM_VSXCLASSES=$vn_mode "$emu" ./vneg > "vneg.vsx$vn_mode" 2> /dev/null
    vn_rc=$?
    vn_bad=$(grep -c '^FAIL' "vneg.vsx$vn_mode" 2> /dev/null)
    vn_checks=$(sed -n 's/^checks \([0-9]*\) failures 0$/\1/p' "vneg.vsx$vn_mode")
    vn_ctl=$(grep -c '^PASS control-detects-corruption$' "vneg.vsx$vn_mode" 2> /dev/null)
    if [ "$vn_rc" != 0 ] || [ "$vn_bad" != 0 ] || [ "$vn_ctl" != 1 ] || [ -z "$vn_checks" ] || [ "$vn_checks" -lt 150 ]; then
      vn_fail=1
      vn_detail="$vn_detail switch=$vn_mode rc=$vn_rc fails=$vn_bad checks=${vn_checks:-none} control=$vn_ctl;"
    fi
  done
  if [ "$vn_fail" = 0 ]; then
    report PASS vneg "$vn_checks lane-negation checks clean with VSXCLASSES off and on"
  else
    report FAIL vneg "$vn_detail see $out/vneg.vsxon"
  fi
fi

# vlanetest is the gate on the vector lane-test fusion (POWER-STRENGTHS.md
# 2.1): a compare, UMAXV/UMINV/ADDV, FMOV and CBZ/CBNZ become a record-form
# compare and the guest's own branch. Three things are checked, and the second
# and third are the ones a value test alone would miss.
#
#  1. The architecturally forced answer, for all 300 recognised shapes: the
#     lane mask, the reduction, the GPR and the branch direction. The mask and
#     the GPR are read AFTER the join, i.e. in a block the branch reaches from
#     either side, because they are real guest registers that the fused path
#     does not write where the unfused one does. Deliberately breaking the
#     fused edge's register writes takes this from 0 to 1,914 failures, and
#     taking the wrong CR6 bit to 1,870.
#
#     The ADDV arm is the one whose reduction is not a lane of ones: the lane
#     sum is -k modulo the element for k matching lanes, so sixteen matching
#     byte lanes give 0xf0 and not 0xff. That value is what separates a correct
#     ADDV lowering from one copied from UMAXV, and it is hand-computed in the
#     test's anchors rather than modelled. The file also covers the shapes the
#     ADDV arm must REFUSE, arranged so that fusing them answers differently:
#     a Q-mismatched, an other-source and a WZR-destination shape all branch
#     the OTHER WAY, and the two unallocated arrangements (2S and .2D, written
#     as .inst because the assembler will not encode them) must raise SIGILL
#     rather than take a branch.
#
#     Measured, by breaking the ADDV arm one way at a time and reverting each:
#     dropping its forced-edge register writes gives 779 failures, every one on
#     the MASK, every one in an addv shape, none in a umaxv or uminv one, and
#     the branch direction untouched -- the c511d41c2 bug class, one arm later.
#     Giving ADDV the "all lanes" predicate, so CR6 is read from the wrong bit,
#     gives 2,326, split 1,233 branch and 1,093 reduction. Dropping the
#     reduction's Q match gives 12, all of them the branch DIRECTION of
#     addv-qmix. Dropping the element-size match gives exactly one, the .2D
#     unallocated case, and dropping the compare's 2S refusal exactly one, the
#     2S case -- so each unallocated arrangement is the sole witness for one
#     check, which is why both are here and why their inputs make no lane
#     match: with every lane matching, a wrongly-fused idiom re-translates the
#     bad word on its other edge and faults anyway, and both breaks go
#     unnoticed.
#  2. The switch changed nothing observable: the same binary with
#     POWERARM_DISABLELANETESTFUSION=1 must produce byte-identical output,
#     which makes the fused and the unfused lowering differential against each
#     other on every shape at once -- and the unfused one is what the goldens
#     already pin.
#  3. The fusion actually fired, counted in the IR dump, and did not fire with
#     the switch. Under POWERARM_MAXINST=1 the block is one instruction, so the
#     idiom is never the last four of a block and NOTHING may fuse: that is the
#     unit-boundary case, and firing there would be a fusion over operands it
#     cannot see.
if [ -f vlanetest ]; then
  vl_fail=0
  vl_detail=""
  for vl_mode in on off; do
    POWERARM_VSXCLASSES=$vl_mode "$emu" ./vlanetest > "vlanetest.vsx$vl_mode" 2> /dev/null
    vl_rc=$?
    vl_bad=$(grep -c '^FAIL' "vlanetest.vsx$vl_mode" 2> /dev/null)
    vl_checks=$(sed -n 's/^checks \([0-9]*\) failures 0$/\1/p' "vlanetest.vsx$vl_mode")
    vl_ctl=$(grep -c '^PASS control-detects-corruption$' "vlanetest.vsx$vl_mode" 2> /dev/null)
    if [ "$vl_rc" != 0 ] || [ "$vl_bad" != 0 ] || [ "$vl_ctl" != 1 ] || [ -z "$vl_checks" ] || [ "$vl_checks" -lt 9000 ]; then
      vl_fail=1
      vl_detail="$vl_detail switch=$vl_mode rc=$vl_rc fails=$vl_bad checks=${vl_checks:-none} control=$vl_ctl;"
    fi
  done
  POWERARM_DISABLELANETESTFUSION=1 "$emu" ./vlanetest > vlanetest.nofuse 2> /dev/null
  vl_nf_rc=$?
  if [ "$vl_nf_rc" != 0 ]; then
    vl_fail=1
    vl_detail="$vl_detail unfused run exited $vl_nf_rc;"
  fi
  if ! cmp -s vlanetest.vsxon vlanetest.nofuse; then
    vl_fail=1
    vl_detail="$vl_detail the switch changed the output: $(cmp vlanetest.vsxon vlanetest.nofuse 2>&1 | head -1);"
  fi
  POWERARM_ENABLECODECACHINGWIP=0 POWERARM_DUMPIR=stderr POWERARM_PASSMANAGERDUMPIR=afteropt \
    "$emu" ./vlanetest > /dev/null 2> vlanetest.irdump
  POWERARM_DISABLELANETESTFUSION=1 POWERARM_ENABLECODECACHINGWIP=0 POWERARM_DUMPIR=stderr POWERARM_PASSMANAGERDUMPIR=afteropt \
    "$emu" ./vlanetest > /dev/null 2> vlanetest.irdump.nofuse
  vl_fused=$(grep -cE '\bVLaneTestCR\b' vlanetest.irdump 2> /dev/null)
  vl_fused_off=$(grep -cE '\bVLaneTestCR\b' vlanetest.irdump.nofuse 2> /dev/null)
  # One instruction per block leaves no four-instruction idiom to recognise,
  # so the fusion must be absent rather than guessing across the boundary.
  if [ -n "${POWERARM_MAXINST:-}" ] && [ "${POWERARM_MAXINST:-}" != 0 ]; then
    vl_want=0
  else
    vl_want=300
  fi
  if [ "$vl_fused" -lt "$vl_want" ]; then
    vl_fail=1
    vl_detail="$vl_detail only $vl_fused of $vl_want shapes fused;"
  fi
  if [ "$vl_want" = 0 ] && [ "$vl_fused" != 0 ]; then
    vl_fail=1
    vl_detail="$vl_detail $vl_fused fused under MAXINST, where no block holds the idiom;"
  fi
  if [ "$vl_fused_off" != 0 ]; then
    vl_fail=1
    vl_detail="$vl_detail POWERARM_DISABLELANETESTFUSION=1 still fused $vl_fused_off;"
  fi
  if [ "$vl_fail" = 0 ]; then
    report PASS vlanetest "$vl_checks lane-test checks clean in both switch states, control FAILed; fused $vl_fused, none with the switch, output identical to the unfused run"
  else
    report FAIL vlanetest "$vl_detail see $out/vlanetest.vsxon"
  fi
fi

# fminmaxnm is the gate on FMAXNM/FMINNM taking the hardware answer. The NUMBER
# forms check their RESULT for NaN rather than both INPUTS (POWER-STRENGTHS.md
# 2.7), so a single-QNaN input no longer reaches the ARM-exact cold stub. The test
# computes FPMaxNum/FPMinNum from the ARM pseudocode -- not from the lowering --
# and compares bit for bit including NaN payloads.
#
# It runs twice. The second run is POWERARM_FPCOLD=0, which emits the NaN check and
# never the branch, so the cold path is unreachable and the PROPAGATING fmax/fmin
# rows must FAIL. A clean FPCOLD=0 run would mean the test is not reaching the cold
# path at all and proves nothing, so a zero there is itself a failure.
#
# FPCR.DN and FZ are stored but not emulated here (COLD-BLOCK-DESIGN.md 53), so the
# test buckets those rows as a known gap and says so; it still FAILS a DN/FZ row
# whose answer is neither architectural nor the documented DN=0,FZ=0 one.
if [ -f fminmaxnm ]; then
  fmm_fail=0
  fmm_detail=""
  "$emu" ./fminmaxnm > fminmaxnm.out 2> /dev/null
  fmm_bad=$(grep -c '^FAIL' fminmaxnm.out 2> /dev/null)
  fmm_checks=$(sed -n 's/^checks \([0-9]*\) failures 0$/\1/p' fminmaxnm.out)
  fmm_ref=$(grep -c '^PASS control-reference-pinned$' fminmaxnm.out 2> /dev/null)
  fmm_fpcr=$(grep -c '^PASS fpcr-restored$' fminmaxnm.out 2> /dev/null)
  if [ "$fmm_bad" != 0 ] || [ -z "$fmm_checks" ]; then
    fmm_fail=1
    fmm_detail="$fmm_detail $fmm_bad failing rows;"
  fi
  if [ "$fmm_ref" != 1 ]; then
    fmm_fail=1
    fmm_detail="$fmm_detail the reference itself was not pinned, so a pass means nothing;"
  fi
  if [ "$fmm_fpcr" != 1 ]; then
    fmm_fail=1
    fmm_detail="$fmm_detail FPCR was left modified;"
  fi
  POWERARM_FPCOLD=0 "$emu" ./fminmaxnm > fminmaxnm.fpcold0 2> /dev/null
  fmm_cold=$(grep -c '^FAIL' fminmaxnm.fpcold0 2> /dev/null)
  if [ "$fmm_cold" = 0 ]; then
    fmm_fail=1
    fmm_detail="$fmm_detail FPCOLD=0 produced no failures, so the cold path is never exercised;"
  fi
  if [ "$fmm_fail" = 0 ]; then
    report PASS fminmaxnm "$fmm_checks FPMaxNum/FPMinNum checks clean, $fmm_cold rows fail with the cold path disabled"
  else
    report FAIL fminmaxnm "$fmm_detail see $out/fminmaxnm.out"
  fi
fi

# memrev is the gate on the load+REV / REV+store fusion (TryFuseMemRev):
# every width at every alignment, every addressing form the fusion accepts
# and the ones it refuses, the store direction's register result, and the
# fault boundary at a page edge. Every answer is architecturally forced and
# the test computes it from the bytes itself, so there is no golden.
#
# It runs twice, and the two runs must agree byte for byte. The second run is
# POWERARM_MAXINST=1, which puts one guest instruction in a block and so
# cannot fuse anything: the matcher needs the pair in one decoded block. That
# makes the fused and unfused lowerings differential against each other on
# this host, which is a sharper gate than either run alone -- a golden only
# says "matches the Pi", this says "the fusion changed nothing observable".
if [ -f memrev ]; then
  mr_fail=0
  mr_detail=""
  for mr_mode in fused unfused; do
    if [ "$mr_mode" = unfused ]; then
      POWERARM_MAXINST=1 "$emu" ./memrev > "memrev.$mr_mode" 2> /dev/null
    else
      "$emu" ./memrev > "memrev.$mr_mode" 2> /dev/null
    fi
    mr_rc=$?
    mr_bad=$(grep -c '^FAIL' "memrev.$mr_mode" 2> /dev/null)
    mr_checks=$(sed -n 's/^checks \([0-9]*\) failures 0$/\1/p' "memrev.$mr_mode")
    mr_ctl=$(grep -c '^PASS control-detects-corruption$' "memrev.$mr_mode" 2> /dev/null)
    # A missing summary line means it died before printing one, which an rc
    # check alone would miss if it died with status 0.
    if [ "$mr_rc" != 0 ] || [ "$mr_bad" != 0 ] || [ "$mr_ctl" != 1 ] || [ -z "$mr_checks" ] || [ "$mr_checks" -lt 200 ]; then
      mr_fail=1
      mr_detail="$mr_detail mode=$mr_mode rc=$mr_rc fails=$mr_bad checks=${mr_checks:-none} control=$mr_ctl;"
    fi
  done
  if [ "$mr_fail" = 0 ] && ! cmp -s memrev.fused memrev.unfused; then
    mr_fail=1
    mr_detail="$mr_detail fused and unfused output differ: $(cmp memrev.fused memrev.unfused 2>&1 | head -1);"
  fi
  if [ "$mr_fail" = 0 ]; then
    report PASS memrev "$mr_checks byte-reversed access checks clean, fused and unfused identical"
  else
    report FAIL memrev "$mr_detail see $out/memrev.fused"
  fi
fi

# prfmhint is the gate on PRFM reaching dcbt/dcbtst: every hint kind and every
# addressing form against a writable page, a read-only page, a PROT_NONE page,
# an unmapped hole, the last byte of a mapping, its own code page and
# addresses no mapping covers. A touch cannot raise a data storage exception,
# so the forced answer is "no signal, no byte moved, and the instructions
# after it still ran", which the test checks itself -- no golden.
#
# Both switch states, and they must agree byte for byte: POWERARM_GUESTPREFETCH=0
# emits the nop PRFM used to be. That a hint changed nothing observable is the
# whole correctness claim, so it is the thing the gate checks.
if [ -f prfmhint ]; then
  pf_fail=0
  pf_detail=""
  for pf_mode in 1 0; do
    POWERARM_GUESTPREFETCH=$pf_mode "$emu" ./prfmhint > "prfmhint.gp$pf_mode" 2> /dev/null
    pf_rc=$?
    pf_bad=$(grep -c '^FAIL' "prfmhint.gp$pf_mode" 2> /dev/null)
    pf_checks=$(sed -n 's/^checks \([0-9]*\) failures 0$/\1/p' "prfmhint.gp$pf_mode")
    pf_ctl=$(grep -c '^PASS control-unmapped-page-really-faults$' "prfmhint.gp$pf_mode" 2> /dev/null)
    if [ "$pf_rc" != 0 ] || [ "$pf_bad" != 0 ] || [ "$pf_ctl" != 1 ] || [ -z "$pf_checks" ] || [ "$pf_checks" -lt 10 ]; then
      pf_fail=1
      pf_detail="$pf_detail switch=$pf_mode rc=$pf_rc fails=$pf_bad checks=${pf_checks:-none} control=$pf_ctl;"
    fi
  done
  if [ "$pf_fail" = 0 ] && ! cmp -s prfmhint.gp1 prfmhint.gp0; then
    pf_fail=1
    pf_detail="$pf_detail dcbt and nop output differ: $(cmp prfmhint.gp1 prfmhint.gp0 2>&1 | head -1);"
  fi
  if [ "$pf_fail" = 0 ]; then
    report PASS prfmhint "$pf_checks prefetch checks clean with GUESTPREFETCH on and off"
  else
    report FAIL prfmhint "$pf_detail see $out/prfmhint.gp1"
  fi
fi

# vcmpscan is the gate on the vector-scan fusion (TryFuseVectorScan): CMEQ,
# UMAXP/ADDP, FMOV and CBZ/CBNZ collapse into one record-form vector compare
# and the guest's own branch, so the mask never gets reduced through a GPR.
# Three things are checked, and the second and third are the ones a
# branch-direction test alone would miss.
#
#  1. The architecturally forced answer for every recognised shape: the lane
#     mask, the pairwise reduction of it, the GPR the FMOV writes and the
#     branch direction. All three registers are read AFTER THE JOIN, i.e. in
#     a block the branch reaches from either side, because they are real
#     guest registers that the fused branch itself does not write. They are
#     poisoned beforehand, so a stale value is visible rather than
#     accidentally equal to a zeroed buffer: before the no-match edge wrote
#     its forced zeros, 578 of these checks failed.
#  2. The switch changed nothing observable: the same binary with
#     POWERARM_VCMPFUSION=0 must produce byte-identical output, which makes
#     the fused and the unfused lowering differential against each other on
#     every shape at once -- and the unfused one is the lowering the rest of
#     this suite's goldens already pin.
#  3. The fusion actually fired, counted in the IR dump, and did not fire
#     with the switch. Under POWERARM_MAXINST=1 a block is one instruction,
#     so the idiom is never the last four of one and NOTHING may fuse: that
#     is the unit-boundary case, and firing there would be a fusion over
#     operands it cannot see.
#
# The fused CondJump is the third arm of DEF_OP(CondJump) -- the one whose
# CompareSize is Invalid, whose FromNZCV is clear and whose VCmpElementSize
# is a real element size. 32 sites in the file fuse; `gap` (an instruction
# between the compare and the reduction) and `fmov-w` (the 32-bit FMOV and
# branch) are refused and keep the old lowering.
if [ -f vcmpscan ]; then
  vs_fail=0
  vs_detail=""
  "$emu" ./vcmpscan > vcmpscan.powerarm 2> /dev/null
  vs_rc=$?
  vs_bad=$(grep -c '^FAIL' vcmpscan.powerarm 2> /dev/null)
  vs_checks=$(sed -n 's/^checks \([0-9]*\) failures 0$/\1/p' vcmpscan.powerarm)
  vs_ctl=$(grep -c '^PASS control-detects-corruption$' vcmpscan.powerarm 2> /dev/null)
  # A missing summary line means it died before printing one, which an rc
  # check alone would miss if it died with status 0.
  if [ "$vs_rc" != 0 ] || [ "$vs_bad" != 0 ] || [ "$vs_ctl" != 1 ] || [ -z "$vs_checks" ] || [ "$vs_checks" -lt 1700 ]; then
    vs_fail=1
    vs_detail="$vs_detail rc=$vs_rc fails=$vs_bad checks=${vs_checks:-none} control=$vs_ctl;"
  fi
  POWERARM_VCMPFUSION=0 "$emu" ./vcmpscan > vcmpscan.nofuse 2> /dev/null
  vs_nf_rc=$?
  if [ "$vs_nf_rc" != 0 ]; then
    vs_fail=1
    vs_detail="$vs_detail unfused run exited $vs_nf_rc;"
  fi
  if ! cmp -s vcmpscan.powerarm vcmpscan.nofuse; then
    vs_fail=1
    vs_detail="$vs_detail the switch changed the output: $(cmp vcmpscan.powerarm vcmpscan.nofuse 2>&1 | head -1);"
  fi
  POWERARM_ENABLECODECACHINGWIP=0 POWERARM_DUMPIR=stderr POWERARM_PASSMANAGERDUMPIR=afteropt \
    "$emu" ./vcmpscan > /dev/null 2> vcmpscan.irdump
  POWERARM_VCMPFUSION=0 POWERARM_ENABLECODECACHINGWIP=0 POWERARM_DUMPIR=stderr POWERARM_PASSMANAGERDUMPIR=afteropt \
    "$emu" ./vcmpscan > /dev/null 2> vcmpscan.irdump.nofuse
  vs_re=', Invalid, #0x0, i(8|16|32|64), #0x0, #0x0$'
  vs_fused=$(grep -cE "$vs_re" vcmpscan.irdump 2> /dev/null)
  vs_fused_off=$(grep -cE "$vs_re" vcmpscan.irdump.nofuse 2> /dev/null)
  # One instruction per block leaves no four-instruction idiom to recognise,
  # so the fusion must be absent rather than guessing across the boundary.
  if [ -n "${POWERARM_MAXINST:-}" ] && [ "${POWERARM_MAXINST:-}" != 0 ]; then
    vs_want=0
  else
    vs_want=32
  fi
  if [ "$vs_fused" -lt "$vs_want" ]; then
    vs_fail=1
    vs_detail="$vs_detail only $vs_fused of $vs_want fusable sites fused;"
  fi
  if [ "$vs_want" = 0 ] && [ "$vs_fused" != 0 ]; then
    vs_fail=1
    vs_detail="$vs_detail $vs_fused fused under MAXINST, where no block holds the idiom;"
  fi
  if [ "$vs_fused_off" != 0 ]; then
    vs_fail=1
    vs_detail="$vs_detail POWERARM_VCMPFUSION=0 still fused $vs_fused_off;"
  fi
  if [ "$vs_fail" = 0 ]; then
    report PASS vcmpscan \
      "$vs_checks scan checks clean, control FAILed as required; fused $vs_fused sites, none with the switch, output identical to the unfused run"
  else
    report FAIL vcmpscan "$vs_detail see $out/vcmpscan.powerarm"
  fi
fi

# fcmp_denorm is the gate on the f32 compare arms having moved from the VMX
# vcmpeqfp/vcmpgtfp/vcmpgefp to their VSX twins. The VMX ones flush denormal
# inputs to zero when VSCR.NJ is set and the VSX ones never do, so the test
# asserts the forced answer for denormal, NaN, signed-zero and infinity
# inputs at BOTH element widths: the swap's whole premise is that the 4-byte
# arm and the 8-byte arm of one IR op agree, and they are different host
# instructions. Run in both switch states for the usual reason -- with it on,
# a compare of V16-V31 is the same instruction V0-V15 gets.
if [ -f fcmp_denorm ]; then
  fd_fail=0
  fd_detail=""
  for fd_mode in on off; do
    POWERARM_VSXCLASSES=$fd_mode "$emu" ./fcmp_denorm > "fcmp_denorm.vsx$fd_mode" 2> /dev/null
    fd_rc=$?
    fd_bad=$(grep -c '^FAIL' "fcmp_denorm.vsx$fd_mode" 2> /dev/null)
    fd_checks=$(sed -n 's/^checks \([0-9]*\) failures 0$/\1/p' "fcmp_denorm.vsx$fd_mode")
    fd_ctl=$(grep -c '^PASS control-detects-corruption$' "fcmp_denorm.vsx$fd_mode" 2> /dev/null)
    # A missing summary line means it died before printing one, which an rc
    # check alone would miss if it died with status 0.
    if [ "$fd_rc" != 0 ] || [ "$fd_bad" != 0 ] || [ "$fd_ctl" != 1 ] || [ -z "$fd_checks" ] || [ "$fd_checks" -lt 350 ]; then
      fd_fail=1
      fd_detail="$fd_detail switch=$fd_mode rc=$fd_rc fails=$fd_bad checks=${fd_checks:-none} control=$fd_ctl;"
    fi
  done
  if [ "$fd_fail" = 0 ]; then
    report PASS fcmp_denorm "$fd_checks forced compare answers clean with VSXCLASSES off and on"
  else
    report FAIL fcmp_denorm "$fd_detail see $out/fcmp_denorm.vsxon"
  fi
fi

# vcvtf16 is the gate on the FP16 vector conversions. On ISA 3.0 they are
# xvcvhpsp/xvcvsphp; on 2.07 there is no half-precision instruction at all and
# they are a branch-free vector sequence (VectorOps.cpp EmitHalfToSingleVec /
# EmitSingleToHalfVec). Every answer it checks is architecturally forced and
# the test computes all of them, so it needs no reference implementation.
#
# Unlike every other block here it forces its own host-feature mode, and that
# is the point: the 2.07 arm is the one with the arithmetic in it, and a suite
# run in the default mode never executes a single instruction of it. Forcing
# disableisa30 for the second run makes this test mean the same thing in all
# four suite modes instead of only one of them.
#
# Two checks, and the second is the sharper one. Each run's own arithmetic has
# to come out -- every check clean, the positive control present -- AND on a
# host that has ISA 3.0 the two runs' output has to be IDENTICAL, which is the
# software sequence agreeing with the hardware instruction value for value on
# this machine. On a real POWER8 both runs take the same path and the
# comparison is vacuous rather than wrong.
if [ -f vcvtf16 ]; then
  vc_fail=0
  vc_detail=""
  for vc_mode in ambient isa207; do
    if [ "$vc_mode" = isa207 ]; then
      POWERARM_HOSTFEATURES=disableisa30 "$emu" ./vcvtf16 > "vcvtf16.$vc_mode" 2> /dev/null
    else
      "$emu" ./vcvtf16 > "vcvtf16.$vc_mode" 2> /dev/null
    fi
    vc_rc=$?
    vc_bad=$(grep -c '^FAIL' "vcvtf16.$vc_mode" 2> /dev/null)
    vc_checks=$(sed -n 's/^checks \([0-9]*\) failures 0$/\1/p' "vcvtf16.$vc_mode")
    vc_ctl=$(grep -c '^PASS control-detects-corruption$' "vcvtf16.$vc_mode" 2> /dev/null)
    # A missing summary line means it died before printing one, which an rc
    # check alone would miss if it died with status 0.
    if [ "$vc_rc" != 0 ] || [ "$vc_bad" != 0 ] || [ "$vc_ctl" != 1 ] || [ -z "$vc_checks" ] || [ "$vc_checks" -lt 4000 ]; then
      vc_fail=1
      vc_detail="$vc_detail mode=$vc_mode rc=$vc_rc fails=$vc_bad checks=${vc_checks:-none} control=$vc_ctl;"
    fi
  done
  if ! cmp -s vcvtf16.ambient vcvtf16.isa207; then
    vc_fail=1
    vc_detail="$vc_detail the ISA 3.0 and ISA 2.07 paths disagreed;"
  fi
  if [ "$vc_fail" = 0 ]; then
    report PASS vcvtf16 "$vc_checks forced FP16 conversion answers clean, ISA 3.0 and ISA 2.07 paths identical"
  else
    report FAIL vcvtf16 "$vc_detail see $out/vcvtf16.isa207"
  fi
fi

# flaglive is the guest-visible half of the flag-liveness permissions
# (ISA-OPPORTUNITIES.md §3.5): Ashr/Sbfe reaching sradi/srad/srawi/sraw and a
# TBZ reaching rldicl., each only where DFCE proved the flag it destroys is
# dead. It needs no reference implementation -- every answer is forced by the
# A64 definitions -- and it carries its own positive control, so exactly one
# FAIL line is required and it has to be that one.
#
# The value check is not enough on its own, for the reason every switch-shaped
# item here has a second check: a permission that is never granted emits the
# old code, and the old code passes. So the run is also required to GRANT some
# of each kind and to grant none with POWERARM_NOFLAGLIVESHIFT=1, and the same
# binary's output must be identical either way -- "the switch changed nothing
# observable", which is a sharper statement than any single run's PASS lines.
# The counts are read from the afteropt IR dump with the code cache off, since
# a unit served from the cache is never compiled and so never annotated.
if [ -f flaglive ]; then
  fl_fail=0
  fl_detail=""
  for fl_sw in on off; do
    if [ "$fl_sw" = on ]; then
      POWERARM_NOFLAGLIVESHIFT=1 "$emu" ./flaglive > flaglive.noperm 2> /dev/null
      fl_out=flaglive.noperm
    else
      "$emu" ./flaglive > flaglive.powerarm 2> /dev/null
      fl_out=flaglive.powerarm
    fi
    fl_rc=$?
    fl_pass=$(grep -c '^PASS' "$fl_out" 2> /dev/null)
    fl_bad=$(grep -c '^FAIL' "$fl_out" 2> /dev/null)
    fl_ctl=$(grep -c '^FAIL control_deliberately_wrong' "$fl_out" 2> /dev/null)
    if [ "$fl_rc" != 0 ] || [ "$fl_bad" != 1 ] || [ "$fl_ctl" != 1 ] || [ "$fl_pass" -lt 50 ]; then
      fl_fail=1
      fl_detail="$fl_detail noflagliveshift=$fl_sw rc=$fl_rc pass=$fl_pass fail=$fl_bad control=$fl_ctl;"
    fi
  done
  if ! cmp -s flaglive.powerarm flaglive.noperm; then
    fl_fail=1
    fl_detail="$fl_detail the switch changed the output: $(cmp flaglive.powerarm flaglive.noperm 2>&1 | head -1);"
  fi
  # Granted-permission counts, with and without the switch.
  POWERARM_ENABLECODECACHINGWIP=0 POWERARM_DUMPIR=stderr POWERARM_PASSMANAGERDUMPIR=afteropt \
    "$emu" ./flaglive > /dev/null 2> flaglive.irdump
  POWERARM_NOFLAGLIVESHIFT=1 POWERARM_ENABLECODECACHINGWIP=0 POWERARM_DUMPIR=stderr POWERARM_PASSMANAGERDUMPIR=afteropt \
    "$emu" ./flaglive > /dev/null 2> flaglive.irdump.noperm
  fl_carry=$(grep -c 'CarryDead=1' flaglive.irdump 2> /dev/null)
  fl_tbz=$(grep -E 'CondJump .*(TSTZ|TSTNZ)' flaglive.irdump 2> /dev/null | grep -c 'NZCVDead=1')
  fl_carry_off=$(grep -c 'CarryDead=1' flaglive.irdump.noperm 2> /dev/null)
  fl_tbz_off=$(grep -E 'CondJump .*(TSTZ|TSTNZ)' flaglive.irdump.noperm 2> /dev/null | grep -c 'NZCVDead=1')
  # Under POWERARM_MAXINST=1 every leg of a granted case is a constant exit and
  # every exit is seeded with all four flags live, so NOTHING is granted and the
  # test is exercising the conservative lowering instead. That is a mode of the
  # test, not a failure of it -- but it must still be true that the switch
  # grants nothing, and the value checks above still have to pass.
  if [ -n "${POWERARM_MAXINST:-}" ] && [ "${POWERARM_MAXINST:-}" != 0 ]; then
    fl_want=0
  else
    fl_want=1
  fi
  if [ "$fl_want" = 1 ] && { [ "$fl_carry" -lt 1 ] || [ "$fl_tbz" -lt 1 ]; }; then
    fl_fail=1
    fl_detail="$fl_detail no permission was granted (CarryDead=$fl_carry TBZ=$fl_tbz), so the values above tested only the old lowering;"
  fi
  if [ "$fl_carry_off" != 0 ] || [ "$fl_tbz_off" != 0 ]; then
    fl_fail=1
    fl_detail="$fl_detail POWERARM_NOFLAGLIVESHIFT=1 still granted CarryDead=$fl_carry_off TBZ=$fl_tbz_off;"
  fi
  if [ "$fl_fail" = 0 ]; then
    report PASS flaglive "$fl_pass flag-liveness checks clean, control FAILed; granted CarryDead=$fl_carry TBZ=$fl_tbz, none with the switch"
  else
    report FAIL flaglive "$fl_detail see $out/flaglive.powerarm"
  fi
fi

# ccmpchain is the conditional-compare chain (ISA-OPPORTUNITIES.md §3.2):
# `cmp ; ccmp ; b.cond` collapsed into two compares, one CR logical op and the
# guest's branch. It needs no reference implementation because it is its own:
# each case runs the chain shape AND the same arithmetic with an MRS between
# the conditional compare and the branch, which the chain walker refuses, so
# the second is the packed-NZCV lowering the differential tests here already
# pin. 896 condition pairs plus eleven hand-computed anchors, and a positive
# control, so exactly one FAIL is required and it has to be that one.
#
# Same second check as flaglive, for the same reason: a chain that never
# collapses emits the old code twice and passes. The run must COLLAPSE some
# chains and collapse none with POWERARM_DISABLECCMPCHAIN=1, with identical
# output either way.
if [ -f ccmpchain ]; then
  cc_fail=0
  cc_detail=""
  for cc_sw in on off; do
    if [ "$cc_sw" = on ]; then
      POWERARM_DISABLECCMPCHAIN=1 "$emu" ./ccmpchain > ccmpchain.nochain 2> /dev/null
      cc_out=ccmpchain.nochain
    else
      "$emu" ./ccmpchain > ccmpchain.powerarm 2> /dev/null
      cc_out=ccmpchain.powerarm
    fi
    cc_rc=$?
    cc_pass=$(grep -c '^PASS' "$cc_out" 2> /dev/null)
    cc_bad=$(grep -c '^FAIL' "$cc_out" 2> /dev/null)
    cc_ctl=$(grep -c '^FAIL control_deliberately_wrong' "$cc_out" 2> /dev/null)
    if [ "$cc_rc" != 0 ] || [ "$cc_bad" != 1 ] || [ "$cc_ctl" != 1 ] || [ "$cc_pass" -lt 25 ]; then
      cc_fail=1
      cc_detail="$cc_detail disablechain=$cc_sw rc=$cc_rc pass=$cc_pass fail=$cc_bad control=$cc_ctl;"
    fi
  done
  if ! cmp -s ccmpchain.powerarm ccmpchain.nochain; then
    cc_fail=1
    cc_detail="$cc_detail the switch changed the output: $(cmp ccmpchain.powerarm ccmpchain.nochain 2>&1 | head -1);"
  fi
  POWERARM_ENABLECODECACHINGWIP=0 POWERARM_DUMPIR=stderr POWERARM_PASSMANAGERDUMPIR=afteropt \
    "$emu" ./ccmpchain > /dev/null 2> ccmpchain.irdump
  POWERARM_DISABLECCMPCHAIN=1 POWERARM_ENABLECODECACHINGWIP=0 POWERARM_DUMPIR=stderr POWERARM_PASSMANAGERDUMPIR=afteropt \
    "$emu" ./ccmpchain > /dev/null 2> ccmpchain.irdump.nochain
  cc_chains=$(grep -cE '\bChainCompare\b' ccmpchain.irdump 2> /dev/null)
  cc_chains_off=$(grep -cE '\bChainCompare\b' ccmpchain.irdump.nochain 2> /dev/null)
  # Under POWERARM_MAXINST=1 the branch's legs are constant exits, every exit
  # is seeded with all four flags live, and no chain is collapsible at all --
  # so the test is exercising the old lowering on both shapes. A mode of the
  # test, not a failure of it; the value checks still have to pass.
  if [ -n "${POWERARM_MAXINST:-}" ] && [ "${POWERARM_MAXINST:-}" != 0 ]; then
    cc_want=0
  else
    cc_want=1
  fi
  if [ "$cc_want" = 1 ] && [ "$cc_chains" -lt 1 ]; then
    cc_fail=1
    cc_detail="$cc_detail no chain was collapsed, so both shapes ran the old lowering;"
  fi
  if [ "$cc_chains_off" != 0 ]; then
    cc_fail=1
    cc_detail="$cc_detail POWERARM_DISABLECCMPCHAIN=1 still collapsed $cc_chains_off;"
  fi
  if [ "$cc_fail" = 0 ]; then
    report PASS ccmpchain "$cc_pass chain checks clean, control FAILed; collapsed $cc_chains chains, none with the switch"
  else
    report FAIL ccmpchain "$cc_detail see $out/ccmpchain.powerarm"
  fi
fi

if [ -f threadexit ]; then
  te_total=96
  te_batch=8
  # Not in OUTDIR: see the re-entry comment at the top of this file. OUTDIR is
  # the shared golden directory, and the count below only means anything if
  # this run is the only one writing the file it counts.
  te_work=$(mktemp -d "${TMPDIR:-/tmp}/threadexit-loop.XXXXXX") || exit 2
  : > "$te_work/codes"
  if command -v setsid > /dev/null 2>&1 && setsid --wait true > /dev/null 2>&1; then
    setsid --wait sh "$me" --threadexit-loop "$emu" "$te_total" "$te_batch" "$te_work" > /dev/null 2>&1
    te_pg=$(cat "$te_work/pgid" 2> /dev/null || echo)
    case $te_pg in
    '' | *[!0-9]*) ;;
    *) kill -KILL -- -"$te_pg" 2> /dev/null ;;
    esac
  else
    sh "$me" --threadexit-loop "$emu" "$te_total" "$te_batch" "$te_work" > /dev/null 2>&1
  fi
  te_bad=$(awk '$1 != 0' "$te_work/codes" | wc -l)
  te_ran=$(wc -l < "$te_work/codes")
  if [ "$te_bad" = 0 ] && [ "$te_ran" = "$te_total" ]; then
    report PASS threadexit_loop "$te_total exits with threads mid-teardown, all clean"
  else
    # 124 in the list is timeout's: that run hung with a thread stuck in
    # teardown rather than crashing. 139 is the old SIGSEGV mode, 191 the old
    # SIG_DFL-on-signal-63 mode.
    report FAIL threadexit_loop "$te_bad of $te_ran (of $te_total) did not exit 0 (124 = hung, killed): $(sort -n "$te_work/codes" | uniq -c | tr '\n' ' ')"
  fi
  rm -rf "$te_work"
fi

if [ "$skip" -gt 0 ]; then
  echo "passed $pass, failed $fail, skipped $skip"
else
  echo "passed $pass, failed $fail"
fi
[ "$fail" = 0 ]
