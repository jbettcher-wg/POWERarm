# A64Tcc: static TinyCC under POWERarm, byte-compared with the Pi

The final M1 smoke test. A static AArch64 TinyCC compiles a few C programs to
objects and static executables. The same compiler, running natively on the
Raspberry Pi 5 and under POWERarm on the POWER9 (64K host and 4K KVM guest),
must produce byte-identical files, and the executables it builds must behave
identically when they run.

The bundle also carries the A64Syscalls programs, so the syscall conformance
tests run on the 4K KVM guest through the same harness.

## Build the bundle (Pi)

```sh
unittests/A64Tcc/build-bundle.sh ~/a64tcc -j 1
```

This fetches pinned inputs, which are recorded in the bundle's `VERSION`:

- TinyCC `0fb54300b56512754221d80adda85ddb9815bceb` from https://repo.or.cz/tinycc.git
- Debian `musl-dev` 1.2.5-3.1~deb13u1 (arm64) for the sysroot
- Debian `busybox-static` 1.37.0-6+b9 (arm64) for the job shell

The script then does the following:

1. Builds `tcc` statically with `--config-musl --config-bcheck=no`. Its
   include, library and crt paths all sit under `{B}`, the `-B` directory,
   so tcc never sees the build machine's own headers.
2. Builds the A64Syscalls programs.
3. Runs every job natively to capture the goldens.
4. Checks that the `sys_*` outputs equal `unittests/A64Syscalls/golden`.
5. Re-runs everything and requires a clean compare.

Nothing it produces is committed. glibc's `libc.a` can't be used: TinyCC's
arm64 linker rejects its TLS relocations.

## Run it (POWER9)

Copy `a64tcc-<hash>/` to the host, then run:

```sh
Scripts/powerarm/a64diff-run.sh 64k    build-process/Bin/POWERarm --bundle DIR --suites programs
Scripts/powerarm/a64diff-run.sh 4k-kvm build-process/Bin/POWERarm --bundle DIR --suites programs
```

## Jobs

Each job runs in a fresh, empty cwd. Jobs that compile first copy their
source into that cwd, so no absolute path can reach the output.

| Job | What it checks |
|---|---|
| `syscalls.sys_*` | A64Syscalls program output (the goldens match `unittests/A64Syscalls/golden`) |
| `tcc.version` | `tcc -v` |
| `tcc.obj.{hello,multi,libc}` | `tcc -c`; the object is `cat` to stdout and compared byte for byte |
| `tcc.exe.{hello,multi,libc}` | `tcc -static` against musl; the executable is compared byte for byte |
| `tcc.run.{hello,multi,libc}` | the tcc-built executable, built and run under POWERarm: stdout and exit status |
| `tcc.jit.multi` | `tcc -run`: code generated into guest memory and executed there |
| `tcc.error.bad` | a compile error: tcc's diagnostic on stderr and its exit status |

The sources in `src/` cover different parts of the compiler:

- `hello.c` is stdio.
- `multi.c` covers recursion, structs by value, a switch table,
  64-bit/unsigned/shift arithmetic, doubles and function pointers.
- `libc.c` covers qsort, printf float formatting, strtod/strtol, math and
  malloc.
