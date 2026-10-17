#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Pi side: generate, build and run the A64Frontend tests natively.
#   golden.sh OUTDIR
# Writes OUTDIR/<test> binaries, OUTDIR/<test>.golden (stdout) and
# OUTDIR/<test>.rc (exit status). Run on an AArch64 Linux machine with gcc.
#
# Optional corpus inputs (skipped when unset):
#   MUSL_ROOT  a directory holding usr/include/aarch64-linux-musl and
#              usr/lib/aarch64-linux-musl, e.g. Debian's musl and musl-dev
#              packages unpacked with `dpkg-deb -x`; builds the musl variants.
#   BUSYBOX    a static AArch64 busybox; adds the applet tests.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: golden.sh OUTDIR}
mkdir -p "$out"

python3 "$here/gen.py" "$out"
python3 "$here/gen_simd.py" "$out"
cp "$here"/*.S "$out"/

cd "$out"
# A large file-backed RW segment, for the destructive-madvise regression
# test; madvfile.S incbins it, so it must exist before the build loop.
python3 "$here/gen_blob.py" "$out/madvfile_blob.bin" $((24 * 1024 * 1024))
# A test may carry extra link flags on a `// LDFLAGS:` line.
for src in *.S; do
  # madvfile.S defines the blob's symbols and is linked with madvfile.c
  # below; building it alone here would produce a stray static binary.
  [ "$src" = common.S ] && continue
  [ "$src" = madvfile.S ] && continue
  gcc -march=armv8.2-a+fp16+crypto+crc+dotprod -nostdlib -static $(sed -n 's|^// LDFLAGS: ||p' "$src") -o "${src%.S}" "$src"
done
gcc -O2 -o madvfile "$here/madvfile.c" "$here/madvfile.S"

# Static libc programs.
corpus="hello printf_float strmem fpmath lse lseminmax lsecasp litmus nosve cntvct vdso vdso_syscalls hlt thunk_callback hoststack procdirfd dcmaint forkexec hostfault exclusive_pair icflush icpartial icfresh"
for t in $corpus; do
  gcc -static -O2 -o "$t" "$here/$t.c" -lm
done
if [ -n "${MUSL_ROOT:-}" ]; then
  sed "s#/usr/include/aarch64-linux-musl#$MUSL_ROOT/usr/include/aarch64-linux-musl#g; s#/usr/lib/aarch64-linux-musl#$MUSL_ROOT/usr/lib/aarch64-linux-musl#g" \
    "$MUSL_ROOT/usr/lib/aarch64-linux-musl/musl-gcc.specs" > musl.specs
  for t in $corpus; do
    gcc -specs="$out/musl.specs" -static -O2 -o "musl_$t" "$here/$t.c" -lm
  done
  corpus="$corpus musl_hello musl_printf_float musl_strmem musl_fpmath"
fi
# glibc only (ucontext): guest call/return shapes for the link-stack pairing,
# signal handlers that move the guest PC, and handlers that edit the
# interrupted registers.
gcc -static -O2 -o callret "$here/callret.c"
gcc -static -O2 -o sigpreempt "$here/sigpreempt.c"
gcc -static -O2 -o sigedit "$here/sigedit.c"
# A signal taken with the live floating-point state in D16-D31, whose handler
# reads and edits the frame's fpsimd_context. Those guest registers are pinned
# in the host's FPR-aliased low bank, so this is the frame test for the VSX
# register-class work (VSX-REGISTER-CLASSES.md).
gcc -static -O2 -o sigfploop "$here/sigfploop.c"

# ld1r is self-checking, so no golden is captured for it -- it is built here
# only so the Pi runs the same binary the POWER host does.
gcc -static -O2 -o ld1r "$here/ld1r.c"

# vins is self-checking for the same reason: the architectural result of an
# INS is the destination with one element replaced, which the test computes
# itself. Built here only so the same binary runs on both sides.
gcc -static -O2 -o vins "$here/vins.c"

# memredirect is self-checking and marked NO-GOLDEN, so no golden is captured:
# it computes every expected value out of its own byte loops (memmove's
# reference IS a byte loop through a temporary), and its whole claim is that the
# redirect is invisible, which run.sh tests by requiring byte-identical output
# with POWERARM_MEMREDIRECT off and on at several thresholds.
#
# STATIC on purpose, and load-bearing: in a static glibc memcpy/memmove/memset
# are IFUNC symbols, so this is the binary that exercises the resolver refusing
# an IFUNC's address and hooking the concrete __memcpy_*/__memset_*/__memmove_*
# implementations instead. A dynamically linked build would test the easy path.
gcc -static -O2 -o memredirect "$here/memredirect.c"

# vneg is self-checking: NEG is 0 - lane in the lane's own width, which the
# architecture forces for every value including the minimum, so the file
# computes its own answers. No golden, and it must stay that way -- it is the
# gate on the ISA 3.0 vnegw/vnegd arms agreeing with the pre-3.0 pair.
gcc -static -O2 -o vneg "$here/vneg.c"

# vlanetest likewise: the lane mask of a compare, its horizontal reduction and
# the branch that reads it are all architecturally forced, and the file
# computes all three. It is the gate on the record-form compare fusion
# (POWER-STRENGTHS.md 2.1), including the register state that fusion must
# leave on both edges of the branch.
#
# It covers all three reductions -- UMAXV, UMINV and ADDV -- and ADDV's answer
# is the lane sum taken modulo the element, which the file computes from the
# lane count rather than assuming it is a lane of ones. It also hand-assembles
# the two arrangements ADDV does not allocate (2S and .2D) with .inst, so it
# installs a SIGILL handler; that is self-contained and leaves no golden.
gcc -static -O2 -o vlanetest "$here/vlanetest.c"
# vcmpscan likewise: the lane mask of a CMEQ, the pairwise reduction of that
# mask by itself, and the GPR the FMOV copies it into are all forced by the
# architecture, and the file computes all three from the input bytes. It is
# the gate on the vector-scan fusion (TryFuseVectorScan), and specifically on
# the register state that fusion must leave on BOTH edges of the branch it
# replaces the idiom with.
gcc -static -O2 -o vcmpscan "$here/vcmpscan.c"

# fcmp_denorm is self-checking and MUST stay that way: it asserts the
# architecturally forced result of a float compare over denormal, NaN,
# signed-zero and infinity inputs, which is precisely what a reference run
# taken from the emulator under test could not tell you.
gcc -static -O2 -o fcmp_denorm "$here/fcmp_denorm.c"

# vcvtf16 is self-checking and MUST stay that way, for the same reason
# fcmp_denorm is, and with a sharper edge: it asserts the forced result of
# FCVTL/FCVTL2/FCVTN/FCVTN2 between FP16 and FP32 over denormals, NaN payloads,
# signed zeros, infinities, every rounding mode (with FPCR.FZ16 both ways,
# which these conversions must ignore) and the round-to-nearest-even and
# overflow boundaries, computing each answer from the input bits itself. It is
# the gate on the ISA 2.07 arm of those four conversions, which is a vector
# sequence rather than hardware -- and a reference taken from the emulator is
# precisely what could not tell you those answers: the software helper that arm
# used to call was wrong on every f32 strictly between 2^-25 and 2^-24, which
# this test catches and no golden did.
gcc -march=armv8.2-a+fp16 -static -O2 -o vcvtf16 "$here/vcvtf16.c"

# sigvcache likewise: self-checking, no golden. It installs its own SIGSEGV
# handler on a PROT_NONE page, so it needs no privileges and no timer.
gcc -static -O2 -o sigvcache "$here/sigvcache.c"

# sigchildteardown is self-checking and MUST stay that way. It asserts that
# every child it reaped was announced to the process by a SIGCHLD, which it
# computes from its own round count -- an emulator that drops one of those
# signals in a thread-teardown window is exactly what a golden taken from the
# emulator under test could not tell you. The marker in the file is what
# enforces it; this line only builds the binary so the same one can run on
# both sides.
gcc -static -O2 -pthread -o sigchildteardown "$here/sigchildteardown.c"

# memrev and prfmhint are self-checking and MUST stay that way, like
# fcmp_denorm. memrev asserts the byte order of a load-and-reverse and a
# reverse-and-store, which is forced by the architecture and which the test
# assembles byte by byte itself; prfmhint asserts that a prefetch raises no
# signal and moves no byte, which is what a hint is. Neither is in $corpus,
# so no golden is captured for either: a reference taken from the emulator
# under test is precisely what could not tell you these answers. Built here
# only so the same binary runs on both sides.
# fminmaxnm is self-checking and MUST stay that way: it computes A64's FPMaxNum /
# FPMinNum from the pseudocode and compares, so a golden taken from the emulator
# would be comparing the thing under test against itself on exactly the NaN rows
# that matter (POWER-STRENGTHS.md 2.7).
gcc -static -O2 -o fminmaxnm "$here/fminmaxnm.c"
gcc -static -O2 -o memrev "$here/memrev.c"
gcc -static -O2 -o prfmhint "$here/prfmhint.c"
# Threaded: a guest signal handler returning through rt_sigreturn while its
# thread is torn down.
gcc -static -O2 -pthread -o sigteardown "$here/sigteardown.c"
# A fault-shaped siginfo the program queued at itself. Natively it prints two
# lines and dies of SIGILL (rc 132); an emulator that mistakes si_code > 0 for
# "the CPU raised this here" swallows it and keeps running. See sigqueued.c.
gcc -static -O2 -o sigqueued "$here/sigqueued.c"
# Threads leaving through a raw SYS_exit while main leaves through
# exit_group. One run proves nothing on its own (it is a race); run.sh loops
# it. Natively it is silent and exits 0, and so is the golden.
gcc -static -O2 -pthread -o threadexit "$here/threadexit.c"
# A dead thread must not cost the process anything permanent: create and join
# threads in a loop and watch the process's own mapped footprint, then fork.
# Natively there are no emulator mappings to grow, so the golden is the two
# "ok" lines and rc 0; under POWERarm a leaked per-thread LookupCache makes the
# first line differ. See threadleak.c.
gcc -static -O2 -pthread -o threadleak "$here/threadleak.c"
# LDXR/STXR retry loops under contention, the shape TryFuseExclusiveLoop lowers
# onto lwarx/stwcx. Every assertion is architecturally forced (N threads x M
# increments leave exactly N*M), so the interleaving differing wildly between a
# 4-core Pi and a 176-thread POWER9 cannot move the output. Nothing
# CONSTRAINED UNPREDICTABLE is asserted -- that stays in exclusive_pair.
gcc -static -O2 -pthread -o exclusive_contend "$here/exclusive_contend.c"
corpus="$corpus callret sigpreempt sigedit sigteardown sigqueued threadexit threadleak exclusive_contend"
# POWERarm configuration for run.sh (<test>.env): vdso_syscalls counts
# syscalls with a seccomp filter, and seccomp emulation is opt-in there.
echo POWERARM_NEEDSSECCOMP=1 > vdso_syscalls.env
# thunk_callback falls back to direct calls where no thunk host exists (the
# Pi); under POWERarm the host path is required.
echo THUNK_CALLBACK_REQUIRE_HOST=1 > thunk_callback.env
# The rootfs overlay test: natively it runs inside a scratch copy of its own
# fixture (OVT_ROOT); run.sh runs it under POWERarm with that fixture as the
# base rootfs.
gcc -static -O2 -o rootfs_overlay "$here/rootfs_overlay.c"
# readlink(2)/readlinkat(2) errno and target fidelity over a fixture tree.
# Natively it runs against its own fixture under OUTDIR (RLFIX_ROOT); run.sh
# runs it twice under POWERarm, once with the fixture as the rootfs so
# FileManager::Readlink{,at}'s openat2(RESOLVE_IN_ROOT) trampoline answers it
# and once against a host path, and requires the two to agree. Every answer is
# architecturally forced, so the golden is also all-PASS.
gcc -static -O2 -o readlinkerr "$here/readlinkerr.c"

run_golden() {
  set +e
  "$@" > "$t.golden" 2>/dev/null
  echo $? > "$t.rc"
  set -e
}

for src in *.S; do
  [ "$src" = common.S ] && continue
  t=${src%.S}
  # A test that cannot run on this kernel states its golden instead: its
  # `// EXPECT:` lines are the stdout and `// EXPECT-RC:` the exit status.
  if grep -q '^// EXPECT-RC: ' "$src"; then
    sed -n 's|^// EXPECT: ||p' "$src" > "$t.golden"
    sed -n 's|^// EXPECT-RC: ||p' "$src" > "$t.rc"
    continue
  fi
  # `// NO-GOLDEN`: built but not run, because the test is self-checking and a
  # golden would add nothing. The C corpus has had such tests since ld1r and
  # sigvcache (they are simply left out of $corpus); this is the same thing for
  # an assembly one, and it is a marker rather than a list here so that the
  # rule travels with the test.
  #
  # It also stops this script from MANUFACTURING a golden for such a test on a
  # host where the only available aarch64 execution is POWERarm itself. A
  # golden taken from the thing under test compares equal to a failing run, and
  # exactly that has shipped in this repo before -- see the exitdead_golden
  # check in run.sh, which exists because of it.
  if grep -q '^// NO-GOLDEN' "$src"; then
    continue
  fi
  run_golden "./$t"
done
for t in $corpus; do
  # `// NO-GOLDEN` in a C test means the same thing it means in an assembly
  # one, and it is honoured here rather than only by omission from $corpus:
  # that list is hand-maintained, and the cost of forgetting a name on it is
  # a golden MANUFACTURED from the emulator under test, which compares equal
  # to a failing run. The marker travels with the test, so it cannot be
  # forgotten.
  if [ -f "$here/$t.c" ] && grep -q '^// NO-GOLDEN' "$here/$t.c"; then
    continue
  fi
  run_golden "./$t"
done
t=rootfs_overlay
rm -rf "$out/ovt-native"
./rootfs_overlay --make-fixture "$out/ovt-native"
run_golden env OVT_ROOT="$out/ovt-native" ./rootfs_overlay
rm -rf "$out/ovt-native"
t=readlinkerr
rm -rf "$out/rlfix-native"
./readlinkerr --make-fixture "$out/rlfix-native"
run_golden env RLFIX_ROOT="$out/rlfix-native" ./readlinkerr
rm -rf "$out/rlfix-native"

# Busybox applets on a fixed input. <test>.bin names the binary and
# <test>.args its arguments for run.sh.
if [ -n "${BUSYBOX:-}" ]; then
  cp "$BUSYBOX" busybox
  python3 "$here/gen_applet_input.py" applet-input.txt
  while read -r t args; do
    echo busybox > "$t.bin"
    echo "$args" > "$t.args"
    run_golden ./busybox $args
  done <<APPLETS
bb_echo echo hello world
bb_cat cat applet-input.txt
bb_wc wc applet-input.txt
bb_sort sort applet-input.txt
bb_sort_n sort -n -k2 applet-input.txt
bb_sha256sum sha256sum applet-input.txt
bb_md5sum md5sum applet-input.txt
APPLETS
fi
echo "golden outputs written to $out"
