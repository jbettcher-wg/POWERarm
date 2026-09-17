#!/bin/sh
# SPDX-License-Identifier: MIT
# Run a command natively inside an AArch64 rootfs, unprivileged (golden side).
#
#   a64diff-rootfs-exec.sh ROOTFS WORKDIR -- ARGV...
#
# This is the thin adapter a64diff uses (--rootfs-exec) until the sysroot
# work's own Pi runner lands; any runner with the same calling convention can
# replace it.  It uses bwrap in a user namespace, so no root is needed:
#   - / is an empty tmpfs; every top-level entry of ROOTFS is bind-mounted
#     read-only onto it (symlinks such as lib -> usr/lib are recreated), so
#     the rootfs doesn't need /dev, /proc or /tmp mount points of its own;
#   - /dev, /proc and /tmp are fresh; WORKDIR is bound read-write at its own
#     absolute path and is the working directory (jobs must not print or embed
#     that path: it differs between the golden machine and the POWER9);
#   - the environment is passed through unchanged (a64diff already fixes it).
set -eu
[ $# -ge 3 ] && [ "$3" = "--" ] || { echo "usage: $0 ROOTFS WORKDIR -- ARGV..." >&2; exit 125; }
rootfs=$(cd "$1" && pwd -P)
work=$(cd "$2" && pwd -P)
shift 3
command -v bwrap >/dev/null || { echo "a64diff-rootfs-exec: bwrap not found" >&2; exit 125; }

set -- --dev /dev --proc /proc --tmpfs /tmp --dir "$work" --bind "$work" "$work" --chdir "$work" -- "$@"
for e in "$rootfs"/* "$rootfs"/.[!.]*; do
  [ -e "$e" ] || [ -L "$e" ] || continue
  name=${e##*/}
  case $name in dev|proc|tmp) continue ;; esac
  if [ -L "$e" ]; then
    set -- --symlink "$(readlink "$e")" "/$name" "$@"
  else
    set -- --ro-bind "$e" "/$name" "$@"
  fi
done
exec bwrap --unshare-user --unshare-pid --unshare-ipc --unshare-uts --die-with-parent --tmpfs / "$@"
