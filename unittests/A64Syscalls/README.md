# A64Syscalls: AArch64 Linux syscall conformance corpus

These are small, static AArch64 Linux programs. Each one tests one area of the
syscall ABI and prints **normalized, deterministic** text. You capture the
output natively on real aarch64 hardware as a golden file. Later you run the
same static binary under POWERarm on a POWER9 host and diff the output
byte-for-byte.

> **Status:** these run under POWERarm now that the A64 frontend has landed.

## Programs

| Program | Covers |
|---|---|
| `sys_mem` | `brk` (grow, shrink, bogus-low); `mmap` anonymous, file private/shared, `MAP_FIXED` replace, `MAP_FIXED_NOREPLACE` (busy → EEXIST, hole → ok), odd lengths, invalid combos; `MAP_FIXED`/`NOREPLACE` at 2^47, 2^48, 2^52 and 2^56 → ENOMEM; hint above 2^47 must land below 2^47; `mprotect` RO/NONE/RW; `mremap` grow in place, shrink, blocked → ENOMEM, MAYMOVE, FIXED; `madvise` DONTNEED zeroing (anon) and revert (file private), advice values; `munmap` holes; `msync` |
| `sys_io` | `read`/`write`, `lseek` SET/CUR/END, holes, `pread64`/`pwrite64` (offset unchanged), `readv`/`writev`, `preadv`/`pwritev`, O_APPEND (including Linux's pwrite-appends quirk), O_TRUNC, `dup`/`dup3` (+O_CLOEXEC), close twice, lowest-fd reuse, `pipe2` (flags, EAGAIN, ESPIPE, EOF), `/dev/zero`, `/dev/null` |
| `sys_fs` | `openat` (O_CREAT\|O_EXCL → EEXIST, O_DIRECTORY on file → ENOTDIR, O_NOFOLLOW on symlink → ELOOP, O_PATH, dirfd-relative, O_TMPFILE if supported); `fcntl` F_GETFL printed with **arm64 kernel** flag values (incl. O_LARGEFILE), F_GETFD/F_SETFD, F_SETFL O_NONBLOCK, F_DUPFD(_CLOEXEC); ioctl on regular file → ENOTTY, FIONREAD; `newfstatat`/`fstat`/`statx` (type, perm, nlink, size, statx-vs-stat agreement); `getdents64` (sorted names, d_type, small buffer, EINVAL/EFAULT/ENOTDIR); `readlinkat` (truncation, dangling, loops); `faccessat`/`faccessat2` (AT_EACCESS, AT_SYMLINK_NOFOLLOW, AT_EMPTY_PATH); `getcwd`/`chdir`/`fchdir` (incl. removed cwd); `mkdirat`/`unlinkat`(AT_REMOVEDIR)/`renameat2` (NOREPLACE, EXCHANGE)/`symlinkat`/`linkat`; `utimensat` with exact ns timestamps, UTIME_OMIT, UTIME_NOW, negative times, NOFOLLOW on symlinks, `futimens` |
| `sys_tty` | pty via `posix_openpt`/`grantpt`/`unlockpt`/`ptsname`; raw `ioctl` with a locally defined arm64 kernel `struct termios`/`termios2`: TCGETS, TCSETS, TCSETSW, TCSETSF (flushes input), TCGETS2/TCSETS2 (BOTHER custom speed), plus libc `tcgetattr`/`cfmakeraw`/`cfgetospeed`; every c_iflag/c_oflag/c_cflag/c_lflag bit and all c_cc by name; ONLCR/ICRNL data path; canonical line reads; FIONREAD, TCFLSH, TIOCOUTQ; TIOCSWINSZ on master → TIOCGWINSZ on slave; TIOCGPGRP/TIOCGSID → ENOTTY when not the ctty; in a `setsid()` child: TIOCSCTTY, TIOCGPGRP == getpgrp, TIOCSPGRP; error paths (ENOTTY, EFAULT, EBADF, EIO after slave close) |
| `sys_proc` | getpid/getppid/gettid, get[res][ug]id, getgroups, getpgid/getsid (all as relations); `umask` round trip; `prlimit64` NOFILE lowered, read back, enforced (EMFILE), restored, errors; `set_tid_address` returns tid; `set_robust_list`/`get_robust_list`; `rseq` (ok-or-enosys); `getrandom` (sizes, GRND_NONBLOCK/GRND_RANDOM, errors); `sysinfo` sanity; `uname` sysname and machine exactly (`Linux`, `aarch64`) |
| `sys_time` | `clock_gettime`/`clock_getres` for 8 clocks (raw syscalls, bypassing the vDSO), monotonic non-decreasing (raw and libc), realtime vs `gettimeofday` within 1 s, `nanosleep` 10 ms, `clock_nanosleep` relative/TIMER_ABSTIME/past, invalid clock → EINVAL, invalid tv_nsec → EINVAL, EFAULT, CPU-time clock advances |
| `sys_signal` | **No delivery.** `rt_sigaction` install/query/restore with arm64 kernel `struct sigaction`; sa_flags round trip for SA_RESTART/SA_SIGINFO/SA_ONSTACK/SA_NODEFER/SA_RESETHAND/SA_NOCLDSTOP/SA_NOCLDWAIT; SA_UNSUPPORTED is cleared, SA_EXPOSE_TAGBITS (an arm64 flag) is kept; SIGKILL/SIGSTOP are stripped from sa_mask; RT signals; errors; `rt_sigprocmask` BLOCK/UNBLOCK/SETMASK (KILL/STOP can't be blocked), errors; `rt_sigpending` empty; `sigaltstack` set/query/ENOMEM/EINVAL/disable |
| `sys_signal_delivery` | raise/kill/tgkill/sigqueue delivery and si_code, handler mask (sa_mask + self, SA_NODEFER), mask restore on sigreturn, SA_RESETHAND, SA_ONSTACK, pending-while-blocked, RT queueing, sigtimedwait, synchronous SIGSEGV + siglongjmp, EINTR vs SA_RESTART on a blocked read |
| `sys_siglongjmp` | handlers abandoned with siglongjmp/longjmp instead of rt_sigreturn, 100,000+ times per section: raise, longjmp with the signal left blocked, SA_NODEFER, guard-page SIGSEGV (every 16th handler returns instead), nested handlers left out of both or only the inner one, SA_NODEFER recursion 8 deep, SA_ONSTACK handlers on a sigaltstack, a jump from 200 calls deep, an ITIMER_REAL busy loop, and 4 threads at once |
| `sys_fork_exec` | `fork` (clone SIGCHLD), raw `clone(SIGCHLD)`, `clone` with exit signal 0 + `__WCLONE`, `vfork`; `wait4` status macros, WNOHANG, rusage, ECHILD/EINVAL; `pipe2` parent↔child; CoW vs MAP_SHARED; shared file offset; `exit` vs `exit_group` codes (incl. truncation to 8 bits); SIGKILL'd child (kernel-side only); `execve` of `/proc/self/exe` (fallback argv[0]) with a marker arg checking O_CLOEXEC, env, handler reset, SIG_IGN kept, mask kept, altstack reset; execve errors (ENOENT, EACCES, ENOEXEC, bad interpreter, directory, ENOTDIR, EFAULT) |
| `sys_process` | the process model a shell relies on: handlers returning through the default restorer (arm64 libcs never set SA_RESTORER), state preserved across a delivered signal, siginfo; SIGCHLD around fork/wait; `sigsuspend` with SIGCHLD pending and blocked runs the handler **before** returning (busybox ash `wait`); SA_RESTART vs EINTR on a pipe read interrupted by SIGCHLD; `execve` keeps the caller's argv[0] (via `/proc/self/exe` and by path) and gives an empty argv argc=1 with argv[0] ""; fork+pipe+dup2+execve pipeline; `posix_spawn` with a dup2 file action; vfork+execve; `readlink /proc/self/exe` names the program |
| `sys_errno` | about 90 calls with guaranteed errnos, all via `syscall()`: EBADF, ENOENT, ESPIPE, EEXIST, ENOTEMPTY, ENOTDIR, EISDIR, ELOOP, ENAMETOOLONG, EFAULT (e.g. `write(fd,(void*)1,10)`), ERANGE (`getcwd` size 1), EINVAL, ENOTTY, EAGAIN, EPIPE, ECHILD, ESRCH, EMFILE, ENOMEM, EACCES, ENOSYS |

`a64sys.h` holds the shared helpers: the errno name table, the flag
pretty-printer, and the temp directory enter/cleanup code.

## Output and normalization rules

- Every line is `name: key=value ...` in a fixed order. The last line is
  `done`, and `main` returns 0.
- Errno is printed as `NAME(number)` from a local table (never `strerror`).
- Return values are printed as numbers only when they are deterministic
  (byte counts, 0/-1, offsets in test files). Otherwise the program prints
  a relation (`yes`/`no`).
- These are **never printed**: addresses, the page size, pids/tids/uids/gids,
  inode/dev/rdev numbers (except "/dev/null is 1:3"), st_blocks, blksize
  (only "nonzero"), timestamps the test didn't set, hostname, kernel
  release/version, sysinfo values, absolute paths outside the test's temp
  dir, and the pts number.
- Directory `st_nlink`/`st_size` aren't printed, because they depend on
  the filesystem.
- Filesystem work happens in a fresh `mkdtemp("/tmp/a64sys.XXXXXX")`
  directory. The programs `chdir` into it, print only relative paths, and
  remove it at the end. `umask(022)` is set wherever modes are printed.
- Page-size independence: sizes are multiples of `sysconf(_SC_PAGESIZE)`.
  The Pi 5 golden runs with 16K pages, and the POWER9 host may use 4K or
  64K pages.
- The output doesn't depend on whether stdin/stdout are ttys, on the
  inherited signal mask or ignored signals, on umask, or on extra inherited
  fds. `build-and-golden.sh` checks file-vs-pipe stdout. This was also
  checked with umask 000/077, ignored USR1/USR2/HUP/PIPE/INT/TERM, blocked
  USR1/USR2/CHLD/WINCH/TERM, and `ulimit -n 200`.
- Checks whose result legitimately depends on the host are collapsed into
  one verdict. Examples: `o_tmpfile: ok-or-eopnotsupp`, `rseq:
  ok-or-enosys`, `faccessat ... denied-or-root`, and coarse clock resolution
  `positive-le-10ms`.
- Deliberately left out because they are host-kernel dependent:
  `MREMAP_DONTUNMAP` (EINVAL on the Pi 5 6.18 kernel), wait4 `ru_maxrss`
  of a reaped child (0 on the Pi 5 6.18 kernel), statx btime/attributes,
  O_DIRECT opens (tmpfs support is only in newer kernels), and
  `O_CREAT|O_DIRECTORY` semantics (changed in 6.4).

Note: under POWERarm the syscalls reach a **ppc64le** host kernel. Several
checks deliberately depend on arm64-specific ABI values that the emulator
must translate or emulate:

- open flags: O_DIRECT and O_LARGEFILE are swapped on ppc64.
- the termios layout, and TCGETS2/TCSETS2, which powerpc doesn't have.
- SA_EXPOSE_TAGBITS.
- the 47-bit user VA limit.

A diff in these places is a real emulator bug, not noise.

## Capturing goldens (natively on aarch64, e.g. the Raspberry Pi 5)

```sh
cd unittests/A64Syscalls
./build-and-golden.sh /tmp/a64sys-out     # OUTDIR: keep it off sshfs/NFS
```

The script builds every `sys_*.c` with `gcc -static -O2 -Wall -Werror` into
`$OUTDIR/bin`. It runs each program three times with `</dev/null`: twice
with stdout to a file and once with stdout through a pipe. It fails loudly
if any run differs, exits nonzero, doesn't end with `done`, or doesn't
build. Then it writes `golden/<prog>.txt`.

Set `CC=...` to use another compiler, or `NO_GOLDEN=1` to verify without
rewriting goldens. The programs also build with clang and are intended to
build with `musl-gcc` (not tested here). Goldens should come from the same
binary you'll run under the emulator: a different libc changes nothing
printed, but keep the binary and golden paired anyway.

The committed goldens were captured on a Raspberry Pi 5 with Linux 6.18
(16K pages, 47-bit VA), glibc static, gcc 14, and /tmp on tmpfs.

## Running on the 4K KVM guest

`compare.sh` runs on the bare host only. The A64Tcc bundle
(`unittests/A64Tcc/build-bundle.sh`) carries these programs and their
goldens as `syscalls.*` jobs, and `Scripts/powerarm/a64diff-run.sh 4k-kvm`
runs them on the 4K kernel.

## Running and diffing under POWERarm (POWER9 host)

Copy the `bin/` directory built on the Pi to the host. These are static
aarch64 binaries, so don't rebuild them there. Then run:

```sh
unset POWERARM_HOSTPAGEMODE
cd unittests/A64Syscalls
./compare.sh "$REPO/build-syscalls/Bin/POWERarm" /path/to/a64sys-out/bin
# a subset:
./compare.sh "$REPO/build-syscalls/Bin/POWERarm" /path/to/bin sys_io sys_fs
# sanity check natively on aarch64 (empty prefix):
./compare.sh "" /tmp/a64sys-out/bin
```

`compare.sh` runs `<prefix> <bindir>/<prog> </dev/null >out`. It prints
`PASS`/`FAIL`/`XFAIL`/`XPASS` per program, with the first 40 lines of the
unified diff for each failure. It then prints a summary and exits nonzero
if any non-XFAIL program failed.

Outputs and `.diff` files are saved under `$RESULTS` (default: a fresh
mktemp dir). `XFAIL` (space-separated program names) marks failures as
expected; it is empty by default. `RUN_TIMEOUT` defaults to 300 s per program.
