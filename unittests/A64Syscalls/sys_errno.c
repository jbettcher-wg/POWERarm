/* sys_errno: a table of calls with guaranteed error results.  Every call is
 * made with syscall() so libc wrappers can't pre-validate arguments; errno is
 * printed by symbolic name + number from a local table. */
#include "a64sys.h"
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/uio.h>

static void chk(const char *name, long r)
{
	int e = errno;
	if (r == -1)
		printf("%s: ret=-1 errno=%s\n", name, errstr(e));
	else
		printf("%s: ret=%ld UNEXPECTED-SUCCESS\n", name, r);
	errno = 0;
}

#define T(name, call) do { errno = 0; chk(name, (long)(call)); } while (0)

int main(void)
{
	tmp_enter();
	int wfd = syscall(SYS_openat, AT_FDCWD, "wonly", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	int rfd = syscall(SYS_openat, AT_FDCWD, "ronly", O_RDONLY | O_CREAT | O_TRUNC, 0644);
	syscall(SYS_mkdirat, AT_FDCWD, "dir", 0755);
	syscall(SYS_mkdirat, AT_FDCWD, "full", 0755);
	close(syscall(SYS_openat, AT_FDCWD, "full/f", O_WRONLY | O_CREAT, 0644));
	int p[2];
	syscall(SYS_pipe2, p, 0);
	char buf[64];
	long ps = sysconf(_SC_PAGESIZE);

	/* EBADF */
	T("close(-1)", syscall(SYS_close, -1));
	T("close(99999)", syscall(SYS_close, 99999));
	T("read(O_WRONLY)", syscall(SYS_read, wfd, buf, 1));
	T("write(O_RDONLY)", syscall(SYS_write, rfd, "x", 1));
	T("fstat(-1)", syscall(SYS_fstat, -1, buf));
	T("dup(-1)", syscall(SYS_dup, -1));
	T("lseek(-1)", syscall(SYS_lseek, -1, 0L, SEEK_SET));
	T("fcntl(-1,F_GETFL)", syscall(SYS_fcntl, -1, F_GETFL));
	T("ioctl(-1)", syscall(SYS_ioctl, -1, 0x5401, buf));
	T("getdents64(-1)", syscall(SYS_getdents64, -1, buf, sizeof(buf)));
	T("mkdirat(badfd)", syscall(SYS_mkdirat, 9999, "x", 0755));

	/* ENOENT */
	T("openat(nonexistent)", syscall(SYS_openat, AT_FDCWD, "nonexistent", O_RDONLY));
	T("openat(empty-path)", syscall(SYS_openat, AT_FDCWD, "", O_RDONLY));
	T("unlinkat(nonexistent)", syscall(SYS_unlinkat, AT_FDCWD, "nonexistent", 0));
	T("chdir(nonexistent)", syscall(SYS_chdir, "nonexistent"));
	T("readlinkat(nonexistent)", syscall(SYS_readlinkat, AT_FDCWD, "nonexistent", buf, sizeof(buf)));
	T("newfstatat(nonexistent)", syscall(SYS_newfstatat, AT_FDCWD, "nonexistent/x", buf, 0));
	T("renameat2(nonexistent)", syscall(SYS_renameat2, AT_FDCWD, "nonexistent", AT_FDCWD, "y", 0));
	T("execve(nonexistent)", syscall(SYS_execve, "/nonexistent/prog", NULL, NULL));

	/* ESPIPE */
	T("lseek(pipe)", syscall(SYS_lseek, p[0], 0L, SEEK_SET));
	T("pread64(pipe)", syscall(SYS_pread64, p[0], buf, 1, 0L));
	T("pwrite64(pipe)", syscall(SYS_pwrite64, p[1], "x", 1, 0L));

	/* EEXIST / ENOTEMPTY / ENOTDIR / EISDIR */
	T("mkdirat(existing)", syscall(SYS_mkdirat, AT_FDCWD, "dir", 0755));
	T("openat(O_CREAT|O_EXCL existing)", syscall(SYS_openat, AT_FDCWD, "wonly", O_CREAT | O_EXCL | O_WRONLY, 0644));
	T("symlinkat(existing)", syscall(SYS_symlinkat, "x", AT_FDCWD, "dir"));
	T("linkat(existing)", syscall(SYS_linkat, AT_FDCWD, "wonly", AT_FDCWD, "ronly", 0));
	T("unlinkat(AT_REMOVEDIR non-empty)", syscall(SYS_unlinkat, AT_FDCWD, "full", AT_REMOVEDIR));
	T("openat(O_DIRECTORY on file)", syscall(SYS_openat, AT_FDCWD, "wonly", O_RDONLY | O_DIRECTORY));
	T("mkdirat(under file)", syscall(SYS_mkdirat, AT_FDCWD, "wonly/sub", 0755));
	T("unlinkat(AT_REMOVEDIR file)", syscall(SYS_unlinkat, AT_FDCWD, "wonly", AT_REMOVEDIR));
	T("openat(dir O_WRONLY)", syscall(SYS_openat, AT_FDCWD, "dir", O_WRONLY));
	T("unlinkat(dir without flag)", syscall(SYS_unlinkat, AT_FDCWD, "dir", 0));
	T("renameat2(dir over file)", syscall(SYS_renameat2, AT_FDCWD, "dir", AT_FDCWD, "wonly", 0));
	int dfd = syscall(SYS_openat, AT_FDCWD, "dir", O_RDONLY | O_DIRECTORY);
	T("read(dir)", syscall(SYS_read, dfd, buf, 1));
	close(dfd);

	/* ELOOP */
	syscall(SYS_symlinkat, "wonly", AT_FDCWD, "lnk");
	T("openat(O_NOFOLLOW symlink)", syscall(SYS_openat, AT_FDCWD, "lnk", O_RDONLY | O_NOFOLLOW));
	syscall(SYS_symlinkat, "self", AT_FDCWD, "self");
	T("openat(self-loop)", syscall(SYS_openat, AT_FDCWD, "self", O_RDONLY));

	/* ENAMETOOLONG */
	char longname[300];
	memset(longname, 'n', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = 0;
	T("openat(name>255)", syscall(SYS_openat, AT_FDCWD, longname, O_RDONLY));
	char *longpath = malloc(PATH_MAX + 10);
	memset(longpath, 'a', PATH_MAX + 9);
	for (int i = 100; i < PATH_MAX + 9; i += 100)
		longpath[i] = '/';
	longpath[PATH_MAX + 9] = 0;
	T("openat(path>PATH_MAX)", syscall(SYS_openat, AT_FDCWD, longpath, O_RDONLY));
	free(longpath);

	/* EFAULT */
	T("write(1,(void*)1,10)", syscall(SYS_write, p[1], (void *)1, 10));
	T("read(pipe->bad ptr)", (syscall(SYS_write, p[1], "abc", 3), syscall(SYS_read, p[0], (void *)1, 3)));
	syscall(SYS_read, p[0], buf, 3);
	T("openat(bad path ptr)", syscall(SYS_openat, AT_FDCWD, (void *)1, O_RDONLY));
	T("uname(bad ptr)", syscall(SYS_uname, (void *)1));
	T("clock_gettime(bad ptr)", syscall(SYS_clock_gettime, 0, (void *)1));
	T("pipe2(bad ptr)", syscall(SYS_pipe2, (void *)1, 0));
	T("fstat(bad ptr)", syscall(SYS_fstat, rfd, (void *)1));
	T("getcwd(bad ptr)", syscall(SYS_getcwd, (void *)1, 4096));
	/* the path copy stops at the NUL: a path whose NUL is the last byte
	 * before an unmapped page is fine, one running into it is EFAULT */
	char *pg = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	munmap(pg + ps, ps);
	memset(pg, 'a', ps);
	pg[ps - 1] = 0;
	T("openat(path NUL at page end)", syscall(SYS_openat, AT_FDCWD, pg + ps - 9, O_RDONLY));
	T("openat(path NUL at page end, 30)", syscall(SYS_openat, AT_FDCWD, pg + ps - 30, O_RDONLY));
	pg[ps - 1] = 'a';
	T("openat(path into unmapped)", syscall(SYS_openat, AT_FDCWD, pg + ps - 9, O_RDONLY));
	T("newfstatat(path into unmapped)", syscall(SYS_newfstatat, AT_FDCWD, pg + ps - 100, buf, 0));
	munmap(pg, ps);
	struct iovec biov = { (void *)1, 10 };
	T("writev(bad iov_base)", syscall(SYS_writev, p[1], &biov, 1));
	T("readv(bad iov ptr)", syscall(SYS_readv, p[0], (void *)1, 1));

	/* ERANGE */
	T("getcwd(size 1)", syscall(SYS_getcwd, buf, 1));
	T("getcwd(size 0)", syscall(SYS_getcwd, buf, 0));

	/* EINVAL */
	T("lseek(bad whence)", syscall(SYS_lseek, rfd, 0L, 99));
	T("lseek(negative)", syscall(SYS_lseek, rfd, (long)-1, SEEK_SET));
	T("mmap(len 0)", syscall(SYS_mmap, 0L, 0L, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0L));
	T("mmap(no MAP_PRIVATE/SHARED)", syscall(SYS_mmap, 0L, ps, PROT_READ, MAP_ANONYMOUS, -1, 0L));
	T("munmap(unaligned)", syscall(SYS_munmap, (void *)(ps + 1), ps));
	T("mprotect(unaligned)", syscall(SYS_mprotect, (void *)(ps + 1), ps, PROT_READ));
	T("madvise(bad advice)", syscall(SYS_madvise, (void *)0, 0L, 999));
	T("dup3(same fd)", syscall(SYS_dup3, rfd, rfd, 0));
	T("pipe2(bad flags)", syscall(SYS_pipe2, p, 0xff));
	T("openat(O_TMPFILE without write)", syscall(SYS_openat, AT_FDCWD, ".", O_TMPFILE | O_RDONLY, 0600));
	T("fcntl(bad cmd)", syscall(SYS_fcntl, rfd, 9999));
	T("rt_sigaction(SIGKILL)", syscall(SYS_rt_sigaction, SIGKILL, &(struct { void *h; unsigned long f; void *r; unsigned long m; }){ 0 }, NULL, 8));
	T("rt_sigprocmask(bad how)", syscall(SYS_rt_sigprocmask, 99, &(unsigned long){ 0 }, NULL, 8));
	T("clock_gettime(bad clock)", syscall(SYS_clock_gettime, 999, buf));
	T("nanosleep(bad nsec)", syscall(SYS_nanosleep, &(struct timespec){ 0, 2000000000 }, NULL));
	T("prlimit64(bad resource)", syscall(SYS_prlimit64, 0, 9999, NULL, buf));
	T("getrandom(bad flags)", syscall(SYS_getrandom, buf, 1, 0xff00));
	T("faccessat(bad mode)", syscall(SYS_faccessat, AT_FDCWD, "ronly", 0xff));
	T("unlinkat(bad flags)", syscall(SYS_unlinkat, AT_FDCWD, "ronly", 0xff));
	T("readlinkat(not a link)", syscall(SYS_readlinkat, AT_FDCWD, "ronly", buf, sizeof(buf)));
	T("renameat2(bad flags)", syscall(SYS_renameat2, AT_FDCWD, "ronly", AT_FDCWD, "x", 0xff00));
	T("utimensat(bad nsec)", syscall(SYS_utimensat, AT_FDCWD, "ronly", (struct timespec[2]){ { 0, -5 }, { 0, 0 } }, 0));
	T("set_robust_list(bad len)", syscall(SYS_set_robust_list, buf, 1));
	T("wait4(bad options)", syscall(SYS_wait4, -1, NULL, 0x00100000, NULL));
	T("fcntl(F_DUPFD -1)", syscall(SYS_fcntl, rfd, F_DUPFD, -1));

	/* ENOTTY */
	T("ioctl(file, TCGETS)", syscall(SYS_ioctl, rfd, 0x5401, buf));
	T("ioctl(pipe, TCGETS)", syscall(SYS_ioctl, p[0], 0x5401, buf));
	T("ioctl(file, unknown)", syscall(SYS_ioctl, rfd, 0xdead, 0));

	/* EAGAIN */
	syscall(SYS_fcntl, p[0], F_SETFL, O_NONBLOCK);
	T("read(empty nonblock pipe)", syscall(SYS_read, p[0], buf, 1));

	/* EPIPE (SIGPIPE ignored so nothing is delivered) */
	signal(SIGPIPE, SIG_IGN);
	close(p[0]);
	T("write(pipe no reader)", syscall(SYS_write, p[1], "x", 1));
	close(p[1]);

	/* ECHILD / ESRCH */
	T("wait4(no children)", syscall(SYS_wait4, -1, NULL, 0, NULL));
	T("kill(bogus pid, 0)", syscall(SYS_kill, 0x7ffffff0, 0));
	T("prlimit64(bogus pid)", syscall(SYS_prlimit64, 0x7ffffff0, RLIMIT_NOFILE, NULL, buf));

	/* EMFILE */
	struct rlimit rl, low = { 16, 0 };
	getrlimit(RLIMIT_NOFILE, &rl);
	low.rlim_max = rl.rlim_max;
	setrlimit(RLIMIT_NOFILE, &low);
	T("dup3(newfd >= RLIMIT_NOFILE)", syscall(SYS_dup3, rfd, 20, 0));
	int opened[32], no = 0;
	long lr;
	while (no < 32 && (lr = syscall(SYS_dup, rfd)) >= 0)
		opened[no++] = lr;
	chk("dup(until EMFILE)", lr);
	for (int i = 0; i < no; i++)
		close(opened[i]);
	setrlimit(RLIMIT_NOFILE, &rl);

	/* ENOMEM / EACCES */
	T("mmap(MAP_FIXED 2^47)", syscall(SYS_mmap, 1UL << 47, ps, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0L));
	T("mprotect(unmapped)", syscall(SYS_mprotect, (void *)(ps * 16), ps, PROT_READ));
	T("mmap(PROT_WRITE shared on O_RDONLY)", syscall(SYS_mmap, 0L, ps, PROT_WRITE, MAP_SHARED, rfd, 0L));
	close(syscall(SYS_openat, AT_FDCWD, "noexec", O_CREAT | O_WRONLY, 0644));
	T("execve(non-executable)", syscall(SYS_execve, "noexec", (char *[]){ "noexec", NULL }, (char *[]){ NULL }));

	/* ENOSYS */
	T("syscall 1023 (unallocated)", syscall(1023));
	T("syscall -1", syscall(-1));

	close(wfd);
	close(rfd);
	tmp_leave();
	done();
	return 0;
}
