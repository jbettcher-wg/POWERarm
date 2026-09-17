/* SPDX-License-Identifier: MIT */
/* Several functions exercising what a small compiler's code generator has to
 * get right: recursion, structs by value and pointer, switch tables, 64-bit
 * and unsigned arithmetic, doubles, function pointers and varargs-free
 * callbacks. Output only through write(2) via a tiny local formatter. */
#include <unistd.h>

struct point {
	long x, y;
	double w;
};

static void put(const char *s)
{
	const char *e = s;
	while (*e)
		e++;
	(void)!write(1, s, e - s);
}

static void put_u64(unsigned long long v)
{
	char buf[24];
	int i = sizeof(buf);
	buf[--i] = 0;
	do {
		buf[--i] = '0' + v % 10;
		v /= 10;
	} while (v);
	put(buf + i);
}

static void put_i64(long long v)
{
	if (v < 0) {
		put("-");
		put_u64(-(unsigned long long)v);
	} else {
		put_u64(v);
	}
}

static unsigned long long fib(unsigned n)
{
	return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static struct point add(struct point a, struct point b)
{
	struct point r = { a.x + b.x, a.y + b.y, a.w * b.w };
	return r;
}

static const char *classify(int v)
{
	switch (v) {
	case 0: return "zero";
	case 1: return "one";
	case 2: return "two";
	case 3: return "three";
	case 7: return "seven";
	case 42: return "answer";
	default: return v < 0 ? "negative" : "other";
	}
}

static long apply(long (*f)(long, long), long a, long b)
{
	return f(a, b);
}

static long mul(long a, long b) { return a * b; }
static long divi(long a, long b) { return b ? a / b : 0; }

int main(void)
{
	put("fib(30)=");
	put_u64(fib(30));
	put("\n");

	struct point p = add((struct point){ 3, -4, 1.5 }, (struct point){ 10, 20, 4.0 });
	put("point=");
	put_i64(p.x);
	put(",");
	put_i64(p.y);
	put(",w*1000=");
	put_i64((long long)(p.w * 1000.0));
	put("\n");

	for (int i = -1; i <= 8; i += 3) {
		put(classify(i));
		put(" ");
	}
	put(classify(42));
	put("\n");

	unsigned long long h = 1469598103934665603ULL;
	for (int i = 0; i < 1000; i++)
		h = (h ^ (unsigned)i) * 1099511628211ULL;
	put("fnv=");
	put_u64(h);
	put("\n");

	put("apply=");
	put_i64(apply(mul, -7, 6));
	put(",");
	put_i64(apply(divi, -100, 7));
	put(",shift=");
	put_i64(-1234567890123LL >> 5);
	put(",ushift=");
	put_u64(0xfedcba9876543210ULL >> 7);
	put("\n");

	double acc = 0;
	for (int i = 1; i <= 100; i++)
		acc += 1.0 / (i * (double)i);
	put("basel*1e9=");
	put_i64((long long)(acc * 1e9));
	put("\n");
	return (int)(fib(10) % 7);
}
