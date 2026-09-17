#!/bin/bash
# Check that the binfmt_misc registration really runs the emulator it names.
#
#   bash Scripts/powerarm/check-binfmt-inode.sh
#
# binfmt_misc's 'F' flag opens the interpreter when the entry is registered and
# pins that inode for the life of the entry.  The entry keeps PRINTING the path
# string it was given, so an entry registered before promote-powerarm-stable.sh
# ran still reads
#
#     interpreter /home/.../powerarm-stable/Bin/POWERarm
#
# while every aarch64 binary it starts executes the PREVIOUS build, which
# promote moved aside to powerarm-stable.prev.  Nothing in the entry, in `ps`
# or in the process's own argv shows it: the kernel passes the registered path
# string as argv[0] regardless of which inode it opened.  A whole evening of
# "the fix didn't take" came out of exactly that.
#
# So ask the running process instead.  Start a real aarch64 program through
# binfmt and read the emulator's mapped path out of /proc/<pid>/maps.  With
# ptrace_scope=1 that file is readable only by an ancestor of the process and
# only through bash builtins -- an external `cat` or `readlink` is a sibling
# and gets EPERM -- hence the probe is a child of THIS shell and the maps file
# is read with `read`.
set -u

ENTRY=/proc/sys/fs/binfmt_misc/POWERarm-aarch64

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -e "$ENTRY" ] || fail "no binfmt entry at $ENTRY (run register-powerarm-binfmt.sh)"
grep -qx enabled "$ENTRY" || fail "the binfmt entry is registered but disabled"

INTERP=$(awk '/^interpreter/ {print $2}' "$ENTRY")
[ -n "$INTERP" ] || fail "no interpreter line in $ENTRY"

# An aarch64 ELF on a host path, so binfmt_misc is what starts the emulator.
PROBE=${1:-}
if [ -z "$PROBE" ]; then
  for C in "${POWERARM_ROOTFS:-/nonexistent}/usr/bin/sleep" "$HOME"/.local/share/powerarm/RootFS/*/usr/bin/sleep; do
    [ -x "$C" ] && { PROBE=$C; break; }
  done
fi
[ -n "$PROBE" ] && [ -x "$PROBE" ] || fail "no aarch64 probe binary; pass one as an argument"

"$PROBE" 5 &
P=$!
trap 'kill $P 2>/dev/null' EXIT

# The emulator maps itself before it maps anything of the guest's, but give the
# exec a moment; a cold start after a rebuild is slower than it looks.
RUNNING=
for _ in $(seq 40); do
  while read -r _ _ _ _ _ PATHNAME; do
    case $PATHNAME in
    */Bin/POWERarm | */Bin/POWERarm' '*) RUNNING=$PATHNAME; break ;;
    esac
  done < "/proc/$P/maps" 2>/dev/null
  [ -n "$RUNNING" ] && break
  kill -0 $P 2>/dev/null || fail "the probe exited before the emulator mapped itself; run it by hand: $PROBE"
  sleep 0.1
done
[ -n "$RUNNING" ] || fail "couldn't read the emulator path from /proc/$P/maps (run this under bash, as the probe's parent)"

kill $P 2>/dev/null

case $RUNNING in
*'(deleted)')
  echo "registered: $INTERP"
  echo "running:    $RUNNING"
  fail "the entry pins a DELETED inode -- re-register: sudo sh register-powerarm-binfmt.sh"
  ;;
esac

if [ "$RUNNING" != "$INTERP" ]; then
  echo "registered: $INTERP"
  echo "running:    $RUNNING"
  fail "the entry names one emulator and runs another (the 'F' flag pinned the
      inode that was at that path when the entry was registered, and a promote
      has moved it since) -- re-register: sudo sh register-powerarm-binfmt.sh"
fi

echo "OK: binfmt runs $RUNNING"
[ -f "${INTERP%/Bin/POWERarm}/VERSION" ] && sed 's/^/    /' "${INTERP%/Bin/POWERarm}/VERSION"
exit 0
