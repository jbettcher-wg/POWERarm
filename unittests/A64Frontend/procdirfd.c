// Directory descriptors for the kernel's own filesystems. A rootfs carries
// empty mount points for /proc, /sys and /dev; open() of the directory itself
// must still reach the host's filesystem, or every *at() relative to it fails.
// Chromium's single-thread check does exactly that (fstatat(proc_fd,
// "self/task/")) and killed its zygote and GPU process.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

static void Check(const char* Name, int Ok) { printf("%s %s\n", Ok ? "PASS" : "FAIL", Name); }

static int FsType(const char* Path, long Want) {
  int Fd = open(Path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  struct statfs Sf;
  int Ok = Fd >= 0 && fstatfs(Fd, &Sf) == 0 && (long)Sf.f_type == Want;
  if (Fd >= 0) close(Fd);
  return Ok;
}

int main(void) {
  Check("proc-is-procfs", FsType("/proc", 0x9fa0));
  Check("sys-is-sysfs", FsType("/sys", 0x62656572));
  // devtmpfs reports tmpfs's magic, so compare against / instead: the rootfs's
  // empty /dev sits on the same filesystem as the rootfs itself.
  struct statfs Root, DevFs;
  Check("dev-not-on-root-fs", statfs("/", &Root) == 0 && statfs("/dev", &DevFs) == 0 && Root.f_type != DevFs.f_type);
  int Proc = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  struct stat St;
  Check("fstatat-self-task", Proc >= 0 && fstatat(Proc, "self/task/", &St, 0) == 0 && St.st_nlink == 3);
  Check("openat-self-status", Proc >= 0 && openat(Proc, "self/status", O_RDONLY | O_CLOEXEC) >= 0);
  int Dev = open("/dev", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  Check("openat-dev-null", Dev >= 0 && openat(Dev, "null", O_WRONLY | O_CLOEXEC) >= 0);
  return 0;
}
