/* sys_signal_delivery: tests that REQUIRE real signal delivery (signal
 * frames, sigreturn, SA_* delivery semantics, pending signals, sigtimedwait,
 * synchronous faults).  EXPECTED TO FAIL under POWERarm until guest signal
 * frames are implemented.  Kept separate from sys_signal.c on purpose. */
#include "a64sys.h"
#include <setjmp.h>
#include <stdint.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#define BIT(s) (1UL << ((s) - 1))

static volatile sig_atomic_t hits, last_sig, last_code, onstack_ok, handler_mask_ok;
static volatile sig_atomic_t si_pid_ok;
static char *alt_lo, *alt_hi;
static sigjmp_buf jb;

static void h_basic(int s) { hits++; last_sig = s; }

static void h_info(int s, siginfo_t *si, void *uc)
{
	(void)uc;
	hits++;
	last_sig = s;
	last_code = si->si_code;
	si_pid_ok = si->si_pid == getpid();
}

static void h_mask(int s, siginfo_t *si, void *uc)
{
	(void)s; (void)si; (void)uc;
	unsigned long cur = 0;
	syscall(SYS_rt_sigprocmask, SIG_BLOCK, NULL, &cur, 8);
	/* sa_mask (USR2) plus the signal itself unless SA_NODEFER */
	handler_mask_ok = (int)(((cur & BIT(SIGUSR2)) ? 1 : 0) | ((cur & BIT(SIGUSR1)) ? 2 : 0));
	hits++;
}

static void h_onstack(int s)
{
	char local;
	(void)s;
	onstack_ok = &local >= alt_lo && &local < alt_hi;
	hits++;
}

static void h_segv(int s, siginfo_t *si, void *uc)
{
	(void)uc;
	last_sig = s;
	last_code = si->si_code;
	si_pid_ok = si->si_addr == (void *)16;
	siglongjmp(jb, 1);
}

static void h_nop(int s) { (void)s; hits++; }

static void setact(int sig, void *h, int flags, const sigset_t *mask)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	if (flags & SA_SIGINFO)
		sa.sa_sigaction = h;
	else
		sa.sa_handler = h;
	sa.sa_flags = flags;
	if (mask)
		sa.sa_mask = *mask;
	else
		sigemptyset(&sa.sa_mask);
	sigaction(sig, &sa, NULL);
}

static const char *codename(int c)
{
	switch (c) {
	case SI_USER: return "SI_USER";
	case SI_TKILL: return "SI_TKILL";
	case SI_QUEUE: return "SI_QUEUE";
	case SEGV_MAPERR: return "SEGV_MAPERR";
	default: return "other";
	}
}

int main(void)
{
	sigset_t empty;
	sigemptyset(&empty);
	sigprocmask(SIG_SETMASK, &empty, NULL);

	/* basic raise */
	setact(SIGUSR1, h_basic, 0, NULL);
	hits = 0;
	int r = raise(SIGUSR1);
	printf("raise-basic: ret=%d hits=%d sig-is-usr1=%s\n", r, (int)hits, YN(last_sig == SIGUSR1));

	/* SA_SIGINFO with kill() -> SI_USER, tgkill -> SI_TKILL */
	setact(SIGUSR1, h_info, SA_SIGINFO, NULL);
	hits = 0;
	kill(getpid(), SIGUSR1);
	printf("kill-siginfo: hits=%d code=%s pid-matches=%s\n", (int)hits, codename(last_code), YN(si_pid_ok));
	hits = 0;
	syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1);
	printf("tgkill-siginfo: hits=%d code=%s\n", (int)hits, codename(last_code));
	hits = 0;
	union sigval sv = { .sival_int = 7 };
	sigqueue(getpid(), SIGUSR1, sv);
	printf("sigqueue-siginfo: hits=%d code=%s\n", (int)hits, codename(last_code));

	/* handler mask: sa_mask + the signal itself; SA_NODEFER omits the signal */
	sigset_t m2;
	sigemptyset(&m2);
	sigaddset(&m2, SIGUSR2);
	setact(SIGUSR1, h_mask, SA_SIGINFO, &m2);
	raise(SIGUSR1);
	printf("handler-mask: usr2-blocked=%s usr1-blocked=%s\n", YN(handler_mask_ok & 1), YN(handler_mask_ok & 2));
	setact(SIGUSR1, h_mask, SA_SIGINFO | SA_NODEFER, &m2);
	raise(SIGUSR1);
	printf("handler-mask-nodefer: usr2-blocked=%s usr1-blocked=%s\n", YN(handler_mask_ok & 1), YN(handler_mask_ok & 2));
	sigset_t after;
	sigprocmask(SIG_BLOCK, NULL, &after);
	printf("mask-restored-after-handler: usr1=%s usr2=%s\n", YN(sigismember(&after, SIGUSR1)),
	       YN(sigismember(&after, SIGUSR2)));

	/* SA_RESETHAND */
	setact(SIGUSR2, h_basic, SA_RESETHAND, NULL);
	hits = 0;
	raise(SIGUSR2);
	struct sigaction q;
	sigaction(SIGUSR2, NULL, &q);
	printf("resethand: hits=%d reset-to-dfl=%s\n", (int)hits, YN(q.sa_handler == SIG_DFL));

	/* SA_ONSTACK */
	size_t sz = 256 * 1024;
	char *stk = malloc(sz);
	alt_lo = stk;
	alt_hi = stk + sz;
	stack_t ss = { .ss_sp = stk, .ss_size = sz, .ss_flags = 0 };
	sigaltstack(&ss, NULL);
	setact(SIGUSR2, h_onstack, SA_ONSTACK, NULL);
	onstack_ok = 0;
	raise(SIGUSR2);
	printf("onstack: ran-on-altstack=%s\n", YN(onstack_ok));
	setact(SIGUSR2, h_onstack, 0, NULL);
	onstack_ok = 1;
	raise(SIGUSR2);
	printf("no-onstack: ran-on-altstack=%s\n", YN(onstack_ok));
	ss.ss_flags = SS_DISABLE;
	sigaltstack(&ss, NULL);

	/* pending while blocked, delivered on unblock */
	setact(SIGUSR1, h_basic, 0, NULL);
	sigset_t b;
	sigemptyset(&b);
	sigaddset(&b, SIGUSR1);
	sigprocmask(SIG_BLOCK, &b, NULL);
	hits = 0;
	raise(SIGUSR1);
	raise(SIGUSR1); /* standard signals don't queue */
	sigset_t pend;
	sigpending(&pend);
	printf("blocked: hits=%d pending-usr1=%s\n", (int)hits, YN(sigismember(&pend, SIGUSR1)));
	sigprocmask(SIG_UNBLOCK, &b, NULL);
	printf("unblocked: hits=%d\n", (int)hits);

	/* RT signals queue */
	setact(SIGRTMIN + 2, h_basic, 0, NULL);
	sigemptyset(&b);
	sigaddset(&b, SIGRTMIN + 2);
	sigprocmask(SIG_BLOCK, &b, NULL);
	hits = 0;
	raise(SIGRTMIN + 2);
	raise(SIGRTMIN + 2);
	raise(SIGRTMIN + 2);
	sigprocmask(SIG_UNBLOCK, &b, NULL);
	printf("rt-queue: hits=%d\n", (int)hits);

	/* sigtimedwait consumes a pending blocked signal without a handler run */
	sigemptyset(&b);
	sigaddset(&b, SIGUSR2);
	sigprocmask(SIG_BLOCK, &b, NULL);
	raise(SIGUSR2);
	siginfo_t si;
	struct timespec to = { 1, 0 };
	int got = sigtimedwait(&b, &si, &to);
	printf("sigtimedwait: got-usr2=%s code=%s\n", YN(got == SIGUSR2), codename(si.si_code));
	to.tv_sec = 0;
	to.tv_nsec = 10000000;
	errno = 0;
	got = sigtimedwait(&b, &si, &to);
	printf("sigtimedwait-timeout: ret=%d errno=%s\n", got, errstr(errno));
	sigprocmask(SIG_UNBLOCK, &b, NULL);

	/* SIG_IGN discards */
	signal(SIGUSR2, SIG_IGN);
	r = raise(SIGUSR2);
	printf("ignored-raise: ret=%d survived=yes\n", r);

	/* synchronous SIGSEGV with siglongjmp out */
	setact(SIGSEGV, h_segv, SA_SIGINFO | SA_NODEFER, NULL);
	last_sig = 0;
	if (sigsetjmp(jb, 1) == 0) {
		static volatile uintptr_t bad = 16;
		*(volatile int *)bad = 1;
		printf("segv: not-raised\n");
	} else {
		printf("segv: sig=%s code=%s addr-matches=%s\n", last_sig == SIGSEGV ? "SIGSEGV" : "other",
		       codename(last_code), YN(si_pid_ok));
	}

	/* EINTR vs SA_RESTART on a blocking pipe read: child signals then writes */
	for (int restart = 0; restart <= 1; restart++) {
		setact(SIGUSR1, h_nop, restart ? SA_RESTART : 0, NULL);
		int p[2];
		if (pipe(p) != 0)
			return 1;
		fflush(stdout);
		pid_t parent = getpid();
		pid_t c = fork();
		if (c == 0) {
			struct timespec d = { 0, 100000000 };
			nanosleep(&d, NULL);
			kill(parent, SIGUSR1);
			nanosleep(&d, NULL);
			(void)!write(p[1], "k", 1);
			_exit(0);
		}
		char ch;
		hits = 0;
		errno = 0;
		ssize_t n = read(p[0], &ch, 1);
		printf("read-interrupted-%s: ret=%zd errno=%s hits=%d\n", restart ? "sa_restart" : "no-restart", n,
		       errstr(n < 0 ? errno : 0), (int)hits);
		waitpid(c, NULL, 0);
		close(p[0]);
		close(p[1]);
	}

	/* child killed by a signal */
	fflush(stdout);
	pid_t c = fork();
	if (c == 0) {
		signal(SIGTERM, SIG_DFL);
		raise(SIGTERM);
		_exit(99);
	}
	int st;
	waitpid(c, &st, 0);
	printf("child-killed: signaled=%s sig-is-term=%s\n", YN(WIFSIGNALED(st)), YN(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM));

	done();
	return 0;
}
