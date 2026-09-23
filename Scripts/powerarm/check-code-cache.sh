#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# check-code-cache.sh BUILD_DIR [ROOTFS]
#
# Stress test for the runtime code cache (POWERARM_ENABLECODECACHINGWIP with
# POWERARM_CODECACHESCOPE). Every check compares guest output with a cache-off
# run, and reads the cache's own counters (POWERARM_CODECACHESTATS=1) to prove
# the path under test was really taken:
#
#   parallel   16 parallel `gcc -O2 -c` sharing one cold cache directory, then
#              again warm; objects must match the cache-off objects, blocks must
#              load, no entry may fail its hash, no temp file may be left.
#   replace    a guest binary rewritten in place (same path and inode, same
#              size, one instruction changed) after its cache was written.
#   forged     the replaced binary's cache files renamed to its new identity, so
#              the lookup finds blocks of the old code: every one must be
#              rejected on its guest-byte hash.
#   corrupt    one byte flipped in a cache segment's code: that entry must fail
#              its integrity hash and the program still run correctly.
#   isa30      the parallel compiles again with POWERARM_HOSTFEATURES=disableisa30
#              on the same cache directory: a separate cache (a second config id
#              in the file names), same objects.
#   smc        a guest that patches its own code after the block was cached, and
#              one that patches it before the cached block is first reached.
#   smalllib   a program built from small pieces compiles next to nothing warm:
#              libgcc_s.so.1 (installed 0644) reached through a C++ throw, a
#              library it dlopens and dlcloses, and DT_NEEDED libraries whose
#              only code run is their constructors and destructors. It closes
#              its stderr before exiting, and the counters must still arrive.
#   evict      over the size cap, a namespace of another emulator build is
#              evicted before any of this build's, however recently it was
#              written.
#   lld        programs linked by lld (the Claude CLI's and Chromium's linker)
#              compile next to nothing warm: one with lld's default 64K
#              layout, whose text is 64K-congruent at a file offset that is not
#              64K-aligned, and one linked -z max-page-size=4096, whose segments
#              are only 4K-congruent. Built with the host's clang and ld.lld
#              against the rootfs; skipped when they are missing.
#   homelink   the default scope, home, caches a program reached through a
#              symlink directly in $HOME whose target is outside it (the
#              owner's ~/Development), and not one behind a symlink into /tmp.
#              The target is a temporary directory in BUILD_DIR, removed at
#              exit; skipped when BUILD_DIR is on a tmpfs.
#
# Uses a private HOME, TMPDIR, cache directory and server socket, so it neither
# sees nor disturbs any other POWERarmServer or cache. Every check but homelink
# uses CodeCacheScope=all, since the private HOME and sources are in TMPDIR.
#
# Exit 0: OK. 1: a check failed. 2: could not run.
set -u

build=${1:?usage: check-code-cache.sh BUILD_DIR [ROOTFS]}
emu=$build/Bin/POWERarm
[ -x "$emu" ] || { echo "check-code-cache: no POWERarm in $build/Bin" >&2; exit 2; }
rootfs=${2:-${XDG_DATA_HOME:-$HOME/.local/share}/powerarm/RootFS/ArchLinuxARM-m2}
[ -x "$rootfs/usr/bin/gcc" ] || { echo "check-code-cache: no gcc in rootfs $rootfs" >&2; exit 2; }

w=$(mktemp -d "${TMPDIR:-/tmp}/check-code-cache.XXXXXX")
apps=
trap 'rm -rf "$w" ${apps:+"$apps"}' EXIT
mkdir -p "$w/home" "$w/run" "$w/src" "$w/bin"
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

# run CACHEDIR [ENV=V...] -- ARGS : one POWERarm process tree, cache on when
# CACHEDIR is not "-". The caller's ENV come last, so they win.
run() {
  local cache=$1
  shift
  local envs=()
  if [ "$cache" != - ]; then
    envs+=(POWERARM_ENABLECODECACHINGWIP=1 POWERARM_CODECACHESCOPE=all POWERARM_CODECACHESTATS=1 POWERARM_APP_CACHE_LOCATION="$cache/")
  fi
  while [ $# -gt 0 ] && [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  # POWERARM_PORTABLE keeps the processes gcc execs (cc1, as, collect2) on this
  # build: without it they go through binfmt to whatever build is registered
  # there, and the parallel, isa30 and corrupt checks tested that one instead.
  env -i PATH=/usr/bin:/bin HOME="$w/home" TMPDIR="$w/run" LC_ALL=C POWERARM_SERVERSOCKETPATH="$w/run/server.sock" \
    POWERARM_PORTABLE=1 POWERARM_ROOTFS="$rootfs" "${envs[@]}" "$emu" "$@"
}

# counter FIELD FILE... : sum of one counter over every process in the stats lines of the given logs.
counter() {
  local field=$1
  shift
  cat "$@" | awk -v f="$field" '/POWERarm code cache/ { for (i = 1; i < NF; i++) if ($i == f) s += $(i + 1) } END { print s + 0 }'
}

# ---------------------------------------------------------------------------
# Sources: 16 translation units big enough to take a real compile, and the
# guest programs for the replace/smc checks.
for i in $(seq 1 16); do
  {
    echo "#include <stdio.h>"
    echo "#include <string.h>"
    for f in $(seq 1 40); do
      echo "static int t${i}_${f}(const char *s, int n) { int h = $i; for (int k = 0; k < n && s[k]; k++) h = h * 31 + s[k] * $f; return h ^ (int)strlen(s); }"
    done
    echo "int unit$i(void) { char b[64]; int h = 0; snprintf(b, sizeof b, \"%d\", $i);"
    for f in $(seq 1 40); do echo "  h += t${i}_${f}(b, $f);"; done
    echo "  return h; }"
  } > "$w/src/u$i.c"
done

cat > "$w/src/prog.c" << 'EOF'
#include <stdio.h>
__attribute__((noipa)) int value(void) { return VALUE; }
int main(void) { printf("value %d\n", value()); return 0; }
EOF

cat > "$w/src/smc.c" << 'EOF'
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
__attribute__((noipa)) int value(void) { return 1; }
static void patch(void) {
  long page = sysconf(_SC_PAGESIZE);
  uintptr_t start = (uintptr_t)value & ~(uintptr_t)(page - 1);
  if (mprotect((void *)start, 2 * page, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) { perror("mprotect"); _exit(3); }
  uint32_t insn = 0x52800040; /* mov w0, #2 */
  memcpy((void *)value, &insn, sizeof insn);
  __builtin___clear_cache((char *)value, (char *)value + 4);
  mprotect((void *)start, 2 * page, PROT_READ | PROT_EXEC);
}
int main(int argc, char **argv) {
  if (argc > 1) { patch(); printf("early %d\n", value()); return 0; }
  printf("before %d\n", value());
  patch();
  printf("after %d\n", value());
  return 0;
}
EOF

cd "$w/src" || exit 2
run - -- /usr/bin/gcc -O2 -o prog1 -DVALUE=1 prog.c && run - -- /usr/bin/gcc -O2 -o prog2 -DVALUE=2 prog.c &&
  run - -- /usr/bin/gcc -O2 -o smc smc.c || { echo "check-code-cache: building the guest programs failed" >&2; exit 2; }
cmp -s prog1 prog2 && { echo "check-code-cache: prog1 and prog2 are identical" >&2; exit 2; }
[ "$(stat -c %s prog1)" = "$(stat -c %s prog2)" ] || { echo "check-code-cache: prog1 and prog2 differ in size" >&2; exit 2; }

# ---------------------------------------------------------------------------
# parallel
mkdir -p "$w/ref" "$w/cold" "$w/warm" "$w/isa"
for i in $(seq 1 16); do run - -- /usr/bin/gcc -O2 -c "u$i.c" -o "$w/ref/u$i.o" & done
wait
compile_all() { # OUTDIR CACHEDIR [ENV...]
  local out=$1 cache=$2
  shift 2
  for i in $(seq 1 16); do run "$cache" "$@" -- /usr/bin/gcc -O2 -c "u$i.c" -o "$out/u$i.o" 2> "$out/u$i.log" & done
  wait
  for i in $(seq 1 16); do cmp -s "$w/ref/u$i.o" "$out/u$i.o" || { bad "$(basename "$out"): u$i.o differs from the cache-off object"; return; }; done
  ok "$(basename "$out"): 16 parallel objects match the cache-off objects"
}
compile_all "$w/cold" "$w/cache"
compile_all "$w/warm" "$w/cache"
loaded=$(counter loaded "$w"/warm/*.log)
badentry=$(counter bad-entry "$w"/cold/*.log "$w"/warm/*.log)
[ "$loaded" -gt 100000 ] && ok "warm: $loaded blocks loaded" || bad "warm: only $loaded blocks loaded"
[ "$badentry" = 0 ] && ok "no entry failed its hash" || bad "$badentry entries failed their hash"
ls "$w/cache/cache" | grep -q '\.tmp\.' && bad "temp files left in the cache" || ok "no temp files left"
too_many=$(ls "$w/cache/cache" | grep -v '\.lock$' | sed 's/\.[0-9]*$//' | sort | uniq -c | awk '$1 > 8' | wc -l)
[ "$too_many" = 0 ] && ok "at most 8 segments per file" || bad "$too_many files have more than 8 segments"

# ---------------------------------------------------------------------------
# isa30
# One compile first, with the cache directory read-only so nothing it compiles is
# written back: the default-config cache is warm, and none of it may load.
chmod a-w "$w/cache/cache"
run "$w/cache" POWERARM_HOSTFEATURES=disableisa30 -- /usr/bin/gcc -O2 -c u1.c -o "$w/isa/first.o" 2> "$w/isa/first.log"
chmod u+w "$w/cache/cache"
cmp -s "$w/ref/u1.o" "$w/isa/first.o" && [ "$(counter loaded "$w/isa/first.log")" = 0 ] && [ "$(counter no-file "$w/isa/first.log")" -gt 0 ] &&
  ok "isa30: nothing loaded across the ISA 3.0 switch" || { bad "isa30: first disableisa30 process loaded blocks or produced a different object"; cat "$w/isa/first.log"; }
compile_all "$w/isa" "$w/cache" POWERARM_HOSTFEATURES=disableisa30
ids=$(ls "$w/cache/cache" | grep '^cc1-' | grep -v '\.lock$' | sed 's/\.[0-9]*$//' | sort -u | wc -l)
[ "$ids" = 2 ] && ok "isa30: disableisa30 wrote a separate cc1 cache ($ids config ids)" || bad "isa30: expected 2 cc1 cache names, found $ids"

# ---------------------------------------------------------------------------
# replace, forged, corrupt
cp prog1 "$w/bin/prog"
for pass in 1 2; do out=$(run "$w/pc" -- "$w/bin/prog" 2> "$w/prog$pass.log"); done
[ "$out" = "value 1" ] && [ "$(counter loaded "$w/prog2.log")" -gt 0 ] && ok "replace: prog1 cached and loaded" || bad "replace: prog1 run: '$out'"
ino=$(stat -c %i "$w/bin/prog")
old=$(ls "$w/pc/cache" | grep '^prog-' | grep -v '\.lock$' | head -1 | sed 's/^prog-\([0-9a-f]*\)-.*/\1/')
cat prog2 > "$w/bin/prog"
[ "$(stat -c %i "$w/bin/prog")" = "$ino" ] || { echo "check-code-cache: in-place rewrite changed the inode" >&2; exit 2; }
out=$(run "$w/pc" -- "$w/bin/prog" 2> "$w/prog3.log")
[ "$out" = "value 2" ] && ok "replace: rewritten binary runs its new code" || bad "replace: rewritten binary printed '$out'"

# Give the new binary's identity the old binary's cache files.
new=$(ls "$w/pc/cache" | grep '^prog-' | grep -v "$old" | grep -v '\.lock$' | head -1 | sed 's/^prog-\([0-9a-f]*\)-.*/\1/')
if [ -n "$old" ] && [ -n "$new" ] && [ "$old" != "$new" ]; then
  for f in "$w/pc/cache"/prog-"$new"-*; do rm -f "$f"; done
  for f in "$w/pc/cache"/prog-"$old"-*; do cp "$f" "${f/prog-$old-/prog-$new-}"; done
  # The segments carry the id they were written for: rewrite it in the header
  # too, as a same-identity collision would have it.
  python3 - "$w/pc/cache" "$old" "$new" << 'EOF'
import ctypes, ctypes.util, os, struct, sys
d, old, new = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
lib = ctypes.util.find_library("xxhash")
if not lib:
    sys.exit(3)
xxh = ctypes.CDLL(lib)
xxh.XXH3_64bits.restype = ctypes.c_uint64
xxh.XXH3_64bits.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
for n in os.listdir(d):
    if not n.startswith("prog-%016x-" % new) or n.endswith(".lock"):
        continue
    p = os.path.join(d, n)
    b = bytearray(open(p, "rb").read())
    struct.pack_into("<Q", b, 16, new)
    struct.pack_into("<Q", b, 104, xxh.XXH3_64bits(bytes(b[:104]), 104))
    open(p, "wb").write(b)
EOF
  if [ $? = 0 ]; then
    out=$(run "$w/pc" -- "$w/bin/prog" 2> "$w/prog4.log")
    mism=$(counter guest-mismatch "$w/prog4.log")
    [ "$out" = "value 2" ] && [ "$mism" -gt 0 ] && ok "forged: $mism old blocks rejected on their guest bytes" ||
      { bad "forged: printed '$out', guest-mismatch $mism (old $old new $new)"; cat "$w/prog4.log"; ls "$w/pc/cache"; }
  else
    echo "skip forged: libxxhash not found"
  fi
else
  bad "replace: expected two prog cache identities (old '$old', new '$new')"
fi

# Flip one code byte of every block in every cc1 segment.
python3 - "$w"/cache/cache/cc1-* << 'EOF'
import struct, sys
for p in sys.argv[1:]:
    if p.endswith(".lock"):
        continue
    b = bytearray(open(p, "rb").read())
    nblocks, = struct.unpack_from("<I", b, 44)
    index, = struct.unpack_from("<Q", b, 72)
    code, = struct.unpack_from("<Q", b, 88)
    for i in range(nblocks):
        code_offset, = struct.unpack_from("<Q", b, index + i * 56 + 16)
        b[code + code_offset + 64] ^= 0xff
    open(p, "wb").write(b)
EOF
mkdir -p "$w/corrupt"
# Entry hashes are only checked for another boot's files unless forced.
for i in $(seq 1 16); do run "$w/cache" POWERARM_CODECACHEVERIFY=1 -- /usr/bin/gcc -O2 -c "u$i.c" -o "$w/corrupt/u$i.o" 2> "$w/corrupt/u$i.log"; done
same=1
for i in $(seq 1 16); do cmp -s "$w/ref/u$i.o" "$w/corrupt/u$i.o" || same=0; done
be=$(counter bad-entry "$w"/corrupt/*.log)
[ $same = 1 ] && [ "$be" -gt 0 ] && ok "corrupt: flipped byte caught ($be bad entries), objects match" || bad "corrupt: objects match=$same, bad-entry $be"

# ---------------------------------------------------------------------------
# smc
out=$(run - -- ./smc | tr '\n' ' ')$(run - -- ./smc early | tr '\n' ' ')
[ "$out" = "before 1 after 2 early 2 " ] || { echo "check-code-cache: smc misbehaves with the cache off: '$out'" >&2; exit 2; }
for pass in 1 2; do
  out=$(run "$w/sc" -- ./smc 2> "$w/smc$pass.log" | tr '\n' ' ')
  [ "$out" = "before 1 after 2 " ] && ok "smc pass $pass: patch after first call" || bad "smc pass $pass: printed '$out'"
  out=$(run "$w/sc" -- ./smc early 2> "$w/smce$pass.log" | tr '\n' ' ')
  [ "$out" = "early 2 " ] && ok "smc pass $pass: patch before first call" || bad "smc pass $pass (early): printed '$out'"
done
[ "$(counter loaded "$w/smc2.log")" -gt 0 ] && ok "smc: warm run loaded blocks" || bad "smc: warm run loaded nothing"

# ---------------------------------------------------------------------------
# smalllib
cat > "$w/src/smalllib.cc" << 'EOF'
#include <cstdio>
#include <dlfcn.h>
#include <stdexcept>
__attribute__((noipa)) static int thrower(int x) {
  if (x > 0) throw std::runtime_error("x");
  return x;
}
int main(int argc, char **) {
  int r = 0;
  for (int i = 0; i < 3; ++i) {
    try { r += thrower(argc); } catch (const std::exception &) { r += 1; }
  }
  if (void *h = dlopen("libbz2.so.1", RTLD_NOW)) {
    r += dlsym(h, "BZ2_bzlibVersion") != nullptr;
    dlclose(h);
  }
  printf("r=%d\n", r);
  fflush(stdout);
  fclose(stderr);
  return 0;
}
EOF
run - -- /usr/bin/g++ -O1 -o smalllib smalllib.cc -Wl,--no-as-needed -lz -ldl ||
  { echo "check-code-cache: building smalllib failed" >&2; exit 2; }
for pass in 1 2; do
  out=$(run "$w/sl" POWERARM_CODEHASHLOG="$w/smalllib$pass.hash" -- ./smalllib 2> "$w/smalllib$pass.log")
  [ "$out" = "r=4" ] || bad "smalllib pass $pass: printed '$out'"
done
# The hash log is created by the first compile: none means none compiled.
cold=$(cat "$w/smalllib1.hash" 2> /dev/null | wc -l)
warm=$(cat "$w/smalllib2.hash" 2> /dev/null | wc -l)
[ "$warm" -lt 10 ] && ok "smalllib: warm run compiled $warm blocks (cold $cold)" || bad "smalllib: warm run compiled $warm blocks (cold $cold)"
[ "$(counter loaded "$w/smalllib2.log")" -gt 0 ] && ok "smalllib: counters reached the log after the guest closed stderr" ||
  bad "smalllib: no counters in the log after the guest closed stderr"

# ---------------------------------------------------------------------------
# lockbusy, evict
#
# Both need a library the guest dlopens and dlcloses: the dlclose is the save
# pass (SaveCodeCachesBeforeUnmap) that publishes a segment and sweeps the
# directory, without waiting for a process to run 60 s or compile 50k blocks.
cat > "$w/src/libdrop.c" << 'EOF'
#define F(n) \
  __attribute__((noinline)) int drop##n(int x) { \
    for (int i = 0; i < (x & 3) + n + 2; i++) x = x * 31 + i * n; \
    return x; \
  }
F(1) F(2) F(3) F(4) F(5) F(6) F(7) F(8) F(9) F(10) F(11) F(12)
int dropall(int x) {
  int (*fns[])(int) = {drop1, drop2, drop3, drop4, drop5, drop6, drop7, drop8, drop9, drop10, drop11, drop12};
  for (unsigned i = 0; i < sizeof fns / sizeof *fns; i++) x = fns[i](x) ^ (int)i;
  return x;
}
EOF
cat > "$w/src/dropprog.c" << 'EOF'
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  void *h = dlopen(argv[1], RTLD_NOW);
  if (!h) { printf("dlopen failed: %s\n", dlerror()); return 2; }
  int (*f)(int) = (int (*)(int))dlsym(h, "dropall");
  int r = f ? f(argc) : 0;
  /* The dlclose unmaps the library: its blocks are saved here or never. */
  dlclose(h);
  printf("drop %d\n", r);
  return 0;
}
EOF
run - -- /usr/bin/gcc -O1 -shared -fPIC -o libdrop.so libdrop.c &&
  run - -- /usr/bin/gcc -O1 -o dropprog dropprog.c -ldl ||
  { echo "check-code-cache: building the dlopen programs failed" >&2; exit 2; }
cp libdrop.so libdrop2.so
dropref=$(run - -- ./dropprog ./libdrop.so)

# wait_for SECONDS GLOB / wait_gone SECONDS GLOB : the forked cache writer
# publishes and sweeps after the guest has moved on, so both are polled for.
wait_for() {
  local i=0
  while [ "$i" -lt "$(($1 * 4))" ]; do
    # shellcheck disable=SC2086
    ls $2 > /dev/null 2>&1 && return 0
    sleep 0.25
    i=$((i + 1))
  done
  return 1
}
wait_gone() {
  local i=0
  while [ "$i" -lt "$(($1 * 4))" ]; do
    # shellcheck disable=SC2086
    ls $2 > /dev/null 2>&1 || return 0
    sleep 0.25
    i=$((i + 1))
  done
  return 1
}

# ---------------------------------------------------------------------------
# evict
# Over the cap, namespaces of another emulator build go first whatever their
# mtime: nothing started after a promote can ever read them, while a pure LRU
# evicted apps that are still in use (COLD-ROUND2 1.1).
mkdir -p "$w/ev"
run "$w/ev" -- ./dropprog ./libdrop.so > /dev/null 2> "$w/ev1.log"
if [ ! -d "$w/ev/cache" ]; then
  bad "evict: the first run wrote no cache"
else
  # 20 MiB of another build's cache: a segment whose header this build rejects
  # (zeros are not the magic), written now, so it is the NEWEST namespace here
  # and the one a pure LRU would keep.
  fake="$w/ev/cache/evictfake-0123456789abcdef-fedcba9876543210"
  dd if=/dev/zero of="$fake" bs=1048576 count=20 status=none
  : > "$fake.lock"
  total_kb=$(du -sk "$w/ev/cache" | cut -f1)
  own_kb=$((total_kb - 20480))
  cap=$((total_kb / 1024 - 1))
  if [ "$own_kb" -le 0 ] || [ "$cap" -lt 1 ] || [ "$((own_kb * 10))" -gt "$((cap * 1024 * 9))" ]; then
    bad "evict: cannot size the cap (own ${own_kb} KiB, cap ${cap} MiB)"
  else
    # The sweep runs at most once a minute across all processes.
    touch -d '5 minutes ago' "$w/ev/cache/.sweep" 2> /dev/null
    out=$(run "$w/ev" POWERARM_CODECACHEMAXSIZE=$cap -- ./dropprog ./libdrop2.so 2> "$w/ev2.log")
    [ -n "$out" ] || bad "evict: the second run printed nothing"
    if wait_gone 30 "$fake"; then
      ls "$w/ev/cache"/libdrop.so-* > /dev/null 2>&1 &&
        ok "evict: another build's namespace went before this build's (cap ${cap} MiB)" ||
        bad "evict: this build's libdrop namespace was evicted too"
    else
      bad "evict: another build's namespace survived the cap (own ${own_kb} KiB, cap ${cap} MiB)"
      ls "$w/ev/cache"
    fi
  fi
fi

# ---------------------------------------------------------------------------
# lld
# On a 64K host both layouts used to load the text as anonymous memory, which
# the cache cannot name: every warm run compiled the program's own code again.
# 128K of padding on each side keeps that code in text pages it shares with no
# other segment, so the 4k layout exercises the fallback copy itself rather
# than a neighbour's file mapping. The count is of blocks in the program's own
# functions, whose range it prints to stderr: code in host pages shared with
# its writable data (the PLT, .fini) is invalidated by those writes and
# recompiled, a few blocks that vary from run to run.
if command -v clang > /dev/null && command -v ld.lld > /dev/null; then
  cat > "$w/src/lldprog.c" << 'EOF'
#include <stdint.h>
#include <stdio.h>
/* 96 KiB of read-only data ahead of .text: lld's default layout then puts the
   text at a file offset past the first 64K. */
static const uint32_t table[24576] = {1, 2, 3, 5, 8, 13};
#define F(n) \
  __attribute__((noinline)) static uint64_t f##n(uint64_t x) { \
    for (int i = 0; i < (int)(x & 7) + n; i++) x = x * 6364136223846793005ull + table[(x >> 7) % 24576] + n; \
    return x; \
  }
F(0) F(1) F(2) F(3) F(4) F(5) F(6) F(7) F(8) F(9) F(10) F(11) F(12) F(13) F(14) F(15)
static uint64_t (*const fns[])(uint64_t) = {f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15};
int main(int argc, char **argv) {
  uintptr_t lo = (uintptr_t)main, hi = (uintptr_t)main;
  for (int i = 0; i < 16; i++) {
    lo = (uintptr_t)fns[i] < lo ? (uintptr_t)fns[i] : lo;
    hi = (uintptr_t)fns[i] > hi ? (uintptr_t)fns[i] : hi;
  }
  fprintf(stderr, "lldhot %lx %lx\n", (unsigned long)lo, (unsigned long)hi + 0x200);
  uint64_t x = (uint64_t)argc;
  for (int r = 0; r < 64; r++) x = fns[(x >> 3) % 16](x) ^ (uint64_t)r;
  printf("lldprog %016llx\n", (unsigned long long)x);
  return 0;
}
EOF
  printf '\t.text\n\t.skip 0x20000\n' > "$w/src/lldpad.s"
  # lld_own PREFIX : blocks in the program's own functions (the range PREFIX.log
  # names) that the run compiled (PREFIX.hash); "none" when no range was printed.
  lld_own() {
    python3 - "$1.log" "$1.hash" << 'EOF'
import sys
lo = hi = None
for line in open(sys.argv[1]):
    if line.startswith("lldhot "):
        lo, hi = (int(f, 16) for f in line.split()[1:3])
if lo is None:
    print("none")
    sys.exit()
n = 0
try:
    for line in open(sys.argv[2]):
        if lo <= int(line.split()[0], 16) < hi:
            n += 1
except FileNotFoundError:
    pass
print(n)
EOF
  }
  for v in 64k 4k; do
    z=
    [ $v = 4k ] && z=-Wl,-z,max-page-size=4096
    if ! clang --target=aarch64-unknown-linux-gnu --sysroot="$rootfs" -fuse-ld=lld -O2 $z -o "lld$v" lldpad.s lldprog.c lldpad.s \
      2> "$w/lld$v.build.log"; then
      echo "skip lld $v: clang could not build it against $rootfs"
      head -5 "$w/lld$v.build.log" | sed 's/^/  /'
      continue
    fi
    # The layout under test, from the executable PT_LOAD: 64k = congruent modulo
    # 64K at an offset whose 4K rounding is not 64K-aligned; 4k = not congruent.
    layout=$(python3 - "lld$v" << 'EOF'
import struct, sys
b = open(sys.argv[1], "rb").read()
phoff, = struct.unpack_from("<Q", b, 32)
phentsize, phnum = struct.unpack_from("<HH", b, 54)
for i in range(phnum):
    kind, flags, off, vaddr = struct.unpack_from("<IIQQ", b, phoff + i * phentsize)
    if kind == 1 and flags & 1:
        if (vaddr - off) % 0x10000:
            print("4k")
        elif (off & ~0xfff) % 0x10000:
            print("64k")
        else:
            print("aligned")
EOF
    )
    [ "$layout" = $v ] || { bad "lld $v: the text segment's layout is '$layout', not the one under test"; continue; }
    ref=$(run - -- "./lld$v" 2> /dev/null)
    for pass in 1 2; do
      out=$(run "$w/ld" POWERARM_CODEHASHLOG="$w/lld$v.$pass.hash" -- "./lld$v" 2> "$w/lld$v.$pass.log")
      [ -n "$ref" ] && [ "$out" = "$ref" ] || bad "lld $v pass $pass: printed '$out', cache off '$ref'"
    done
    cold=$(lld_own "$w/lld$v.1")
    warm=$(lld_own "$w/lld$v.2")
    total=$(cat "$w/lld$v.2.hash" 2> /dev/null | wc -l)
    [ "$cold" != none ] && [ "$cold" -gt 0 ] && [ "$warm" = 0 ] &&
      ok "lld $v: warm run compiled none of the program's $cold blocks ($total blocks in all)" ||
      bad "lld $v: warm run compiled $warm of the program's $cold blocks ($total blocks in all)"
  done
else
  echo "skip lld: no clang or ld.lld on PATH"
fi

# ---------------------------------------------------------------------------
# homelink
# Cache names come from /proc/self/fd, i.e. the resolved path, so a program
# under ~/Development -> /mnt/arch/home/<user>/Development is not below $HOME
# by name. The home scope follows symlinks directly in $HOME, except into
# scratch space (/tmp, tmpfs).
apps=$(mktemp -d "$build/check-code-cache-home.XXXXXX")
case $(stat -f -c %T "$apps") in
tmpfs | ramfs) echo "skip homelink: $build is on a tmpfs, which the home scope never follows" ;;
*)
  mkdir -p "$w/scratch"
  cp "$w/src/prog1" "$apps/homeprog"
  cp "$w/src/prog1" "$w/scratch/scratchprog"
  ln -s "$apps" "$w/home/apps"
  ln -s "$w/scratch" "$w/home/scratch"
  for pass in 1 2; do
    a=$(run "$w/hl" POWERARM_CODECACHESCOPE=home -- "$w/home/apps/homeprog" 2> "$w/homeprog$pass.log")
    s=$(run "$w/hl" POWERARM_CODECACHESCOPE=home -- "$w/home/scratch/scratchprog" 2> "$w/scratchprog$pass.log")
    [ "$a" = "value 1" ] && [ "$s" = "value 1" ] || bad "homelink pass $pass: printed '$a' and '$s'"
  done
  ls "$w/hl/cache" 2> /dev/null | grep -q '^homeprog-' &&
    ok "homelink: a program behind a symlink in \$HOME is cached" || bad "homelink: no cache for the program behind a symlink in \$HOME"
  case $w:$(stat -f -c %T "$w") in
  /tmp/* | /var/tmp/* | *:tmpfs | *:ramfs)
    ls "$w/hl/cache" 2> /dev/null | grep -q '^scratchprog-' && bad "homelink: a program behind a symlink into scratch space was cached" ||
      ok "homelink: a program behind a symlink into scratch space is not"
    ;;
  *) echo "skip homelink scratch: TMPDIR $w is not scratch space" ;;
  esac
  ;;
esac

exit $fail
