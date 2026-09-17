#!/bin/bash
# SPDX-License-Identifier: MIT
# Run a command natively inside an aarch64 sysroot as its "/", without root.
# Reference side of the M2 byte-compares: use it on the Raspberry Pi 5 (or any arm64 Linux).
#
#   run-in-sysroot.sh [options] <sysroot> <cwd> -- <cmd> [args...]
#
#   <sysroot>        rootfs directory, mounted read-only as /
#   <cwd>            host directory bound read-write and used as the working directory
#   --cwd-as PATH    path <cwd> appears at inside the sysroot (default: the same absolute
#                    path as on the host, which is what POWERarm sees; its top-level
#                    directory is covered by a tmpfs if needed)
#   --bind SRC DST   extra read-write bind (repeatable)
#   --ro-bind SRC DST  extra read-only bind (repeatable)
#   --env KEY=VALUE  add or override one environment variable (repeatable)
#   --rw-root        mount the sysroot read-write (never for reference runs)
#   --backend bwrap|unshare   default: bwrap if installed, else unshare + chroot
#
# Sandbox: new user/mount/pid/ipc/uts/net namespaces (no network), private /proc, a minimal
# /dev (null zero full random urandom tty, pts, shm), empty tmpfs /tmp, hostname
# "powerarm", umask 022.  The environment is cleared and set to the fixed values below so
# outputs don't depend on the caller:
#   PATH=/usr/local/sbin:/usr/local/bin:/usr/bin  LC_ALL=C  LANG=C  TZ=UTC
#   SOURCE_DATE_EPOCH=0  HOME=/tmp  TMPDIR=/tmp  SHELL=/bin/sh  TERM=dumb  PWD=<cwd>
# The command's exit status is returned.
set -euo pipefail

usage() { sed -n '3,26p' "$0" >&2; exit 2; }

cwd_as="" rwroot=0 backend="" extra=() envs=()
while [ $# -gt 0 ]; do
  case "$1" in
    --cwd-as) cwd_as=$2; shift 2 ;;
    --bind|--ro-bind) extra+=("$1" "$2" "$3"); shift 3 ;;
    --env) envs+=("$2"); shift 2 ;;
    --rw-root) rwroot=1; shift ;;
    --backend) backend=$2; shift 2 ;;
    -h|--help) usage ;;
    --) shift; break ;;
    -*) echo "unknown option: $1" >&2; usage ;;
    *) break ;;
  esac
done
[ $# -ge 4 ] && [ "$3" = "--" ] || usage
sysroot=$(realpath -e "$1") cwd=$(realpath -e "$2")
shift 3
[ -x "$sysroot/usr/bin/sh" ] || [ -L "$sysroot/bin/sh" ] || [ -x "$sysroot/bin/sh" ] ||
  { echo "error: $sysroot does not look like a rootfs (no /bin/sh)" >&2; exit 2; }
[ -n "$cwd_as" ] || cwd_as=$cwd
case "$cwd_as" in /*) ;; *) echo "error: --cwd-as must be absolute" >&2; exit 2 ;; esac

fixed_env=(
  PATH=/usr/local/sbin:/usr/local/bin:/usr/bin
  LC_ALL=C LANG=C TZ=UTC SOURCE_DATE_EPOCH=0
  HOME=/tmp TMPDIR=/tmp SHELL=/bin/sh TERM=dumb
  "PWD=$cwd_as"
)
fixed_env+=("${envs[@]}")

# Directories that exist in the sysroot and may be covered by a tmpfs to create mount points.
top=/$(echo "$cwd_as" | cut -d/ -f2)
case "$top" in
  /usr|/etc|/proc|/dev|/sys|/run) echo "error: --cwd-as under $top is not allowed" >&2; exit 2 ;;
esac

if [ -z "$backend" ]; then
  if command -v bwrap >/dev/null; then backend=bwrap; else backend=unshare; fi
fi
umask 022

case "$backend" in
bwrap)
  # "/" is a tmpfs populated with binds of the sysroot's top-level entries, so --cwd-as can
  # name any path; the tmpfs itself is remounted read-only at the end.
  args=(--unshare-all --die-with-parent --hostname powerarm --tmpfs /)
  bindop=--ro-bind; [ "$rwroot" = 1 ] && bindop=--bind
  for ent in "$sysroot"/* "$sysroot"/.[!.]*; do
    [ -e "$ent" ] || [ -L "$ent" ] || continue
    name=/${ent##*/}
    case "$name" in /proc|/dev|/tmp|"$top") continue ;; esac
    if [ -L "$ent" ]; then args+=(--symlink "$(readlink "$ent")" "$name")
    else args+=("$bindop" "$ent" "$name"); fi
  done
  args+=(--proc /proc --dev /dev --tmpfs /tmp)
  if [ "$top" != /tmp ]; then
    if [ -L "$sysroot$top" ]; then
      echo "error: $top is a symlink in the sysroot; use --cwd-as" >&2; exit 2
    elif [ "$cwd_as" != "$top" ]; then
      args+=(--tmpfs "$top")     # hides the sysroot's $top (e.g. /home) to hold the mount point
    fi
  fi
  args+=(--bind "$cwd" "$cwd_as")
  args+=("${extra[@]}")
  [ "$rwroot" = 1 ] || args+=(--remount-ro /)
  args+=(--chdir "$cwd_as" --clearenv)
  for e in "${fixed_env[@]}"; do args+=(--setenv "${e%%=*}" "${e#*=}"); done
  exec bwrap "${args[@]}" -- "$@"
  ;;
unshare)
  # Fallback: unshare -r (map to root in a user namespace) + private mounts + chroot.
  command -v unshare >/dev/null || { echo "error: neither bwrap nor unshare found" >&2; exit 2; }
  [ "${#extra[@]}" = 0 ] || { echo "error: --bind/--ro-bind need the bwrap backend" >&2; exit 2; }
  [ -d "$sysroot$top" ] && [ ! -L "$sysroot$top" ] ||
    { echo "error: unshare backend: $top must be a directory in the sysroot" >&2; exit 2; }
  export _SR="$sysroot" _CWD="$cwd" _CWDAS="$cwd_as" _TOP="$top" _RW="$rwroot"
  exec unshare --user --map-root-user --mount --pid --fork --ipc --uts --net \
    /bin/sh -euc '
    new=$_SR                      # bind the sysroot onto itself; all mounts stay private
    mount --rbind "$_SR" "$new"
    mount -t proc proc "$new/proc"
    mount -t tmpfs -o mode=755 tmpfs "$new/dev"
    for d in null zero full random urandom tty; do
      [ -e "/dev/$d" ] || continue
      touch "$new/dev/$d"; mount --bind "/dev/$d" "$new/dev/$d"
    done
    mkdir -p "$new/dev/shm" "$new/dev/pts"
    ln -s /proc/self/fd "$new/dev/fd"
    mount -t tmpfs -o mode=1777 tmpfs "$new/tmp"
    if [ "$_TOP" != /tmp ] && [ "$_CWDAS" != "$_TOP" ]; then mount -t tmpfs tmpfs "$new$_TOP"; fi
    mkdir -p "$new$_CWDAS"
    mount --bind "$_CWD" "$new$_CWDAS"
    [ "$_RW" = 1 ] || mount -o remount,bind,ro "$new"
    hostname powerarm
    exec chroot "$new" /usr/bin/env -i "$@"
  ' sh "${fixed_env[@]}" /bin/sh -c 'cd "$1" && shift && exec "$@"' sh "$cwd_as" "$@"
  ;;
*) echo "error: unknown backend $backend" >&2; exit 2 ;;
esac
