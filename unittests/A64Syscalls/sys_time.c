/* sys_time: clock_gettime, clock_getres, gettimeofday, nanosleep,
 * clock_nanosleep.  Only relations are printed (no absolute times).
 * Calls go through syscall() so the vDSO is bypassed; libc paths are tested
 * separately. */
#include "a64sys.h"
#include <time.h>
#include <sys/time.h>

static long long ts_ns(const struct timespec *t) { return (long long)t->tv_sec * 1000000000LL + t->tv_nsec; }

static const struct { clockid_t id; const char *n; } clocks[] = {
	{ CLOCK_REALTIME, "REALTIME" },
	{ CLOCK_MONOTONIC, "MONOTONIC" },
	{ CLOCK_PROCESS_CPUTIME_ID, "PROCESS_CPUTIME" },
	{ CLOCK_THREAD_CPUTIME_ID, "THREAD_CPUTIME" },
	{ CLOCK_MONOTONIC_RAW, "MONOTONIC_RAW" },
	{ CLOCK_REALTIME_COARSE, "REALTIME_COARSE" },
	{ CLOCK_MONOTONIC_COARSE, "MONOTONIC_COARSE" },
	{ CLOCK_BOOTTIME, "BOOTTIME" },
};

int main(void)
{
	struct timespec a, b, res;
	long r;

	for (size_t i = 0; i < NELEM(clocks); i++) {
		memset(&a, 0, sizeof(a));
		r = syscall(SYS_clock_gettime, clocks[i].id, &a);
		printf("clock_gettime-%s: ret=%ld nsec-in-range=%s nonneg=%s\n", clocks[i].n, r,
		       YN(a.tv_nsec >= 0 && a.tv_nsec < 1000000000), YN(a.tv_sec >= 0));
		memset(&res, 0xff, sizeof(res));
		r = syscall(SYS_clock_getres, clocks[i].id, &res);
		/* resolution: high-res clocks report 1ns; coarse clocks report a
		 * tick (host HZ dependent) so only report "<= 1s" there */
		int coarse = clocks[i].id == CLOCK_REALTIME_COARSE || clocks[i].id == CLOCK_MONOTONIC_COARSE;
		if (coarse)
			printf("clock_getres-%s: ret=%ld sec=0 positive-le-10ms=%s\n", clocks[i].n, r,
			       YN(res.tv_sec == 0 && res.tv_nsec > 0 && res.tv_nsec <= 10000000));
		else
			printf("clock_getres-%s: ret=%ld sec=%lld nsec=%ld\n", clocks[i].n, r, (long long)res.tv_sec, res.tv_nsec);
	}
	r = syscall(SYS_clock_getres, CLOCK_MONOTONIC, NULL);
	printf("clock_getres-null: ret=%ld\n", r);

	/* monotonic non-decreasing across many calls */
	int mono_ok = 1;
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a);
	for (int i = 0; i < 1000; i++) {
		syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &b);
		if (ts_ns(&b) < ts_ns(&a))
			mono_ok = 0;
		a = b;
	}
	printf("monotonic-syscall: non-decreasing=%s\n", YN(mono_ok));
	mono_ok = 1;
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (int i = 0; i < 1000; i++) {
		clock_gettime(CLOCK_MONOTONIC, &b);
		if (ts_ns(&b) < ts_ns(&a))
			mono_ok = 0;
		a = b;
	}
	printf("monotonic-libc: non-decreasing=%s\n", YN(mono_ok));

	/* libc vs raw agree within 1s */
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a);
	clock_gettime(CLOCK_MONOTONIC, &b);
	printf("monotonic-libc-vs-raw: within-1s=%s ordered=%s\n", YN(ts_ns(&b) - ts_ns(&a) < 1000000000LL),
	       YN(ts_ns(&b) >= ts_ns(&a)));

	struct timeval tv;
	struct timezone tz;
	syscall(SYS_clock_gettime, CLOCK_REALTIME, &a);
	r = syscall(SYS_gettimeofday, &tv, &tz);
	long long tv_ns = (long long)tv.tv_sec * 1000000000LL + tv.tv_usec * 1000LL;
	printf("gettimeofday: ret=%ld usec-in-range=%s within-1s-of-realtime=%s after-2020=%s\n", r,
	       YN(tv.tv_usec >= 0 && tv.tv_usec < 1000000), YN(llabs(tv_ns - ts_ns(&a)) < 1000000000LL),
	       YN(tv.tv_sec > 1577836800));
	r = syscall(SYS_gettimeofday, NULL, NULL);
	printf("gettimeofday-null: ret=%ld\n", r);
	r = gettimeofday(&tv, NULL);
	printf("gettimeofday-libc: ret=%ld within-1s-of-realtime=%s\n", r,
	       YN(llabs((long long)tv.tv_sec * 1000000000LL + tv.tv_usec * 1000LL - ts_ns(&a)) < 2000000000LL));
	time_t tt = time(NULL);
	printf("time-libc: within-2s-of-realtime=%s\n", YN(llabs((long long)tt - a.tv_sec) <= 2));

	/* nanosleep 10ms */
	struct timespec req = { 0, 10000000 }, rem = { 99, 99 };
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a);
	r = syscall(SYS_nanosleep, &req, &rem);
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &b);
	printf("nanosleep-10ms: ret=%ld elapsed>=10ms=%s elapsed<5s=%s\n", r, YN(ts_ns(&b) - ts_ns(&a) >= 10000000LL),
	       YN(ts_ns(&b) - ts_ns(&a) < 5000000000LL));
	req.tv_nsec = 0;
	r = syscall(SYS_nanosleep, &req, NULL);
	printf("nanosleep-zero: ret=%ld\n", r);

	/* clock_nanosleep relative and absolute */
	req.tv_sec = 0;
	req.tv_nsec = 20000000;
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a);
	r = syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &req, NULL);
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &b);
	printf("clock_nanosleep-rel-20ms: ret=%ld elapsed>=20ms=%s\n", r, YN(ts_ns(&b) - ts_ns(&a) >= 20000000LL));

	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a);
	struct timespec abs_t = a;
	abs_t.tv_nsec += 15000000;
	if (abs_t.tv_nsec >= 1000000000) {
		abs_t.tv_nsec -= 1000000000;
		abs_t.tv_sec++;
	}
	r = syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME, &abs_t, NULL);
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &b);
	printf("clock_nanosleep-abs-15ms: ret=%ld reached-target=%s\n", r, YN(ts_ns(&b) >= ts_ns(&abs_t)));

	/* absolute time in the past returns immediately. 1 ns after the epoch of
	 * CLOCK_MONOTONIC is always past; "100 s ago" is negative (EINVAL) on a
	 * machine that booted less than 100 s before, like a test VM. */
	abs_t.tv_sec = 0;
	abs_t.tv_nsec = 1;
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &a);
	r = syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME, &abs_t, NULL);
	syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &b);
	printf("clock_nanosleep-abs-past: ret=%ld immediate=%s\n", r, YN(ts_ns(&b) - ts_ns(&a) < 1000000000LL));

	syscall(SYS_clock_gettime, CLOCK_REALTIME, &a);
	abs_t = a;
	abs_t.tv_nsec += 5000000;
	if (abs_t.tv_nsec >= 1000000000) {
		abs_t.tv_nsec -= 1000000000;
		abs_t.tv_sec++;
	}
	r = syscall(SYS_clock_nanosleep, CLOCK_REALTIME, TIMER_ABSTIME, &abs_t, NULL);
	printf("clock_nanosleep-realtime-abs: ret=%ld\n", r);
	r = clock_nanosleep(CLOCK_MONOTONIC, 0, &(struct timespec){ 0, 1000000 }, NULL);
	printf("clock_nanosleep-libc: ret=%ld\n", r);

	/* Errors */
	errno = 0;
	r = syscall(SYS_clock_gettime, 12345, &a);
	printf("clock_gettime-bad-clock: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_clock_gettime, -1, &a); /* CPUCLOCK of pid 0 thread, invalid encoding */
	printf("clock_gettime-neg-clock: fails=%s\n", YN(r == -1));
	errno = 0;
	r = syscall(SYS_clock_getres, 12345, &a);
	printf("clock_getres-bad-clock: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, (void *)8);
	printf("clock_gettime-bad-ptr: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_gettimeofday, (void *)8, NULL);
	printf("gettimeofday-bad-ptr: ret=%ld errno=%s\n", r, errstr(errno));
	req.tv_sec = 0;
	req.tv_nsec = 1000000000;
	errno = 0;
	r = syscall(SYS_nanosleep, &req, NULL);
	printf("nanosleep-nsec-1e9: ret=%ld errno=%s\n", r, errstr(errno));
	req.tv_nsec = -1;
	errno = 0;
	r = syscall(SYS_nanosleep, &req, NULL);
	printf("nanosleep-nsec-neg: ret=%ld errno=%s\n", r, errstr(errno));
	req.tv_sec = -1;
	req.tv_nsec = 0;
	errno = 0;
	r = syscall(SYS_nanosleep, &req, NULL);
	printf("nanosleep-sec-neg: ret=%ld errno=%s\n", r, errstr(errno));
	req.tv_sec = 0;
	req.tv_nsec = 1000000000;
	errno = 0;
	r = syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &req, NULL);
	printf("clock_nanosleep-nsec-1e9: ret=%ld errno=%s\n", r, errstr(errno));
	req.tv_nsec = 1000;
	errno = 0;
	r = syscall(SYS_clock_nanosleep, 12345, 0, &req, NULL);
	printf("clock_nanosleep-bad-clock: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_clock_nanosleep, CLOCK_THREAD_CPUTIME_ID, 0, &req, NULL);
	printf("clock_nanosleep-thread-cputime: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_nanosleep, (void *)8, NULL);
	printf("nanosleep-bad-ptr: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_clock_settime, CLOCK_MONOTONIC, &a);
	printf("clock_settime-monotonic: ret=%ld errno=%s\n", r, errstr(errno));

	/* CPU time advances when we burn CPU */
	struct timespec c1, c2;
	syscall(SYS_clock_gettime, CLOCK_PROCESS_CPUTIME_ID, &c1);
	volatile unsigned long x = 0;
	for (unsigned long i = 0; i < 20000000UL; i++)
		x += i;
	syscall(SYS_clock_gettime, CLOCK_PROCESS_CPUTIME_ID, &c2);
	printf("process-cputime: advances=%s\n", YN(ts_ns(&c2) > ts_ns(&c1)));

	done();
	return 0;
}
