#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# check-code-cache-contend.sh BUILD_DIR [ROOTFS]
#
# Regression test for the code-cache save path holding the shared
# CodeInvalidationMutex across a blocking cross-process flock(LOCK_SH), which
# starves an in-process SMC invalidation past the 4 s stall detector and aborts
# the process with a forced trap (POWERarm issue #1, the 2026-09-19 VS Code
# crash). The stall detector (TakeCodeInvalidationWriteLockOrSteal) is a
# deadlock detector; a cross-process cache save makes contention look like a
# deadlock.
#
# The contender guest (a) compiles fresh blocks in a small dlopen'd library and
# (b) runs a background thread that repeatedly self-modifies its own code (an
# SMC invalidation, which wants the EXCLUSIVE CodeInvalidationMutex). Its
# dlclose triggers SaveCodeCachesBeforeUnmap, which writes a segment and then
# takes flock(LOCK_SH) on the library's cache .lock. A holder process keeps
# LOCK_EX on that file for the whole run, so the save's flock blocks.
#
#   pre-fix : the save holds the shared invalidation lock across the blocked
#             flock, so the SMC invalidation cannot acquire the exclusive lock
#             for 4 s and the process aborts (SIGTRAP, backtrace through
#             TakeCodeInvalidationWriteLockOrSteal -> ForcedAssert).
#   post-fix: the save no longer holds the invalidation lock, so the
#             invalidation proceeds and the guest survives.
#
# The contender runs against a FRESH cache directory (not the one primed to
# learn the lock name), so it actually has new blocks to save and its dlclose
# save reaches the flock instead of being a no-op.
#
# Pass: the contender prints "contend: survived" and exits 0.
# Uses a private HOME/TMPDIR/cache/socket like check-code-cache.sh, so it
# neither sees nor disturbs any other POWERarmServer or cache.
# Exit 0: OK. 1: a check failed. 2: could not run.
set -u
ulimit -c 0 # the pre-fix abort traps; do not write a (large) core

build=${1:?usage: check-code-cache-contend.sh BUILD_DIR [ROOTFS]}
build=$(realpath -e "$build") || { echo "check-code-cache-contend: no build dir '$1'" >&2; exit 2; }
emu=$build/Bin/POWERarm
[ -x "$emu" ] || { echo "check-code-cache-contend: no POWERarm in $build/Bin" >&2; exit 2; }
rootfs=${2:-${XDG_DATA_HOME:-$HOME/.local/share}/powerarm/RootFS/ArchLinuxARM-m2}
rootfs=$(realpath -e "$rootfs") || { echo "check-code-cache-contend: no rootfs '$rootfs'" >&2; exit 2; }
[ -x "$rootfs/usr/bin/gcc" ] || { echo "check-code-cache-contend: no gcc in rootfs $rootfs" >&2; exit 2; }
command -v flock >/dev/null || { echo "check-code-cache-contend: host flock(1) not found" >&2; exit 2; }

w=$(mktemp -d "${TMPDIR:-/tmp}/check-code-cache-contend.XXXXXX")
trap 'rm -rf "$w"' EXIT
mkdir -p "$w/home" "$w/run" "$w/src" "$w/bin" "$w/cacheA/cache" "$w/cacheB/cache"
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

# run CACHEDIR [ENV=V...] -- ARGS : one POWERarm process tree, cache on when
# CACHEDIR is not "-". Same shape as check-code-cache.sh.
run() {
  local cache=$1
  shift
  local envs=()
  if [ "$cache" != - ]; then
    envs+=(POWERARM_ENABLECODECACHINGWIP=1 POWERARM_CODECACHESCOPE=all POWERARM_CODECACHESTATS=1 POWERARM_APP_CACHE_LOCATION="$cache/")
    # Force the hard SMC invalidation (TakeCodeInvalidationWriteLockOrSteal),
    # the exclusive-CodeInvalidationMutex waiter the bug starves.
    envs+=(POWERARM_SMCCHECKS=full)
  fi
  while [ $# -gt 0 ] && [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  env -i PATH=/usr/bin:/bin HOME="$w/home" TMPDIR="$w/run" LC_ALL=C \
    POWERARM_SERVERSOCKETPATH="$w/run/server.sock" \
    POWERARM_PORTABLE=1 POWERARM_ROOTFS="$rootfs" "${envs[@]}" "$emu" "$@"
}

# ---------------------------------------------------------------------------
# Guest sources.
#
# libpad.so: 16 distinct exported function entries (more than the cache's
# MinNewBlocksPerSegment = 8), so its dlclose save writes a segment and reaches
# the cache .lock.
cat > "$w/src/libpad.c" << 'EOF'
int p0(void){return 0;}  int p1(void){return 1;}  int p2(void){return 2;}
int p3(void){return 3;}  int p4(void){return 4;}  int p5(void){return 5;}
int p6(void){return 6;}  int p7(void){return 7;} int p8(void){return 8;}
int p9(void){return 9;}  int p10(void){return 10;} int p11(void){return 11;}
int p12(void){return 12;} int p13(void){return 13;} int p14(void){return 14;}
int p15(void){return 15;}
EOF

# contend: compile fresh blocks in libpad, and run a background thread that
# self-modifies its own code (an SMC invalidation) until the dlclose save is
# through. The dlclose is the deterministic, single-thread save that blocks
# behind the holder's LOCK_EX.
cat > "$w/src/contend.c" << 'EOF'
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <unistd.h>

__attribute__((noipa)) int value(void) { return 1; }

static void patch(void) {
  long page = sysconf(_SC_PAGESIZE);
  uintptr_t start = (uintptr_t)value & ~(uintptr_t)(page - 1);
  if (mprotect((void *)start, 2 * page, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    _exit(3);
  uint32_t insn = 0x52800040; /* aarch64: mov w0, #2 */
  memcpy((void *)value, &insn, sizeof insn);
  __builtin___clear_cache((char *)value, (char *)value + 4);
  mprotect((void *)start, 2 * page, PROT_READ | PROT_EXEC);
}

static volatile int done = 0;

/* Each patch is an SMC invalidation that wants the exclusive
   CodeInvalidationMutex. */
static void *inval(void *a) {
  (void)a;
  while (!done) {
    patch();
    usleep(20 * 1000);
  }
  return NULL;
}

int main(void) {
  value(); /* compile the block the invalidations target */

  pthread_t t;
  pthread_create(&t, NULL, inval, NULL);

  void *h = dlopen("./libpad.so", RTLD_NOW);
  if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
  for (int i = 0; i < 16; i++) {
    char nm[16];
    snprintf(nm, sizeof nm, "p%d", i);
    void (*f)(void) = dlsym(h, nm);
    if (!f) { fprintf(stderr, "dlsym %s\n", nm); return 2; }
    f(); /* compile a fresh block in the library */
  }

  /* dlclose -> munmap -> SaveCodeCachesBeforeUnmap on THIS thread. It writes a
     segment, then flock(LOCK_SH) on the library's cache .lock, which blocks
     behind the holder's LOCK_EX. While it is blocked, the inval thread's SMC
     invalidation wants the exclusive CodeInvalidationMutex. */
  dlclose(h);

  done = 1;
  pthread_join(t, NULL);
  printf("contend: survived\n");
  return 0;
}
EOF

cd "$w/src" || exit 2
run - -- /usr/bin/gcc -O2 -shared -fPIC -o libpad.so libpad.c && \
  run - -- /usr/bin/gcc -O2 -o contend contend.c -ldl -pthread || {
    echo "check-code-cache-contend: building the guest programs failed" >&2
    exit 2
  }
cp libpad.so contend "$w/bin/"

# ---------------------------------------------------------------------------
# Phase 1: prime a throwaway cache (no holder) so the library's cache .lock
# exists and we know its name. The contender runs against a different, fresh
# cache so it compiles libpad fresh and has new blocks to save.
run "$w/cacheA" -- "$w/bin/contend" > "$w/prime.log" 2>&1 || true
lockbase=$(find "$w/cacheA" -name '*libpad*.lock' 2>/dev/null | head -1 | xargs -r basename)
if [ -z "$lockbase" ]; then
  echo "check-code-cache-contend: no libpad cache .lock after the prime run" >&2
  echo "---- cacheA ----"; find "$w/cacheA" -type f 2>/dev/null >&2
  echo "---- prime log ----"; cat "$w/prime.log" >&2
  exit 2
fi
echo "lock: $lockbase"

# ---------------------------------------------------------------------------
# Phase 2: holder keeps LOCK_EX on the library's cache .lock in the FRESH
# cacheB directory for the whole contender run. This is the other process the
# contender's save's flock(LOCK_SH) blocks behind (in the VS Code crash it was
# a sibling compacting the cache, and after it crashed, the held lock survived
# until the core dump drained).
mkdir -p "$w/cacheB/cache"
holder_secs=${HOLDER_SECS:-10}
flock "$w/cacheB/cache/$lockbase" -c "sleep $holder_secs" &
holder=$!
sleep 0.5 # let the holder take the lock before the contender runs

# ---------------------------------------------------------------------------
# Phase 3: contender against the fresh cacheB.
out=$(run "$w/cacheB" -- "$w/bin/contend" 2> "$w/contend.log")
rc=$?
wait "$holder" 2>/dev/null || true

if [ "$rc" -eq 0 ] && [ "$out" = "contend: survived" ]; then
  ok "contend: guest survived a save blocked behind a cross-process LOCK_EX"
else
  bad "contend: guest did not survive (rc=$rc, out='$out')"
  echo "---- contender log ----"; cat "$w/contend.log"
  if grep -q 'write-lock stalled' "$w/contend.log" 2>/dev/null; then
    echo "---- reproduced the issue-1 abort: 'write-lock stalled' ----"
  elif grep -qE 'TakeCodeInvalidationWriteLockOrSteal|FATAL host fault: signal 5' "$w/contend.log" 2>/dev/null; then
    echo "---- reproduced the issue-1 abort: forced trap in the invalidation lock ----"
  fi
fi

exit "$fail"
