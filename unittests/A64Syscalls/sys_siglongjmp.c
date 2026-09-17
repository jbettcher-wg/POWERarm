/* sys_siglongjmp: guest signal handlers that are abandoned instead of
 * returning through rt_sigreturn -- siglongjmp/longjmp out of the handler.
 * Real programs do this all the time (interpreters, test harnesses, crash
 * handlers, SIGILL feature probes), so every section repeats it far more
 * often than an emulator could afford to leak per handler, and mixes in
 * handlers that do return so both paths stay correct side by side.
 *
 * Sections:
 *   raise             sigsetjmp/raise/siglongjmp, the mask restored each time
 *   longjmp-blocked   setjmp/longjmp (no mask restore): the signal stays
 *                     blocked, as on Linux, and is unblocked by hand
 *   longjmp-nodefer   SA_NODEFER handler left with longjmp
 *   segv              SIGSEGV from a guard page, siglongjmp out; every 16th
 *                     handler makes the page readable and returns instead
 *   nested            a handler that raises a second signal whose handler
 *                     jumps out of both, or only out of itself into the first
 *                     handler, which then returns
 *   nodefer-recursive SA_NODEFER handler re-raising itself 8 deep, then out
 *   altstack          SA_ONSTACK SIGSEGV handler on a sigaltstack, siglongjmp
 *                     back to the main stack; nested onstack signal too
 *   callret           a handler 200 calls deep jumps out, then the program's
 *                     own calls and returns are checked
 *   deepening         the signal raised at a recursion depth that changes
 *                     every time, so the next handler's frame lands over
 *                     the last one's; setjmp/longjmp, no mask syscalls
 *   timer             ITIMER_REAL interrupting a busy loop, siglongjmp out
 *   threads           the raise and segv loops on 4 threads at once
 *
 * Output is counts and checksums only. */
#include "a64sys.h"
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/time.h>

#define BIT(s) (1UL << ((s) - 1))

static uint64_t mix(uint64_t h, uint64_t v)
{
	return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

static void setact(int sig, void *h, int flags)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	if (flags & SA_SIGINFO)
		sa.sa_sigaction = h;
	else
		sa.sa_handler = h;
	sa.sa_flags = flags;
	sigemptyset(&sa.sa_mask);
	sigaction(sig, &sa, NULL);
}

static unsigned long cur_mask(void)
{
	unsigned long cur = 0;
	syscall(SYS_rt_sigprocmask, SIG_BLOCK, NULL, &cur, 8);
	return cur;
}

__attribute__((noinline)) static uint64_t rec(uint64_t n)
{
	if (n == 0)
		return 1;
	uint64_t r = rec(n - 1);
	return mix(r, n);
}

/* ---- raise ---- */
static sigjmp_buf raise_jb;
static void h_raise(int s)
{
	(void)s;
	siglongjmp(raise_jb, 1);
}

static void t_raise(void)
{
	setact(SIGUSR1, h_raise, 0);
	volatile unsigned long jumped = 0, mask_bad = 0;
	for (volatile unsigned long i = 0; i < 100000; i++) {
		if (sigsetjmp(raise_jb, 1) == 0) {
			raise(SIGUSR1);
		} else {
			jumped++;
			if ((i & 1023) == 0 && (cur_mask() & BIT(SIGUSR1)))
				mask_bad++;
		}
	}
	printf("raise: iterations=100000 jumped=%lu mask-restored=%s\n", (unsigned long)jumped, YN(mask_bad == 0));
}

/* ---- longjmp without mask restore ---- */
static jmp_buf plain_jb;
static void h_plain(int s)
{
	(void)s;
	longjmp(plain_jb, 1);
}

static void t_longjmp(void)
{
	/* glibc setjmp does not save the mask: after longjmp the handler's mask
	 * (with SIGUSR1 blocked) is still in effect, exactly as on Linux. */
	setact(SIGUSR1, h_plain, 0);
	volatile unsigned long jumped = 0, blocked = 0;
	sigset_t usr1;
	sigemptyset(&usr1);
	sigaddset(&usr1, SIGUSR1);
	for (volatile unsigned long i = 0; i < 20000; i++) {
		if (setjmp(plain_jb) == 0) {
			kill(getpid(), SIGUSR1);
		} else {
			jumped++;
			if (cur_mask() & BIT(SIGUSR1))
				blocked++;
			sigprocmask(SIG_UNBLOCK, &usr1, NULL);
		}
	}
	printf("longjmp-blocked: jumped=%lu still-blocked=%lu\n", (unsigned long)jumped, (unsigned long)blocked);

	setact(SIGUSR2, h_plain, SA_NODEFER);
	jumped = 0;
	blocked = 0;
	for (volatile unsigned long i = 0; i < 20000; i++) {
		if (setjmp(plain_jb) == 0) {
			raise(SIGUSR2);
		} else {
			jumped++;
			if (cur_mask() & BIT(SIGUSR2))
				blocked++;
		}
	}
	printf("longjmp-nodefer: jumped=%lu blocked=%lu\n", (unsigned long)jumped, (unsigned long)blocked);
	setact(SIGUSR1, SIG_DFL, 0);
	setact(SIGUSR2, SIG_DFL, 0);
}

/* ---- segv ---- */
static sigjmp_buf segv_jb;
static char *guard;
static long pagesz;
static volatile unsigned long segv_returns, segv_addr_bad, segv_count;

static void h_segv(int s, siginfo_t *si, void *uc)
{
	(void)s;
	(void)uc;
	if (si->si_addr != guard)
		segv_addr_bad++;
	if ((++segv_count & 15) == 0) {
		mprotect(guard, pagesz, PROT_READ);
		segv_returns++;
		return;
	}
	siglongjmp(segv_jb, 1);
}

static void t_segv(void)
{
	pagesz = sysconf(_SC_PAGESIZE);
	guard = mmap(NULL, pagesz, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	setact(SIGSEGV, h_segv, SA_SIGINFO);
	volatile unsigned long jumped = 0, read_back = 0;
	volatile uint64_t h = 0;
	for (volatile unsigned long i = 0; i < 100000; i++) {
		if (sigsetjmp(segv_jb, 1) == 0) {
			h = mix(h, i);
			read_back += (unsigned char)guard[0]; /* faults */
			mprotect(guard, pagesz, PROT_NONE);
			h = mix(h, 7);
		} else {
			jumped++;
			h = mix(h, rec(i % 16));
		}
	}
	setact(SIGSEGV, SIG_DFL, 0);
	munmap(guard, pagesz);
	printf("segv: faults=%lu jumped=%lu returned=%lu addr-ok=%s read-zero=%s sum=%016llx\n", (unsigned long)segv_count,
	       (unsigned long)jumped, (unsigned long)segv_returns, YN(segv_addr_bad == 0), YN(read_back == 0),
	       (unsigned long long)h);
}

/* ---- nested ---- */
static sigjmp_buf outer_jb, inner_jb;
static volatile int nest_mode;
static volatile unsigned long outer_returned, inner_entered;

static void h_inner(int s)
{
	(void)s;
	inner_entered++;
	if (nest_mode == 0)
		siglongjmp(outer_jb, 2); /* out of both handlers */
	siglongjmp(inner_jb, 1);         /* back into the outer handler */
}

static void h_outer(int s)
{
	(void)s;
	if (sigsetjmp(inner_jb, 1) == 0) {
		raise(SIGUSR2);
		return; /* not reached */
	}
	outer_returned++;
}

static void t_nested(void)
{
	setact(SIGUSR1, h_outer, 0);
	setact(SIGUSR2, h_inner, 0);
	volatile unsigned long both = 0, after_return = 0, mask_bad = 0;
	volatile uint64_t keep = 0x1234567890abcdefull;
	for (volatile unsigned long i = 0; i < 40000; i++) {
		nest_mode = (int)(i & 1);
		int v = sigsetjmp(outer_jb, 1);
		if (v == 0) {
			raise(SIGUSR1);
			after_return++;
		} else {
			both++;
		}
		keep = mix(keep, i);
		if ((i & 511) == 0 && (cur_mask() & (BIT(SIGUSR1) | BIT(SIGUSR2))))
			mask_bad++;
	}
	setact(SIGUSR1, SIG_DFL, 0);
	setact(SIGUSR2, SIG_DFL, 0);
	printf("nested: inner=%lu out-of-both=%lu outer-returned=%lu raise-returned=%lu mask-restored=%s keep=%016llx\n",
	       (unsigned long)inner_entered, (unsigned long)both, (unsigned long)outer_returned, (unsigned long)after_return,
	       YN(mask_bad == 0), (unsigned long long)keep);
}

/* ---- SA_NODEFER recursion ---- */
static sigjmp_buf rec_jb;
static volatile int rec_depth, rec_max;

static void h_rec(int s)
{
	(void)s;
	if (++rec_depth > rec_max)
		rec_max = rec_depth;
	if (rec_depth < 8)
		raise(SIGUSR1);
	siglongjmp(rec_jb, rec_depth);
}

static void t_nodefer_rec(void)
{
	setact(SIGUSR1, h_rec, SA_NODEFER);
	volatile unsigned long jumped = 0, depth_ok = 0;
	for (volatile unsigned long i = 0; i < 10000; i++) {
		rec_depth = 0;
		int v = sigsetjmp(rec_jb, 1);
		if (v == 0) {
			raise(SIGUSR1);
		} else {
			jumped++;
			if (v == 8)
				depth_ok++;
		}
	}
	setact(SIGUSR1, SIG_DFL, 0);
	printf("nodefer-recursive: jumped=%lu depth8=%lu max=%d\n", (unsigned long)jumped, (unsigned long)depth_ok, rec_max);
}

/* ---- altstack ---- */
static sigjmp_buf alt_jb;
static char *alt_lo, *alt_hi, *alt_guard;
static volatile unsigned long alt_on, alt_off, alt_nested_on;

static void h_alt_usr1(int s)
{
	char local;
	(void)s;
	if (&local >= alt_lo && &local < alt_hi)
		alt_nested_on++;
	siglongjmp(alt_jb, 2);
}

static void h_alt_segv(int s, siginfo_t *si, void *uc)
{
	char local;
	(void)s;
	(void)si;
	(void)uc;
	if (&local >= alt_lo && &local < alt_hi)
		alt_on++;
	else
		alt_off++;
	if (segv_count++ & 1)
		raise(SIGUSR1);
	siglongjmp(alt_jb, 1);
}

static void t_altstack(void)
{
	const size_t sz = 1 << 17;
	alt_lo = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	alt_hi = alt_lo + sz;
	stack_t ss = {.ss_sp = alt_lo, .ss_size = sz, .ss_flags = 0};
	sigaltstack(&ss, NULL);
	alt_guard = mmap(NULL, pagesz, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	setact(SIGSEGV, h_alt_segv, SA_SIGINFO | SA_ONSTACK);
	setact(SIGUSR1, h_alt_usr1, SA_ONSTACK);
	segv_count = 0;
	volatile unsigned long jumped = 0, nested = 0, onstack_flag = 0;
	for (volatile unsigned long i = 0; i < 50000; i++) {
		int v = sigsetjmp(alt_jb, 1);
		if (v == 0) {
			volatile char c = alt_guard[i & 7];
			(void)c;
		} else {
			if (v == 2)
				nested++;
			else
				jumped++;
			if ((i & 255) == 0) {
				stack_t cur;
				sigaltstack(NULL, &cur);
				if (cur.ss_flags & SS_ONSTACK)
					onstack_flag++;
			}
		}
	}
	setact(SIGSEGV, SIG_DFL, 0);
	setact(SIGUSR1, SIG_DFL, 0);
	ss.ss_flags = SS_DISABLE;
	sigaltstack(&ss, NULL);
	munmap(alt_guard, pagesz);
	munmap(alt_lo, sz);
	printf("altstack: jumped=%lu nested-jumped=%lu on-altstack=%lu off-altstack=%lu nested-on-altstack=%lu "
	       "ss_onstack-after-jump=%lu\n",
	       (unsigned long)jumped, (unsigned long)nested, (unsigned long)alt_on, (unsigned long)alt_off,
	       (unsigned long)alt_nested_on, (unsigned long)onstack_flag);
}

/* ---- deep calls inside the handler ---- */
static sigjmp_buf deep_jb;
__attribute__((noinline)) static uint64_t deep(uint64_t n)
{
	if (n == 0)
		siglongjmp(deep_jb, 1);
	uint64_t r = deep(n - 1);
	return mix(r, n);
}

static void h_deep(int s)
{
	(void)s;
	deep(200);
}

static void t_callret(void)
{
	setact(SIGUSR1, h_deep, 0);
	volatile uint64_t h = 0;
	volatile unsigned long jumped = 0;
	for (volatile unsigned long i = 0; i < 20000; i++) {
		if (sigsetjmp(deep_jb, 1) == 0) {
			raise(SIGUSR1);
		} else {
			jumped++;
			h = mix(h, rec(i % 97));
		}
	}
	setact(SIGUSR1, SIG_DFL, 0);
	printf("callret: jumped=%lu sum=%016llx rec=%016llx\n", (unsigned long)jumped, (unsigned long long)h,
	       (unsigned long long)rec(5000));
}

/* ---- deepening ---- */
static jmp_buf deepen_jb;
static void h_deepen(int s)
{
	(void)s;
	longjmp(deepen_jb, 1);
}

__attribute__((noinline)) static uint64_t deepen(uint64_t n)
{
	if (n == 0) {
		raise(SIGUSR2);
		return 0;
	}
	uint64_t r = deepen(n - 1);
	return mix(r, n);
}

static void t_deepening(void)
{
	setact(SIGUSR2, h_deepen, SA_NODEFER);
	volatile uint64_t h = 0;
	volatile unsigned long jumped = 0;
	for (volatile unsigned long i = 0; i < 20000; i++) {
		if (setjmp(deepen_jb) == 0) {
			h = mix(h, deepen(i % 300));
		} else {
			jumped++;
			h = mix(h, rec(i % 50));
		}
	}
	setact(SIGUSR2, SIG_DFL, 0);
	printf("deepening: jumped=%lu sum=%016llx\n", (unsigned long)jumped, (unsigned long long)h);
}

/* ---- timer ---- */
static sigjmp_buf timer_jb;
static volatile unsigned long timer_hits;

static void h_timer(int s)
{
	(void)s;
	timer_hits++;
	siglongjmp(timer_jb, 1);
}

static void t_timer(void)
{
	const unsigned long want = 3000;
	setact(SIGALRM, h_timer, 0);
	struct itimerval it = {.it_interval = {0, 100}, .it_value = {0, 100}};
	volatile unsigned long wrong = 0;
	sigsetjmp(timer_jb, 1);
	if (timer_hits < want) {
		setitimer(ITIMER_REAL, &it, NULL);
		for (;;) {
			/* after every jump the program's own state must be intact */
			if (rec(64) != rec(64))
				wrong++;
			volatile uint64_t x = 1;
			for (unsigned k = 0; k < 1000; k++)
				x = x * 6364136223846793005ull + k;
		}
	}
	memset(&it, 0, sizeof(it));
	setitimer(ITIMER_REAL, &it, NULL);
	setact(SIGALRM, SIG_IGN, 0);
	printf("timer: hits>=%lu state-ok=%s rec=%016llx\n", want, YN(wrong == 0), (unsigned long long)rec(300));
}

/* ---- threads ---- */
static __thread sigjmp_buf thr_jb;
static __thread char *thr_guard;

static void h_thr(int s, siginfo_t *si, void *uc)
{
	(void)s;
	(void)si;
	(void)uc;
	siglongjmp(thr_jb, 1);
}

static void *thr_main(void *arg)
{
	(void)arg;
	thr_guard = mmap(NULL, pagesz, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unsigned long n = 0;
	volatile uint64_t h = 0;
	for (volatile unsigned long i = 0; i < 20000; i++) {
		if (sigsetjmp(thr_jb, 1) == 0) {
			if (i & 1)
				raise(SIGUSR1);
			else
				h = mix(h, (unsigned char)thr_guard[0]);
		} else {
			n++;
			h = mix(h, rec(i % 50));
		}
	}
	munmap(thr_guard, pagesz);
	return (void *)(n + (h == 0 ? 1000000 : 0));
}

static void t_threads(void)
{
	setact(SIGUSR1, h_thr, SA_SIGINFO);
	setact(SIGSEGV, h_thr, SA_SIGINFO);
	pthread_t t[4];
	unsigned long total = 0;
	int created = 0;
	for (int i = 0; i < 4; i++)
		if (pthread_create(&t[i], NULL, thr_main, NULL) == 0)
			created++;
	for (int i = 0; i < created; i++) {
		void *r;
		pthread_join(t[i], &r);
		total += (unsigned long)r;
	}
	setact(SIGUSR1, SIG_DFL, 0);
	setact(SIGSEGV, SIG_DFL, 0);
	printf("threads: created=%d jumped=%lu\n", created, total);
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	pagesz = sysconf(_SC_PAGESIZE);
	t_raise();
	t_longjmp();
	t_segv();
	t_nested();
	t_nodefer_rec();
	t_altstack();
	t_callret();
	t_deepening();
	t_timer();
	t_threads();
	done();
	return 0;
}
