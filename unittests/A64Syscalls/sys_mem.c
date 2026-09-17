/* sys_mem: brk, mmap, munmap, mprotect, mremap, madvise.
 * Never prints addresses or the page size; only alignment/relations.
 * Accessibility is probed with write()/read() on a pipe (EFAULT) instead of
 * touching memory, so no signal delivery is needed. */
#include "a64sys.h"
#include <fcntl.h>
#include <sys/mman.h>

static long ps;
static int pfd[2];

static const char *al(void *p) { return YN(((unsigned long)p & (ps - 1)) == 0); }

/* 1 = readable, 0 = EFAULT, -1 = other */
static int readable(void *p)
{
	char c;
	ssize_t r = write(pfd[1], p, 1);
	if (r == 1) {
		if (read(pfd[0], &c, 1) != 1)
			return -1;
		return 1;
	}
	return errno == EFAULT ? 0 : -1;
}

/* 1 = writable (kernel copy into it succeeded), 0 = EFAULT */
static int writable(void *p)
{
	char c = 'w';
	if (write(pfd[1], &c, 1) != 1)
		return -1;
	ssize_t r = read(pfd[0], p, 1);
	if (r == 1)
		return 1;
	if (errno == EFAULT) {
		/* drain the byte left in the pipe */
		if (read(pfd[0], &c, 1) != 1)
			return -1;
		return 0;
	}
	return -1;
}

static int allzero(const unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

static void t_brk(void)
{
	void *cur = (void *)syscall(SYS_brk, 0);
	printf("brk-query: nonzero=%s\n", YN(cur != NULL));
	void *want = (char *)cur + 4 * ps + 123;
	void *got = (void *)syscall(SYS_brk, want);
	printf("brk-grow: returns-requested=%s\n", YN(got == want));
	if (got == want) {
		memset(cur, 0xab, 4 * ps + 123);
		printf("brk-grow: writable=%s\n", YN(((unsigned char *)cur)[4 * ps + 122] == 0xab));
	}
	void *q = (void *)syscall(SYS_brk, 0);
	printf("brk-query2: equals-grown=%s\n", YN(q == want));
	got = (void *)syscall(SYS_brk, cur);
	printf("brk-shrink: returns-requested=%s\n", YN(got == cur));
	/* brk below the start of the data segment is refused: returns old brk */
	got = (void *)syscall(SYS_brk, (void *)ps);
	printf("brk-bogus-low: unchanged=%s\n", YN(got == cur));
}

static void t_mmap_anon(void)
{
	errno = 0;
	unsigned char *p = mmap(NULL, 3 * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		printf("mmap-anon: fail errno=%s\n", errstr(errno));
		return;
	}
	printf("mmap-anon: aligned=%s zero=%s\n", al(p), YN(allzero(p, 3 * ps)));
	memset(p, 0x5a, 3 * ps);
	printf("mmap-anon: readback=%s\n", YN(p[0] == 0x5a && p[3 * ps - 1] == 0x5a));

	/* MAP_FIXED over the middle page replaces it with fresh zero page */
	unsigned char *m = mmap(p + ps, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	printf("mmap-fixed-replace: same-addr=%s middle-zero=%s neighbours-kept=%s\n",
	       YN(m == p + ps), YN(allzero(p + ps, ps)), YN(p[0] == 0x5a && p[2 * ps] == 0x5a));

	/* MAP_FIXED_NOREPLACE over existing mapping */
	errno = 0;
	void *n = mmap(p + ps, ps, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	printf("mmap-fixed-noreplace-busy: failed=%s errno=%s\n", YN(n == MAP_FAILED), errstr(n == MAP_FAILED ? errno : 0));

	/* munmap the middle page, then NOREPLACE into the hole succeeds */
	printf("munmap-middle: ret=%d\n", munmap(p + ps, ps));
	printf("munmap-middle: hole-readable=%d left-readable=%d right-readable=%d\n",
	       readable(p + ps), readable(p), readable(p + 2 * ps));
	n = mmap(p + ps, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	printf("mmap-fixed-noreplace-hole: same-addr=%s\n", YN(n == p + ps));
	printf("munmap-all: ret=%d\n", munmap(p, 3 * ps));
	printf("munmap-all: readable=%d\n", readable(p));
	/* unmapping an already-unmapped range is fine */
	printf("munmap-again: ret=%d\n", munmap(p, 3 * ps));

	errno = 0;
	int r = munmap(p + 1, ps);
	printf("munmap-unaligned: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = munmap(p, 0);
	printf("munmap-zero-len: ret=%d errno=%s\n", r, errstr(errno));

	errno = 0;
	n = mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	printf("mmap-zero-len: failed=%s errno=%s\n", YN(n == MAP_FAILED), errstr(errno));
	errno = 0;
	n = mmap(NULL, ps, PROT_READ, MAP_PRIVATE | MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	printf("mmap-private-and-shared: failed=%s errno=%s\n", YN(n == MAP_FAILED), errstr(errno));
	errno = 0;
	n = mmap(NULL, ps, PROT_READ, MAP_PRIVATE, 9999, 0);
	printf("mmap-badfd: failed=%s errno=%s\n", YN(n == MAP_FAILED), errstr(errno));
	errno = 0;
	n = mmap(NULL, ps, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 1);
	if (n != MAP_FAILED)
		munmap(n, ps);
	printf("mmap-anon-unaligned-offset: failed=%s errno=%s\n", YN(n == MAP_FAILED), errstr(n == MAP_FAILED ? errno : 0));

	/* Odd length is rounded up to a whole page */
	p = mmap(NULL, ps + 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	printf("mmap-odd-len: aligned=%s second-page-writable=%d\n", al(p), writable(p + ps + 5));
	munmap(p, 2 * ps);

	/* Shared anonymous */
	p = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	printf("mmap-shared-anon: ok=%s zero=%s\n", YN(p != MAP_FAILED), YN(p != MAP_FAILED && allzero(p, ps)));
	if (p != MAP_FAILED)
		munmap(p, ps);
}

static void t_highaddr(void)
{
	static const int bits[] = { 47, 48, 52, 56 };
	for (size_t i = 0; i < NELEM(bits); i++) {
		void *a = (void *)(1UL << bits[i]);
		errno = 0;
		void *p = mmap(a, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		printf("mmap-fixed-2^%d: failed=%s errno=%s\n", bits[i], YN(p == MAP_FAILED),
		       errstr(p == MAP_FAILED ? errno : 0));
		if (p != MAP_FAILED)
			munmap(p, ps);
		errno = 0;
		p = mmap(a, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		printf("mmap-noreplace-2^%d: failed=%s errno=%s\n", bits[i], YN(p == MAP_FAILED),
		       errstr(p == MAP_FAILED ? errno : 0));
		if (p != MAP_FAILED)
			munmap(p, ps);
		p = mmap(a, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		printf("mmap-hint-2^%d: ok=%s below-limit=%s\n", bits[i], YN(p != MAP_FAILED),
		       YN(p != MAP_FAILED && (unsigned long)p + ps <= (1UL << 47)));
		if (p != MAP_FAILED)
			munmap(p, ps);
	}
	/* Just below the 47-bit limit */
	void *a = (void *)((1UL << 47) - 4 * ps);
	void *p = mmap(a, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	printf("mmap-noreplace-below-2^47: same-addr=%s writable=%d\n", YN(p == a), p == a ? writable(p) : -1);
	if (p != MAP_FAILED)
		munmap(p, ps);
}

static void t_file(void)
{
	int fd = open("mapfile", O_RDWR | O_CREAT | O_TRUNC, 0644);
	char *buf = malloc(2 * ps);
	for (long i = 0; i < 2 * ps; i++)
		buf[i] = 'A' + (i % 26);
	if (write(fd, buf, 2 * ps) != 2 * ps)
		printf("mapfile: short write\n");

	char *p = mmap(NULL, 2 * ps, PROT_READ, MAP_PRIVATE, fd, 0);
	printf("mmap-file-private-ro: aligned=%s content=%s\n", al(p), YN(p != MAP_FAILED && !memcmp(p, buf, 2 * ps)));
	printf("mmap-file-private-ro: writable=%d\n", writable(p));
	munmap(p, 2 * ps);

	p = mmap(NULL, ps, PROT_READ, MAP_PRIVATE, fd, ps);
	printf("mmap-file-offset: content=%s\n", YN(p != MAP_FAILED && !memcmp(p, buf + ps, ps)));
	munmap(p, ps);

	errno = 0;
	p = mmap(NULL, ps, PROT_READ, MAP_PRIVATE, fd, 1);
	printf("mmap-file-unaligned-offset: failed=%s errno=%s\n", YN(p == MAP_FAILED), errstr(errno));

	/* Private writable: COW, file untouched */
	p = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	p[0] = 'z';
	char c = 0;
	(void)!pread(fd, &c, 1, 0);
	printf("mmap-file-private-rw: mapping-changed=%s file-unchanged=%s\n", YN(p[0] == 'z'), YN(c == 'A'));
	/* MADV_DONTNEED on private file mapping reverts to file contents */
	int r = madvise(p, ps, MADV_DONTNEED);
	printf("madvise-dontneed-file-private: ret=%d reverted=%s\n", r, YN(p[0] == 'A'));
	munmap(p, 2 * ps);

	/* Shared: writes reach the file */
	p = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	p[ps + 3] = '#';
	c = 0;
	(void)!pread(fd, &c, 1, ps + 3);
	printf("mmap-file-shared: file-sees-write=%s\n", YN(c == '#'));
	/* and file writes reach the mapping */
	(void)!pwrite(fd, "!", 1, 7);
	printf("mmap-file-shared: mapping-sees-pwrite=%s\n", YN(p[7] == '!'));
	printf("msync: ret=%d\n", msync(p, 2 * ps, MS_SYNC));
	munmap(p, 2 * ps);

	int rofd = open("mapfile", O_RDONLY);
	errno = 0;
	p = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_SHARED, rofd, 0);
	printf("mmap-shared-rw-on-rdonly-fd: failed=%s errno=%s\n", YN(p == MAP_FAILED), errstr(errno));
	p = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE, rofd, 0);
	printf("mmap-private-rw-on-rdonly-fd: ok=%s\n", YN(p != MAP_FAILED));
	if (p != MAP_FAILED)
		munmap(p, ps);
	close(rofd);

	int wofd = open("mapfile", O_WRONLY);
	errno = 0;
	p = mmap(NULL, ps, PROT_READ, MAP_PRIVATE, wofd, 0);
	printf("mmap-on-wronly-fd: failed=%s errno=%s\n", YN(p == MAP_FAILED), errstr(errno));
	close(wofd);

	int dfd = open(".", O_RDONLY | O_DIRECTORY);
	errno = 0;
	p = mmap(NULL, ps, PROT_READ, MAP_PRIVATE, dfd, 0);
	printf("mmap-directory: failed=%s errno=%s\n", YN(p == MAP_FAILED), errstr(errno));
	close(dfd);

	close(fd);
	free(buf);
	unlink("mapfile");
}

static void t_mprotect(void)
{
	char *p = mmap(NULL, 3 * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memset(p, 'q', 3 * ps);
	int r = mprotect(p + ps, ps, PROT_READ);
	printf("mprotect-ro: ret=%d readable=%d writable=%d left-writable=%d\n", r, readable(p + ps), writable(p + ps),
	       writable(p));
	r = mprotect(p + ps, ps, PROT_NONE);
	printf("mprotect-none: ret=%d readable=%d\n", r, readable(p + ps));
	r = mprotect(p + ps, ps, PROT_READ | PROT_WRITE);
	printf("mprotect-rw: ret=%d writable=%d data-kept=%s\n", r, writable(p + ps + 1), YN(p[ps] == 'q'));
	errno = 0;
	r = mprotect(p + 1, ps, PROT_READ);
	printf("mprotect-unaligned: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = mprotect(p, ps, 0x1000);
	printf("mprotect-bad-prot: ret=%d errno=%s\n", r, errstr(errno));
	munmap(p, 3 * ps);
	errno = 0;
	r = mprotect(p, ps, PROT_READ);
	printf("mprotect-unmapped: ret=%d errno=%s\n", r, errstr(errno));
}

static void t_mremap(void)
{
	/* Reserve 4 pages, unmap last 3 so an in-place grow is possible. */
	char *p = mmap(NULL, 4 * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	munmap(p + ps, 3 * ps);
	p[0] = 'r';
	char *q = mremap(p, ps, 3 * ps, 0);
	printf("mremap-grow-inplace: same-addr=%s data-kept=%s new-zero=%s\n", YN(q == p), YN(q != MAP_FAILED && q[0] == 'r'),
	       YN(q != MAP_FAILED && allzero((unsigned char *)q + ps, 2 * ps)));
	/* Shrink */
	q = mremap(p, 3 * ps, ps, 0);
	printf("mremap-shrink: same-addr=%s tail-readable=%d\n", YN(q == p), readable(p + ps));

	/* Block growth with a neighbour mapping, grow without MAYMOVE -> ENOMEM */
	char *blk = mmap(p + ps, ps, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	printf("mremap-setup-blocker: ok=%s\n", YN(blk == p + ps));
	errno = 0;
	q = mremap(p, ps, 2 * ps, 0);
	printf("mremap-grow-blocked: failed=%s errno=%s\n", YN(q == MAP_FAILED), errstr(errno));
	q = mremap(p, ps, 2 * ps, MREMAP_MAYMOVE);
	printf("mremap-grow-maymove: moved=%s aligned=%s data-kept=%s old-readable=%d\n", YN(q != p && q != MAP_FAILED),
	       al(q), YN(q != MAP_FAILED && q[0] == 'r'), readable(p));
	munmap(blk, ps);

	/* MREMAP_FIXED to a chosen address */
	char *dst = mmap(NULL, 2 * ps, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	char *f = mremap(q, 2 * ps, 2 * ps, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	printf("mremap-fixed: at-dst=%s data-kept=%s old-readable=%d\n", YN(f == dst), YN(f == dst && f[0] == 'r'),
	       readable(q));
	errno = 0;
	char *g = mremap(f, 2 * ps, 2 * ps, MREMAP_FIXED, dst + 8 * ps);
	printf("mremap-fixed-without-maymove: failed=%s errno=%s\n", YN(g == MAP_FAILED), errstr(errno));
	errno = 0;
	g = mremap(f + 1, ps, ps, 0);
	printf("mremap-unaligned: failed=%s errno=%s\n", YN(g == MAP_FAILED), errstr(errno));
	/* MREMAP_DONTUNMAP is deliberately not tested: it returns EINVAL on some
	 * kernel configs (seen on the Pi 5 6.18 kernel) so it is host-dependent. */
	munmap(f, 2 * ps);
}

static void t_madvise(void)
{
	unsigned char *p = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memset(p, 0x77, 2 * ps);
	int r = madvise(p, ps, MADV_DONTNEED);
	printf("madvise-dontneed-anon: ret=%d zeroed=%s other-kept=%s\n", r, YN(allzero(p, ps)), YN(p[ps] == 0x77));
	printf("madvise-willneed: ret=%d\n", madvise(p, 2 * ps, MADV_WILLNEED));
	printf("madvise-normal: ret=%d\n", madvise(p, 2 * ps, MADV_NORMAL));
	printf("madvise-random: ret=%d\n", madvise(p, 2 * ps, MADV_RANDOM));
	printf("madvise-sequential: ret=%d\n", madvise(p, 2 * ps, MADV_SEQUENTIAL));
	errno = 0;
	r = madvise(p + 1, ps, MADV_NORMAL);
	printf("madvise-unaligned: ret=%d errno=%s\n", r, errstr(errno));
	errno = 0;
	r = madvise(p, ps, 12345);
	printf("madvise-bad-advice: ret=%d errno=%s\n", r, errstr(errno));
	munmap(p, 2 * ps);
	errno = 0;
	r = madvise(p, ps, MADV_NORMAL);
	printf("madvise-unmapped: ret=%d errno=%s\n", r, errstr(errno));
}

int main(void)
{
	ps = sysconf(_SC_PAGESIZE);
	printf("pagesize: power-of-two=%s at-least-4k=%s\n", YN(ps > 0 && (ps & (ps - 1)) == 0), YN(ps >= 4096));
	if (pipe(pfd) != 0) {
		perror("pipe");
		return 1;
	}
	tmp_enter();
	t_brk();
	t_mmap_anon();
	t_highaddr();
	t_file();
	t_mprotect();
	t_mremap();
	t_madvise();
	tmp_leave();
	done();
	return 0;
}
