// va_probe.c - user VA-size probe for the POWERarm VA research.
// Build: cc -O1 -pie -fPIE -o va_probe va_probe.c
// Runs identically on arm64 and ppc64le. No privileges needed.
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static long pg;
static int fails;

static void *try_map(uintptr_t addr, size_t len, int extra, int *err) {
  void *p = mmap((void *)addr, len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | extra, -1, 0);
  *err = (p == MAP_FAILED) ? errno : 0;
  return p;
}

static const char *bits(uintptr_t a) {
  static char b[32];
  if (!a) return "-";
  int hi = 63 - __builtin_clzl(a);
  snprintf(b, sizeof b, "bit%d", hi);
  return b;
}

static void hint_row(const char *name, uintptr_t addr) {
  int err;
  void *p = try_map(addr, pg, 0, &err);
  if (p == MAP_FAILED) {
    printf("  hint   %-12s 0x%016lx -> FAILED errno=%d (%s)\n", name, addr, err, strerror(err));
  } else {
    ((char *)p)[0] = 42; // touch to prove it's backed
    printf("  hint   %-12s 0x%016lx -> 0x%016lx %-6s %s\n", name, addr, (uintptr_t)p,
           bits((uintptr_t)p), (uintptr_t)p == addr ? "EXACT" : "relocated");
    munmap(p, pg);
  }
  p = try_map(addr, pg, MAP_FIXED_NOREPLACE, &err);
  if (p == MAP_FAILED) {
    printf("  FNR    %-12s 0x%016lx -> FAILED errno=%d (%s)\n", name, addr, err, strerror(err));
  } else {
    ((char *)p)[0] = 42;
    printf("  FNR    %-12s 0x%016lx -> 0x%016lx %s\n", name, addr, (uintptr_t)p,
           (uintptr_t)p == addr ? "EXACT" : "WRONG-ADDR(old kernel?)");
    munmap(p, pg);
  }
}

// Positive/negative controls: exit status reflects them.
static void control(const char *name, uintptr_t addr, int extra, int expect_ok, int expect_errno) {
  int err;
  void *p = try_map(addr, pg, extra, &err);
  int ok = (p != MAP_FAILED) && (uintptr_t)p == addr;
  int pass = expect_ok ? ok : (!ok && (expect_errno == 0 || err == expect_errno));
  printf("  CONTROL %-40s 0x%016lx -> %s errno=%d  [%s]\n", name, addr,
         p == MAP_FAILED ? "FAILED" : "mapped", err, pass ? "PASS" : "FAIL");
  if (!pass) fails++;
  if (p != MAP_FAILED) munmap(p, pg);
}

int main(int argc, char **argv) {
  struct utsname u;
  uname(&u);
  pg = sysconf(_SC_PAGESIZE);
  printf("== va_probe: %s %s %s  pagesize=%ld personality=0x%x\n", u.machine, u.release,
         u.nodename, pg, personality(0xffffffff));
  FILE *f = fopen("/proc/sys/vm/mmap_min_addr", "r");
  unsigned long mma = 0;
  if (f) { if (fscanf(f, "%lu", &mma) != 1) mma = 0; fclose(f); }
  printf("mmap_min_addr=%lu\n", mma);

  // 1. no-hint placement
  int err;
  void *p1 = try_map(0, pg, 0, &err);
  printf("no-hint 1 page            -> 0x%016lx %s\n", (uintptr_t)p1, bits((uintptr_t)p1));
  munmap(p1, pg);
  void *p2 = mmap(0, 1UL << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  printf("no-hint 1 GiB PROT_NONE   -> 0x%016lx\n", (uintptr_t)p2);
  munmap(p2, 1UL << 30);

  // 2. How much no-hint VA can be reserved, and the highest end address seen.
  {
    size_t chunk = 1UL << 40; // 1 TiB PROT_NONE reservations
    uintptr_t hi = 0, lo = UINTPTR_MAX;
    void *list[1 << 13];
    int n = 0;
    while (n < (int)(sizeof list / sizeof list[0])) {
      void *p = mmap(0, chunk, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
      if (p == MAP_FAILED) break;
      list[n++] = p;
      if ((uintptr_t)p + chunk > hi) hi = (uintptr_t)p + chunk;
      if ((uintptr_t)p < lo) lo = (uintptr_t)p;
    }
    printf("no-hint 1TiB reservations: %d TiB until ENOMEM; lowest=0x%016lx highest_end=0x%016lx (%s)\n",
           n, lo, hi, bits(hi - 1));
    for (int i = 0; i < n; i++) munmap(list[i], chunk);
  }

  // 3. hints and MAP_FIXED_NOREPLACE
  struct { const char *n; uintptr_t a; } pts[] = {
      {"2^46", 1UL << 46},
      {"2^47-pg", (1UL << 47) - pg},
      {"2^47", 1UL << 47},
      {"2^47+pg", (1UL << 47) + pg},
      {"2^48-pg", (1UL << 48) - pg},
      {"2^48", 1UL << 48},
      {"2^52-pg", (1UL << 52) - pg},
  };
  printf("hint / MAP_FIXED_NOREPLACE (FNR), len=1 page:\n");
  for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; i++) hint_row(pts[i].n, pts[i].a);

  // 4. Highest FNR-mappable address by bit.
  printf("FNR at (2^b - 16 pages):");
  int top = -1;
  for (int b = 40; b <= 56; b++) {
    void *p = try_map((1UL << b) - 16 * pg, pg, MAP_FIXED_NOREPLACE, &err);
    int ok = p != MAP_FAILED;
    printf(" %d:%c", b, ok ? 'Y' : 'n');
    if (ok) { top = b; munmap(p, pg); }
  }
  printf("\n  => user VA ceiling (TASK_SIZE) = 2^%d\n", top);

  // 5. Does a high hint change subsequent no-hint placement?
  void *h = try_map((1UL << 47) + (1UL << 40), pg, 0, &err);
  void *after = try_map(0, pg, 0, &err);
  printf("after hint 2^47+2^40 (got 0x%016lx), next no-hint -> 0x%016lx\n", (uintptr_t)h, (uintptr_t)after);
  if (h != MAP_FAILED) munmap(h, pg);
  if (after != MAP_FAILED) munmap(after, pg);

  // 6. Controls
  printf("controls:\n");
  control("must-succeed FNR 2^40 (1 TiB)", 1UL << 40, MAP_FIXED_NOREPLACE, 1, 0);
  control("must-succeed FNR 2^46-16pg", (1UL << 46) - 16 * pg, MAP_FIXED_NOREPLACE, 1, 0);
  control("must-fail FNR 2^56 (above any TASK_SIZE)", 1UL << 56, MAP_FIXED_NOREPLACE, 0, ENOMEM);
  control("must-fail FNR below mmap_min_addr", 0, MAP_FIXED_NOREPLACE, 0, EPERM);
  {
    void *occ = try_map(0, pg, 0, &err);
    control("must-fail FNR over existing mapping", (uintptr_t)occ, MAP_FIXED_NOREPLACE, 0, EEXIST);
    munmap(occ, pg);
  }

  // 7. maps
  printf("/proc/self/maps (exe, heap, stack, vdso, vvar, highest):\n");
  f = fopen("/proc/self/maps", "r");
  char line[512], last[512] = "";
  int first = 1;
  while (f && fgets(line, sizeof line, f)) {
    int want = first || strstr(line, "[stack]") || strstr(line, "[vdso]") ||
               strstr(line, "[vvar") || strstr(line, "[heap]") ||
               (strstr(line, "ld") && strstr(line, "r-xp"));
    if (want)
      printf("  %s", line);
    first = 0;
    strcpy(last, line);
  }
  if (f) fclose(f);
  printf("  last: %s", last);
  printf("main=%p (PIE text)\n", (void *)main);
  printf("RESULT controls_failed=%d\n", fails);
  return fails ? 1 : 0;
}
