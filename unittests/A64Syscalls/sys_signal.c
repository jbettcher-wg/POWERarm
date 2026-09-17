/* sys_signal: rt_sigaction / rt_sigprocmask / sigaltstack bookkeeping only.
 * No signal is ever generated or delivered here (see sys_signal_delivery.c
 * for that).  Uses the arm64 KERNEL struct sigaction via raw syscalls, plus
 * the libc wrappers.  The inherited mask/dispositions are saved first and
 * never printed. */
#include "a64sys.h"
#include <signal.h>

/* arm64 kernel ABI */
struct k_sigaction {
	void *handler;
	unsigned long flags;
	void *restorer;
	unsigned long mask; /* _NSIG/8 = 8 bytes */
};
#define K_SA_NOCLDSTOP 0x00000001UL
#define K_SA_NOCLDWAIT 0x00000002UL
#define K_SA_SIGINFO 0x00000004UL
#define K_SA_UNSUPPORTED 0x00000400UL
#define K_SA_EXPOSE_TAGBITS 0x00000800UL
#define K_SA_RESTORER 0x04000000UL
#define K_SA_ONSTACK 0x08000000UL
#define K_SA_RESTART 0x10000000UL
#define K_SA_NODEFER 0x40000000UL
#define K_SA_RESETHAND 0x80000000UL
static const struct flagname saflags[] = {
	{ K_SA_NOCLDSTOP, "SA_NOCLDSTOP" }, { K_SA_NOCLDWAIT, "SA_NOCLDWAIT" }, { K_SA_SIGINFO, "SA_SIGINFO" },
	{ K_SA_UNSUPPORTED, "SA_UNSUPPORTED" }, { K_SA_EXPOSE_TAGBITS, "SA_EXPOSE_TAGBITS" },
	{ K_SA_RESTORER, "SA_RESTORER" }, { K_SA_ONSTACK, "SA_ONSTACK" }, { K_SA_RESTART, "SA_RESTART" },
	{ K_SA_NODEFER, "SA_NODEFER" }, { K_SA_RESETHAND, "SA_RESETHAND" },
};

#define BIT(s) (1UL << ((s) - 1))

static void h1(int s) { (void)s; }
static void h2(int s, siginfo_t *i, void *c) { (void)s; (void)i; (void)c; }

static const char *hname(void *h)
{
	if (h == (void *)SIG_DFL) return "SIG_DFL";
	if (h == (void *)SIG_IGN) return "SIG_IGN";
	if (h == (void *)h1) return "h1";
	if (h == (void *)h2) return "h2";
	return "other";
}

static const char *sigsetstr(unsigned long m)
{
	static char buf[FLAGSTR_MAX];
	static const struct flagname t[] = {
		{ BIT(SIGHUP), "HUP" }, { BIT(SIGINT), "INT" }, { BIT(SIGQUIT), "QUIT" }, { BIT(SIGILL), "ILL" },
		{ BIT(SIGTRAP), "TRAP" }, { BIT(SIGABRT), "ABRT" }, { BIT(SIGBUS), "BUS" }, { BIT(SIGFPE), "FPE" },
		{ BIT(SIGKILL), "KILL" }, { BIT(SIGUSR1), "USR1" }, { BIT(SIGSEGV), "SEGV" }, { BIT(SIGUSR2), "USR2" },
		{ BIT(SIGPIPE), "PIPE" }, { BIT(SIGALRM), "ALRM" }, { BIT(SIGTERM), "TERM" }, { BIT(SIGSTKFLT), "STKFLT" },
		{ BIT(SIGCHLD), "CHLD" }, { BIT(SIGCONT), "CONT" }, { BIT(SIGSTOP), "STOP" }, { BIT(SIGTSTP), "TSTP" },
		{ BIT(SIGTTIN), "TTIN" }, { BIT(SIGTTOU), "TTOU" }, { BIT(SIGURG), "URG" }, { BIT(SIGXCPU), "XCPU" },
		{ BIT(SIGXFSZ), "XFSZ" }, { BIT(SIGVTALRM), "VTALRM" }, { BIT(SIGPROF), "PROF" },
		{ BIT(SIGWINCH), "WINCH" }, { BIT(SIGIO), "IO" }, { BIT(SIGPWR), "PWR" }, { BIT(SIGSYS), "SYS" },
		{ BIT(32), "32" }, { BIT(33), "33" }, { BIT(34), "RT34" }, { BIT(40), "RT40" }, { BIT(64), "RT64" },
	};
	return flagstr(buf, m, t, NELEM(t));
}

static long ksa(int sig, const struct k_sigaction *act, struct k_sigaction *old)
{
	return syscall(SYS_rt_sigaction, sig, act, old, 8);
}

static long kmask(int how, const unsigned long *set, unsigned long *old)
{
	return syscall(SYS_rt_sigprocmask, how, set, old, 8);
}

static void pr_sa(const char *tag, long r, const struct k_sigaction *sa)
{
	char b[FLAGSTR_MAX];
	printf("%s: ret=%ld handler=%s flags=%s mask=%s\n", tag, r, hname(sa->handler),
	       flagstr(b, sa->flags, saflags, NELEM(saflags)), sigsetstr(sa->mask));
}

static void t_sigaction(void)
{
	struct k_sigaction saved[65];
	for (int s = 1; s <= 64; s++)
		ksa(s, NULL, &saved[s]);

	struct k_sigaction act, old, q;
	static const unsigned long flagsets[] = {
		0,
		K_SA_RESTART,
		K_SA_SIGINFO,
		K_SA_ONSTACK,
		K_SA_NODEFER,
		K_SA_RESETHAND,
		K_SA_SIGINFO | K_SA_RESTART | K_SA_ONSTACK | K_SA_NODEFER | K_SA_RESETHAND,
		K_SA_NOCLDSTOP | K_SA_NOCLDWAIT,
	};
	for (size_t i = 0; i < NELEM(flagsets); i++) {
		memset(&act, 0, sizeof(act));
		act.handler = (flagsets[i] & K_SA_SIGINFO) ? (void *)h2 : (void *)h1;
		act.flags = flagsets[i];
		act.mask = BIT(SIGUSR2) | BIT(SIGTERM);
		long r = ksa(SIGUSR1, &act, NULL);
		memset(&q, 0xaa, sizeof(q));
		long r2 = ksa(SIGUSR1, NULL, &q);
		char tag[64];
		snprintf(tag, sizeof(tag), "rt_sigaction-set%zu", i);
		printf("%s: ret=%ld\n", tag, r);
		snprintf(tag, sizeof(tag), "rt_sigaction-get%zu", i);
		pr_sa(tag, r2, &q);
	}

	/* old-action returned by a set */
	memset(&act, 0, sizeof(act));
	act.handler = SIG_IGN;
	memset(&old, 0, sizeof(old));
	long r = ksa(SIGUSR1, &act, &old);
	pr_sa("rt_sigaction-old-on-set", r, &old);
	ksa(SIGUSR1, NULL, &q);
	pr_sa("rt_sigaction-sig_ign", 0, &q);
	act.handler = SIG_DFL;
	ksa(SIGUSR1, &act, NULL);
	ksa(SIGUSR1, NULL, &q);
	pr_sa("rt_sigaction-sig_dfl", 0, &q);

	/* KILL/STOP in sa_mask are silently removed */
	act.handler = h1;
	act.flags = K_SA_RESTART;
	act.mask = BIT(SIGKILL) | BIT(SIGSTOP) | BIT(SIGINT) | BIT(34) | BIT(64);
	r = ksa(SIGUSR2, &act, NULL);
	ksa(SIGUSR2, NULL, &q);
	pr_sa("rt_sigaction-mask-kill-stop-stripped", r, &q);

	/* SA_UNSUPPORTED is always cleared on read back; SA_EXPOSE_TAGBITS is
	 * an arm64 flag that the kernel keeps. */
	act.flags = K_SA_RESTART | K_SA_UNSUPPORTED | K_SA_EXPOSE_TAGBITS;
	act.mask = 0;
	r = ksa(SIGUSR2, &act, NULL);
	ksa(SIGUSR2, NULL, &q);
	pr_sa("rt_sigaction-unsupported-probe", r, &q);
	/* an unknown high bit is dropped too */
	act.flags = K_SA_RESTART | 0x00100000UL;
	r = ksa(SIGUSR2, &act, NULL);
	ksa(SIGUSR2, NULL, &q);
	pr_sa("rt_sigaction-unknown-bit", r, &q);

	/* RT signals */
	act.handler = h2;
	act.flags = K_SA_SIGINFO;
	act.mask = BIT(40);
	r = ksa(40, &act, NULL);
	ksa(40, NULL, &q);
	pr_sa("rt_sigaction-sig40", r, &q);
	r = ksa(64, &act, NULL);
	ksa(64, NULL, &q);
	pr_sa("rt_sigaction-sig64", r, &q);

	/* errors */
	act.handler = h1;
	act.flags = 0;
	act.mask = 0;
	errno = 0;
	printf("rt_sigaction-set-sigkill: ret=%ld errno=%s\n", ksa(SIGKILL, &act, NULL), errstr(errno));
	errno = 0;
	printf("rt_sigaction-set-sigstop: ret=%ld errno=%s\n", ksa(SIGSTOP, &act, NULL), errstr(errno));
	act.handler = SIG_IGN;
	errno = 0;
	printf("rt_sigaction-ign-sigkill: ret=%ld errno=%s\n", ksa(SIGKILL, &act, NULL), errstr(errno));
	r = ksa(SIGKILL, NULL, &q);
	pr_sa("rt_sigaction-query-sigkill", r, &q);
	errno = 0;
	printf("rt_sigaction-sig0: ret=%ld errno=%s\n", ksa(0, NULL, &q), errstr(errno));
	errno = 0;
	printf("rt_sigaction-sig65: ret=%ld errno=%s\n", ksa(65, NULL, &q), errstr(errno));
	errno = 0;
	printf("rt_sigaction-bad-sigsetsize: ret=%ld errno=%s\n", syscall(SYS_rt_sigaction, SIGUSR1, NULL, &q, 4),
	       errstr(errno));
	errno = 0;
	printf("rt_sigaction-bad-ptr: ret=%ld errno=%s\n", syscall(SYS_rt_sigaction, SIGUSR1, (void *)8, NULL, 8),
	       errstr(errno));
	errno = 0;
	printf("rt_sigaction-bad-oldptr: ret=%ld errno=%s\n", syscall(SYS_rt_sigaction, SIGUSR1, NULL, (void *)8, 8),
	       errstr(errno));

	/* libc sigaction: glibc/musl reserve 32/33; round-trip through libc */
	struct sigaction la, lq;
	memset(&la, 0, sizeof(la));
	la.sa_sigaction = h2;
	la.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK | SA_NODEFER | SA_RESETHAND;
	sigemptyset(&la.sa_mask);
	sigaddset(&la.sa_mask, SIGALRM);
	int lr = sigaction(SIGHUP, &la, NULL);
	memset(&lq, 0, sizeof(lq));
	sigaction(SIGHUP, NULL, &lq);
	printf("sigaction-libc: ret=%d handler-matches=%s siginfo=%s restart=%s onstack=%s nodefer=%s resethand=%s "
	       "mask-alrm=%s mask-int=%s\n",
	       lr, YN(lq.sa_sigaction == h2), YN(lq.sa_flags & SA_SIGINFO), YN(lq.sa_flags & SA_RESTART),
	       YN(lq.sa_flags & SA_ONSTACK), YN(lq.sa_flags & SA_NODEFER), YN(lq.sa_flags & SA_RESETHAND),
	       YN(sigismember(&lq.sa_mask, SIGALRM)), YN(sigismember(&lq.sa_mask, SIGINT)));
	ksa(SIGHUP, NULL, &q);
	/* libc may add SA_RESTORER on some ABIs; mask it for the printout */
	q.flags &= ~K_SA_RESTORER;
	pr_sa("sigaction-libc-kernel-view", 0, &q);
	errno = 0;
	printf("signal-libc-sigkill: err=%s errno=%s\n", YN(signal(SIGKILL, SIG_IGN) == SIG_ERR), errstr(errno));

	for (int s = 1; s <= 64; s++)
		if (s != SIGKILL && s != SIGSTOP)
			ksa(s, &saved[s], NULL);
}

static void t_sigprocmask(void)
{
	unsigned long orig, set, old, cur;
	kmask(SIG_SETMASK, NULL, &orig);

	set = 0;
	long r = kmask(SIG_SETMASK, &set, &old);
	kmask(SIG_BLOCK, NULL, &cur);
	printf("rt_sigprocmask-setmask-empty: ret=%ld cur=%s\n", r, sigsetstr(cur));

	set = BIT(SIGUSR1) | BIT(SIGINT) | BIT(40);
	r = kmask(SIG_BLOCK, &set, &old);
	kmask(SIG_BLOCK, NULL, &cur);
	printf("rt_sigprocmask-block: ret=%ld old=%s cur=%s\n", r, sigsetstr(old), sigsetstr(cur));

	set = BIT(SIGUSR2) | BIT(SIGTERM);
	r = kmask(SIG_BLOCK, &set, &old);
	kmask(SIG_BLOCK, NULL, &cur);
	printf("rt_sigprocmask-block-more: ret=%ld old=%s cur=%s\n", r, sigsetstr(old), sigsetstr(cur));

	set = BIT(SIGINT) | BIT(SIGTERM) | BIT(SIGHUP);
	r = kmask(SIG_UNBLOCK, &set, &old);
	kmask(SIG_BLOCK, NULL, &cur);
	printf("rt_sigprocmask-unblock: ret=%ld old=%s cur=%s\n", r, sigsetstr(old), sigsetstr(cur));

	set = BIT(SIGKILL) | BIT(SIGSTOP) | BIT(SIGPIPE) | BIT(64);
	r = kmask(SIG_SETMASK, &set, &old);
	kmask(SIG_BLOCK, NULL, &cur);
	printf("rt_sigprocmask-setmask-kill-stop: ret=%ld old=%s cur=%s\n", r, sigsetstr(old), sigsetstr(cur));

	set = ~0UL;
	r = kmask(SIG_SETMASK, &set, NULL);
	kmask(SIG_BLOCK, NULL, &cur);
	printf("rt_sigprocmask-setmask-all: ret=%ld cur=%s\n", r, sigsetstr(cur));

	/* query-only: how is ignored when set == NULL */
	r = kmask(12345, NULL, &cur);
	printf("rt_sigprocmask-query-bad-how: ret=%ld\n", r);
	errno = 0;
	set = 0;
	r = kmask(12345, &set, NULL);
	printf("rt_sigprocmask-bad-how: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_rt_sigprocmask, SIG_BLOCK, &set, NULL, 4);
	printf("rt_sigprocmask-bad-sigsetsize: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_rt_sigprocmask, SIG_BLOCK, (void *)8, NULL, 8);
	printf("rt_sigprocmask-bad-ptr: ret=%ld errno=%s\n", r, errstr(errno));

	/* libc sigprocmask */
	sigset_t ls, lo;
	sigemptyset(&ls);
	sigaddset(&ls, SIGWINCH);
	sigaddset(&ls, SIGCHLD);
	r = sigprocmask(SIG_SETMASK, &ls, NULL);
	sigprocmask(SIG_BLOCK, NULL, &lo);
	kmask(SIG_BLOCK, NULL, &cur);
	printf("sigprocmask-libc: ret=%ld winch=%s chld=%s usr1=%s kernel-view=%s\n", r, YN(sigismember(&lo, SIGWINCH)),
	       YN(sigismember(&lo, SIGCHLD)), YN(sigismember(&lo, SIGUSR1)), sigsetstr(cur));
	/* rt_sigpending with nothing pending */
	unsigned long pend = 0xdead;
	r = syscall(SYS_rt_sigpending, &pend, 8);
	printf("rt_sigpending: ret=%ld pending=%s\n", r, sigsetstr(pend));

	kmask(SIG_SETMASK, &orig, NULL);
}

static void t_altstack(void)
{
	stack_t ss, os;
	memset(&os, 0xff, sizeof(os));
	long r = syscall(SYS_sigaltstack, NULL, &os);
	printf("sigaltstack-query-initial: ret=%ld disabled=%s\n", r, YN(os.ss_flags & SS_DISABLE));
	size_t sz = 256 * 1024;
	char *stk = malloc(sz);
	ss.ss_sp = stk;
	ss.ss_size = sz;
	ss.ss_flags = 0;
	r = syscall(SYS_sigaltstack, &ss, NULL);
	memset(&os, 0, sizeof(os));
	syscall(SYS_sigaltstack, NULL, &os);
	printf("sigaltstack-set: ret=%ld sp-matches=%s size-matches=%s flags=%d\n", r, YN(os.ss_sp == stk),
	       YN(os.ss_size == sz), os.ss_flags);
	ss.ss_size = 1024;
	errno = 0;
	r = syscall(SYS_sigaltstack, &ss, NULL);
	printf("sigaltstack-too-small: ret=%ld errno=%s\n", r, errstr(errno));
	ss.ss_size = sz;
	ss.ss_flags = 0x1234;
	errno = 0;
	r = syscall(SYS_sigaltstack, &ss, NULL);
	printf("sigaltstack-bad-flags: ret=%ld errno=%s\n", r, errstr(errno));
	ss.ss_flags = SS_DISABLE;
	r = syscall(SYS_sigaltstack, &ss, &os);
	printf("sigaltstack-disable: ret=%ld old-sp-matches=%s\n", r, YN(os.ss_sp == stk));
	syscall(SYS_sigaltstack, NULL, &os);
	printf("sigaltstack-after-disable: disabled=%s\n", YN(os.ss_flags & SS_DISABLE));
	/* SS_DISABLE with a too-small size is accepted */
	ss.ss_size = 0;
	r = syscall(SYS_sigaltstack, &ss, NULL);
	printf("sigaltstack-disable-zero-size: ret=%ld\n", r);
	free(stk);
}

int main(void)
{
	t_sigaction();
	t_sigprocmask();
	t_altstack();
	done();
	return 0;
}
