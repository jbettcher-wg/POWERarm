/* SPDX-License-Identifier: MIT */
/* /init for the A64Diff 4K KVM guest (same approach as
 * docs/powerarm/research/va-size/vm_init.c).
 *
 * The initramfs holds /a64diff/{bundle,bin/a64diff,emu/POWERarm...} and the
 * emulator's shared libraries.  This init mounts /proc, /dev and /tmp, checks
 * the page size, runs the instruction and program suites as uid 65534 through
 * a64diff, prints the reports to the console between markers, prints
 * "A64DIFF-GUEST-EXIT insn=N programs=M" and powers off.
 *
 * Rootfs jobs: the host attaches each rootfs as a read-only virtio-blk disk,
 * in the order of A64DIFF_ROOTFS=name:fstype,... (vda, vdb, ...).  The
 * initramfs carries the virtio and filesystem modules of the booted kernel
 * (/a64diff/modules, loaded in the order of /a64diff/modules/order); each disk
 * is mounted at /a64diff/rootfs/<name> and handed to a64diff as --rootfs.
 *
 * Suites: "insn" (manifest.tsv) and one per /a64diff/bundle/programs/<suite>.jobs,
 * named after the file; A64DIFF_SUITES lists them ("all" = every one).
 *
 * Knobs arrive as kernel command-line environment variables:
 *   A64DIFF_JOBS, A64DIFF_TIMEOUT, A64DIFF_DEADLINE (seconds from boot for
 *   both suites together), A64DIFF_SKIP,
 *   A64DIFF_EXPECT_PAGESIZE, A64DIFF_SUITES (insn,programs), and any
 *   POWERARM_* / FEX_* variable, which a64diff passes to the emulator.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define B "/a64diff/bundle"
#define TOOL "/a64diff/bin/a64diff"
#define EMU "/a64diff/emu/POWERarm"

static const char* env_or(const char* k, const char* d) {
  const char* v = getenv(k);
  return v && *v ? v : d;
}

static int run(char* const argv[], const char* out_path) {
  fflush(stdout);
  pid_t p = fork();
  if (p == 0) {
    if (setgid(65534) || setuid(65534)) {
      perror("setuid");
      _exit(126);
    }
    if (out_path) {
      int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) dup2(fd, 1);
    }
    execv(argv[0], argv);
    perror(argv[0]);
    _exit(127);
  }
  int st;
  waitpid(p, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static time_t t_start;
static long deadline_s;

/* Seconds left of the shared deadline, as a string for a64diff --deadline. */
static char* left(char* buf) {
  long l = deadline_s > 0 ? deadline_s - (long)(time(NULL) - t_start) : 0;
  if (deadline_s > 0 && l < 1) l = 1;
  sprintf(buf, "%ld", l);
  return buf;
}

static double uptime(void) {
  double u = -1;
  FILE* f = fopen("/proc/uptime", "r");
  if (f) {
    if (fscanf(f, "%lf", &u) != 1) u = -1;
    fclose(f);
  }
  return u;
}

static void load_modules(void) {
  FILE* f = fopen("/a64diff/modules/order", "r");
  char name[256];
  while (f && fgets(name, sizeof name, f)) {
    name[strcspn(name, "\n")] = 0;
    if (!name[0]) continue;
    char path[512];
    snprintf(path, sizeof path, "/a64diff/modules/%s", name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || (syscall(SYS_finit_module, fd, "", 0) && errno != EEXIST))
      printf("A64DIFF-GUEST-ERROR module %s: %s\n", name, strerror(errno));
    if (fd >= 0) close(fd);
  }
  if (f) fclose(f);
}

/* Mount the rootfs disks; returns the number of --rootfs arguments added. */
static int mount_rootfs(char** args, int max) {
  const char* spec = getenv("A64DIFF_ROOTFS");
  if (!spec || !*spec) return 0;
  load_modules();
  mkdir("/a64diff/rootfs", 0755);
  char* list = strdup(spec);
  int n = 0, disk = 0;
  for (char* item = strtok(list, ","); item && n + 2 <= max; item = strtok(NULL, ","), disk++) {
    char* colon = strchr(item, ':');
    const char* fstype = colon ? colon + 1 : "ext4";
    if (colon) *colon = 0;
    char dev[32], dir[256];
    snprintf(dev, sizeof dev, "/dev/vd%c", 'a' + disk);
    snprintf(dir, sizeof dir, "/a64diff/rootfs/%s", item);
    mkdir(dir, 0755);
    for (int i = 0; i < 200 && access(dev, F_OK); i++) usleep(50000);
    int rc = mount(dev, dir, fstype, MS_RDONLY, !strcmp(fstype, "ext4") ? "noload" : "");
    if (rc) {
      printf("A64DIFF-GUEST-ERROR rootfs %s: mount %s (%s): %s\n", item, dev, fstype, strerror(errno));
      continue;
    }
    printf("A64DIFF-GUEST rootfs %s mounted from %s (%s) at %.2fs\n", item, dev, fstype, uptime());
    char* a = malloc(strlen(item) + strlen(dir) + 2);
    sprintf(a, "%s=%s", item, dir);
    args[n++] = "--rootfs";
    args[n++] = a;
  }
  return n;
}

/* A64DIFF_ROOTFS_BENCH=1 (measurement aid): read every file of each mounted
 * rootfs once and report the time, then power off without running suites. */
static void read_tree(const char* dir, long* files, long long* bytes) {
  DIR* d = opendir(dir);
  if (!d) return;
  struct dirent* e;
  static char buf[1 << 16];
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char p[4096];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (lstat(p, &st)) continue;
    if (S_ISDIR(st.st_mode)) {
      read_tree(p, files, bytes);
    } else if (S_ISREG(st.st_mode)) {
      int fd = open(p, O_RDONLY);
      ssize_t n;
      while (fd >= 0 && (n = read(fd, buf, sizeof buf)) > 0) *bytes += n;
      if (fd >= 0) close(fd);
      (*files)++;
    }
  }
  closedir(d);
}

static int wanted(const char* suites, const char* name) {
  if (!strcmp(suites, "all")) return 1;
  size_t n = strlen(name);
  for (const char* p = suites; *p;) {
    const char* e = strchr(p, ',');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l == n && !strncmp(p, name, n)) return 1;
    if (!e) break;
    p = e + 1;
  }
  return 0;
}

static int cmp_names(const void* a, const void* b) {
  return strcmp(*(char* const*)a, *(char* const*)b);
}

static void cat(const char* marker, const char* path) {
  printf("A64DIFF-BEGIN %s\n", marker);
  FILE* f = fopen(path, "r");
  char buf[4096];
  size_t n;
  while (f && (n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, stdout);
  if (f) fclose(f);
  printf("A64DIFF-END %s\n", marker);
  fflush(stdout);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  t_start = time(NULL);
  mkdir("/proc", 0555);
  mkdir("/dev", 0755);
  mkdir("/tmp", 01777);
  mount("proc", "/proc", "proc", 0, 0);
  mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
  mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
  mkdir("/dev/shm", 01777);
  mount("tmpfs", "/dev/shm", "tmpfs", 0, "mode=1777");
  /* Pseudo-terminals (posix_openpt) for the A64Syscalls sys_tty program. */
  mkdir("/dev/pts", 0755);
  mount("devpts", "/dev/pts", "devpts", 0, "ptmxmode=0666,mode=0620");
  if (access("/dev/ptmx", F_OK) != 0) symlink("pts/ptmx", "/dev/ptmx");
  setenv("HOME", "/tmp", 1);

  long ps = sysconf(_SC_PAGESIZE);
  const char* want = env_or("A64DIFF_EXPECT_PAGESIZE", "4096");
  printf("\nA64DIFF-GUEST pagesize=%ld expected=%s cpus=%ld\n", ps, want, sysconf(_SC_NPROCESSORS_ONLN));
  FILE* f = fopen("/proc/cpuinfo", "r");
  char l[256];
  while (f && fgets(l, sizeof l, f))
    if (!strncmp(l, "MMU", 3)) printf("A64DIFF-GUEST %s", l);
  if (f) fclose(f);
  char exitline[1024] = "A64DIFF-GUEST-EXIT";
  if (ps != atol(want)) {
    printf("A64DIFF-GUEST-ERROR page size %ld, expected %s: wrong kernel image\n", ps, want);
    strcat(exitline, " error=1");
  } else {
    const char* jobs = env_or("A64DIFF_JOBS", "4");
    const char* timeout = env_or("A64DIFF_TIMEOUT", "30");
    char dl[32];
    deadline_s = atol(env_or("A64DIFF_DEADLINE", "0"));
    const char* skip = env_or("A64DIFF_SKIP", "");
    const char* suites = env_or("A64DIFF_SUITES", "all");
    char* rootfs_args[32];
    int nrootfs = mount_rootfs(rootfs_args, 32);
    if (getenv("A64DIFF_ROOTFS_BENCH")) {
      for (int j = 1; j < nrootfs; j += 2) {
        const char* dir = strchr(rootfs_args[j], '=') + 1;
        long files = 0;
        long long bytes = 0;
        double t = uptime();
        read_tree(dir, &files, &bytes);
        printf("A64DIFF-GUEST-BENCH %s files=%ld bytes=%lld read-all=%.2fs\n", rootfs_args[j], files, bytes, uptime() - t);
      }
      suites = "none";
    }
    mkdir("/tmp/r", 0755);
    chown("/tmp/r", 65534, 65534);
    if (wanted(suites, "insn") && access(B "/manifest.tsv", F_OK) == 0) {
      char* r1[] = {TOOL, "run", "--manifest", B "/manifest.tsv", "--root", B, "--out", "/tmp/r/insn", "-j", (char*)jobs, "--timeout",
                    (char*)timeout, "--deadline", left(dl), "--skip", (char*)skip, "--", EMU, NULL};
      run(r1, NULL);
      char* c1[] = {TOOL, "compare", "--manifest", B "/manifest.tsv", "--golden", B "/golden", "--actual", "/tmp/r/insn", "--report",
                    "/tmp/r/insn.report", "--max-detail", "5", "--skip", (char*)skip, NULL};
      int rc = run(c1, "/tmp/r/insn.log");
      cat("insn.log", "/tmp/r/insn.log");
      cat("insn.report", "/tmp/r/insn.report");
      sprintf(exitline + strlen(exitline), " insn=%d", rc);
    }
    char* names[64];
    int nn = 0;
    DIR* d = opendir(B "/programs");
    struct dirent* e;
    while (d && (e = readdir(d)) && nn < 64) {
      size_t l = strlen(e->d_name);
      if (l > 5 && !strcmp(e->d_name + l - 5, ".jobs")) names[nn++] = strndup(e->d_name, l - 5);
    }
    if (d) closedir(d);
    qsort(names, nn, sizeof(char*), cmp_names);
    for (int i = 0; i < nn; i++) {
      const char* suite = names[i];
      if (!wanted(suites, suite)) continue;
      char jobsf[256], golden[256], out[256], log[256], report[256], m1[128], m2[128];
      snprintf(jobsf, sizeof jobsf, B "/programs/%s.jobs", suite);
      snprintf(golden, sizeof golden, B "/golden-%s", suite);
      snprintf(out, sizeof out, "/tmp/r/%s", suite);
      snprintf(log, sizeof log, "/tmp/r/%s.log", suite);
      snprintf(report, sizeof report, "/tmp/r/%s.report", suite);
      char* r2[64] = {TOOL, "run", "--jobs", jobsf, "--root", B, "--out", out, "-j", (char*)jobs, "--timeout", (char*)timeout,
                      "--deadline", left(dl)};
      int k = 14;
      for (int j = 0; j < nrootfs; j++) r2[k++] = rootfs_args[j];
      r2[k++] = "--";
      r2[k++] = EMU;
      r2[k] = NULL;
      run(r2, NULL);
      char* c2[] = {TOOL, "pcompare", "--jobs", jobsf, "--golden", golden, "--actual", out, "--report", report, "--max-detail", "5", NULL};
      int rc = run(c2, log);
      snprintf(m1, sizeof m1, "%s.log", suite);
      snprintf(m2, sizeof m2, "%s.report", suite);
      cat(m1, log);
      cat(m2, report);
      sprintf(exitline + strlen(exitline), " %s=%d", suite, rc);
    }
  }
  printf("%s\n", exitline);
  fflush(stdout);
  sync();
  reboot(RB_POWER_OFF);
  return 0;
}
