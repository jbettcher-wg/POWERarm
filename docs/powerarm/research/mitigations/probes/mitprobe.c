/* mitprobe.c -- price POWER9 kernel entry/exit mitigations against the
** POWERarm dispatch structures.  Native ppc64le; build with the host gcc:
**   /mnt/arch/usr/bin/gcc -O2 -mcpu=power9 -fno-pie -no-pie -o mitprobe mitprobe.c
**
** Kernels, each a loop of "units", with a chosen syscall injected every R units:
**   null     dependent addi chain          -- no cache/predictor state to lose
**   probe    the emitted L1 lookup probe   -- 2 dependent loads, 2 MiB table
**   ccache   ring of monomorphic bctr      -- count cache
**   ctrl     the same ring without bctr    -- subtracts the ring's own loads
**   lstack   nested bl/blr to depth 24     -- link stack
** The per-syscall cost of `null` is the syscall itself (mitigations included,
** nothing to re-warm); any other kernel minus `null` is that kernel's re-warm
** tax on the structure it depends on.
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>

/* ---------------- the lookup cache, shaped exactly like LookupCache.h ------- */
/* MAX_L1_ENTRIES = 128 * 1024, sizeof(LookupCacheEntry) = 16, GUEST_PC_SHIFT = 2 */
#define L1_ENTRIES (128u * 1024u)
#define L1_BYTES   ((size_t)L1_ENTRIES * 16u)
typedef struct { uint64_t HostCode; uint64_t GuestCode; } L1Entry;
static L1Entry *l1;
static uint64_t *stateframe;      /* State.L1Pointer lives at offset 0 */

/* ---------------- perf self counters --------------------------------------- */
#define MAXEV 6
static int    ev_fd[MAXEV];
static char  *ev_nm[MAXEV];
static long long ev_v[MAXEV];
static int    nev;

static int open_ev(uint32_t type, uint64_t cfg, const char *nm) {
  struct perf_event_attr a;
  memset(&a, 0, sizeof a);
  a.type = type; a.size = sizeof a; a.config = cfg;
  a.disabled = 1; a.exclude_kernel = 1; a.exclude_hv = 1; a.inherit = 0;
  int fd = syscall(__NR_perf_event_open, &a, 0, -1, -1, 0);
  if (fd < 0) { fprintf(stderr, "perf_event_open(%s): %s\n", nm, strerror(errno)); return -1; }
  ev_fd[nev] = fd; ev_nm[nev] = strdup(nm); nev++;
  return 0;
}
static void ev_start(void) {
  for (int i = 0; i < nev; i++) { ioctl(ev_fd[i], PERF_EVENT_IOC_RESET, 0); ioctl(ev_fd[i], PERF_EVENT_IOC_ENABLE, 0); }
}
static void ev_stop(void) {
  for (int i = 0; i < nev; i++) {
    ioctl(ev_fd[i], PERF_EVENT_IOC_DISABLE, 0);
    if (read(ev_fd[i], &ev_v[i], sizeof ev_v[i]) != (ssize_t)sizeof ev_v[i]) ev_v[i] = -1;
  }
}

/* ---------------- syscall victims ------------------------------------------ */
static void *scratch;
static volatile uint64_t sink;

__attribute__((noinline)) static void sc_none(void)    { __asm__ __volatile__("" ::: "memory"); }
__attribute__((noinline)) static void sc_getppid(void) { sink = syscall(__NR_getppid); }
__attribute__((noinline)) static void sc_madvise(void) { sink = syscall(__NR_madvise, scratch, 65536, MADV_NORMAL); }
__attribute__((noinline)) static void sc_mprotect(void){ sink = syscall(__NR_mprotect, scratch, 65536, PROT_READ|PROT_WRITE); }
__attribute__((noinline)) static void sc_dontneed(void){ sink = syscall(__NR_madvise, scratch, 65536, MADV_DONTNEED); }
__attribute__((noinline)) static void sc_vdso(void)    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); sink = ts.tv_nsec; }

/* ---------------- kernels -------------------------------------------------- */
__attribute__((noinline)) static uint64_t k_null(uint64_t x, long n) {
  __asm__ __volatile__(
    "mtctr %1\n"
    "1:\n\t addi %0,%0,1\n\t addi %0,%0,1\n\t addi %0,%0,1\n\t addi %0,%0,1\n\t"
    "bdnz 1b\n"
    : "+r"(x) : "r"(n) : "ctr");
  return x;
}

/* The spec_store_bypass mitigation is "Kernel entry/exit barrier (eieio)":
** one eieio on entry and one on exit.  Price an eieio in user mode, quiet and
** with a pending store, against k_null's identical addi chain. */
__attribute__((noinline)) static uint64_t k_eieio(uint64_t x, long n) {
  __asm__ __volatile__(
    "mtctr %1\n"
    "1:\n\t eieio\n\t addi %0,%0,1\n\t addi %0,%0,1\n\t addi %0,%0,1\n\t addi %0,%0,1\n\t"
    "bdnz 1b\n"
    : "+r"(x) : "r"(n) : "ctr", "memory");
  return x;
}
__attribute__((noinline)) static uint64_t k_eieiost(uint64_t x, long n, uint64_t *p) {
  __asm__ __volatile__(
    "mtctr %[n]\n"
    "1:\n\t std %[x], 0(%[p])\n\t eieio\n\t addi %[x],%[x],1\n\t addi %[x],%[x],1\n\t"
    "addi %[x],%[x],1\n\t addi %[x],%[x],1\n\t bdnz 1b\n"
    : [x]"+r"(x) : [n]"r"(n), [p]"r"(p) : "ctr", "memory");
  return x;
}
__attribute__((noinline)) static uint64_t k_nullst(uint64_t x, long n, uint64_t *p) {
  __asm__ __volatile__(
    "mtctr %[n]\n"
    "1:\n\t std %[x], 0(%[p])\n\t addi %[x],%[x],1\n\t addi %[x],%[x],1\n\t"
    "addi %[x],%[x],1\n\t addi %[x],%[x],1\n\t bdnz 1b\n"
    : [x]"+r"(x) : [n]"r"(n), [p]"r"(p) : "ctr", "memory");
  return x;
}

/* The emitted probe from BranchOps.cpp: ld L1Pointer, rlwinm index, add,
** ld key at +8, cmpd, not-taken bne to the miss leg, xor dep, ldx HostCode.
** HostCode holds the next guest PC, so the whole thing is one dependent chain. */
__attribute__((noinline)) static uint64_t k_probe(uint64_t pc, long n, uint64_t *st) {
  uint64_t t2, t3, t4;
  __asm__ __volatile__(
    "mtctr %[n]\n"
    "1:\n\t"
    "ld     %[t2], 0(%[st])\n\t"
    "rlwinm %[t4], %[pc], 2, 11, 27\n\t"
    "add    %[t2], %[t2], %[t4]\n\t"
    "ld     %[t4], 8(%[t2])\n\t"
    "cmpd   7, %[t4], %[pc]\n\t"
    "bne    7, 2f\n\t"
    "xor    %[t3], %[t4], %[t4]\n\t"
    "ldx    %[pc], %[t2], %[t3]\n\t"
    "bdnz   1b\n\t"
    "b      3f\n"
    "2:\t trap\n"
    "3:\n"
    : [pc]"+r"(pc), [t2]"=&r"(t2), [t3]"=&r"(t3), [t4]"=&r"(t4)
    : [st]"r"(st), [n]"r"(n) : "ctr", "cr7", "memory");
  return pc;
}

/* 256 monomorphic bctr sites, 64 bytes apart (clear of the 32-byte
** branch-density artifact).  slots[i] holds the address of site i+1; the
** caller shortens the ring by pointing slots[S-1] back at the stub. */
#define NSITES 512
extern void ccache_ring(long n, uint64_t *slots);
extern void ctrl_ring(long n, uint64_t *slots);
extern uint64_t ccache_site_addr(long i);
extern void lstack_chain(long n);

__asm__(
".text\n"
".globl ccache_ring\n"
".type ccache_ring,@function\n"
".balign 64\n"
"ccache_ring:\n"
"  mr 5, 3\n"              /* r5 = iteration count */
"  std 31, -16(1)\n"
"  mr 31, 4\n"             /* r31 = slots base */
"  b ccache_s0\n"
".balign 64\n"
"ccache_s0:\n"
".set CCI, 0\n"
".rept 512\n"
"  ld 12, CCI*8(31)\n"
"  mtctr 12\n"
"  bctr\n"
".set CCI, CCI+1\n"
".balign 64\n"
".endr\n"
"ccache_stub:\n"
"  addic. 5, 5, -1\n"
"  beq 8f\n"
"  b ccache_s0\n"
"8:\n"
"  ld 31, -16(1)\n"
"  blr\n"
".size ccache_ring,.-ccache_ring\n"

".globl ctrl_ring\n"
".type ctrl_ring,@function\n"
".balign 64\n"
"ctrl_ring:\n"
"  mr 5, 3\n"
"  std 31, -16(1)\n"
"  mr 31, 4\n"
"  li 6, 0\n"
"  b ctrl_s0\n"
".balign 64\n"
"ctrl_s0:\n"
".set CTI, 0\n"
".rept 512\n"
"  ld 12, CTI*8(31)\n"
"  add 6, 6, 12\n"
"  nop\n"
".set CTI, CTI+1\n"
".balign 64\n"
".endr\n"
"  addic. 5, 5, -1\n"
"  beq 9f\n"
"  b ctrl_s0\n"
"9:\n"
"  ld 31, -16(1)\n"
"  blr\n"
".size ctrl_ring,.-ctrl_ring\n"

".globl ccache_site_addr\n"
".type ccache_site_addr,@function\n"
"ccache_site_addr:\n"
"  cmpdi 3, -1\n"
"  bne 1f\n"
"  lis 3, ccache_stub@highest\n"
"  ori 3, 3, ccache_stub@higher\n"
"  rldicr 3, 3, 32, 31\n"
"  oris 3, 3, ccache_stub@h\n"
"  ori 3, 3, ccache_stub@l\n"
"  blr\n"
"1:\n"
"  lis 4, ccache_s0@highest\n"
"  ori 4, 4, ccache_s0@higher\n"
"  rldicr 4, 4, 32, 31\n"
"  oris 4, 4, ccache_s0@h\n"
"  ori 4, 4, ccache_s0@l\n"
"  sldi  3, 3, 6\n"
"  add   3, 4, 3\n"
"  blr\n"
".size ccache_site_addr,.-ccache_site_addr\n"

/* depth-24 nested bl/blr, each level a 64-byte block that saves LR. */
".globl lstack_chain\n"
".type lstack_chain,@function\n"
".balign 64\n"
"lstack_chain:\n"
"  mflr 0\n"
"  std 0, 16(1)\n"
"  stdu 1, -2048(1)\n"
"  mr 5, 3\n"
"lst_loop:\n"
"  bl lst_lvl0\n"
"  addic. 5, 5, -1\n"
"  bne lst_loop\n"
"  addi 1, 1, 2048\n"
"  ld 0, 16(1)\n"
"  mtlr 0\n"
"  blr\n"
".balign 64\n"
"lst_lvl0:\n"
".rept 24\n"
"  mflr 0\n"
"  std 0, 16(1)\n"
"  stdu 1, -64(1)\n"
"  bl . + 52\n"
"  addi 1, 1, 64\n"
"  ld 0, 16(1)\n"
"  mtlr 0\n"
"  blr\n"
".balign 64\n"
".endr\n"
"  blr\n"
".size lstack_chain,.-lstack_chain\n"
);

/* The syscall taken at the bottom of a 24-deep bl/blr chain: 24 returns are
** pending on the link stack across the kernel entry/exit. */
typedef void (*scfn0)(void);
__attribute__((noinline)) static uint64_t deepcall(int d, scfn0 f) {
  if (d == 0) { f(); return 1; }
  uint64_t r = deepcall(d - 1, f);
  __asm__ __volatile__("" ::: "memory");
  return r + 1;
}

/* ---------------- drivers -------------------------------------------------- */
static uint64_t *slots;
static uint64_t chain_start;

static void build_probe_chain(unsigned hot) {
  /* hot distinct guest PCs with distinct L1 slots, wired into a cycle. */
  uint64_t *pcs = malloc(hot * sizeof(uint64_t));
  uint8_t *used = calloc(L1_ENTRIES, 1);
  uint64_t s = 0x9E3779B97F4A7C15ull;
  for (unsigned i = 0; i < hot; i++) {
    uint64_t pc, idx;
    for (;;) {
      s ^= s << 13; s ^= s >> 7; s ^= s << 17;
      /* a 256 MiB guest text range, 4-byte aligned */
      pc = 0x400000ull + ((s >> 11) & 0x0FFFFFFCull);
      idx = (pc >> 2) & (L1_ENTRIES - 1);
      if (!used[idx]) { used[idx] = 1; break; }
    }
    pcs[i] = pc;
  }
  memset(l1, 0, L1_BYTES);
  for (unsigned i = 0; i < hot; i++) {
    uint64_t idx = (pcs[i] >> 2) & (L1_ENTRIES - 1);
    l1[idx].GuestCode = pcs[i];
    l1[idx].HostCode  = pcs[(i + 1) % hot];
  }
  chain_start = pcs[0];
  free(used); free(pcs);
}

static void build_ccache_ring(int sites) {
  for (int i = 0; i < sites - 1; i++) slots[i] = ccache_site_addr(i + 1);
  slots[sites - 1] = ccache_site_addr(-1);          /* back to the stub */
  for (int i = sites; i < NSITES; i++) slots[i] = ccache_site_addr(-1);
}

static double now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e9 + ts.tv_nsec;
}

typedef void (*scfn)(void);

int main(int argc, char **argv) {
  /* args: kernel sysmode units R [hot|sites] [reps] [raw events...] */
  if (argc < 5) {
    fprintf(stderr, "usage: %s <null|probe|ccache|ctrl|lstack> <none|getppid|madvise|mprotect|dontneed|vdso>"
                    " <units> <R> [hot|sites] [reps] [rawev...]\n", argv[0]);
    return 2;
  }
  const char *kn = argv[1], *sn = argv[2];
  long units = atol(argv[3]), R = atol(argv[4]);
  long param = argc > 5 ? atol(argv[5]) : 1024;
  int reps = argc > 6 ? atoi(argv[6]) : 5;
  if (R < 1) R = 1;

  l1 = mmap(NULL, L1_BYTES, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (l1 == MAP_FAILED) { perror("mmap l1"); return 2; }
  madvise(l1, L1_BYTES, MADV_POPULATE_WRITE);
  stateframe = mmap(NULL, 65536, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  stateframe[0] = (uint64_t)l1;
  scratch = mmap(NULL, 65536, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  memset(scratch, 1, 65536);
  slots = mmap(NULL, 65536, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

  scfn sc = sc_none;
  if      (!strcmp(sn, "getppid"))  sc = sc_getppid;
  else if (!strcmp(sn, "madvise"))  sc = sc_madvise;
  else if (!strcmp(sn, "mprotect")) sc = sc_mprotect;
  else if (!strcmp(sn, "dontneed")) sc = sc_dontneed;
  else if (!strcmp(sn, "vdso"))     sc = sc_vdso;
  else if (strcmp(sn, "none"))      { fprintf(stderr, "bad sysmode\n"); return 2; }

  if (!strcmp(kn, "probe"))  build_probe_chain((unsigned)param);
  if (!strcmp(kn, "ccache") || !strcmp(kn, "ctrl")) {
    if (param < 2) param = 2;
    if (param > NSITES) param = NSITES;
    build_ccache_ring((int)param);
  }

  open_ev(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, "cyc_u");
  for (int i = 7; i < argc; i++) {
    uint64_t cfg = strtoull(argv[i], NULL, 0);
    open_ev(PERF_TYPE_RAW, cfg, argv[i]);
  }

  long outer = units / R; if (outer < 1) outer = 1;
  double best = 1e30; long long bcyc = 0; long long bev[MAXEV] = {0};
  uint64_t guard = 0;

  for (int r = 0; r < reps + 1; r++) {               /* rep 0 is a warm-up */
    uint64_t pc = chain_start;
    ev_start();
    double t0 = now_ns();
    if (!strcmp(kn, "null")) {
      for (long o = 0; o < outer; o++) { guard += k_null(guard, R); sc(); }
    } else if (!strcmp(kn, "eieio")) {
      for (long o = 0; o < outer; o++) { guard += k_eieio(guard, R); sc(); }
    } else if (!strcmp(kn, "eieiost")) {
      for (long o = 0; o < outer; o++) { guard += k_eieiost(guard, R, (uint64_t *)scratch); sc(); }
    } else if (!strcmp(kn, "nullst")) {
      for (long o = 0; o < outer; o++) { guard += k_nullst(guard, R, (uint64_t *)scratch); sc(); }
    } else if (!strcmp(kn, "probe")) {
      for (long o = 0; o < outer; o++) { pc = k_probe(pc, R, stateframe); sc(); }
      guard += pc;
    } else if (!strcmp(kn, "ccache")) {
      long inner = R / param; if (inner < 1) inner = 1;
      for (long o = 0; o < outer; o++) { ccache_ring(inner, slots); sc(); }
    } else if (!strcmp(kn, "ctrl")) {
      long inner = R / param; if (inner < 1) inner = 1;
      for (long o = 0; o < outer; o++) { ctrl_ring(inner, slots); sc(); }
    } else if (!strcmp(kn, "lstack")) {
      for (long o = 0; o < outer; o++) { lstack_chain(R); sc(); }
    } else if (!strcmp(kn, "lstackdeep")) {
      for (long o = 0; o < outer; o++) { guard += deepcall((int)param, sc); }
    } else { fprintf(stderr, "bad kernel\n"); return 2; }
    double t1 = now_ns();
    ev_stop();
    if (r == 0) continue;
    if (t1 - t0 < best) { best = t1 - t0; bcyc = ev_v[0]; for (int i = 0; i < nev; i++) bev[i] = ev_v[i]; }
  }

  long realunits = outer * R;
  if (!strcmp(kn, "ccache") || !strcmp(kn, "ctrl")) {
    long inner = R / param; if (inner < 1) inner = 1;
    realunits = outer * inner * param;
  }
  printf("kernel=%s sys=%s param=%ld R=%ld units=%ld syscalls=%ld "
         "ns=%.0f ns_per_unit=%.4f cyc_per_unit=%.4f cyc_per_syscall=%.1f",
         kn, sn, param, R, realunits, outer, best,
         best / realunits, (double)bcyc / realunits, (double)bcyc / outer);
  for (int i = 1; i < nev; i++) printf(" %s=%.4f/unit", ev_nm[i], (double)bev[i] / realunits);
  for (int i = 1; i < nev; i++) printf(" %s_per_sys=%.3f", ev_nm[i], (double)bev[i] / outer);
  printf(" guard=%llx\n", (unsigned long long)guard);
  return 0;
}
