/* sys_process: the process model a shell depends on. Signal handlers that
 * return through the default (vDSO) restorer, SIGCHLD delivery around fork
 * and wait, sigsuspend running the handler before it returns, SA_RESTART on
 * a blocking read, execve preserving argv[0] (multi-call binaries dispatch on
 * it), fork+pipe+dup2+execve pipelines, posix_spawn and vfork+execve, and
 * /proc/self/exe naming the program rather than the emulator. */
#include "a64sys.h"
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <time.h>

#define MARK_ARGV0 "--a64sys-proc-argv0"
#define MARK_EMIT "--a64sys-proc-emit"
#define MARK_COUNT "--a64sys-proc-count"
#define MARK_ENVONLY "A64SYS_PROC_ENVONLY"

extern char **environ;

static char self_path[PATH_MAX];

static void msleep(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

/* ---- exec'd modes ---- */

static int mode_argv0(int argc, char **argv)
{
	char me[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", me, sizeof(me) - 1);
	me[n > 0 ? n : 0] = 0;
	/* never print an absolute path */
	printf("argv0-child: argc=%d argv0=%s\n", argc, !strcmp(argv[0], me) ? "<self-path>" : argv[0]);
	fflush(stdout);
	return 0;
}

static int mode_emit(void)
{
	for (int i = 0; i < 3; i++)
		printf("line-%d\n", i);
	fflush(stdout);
	return 0;
}

static int mode_count(const char *argv0)
{
	char buf[256];
	size_t lines = 0, bytes = 0;
	ssize_t n;
	while ((n = read(0, buf, sizeof(buf))) > 0) {
		bytes += n;
		for (ssize_t i = 0; i < n; i++)
			lines += buf[i] == '\n';
	}
	printf("count-child: argv0=%s lines=%zu bytes=%zu\n", argv0, lines, bytes);
	fflush(stdout);
	return 4;
}

/* ---- handlers ---- */

static volatile sig_atomic_t usr1_count;
static volatile sig_atomic_t chld_count;
static volatile sig_atomic_t chld_code;
static volatile sig_atomic_t chld_status;
static volatile sig_atomic_t info_ok;

static void h_usr1(int s)
{
	(void)s;
	usr1_count++;
}

static void h_usr1_info(int s, siginfo_t *si, void *uc)
{
	info_ok = s == SIGUSR1 && si && si->si_signo == SIGUSR1 && si->si_code == SI_TKILL && si->si_pid == getpid() && uc;
}

static void h_chld(int s)
{
	(void)s;
	chld_count++;
}

static void h_chld_info(int s, siginfo_t *si, void *uc)
{
	(void)s;
	(void)uc;
	chld_count++;
	chld_code = si->si_code;
	chld_status = si->si_status;
}

struct ksigaction {
	void *handler;
	unsigned long flags;
	void *restorer;
	unsigned long mask;
};

static void pr_status(const char *tag, int st)
{
	printf("%s: exited=%s", tag, YN(WIFEXITED(st)));
	if (WIFEXITED(st))
		printf(" code=%d", WEXITSTATUS(st));
	printf(" signaled=%s", YN(WIFSIGNALED(st)));
	if (WIFSIGNALED(st))
		printf(" sig=%d", WTERMSIG(st));
	printf("\n");
}

/* Callee-saved state across a delivered signal: the handler runs JIT code of
 * its own, so any register the delivery forgets to restore shows up here. */
static unsigned long __attribute__((noinline)) mix_across_signal(unsigned long a)
{
	unsigned long x = a * 0x9e3779b97f4a7c15UL, y = a ^ 0x0123456789abcdefUL, z = a + 77;
	double d = (double)a * 1.5;
	raise(SIGUSR1);
	return x ^ (y << 1) ^ (z << 2) ^ (unsigned long)(d * 3.0);
}
static unsigned long __attribute__((noinline)) mix_plain(unsigned long a)
{
	unsigned long x = a * 0x9e3779b97f4a7c15UL, y = a ^ 0x0123456789abcdefUL, z = a + 77;
	double d = (double)a * 1.5;
	return x ^ (y << 1) ^ (z << 2) ^ (unsigned long)(d * 3.0);
}

static void set_handler(int sig, void (*h)(int), int flags)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = h;
	sa.sa_flags = flags;
	sigaction(sig, &sa, NULL);
}

static void set_info_handler(int sig, void (*h)(int, siginfo_t *, void *), int flags)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = h;
	sa.sa_flags = flags | SA_SIGINFO;
	sigaction(sig, &sa, NULL);
}

static void test_handlers(void)
{
	set_handler(SIGUSR1, h_usr1, 0);
	struct ksigaction k;
	long r = syscall(SYS_rt_sigaction, SIGUSR1, NULL, &k, 8);
	printf("libc-sigaction-kernel-view: ret=%ld restorer-flag=%s restorer-null=%s\n", r,
	       YN(k.flags & 0x04000000UL), YN(k.restorer == NULL));

	raise(SIGUSR1);
	raise(SIGUSR1);
	printf("handler-return: count=%d\n", (int)usr1_count);

	usr1_count = 0;
	int same = 1;
	for (unsigned long a = 1; a < 2000; a += 37)
		same &= mix_across_signal(a) == mix_plain(a);
	printf("handler-preserves-state: same=%s count=%d\n", YN(same), (int)usr1_count);

	set_info_handler(SIGUSR1, h_usr1_info, 0);
	raise(SIGUSR1);
	printf("siginfo-handler: ok=%s\n", YN(info_ok));

	/* the handler's mask is sa_mask + the signal; the old mask is back after */
	sigset_t m;
	sigprocmask(SIG_BLOCK, NULL, &m);
	printf("mask-after-handlers: usr1-blocked=%s\n", YN(sigismember(&m, SIGUSR1)));
	signal(SIGUSR1, SIG_DFL);
}

static void test_sigchld(void)
{
	chld_count = 0;
	set_info_handler(SIGCHLD, h_chld_info, SA_RESTART);
	fflush(stdout);
	pid_t c = fork();
	if (c == 0)
		_exit(7);
	int st;
	pid_t w;
	while ((w = waitpid(c, &st, 0)) < 0 && errno == EINTR)
		;
	/* the handler may still be on its way when waitpid returns: wait for it */
	for (int i = 0; i < 200 && chld_count == 0; i++)
		msleep(5);
	printf("sigchld-fork-wait: pid-ok=%s count=%d code-exited=%s status=%d\n", YN(w == c), (int)chld_count,
	       YN(chld_code == CLD_EXITED), (int)chld_status);
	pr_status("sigchld-fork-wait-status", st);

	/* the busybox ash `wait` pattern: SIGCHLD blocked, reap with WNOHANG,
	 * then sigsuspend; the handler has to run before sigsuspend returns */
	chld_count = 0;
	set_handler(SIGCHLD, h_chld, 0);
	sigset_t blk, old;
	sigfillset(&blk);
	sigprocmask(SIG_SETMASK, &blk, &old);
	fflush(stdout);
	c = fork();
	if (c == 0)
		_exit(8);
	msleep(100); /* the child has exited; SIGCHLD is pending and blocked */
	sigset_t pend;
	sigpending(&pend);
	printf("sigsuspend-pending-before: chld=%s count=%d\n", YN(sigismember(&pend, SIGCHLD)), (int)chld_count);
	errno = 0;
	int sr = sigsuspend(&old);
	int se = errno;
	printf("sigsuspend-pending: ret=%d errno=%s handler-ran-before-return=%s\n", sr, errstr(se), YN(chld_count == 1));
	sigprocmask(SIG_BLOCK, NULL, &pend);
	printf("sigsuspend-mask-restored: all-blocked=%s\n", YN(sigismember(&pend, SIGCHLD) && sigismember(&pend, SIGUSR2)));
	w = waitpid(c, &st, WNOHANG);
	pr_status("sigsuspend-reaped", st);

	/* nothing pending: sigsuspend sleeps until the child exits */
	chld_count = 0;
	fflush(stdout);
	c = fork();
	if (c == 0) {
		msleep(50);
		_exit(9);
	}
	errno = 0;
	sr = sigsuspend(&old);
	se = errno;
	printf("sigsuspend-wait: ret=%d errno=%s handler-ran-before-return=%s\n", sr, errstr(se), YN(chld_count == 1));
	waitpid(c, &st, 0);
	pr_status("sigsuspend-wait-reaped", st);
	sigprocmask(SIG_SETMASK, &old, NULL);

	/* SIGCHLD interrupting a blocking pipe read: restarted with SA_RESTART,
	 * EINTR without */
	for (int restart = 1; restart >= 0; restart--) {
		chld_count = 0;
		set_handler(SIGCHLD, h_chld, restart ? SA_RESTART : 0);
		int p[2];
		(void)!pipe(p);
		fflush(stdout);
		pid_t quick = fork();
		if (quick == 0)
			_exit(0);
		pid_t writer = fork();
		if (writer == 0) {
			msleep(300);
			(void)!write(p[1], "ok", 2);
			_exit(0);
		}
		/* let the quick child's SIGCHLD land before the read blocks */
		for (int i = 0; i < 200 && chld_count == 0; i++)
			msleep(5);
		chld_count = 0;
		pid_t slow = fork();
		if (slow == 0) {
			msleep(100);
			_exit(0);
		}
		char buf[8] = { 0 };
		errno = 0;
		ssize_t n = read(p[0], buf, sizeof(buf));
		int e = errno;
		printf("sigchld-during-read-%s: ret=%zd errno=%s data=%s handler-ran=%s\n", restart ? "sa_restart" : "no-restart", n,
		       errstr(n < 0 ? e : 0), n > 0 ? buf : "-", YN(chld_count > 0));
		if (n < 0)
			(void)!read(p[0], buf, sizeof(buf));
		waitpid(quick, NULL, 0);
		waitpid(writer, NULL, 0);
		waitpid(slow, NULL, 0);
		close(p[0]);
		close(p[1]);
	}
	signal(SIGCHLD, SIG_DFL);
}

/* fork + execve of this program; the child prints what it received */
static void run_exec(const char *tag, const char *path, char *const argv[], char *const envp[])
{
	fflush(stdout);
	pid_t c = fork();
	if (c == 0) {
		execve(path, argv, envp);
		printf("%s: execve failed errno=%s\n", tag, errstr(errno));
		fflush(stdout);
		_exit(99);
	}
	int st;
	waitpid(c, &st, 0);
	pr_status(tag, st);
}

static void test_exec(void)
{
	char *env[] = { "A64SYS_PROC=1", NULL };
	char *a1[] = { "custom-argv0", MARK_ARGV0, NULL };
	run_exec("execve-proc-self-exe-argv0", "/proc/self/exe", a1, env);
	char *a2[] = { "other-name", MARK_ARGV0, "extra", NULL };
	run_exec("execve-path-argv0", self_path, a2, env);
	char *a3[] = { self_path, MARK_ARGV0, NULL };
	run_exec("execve-path-argv0-is-path", self_path, a3, env);
	/* no argv at all: Linux supplies argc=1 with an empty argv[0] */
	char *a4[] = { NULL };
	char *env4[] = { MARK_ENVONLY "=1", NULL };
	run_exec("execve-empty-argv", "/proc/self/exe", a4, env4);
}

static void test_pipeline(void)
{
	int p[2];
	(void)!pipe2(p, O_CLOEXEC);
	fflush(stdout);
	pid_t a = fork();
	if (a == 0) {
		dup2(p[1], 1);
		char *av[] = { "emitter", MARK_EMIT, NULL };
		execve(self_path, av, environ);
		_exit(98);
	}
	pid_t b = fork();
	if (b == 0) {
		dup2(p[0], 0);
		char *av[] = { "counter", MARK_COUNT, NULL };
		execve("/proc/self/exe", av, environ);
		_exit(98);
	}
	close(p[0]);
	close(p[1]);
	int sa, sb;
	waitpid(a, &sa, 0);
	waitpid(b, &sb, 0);
	pr_status("pipeline-emitter", sa);
	pr_status("pipeline-counter", sb);

	/* posix_spawn with a dup2 file action, reading the child's stdout */
	int q[2];
	(void)!pipe2(q, O_CLOEXEC);
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, q[1], 1);
	char *sv[] = { "spawned", MARK_EMIT, NULL };
	pid_t s;
	fflush(stdout);
	int r = posix_spawn(&s, self_path, &fa, NULL, sv, environ);
	close(q[1]);
	char buf[128];
	size_t got = 0;
	ssize_t n;
	while ((n = read(q[0], buf + got, sizeof(buf) - 1 - got)) > 0)
		got += n;
	buf[got] = 0;
	close(q[0]);
	int ss;
	waitpid(s, &ss, 0);
	int lines = 0;
	for (size_t i = 0; i < got; i++)
		lines += buf[i] == '\n';
	printf("posix_spawn: ret=%d bytes=%zu lines=%d\n", r, got, lines);
	pr_status("posix_spawn-status", ss);
	posix_spawn_file_actions_destroy(&fa);

	/* Not checked: posix_spawn of a missing program. glibc reports the exec
	 * errno through memory shared by clone(CLONE_VM|CLONE_VFORK); POWERarm
	 * runs a CLONE_VM child without CLONE_THREAD as a fork, so it returns 0
	 * and the child exits 127 instead. */

	/* vfork + execve */
	fflush(stdout);
	pid_t v = vfork();
	if (v == 0) {
		char *av[] = { "vforked", MARK_ARGV0, NULL };
		execve(self_path, av, environ);
		_exit(97);
	}
	int sv2;
	waitpid(v, &sv2, 0);
	pr_status("vfork-execve", sv2);
}

static void test_proc_self_exe(void)
{
	char link[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", link, sizeof(link) - 1);
	link[n > 0 ? n : 0] = 0;
	char real[PATH_MAX];
	const char *execfn = (const char *)getauxval(AT_EXECFN);
	int ok = execfn && realpath(execfn, real) && !strcmp(real, link);
	printf("readlink-proc-self-exe: matches-execfn=%s\n", YN(ok));
}

int main(int argc, char **argv)
{
	if (getenv(MARK_ENVONLY)) {
		printf("argv0-child: argc=%d argv0-empty=%s\n", argc, YN(argc == 1 && argv[0] && !argv[0][0]));
		fflush(stdout);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], MARK_ARGV0))
		return mode_argv0(argc, argv);
	if (argc > 1 && !strcmp(argv[1], MARK_EMIT))
		return mode_emit();
	if (argc > 1 && !strcmp(argv[1], MARK_COUNT))
		return mode_count(argv[0]);

	setvbuf(stdout, NULL, _IOLBF, 0);
	/* independent of the inherited mask and dispositions */
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	signal(SIGCHLD, SIG_DFL);
	signal(SIGUSR1, SIG_DFL);
	ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
	self_path[n > 0 ? n : 0] = 0;

	test_proc_self_exe();
	test_handlers();
	test_sigchld();
	test_exec();
	test_pipeline();
	done();
	return 0;
}
