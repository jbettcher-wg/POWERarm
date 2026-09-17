/* Shared helpers for the A64Syscalls test corpus.
 *
 * Every program prints normalized, deterministic "name: key=value" lines so
 * that native aarch64 output can be diffed byte-for-byte against the same
 * static binary run under POWERarm.  Nothing here may print addresses, page
 * sizes, ids, inode numbers, timestamps or host-specific strings.
 */
#ifndef A64SYS_H
#define A64SYS_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

static const char *errname(int e)
{
	switch (e) {
	case 0: return "OK";
	case EPERM: return "EPERM";
	case ENOENT: return "ENOENT";
	case ESRCH: return "ESRCH";
	case EINTR: return "EINTR";
	case EIO: return "EIO";
	case ENXIO: return "ENXIO";
	case E2BIG: return "E2BIG";
	case ENOEXEC: return "ENOEXEC";
	case EBADF: return "EBADF";
	case ECHILD: return "ECHILD";
	case EAGAIN: return "EAGAIN";
	case ENOMEM: return "ENOMEM";
	case EACCES: return "EACCES";
	case EFAULT: return "EFAULT";
	case EBUSY: return "EBUSY";
	case EEXIST: return "EEXIST";
	case EXDEV: return "EXDEV";
	case ENODEV: return "ENODEV";
	case ENOTDIR: return "ENOTDIR";
	case EISDIR: return "EISDIR";
	case EINVAL: return "EINVAL";
	case EMFILE: return "EMFILE";
	case ENOSPC: return "ENOSPC";
	case ESPIPE: return "ESPIPE";
	case EROFS: return "EROFS";
	case EMLINK: return "EMLINK";
	case EPIPE: return "EPIPE";
	case EDOM: return "EDOM";
	case ERANGE: return "ERANGE";
	case ENAMETOOLONG: return "ENAMETOOLONG";
	case ENOSYS: return "ENOSYS";
	case ENOTEMPTY: return "ENOTEMPTY";
	case ELOOP: return "ELOOP";
	case ENOTTY: return "ENOTTY";
	case EOPNOTSUPP: return "EOPNOTSUPP";
	case EOVERFLOW: return "EOVERFLOW";
	case ETIMEDOUT: return "ETIMEDOUT";
	case EFBIG: return "EFBIG";
	default: return "E?";
	}
}

/* "ENOENT(2)" */
static inline const char *errstr(int e)
{
	static char buf[4][32];
	static int idx;
	char *b = buf[idx++ & 3];
	snprintf(b, sizeof(buf[0]), "%s(%d)", errname(e), e);
	return b;
}

/* Print "name: ret=<r> errno=<E>" for calls whose return is deterministic
 * (typically 0 or -1). */
static inline void pr_ret(const char *name, long r, int e)
{
	if (r < 0)
		printf("%s: ret=%ld errno=%s\n", name, r, errstr(e));
	else
		printf("%s: ret=%ld\n", name, r);
}

/* Print only success/failure (for calls whose return value is not
 * deterministic, e.g. fds). */
static inline void pr_ok(const char *name, long r, int e)
{
	if (r < 0)
		printf("%s: fail errno=%s\n", name, errstr(e));
	else
		printf("%s: ok\n", name);
}

static inline const char *YN(int c) { return c ? "yes" : "no"; }

/* Fresh temp directory under /tmp; chdir into it so all paths printed are
 * relative. */
static char a64_tmpdir[64];
static inline void tmp_enter(void)
{
	strcpy(a64_tmpdir, "/tmp/a64sys.XXXXXX");
	if (!mkdtemp(a64_tmpdir)) {
		perror("mkdtemp");
		exit(2);
	}
	if (chdir(a64_tmpdir) != 0) {
		perror("chdir");
		exit(2);
	}
}

#include <ftw.h>
static int a64_rm_cb(const char *p, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb; (void)flag; (void)ftw;
	return remove(p);
}
static inline void tmp_leave(void)
{
	if (chdir("/") != 0)
		perror("chdir /");
	nftw(a64_tmpdir, a64_rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

#define FLAGSTR_MAX 512
struct flagname { unsigned long v; const char *n; };

/* Render bits of v symbolically, in table order; leftover bits as hex. */
static inline const char *flagstr(char *buf, unsigned long v, const struct flagname *t, size_t nt)
{
	size_t len = 0;
	buf[0] = 0;
	for (size_t i = 0; i < nt; i++) {
		if (t[i].v && (v & t[i].v) == t[i].v) {
			len += snprintf(buf + len, FLAGSTR_MAX - len, "%s%s", len ? "|" : "", t[i].n);
			v &= ~t[i].v;
		}
	}
	if (v)
		len += snprintf(buf + len, FLAGSTR_MAX - len, "%s0x%lx", len ? "|" : "", v);
	if (!len)
		snprintf(buf, FLAGSTR_MAX, "0");
	return buf;
}
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

static inline void done(void)
{
	printf("done\n");
	fflush(stdout);
}

#endif
