/* sys_fs: openat flags, fcntl, stat/fstat/newfstatat/statx, getdents64,
 * readlinkat, faccessat/faccessat2, getcwd/chdir, mkdirat/unlinkat/
 * renameat2/symlinkat/linkat, utimensat, ioctl ENOTTY on a regular file.
 *
 * Everything runs inside a fresh mkdtemp directory; only relative names are
 * printed.  Inode/dev numbers, timestamps not set by the test, blksize and
 * st_blocks are never printed.  Directory nlink/size are filesystem dependent
 * and are not printed either. */
#include "a64sys.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <termios.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>

#ifndef SYS_faccessat2
#define SYS_faccessat2 439
#endif
#ifndef AT_EACCESS
#define AT_EACCESS 0x200
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif
#ifndef STATX__RESERVED
#define STATX__RESERVED 0x80000000U
#endif
#ifndef O_TMPFILE
#define O_TMPFILE (020000000 | O_DIRECTORY)
#endif

/* Open-file status flags as the arm64 KERNEL defines them (asm-generic plus
 * arch/arm64 overrides).  glibc defines O_LARGEFILE as 0 on 64-bit, so the
 * kernel value is spelled out here.  These differ from ppc64 (O_DIRECT and
 * O_LARGEFILE swap, O_DIRECTORY/O_NOFOLLOW move), so they are a useful
 * translation check.  Bits outside the table are printed in hex. */
#define K_O_WRONLY    00000001
#define K_O_RDWR      00000002
#define K_O_APPEND    00002000
#define K_O_NONBLOCK  00004000
#define K_O_DSYNC     00010000
#define K_FASYNC      00020000
#define K_O_DIRECTORY 00040000
#define K_O_NOFOLLOW  00100000
#define K_O_DIRECT    00200000
#define K_O_LARGEFILE 00400000
#define K_O_NOATIME   01000000
#define K_O_PATH      010000000
#define K___O_SYNC    04000000
static const struct flagname oflags[] = {
	{ K_O_WRONLY, "O_WRONLY" }, { K_O_RDWR, "O_RDWR" },
	{ K_O_APPEND, "O_APPEND" }, { K_O_NONBLOCK, "O_NONBLOCK" },
	{ K_O_DSYNC, "O_DSYNC" }, { K___O_SYNC, "__O_SYNC" },
	{ K_FASYNC, "FASYNC" },
	{ K_O_DIRECTORY, "O_DIRECTORY" }, { K_O_NOFOLLOW, "O_NOFOLLOW" },
	{ K_O_DIRECT, "O_DIRECT" }, { K_O_LARGEFILE, "O_LARGEFILE" },
	{ K_O_NOATIME, "O_NOATIME" }, { K_O_PATH, "O_PATH" },
};
#define OFLAGS_MASK 0xffffffffUL

static const char *getfl(int fd)
{
	static char buf[FLAGSTR_MAX];
	int fl = fcntl(fd, F_GETFL);
	if (fl < 0) {
		snprintf(buf, sizeof(buf), "err-%s", errstr(errno));
		return buf;
	}
	return flagstr(buf, fl & OFLAGS_MASK, oflags, NELEM(oflags));
}

static const char *ftype(mode_t m)
{
	switch (m & S_IFMT) {
	case S_IFREG: return "reg";
	case S_IFDIR: return "dir";
	case S_IFLNK: return "lnk";
	case S_IFIFO: return "fifo";
	case S_IFCHR: return "chr";
	case S_IFBLK: return "blk";
	case S_IFSOCK: return "sock";
	default: return "?";
	}
}

static void t_open(void)
{
	int fd = openat(AT_FDCWD, "file", O_RDWR | O_CREAT | O_EXCL, 0644);
	pr_ok("openat-creat-excl", fd, errno);
	(void)!write(fd, "0123456789", 10);
	close(fd);

	errno = 0;
	fd = openat(AT_FDCWD, "file", O_RDWR | O_CREAT | O_EXCL, 0644);
	pr_ok("openat-creat-excl-exists", fd, errno);
	fd = openat(AT_FDCWD, "file", O_RDWR | O_CREAT, 0600);
	pr_ok("openat-creat-no-excl-exists", fd, errno);
	close(fd);

	errno = 0;
	fd = openat(AT_FDCWD, "file", O_RDONLY | O_DIRECTORY);
	pr_ok("openat-directory-on-file", fd, errno);
	errno = 0;
	fd = openat(AT_FDCWD, "nonexistent", O_RDONLY);
	pr_ok("openat-nonexistent", fd, errno);
	errno = 0;
	fd = openat(AT_FDCWD, "nonexistent/x", O_RDONLY | O_CREAT, 0644);
	pr_ok("openat-creat-missing-parent", fd, errno);
	errno = 0;
	fd = openat(AT_FDCWD, "file/x", O_RDONLY);
	pr_ok("openat-file-as-dir", fd, errno);

	if (symlinkat("file", AT_FDCWD, "link") != 0)
		printf("symlinkat-setup: errno=%s\n", errstr(errno));
	errno = 0;
	fd = openat(AT_FDCWD, "link", O_RDONLY | O_NOFOLLOW);
	pr_ok("openat-nofollow-symlink", fd, errno);
	fd = openat(AT_FDCWD, "link", O_RDONLY);
	pr_ok("openat-follow-symlink", fd, errno);
	close(fd);
	fd = openat(AT_FDCWD, "link", O_PATH | O_NOFOLLOW);
	pr_ok("openat-path-nofollow-symlink", fd, errno);
	struct stat st;
	printf("fstat-opath-symlink: ret=%d type=%s\n", fstat(fd, &st), ftype(st.st_mode));
	printf("fcntl-getfl-opath: %s\n", getfl(fd));
	char rb[8];
	errno = 0;
	printf("read-opath: ret=%zd errno=%s\n", read(fd, rb, 1), errstr(errno));
	ssize_t n = readlinkat(fd, "", rb, sizeof(rb));
	printf("readlinkat-empty-path-on-opath: ret=%zd data=\"%.*s\"\n", n, n > 0 ? (int)n : 0, rb);
	close(fd);

	fd = openat(AT_FDCWD, ".", O_RDONLY | O_DIRECTORY);
	pr_ok("openat-directory-on-dir", fd, errno);
	errno = 0;
	printf("read-on-dir: ret=%zd errno=%s\n", read(fd, rb, 1), errstr(errno));
	close(fd);
	errno = 0;
	fd = openat(AT_FDCWD, ".", O_WRONLY);
	pr_ok("openat-dir-wronly", fd, errno);

	/* dirfd-relative */
	mkdirat(AT_FDCWD, "d", 0755);
	int dfd = openat(AT_FDCWD, "d", O_RDONLY | O_DIRECTORY);
	fd = openat(dfd, "inner", O_WRONLY | O_CREAT | O_TRUNC, 0640);
	pr_ok("openat-dirfd-relative", fd, errno);
	close(fd);
	errno = 0;
	fd = openat(9999, "inner", O_RDONLY);
	pr_ok("openat-bad-dirfd", fd, errno);
	int ffd = open("file", O_RDONLY);
	errno = 0;
	fd = openat(ffd, "inner", O_RDONLY);
	pr_ok("openat-dirfd-is-file", fd, errno);
	/* absolute path ignores dirfd */
	fd = openat(ffd, "/dev/null", O_RDONLY);
	pr_ok("openat-absolute-ignores-dirfd", fd, errno);
	close(fd);
	close(ffd);
	struct stat ist;
	printf("fstatat-dirfd: ret=%d mode=%o\n", fstatat(dfd, "inner", &ist, 0), (unsigned)ist.st_mode);
	unlinkat(dfd, "inner", 0);
	close(dfd);
	unlinkat(AT_FDCWD, "d", AT_REMOVEDIR);

	/* O_TMPFILE */
	fd = openat(AT_FDCWD, ".", O_TMPFILE | O_RDWR, 0600);
	if (fd < 0) {
		printf("o_tmpfile: ok-or-eopnotsupp=%s\n", YN(errno == EOPNOTSUPP || errno == EISDIR));
		printf("o_tmpfile-link: ok-or-skipped=yes\n");
	} else {
		printf("o_tmpfile: ok-or-eopnotsupp=yes\n");
		(void)!write(fd, "tmp", 3);
		char p[64];
		snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
		int r = linkat(AT_FDCWD, p, AT_FDCWD, "tmplinked", AT_SYMLINK_FOLLOW);
		struct stat ts;
		int ok = r == 0 && stat("tmplinked", &ts) == 0 && ts.st_size == 3;
		printf("o_tmpfile-link: ok-or-skipped=%s\n", YN(ok));
		unlink("tmplinked");
		close(fd);
	}
}

static void t_fcntl(void)
{
	int fd = open("file", O_RDWR | O_APPEND);
	printf("fcntl-getfl-rdwr-append: %s\n", getfl(fd));
	printf("fcntl-getfd: %d\n", fcntl(fd, F_GETFD));
	printf("fcntl-setfd-cloexec: ret=%d getfd=%d\n", fcntl(fd, F_SETFD, FD_CLOEXEC), fcntl(fd, F_GETFD));
	printf("fcntl-setfd-0: ret=%d getfd=%d\n", fcntl(fd, F_SETFD, 0), fcntl(fd, F_GETFD));
	printf("fcntl-setfl-nonblock: ret=%d\n", fcntl(fd, F_SETFL, O_NONBLOCK | O_APPEND));
	printf("fcntl-getfl-after-setfl: %s\n", getfl(fd));
	/* F_SETFL ignores access mode bits */
	printf("fcntl-setfl-accmode-ignored: ret=%d\n", fcntl(fd, F_SETFL, O_WRONLY));
	printf("fcntl-getfl-after-setfl2: %s\n", getfl(fd));

	int d = fcntl(fd, F_DUPFD_CLOEXEC, 50);
	printf("fcntl-dupfd-cloexec: at-least-50=%s getfd=%d\n", YN(d >= 50), fcntl(d, F_GETFD));
	int d2 = fcntl(fd, F_DUPFD, 50);
	printf("fcntl-dupfd: at-least-50=%s distinct=%s getfd=%d\n", YN(d2 >= 50), YN(d2 != d), fcntl(d2, F_GETFD));
	close(d);
	close(d2);
	errno = 0;
	printf("fcntl-dupfd-negative: ret=%d errno=%s\n", fcntl(fd, F_DUPFD, -1), errstr(errno));
	errno = 0;
	printf("fcntl-bad-cmd: ret=%d errno=%s\n", fcntl(fd, 0x7fff), errstr(errno));
	errno = 0;
	printf("fcntl-bad-fd: ret=%d errno=%s\n", fcntl(9999, F_GETFD), errstr(errno));
	close(fd);

	fd = open("file", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	printf("fcntl-getfl-rdonly-nonblock: %s getfd=%d\n", getfl(fd), fcntl(fd, F_GETFD));
	close(fd);
	fd = open("file", O_WRONLY | O_SYNC);
	printf("fcntl-getfl-wronly-sync: %s\n", getfl(fd));
	close(fd);
	fd = open("file", O_WRONLY | O_DSYNC);
	printf("fcntl-getfl-wronly-dsync: %s\n", getfl(fd));
	close(fd);
	fd = open(".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	printf("fcntl-getfl-directory-nofollow: %s\n", getfl(fd));
	close(fd);

	/* Unknown ioctl on a regular file */
	fd = open("file", O_RDONLY);
	struct termios t;
	errno = 0;
	printf("ioctl-tcgets-regular-file: ret=%d errno=%s\n", ioctl(fd, TCGETS, &t), errstr(errno));
	errno = 0;
	printf("ioctl-unknown-regular-file: ret=%d errno=%s\n", ioctl(fd, 0x5a5a), errstr(errno));
	struct winsize ws;
	errno = 0;
	printf("ioctl-tiocgwinsz-regular-file: ret=%d errno=%s\n", ioctl(fd, TIOCGWINSZ, &ws), errstr(errno));
	int avail = -1;
	printf("ioctl-fionread-regular-file: ret=%d avail=%d\n", ioctl(fd, FIONREAD, &avail), avail);
	close(fd);
}

static void pr_stat(const char *tag, int r, int e, const struct stat *st)
{
	if (r != 0) {
		printf("%s: ret=%d errno=%s\n", tag, r, errstr(e));
		return;
	}
	printf("%s: ret=0 type=%s perm=%04o", tag, ftype(st->st_mode), (unsigned)(st->st_mode & 07777));
	if (!S_ISDIR(st->st_mode))
		printf(" nlink=%lu size=%lld", (unsigned long)st->st_nlink, (long long)st->st_size);
	printf(" blksize-nonzero=%s uid-is-euid=%s\n", YN(st->st_blksize > 0), YN(st->st_uid == geteuid()));
}

static void t_stat(void)
{
	struct stat st, st2, st3;
	int r;

	errno = 0;
	r = stat("file", &st);
	pr_stat("stat-file", r, errno, &st);
	errno = 0;
	r = fstatat(AT_FDCWD, "link", &st2, 0);
	pr_stat("newfstatat-link-follow", r, errno, &st2);
	printf("newfstatat-link-follow: same-inode-as-file=%s\n", YN(st.st_ino == st2.st_ino && st.st_dev == st2.st_dev));
	errno = 0;
	r = fstatat(AT_FDCWD, "link", &st2, AT_SYMLINK_NOFOLLOW);
	pr_stat("newfstatat-link-nofollow", r, errno, &st2);
	errno = 0;
	r = fstatat(AT_FDCWD, ".", &st2, 0);
	pr_stat("newfstatat-dot", r, errno, &st2);
	errno = 0;
	r = fstatat(AT_FDCWD, "nonexistent", &st2, 0);
	pr_stat("newfstatat-nonexistent", r, errno, &st2);
	errno = 0;
	r = fstatat(AT_FDCWD, "file", &st2, 0x8000);
	pr_stat("newfstatat-bad-flags", r, errno, &st2);

	int fd = open("file", O_RDONLY);
	r = fstat(fd, &st3);
	pr_stat("fstat-file", r, errno, &st3);
	errno = 0;
	r = fstatat(fd, "", &st2, AT_EMPTY_PATH);
	pr_stat("newfstatat-empty-path", r, errno, &st2);
	printf("fstat-vs-stat: same-inode=%s same-mtime=%s\n", YN(st3.st_ino == st.st_ino),
	       YN(st3.st_mtim.tv_sec == st.st_mtim.tv_sec && st3.st_mtim.tv_nsec == st.st_mtim.tv_nsec));
	close(fd);

	int p[2];
	(void)!pipe(p);
	r = fstat(p[0], &st2);
	pr_stat("fstat-pipe", r, errno, &st2);
	close(p[0]);
	close(p[1]);

	fd = open("/dev/null", O_RDONLY);
	r = fstat(fd, &st2);
	printf("fstat-devnull: ret=%d type=%s rdev-is-1:3=%s\n", r, ftype(st2.st_mode),
	       YN(major(st2.st_rdev) == 1 && minor(st2.st_rdev) == 3));
	close(fd);
	errno = 0;
	printf("fstat-badfd: ret=%d errno=%s\n", fstat(9999, &st2), errstr(errno));

	/* statx */
	struct statx sx;
	memset(&sx, 0xff, sizeof(sx));
	r = statx(AT_FDCWD, "file", 0, STATX_BASIC_STATS, &sx);
	printf("statx-file: ret=%d basic-mask-set=%s type=%s perm=%04o nlink=%u size=%llu\n", r,
	       YN((sx.stx_mask & STATX_BASIC_STATS) == STATX_BASIC_STATS), ftype(sx.stx_mode),
	       (unsigned)(sx.stx_mode & 07777), sx.stx_nlink, (unsigned long long)sx.stx_size);
	printf("statx-file: ino-matches-stat=%s dev-matches-stat=%s mtime-matches-stat=%s uid-matches=%s blksize-nonzero=%s\n",
	       YN(sx.stx_ino == st.st_ino),
	       YN(makedev(sx.stx_dev_major, sx.stx_dev_minor) == st.st_dev),
	       YN(sx.stx_mtime.tv_sec == st.st_mtim.tv_sec && sx.stx_mtime.tv_nsec == (unsigned)st.st_mtim.tv_nsec),
	       YN(sx.stx_uid == st.st_uid), YN(sx.stx_blksize > 0));
	r = statx(AT_FDCWD, "file", 0, STATX_TYPE | STATX_SIZE, &sx);
	printf("statx-partial-mask: ret=%d type-and-size-set=%s type=%s size=%llu\n", r,
	       YN((sx.stx_mask & (STATX_TYPE | STATX_SIZE)) == (STATX_TYPE | STATX_SIZE)), ftype(sx.stx_mode),
	       (unsigned long long)sx.stx_size);
	r = statx(AT_FDCWD, "link", AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &sx);
	printf("statx-link-nofollow: ret=%d type=%s size=%llu\n", r, ftype(sx.stx_mode), (unsigned long long)sx.stx_size);
	fd = open("file", O_RDONLY);
	r = statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx);
	printf("statx-empty-path: ret=%d type=%s size=%llu\n", r, ftype(sx.stx_mode), (unsigned long long)sx.stx_size);
	close(fd);
	errno = 0;
	r = syscall(SYS_statx, AT_FDCWD, "file", 0, STATX__RESERVED, &sx);
	printf("statx-reserved-mask: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_statx, AT_FDCWD, "file", 0x80000000, STATX_BASIC_STATS, &sx);
	printf("statx-bad-flags: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_statx, AT_FDCWD, "nonexistent", 0, STATX_BASIC_STATS, &sx);
	printf("statx-nonexistent: ret=%d errno=%s\n", r, errstr(errno));
}

/* kernel layout, flexible name (glibc dirent64 has a fixed 256-byte name) */
struct kdirent64 {
	uint64_t d_ino;
	int64_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

struct ent { char name[64]; unsigned char type; };
static int entcmp(const void *a, const void *b) { return strcmp(((struct ent *)a)->name, ((struct ent *)b)->name); }

static const char *dtype(unsigned char t)
{
	switch (t) {
	case DT_REG: return "reg";
	case DT_DIR: return "dir";
	case DT_LNK: return "lnk";
	case DT_FIFO: return "fifo";
	case DT_UNKNOWN: return "unknown";
	default: return "other";
	}
}

static void t_getdents(void)
{
	mkdirat(AT_FDCWD, "gd", 0755);
	static const char *names[] = { "zeta", "alpha", "Mid", "dir1", "sym", "fifo", "a.b" };
	char path[64];
	for (int i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "gd/%s", names[i]);
		close(open(path, O_CREAT | O_WRONLY, 0644));
	}
	mkdirat(AT_FDCWD, "gd/dir1", 0755);
	symlinkat("alpha", AT_FDCWD, "gd/sym");
	mknodat(AT_FDCWD, "gd/fifo", S_IFIFO | 0644, 0);
	close(open("gd/a.b", O_CREAT | O_WRONLY, 0644));

	int dfd = open("gd", O_RDONLY | O_DIRECTORY);
	struct ent ents[32];
	int ne = 0, calls = 0;
	size_t bufsz = 128; /* small buffer: forces several getdents64 calls */
	char *buf = malloc(bufsz);
	for (;;) {
		long n = syscall(SYS_getdents64, dfd, buf, bufsz);
		if (n < 0) {
			printf("getdents64: errno=%s\n", errstr(errno));
			break;
		}
		if (n == 0)
			break;
		calls++;
		for (long off = 0; off < n;) {
			struct kdirent64 *d = (struct kdirent64 *)(buf + off);
			if (ne < 32) {
				size_t l = strnlen(d->d_name, sizeof(ents[ne].name) - 1);
				memcpy(ents[ne].name, d->d_name, l);
				ents[ne].name[l] = 0;
				ents[ne].type = d->d_type;
				ne++;
			}
			off += d->d_reclen;
		}
	}
	free(buf);
	qsort(ents, ne, sizeof(ents[0]), entcmp);
	printf("getdents64: count=%d multiple-calls=%s\n", ne, YN(calls > 1));
	for (int i = 0; i < ne; i++)
		printf("getdents64-entry: name=%s type=%s\n", ents[i].name, dtype(ents[i].type));

	/* rewind and read again in one go */
	lseek(dfd, 0, SEEK_SET);
	char big[4096];
	long n = syscall(SYS_getdents64, dfd, big, sizeof(big));
	int cnt = 0;
	for (long off = 0; off < n; cnt++)
		off += ((struct kdirent64 *)(big + off))->d_reclen;
	printf("getdents64-rewind: count=%d\n", cnt);
	errno = 0;
	lseek(dfd, 0, SEEK_SET);
	n = syscall(SYS_getdents64, dfd, big, 8);
	printf("getdents64-tiny-buffer: ret=%ld errno=%s\n", n, errstr(errno));
	errno = 0;
	n = syscall(SYS_getdents64, dfd, (void *)8, 4096);
	printf("getdents64-bad-ptr: ret=%ld errno=%s\n", n, errstr(errno));
	close(dfd);
	int ffd = open("file", O_RDONLY);
	errno = 0;
	n = syscall(SYS_getdents64, ffd, big, sizeof(big));
	printf("getdents64-on-file: ret=%ld errno=%s\n", n, errstr(errno));
	close(ffd);

	/* opendir/readdir via libc for good measure */
	DIR *dp = opendir("gd");
	int lc = 0;
	while (readdir(dp))
		lc++;
	closedir(dp);
	printf("readdir-libc: count=%d\n", lc);

	for (int i = 0; i < 7; i++) {
		snprintf(path, sizeof(path), "gd/%s", names[i]);
		unlinkat(AT_FDCWD, path, strcmp(names[i], "dir1") ? 0 : AT_REMOVEDIR);
	}
	printf("rmdir-gd-emptied: ret=%d\n", unlinkat(AT_FDCWD, "gd", AT_REMOVEDIR));
}

static void t_readlink(void)
{
	char buf[64];
	ssize_t n = readlinkat(AT_FDCWD, "link", buf, sizeof(buf));
	printf("readlinkat: ret=%zd data=\"%.*s\"\n", n, n > 0 ? (int)n : 0, buf);
	n = readlinkat(AT_FDCWD, "link", buf, 2);
	printf("readlinkat-truncated: ret=%zd data=\"%.*s\"\n", n, n > 0 ? (int)n : 0, buf);
	errno = 0;
	n = readlinkat(AT_FDCWD, "file", buf, sizeof(buf));
	printf("readlinkat-not-link: ret=%zd errno=%s\n", n, errstr(errno));
	errno = 0;
	n = syscall(SYS_readlinkat, AT_FDCWD, "link", buf, 0);
	printf("readlinkat-zero-size: ret=%zd errno=%s\n", n, errstr(errno));
	errno = 0;
	n = readlinkat(AT_FDCWD, "nonexistent", buf, sizeof(buf));
	printf("readlinkat-nonexistent: ret=%zd errno=%s\n", n, errstr(errno));
	symlinkat("a/very/long/target/that/does/not/exist", AT_FDCWD, "dangling");
	n = readlinkat(AT_FDCWD, "dangling", buf, sizeof(buf));
	printf("readlinkat-dangling: ret=%zd data=\"%.*s\"\n", n, n > 0 ? (int)n : 0, buf);
	struct stat st;
	errno = 0;
	printf("stat-dangling: ret=%d errno=%s\n", stat("dangling", &st), errstr(errno));
	/* symlink loop */
	symlinkat("loop2", AT_FDCWD, "loop1");
	symlinkat("loop1", AT_FDCWD, "loop2");
	errno = 0;
	printf("open-symlink-loop: ret=%d errno=%s\n", open("loop1", O_RDONLY) < 0 ? -1 : 0, errstr(errno));
	errno = 0;
	printf("symlinkat-exists: ret=%d errno=%s\n", symlinkat("x", AT_FDCWD, "file"), errstr(errno));
	/* /proc/self/exe is a symlink; just check it resolves to something absolute */
	n = readlinkat(AT_FDCWD, "/proc/self/exe", buf, sizeof(buf));
	printf("readlinkat-proc-self-exe: positive=%s absolute=%s\n", YN(n > 0), YN(n > 0 && buf[0] == '/'));
	unlink("dangling");
	unlink("loop1");
	unlink("loop2");
}

static void t_access(void)
{
	int euid0 = geteuid() == 0;
	int r;
	close(open("noperm", O_CREAT | O_WRONLY, 0000));
	close(open("rx", O_CREAT | O_WRONLY, 0500));

	printf("faccessat-f_ok: ret=%ld\n", syscall(SYS_faccessat, AT_FDCWD, "file", F_OK));
	printf("faccessat-rw_ok: ret=%ld\n", syscall(SYS_faccessat, AT_FDCWD, "file", R_OK | W_OK));
	errno = 0;
	r = syscall(SYS_faccessat, AT_FDCWD, "file", X_OK);
	printf("faccessat-x_ok-on-0644: denied=%s\n", YN(r == -1 && errno == EACCES));
	errno = 0;
	r = syscall(SYS_faccessat, AT_FDCWD, "noperm", R_OK);
	printf("faccessat-r_ok-on-0000: denied-or-root=%s\n", YN((r == -1 && errno == EACCES) || (euid0 && r == 0)));
	printf("faccessat-x_ok-on-0500: ret=%ld\n", syscall(SYS_faccessat, AT_FDCWD, "rx", X_OK));
	errno = 0;
	r = syscall(SYS_faccessat, AT_FDCWD, "nonexistent", F_OK);
	printf("faccessat-nonexistent: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_faccessat, AT_FDCWD, "file", 0x10);
	printf("faccessat-bad-mode: ret=%d errno=%s\n", r, errstr(errno));

	errno = 0;
	r = syscall(SYS_faccessat2, AT_FDCWD, "file", R_OK, AT_EACCESS);
	printf("faccessat2-eaccess: ret=%d errno=%s\n", r, errstr(r ? errno : 0));
	errno = 0;
	r = syscall(SYS_faccessat2, AT_FDCWD, "noperm", W_OK, AT_EACCESS);
	printf("faccessat2-eaccess-0000: denied-or-root=%s\n", YN((r == -1 && errno == EACCES) || (euid0 && r == 0)));
	symlinkat("nonexistent-target", AT_FDCWD, "dangle");
	errno = 0;
	r = syscall(SYS_faccessat2, AT_FDCWD, "dangle", F_OK, AT_SYMLINK_NOFOLLOW);
	printf("faccessat2-nofollow-dangling: ret=%d errno=%s\n", r, errstr(r ? errno : 0));
	errno = 0;
	r = syscall(SYS_faccessat2, AT_FDCWD, "dangle", F_OK, 0);
	printf("faccessat2-follow-dangling: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_faccessat2, AT_FDCWD, "file", F_OK, 0x1);
	printf("faccessat2-bad-flags: ret=%d errno=%s\n", r, errstr(errno));
	int fd = open("file", O_RDONLY);
	errno = 0;
	r = syscall(SYS_faccessat2, fd, "", R_OK, AT_EMPTY_PATH);
	printf("faccessat2-empty-path: ret=%d errno=%s\n", r, errstr(r ? errno : 0));
	close(fd);
	/* libc wrappers */
	printf("access-libc: ret=%d\n", access("file", R_OK));
	printf("faccessat-libc-eaccess: ret=%d\n", faccessat(AT_FDCWD, "file", R_OK, AT_EACCESS));
	unlink("dangle");
	unlink("noperm");
	unlink("rx");
}

static void t_cwd(void)
{
	char real[PATH_MAX], cwd[PATH_MAX];
	if (!realpath(a64_tmpdir, real))
		strcpy(real, "?");
	size_t rl = strlen(real);
	char *c = getcwd(cwd, sizeof(cwd));
	printf("getcwd: ok=%s is-tmpdir=%s\n", YN(c != NULL), YN(c && !strcmp(cwd, real)));
	long r = syscall(SYS_getcwd, cwd, sizeof(cwd));
	printf("getcwd-raw: ret-is-len+1=%s\n", YN(r == (long)rl + 1));

	mkdirat(AT_FDCWD, "cd1", 0755);
	mkdirat(AT_FDCWD, "cd1/cd2", 0755);
	printf("chdir-sub: ret=%d\n", chdir("cd1/cd2"));
	c = getcwd(cwd, sizeof(cwd));
	printf("getcwd-sub: suffix=%s\n", c && !strncmp(cwd, real, rl) ? cwd + rl : "?");
	printf("chdir-dotdot: ret=%d\n", chdir(".."));
	c = getcwd(cwd, sizeof(cwd));
	printf("getcwd-dotdot: suffix=%s\n", c && !strncmp(cwd, real, rl) ? cwd + rl : "?");
	errno = 0;
	printf("chdir-nonexistent: ret=%d errno=%s\n", chdir("nope"), errstr(errno));
	errno = 0;
	printf("chdir-file: ret=%d errno=%s\n", chdir("../file"), errstr(errno));
	int dfd = open("..", O_RDONLY | O_DIRECTORY);
	printf("fchdir: ret=%d\n", fchdir(dfd));
	close(dfd);
	c = getcwd(cwd, sizeof(cwd));
	printf("getcwd-after-fchdir: is-tmpdir=%s\n", YN(c && !strcmp(cwd, real)));
	/* cwd removed underneath us */
	chdir("cd1/cd2") == 0 ? (void)0 : (void)printf("chdir failed\n");
	rmdir("../cd2");
	errno = 0;
	r = syscall(SYS_getcwd, cwd, sizeof(cwd));
	printf("getcwd-removed: ret=%ld errno=%s\n", r, errstr(errno));
	chdir(real) == 0 ? (void)0 : (void)printf("chdir back failed\n");
	rmdir("cd1");
}

static void t_dirops(void)
{
	umask(022);
	int r = mkdirat(AT_FDCWD, "m1", 0777);
	struct stat st;
	stat("m1", &st);
	printf("mkdirat: ret=%d type=%s perm=%04o\n", r, ftype(st.st_mode), (unsigned)(st.st_mode & 07777));
	errno = 0;
	printf("mkdirat-exists: ret=%d errno=%s\n", mkdirat(AT_FDCWD, "m1", 0755), errstr(errno));
	errno = 0;
	printf("mkdirat-missing-parent: ret=%d errno=%s\n", mkdirat(AT_FDCWD, "no/m2", 0755), errstr(errno));
	errno = 0;
	printf("mkdirat-under-file: ret=%d errno=%s\n", mkdirat(AT_FDCWD, "file/m2", 0755), errstr(errno));
	r = mkdirat(AT_FDCWD, "m1/sub", 0700);
	stat("m1/sub", &st);
	printf("mkdirat-nested: ret=%d perm=%04o\n", r, (unsigned)(st.st_mode & 07777));
	errno = 0;
	printf("unlinkat-dir-without-removedir: ret=%d errno=%s\n", unlinkat(AT_FDCWD, "m1/sub", 0), errstr(errno));
	errno = 0;
	printf("unlinkat-removedir-nonempty: ret=%d errno=%s\n", unlinkat(AT_FDCWD, "m1", AT_REMOVEDIR), errstr(errno));
	errno = 0;
	printf("unlinkat-removedir-on-file: ret=%d errno=%s\n", unlinkat(AT_FDCWD, "file", AT_REMOVEDIR), errstr(errno));
	errno = 0;
	printf("unlinkat-bad-flags: ret=%d errno=%s\n", unlinkat(AT_FDCWD, "file", 0x1), errstr(errno));
	errno = 0;
	printf("unlinkat-nonexistent: ret=%d errno=%s\n", unlinkat(AT_FDCWD, "nonexistent", 0), errstr(errno));
	errno = 0;
	printf("unlinkat-removedir-dot: ret=%d errno=%s\n", unlinkat(AT_FDCWD, "m1/sub/.", AT_REMOVEDIR), errstr(errno));
	printf("unlinkat-removedir: ret=%d\n", unlinkat(AT_FDCWD, "m1/sub", AT_REMOVEDIR));

	/* renameat2 */
	close(open("ra", O_CREAT | O_WRONLY, 0644));
	int fd = open("rb", O_CREAT | O_WRONLY, 0644);
	(void)!write(fd, "BB", 2);
	close(fd);
	errno = 0;
	r = syscall(SYS_renameat2, AT_FDCWD, "ra", AT_FDCWD, "rb", RENAME_NOREPLACE);
	printf("renameat2-noreplace-exists: ret=%d errno=%s\n", r, errstr(errno));
	r = syscall(SYS_renameat2, AT_FDCWD, "ra", AT_FDCWD, "rc", RENAME_NOREPLACE);
	printf("renameat2-noreplace-new: ret=%d ra-gone=%s\n", r, YN(access("ra", F_OK) != 0));
	r = syscall(SYS_renameat2, AT_FDCWD, "rc", AT_FDCWD, "rb", RENAME_EXCHANGE);
	stat("rc", &st);
	printf("renameat2-exchange: ret=%d rc-size=%lld\n", r, (long long)st.st_size);
	errno = 0;
	r = syscall(SYS_renameat2, AT_FDCWD, "rc", AT_FDCWD, "nonexistent", RENAME_EXCHANGE);
	printf("renameat2-exchange-missing: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_renameat2, AT_FDCWD, "rc", AT_FDCWD, "rb", RENAME_EXCHANGE | RENAME_NOREPLACE);
	printf("renameat2-exchange-and-noreplace: ret=%d errno=%s\n", r, errstr(errno));
	r = syscall(SYS_renameat2, AT_FDCWD, "rc", AT_FDCWD, "rb", 0);
	stat("rb", &st);
	printf("renameat2-overwrite: ret=%d rb-size=%lld rc-gone=%s\n", r, (long long)st.st_size, YN(access("rc", F_OK) != 0));
	errno = 0;
	r = syscall(SYS_renameat2, AT_FDCWD, "rb", AT_FDCWD, "m1", 0);
	printf("renameat2-file-over-dir: ret=%d errno=%s\n", r, errstr(errno));
	mkdirat(AT_FDCWD, "m2", 0755);
	close(open("m2/x", O_CREAT | O_WRONLY, 0644));
	errno = 0;
	r = syscall(SYS_renameat2, AT_FDCWD, "m1", AT_FDCWD, "m2", 0);
	printf("renameat2-dir-over-nonempty-dir: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_renameat2, AT_FDCWD, "m1", AT_FDCWD, "m1/inside", 0);
	printf("renameat2-dir-into-itself: ret=%d errno=%s\n", r, errstr(errno));
	r = renameat(AT_FDCWD, "m1", AT_FDCWD, "m3");
	printf("renameat-dir: ret=%d\n", r);
	errno = 0;
	r = syscall(SYS_renameat2, AT_FDCWD, "rb", AT_FDCWD, "rd", 0x80);
	printf("renameat2-bad-flags: ret=%d errno=%s\n", r, errstr(errno));
	unlink("m2/x");
	rmdir("m2");
	rmdir("m3");

	/* linkat */
	r = linkat(AT_FDCWD, "rb", AT_FDCWD, "hard", 0);
	stat("rb", &st);
	printf("linkat: ret=%d nlink=%lu\n", r, (unsigned long)st.st_nlink);
	errno = 0;
	printf("linkat-exists: ret=%d errno=%s\n", linkat(AT_FDCWD, "rb", AT_FDCWD, "hard", 0), errstr(errno));
	mkdirat(AT_FDCWD, "ld", 0755);
	errno = 0;
	printf("linkat-dir: ret=%d errno=%s\n", linkat(AT_FDCWD, "ld", AT_FDCWD, "ld2", 0), errstr(errno));
	rmdir("ld");
	symlinkat("rb", AT_FDCWD, "srb");
	r = linkat(AT_FDCWD, "srb", AT_FDCWD, "hsym", 0);
	fstatat(AT_FDCWD, "hsym", &st, AT_SYMLINK_NOFOLLOW);
	printf("linkat-symlink-nofollow: ret=%d type=%s\n", r, ftype(st.st_mode));
	r = linkat(AT_FDCWD, "srb", AT_FDCWD, "hfol", AT_SYMLINK_FOLLOW);
	fstatat(AT_FDCWD, "hfol", &st, AT_SYMLINK_NOFOLLOW);
	printf("linkat-symlink-follow: ret=%d type=%s nlink=%lu\n", r, ftype(st.st_mode), (unsigned long)st.st_nlink);
	errno = 0;
	printf("linkat-bad-flags: ret=%d errno=%s\n", linkat(AT_FDCWD, "rb", AT_FDCWD, "h3", 0x1), errstr(errno));
	printf("unlink-hard: ret=%d\n", unlinkat(AT_FDCWD, "hard", 0));
	stat("rb", &st);
	printf("nlink-after-unlink: %lu\n", (unsigned long)st.st_nlink);
	/* open fd survives unlink */
	fd = open("hfol", O_RDONLY);
	unlink("hfol");
	fstat(fd, &st);
	char b2[4] = { 0 };
	printf("fstat-unlinked-open: nlink=%lu read=%zd\n", (unsigned long)st.st_nlink, pread(fd, b2, 2, 0));
	close(fd);
	unlink("hsym");
	unlink("srb");
	unlink("rb");
	errno = 0;
	printf("symlinkat-empty-target: ret=%d errno=%s\n", symlinkat("", AT_FDCWD, "empty"), errstr(errno));
}

static void pr_times(const char *tag, const char *path, int flags)
{
	struct stat st;
	fstatat(AT_FDCWD, path, &st, flags);
	printf("%s: atime=%lld.%09ld mtime=%lld.%09ld\n", tag, (long long)st.st_atim.tv_sec, st.st_atim.tv_nsec,
	       (long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
}

static void t_utimens(void)
{
	close(open("ut", O_CREAT | O_WRONLY, 0644));
	struct timespec ts[2] = { { 1000000000, 123456789 }, { 1234567890, 987654321 } };
	int r = utimensat(AT_FDCWD, "ut", ts, 0);
	printf("utimensat-explicit: ret=%d\n", r);
	pr_times("utimensat-explicit", "ut", 0);

	struct timespec om[2] = { { 0, UTIME_OMIT }, { 1500000000, 5 } };
	r = utimensat(AT_FDCWD, "ut", om, 0);
	printf("utimensat-omit-atime: ret=%d\n", r);
	pr_times("utimensat-omit-atime", "ut", 0);

	struct timespec nw[2] = { { 42, 0 }, { 0, UTIME_NOW } };
	time_t before = time(NULL);
	r = utimensat(AT_FDCWD, "ut", nw, 0);
	struct stat st;
	stat("ut", &st);
	printf("utimensat-now-mtime: ret=%d atime=%lld.%09ld mtime-recent=%s\n", r, (long long)st.st_atim.tv_sec,
	       st.st_atim.tv_nsec, YN(st.st_mtim.tv_sec >= before - 2 && st.st_mtim.tv_sec <= time(NULL) + 2));

	/* negative and pre-epoch time */
	struct timespec neg[2] = { { -1, 0 }, { -86400, 500 } };
	r = utimensat(AT_FDCWD, "ut", neg, 0);
	printf("utimensat-negative: ret=%d\n", r);
	pr_times("utimensat-negative", "ut", 0);

	int fd = open("ut", O_RDONLY);
	struct timespec ft[2] = { { 7, 7 }, { 8, 8 } };
	r = futimens(fd, ft);
	printf("futimens: ret=%d\n", r);
	pr_times("futimens", "ut", 0);
	/* utimensat(fd, NULL path) is the raw futimens form */
	struct timespec ft2[2] = { { 9, 9 }, { 10, 10 } };
	r = syscall(SYS_utimensat, fd, NULL, ft2, 0);
	printf("utimensat-null-path: ret=%d\n", r);
	pr_times("utimensat-null-path", "ut", 0);
	close(fd);

	symlinkat("ut", AT_FDCWD, "utl");
	struct timespec lt[2] = { { 100, 1 }, { 200, 2 } };
	r = utimensat(AT_FDCWD, "utl", lt, AT_SYMLINK_NOFOLLOW);
	printf("utimensat-symlink-nofollow: ret=%d\n", r);
	pr_times("utimensat-symlink-nofollow-link", "utl", AT_SYMLINK_NOFOLLOW);
	pr_times("utimensat-symlink-nofollow-target", "ut", 0);

	struct timespec bad[2] = { { 0, 1000000000 }, { 0, 0 } };
	errno = 0;
	r = utimensat(AT_FDCWD, "ut", bad, 0);
	printf("utimensat-bad-nsec: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = utimensat(AT_FDCWD, "ut", ts, 0x1);
	printf("utimensat-bad-flags: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = utimensat(AT_FDCWD, "nonexistent", ts, 0);
	printf("utimensat-nonexistent: ret=%d errno=%s\n", r, errstr(errno));
	struct timespec both_omit[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
	r = utimensat(AT_FDCWD, "ut", both_omit, 0);
	printf("utimensat-both-omit: ret=%d\n", r);
	pr_times("utimensat-both-omit", "ut", 0);
	unlink("utl");
	unlink("ut");
}

int main(void)
{
	umask(022);
	tmp_enter();
	t_open();
	t_fcntl();
	t_stat();
	t_getdents();
	t_readlink();
	t_access();
	t_cwd();
	t_dirops();
	t_utimens();
	unlink("link");
	unlink("file");
	tmp_leave();
	done();
	return 0;
}
