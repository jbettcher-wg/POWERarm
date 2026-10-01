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
        "$te_emu" ./threadexit > /dev/null 2>&1
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
  POWERARM_NZCVEXITDEAD=on POWERARM_NZCVEXITDEADSTATS=1 POWERARM_ENABLECODECACHINGWIP=0 \
    "$emu" ./cmpbranch > /dev/null 2> nzcv_window.err
  POWERARM_NZCVEXITDEAD=on POWERARM_NZCVEXITDEADSTATS=1 POWERARM_NZCVEXITDEADNOWINDOW=1 POWERARM_ENABLECODECACHINGWIP=0 \
    "$emu" ./cmpbranch > /dev/null 2> nzcv_window_nocheck.err
  wl=$(grep '^NZCV_EXITDEAD ' nzcv_window.err | tail -1)
  wl_no=$(grep '^NZCV_EXITDEAD ' nzcv_window_nocheck.err | tail -1)
  w_dead=$(echo "$wl" | sed -n 's/.* dead=\([0-9]*\).*/\1/p')
  w_below=$(echo "$wl" | sed -n 's/.*refused_below_entry=\([0-9]*\).*/\1/p')
  w_below_no=$(echo "$wl_no" | sed -n 's/.*refused_below_entry=\([0-9]*\).*/\1/p')
  w_dead_no=$(echo "$wl_no" | sed -n 's/.* dead=\([0-9]*\).*/\1/p')
  if [ -z "$w_dead" ] || [ -z "$w_below" ] || [ -z "$w_below_no" ]; then
    report FAIL nzcv_exit_dead_window "no NZCV_EXITDEAD stats line: [$wl] [$wl_no]; see $out/nzcv_window.err"
  elif [ "$w_dead" -le 0 ]; then
    report FAIL nzcv_exit_dead_window "no verdict was taken at all (dead=$w_dead), so this check tests nothing; see $out/nzcv_window.err"
  elif [ "$w_below_no" != 0 ]; then
    report FAIL nzcv_exit_dead_window "the lever does not disable the check: refused_below_entry=$w_below_no with POWERARM_NZCVEXITDEADNOWINDOW=1"
  elif [ "$w_below" -gt 0 ] && [ "$w_dead_no" -le "$w_dead" ]; then
    report FAIL nzcv_exit_dead_window "the check refused $w_below verdicts but disabling it did not accept more (dead=$w_dead vs $w_dead_no): it has no teeth"
  else
    report PASS nzcv_exit_dead_window "dead=$w_dead, refused_below_entry=$w_below; lever off: dead=$w_dead_no"
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
    report FAIL threadexit_loop "$te_bad of $te_ran (of $te_total) did not exit 0: $(sort -n "$te_work/codes" | uniq -c | tr '\n' ' ')"
  fi
  rm -rf "$te_work"
fi

if [ "$skip" -gt 0 ]; then
  echo "passed $pass, failed $fail, skipped $skip"
else
  echo "passed $pass, failed $fail"
fi
[ "$fail" = 0 ]
