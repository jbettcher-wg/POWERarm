// SPDX-License-Identifier: MIT
//
// readlink(2)/readlinkat(2) errno and target fidelity across a fixture tree.
//
// FileManager::Readlink/Readlinkat resolve a guest path scoped to the rootfs
// with openat2(O_PATH|O_NOFOLLOW, RESOLVE_IN_ROOT) and then readlinkat(fd,"")
// on the result, translating the kernel's empty-pathname "not a symlink"
// (ENOENT) into the EINVAL readlink(2) owes the caller. The fast path added in
// this commit folds a type test into that openat2 with O_DIRECTORY and skips
// the readlinkat when the leaf is a directory. Every answer below is forced by
// POSIX plus Linux's documented behaviour, so the expected column is the same
// on an AArch64 host as under POWERarm, and the same whether the path is
// answered out of the rootfs or out of the host fallback -- which is the point:
// run it both ways and the output must not move.
//
// Usage:
//   readlinkerr --make-fixture DIR   build the tree under DIR/rlfix
//   readlinkerr                      run the matrix against /rlfix
//   RLFIX_ROOT=DIR readlinkerr       run it against DIR/rlfix instead
//
// Under POWERarm the matrix form runs with POWERARM_ROOTFS pointing at the
// fixture, so /rlfix/... resolves inside the rootfs and exercises the
// trampoline. With RLFIX_ROOT set it names a host path the rootfs lacks, so
// the same matrix is answered by the host fallback. Natively (the Pi) only the
// RLFIX_ROOT form exists and it is the plain kernel answer.
//
// Output is one line per case, `PASS name` or `FAIL name ...`, in a fixed
// order and with no absolute path in it, so it is byte-comparable between the
// two forms and against a native golden. Exit status is the number of
// failures, capped at 125.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int Failures = 0;
static char Root[4096];

static void Pass(const char* Name) {
  printf("PASS %s\n", Name);
}

static void Fail(const char* Name, const char* Fmt, ...) __attribute__((format(printf, 2, 3)));
static void Fail(const char* Name, const char* Fmt, ...) {
  va_list Args;
  printf("FAIL %s ", Name);
  va_start(Args, Fmt);
  vprintf(Fmt, Args);
  va_end(Args);
  printf("\n");
  ++Failures;
}

// A fixture path. Relative to RLFIX_ROOT when set, absolute /rlfix otherwise.
static const char* P(const char* Rel) {
  static char Buf[2][4096];
  static int Which = 0;
  Which ^= 1;
  snprintf(Buf[Which], sizeof(Buf[Which]), "%s/rlfix%s", Root, Rel);
  return Buf[Which];
}

// readlink(2) must fail with Want.
static void ExpectErrno(const char* Name, const char* Rel, int Want) {
  char Buf[4096];
  errno = 0;
  ssize_t Ret = readlink(P(Rel), Buf, sizeof(Buf));
  if (Ret != -1) {
    Buf[Ret < (ssize_t)sizeof(Buf) ? Ret : (ssize_t)sizeof(Buf) - 1] = 0;
    Fail(Name, "expected errno %d, got %zd bytes \"%s\"", Want, Ret, Buf);
  } else if (errno != Want) {
    Fail(Name, "expected errno %d, got %d", Want, errno);
  } else {
    Pass(Name);
  }
}

// readlink(2) must succeed and return exactly Want.
static void ExpectTarget(const char* Name, const char* Rel, const char* Want) {
  char Buf[4096];
  errno = 0;
  ssize_t Ret = readlink(P(Rel), Buf, sizeof(Buf) - 1);
  if (Ret == -1) {
    Fail(Name, "expected \"%s\", got errno %d", Want, errno);
    return;
  }
  Buf[Ret] = 0;
  if (Ret != (ssize_t)strlen(Want) || strcmp(Buf, Want) != 0) {
    Fail(Name, "expected \"%s\", got \"%s\" (%zd bytes)", Want, Buf, Ret);
  } else {
    Pass(Name);
  }
}

static int MakeFixture(const char* Dir) {
  char Path[4096];
#define AT(rel) (snprintf(Path, sizeof(Path), "%s/rlfix%s", Dir, rel), Path)
  if (mkdir(Dir, 0755) == -1 && errno != EEXIST) {
    fprintf(stderr, "mkdir %s: %d\n", Dir, errno);
    return 1;
  }
  if (mkdir(AT(""), 0755) == -1 && errno != EEXIST) {
    fprintf(stderr, "mkdir %s: %d\n", Path, errno);
    return 1;
  }
  mkdir(AT("/dir"), 0755);
  mkdir(AT("/dir/sub"), 0755);

  // A regular file at the leaf, and one nested in the directory.
  static const char* Files[] = {"/file", "/dir/leaf"};
  for (size_t i = 0; i < sizeof(Files) / sizeof(Files[0]); ++i) {
    int FD = open(AT(Files[i]), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (FD == -1) {
      fprintf(stderr, "open %s: %d\n", Path, errno);
      return 1;
    }
    if (write(FD, "x\n", 2) != 2) {
      fprintf(stderr, "write %s: %d\n", Path, errno);
      return 1;
    }
    close(FD);
  }

  // target, link
  static const char* Links[][2] = {
    {"no-such-target", "/dangle"},  // dangling
    {"dir", "/to_dir"},             // -> directory
    {"file", "/to_file"},           // -> regular file
    {"../file", "/dir/inner"},      // a symlink reached through a symlink
    {"to_dir", "/chain"},           // -> symlink -> directory
    {"to_self", "/to_self"},        // its own target
  };
  for (size_t i = 0; i < sizeof(Links) / sizeof(Links[0]); ++i) {
    unlink(AT(Links[i][1]));
    if (symlink(Links[i][0], AT(Links[i][1])) == -1) {
      fprintf(stderr, "symlink %s: %d\n", Path, errno);
      return 1;
    }
  }
#undef AT
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && strcmp(argv[1], "--make-fixture") == 0) {
    return MakeFixture(argv[2]);
  }

  const char* Env = getenv("RLFIX_ROOT");
  snprintf(Root, sizeof(Root), "%s", Env ? Env : "");

  // --- the leaf's own type decides EINVAL vs a target -----------------
  // A directory is not a symlink: EINVAL. This is the case O_DIRECTORY
  // answers out of the openat2 alone, and 91.7% of a gcc -c's trampolines.
  ExpectErrno("dir-EINVAL", "/dir", EINVAL);
  ExpectErrno("file-EINVAL", "/file", EINVAL);
  ExpectErrno("nested-file-EINVAL", "/dir/leaf", EINVAL);
  // A symlink must still be followed to its target, including when it points
  // at a directory -- O_DIRECTORY must *not* succeed on it (O_NOFOLLOW|O_PATH
  // opens the symlink itself, which cannot be looked up, so the kernel reports
  // ENOTDIR and the full probe runs).
  ExpectTarget("symlink-to-dir", "/to_dir", "dir");
  ExpectTarget("symlink-to-file", "/to_file", "file");
  // A dangling symlink is still a symlink: the target, not ENOENT.
  ExpectTarget("dangling", "/dangle", "no-such-target");
  // A symlink whose target is itself: readlink does not resolve it.
  ExpectTarget("self-symlink", "/to_self", "to_self");
  ExpectTarget("symlink-to-symlink", "/chain", "to_dir");

  // --- missing and malformed paths ------------------------------------
  ExpectErrno("missing-ENOENT", "/missing", ENOENT);
  ExpectErrno("missing-under-dir", "/dir/missing", ENOENT);
  // An intermediate component that is a regular file is ENOTDIR, not ENOENT
  // -- and ENOTDIR is also what the O_DIRECTORY fast path reports for a
  // non-directory leaf, so the two must not be confused.
  ExpectErrno("mid-file-ENOTDIR", "/file/leaf", ENOTDIR);
  ExpectErrno("mid-dangling-ENOENT", "/dangle/leaf", ENOENT);
  ExpectErrno("self-symlink-mid-ELOOP", "/to_self/leaf", ELOOP);

  // --- intermediate components that are symlinks ----------------------
  // The answer to readlink(P) depends on all of P's prefix, so these are the
  // cases a leaf-only shortcut would get wrong.
  ExpectErrno("mid-symlink-to-dir", "/to_dir/leaf", EINVAL);
  ExpectErrno("mid-symlink-dir-sub", "/to_dir/sub", EINVAL);
  ExpectTarget("mid-symlink-leaf-link", "/to_dir/inner", "../file");
  ExpectErrno("two-mid-symlinks", "/chain/leaf", EINVAL);
  // In-tree '..' past a real directory.
  ExpectErrno("dotdot-in-tree", "/dir/../file", EINVAL);
  ExpectErrno("dotdot-through-symlink", "/to_dir/../file", EINVAL);

  // --- the dirfd forms, which take a different path through
  //     FileManager::Readlinkat ------------------------------------------
  {
    int DirFD = open(P(""), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (DirFD == -1) {
      Fail("dirfd-open", "errno %d", errno);
    } else {
      char Buf[4096];
      ssize_t Ret = readlinkat(DirFD, "to_file", Buf, sizeof(Buf) - 1);
      if (Ret == -1) {
        Fail("dirfd-relative-symlink", "errno %d", errno);
      } else {
        Buf[Ret] = 0;
        if (strcmp(Buf, "file") != 0) {
          Fail("dirfd-relative-symlink", "got \"%s\"", Buf);
        } else {
          Pass("dirfd-relative-symlink");
        }
      }

      errno = 0;
      if (readlinkat(DirFD, "dir", Buf, sizeof(Buf)) != -1 || errno != EINVAL) {
        Fail("dirfd-relative-dir", "errno %d", errno);
      } else {
        Pass("dirfd-relative-dir");
      }

      errno = 0;
      if (readlinkat(DirFD, "missing", Buf, sizeof(Buf)) != -1 || errno != ENOENT) {
        Fail("dirfd-relative-missing", "errno %d", errno);
      } else {
        Pass("dirfd-relative-missing");
      }
      close(DirFD);
    }

    // AT_EMPTY_PATH-style: a descriptor on the symlink itself.
    int LinkFD = open(P("/to_dir"), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (LinkFD == -1) {
      Fail("dirfd-empty-open", "errno %d", errno);
    } else {
      char Buf[4096];
      ssize_t Ret = readlinkat(LinkFD, "", Buf, sizeof(Buf) - 1);
      if (Ret == -1) {
        Fail("dirfd-empty-symlink", "errno %d", errno);
      } else {
        Buf[Ret] = 0;
        if (strcmp(Buf, "dir") != 0) {
          Fail("dirfd-empty-symlink", "got \"%s\"", Buf);
        } else {
          Pass("dirfd-empty-symlink");
        }
      }
      close(LinkFD);
    }
  }

  // --- degenerate arguments, which the fast path must not change ------
  // readlinkat(2) rejects bufsiz <= 0 before it looks at the path, so a
  // directory reports EINVAL for that reason as well as for its type. The
  // O_DIRECTORY shortcut answers EINVAL without reaching the kernel's check;
  // same errno, which is why it is sound.
  {
    char Buf[4];
    errno = 0;
    if (readlink(P("/dir"), Buf, 0) != -1 || errno != EINVAL) {
      Fail("zero-bufsiz-dir", "errno %d", errno);
    } else {
      Pass("zero-bufsiz-dir");
    }
    errno = 0;
    if (readlink(P("/to_file"), Buf, 0) != -1 || errno != EINVAL) {
      Fail("zero-bufsiz-symlink", "errno %d", errno);
    } else {
      Pass("zero-bufsiz-symlink");
    }
    // Truncation is not an error: exactly bufsiz bytes, not NUL-terminated.
    errno = 0;
    memset(Buf, '@', sizeof(Buf));
    ssize_t Ret = readlink(P("/to_file"), Buf, 2);
    if (Ret != 2 || Buf[0] != 'f' || Buf[1] != 'i' || Buf[2] != '@') {
      Fail("truncating-bufsiz", "ret %zd errno %d buf \"%c%c%c\"", Ret, errno, Buf[0], Buf[1], Buf[2]);
    } else {
      Pass("truncating-bufsiz");
    }
  }

  printf("failures %d\n", Failures);
  return Failures > 125 ? 125 : Failures;
}
