// Invalidation probe for a guest-path -> readlink-answer cache.
//
// Each case does: an observation that a cache would memoise, a mutation that
// changes the right answer, and the same observation again. Every step runs
// inside ONE guest process, because a per-process cache is only wrong within
// a process's lifetime -- a shell script spawning `readlink` per step gets a
// fresh cache each time and can never see the bug.
//
// Run under POWERarm against a fixture rootfs. The output must be identical
// with and without the cache; where it differs, the cache is wrong.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void rl(const char* Tag, const char* Path) {
  char Buf[4096];
  errno = 0;
  ssize_t N = readlink(Path, Buf, sizeof(Buf) - 1);
  if (N < 0) {
    printf("%-28s readlink(%s) = -1 %s\n", Tag, Path, strerror(errno));
  } else {
    Buf[N] = 0;
    printf("%-28s readlink(%s) = \"%s\"\n", Tag, Path, Buf);
  }
  fflush(stdout);
}

static void step(const char* What, int Rc) {
  printf("%-28s %s -> %s\n", "[mutate]", What, Rc == 0 ? "ok" : strerror(errno));
  fflush(stdout);
}

int main(void) {
  // 1. A memoised negative followed by a guest create.
  //    "/pafix/created" is absent, then the guest makes it a symlink.
  puts("== case 1: memoised ENOENT, then the guest creates a symlink");
  rl("1a before create", "/pafix/created");
  step("symlink(\"tgt1\", \"/pafix/created\")", symlink("tgt1", "/pafix/created"));
  rl("1b after create", "/pafix/created"); // must be "tgt1"

  // 2. A resolution that answered "not a symlink", then the guest replaces
  //    that very file with a symlink (unlink + symlink, as every tool does).
  puts("\n== case 2: memoised \"not a symlink\", then the guest makes it one");
  rl("2a before replace", "/pafix/plain");
  step("unlink(\"/pafix/plain\")", unlink("/pafix/plain"));
  step("symlink(\"tgt2\", \"/pafix/plain\")", symlink("tgt2", "/pafix/plain"));
  rl("2b after replace", "/pafix/plain"); // must be "tgt2"

  // 3. A memoised positive, then the guest deletes it.
  puts("\n== case 3: memoised symlink target, then the guest deletes it");
  rl("3a before delete", "/pafix/link");
  step("unlink(\"/pafix/link\")", unlink("/pafix/link"));
  rl("3b after delete", "/pafix/link"); // must be ENOENT

  // 4. A symlink whose target moves (unlink + symlink; targets are immutable).
  puts("\n== case 4: a symlink whose target moves");
  rl("4a before retarget", "/pafix/moving");
  step("unlink(\"/pafix/moving\")", unlink("/pafix/moving"));
  step("symlink(\"after\", \"/pafix/moving\")", symlink("after", "/pafix/moving"));
  rl("4b after retarget", "/pafix/moving"); // must be "after"

  // 5. An intermediate directory replaced by a symlink: the answer for a path
  //    BELOW it changes without that leaf being touched at all. This is the
  //    case a leaf-only validator cannot see.
  puts("\n== case 5: an intermediate component becomes a symlink");
  rl("5a below dir", "/pafix/mid/leaf");
  step("rename(\"/pafix/mid\", \"/pafix/mid.real\")", rename("/pafix/mid", "/pafix/mid.real"));
  step("symlink(\"other\", \"/pafix/mid\")", symlink("other", "/pafix/mid"));
  rl("5b below symlink", "/pafix/mid/leaf"); // must be ENOENT (other/leaf absent)
  return 0;
}
