#!/bin/bash
# wl-perf.sh -- the gcc compile under POWERarm with the mitigation-relevant PMCs,
# so the per-syscall tax can be compared against the workload's own totals.
set -u
S=$(cd "$(dirname "$0")" && pwd)
EMU=${EMU:?set EMU to a POWERarm build binary, host path (no /mnt/arch prefix)}
SR=/home/jbettcher/.local/share/powerarm/RootFS/ArchLinuxARM-m2
PERF=/mnt/arch/usr/bin/perf
export LD_LIBRARY_PATH=$HOME/.local/lib/perfshim
CPU=${CPU:-120-127}
W=/tmp/pa-mit-perf
FILES="lapi.c lcode.c ldebug.c ldo.c lgc.c llex.c lparser.c lstrlib.c ltable.c lvm.c"
rm -rf "$W"; mkdir -p "$W"
tar xzf "$HOME/.cache/powerarm/m2-sources/lua-5.4.9.tar.gz" -C "$W" || exit 1
SRC=$W/lua-5.4.9/src
CMD="for f in $FILES; do gcc -std=gnu99 -O2 -Wall -fPIC -DLUA_COMPAT_5_3 -DLUA_USE_LINUX -c \$f || exit 1; done"

# r3e054 PM_LD_MISS_L1, r40ac pm_br_mpred_ccache, r40a4 pm_br_pred_ccache,
# r48ac pm_br_mpred_lstack, r40a8 pm_br_pred_lstack
for g in "cycles:u,instructions:u,r3e054:u,r40ac:u" "cycles:u,r40a4:u,r48ac:u,r40a8:u"; do
  rm -f "$SRC"/*.o
  echo "-- group: $g"
  ( cd "$SRC" && taskset -c $CPU env SOURCE_DATE_EPOCH=0 LC_ALL=C TZ=UTC \
      POWERARM_PORTABLE=1 POWERARM_ROOTFS="$SR" \
      $PERF stat -x, -e "$g" -- "$EMU" /usr/bin/sh -c "$CMD" ) 2>&1 >/dev/null | grep -v '^$'
done
