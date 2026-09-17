// vm_init.c - /init for a throwaway QEMU guest: mount /proc, run /va_probe, power off.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
  mkdir("/proc", 0555);
  mount("proc", "/proc", "proc", 0, 0);
  FILE *f = fopen("/proc/cpuinfo", "r"); char l[256];
  while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "MMU", 3) || !strncmp(l, "cpu\t", 4)) { printf("GUEST %s", l); }
  if (f) fclose(f);
  pid_t p = fork();
  if (p == 0) { if (setgid(65534) || setuid(65534)) perror("setuid"); execl("/va_probe", "/va_probe", (char *)0); _exit(127); }
  int st; waitpid(p, &st, 0);
  if (fork() == 0) { if (setgid(65534) || setuid(65534)) perror("setuid"); execl("/fex_vasize_probe", "/fex_vasize_probe", (char *)0); _exit(127); }
  wait(0);
  printf("GUEST probe exit=%d\n", WEXITSTATUS(st));
  fflush(stdout); sync();
  reboot(RB_POWER_OFF);
  return 0;
}
