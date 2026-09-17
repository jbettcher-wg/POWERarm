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
 * Knobs arrive as kernel command-line environment variables:
 *   A64DIFF_JOBS, A64DIFF_TIMEOUT, A64DIFF_DEADLINE, A64DIFF_SKIP,
 *   A64DIFF_EXPECT_PAGESIZE, A64DIFF_SUITES (insn,programs), and any
 *   POWERARM_* / FEX_* variable, which a64diff passes to the emulator.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
  mkdir("/proc", 0555);
  mkdir("/dev", 0755);
  mkdir("/tmp", 01777);
  mount("proc", "/proc", "proc", 0, 0);
  mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
  mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
  mkdir("/dev/shm", 01777);
  mount("tmpfs", "/dev/shm", "tmpfs", 0, "mode=1777");
  setenv("HOME", "/tmp", 1);

  long ps = sysconf(_SC_PAGESIZE);
  const char* want = env_or("A64DIFF_EXPECT_PAGESIZE", "4096");
  printf("A64DIFF-GUEST pagesize=%ld expected=%s cpus=%ld\n", ps, want, sysconf(_SC_NPROCESSORS_ONLN));
  FILE* f = fopen("/proc/cpuinfo", "r");
  char l[256];
  while (f && fgets(l, sizeof l, f))
    if (!strncmp(l, "MMU", 3)) printf("A64DIFF-GUEST %s", l);
  if (f) fclose(f);
  int insn_rc = -1, prog_rc = -1;
  if (ps != atol(want)) {
    printf("A64DIFF-GUEST-ERROR page size %ld, expected %s: wrong kernel image\n", ps, want);
  } else {
    const char* jobs = env_or("A64DIFF_JOBS", "4");
    const char* timeout = env_or("A64DIFF_TIMEOUT", "30");
    const char* deadline = env_or("A64DIFF_DEADLINE", "0");
    const char* skip = env_or("A64DIFF_SKIP", "");
    const char* suites = env_or("A64DIFF_SUITES", "insn,programs");
    mkdir("/tmp/r", 0755);
    chown("/tmp/r", 65534, 65534);
    if (strstr(suites, "insn")) {
      char* r1[] = {TOOL, "run", "--manifest", B "/manifest.tsv", "--root", B, "--out", "/tmp/r/insn", "-j", (char*)jobs, "--timeout",
                    (char*)timeout, "--deadline", (char*)deadline, "--skip", (char*)skip, "--", EMU, NULL};
      run(r1, NULL);
      char* c1[] = {TOOL, "compare", "--manifest", B "/manifest.tsv", "--golden", B "/golden", "--actual", "/tmp/r/insn", "--report",
                    "/tmp/r/insn.report", "--max-detail", "5", "--skip", (char*)skip, NULL};
      insn_rc = run(c1, "/tmp/r/insn.log");
      cat("insn.log", "/tmp/r/insn.log");
      cat("insn.report", "/tmp/r/insn.report");
    }
    if (strstr(suites, "programs")) {
      char* r2[] = {TOOL, "run", "--jobs", B "/programs/programs.jobs", "--root", B, "--out", "/tmp/r/programs", "-j", (char*)jobs,
                    "--timeout", (char*)timeout, "--deadline", (char*)deadline, "--", EMU, NULL};
      run(r2, NULL);
      char* c2[] = {TOOL, "pcompare", "--jobs", B "/programs/programs.jobs", "--golden", B "/golden-programs", "--actual",
                    "/tmp/r/programs", "--report", "/tmp/r/programs.report", "--max-detail", "5", NULL};
      prog_rc = run(c2, "/tmp/r/programs.log");
      cat("programs.log", "/tmp/r/programs.log");
      cat("programs.report", "/tmp/r/programs.report");
    }
  }
  printf("A64DIFF-GUEST-EXIT insn=%d programs=%d\n", insn_rc, prog_rc);
  fflush(stdout);
  sync();
  reboot(RB_POWER_OFF);
  return 0;
}
