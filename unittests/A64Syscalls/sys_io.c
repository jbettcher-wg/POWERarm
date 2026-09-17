/* sys_io: read, write, readv, writev, pread64, pwrite64, lseek, close,
 * dup, dup3, pipe2 and basic fd behaviour. */
#include "a64sys.h"
#include <fcntl.h>
#include <sys/uio.h>

static void show_file(const char *tag, int fd)
{
	char buf[128];
	ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
	if (n < 0) {
		printf("%s: pread errno=%s\n", tag, errstr(errno));
		return;
	}
	for (ssize_t i = 0; i < n; i++)
		if (buf[i] == 0)
			buf[i] = '.';
	buf[n] = 0;
	printf("%s: len=%zd content=\"%s\"\n", tag, n, buf);
}

static void t_rw(void)
{
	int fd = openat(AT_FDCWD, "f1", O_RDWR | O_CREAT | O_TRUNC, 0600);
	pr_ok("openat-create", fd, errno);
	ssize_t n = write(fd, "hello world", 11);
	printf("write: ret=%zd\n", n);
	printf("lseek-cur-after-write: ret=%ld\n", (long)lseek(fd, 0, SEEK_CUR));
	char buf[64] = { 0 };
	n = read(fd, buf, sizeof(buf));
	printf("read-at-eof: ret=%zd\n", n);
	printf("lseek-set: ret=%ld\n", (long)lseek(fd, 6, SEEK_SET));
	n = read(fd, buf, 3);
	printf("read-3: ret=%zd data=\"%.*s\"\n", n, (int)n, buf);
	printf("lseek-cur+1: ret=%ld\n", (long)lseek(fd, 1, SEEK_CUR));
	n = read(fd, buf, sizeof(buf));
	printf("read-short: ret=%zd data=\"%.*s\"\n", n, (int)n, buf);
	printf("lseek-end-2: ret=%ld\n", (long)lseek(fd, -2, SEEK_END));
	errno = 0;
	printf("lseek-negative: ret=%ld errno=%s\n", (long)lseek(fd, -100, SEEK_SET), errstr(errno));
	errno = 0;
	printf("lseek-bad-whence: ret=%ld errno=%s\n", (long)lseek(fd, 0, 17), errstr(errno));
	printf("read-zero-len: ret=%zd\n", read(fd, buf, 0));
	printf("write-zero-len: ret=%zd\n", write(fd, buf, 0));

	/* pread/pwrite don't move the offset */
	lseek(fd, 3, SEEK_SET);
	n = pread(fd, buf, 5, 0);
	printf("pread: ret=%zd data=\"%.*s\" offset-after=%ld\n", n, (int)n, buf, (long)lseek(fd, 0, SEEK_CUR));
	n = pwrite(fd, "HELLO", 5, 0);
	printf("pwrite: ret=%zd offset-after=%ld\n", n, (long)lseek(fd, 0, SEEK_CUR));
	n = pread(fd, buf, sizeof(buf), 100);
	printf("pread-past-eof: ret=%zd\n", n);
	errno = 0;
	n = pread(fd, buf, 1, -1);
	printf("pread-negative-offset: ret=%zd errno=%s\n", n, errstr(errno));
	show_file("after-pwrite", fd);

	/* Hole: seek past the end and write */
	lseek(fd, 16, SEEK_SET);
	n = write(fd, "X", 1);
	printf("write-after-hole: ret=%zd size=%ld\n", n, (long)lseek(fd, 0, SEEK_END));
	show_file("hole", fd);

	/* readv / writev */
	struct iovec wv[3] = { { "abc", 3 }, { "", 0 }, { "defgh", 5 } };
	ftruncate(fd, 0) == 0 ? (void)0 : (void)printf("ftruncate failed\n");
	lseek(fd, 0, SEEK_SET);
	n = writev(fd, wv, 3);
	printf("writev: ret=%zd\n", n);
	char a[2], b[4], c[10];
	struct iovec rv[3] = { { a, 2 }, { b, 4 }, { c, 10 } };
	lseek(fd, 0, SEEK_SET);
	n = readv(fd, rv, 3);
	printf("readv: ret=%zd a=\"%.2s\" b=\"%.4s\" c=\"%.2s\"\n", n, a, b, c);
	errno = 0;
	n = syscall(SYS_writev, fd, wv, (long)-1);
	printf("writev-negative-count: ret=%zd errno=%s\n", n, errstr(errno));
	errno = 0;
	n = readv(fd, rv, 0);
	printf("readv-zero-count: ret=%zd\n", n);

	/* preadv / pwritev (arm64 preadv/pwritev with pos) */
	n = pwritev(fd, wv, 3, 2);
	printf("pwritev: ret=%zd\n", n);
	show_file("after-pwritev", fd);
	memset(a, 0, 2); memset(b, 0, 4);
	struct iovec rv2[2] = { { a, 2 }, { b, 4 } };
	n = preadv(fd, rv2, 2, 1);
	printf("preadv: ret=%zd a=\"%.2s\" b=\"%.4s\"\n", n, a, b);

	close(fd);

	/* O_APPEND: write always goes to end, lseek ignored for write */
	fd = openat(AT_FDCWD, "f1", O_WRONLY | O_APPEND);
	lseek(fd, 0, SEEK_SET);
	n = write(fd, "+app", 4);
	printf("append-write: ret=%zd offset=%ld\n", n, (long)lseek(fd, 0, SEEK_CUR));
	/* Linux quirk: pwrite on O_APPEND fd appends */
	n = pwrite(fd, "*", 1, 0);
	printf("append-pwrite: ret=%zd\n", n);
	close(fd);
	fd = open("f1", O_RDONLY);
	show_file("after-append", fd);
	errno = 0;
	n = write(fd, "x", 1);
	printf("write-on-rdonly: ret=%zd errno=%s\n", n, errstr(errno));
	close(fd);

	/* O_TRUNC */
	fd = open("f1", O_RDWR | O_TRUNC);
	printf("o_trunc: size=%ld\n", (long)lseek(fd, 0, SEEK_END));
	close(fd);
	unlink("f1");
}

static void t_dup(void)
{
	int fd = open("f2", O_RDWR | O_CREAT | O_TRUNC, 0600);
	int d = dup(fd);
	printf("dup: distinct=%s greater=%s\n", YN(d != fd), YN(d > fd));
	/* shared offset */
	(void)!write(fd, "12345", 5);
	printf("dup-shared-offset: ret=%ld\n", (long)lseek(d, 0, SEEK_CUR));
	/* dup3 to a specific number */
	int target = 100;
	int r = dup3(fd, target, O_CLOEXEC);
	printf("dup3: ret-equals-target=%s cloexec=%s\n", YN(r == target), YN(fcntl(target, F_GETFD) & FD_CLOEXEC));
	r = dup3(fd, target, 0);
	printf("dup3-replace: ret-equals-target=%s cloexec=%s\n", YN(r == target), YN(fcntl(target, F_GETFD) & FD_CLOEXEC));
	errno = 0;
	r = dup3(fd, fd, 0);
	printf("dup3-same: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = dup3(fd, 101, 0x4);
	printf("dup3-bad-flags: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = dup3(9999, 101, 0);
	printf("dup3-bad-oldfd: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = dup(9999);
	printf("dup-bad: ret=%d errno=%s\n", r, errstr(errno));
	/* dup2 via dup3 path with same fd returns fd (libc dup2 checks) */
	r = dup2(fd, fd);
	printf("dup2-same: ret-equals-fd=%s\n", YN(r == fd));
	printf("close-dup: ret=%d\n", close(d));
	printf("close-target: ret=%d\n", close(target));
	errno = 0;
	r = close(target);
	printf("close-twice: ret=%d errno=%s\n", r, errstr(errno));
	/* lowest fd reuse */
	close(fd);
	int n1 = open("f2", O_RDONLY);
	printf("lowest-fd-reused: %s\n", YN(n1 == fd));
	close(n1);
	unlink("f2");
}

static void t_pipe(void)
{
	int p[2];
	int r = pipe2(p, 0);
	printf("pipe2: ret=%d distinct=%s\n", r, YN(p[0] != p[1]));
	printf("pipe2: rd-flags-accmode-rdonly=%s wr-flags-accmode-wronly=%s\n",
	       YN((fcntl(p[0], F_GETFL) & O_ACCMODE) == O_RDONLY), YN((fcntl(p[1], F_GETFL) & O_ACCMODE) == O_WRONLY));
	ssize_t n = write(p[1], "pipedata", 8);
	char buf[16];
	ssize_t m = read(p[0], buf, 4);
	printf("pipe-rw: wrote=%zd read=%zd data=\"%.4s\"\n", n, m, buf);
	m = read(p[0], buf, 16);
	printf("pipe-rest: read=%zd data=\"%.*s\"\n", m, (int)m, buf);
	errno = 0;
	n = write(p[0], "x", 1);
	printf("pipe-write-readend: ret=%zd errno=%s\n", n, errstr(errno));
	errno = 0;
	printf("pipe-lseek: ret=%ld errno=%s\n", (long)lseek(p[0], 0, SEEK_CUR), errstr(errno));
	errno = 0;
	n = pread(p[0], buf, 1, 0);
	printf("pipe-pread: ret=%zd errno=%s\n", n, errstr(errno));
	close(p[1]);
	m = read(p[0], buf, 16);
	printf("pipe-eof: ret=%zd\n", m);
	close(p[0]);

	r = pipe2(p, O_NONBLOCK | O_CLOEXEC);
	printf("pipe2-flags: ret=%d nonblock=%s cloexec=%s\n", r, YN(fcntl(p[0], F_GETFL) & O_NONBLOCK),
	       YN(fcntl(p[1], F_GETFD) & FD_CLOEXEC));
	errno = 0;
	m = read(p[0], buf, 1);
	printf("pipe-nonblock-empty: ret=%zd errno=%s\n", m, errstr(errno));
	close(p[0]);
	close(p[1]);

	errno = 0;
	r = pipe2(p, 0x1);
	printf("pipe2-bad-flags: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_pipe2, (int *)8, 0);
	printf("pipe2-bad-ptr: ret=%d errno=%s\n", r, errstr(errno));
}

static void t_dev(void)
{
	char buf[32];
	int fd = open("/dev/zero", O_RDONLY);
	memset(buf, 0x11, sizeof(buf));
	ssize_t n = read(fd, buf, sizeof(buf));
	int z = 1;
	for (int i = 0; i < 32; i++)
		z &= buf[i] == 0;
	printf("dev-zero: ret=%zd zero=%s\n", n, YN(z));
	close(fd);
	fd = open("/dev/null", O_RDWR);
	printf("dev-null: write=%zd read=%zd\n", write(fd, "abc", 3), read(fd, buf, 3));
	close(fd);
}

int main(void)
{
	tmp_enter();
	t_rw();
	t_dup();
	t_pipe();
	t_dev();
	tmp_leave();
	done();
	return 0;
}
