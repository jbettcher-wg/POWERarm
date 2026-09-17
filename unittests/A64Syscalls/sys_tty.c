/* sys_tty: terminal ioctls on a freshly allocated pty.
 * TCGETS/TCSETS/TCSETSW/TCSETSF (raw ioctl with the arm64 KERNEL struct
 * termios, and via libc), TCGETS2/TCSETS2, TIOCGWINSZ/TIOCSWINSZ,
 * TIOCGPGRP/TIOCSPGRP/TIOCSCTTY (in a setsid() child), FIONREAD, TCFLSH.
 * Never depends on whether stdin/stdout are ttys.  The pts number is never
 * printed. */
#include "a64sys.h"
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

/* ---- arm64 (asm-generic) kernel ABI, spelled out locally ---- */
typedef unsigned int k_tcflag_t;
typedef unsigned char k_cc_t;
typedef unsigned int k_speed_t;
#define K_NCCS 19
struct k_termios {
	k_tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
	k_cc_t c_line;
	k_cc_t c_cc[K_NCCS];
};
struct k_termios2 {
	k_tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
	k_cc_t c_line;
	k_cc_t c_cc[K_NCCS];
	k_speed_t c_ispeed, c_ospeed;
};
struct k_winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

#define K_TCGETS 0x5401
#define K_TCSETS 0x5402
#define K_TCSETSW 0x5403
#define K_TCSETSF 0x5404
#define K_TCFLSH 0x540B
#define K_TIOCSCTTY 0x540E
#define K_TIOCGPGRP 0x540F
#define K_TIOCSPGRP 0x5410
#define K_TIOCOUTQ 0x5411
#define K_TIOCGWINSZ 0x5413
#define K_TIOCSWINSZ 0x5414
#define K_FIONREAD 0x541B
#define K_TIOCNOTTY 0x5422
#define K_TIOCGSID 0x5429
#define K_TCGETS2 0x802c542aUL /* _IOR('T', 0x2A, struct termios2) */
#define K_TCSETS2 0x402c542bUL /* _IOW('T', 0x2B, struct termios2) */
#define K_TIOCGPTN 0x80045430UL
#define K_TIOCSPTLCK 0x40045431UL

/* c_cc indices */
static const struct { int idx; const char *n; } ccnames[] = {
	{ 0, "VINTR" }, { 1, "VQUIT" }, { 2, "VERASE" }, { 3, "VKILL" }, { 4, "VEOF" },
	{ 11, "VEOL" }, { 16, "VEOL2" }, { 8, "VSTART" }, { 9, "VSTOP" }, { 10, "VSUSP" },
	{ 6, "VMIN" }, { 5, "VTIME" }, { 12, "VREPRINT" }, { 13, "VDISCARD" }, { 14, "VWERASE" },
	{ 15, "VLNEXT" }, { 7, "VSWTC" },
};

static const struct flagname iflags[] = {
	{ 01, "IGNBRK" }, { 02, "BRKINT" }, { 04, "IGNPAR" }, { 010, "PARMRK" }, { 020, "INPCK" },
	{ 040, "ISTRIP" }, { 0100, "INLCR" }, { 0200, "IGNCR" }, { 0400, "ICRNL" }, { 01000, "IUCLC" },
	{ 02000, "IXON" }, { 04000, "IXANY" }, { 010000, "IXOFF" }, { 020000, "IMAXBEL" }, { 040000, "IUTF8" },
};
static const struct flagname oflagsT[] = {
	{ 01, "OPOST" }, { 02, "OLCUC" }, { 04, "ONLCR" }, { 010, "OCRNL" }, { 020, "ONOCR" },
	{ 040, "ONLRET" }, { 0100, "OFILL" }, { 0200, "OFDEL" }, { 0400, "NL1" },
	{ 03000, "CR3" }, { 01000, "CR1" }, { 02000, "CR2" },
	{ 014000, "TAB3" }, { 04000, "TAB1" }, { 010000, "TAB2" },
	{ 020000, "BS1" }, { 040000, "VT1" }, { 0100000, "FF1" },
};
#define K_CBAUD 0010017
#define K_CBAUDEX 0010000
#define K_CSIZE 0000060
#define K_CIBAUD 002003600000
#define K_BOTHER 0010000
static const struct flagname cflags[] = {
	{ 0100, "CSTOPB" }, { 0200, "CREAD" }, { 0400, "PARENB" }, { 01000, "PARODD" },
	{ 02000, "HUPCL" }, { 04000, "CLOCAL" }, { 010000000000, "CMSPAR" }, { 020000000000, "CRTSCTS" },
};
static const struct flagname lflags[] = {
	{ 01, "ISIG" }, { 02, "ICANON" }, { 04, "XCASE" }, { 010, "ECHO" }, { 020, "ECHOE" },
	{ 040, "ECHOK" }, { 0100, "ECHONL" }, { 0200, "NOFLSH" }, { 0400, "TOSTOP" }, { 01000, "ECHOCTL" },
	{ 02000, "ECHOPRT" }, { 04000, "ECHOKE" }, { 010000, "FLUSHO" }, { 040000, "PENDIN" },
	{ 0100000, "IEXTEN" }, { 0200000, "EXTPROC" },
};

static const char *baudname(unsigned v)
{
	static const struct { unsigned v; const char *n; } t[] = {
		{ 0, "B0" }, { 1, "B50" }, { 2, "B75" }, { 3, "B110" }, { 4, "B134" }, { 5, "B150" },
		{ 6, "B200" }, { 7, "B300" }, { 010, "B600" }, { 011, "B1200" }, { 012, "B1800" },
		{ 013, "B2400" }, { 014, "B4800" }, { 015, "B9600" }, { 016, "B19200" }, { 017, "B38400" },
		{ 010000, "BOTHER" }, { 010001, "B57600" }, { 010002, "B115200" }, { 010003, "B230400" },
		{ 010004, "B460800" }, { 010005, "B500000" }, { 010006, "B576000" }, { 010007, "B921600" },
		{ 010010, "B1000000" }, { 010017, "B4000000" },
	};
	static char buf[16];
	for (size_t i = 0; i < NELEM(t); i++)
		if (t[i].v == v)
			return t[i].n;
	snprintf(buf, sizeof(buf), "0%o", v);
	return buf;
}

static void pr_ktermios(const char *tag, const struct k_termios *t)
{
	char b[FLAGSTR_MAX];
	printf("%s: iflag=%s\n", tag, flagstr(b, t->c_iflag, iflags, NELEM(iflags)));
	printf("%s: oflag=%s\n", tag, flagstr(b, t->c_oflag, oflagsT, NELEM(oflagsT)));
	unsigned cs = t->c_cflag & K_CSIZE;
	printf("%s: cflag=CS%u|%s|baud=%s|ibaud=%s\n", tag, 5 + (cs >> 4),
	       flagstr(b, t->c_cflag & ~(K_CBAUD | K_CSIZE | K_CIBAUD), cflags, NELEM(cflags)),
	       baudname(t->c_cflag & K_CBAUD), baudname((t->c_cflag & K_CIBAUD) >> 16));
	printf("%s: lflag=%s\n", tag, flagstr(b, t->c_lflag, lflags, NELEM(lflags)));
	printf("%s: line=%u cc=", tag, t->c_line);
	for (size_t i = 0; i < NELEM(ccnames); i++)
		printf("%s%s:%u", i ? "," : "", ccnames[i].n, t->c_cc[ccnames[i].idx]);
	printf(" cc17=%u cc18=%u\n", t->c_cc[17], t->c_cc[18]);
}

/* Read up to want bytes, waiting (poll) up to 2s for data. */
static ssize_t read_wait(int fd, char *buf, size_t want)
{
	size_t got = 0;
	while (got < want) {
		struct pollfd p = { fd, POLLIN, 0 };
		int r = poll(&p, 1, 2000);
		if (r <= 0)
			break;
		ssize_t n = read(fd, buf + got, want - got);
		if (n <= 0)
			break;
		got += n;
	}
	return got;
}

static void escape(char *out, const char *in, size_t n)
{
	size_t o = 0;
	for (size_t i = 0; i < n && o < 60; i++) {
		unsigned char c = in[i];
		if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
		else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
		else if (c < 32 || c > 126) o += sprintf(out + o, "\\x%02x", c);
		else out[o++] = c;
	}
	out[o] = 0;
}

int main(void)
{
	errno = 0;
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0) {
		printf("posix_openpt: fail errno=%s\n", errstr(errno));
		done();
		return 0;
	}
	printf("posix_openpt: ok\n");
	printf("grantpt: ret=%d\n", grantpt(m));
	printf("unlockpt: ret=%d\n", unlockpt(m));
	unsigned ptn = 0xffffffff;
	printf("tiocgptn: ret=%d set=%s\n", ioctl(m, K_TIOCGPTN, &ptn), YN(ptn != 0xffffffff));
	char *name = ptsname(m);
	printf("ptsname: prefix-dev-pts=%s\n", YN(name && !strncmp(name, "/dev/pts/", 9)));
	int s = open(name, O_RDWR | O_NOCTTY);
	pr_ok("open-slave", s, errno);
	if (s < 0) {
		done();
		return 0;
	}
	printf("isatty: master=%d slave=%d\n", isatty(m), isatty(s));

	/* Kernel TCGETS */
	struct k_termios kt;
	memset(&kt, 0xee, sizeof(kt));
	long r = syscall(SYS_ioctl, s, K_TCGETS, &kt);
	printf("tcgets-raw-slave: ret=%ld\n", r);
	pr_ktermios("initial", &kt);

	struct k_termios2 kt2;
	memset(&kt2, 0xee, sizeof(kt2));
	r = syscall(SYS_ioctl, s, K_TCGETS2, &kt2);
	printf("tcgets2-raw-slave: ret=%ld ispeed=%u ospeed=%u matches-tcgets=%s\n", r, kt2.c_ispeed, kt2.c_ospeed,
	       YN(!memcmp(&kt2, &kt, sizeof(kt))));

	/* libc view */
	struct termios lt;
	r = tcgetattr(s, &lt);
	printf("tcgetattr: ret=%ld flags-match-raw=%s cc-match-raw=%s ospeed=%s ispeed=%s\n", r,
	       YN(lt.c_iflag == kt.c_iflag && lt.c_oflag == kt.c_oflag && lt.c_cflag == kt.c_cflag &&
	          lt.c_lflag == kt.c_lflag),
	       YN(!memcmp(lt.c_cc, kt.c_cc, K_NCCS)), baudname(cfgetospeed(&lt)), baudname(cfgetispeed(&lt)));

	struct k_termios mt;
	r = syscall(SYS_ioctl, m, K_TCGETS, &mt);
	printf("tcgets-raw-master: ret=%ld\n", r);
	pr_ktermios("master", &mt);

	/* cfmakeraw + TCSETS via libc */
	struct termios raw = lt;
	cfmakeraw(&raw);
	r = tcsetattr(s, TCSANOW, &raw);
	printf("tcsetattr-tcsanow-raw: ret=%ld\n", r);
	syscall(SYS_ioctl, s, K_TCGETS, &kt);
	pr_ktermios("after-cfmakeraw", &kt);

	/* raw TCSETS: set VMIN/VTIME and B9600 */
	kt.c_cc[6] = 3;
	kt.c_cc[5] = 7;
	kt.c_cflag = (kt.c_cflag & ~K_CBAUD) | 015;
	r = syscall(SYS_ioctl, s, K_TCSETS, &kt);
	printf("tcsets-raw: ret=%ld\n", r);
	struct k_termios chk;
	syscall(SYS_ioctl, s, K_TCGETS, &chk);
	printf("tcsets-raw: vmin=%u vtime=%u baud=%s equal=%s\n", chk.c_cc[6], chk.c_cc[5], baudname(chk.c_cflag & K_CBAUD),
	       YN(!memcmp(&chk, &kt, sizeof(kt))));
	tcgetattr(s, &lt);
	printf("cfgetospeed-after-b9600: %s\n", baudname(cfgetospeed(&lt)));

	/* TCSETSW: turn on OPOST|ONLCR, B115200 via libc cfsetspeed */
	kt.c_oflag |= 01 | 04;
	kt.c_cflag = (kt.c_cflag & ~K_CBAUD) | 010002;
	r = syscall(SYS_ioctl, s, K_TCSETSW, &kt);
	syscall(SYS_ioctl, s, K_TCGETS, &chk);
	printf("tcsetsw-raw: ret=%ld oflag-opost-onlcr=%s baud=%s equal=%s\n", r, YN((chk.c_oflag & 05) == 05),
	       baudname(chk.c_cflag & K_CBAUD), YN(!memcmp(&chk, &kt, sizeof(kt))));

	/* data path in raw mode, with ONLCR on output */
	char buf[64], esc[128];
	ssize_t n = write(m, "abc\r", 4);
	ssize_t got = read_wait(s, buf, 4);
	escape(esc, buf, got > 0 ? got : 0);
	printf("raw-master-to-slave: wrote=%zd read=%zd data=\"%s\"\n", n, got, esc);
	n = write(s, "hi\n", 3);
	got = read_wait(m, buf, 4);
	escape(esc, buf, got > 0 ? got : 0);
	printf("onlcr-slave-to-master: wrote=%zd read=%zd data=\"%s\"\n", n, got, esc);

	/* FIONREAD then TCSETSF flushes pending input */
	n = write(m, "pending", 7);
	struct pollfd p = { s, POLLIN, 0 };
	poll(&p, 1, 2000);
	int avail = -1;
	for (int i = 0; i < 200; i++) { /* line discipline delivery is async */
		syscall(SYS_ioctl, s, K_FIONREAD, &avail);
		if (avail == 7)
			break;
		usleep(10000);
	}
	printf("fionread-slave: avail=%d\n", avail);
	kt.c_lflag |= 010; /* ECHO */
	r = syscall(SYS_ioctl, s, K_TCSETSF, &kt);
	avail = -1;
	syscall(SYS_ioctl, s, K_FIONREAD, &avail);
	syscall(SYS_ioctl, s, K_TCGETS, &chk);
	printf("tcsetsf-raw: ret=%ld echo=%s avail-after=%d\n", r, YN(chk.c_lflag & 010), avail);
	kt.c_lflag &= ~010;
	syscall(SYS_ioctl, s, K_TCSETS, &kt);

	/* TCFLSH */
	n = write(m, "xyz", 3);
	poll(&p, 1, 2000);
	for (int i = 0; i < 200; i++) {
		syscall(SYS_ioctl, s, K_FIONREAD, &avail);
		if (avail == 3)
			break;
		usleep(10000);
	}
	r = syscall(SYS_ioctl, s, K_TCFLSH, TCIFLUSH);
	syscall(SYS_ioctl, s, K_FIONREAD, &avail);
	printf("tcflsh-input: ret=%ld avail-after=%d\n", r, avail);
	int outq = -1;
	r = syscall(SYS_ioctl, s, K_TIOCOUTQ, &outq);
	printf("tiocoutq: ret=%ld outq=%d\n", r, outq);
	errno = 0;
	r = syscall(SYS_ioctl, s, K_TCFLSH, 17);
	printf("tcflsh-bad-arg: ret=%ld errno=%s\n", r, errstr(errno));

	/* Canonical mode line assembly */
	tcgetattr(s, &lt);
	lt.c_lflag |= ICANON;
	lt.c_lflag &= ~ECHO;
	lt.c_iflag |= ICRNL;
	printf("tcsetattr-tcsadrain-canon: ret=%d\n", tcsetattr(s, TCSADRAIN, &lt));
	n = write(m, "one\rtwo\n", 8);
	got = read_wait(s, buf, 1); /* canonical read returns one line at a time */
	if (got == 1) {
		ssize_t k = read(s, buf + 1, sizeof(buf) - 1);
		got += k > 0 ? k : 0;
	}
	escape(esc, buf, got);
	printf("canon-read-line1: read=%zd data=\"%s\"\n", got, esc);
	got = read_wait(s, buf, 1);
	if (got == 1) {
		ssize_t k = read(s, buf + 1, sizeof(buf) - 1);
		got += k > 0 ? k : 0;
	}
	escape(esc, buf, got);
	printf("canon-read-line2: read=%zd data=\"%s\"\n", got, esc);

	/* termios2 with BOTHER custom speed */
	syscall(SYS_ioctl, s, K_TCGETS2, &kt2);
	kt2.c_cflag = (kt2.c_cflag & ~(K_CBAUD | K_CIBAUD)) | K_BOTHER;
	kt2.c_ospeed = 12345;
	kt2.c_ispeed = 12345;
	r = syscall(SYS_ioctl, s, K_TCSETS2, &kt2);
	struct k_termios2 c2;
	syscall(SYS_ioctl, s, K_TCGETS2, &c2);
	printf("tcsets2-bother: ret=%ld baud=%s ospeed=%u ispeed=%u\n", r, baudname(c2.c_cflag & K_CBAUD), c2.c_ospeed,
	       c2.c_ispeed);

	/* Window size */
	struct k_winsize ws = { 0, 0, 0, 0 };
	r = syscall(SYS_ioctl, s, K_TIOCGWINSZ, &ws);
	printf("tiocgwinsz-initial: ret=%ld row=%u col=%u xpix=%u ypix=%u\n", r, ws.ws_row, ws.ws_col, ws.ws_xpixel,
	       ws.ws_ypixel);
	struct k_winsize ws2 = { 43, 132, 640, 480 };
	r = syscall(SYS_ioctl, m, K_TIOCSWINSZ, &ws2);
	memset(&ws, 0, sizeof(ws));
	long r2 = syscall(SYS_ioctl, s, K_TIOCGWINSZ, &ws);
	printf("tiocswinsz-master-tiocgwinsz-slave: set=%ld get=%ld row=%u col=%u xpix=%u ypix=%u\n", r, r2, ws.ws_row,
	       ws.ws_col, ws.ws_xpixel, ws.ws_ypixel);
	struct winsize lws;
	r = ioctl(s, TIOCGWINSZ, &lws);
	printf("tiocgwinsz-libc: ret=%ld row=%u col=%u\n", r, lws.ws_row, lws.ws_col);

	/* Process group ioctls: not our controlling tty yet */
	int pg = -1;
	errno = 0;
	r = syscall(SYS_ioctl, s, K_TIOCGPGRP, &pg);
	printf("tiocgpgrp-not-ctty: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_ioctl, s, K_TIOCGSID, &pg);
	printf("tiocgsid-not-ctty: ret=%ld errno=%s\n", r, errstr(errno));

	fflush(stdout);
	pid_t child = fork();
	if (child == 0) {
		setsid();
		errno = 0;
		long cr = syscall(SYS_ioctl, s, K_TIOCSCTTY, 0);
		printf("child-tiocsctty: ret=%ld errno=%s\n", cr, errstr(cr ? errno : 0));
		pg = -1;
		cr = syscall(SYS_ioctl, s, K_TIOCGPGRP, &pg);
		printf("child-tiocgpgrp: ret=%ld matches-getpgrp=%s\n", cr, YN(pg == getpgrp()));
		cr = syscall(SYS_ioctl, s, K_TIOCGSID, &pg);
		printf("child-tiocgsid: ret=%ld matches-getsid=%s\n", cr, YN(pg == getsid(0)));
		pg = getpgrp();
		cr = syscall(SYS_ioctl, s, K_TIOCSPGRP, &pg);
		printf("child-tiocspgrp-self: ret=%ld\n", cr);
		printf("child-tcgetpgrp-libc: matches=%s\n", YN(tcgetpgrp(s) == getpgrp()));
		int bogus = 0x7ffffff0;
		errno = 0;
		cr = syscall(SYS_ioctl, s, K_TIOCSPGRP, &bogus);
		printf("child-tiocspgrp-bogus: ret=%ld errno=%s\n", cr, errstr(errno));
		bogus = -5;
		errno = 0;
		cr = syscall(SYS_ioctl, s, K_TIOCSPGRP, &bogus);
		printf("child-tiocspgrp-negative: ret=%ld errno=%s\n", cr, errstr(errno));
		int fd = open("/dev/tty", O_RDWR);
		printf("child-open-dev-tty: ok=%s\n", YN(fd >= 0));
		if (fd >= 0)
			close(fd);
		/* Detach: we are the session leader so we get SIGHUP semantics; ignore */
		fflush(stdout);
		_exit(0);
	}
	int st;
	waitpid(child, &st, 0);
	printf("child: exited=%s code=%d\n", YN(WIFEXITED(st)), WIFEXITED(st) ? WEXITSTATUS(st) : -1);

	/* Errors */
	int dn = open("/dev/null", O_RDWR);
	errno = 0;
	r = syscall(SYS_ioctl, dn, K_TCGETS, &kt);
	printf("tcgets-devnull: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_ioctl, dn, K_TIOCGWINSZ, &ws);
	printf("tiocgwinsz-devnull: ret=%ld errno=%s\n", r, errstr(errno));
	close(dn);
	errno = 0;
	r = syscall(SYS_ioctl, s, K_TCGETS, (void *)8);
	printf("tcgets-bad-ptr: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_ioctl, s, K_TCSETS, (void *)8);
	printf("tcsets-bad-ptr: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_ioctl, 9999, K_TCGETS, &kt);
	printf("tcgets-bad-fd: ret=%ld errno=%s\n", r, errstr(errno));
	errno = 0;
	r = syscall(SYS_ioctl, s, 0x54ff, 0);
	printf("unknown-ioctl-pty: ret=%ld errno=%s\n", r, errstr(errno));

	/* Close slave, master read gets EIO */
	close(s);
	errno = 0;
	n = read(m, buf, sizeof(buf));
	printf("master-read-after-slave-close: ret=%zd errno=%s\n", n, errstr(n < 0 ? errno : 0));
	close(m);
	done();
	return 0;
}
