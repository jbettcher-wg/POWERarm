/* sys_fork_exec: fork (clone SIGCHLD), raw clone, vfork, wait4 + status
 * macros + rusage, pipe2 parent<->child, execve of self with a marker
 * argument (O_CLOEXEC, env, signal disposition and mask inheritance), exit
 * and exit_group codes, execve error paths.
 * Signals are never delivered to guest code here; SIGKILL of a child is
 * handled entirely by the kernel. */
#include "a64sys.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/mman.h>

#define MARK "--a64sys-exec-child"

static int exec_child(int argc, char **argv)
{
	/* argv: self MARK cloexec_fd keep_fd */
	printf("exec-child: argc=%d arg1=%s\n", argc, argv[1]);
	const char *e = getenv("A64SYS_MARK");
	printf("exec-child: env-mark=%s path-unset=%s\n", e ? e : "(null)", YN(getenv("PATH") == NULL));
	int cfd = argc > 2 ? atoi(argv[2]) : -1;
	int kfd = argc > 3 ? atoi(argv[3]) : -1;
	errno = 0;
	int r = fcntl(cfd, F_GETFD);
	printf("exec-child: cloexec-fd-closed=%s\n", YN(r == -1 && errno == EBADF));
	r = fcntl(kfd, F_GETFD);
	printf("exec-child: plain-fd-open=%s\n", YN(r >= 0));
	char buf[16] = { 0 };
	ssize_t n = pread(kfd, buf, 5, 0);
	printf("exec-child: plain-fd-read=%zd data=%.5s\n", n, buf);

	struct sigaction sa;
	sigaction(SIGUSR1, NULL, &sa);
	printf("exec-child: usr1-handler-reset-to-dfl=%s\n", YN(sa.sa_handler == SIG_DFL));
	sigaction(SIGUSR2, NULL, &sa);
	printf("exec-child: usr2-ignore-kept=%s\n", YN(sa.sa_handler == SIG_IGN));
	sigset_t m;
	sigprocmask(SIG_BLOCK, NULL, &m);
	printf("exec-child: mask-winch-kept=%s mask-usr1=%s\n", YN(sigismember(&m, SIGWINCH)), YN(sigismember(&m, SIGUSR1)));
	/* altstack is reset by exec */
	stack_t ss;
	sigaltstack(NULL, &ss);
	printf("exec-child: altstack-disabled=%s\n", YN(ss.ss_flags & SS_DISABLE));
	fflush(stdout);
	syscall(SYS_exit_group, 3);
	return 99;
}

static void h_usr1(int s) { (void)s; }

static void pr_status(const char *tag, pid_t w, pid_t want, int st)
{
	printf("%s: pid-matches=%s exited=%s", tag, YN(w == want), YN(WIFEXITED(st)));
	if (WIFEXITED(st))
		printf(" code=%d", WEXITSTATUS(st));
	printf(" signaled=%s", YN(WIFSIGNALED(st)));
	if (WIFSIGNALED(st))
		printf(" sig=%d core=%s", WTERMSIG(st), YN(WCOREDUMP(st)));
	printf(" stopped=%s\n", YN(WIFSTOPPED(st)));
}

static int global_val = 1;

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], MARK))
		return exec_child(argc, argv);

	int st;
	struct rusage ru;
	errno = 0;
	pid_t w = wait4(-1, &st, 0, NULL);
	printf("wait4-no-children: ret=%d errno=%s\n", w, errstr(errno));

	/* --- fork + pipe2 + wait4 + rusage --- */
	int toc[2], top[2];
	printf("pipe2-cloexec: ret=%d\n", pipe2(toc, O_CLOEXEC));
	printf("pipe2-plain: ret=%d\n", pipe2(top, 0));
	pid_t parent = getpid();
	char *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	char *priv = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	shared[0] = 's';
	priv[0] = 'p';
	fflush(stdout);
	pid_t c = fork();
	if (c == 0) {
		char go;
		/* wait for the parent's go-ahead so WNOHANG sees us running */
		if (read(toc[0], &go, 1) != 1)
			_exit(90);
		char msg[64];
		int len = snprintf(msg, sizeof(msg), "ppid-ok=%s pid-differs=%s global=%d", YN(getppid() == parent),
		                   YN(getpid() != parent), global_val);
		global_val = 777;
		shared[0] = 'S';
		priv[0] = 'P';
		(void)!write(top[1], msg, len);
		/* burn a little CPU so rusage has something */
		volatile unsigned long x = 0;
		for (unsigned long i = 0; i < 5000000UL; i++)
			x += i;
		_exit(42);
	}
	printf("fork: parent-sees-positive=%s\n", YN(c > 0));
	w = wait4(c, &st, WNOHANG, &ru);
	printf("wait4-wnohang-running: ret=%d\n", w);
	(void)!write(toc[1], "g", 1);
	char buf[128];
	ssize_t n = read(top[0], buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = 0;
	printf("pipe-from-child: %s\n", buf);
	memset(&ru, 0, sizeof(ru));
	w = wait4(c, &st, 0, &ru);
	pr_status("wait4-exit42", w, c, st);
	/* ru_maxrss of a reaped child is 0 on some kernels (seen on the Pi 5
	 * 6.18 kernel), so only fault count and time sanity are checked */
	printf("wait4-rusage: utime-nonneg=%s usec-in-range=%s minflt-positive=%s\n",
	       YN(ru.ru_utime.tv_sec >= 0 && ru.ru_utime.tv_usec >= 0), YN(ru.ru_utime.tv_usec < 1000000),
	       YN(ru.ru_minflt > 0));
	printf("fork-cow: global-unchanged=%s private-unchanged=%s shared-changed=%s\n", YN(global_val == 1),
	       YN(priv[0] == 'p'), YN(shared[0] == 'S'));
	errno = 0;
	w = wait4(c, &st, 0, NULL);
	printf("wait4-reaped-again: ret=%d errno=%s\n", w, errstr(errno));
	errno = 0;
	w = wait4(0x7ffffff0, &st, 0, NULL);
	printf("wait4-bogus-pid: ret=%d errno=%s\n", w, errstr(errno));
	errno = 0;
	w = wait4(-1, &st, 0x00100000, NULL);
	printf("wait4-bad-options: ret=%d errno=%s\n", w, errstr(errno));

	/* shared file offset across fork */
	tmp_enter();
	int fd = open("off", O_RDWR | O_CREAT | O_TRUNC, 0644);
	fflush(stdout);
	c = fork();
	if (c == 0) {
		(void)!write(fd, "child", 5);
		_exit(0);
	}
	waitpid(c, &st, 0);
	printf("fork-shared-offset: parent-offset=%ld\n", (long)lseek(fd, 0, SEEK_CUR));

	/* --- exit codes --- */
	static const struct { const char *how; int code; } codes[] = {
		{ "exit_group", 5 }, { "exit", 6 }, { "exit_group-truncated", 256 + 3 }, { "return-from-child-libc-exit", 0 },
	};
	for (size_t i = 0; i < NELEM(codes); i++) {
		fflush(stdout);
		c = fork();
		if (c == 0) {
			if (i == 0 || i == 2)
				syscall(SYS_exit_group, codes[i].code);
			else if (i == 1)
				syscall(SYS_exit, codes[i].code);
			else
				exit(0);
			_exit(99);
		}
		w = wait4(c, &st, 0, NULL);
		char tag[64];
		snprintf(tag, sizeof(tag), "child-%s", codes[i].how);
		pr_status(tag, w, c, st);
	}

	/* raw clone(SIGCHLD) behaves like fork */
	fflush(stdout);
	long rc = syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0);
	if (rc == 0) {
		syscall(SYS_exit_group, 11);
		_exit(99);
	}
	w = wait4(rc, &st, 0, NULL);
	pr_status("raw-clone-sigchld", w, rc, st);

	/* clone with exit signal 0: plain wait4 won't see it without __WCLONE */
	fflush(stdout);
	rc = syscall(SYS_clone, 0, 0, 0, 0, 0);
	if (rc == 0) {
		syscall(SYS_exit_group, 12);
		_exit(99);
	}
	w = wait4(rc, &st, __WCLONE, NULL);
	pr_status("raw-clone-no-exitsig-wclone", w, rc, st);

	/* vfork */
	fflush(stdout);
	c = vfork();
	if (c == 0)
		_exit(13);
	w = wait4(c, &st, 0, NULL);
	pr_status("vfork-exit13", w, c, st);

	/* SIGKILL a child blocked on a pipe read (kernel-side only) */
	int blk[2];
	(void)!pipe(blk);
	fflush(stdout);
	c = fork();
	if (c == 0) {
		char ch;
		(void)!read(blk[0], &ch, 1);
		_exit(0);
	}
	kill(c, SIGKILL);
	w = wait4(c, &st, 0, NULL);
	pr_status("child-sigkill", w, c, st);
	close(blk[0]);
	close(blk[1]);

	/* --- execve self --- */
	int kfd = open("off", O_RDONLY);
	int cfd = open("off", O_RDONLY | O_CLOEXEC);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = h_usr1;
	sigaction(SIGUSR1, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGUSR2, &sa, NULL);
	sigset_t bm;
	sigemptyset(&bm);
	sigaddset(&bm, SIGWINCH);
	sigset_t oldmask;
	sigprocmask(SIG_SETMASK, &bm, &oldmask);
	char cfds[16], kfds[16];
	snprintf(cfds, sizeof(cfds), "%d", cfd);
	snprintf(kfds, sizeof(kfds), "%d", kfd);
	char *cargv[] = { argv[0], MARK, cfds, kfds, NULL };
	char *cenv[] = { "A64SYS_MARK=hello-exec", NULL };
	fflush(stdout);
	c = fork();
	if (c == 0) {
		execve("/proc/self/exe", cargv, cenv);
		/* fallback when /proc is unavailable */
		execve(argv[0], cargv, cenv);
		printf("execve-self: failed errno=%s\n", errstr(errno));
		fflush(stdout);
		_exit(98);
	}
	w = wait4(c, &st, 0, NULL);
	pr_status("execve-self", w, c, st);
	sigprocmask(SIG_SETMASK, &oldmask, NULL);
	signal(SIGUSR1, SIG_DFL);
	signal(SIGUSR2, SIG_DFL);
	close(kfd);
	close(cfd);

	/* execve errors (run in children so a surprise success can't replace us) */
	close(open("notexec", O_CREAT | O_WRONLY, 0644));
	int gfd = open("garbage", O_CREAT | O_WRONLY, 0755);
	(void)!write(gfd, "\x01\x02\x03\x04garbage-not-elf", 20);
	close(gfd);
	int sfd = open("script", O_CREAT | O_WRONLY, 0755);
	(void)!write(sfd, "#!/nonexistent/interp\n", 22);
	close(sfd);
	mkdir("adir", 0755);
	static const char *const bad[] = { "nonexistent", "notexec", "garbage", "script", "adir", "notexec/x" };
	for (size_t i = 0; i < NELEM(bad); i++) {
		int ep[2];
		(void)!pipe2(ep, O_CLOEXEC);
		fflush(stdout);
		c = fork();
		if (c == 0) {
			char *av[] = { (char *)bad[i], NULL };
			char *ev[] = { NULL };
			syscall(SYS_execve, bad[i], av, ev);
			int e = errno;
			(void)!write(ep[1], &e, sizeof(e));
			_exit(0);
		}
		close(ep[1]);
		int e = -1;
		ssize_t got = read(ep[0], &e, sizeof(e));
		close(ep[0]);
		waitpid(c, &st, 0);
		printf("execve-%s: failed=%s errno=%s\n", bad[i], YN(got == sizeof(e)), errstr(got == sizeof(e) ? e : 0));
	}
	errno = 0;
	fflush(stdout);
	c = fork();
	if (c == 0) {
		long r = syscall(SYS_execve, "/proc/self/exe", (void *)8, NULL);
		_exit(r == -1 && errno == EFAULT ? 20 : 21);
	}
	waitpid(c, &st, 0);
	printf("execve-bad-argv: efault=%s\n", YN(WIFEXITED(st) && WEXITSTATUS(st) == 20));

	unlink("notexec");
	unlink("garbage");
	unlink("script");
	rmdir("adir");
	close(fd);
	unlink("off");
	tmp_leave();
	done();
	return 0;
}
