// SPDX-License-Identifier: MIT
//
// The per-user rootfs overlay (docs/powerarm/DESIGN.md section 6.2a): under
// /usr and /etc a guest must see an ordinary writable filesystem while every
// change lands in the overlay directory and the base rootfs stays untouched.
//
// Self-checking and differential at once. The same operations run natively on
// the Pi inside a scratch copy of the fixture (OVT_ROOT=<dir>) and under
// POWERarm with the fixture as the base rootfs and an empty overlay
// (OVT_ROOT unset, so the paths are the guest's real /usr and /etc), once as
// an ordinary program and once sealed (POWERARM_ROOTFSOVERLAYSEAL=on). All
// runs must print the same PASS lines. run.sh adds the checks only the host
// can make: the base is byte-for-byte unchanged, the overlay holds the
// changes, the host is untouched, a host tool the base lacks is still
// reachable, and a sealed process does not see it.
//
//   rootfs_overlay --make-fixture DIR   create the base tree in DIR
//   rootfs_overlay --probe PATH         print "exists" or the errno name
//   rootfs_overlay --stat PATH          print mode, mtime and size, or the errno name
//   rootfs_overlay                      run the checks (OVT_ROOT optional)
//   rootfs_overlay --cwd DIR            chdir, print getcwd and the short-buffer error
//   rootfs_overlay --host-file PATH     change PATH through a read-only
//                                       descriptor, then delete it; print what was seen
//   rootfs_overlay --install PATH       create PATH exclusively, as a package manager does
//   rootfs_overlay --remove PATH        unlink PATH
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char* Root = "";
static int Failures;

static void Check(const char* Name, int Ok) {
  printf("%s %s\n", Ok ? "PASS" : "FAIL", Name);
  if (!Ok) {
    Failures++;
  }
}

static const char* P(const char* Path) {
  static char Buf[8][4096];
  static int Next;
  char* B = Buf[Next++ & 7];
  snprintf(B, sizeof(Buf[0]), "%s%s", Root, Path);
  return B;
}

static int WriteFile(const char* Path, int Flags, const char* Text) {
  int FD = open(Path, Flags | O_WRONLY | O_CLOEXEC, 0644);
  if (FD < 0) {
    return -errno;
  }
  ssize_t Len = (ssize_t)strlen(Text);
  ssize_t N = write(FD, Text, Len);
  close(FD);
  return N == Len ? 0 : -EIO;
}

// The file's contents equal Text exactly.
static int Holds(const char* Path, const char* Text) {
  char Buf[4096];
  int FD = open(Path, O_RDONLY | O_CLOEXEC);
  if (FD < 0) {
    return 0;
  }
  ssize_t N = read(FD, Buf, sizeof(Buf) - 1);
  close(FD);
  if (N < 0) {
    return 0;
  }
  Buf[N] = 0;
  return strcmp(Buf, Text) == 0;
}

static int Missing(const char* Path) {
  struct stat St;
  return lstat(Path, &St) != 0 && errno == ENOENT;
}

static int CompareNames(const void* A, const void* B) {
  return strcmp(*(char* const*)A, *(char* const*)B);
}

// The directory's entries, without . and .., sorted and joined by spaces.
static void Listing(const char* Path, char* Out, size_t Size) {
  char* Names[256];
  int Count = 0;
  Out[0] = 0;
  DIR* D = opendir(Path);
  if (!D) {
    snprintf(Out, Size, "<opendir: %s>", strerror(errno));
    return;
  }
  struct dirent* E;
  while ((E = readdir(D)) && Count < 256) {
    if (strcmp(E->d_name, ".") && strcmp(E->d_name, "..")) {
      Names[Count++] = strdup(E->d_name);
    }
  }
  closedir(D);
  qsort(Names, Count, sizeof(Names[0]), CompareNames);
  for (int i = 0; i < Count; i++) {
    strncat(Out, Names[i], Size - strlen(Out) - 1);
    if (i + 1 < Count) {
      strncat(Out, " ", Size - strlen(Out) - 1);
    }
    free(Names[i]);
  }
}

static int MakeFixture(const char* Dir) {
  // The base rootfs: a slice of /usr and /etc plus Arch's root-level
  // lib -> usr/lib symlink. No /var/lib/pacman.
  char Path[4096];
  const char* Dirs[] = {"", "/usr", "/usr/lib", "/usr/share", "/usr/share/ovtest", "/usr/share/ovtest/sub", "/usr/share/ovtest/rmdir",
                        "/etc", "/etc/ovtest.d", "/var", "/var/lib", "/tmp"};
  for (size_t i = 0; i < sizeof(Dirs) / sizeof(Dirs[0]); i++) {
    snprintf(Path, sizeof(Path), "%s%s", Dir, Dirs[i]);
    if (mkdir(Path, 0755) != 0 && errno != EEXIST) {
      perror(Path);
      return 1;
    }
  }
  const char* Files[][2] = {
    {"/usr/share/ovtest/keep.txt", "keep\n"},       {"/usr/share/ovtest/modify.txt", "original\n"},
    {"/usr/share/ovtest/delete.txt", "delete\n"},   {"/usr/share/ovtest/rename.txt", "rename\n"},
    {"/usr/share/ovtest/chmod.txt", "chmod\n"},     {"/usr/share/ovtest/sub/inner.txt", "inner\n"},
    {"/etc/ovtest.conf", "a=1\n"},                  {"/etc/ovtest.d/one.conf", "one\n"},
    {"/etc/ovtest.d/two.conf", "two\n"},
  };
  for (size_t i = 0; i < sizeof(Files) / sizeof(Files[0]); i++) {
    snprintf(Path, sizeof(Path), "%s%s", Dir, Files[i][0]);
    if (WriteFile(Path, O_CREAT | O_TRUNC, Files[i][1]) != 0) {
      perror(Path);
      return 1;
    }
  }
  snprintf(Path, sizeof(Path), "%s/usr/share/ovtest/link", Dir);
  if (symlink("keep.txt", Path) != 0) {
    perror(Path);
    return 1;
  }
  snprintf(Path, sizeof(Path), "%s/lib", Dir);
  if (symlink("usr/lib", Path) != 0) {
    perror(Path);
    return 1;
  }
  return 0;
}

static int Probe(const char* Path) {
  struct stat St;
  if (stat(Path, &St) == 0) {
    printf("exists\n");
  } else {
    printf("%s\n", strerrorname_np(errno));
  }
  return 0;
}

// Mode, mtime and size, or the errno name.
static int StatPath(const char* Path) {
  struct stat St;
  if (stat(Path, &St) == 0) {
    printf("%o %lld %lld\n", St.st_mode & 07777, (long long)St.st_mtime, (long long)St.st_size);
  } else {
    printf("%s\n", strerrorname_np(errno));
  }
  return 0;
}

// The working directory's path after chdir(Path), then the error from a
// getcwd buffer one byte too small for it.
static int Cwd(const char* Path) {
  char Buf[4096];
  if (chdir(Path) != 0 || getcwd(Buf, sizeof(Buf)) == NULL) {
    printf("%s\n", strerrorname_np(errno));
    return 0;
  }
  char Short[4096];
  const char* Err = getcwd(Short, strlen(Buf)) != NULL ? "fits" : strerrorname_np(errno);
  printf("%s %s\n", Buf, Err);
  return 0;
}

// A file the guest sees but the base lacks (a host file, under POWERarm):
// change it through a read-only descriptor, as libarchive's fixups and
// install(1) do, check the path shows the change, then delete it.
static int HostFile(const char* Path) {
  struct stat St;
  if (stat(Path, &St) != 0) {
    printf("%s\n", strerrorname_np(errno));
    return 0;
  }
  int FD = open(Path, O_RDONLY | O_CLOEXEC);
  const int Chmod = FD >= 0 && fchmod(FD, 0444) == 0;
  const struct timespec Times[2] = {{1000000000, 0}, {1000000000, 0}};
  const int Utime = FD >= 0 && futimens(FD, Times) == 0;
  const int Seen = stat(Path, &St) == 0 && (St.st_mode & 07777) == 0444 && St.st_mtime == 1000000000;
  if (FD >= 0) {
    close(FD);
  }
  const int Gone = unlink(Path) == 0 && Missing(Path);
  printf("fchmod=%d futimens=%d seen=%d unlink=%d\n", Chmod, Utime, Seen, Gone);
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && strcmp(argv[1], "--cwd") == 0) {
    return Cwd(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--stat") == 0) {
    return StatPath(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--host-file") == 0) {
    return HostFile(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--install") == 0) {
    const int R = WriteFile(argv[2], O_CREAT | O_EXCL, "guest\n");
    printf("%s\n", R == 0 && Holds(argv[2], "guest\n") ? "installed" : R ? strerrorname_np(-R) : "EIO");
    return 0;
  }
  if (argc == 3 && strcmp(argv[1], "--remove") == 0) {
    if (unlink(argv[2]) != 0) {
      printf("%s\n", strerrorname_np(errno));
    } else {
      printf("%s\n", Missing(argv[2]) ? "removed" : "still-visible");
    }
    return 0;
  }
  if (argc == 3 && strcmp(argv[1], "--make-fixture") == 0) {
    return MakeFixture(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--probe") == 0) {
    return Probe(argv[2]);
  }
  if (getenv("OVT_ROOT")) {
    Root = getenv("OVT_ROOT");
  }
  setvbuf(stdout, NULL, _IOLBF, 0);

  char List[4096];
  struct stat St;

  // Changes through a read-only descriptor onto a base file and a base
  // directory. Under POWERarm they land on overlay copies; run.sh checks the
  // base did not change.
  {
    int FD = open(P("/etc/ovtest.d/two.conf"), O_RDONLY | O_CLOEXEC);
    Check("fd.fchmod", FD >= 0 && fchmod(FD, 0640) == 0 && stat(P("/etc/ovtest.d/two.conf"), &St) == 0 && (St.st_mode & 07777) == 0640 &&
                         Holds(P("/etc/ovtest.d/two.conf"), "two\n"));
    const struct timespec Times[2] = {{1100000000, 0}, {1100000000, 0}};
    Check("fd.futimens", FD >= 0 && futimens(FD, Times) == 0 && stat(P("/etc/ovtest.d/two.conf"), &St) == 0 && St.st_mtime == 1100000000);
    Check("fd.fchownat-empty-path", FD >= 0 && fchownat(FD, "", getuid(), getgid(), AT_EMPTY_PATH) == 0 &&
                                      stat(P("/etc/ovtest.d/two.conf"), &St) == 0 && St.st_uid == getuid() && (St.st_mode & 07777) == 0640);
    if (FD >= 0) {
      close(FD);
    }
    int Dir = open(P("/usr/lib"), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    Check("fd.fchmod-dir", Dir >= 0 && fchmod(Dir, 0700) == 0 && stat(P("/usr/lib"), &St) == 0 && (St.st_mode & 07777) == 0700 &&
                             fchmod(Dir, 0755) == 0 && stat(P("/usr/lib"), &St) == 0 && (St.st_mode & 07777) == 0755);
    if (Dir >= 0) {
      close(Dir);
    }
  }

  // A directory the base lacks. Under POWERarm /usr/bin is the host's, so it
  // already exists unless the process is sealed; either way everything made
  // inside it lands in the overlay (run.sh checks the host is untouched).
  Check("hostdir.mkdir", (mkdir(P("/usr/bin"), 0755) == 0 || errno == EEXIST) && mkdir(P("/usr/bin/ovtest.d"), 0755) == 0 &&
                           stat(P("/usr/bin/ovtest.d"), &St) == 0 && S_ISDIR(St.st_mode));
  Check("hostdir.create", WriteFile(P("/usr/bin/ovtest.d/kept"), O_CREAT | O_EXCL, "kept\n") == 0 && Holds(P("/usr/bin/ovtest.d/kept"), "kept\n"));
  Check("hostdir.rename-unlink", WriteFile(P("/usr/bin/ovtest.d/a"), O_CREAT | O_EXCL, "a\n") == 0 &&
                                   rename(P("/usr/bin/ovtest.d/a"), P("/usr/bin/ovtest.d/b")) == 0 && Missing(P("/usr/bin/ovtest.d/a")) &&
                                   Holds(P("/usr/bin/ovtest.d/b"), "a\n") && unlink(P("/usr/bin/ovtest.d/b")) == 0 &&
                                   Missing(P("/usr/bin/ovtest.d/b")));
  Listing(P("/usr/bin/ovtest.d"), List, sizeof(List));
  Check("hostdir.readdir", strcmp(List, "kept") == 0);

  // Create.
  Check("create.usr", WriteFile(P("/usr/share/ovtest/new.txt"), O_CREAT | O_EXCL, "new\n") == 0 &&
                        Holds(P("/usr/share/ovtest/new.txt"), "new\n"));
  Check("create.etc", WriteFile(P("/etc/ovtest.new"), O_CREAT | O_EXCL, "fresh\n") == 0 && Holds(P("/etc/ovtest.new"), "fresh\n"));
  Check("create.excl-existing", WriteFile(P("/usr/share/ovtest/keep.txt"), O_CREAT | O_EXCL, "x") == -EEXIST);
  Check("create.no-parent", WriteFile(P("/usr/share/ovtest/nodir/x"), O_CREAT, "x") == -ENOENT);

  // Modify base files.
  Check("modify.append", WriteFile(P("/usr/share/ovtest/modify.txt"), O_APPEND, "appended\n") == 0 &&
                           Holds(P("/usr/share/ovtest/modify.txt"), "original\nappended\n"));
  Check("modify.truncate-etc", WriteFile(P("/etc/ovtest.conf"), O_TRUNC, "a=2\n") == 0 && Holds(P("/etc/ovtest.conf"), "a=2\n"));
  Check("modify.untouched-neighbour", Holds(P("/usr/share/ovtest/keep.txt"), "keep\n"));
  Check("chmod", chmod(P("/usr/share/ovtest/chmod.txt"), 0600) == 0 && stat(P("/usr/share/ovtest/chmod.txt"), &St) == 0 &&
                   (St.st_mode & 07777) == 0600 && Holds(P("/usr/share/ovtest/chmod.txt"), "chmod\n"));
  {
    struct timespec Times[2] = {{1000000000, 0}, {1000000000, 0}};
    Check("utimens", utimensat(AT_FDCWD, P("/usr/share/ovtest/keep.txt"), Times, 0) == 0 &&
                       stat(P("/usr/share/ovtest/keep.txt"), &St) == 0 && St.st_mtime == 1000000000);
  }

  // Rename.
  Check("rename.base-file", rename(P("/usr/share/ovtest/rename.txt"), P("/usr/share/ovtest/renamed.txt")) == 0 &&
                              Missing(P("/usr/share/ovtest/rename.txt")) && Holds(P("/usr/share/ovtest/renamed.txt"), "rename\n"));
  Check("rename.etc", rename(P("/etc/ovtest.d/one.conf"), P("/etc/ovtest.d/uno.conf")) == 0 && Missing(P("/etc/ovtest.d/one.conf")) &&
                        Holds(P("/etc/ovtest.d/uno.conf"), "one\n"));
  Check("rename.replace", rename(P("/usr/share/ovtest/new.txt"), P("/usr/share/ovtest/modify.txt")) == 0 &&
                            Missing(P("/usr/share/ovtest/new.txt")) && Holds(P("/usr/share/ovtest/modify.txt"), "new\n"));
  Check("rename.noreplace", renameat2(AT_FDCWD, P("/usr/share/ovtest/renamed.txt"), AT_FDCWD, P("/usr/share/ovtest/keep.txt"),
                                      RENAME_NOREPLACE) != 0 &&
                              errno == EEXIST);

  // Delete.
  Check("unlink.base", unlink(P("/usr/share/ovtest/delete.txt")) == 0 && Missing(P("/usr/share/ovtest/delete.txt")));
  Check("unlink.again", unlink(P("/usr/share/ovtest/delete.txt")) != 0 && errno == ENOENT);
  Check("unlink.own", unlink(P("/etc/ovtest.new")) == 0 && Missing(P("/etc/ovtest.new")));
  Check("recreate.file", WriteFile(P("/usr/share/ovtest/delete.txt"), O_CREAT | O_EXCL, "again\n") == 0 &&
                           Holds(P("/usr/share/ovtest/delete.txt"), "again\n"));
  Check("unlink.dir-is-eisdir", unlink(P("/usr/share/ovtest/sub")) != 0 && errno == EISDIR);

  // Directories.
  Check("mkdir", mkdir(P("/usr/share/ovtest/newdir"), 0755) == 0 && stat(P("/usr/share/ovtest/newdir"), &St) == 0 && S_ISDIR(St.st_mode));
  Check("mkdir.exists", mkdir(P("/usr/share/ovtest/sub"), 0755) != 0 && errno == EEXIST);
  Check("mkdir.file-inside", WriteFile(P("/usr/share/ovtest/newdir/f"), O_CREAT, "f\n") == 0 && Holds(P("/usr/share/ovtest/newdir/f"), "f\n"));
  Check("rmdir.base-empty", rmdir(P("/usr/share/ovtest/rmdir")) == 0 && Missing(P("/usr/share/ovtest/rmdir")));
  Check("rmdir.not-empty", rmdir(P("/usr/share/ovtest/sub")) != 0 && errno == ENOTEMPTY);
  Check("rmdir.after-unlink", unlink(P("/usr/share/ovtest/sub/inner.txt")) == 0 && rmdir(P("/usr/share/ovtest/sub")) == 0 &&
                                Missing(P("/usr/share/ovtest/sub")));
  Listing(P("/usr/share/ovtest/sub"), List, sizeof(List));
  Check("rmdir.recreate-is-empty", mkdir(P("/usr/share/ovtest/sub"), 0755) == 0 && strcmp(List, "<opendir: No such file or directory>") == 0 &&
                                     (Listing(P("/usr/share/ovtest/sub"), List, sizeof(List)), List[0] == 0) &&
                                     Missing(P("/usr/share/ovtest/sub/inner.txt")));
  Check("rename.own-dir", rename(P("/usr/share/ovtest/newdir"), P("/usr/share/ovtest/movedir")) == 0 &&
                            Holds(P("/usr/share/ovtest/movedir/f"), "f\n") && Missing(P("/usr/share/ovtest/newdir")));

  // Links.
  Check("symlink", symlink("keep.txt", P("/usr/share/ovtest/newlink")) == 0 && Holds(P("/usr/share/ovtest/newlink"), "keep\n"));
  {
    char Target[256];
    ssize_t N = readlink(P("/usr/share/ovtest/link"), Target, sizeof(Target) - 1);
    Check("readlink.base", N == 8 && memcmp(Target, "keep.txt", 8) == 0);
    N = readlink(P("/usr/share/ovtest/newlink"), Target, sizeof(Target) - 1);
    Check("readlink.new", N == 8 && memcmp(Target, "keep.txt", 8) == 0);
  }
  Check("link.hard", link(P("/usr/share/ovtest/chmod.txt"), P("/usr/share/ovtest/chmod.hard")) == 0 &&
                       stat(P("/usr/share/ovtest/chmod.hard"), &St) == 0 && St.st_nlink == 2 && Holds(P("/usr/share/ovtest/chmod.hard"), "chmod\n"));
  Check("symlink.dir-traverse", WriteFile(P("/lib/ovtest-via-link.txt"), O_CREAT | O_EXCL, "via\n") == 0 &&
                                  Holds(P("/usr/lib/ovtest-via-link.txt"), "via\n"));

  // Listings: the guest's own changes, nothing deleted, no bookkeeping entries.
  Listing(P("/usr/share/ovtest"), List, sizeof(List));
  Check("readdir.usr", strcmp(List, "chmod.hard chmod.txt delete.txt keep.txt link modify.txt movedir newlink renamed.txt sub") == 0);
  if (strcmp(List, "chmod.hard chmod.txt delete.txt keep.txt link modify.txt movedir newlink renamed.txt sub") != 0) {
    printf("  got: %s\n", List);
  }
  Listing(P("/etc/ovtest.d"), List, sizeof(List));
  Check("readdir.etc", strcmp(List, "two.conf uno.conf") == 0);
  {
    // rewinddir, and seekdir back to a telldir position, replay the same entries.
    DIR* D = opendir(P("/usr/share/ovtest"));
    char First[4096] = "", Second[4096] = "", Tail1[4096] = "", Tail2[4096] = "";
    long Mark = -1;
    int Count = 0;
    struct dirent* E;
    while (D && (E = readdir(D))) {
      strncat(First, E->d_name, sizeof(First) - strlen(First) - 2);
      strcat(First, "/");
      if (++Count == 4) {
        Mark = telldir(D);
      }
      if (Mark != -1 && Count > 4) {
        strncat(Tail1, E->d_name, sizeof(Tail1) - strlen(Tail1) - 2);
        strcat(Tail1, "/");
      }
    }
    if (D) {
      rewinddir(D);
      while ((E = readdir(D))) {
        strncat(Second, E->d_name, sizeof(Second) - strlen(Second) - 2);
        strcat(Second, "/");
      }
      seekdir(D, Mark);
      while ((E = readdir(D))) {
        strncat(Tail2, E->d_name, sizeof(Tail2) - strlen(Tail2) - 2);
        strcat(Tail2, "/");
      }
      closedir(D);
    }
    Check("readdir.rewind", D && Count == 12 && strcmp(First, Second) == 0);
    Check("readdir.seekdir", D && Tail1[0] && strcmp(Tail1, Tail2) == 0);
  }

  // Relative and descriptor-relative paths.
  {
    char Cwd[4096];
    int Ok = chdir(P("/usr/share/ovtest")) == 0 && WriteFile("rel.txt", O_CREAT | O_EXCL, "rel\n") == 0 &&
             Holds(P("/usr/share/ovtest/rel.txt"), "rel\n") && getcwd(Cwd, sizeof(Cwd)) != NULL &&
             strcmp(Cwd, P("/usr/share/ovtest")) == 0;
    Check("relative.cwd", Ok);
    Check("relative.unlink", unlink("rel.txt") == 0 && Missing(P("/usr/share/ovtest/rel.txt")));
    Check("relative.chdir-back", chdir("/") == 0);
    int Dir = open(P("/usr/share/ovtest"), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int FD = Dir < 0 ? -1 : openat(Dir, "keep.txt", O_RDONLY | O_CLOEXEC);
    char Buf[16] = "";
    Check("dirfd.read-base", FD >= 0 && read(FD, Buf, sizeof(Buf) - 1) == 5 && strcmp(Buf, "keep\n") == 0);
    if (FD >= 0) {
      close(FD);
    }
    FD = Dir < 0 ? -1 : openat(Dir, "at.txt", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
    Check("dirfd.create", FD >= 0 && write(FD, "at\n", 3) == 3 && close(FD) == 0 && Holds(P("/usr/share/ovtest/at.txt"), "at\n"));
    Check("dirfd.stat-deleted", Dir >= 0 && fstatat(Dir, "rename.txt", &St, 0) != 0 && errno == ENOENT);
    Check("dirfd.unlink", Dir >= 0 && unlinkat(Dir, "at.txt", 0) == 0 && Missing(P("/usr/share/ovtest/at.txt")));
    if (Dir >= 0) {
      close(Dir);
    }
  }

  // Outside the guest-owned prefixes nothing changes: /tmp is the host's.
  {
    char Tmp[256];
    snprintf(Tmp, sizeof(Tmp), "/tmp/rootfs_overlay.%d", (int)getpid());
    Check("host.tmp", WriteFile(Tmp, O_CREAT | O_EXCL, "tmp\n") == 0 && Holds(Tmp, "tmp\n") && unlink(Tmp) == 0);
  }
  // Package-manager state never falls through to the host's.
  Check("pacman-state.absent", Missing(P("/var/lib/pacman")) && Missing(P("/var/lib/pacman/local")));

  printf("%s\n", Failures ? "FAILED" : "ALL PASSED");
  return Failures ? 1 : 0;
}
