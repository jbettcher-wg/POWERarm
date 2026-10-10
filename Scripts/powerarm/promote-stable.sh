#!/bin/sh
# Copy a POWERarm build into the stable install that binfmt_misc, the desktop
# launchers and the `claude` wrapper all point at.
#
#   sh Scripts/powerarm/promote-stable.sh              # promote build-powerarm
#   sh Scripts/powerarm/promote-stable.sh DIR          # promote another build dir
#   sh Scripts/powerarm/promote-stable.sh -n [DIR]     # say what it would do
#
# WHY A SEPARATE INSTALL. Rebuilding build-powerarm must not change what
# binfmt-launched programs run: binfmt_misc is registered with the 'F' flag,
# which opens the interpreter at registration time, so a rebuild under a
# running registration is at best ignored and at worst half-swapped. The stable
# copy is only ever replaced by this script, as a whole directory.
#
# AND WHY THE REGISTRATION MUST BE REDONE AFTER. The swap below is `mv` of
# directories, so the file at .../Bin/POWERarm gets a NEW INODE. The 'F' flag
# pinned the old one. Until the entry is re-registered, binfmt may keep running
# the PREVIOUS emulator while still printing the right path -- a silent stale
# emulator, which once cost an evening. check-binfmt-inode.sh is what proves
# which inode is actually being executed; do not trust the printed path.
#
# Previous stable is kept as <dst>.prev, so a bad promotion is a swap back.
set -e

DRY=0
if [ "${1:-}" = -n ] || [ "${1:-}" = --dry-run ]; then
  DRY=1
  shift
fi

# The repo is wherever this script lives, not a hardcoded home path: this tree
# is reachable as both /home/... and /mnt/arch/home/... and the build dir has
# to come from the same one the script was invoked from.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/../.." && pwd)

SRC=${1:-$REPO/build-powerarm}
DST=$HOME/.local/opt/powerarm-stable

[ -x "$SRC/Bin/POWERarm" ] || {
  echo "no POWERarm binary in $SRC/Bin" >&2
  exit 2
}

# Refuse to promote a tree with uncommitted changes unless it is deliberate:
# VERSION records a commit hash, and a hash that does not describe the bytes is
# worse than no hash at all.
DIRTY=$(cd "$REPO" && git status --porcelain 2>/dev/null | head -1)
if [ -n "$DIRTY" ]; then
  echo "warning: $REPO has uncommitted changes; VERSION will name a commit that is not what you are installing" >&2
fi

VER=$(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null) || VER=
SUBJ=$(cd "$REPO" && git log -1 --format=%s 2>/dev/null) || SUBJ=

if [ "$DRY" = 1 ]; then
  echo "would promote: $SRC"
  echo "           to: $DST   (current -> $DST.prev)"
  echo "      version: ${VER:-unknown} ${SUBJ:+- $SUBJ}"
  [ -f "$DST/VERSION" ] && {
    echo "  replacing:"
    sed 's/^/    /' "$DST/VERSION"
  }
  exit 0
fi

rm -rf "$DST.new" "$DST.prev"
mkdir -p "$DST.new"
cp -a "$SRC/Bin" "$DST.new/Bin"
[ -d "$SRC/Data" ] && cp -a "$SRC/Data" "$DST.new/Data"
# The build calls it Guest/; the install calls it GuestThunks/, which is what
# ThunkGuestLibs defaults to.
if [ -d "$SRC/Guest" ]; then
  mkdir -p "$DST.new/GuestThunks"
  cp -a "$SRC/Guest"/* "$DST.new/GuestThunks/"
elif [ -d "$SRC/GuestThunks" ]; then
  cp -a "$SRC/GuestThunks" "$DST.new/GuestThunks"
fi

if [ -n "$VER" ]; then
  printf '%s\n%s\n' "$VER" "$SUBJ" > "$DST.new/VERSION"
else
  echo "unknown revision" > "$DST.new/VERSION"
fi

[ -d "$DST" ] && mv "$DST" "$DST.prev"
mv "$DST.new" "$DST"

echo "promoted to $DST"
sed 's/^/  /' "$DST/VERSION"
echo
echo "now:  sudo systemctl restart systemd-binfmt     # or: sudo sh $HERE/register-binfmt.sh"
echo "then: bash $HERE/check-binfmt-inode.sh          # proves WHICH inode binfmt runs"
