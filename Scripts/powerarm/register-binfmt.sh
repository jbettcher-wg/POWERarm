#!/bin/sh
# Register the promoted POWERarm install as the binfmt_misc interpreter for
# AArch64 ELFs. Run as root, after every promote:
#
#   sudo sh Scripts/powerarm/register-binfmt.sh
#   sudo sh Scripts/powerarm/register-binfmt.sh /path/to/other/Bin/POWERarm
#
# WHY AFTER EVERY PROMOTE. The 'F' flag in the entry below opens the
# interpreter when the entry is registered and keeps that open file for the
# life of the entry. promote-stable.sh replaces the install by `mv`-ing
# directories, so .../Bin/POWERarm gets a new inode and the entry keeps
# executing the OLD one -- while still printing the new path, which is what
# makes it a silent failure. check-binfmt-inode.sh is the only honest check.
#
# The same registration is also in /etc/binfmt.d/POWERarm-aarch64.conf, which
# is what restores it across a reboot; `systemctl restart systemd-binfmt`
# re-registers from there and is equivalent to this script whenever the
# interpreter path has not changed. This script exists for the case where it
# has, and to be explicit about the flags.
set -e

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
EMU=${1:-$HOME/.local/opt/powerarm-stable/Bin/POWERarm}
NAME=POWERarm-aarch64
PROC=/proc/sys/fs/binfmt_misc

# $HOME under sudo is root's unless -E was passed, so a bare `sudo sh ...` would
# look in /root. Fall back to the invoking user's home.
if [ ! -x "$EMU" ] && [ -n "${SUDO_USER:-}" ]; then
  ALT=$(getent passwd "$SUDO_USER" | cut -d: -f6)/.local/opt/powerarm-stable/Bin/POWERarm
  [ -x "$ALT" ] && EMU=$ALT
fi

[ "$(id -u)" = 0 ] || {
  echo "must run as root (writes $PROC/register)" >&2
  exit 2
}
[ -x "$EMU" ] || {
  echo "no executable emulator at $EMU -- promote first" >&2
  exit 2
}

[ -e "$PROC/register" ] || {
  modprobe binfmt_misc 2>/dev/null || true
  mountpoint -q "$PROC" || mount -t binfmt_misc binfmt_misc "$PROC" 2>/dev/null || true
}
[ -e "$PROC/register" ] || {
  echo "binfmt_misc is not available at $PROC" >&2
  exit 2
}

# An AArch64 ELF64 LE object: e_ident + e_type(2, ET_EXEC) + e_machine(0xb7).
# The mask lets e_type's low byte vary so ET_DYN (3) matches too, which is what
# a PIE guest is, and ignores the ABI/padding bytes.
MAGIC='\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00'
MASK='\xff\xff\xff\xff\xff\xfe\xfe\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'

# P: preserve argv[0]. O: open the binary for the interpreter. C: compute
# credentials from the target, not the interpreter. F: pin the interpreter now
# (see the header).
FLAGS=POCF

# A stale entry of the same name must go first; register refuses a duplicate.
if [ -e "$PROC/$NAME" ]; then
  printf -- -1 > "$PROC/$NAME"
fi

# Any other entry claiming AArch64 would shadow this one depending on order.
for q in "$PROC"/*; do
  case $q in
  "$PROC/register" | "$PROC/status" | "$PROC/$NAME") continue ;;
  esac
  [ -e "$q" ] || continue
  if grep -q 'b7 00\|\xb7' "$q" 2>/dev/null && grep -q '^enabled' "$q" 2>/dev/null; then
    echo "warning: $(basename "$q") is also registered and may take precedence;" >&2
    echo "         disable it with: printf -- -1 > $q" >&2
  fi
done

printf '%s' ":$NAME:M:0:$MAGIC:$MASK:$EMU:$FLAGS" > "$PROC/register"

echo "registered: $EMU"
[ -f "$(dirname "$(dirname "$EMU")")/VERSION" ] && sed 's/^/  /' "$(dirname "$(dirname "$EMU")")/VERSION"
echo
echo "verify:  bash $HERE/check-binfmt-inode.sh"
