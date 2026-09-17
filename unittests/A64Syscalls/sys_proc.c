/* sys_proc: getpid/getppid/gettid, get*uid/get*gid, umask, prlimit64,
 * set_tid_address, set_robust_list, rseq, getrandom, sysinfo, uname.
 * Ids and sysinfo values are printed only as relations. */
#include "a64sys.h"
#include <sys/auxv.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/random.h>
#include <sys/types.h>

#ifndef SYS_rseq
#define SYS_rseq 293
#endif

struct k_robust_list_head {
	void *list;
	long futex_offset;
	void *list_op_pending;
};

struct k_rlimit64 { unsigned long long cur, max; };

static void t_ids(void)
{
	pid_t pid = syscall(SYS_getpid);
	pid_t ppid = syscall(SYS_getppid);
	pid_t tid = syscall(SYS_gettid);
	printf("getpid: positive=%s\n", YN(pid > 0));
	printf("getppid: positive-or-zero=%s ne-pid=%s\n", YN(ppid >= 0), YN(ppid != pid));
	printf("gettid: equals-pid=%s\n", YN(tid == pid));
	printf("getpid-libc: equals-raw=%s\n", YN(getpid() == pid));

	uid_t uid = syscall(SYS_getuid), euid = syscall(SYS_geteuid);
	gid_t gid = syscall(SYS_getgid), egid = syscall(SYS_getegid);
	printf("uid: getuid==geteuid=%s\n", YN(uid == euid));
	printf("gid: getgid==getegid=%s\n", YN(gid == egid));
	uid_t r, e, s;
	gid_t rg, eg, sg;
	int ret = getresuid(&r, &e, &s);
	printf("getresuid: ret=%d real-matches=%s eff-matches=%s saved==eff=%s\n", ret, YN(r == uid), YN(e == euid),
	       YN(s == euid));
	ret = getresgid(&rg, &eg, &sg);
	printf("getresgid: ret=%d real-matches=%s eff-matches=%s saved==eff=%s\n", ret, YN(rg == gid), YN(eg == egid),
	       YN(sg == egid));
	int ng = getgroups(0, NULL);
	if (ng < 0)
		ng = 0;
	printf("getgroups-count: nonneg=%s\n", YN(ng >= 0));
	gid_t *groups = malloc(sizeof(gid_t) * (ng + 1));
	int ng2 = getgroups(ng, groups);
	printf("getgroups: consistent=%s\n", YN(ng2 == ng));
	if (ng > 0) {
		errno = 0;
		long rr = syscall(SYS_getgroups, ng > 1 ? 1 : 0, groups);
		/* size 0 always returns count; only test EINVAL when there are >1 groups */
		printf("getgroups-too-small: einval-or-single=%s\n", YN((ng > 1 && rr == -1 && errno == EINVAL) || ng <= 1));
	} else {
		printf("getgroups-too-small: einval-or-single=yes\n");
	}
	free(groups);
	/* setuid to own uid is always permitted */
	printf("setresuid-noop: ret=%ld\n", syscall(SYS_setresuid, -1, -1, -1));
	printf("setregid-noop: ret=%ld\n", syscall(SYS_setregid, -1, -1));

	pid_t pg = getpgid(0);
	pid_t sid = getsid(0);
	printf("getpgid: positive=%s equals-getpgrp=%s\n", YN(pg > 0), YN(pg == getpgrp()));
	/* 0 when the session is init's own (a process started straight from an
	 * initramfs init, e.g. the 4K KVM test guest), otherwise positive */
	printf("getsid: nonneg=%s\n", YN(sid >= 0));
	errno = 0;
	printf("getpgid-bogus: ret=%d errno=%s\n", getpgid(0x7ffffff0), errstr(errno));
	errno = 0;
	printf("getsid-bogus: ret=%d errno=%s\n", getsid(0x7ffffff0), errstr(errno));
}

static void t_umask(void)
{
	mode_t old = umask(027);
	mode_t back = umask(0777);
	mode_t back2 = umask(01777); /* extra bits are masked off */
	mode_t back3 = umask(old);
	printf("umask: roundtrip-027=%04o roundtrip-0777=%04o masked-01777=%04o old-restored=%s\n", (unsigned)back,
	       (unsigned)back2, (unsigned)back3, YN(umask(old) == old));
}

static void t_prlimit(void)
{
	struct k_rlimit64 cur, nw, old;
	long r = syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, NULL, &cur);
	printf("prlimit64-get-nofile: ret=%ld soft<=hard=%s soft>=3=%s\n", r, YN(cur.cur <= cur.max), YN(cur.cur >= 3));
	nw.cur = cur.cur > 64 ? 64 : cur.cur;
	nw.max = cur.max;
	r = syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &nw, &old);
	printf("prlimit64-set-nofile: ret=%ld old-matches=%s\n", r, YN(old.cur == cur.cur && old.max == cur.max));
	struct k_rlimit64 chk;
	syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, NULL, &chk);
	printf("prlimit64-readback: soft=%llu hard-unchanged=%s\n", chk.cur, YN(chk.max == cur.max));
	struct rlimit rl;
	getrlimit(RLIMIT_NOFILE, &rl);
	printf("getrlimit-libc: soft=%llu\n", (unsigned long long)rl.rlim_cur);
	/* open beyond the limit */
	int fds[80], n = 0, err = 0;
	for (; n < 80; n++) {
		fds[n] = dup(0);
		if (fds[n] < 0) {
			err = errno;
			break;
		}
	}
	printf("dup-past-nofile: stopped-before-80=%s errno=%s\n", YN(n < 80), errstr(err));
	for (int i = 0; i < n; i++)
		close(fds[i]);
	/* raise soft above hard -> EINVAL */
	struct k_rlimit64 bad = { cur.max == ~0ULL ? ~0ULL : cur.max + 1, cur.max };
	errno = 0;
	r = syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &bad, NULL);
	printf("prlimit64-soft-above-hard: einval-or-infinite=%s\n", YN((r == -1 && errno == EINVAL) || cur.max == ~0ULL));
	r = syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &cur, NULL);
	printf("prlimit64-restore: ret=%ld\n", r);
	errno = 0;
	r = syscall(SYS_prlimit64, 0, 999, NULL, &chk);
	printf("prlimit64-bad-resource: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_prlimit64, 0x7ffffff0, RLIMIT_NOFILE, NULL, &chk);
	printf("prlimit64-bad-pid: ret=%ld errno=%s\n", r, errstr(errno));
	/* RLIMIT_STACK / AS readable */
	r = syscall(SYS_prlimit64, 0, RLIMIT_STACK, NULL, &chk);
	printf("prlimit64-get-stack: ret=%ld soft<=hard=%s\n", r, YN(chk.cur <= chk.max));
	/* lower RLIMIT_CORE to 0 and read back */
	struct k_rlimit64 core0 = { 0, 0 }, corechk;
	syscall(SYS_prlimit64, 0, RLIMIT_CORE, NULL, &corechk);
	core0.max = corechk.max;
	r = syscall(SYS_prlimit64, 0, RLIMIT_CORE, &core0, NULL);
	syscall(SYS_prlimit64, 0, RLIMIT_CORE, NULL, &corechk);
	printf("prlimit64-core-zero: ret=%ld soft=%llu\n", r, corechk.cur);
}

static void t_threadish(void)
{
	static int tidslot;
	long r = syscall(SYS_set_tid_address, &tidslot);
	printf("set_tid_address: returns-gettid=%s\n", YN(r == syscall(SYS_gettid)));

	static struct k_robust_list_head head;
	head.list = &head;
	r = syscall(SYS_set_robust_list, &head, sizeof(head));
	printf("set_robust_list: ret=%ld\n", r);
	errno = 0;
	r = syscall(SYS_set_robust_list, &head, sizeof(head) + 1);
	printf("set_robust_list-bad-len: ret=%ld errno=%s\n", r, errstr(errno));
	void *gh = NULL;
	size_t glen = 0;
	r = syscall(SYS_get_robust_list, 0, &gh, &glen);
	printf("get_robust_list: ret=%ld head-matches=%s len=%zu\n", r, YN(gh == &head), glen);

	/* rseq: glibc may already have registered one; either way, a fresh
	 * registration attempt is EBUSY (already registered), EINVAL, ENOSYS or
	 * success.  Print a normalized verdict. */
	static struct {
		unsigned int cpu_id_start, cpu_id;
		unsigned long long rseq_cs;
		unsigned int flags;
		unsigned int node_id, mm_cid;
		char pad[12];
	} rs;
	errno = 0;
	r = syscall(SYS_rseq, &rs, 32, 0, 0x53053053);
	int e = errno;
	printf("rseq: ok-or-enosys=%s\n", YN(r == 0 || e == ENOSYS || e == EBUSY || e == EINVAL || e == EPERM));
	errno = 0;
	r = syscall(SYS_rseq, &rs, 7, 0, 0x53053053);
	printf("rseq-bad-len: fails=%s\n", YN(r == -1));
}

static void t_random(void)
{
	unsigned char b1[64], b2[64];
	memset(b1, 0, sizeof(b1));
	memset(b2, 0, sizeof(b2));
	long r = syscall(SYS_getrandom, b1, sizeof(b1), 0);
	printf("getrandom: ret=%ld\n", r);
	r = syscall(SYS_getrandom, b2, sizeof(b2), GRND_NONBLOCK);
	printf("getrandom-nonblock: ret=%ld\n", r);
	printf("getrandom: two-calls-differ=%s\n", YN(memcmp(b1, b2, sizeof(b1)) != 0));
	int nz = 0;
	for (int i = 0; i < 64; i++)
		nz += b1[i] != 0;
	printf("getrandom: mostly-nonzero=%s\n", YN(nz > 32));
	r = syscall(SYS_getrandom, b1, 0, 0);
	printf("getrandom-zero-len: ret=%ld\n", r);
	r = syscall(SYS_getrandom, b1, 1, GRND_RANDOM | GRND_NONBLOCK);
	printf("getrandom-random-nonblock: one-or-eagain=%s\n", YN(r == 1 || (r == -1 && errno == EAGAIN)));
	errno = 0;
	r = syscall(SYS_getrandom, b1, sizeof(b1), 0x40);
	printf("getrandom-bad-flags: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_getrandom, (void *)8, 16, 0);
	printf("getrandom-bad-ptr: ret=%ld errno=%s\n", r, errstr(errno));
	r = getentropy(b1, 32);
	printf("getentropy: ret=%ld\n", r);
}

static void t_sysinfo(void)
{
	struct sysinfo si;
	memset(&si, 0, sizeof(si));
	long r = syscall(SYS_sysinfo, &si);
	printf("sysinfo: ret=%ld mem_unit>0=%s uptime>0=%s totalram>0=%s freeram<=totalram=%s procs>0=%s\n", r,
	       YN(si.mem_unit > 0), YN(si.uptime > 0), YN(si.totalram > 0), YN(si.freeram <= si.totalram),
	       YN(si.procs > 0));
	errno = 0;
	r = syscall(SYS_sysinfo, (void *)8);
	printf("sysinfo-bad-ptr: ret=%ld errno=%s\n", r, errstr(errno));
	long np = sysconf(_SC_NPROCESSORS_ONLN);
	printf("nprocs-online: positive=%s\n", YN(np > 0));
}

static void t_uname(void)
{
	struct utsname u;
	memset(&u, 0, sizeof(u));
	long r = syscall(SYS_uname, &u);
	printf("uname: ret=%ld sysname=%s machine=%s\n", r, u.sysname, u.machine);
	printf("uname: nodename-nonempty=%s release-nonempty=%s version-nonempty=%s\n", YN(u.nodename[0] != 0),
	       YN(u.release[0] != 0), YN(u.version[0] != 0));
	int maj = 0, min = 0;
	sscanf(u.release, "%d.%d", &maj, &min);
	printf("uname: release-at-least-4.x=%s\n", YN(maj >= 4));
	errno = 0;
	r = syscall(SYS_uname, (void *)8);
	printf("uname-bad-ptr: ret=%ld errno=%s\n", r, errstr(errno));
	char host[256];
	r = gethostname(host, sizeof(host));
	printf("gethostname: ret=%ld matches-uname=%s\n", r, YN(!strcmp(host, u.nodename)));
}

/* auxv details a dynamic loader relies on. */
static void t_auxv(void)
{
	const char *plat = (const char *)getauxval(AT_PLATFORM);
	printf("auxv: platform=%s execfn-set=%s random-set=%s pagesz-pow2=%s\n", plat ? plat : "(null)",
	       YN(getauxval(AT_EXECFN) != 0), YN(getauxval(AT_RANDOM) != 0),
	       YN(getauxval(AT_PAGESZ) && !(getauxval(AT_PAGESZ) & (getauxval(AT_PAGESZ) - 1))));
}

int main(void)
{
	t_ids();
	t_umask();
	t_prlimit();
	t_threadish();
	t_random();
	t_sysinfo();
	t_uname();
	t_auxv();
	done();
	return 0;
}
