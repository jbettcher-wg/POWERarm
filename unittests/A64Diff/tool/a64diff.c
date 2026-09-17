/* SPDX-License-Identifier: MIT */
/* a64diff: runner and comparator for the A64 differential harness.
 *
 *   a64diff run      --jobs FILE | --manifest FILE  --root DIR --out DIR
 *                    [-j N] [--timeout S] [--deadline S] [-- PREFIX...]
 *   a64diff check    --manifest FILE --golden DIR
 *   a64diff compare  --manifest FILE --golden DIR --actual DIR [options]
 *   a64diff pcompare --jobs FILE --golden DIR --actual DIR [options]
 *
 * Plain C99 + POSIX, no dependencies, so it builds statically for the KVM
 * initramfs as well as natively on the golden machine.  See ../README.md.
 */
#define _GNU_SOURCE
#include "a64diff.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void die(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("a64diff: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(2);
}

static void* xmalloc(size_t n) {
  void* p = calloc(1, n ? n : 1);
  if (!p) die("out of memory");
  return p;
}

static char* xstrdup(const char* s) {
  char* p = strdup(s);
  if (!p) die("out of memory");
  return p;
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Read a whole file.  Returns NULL (and *len = 0) if it doesn't exist. */
static char* slurp(const char* path, size_t* len) {
  *len = 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  size_t cap = 8192, n = 0;
  char* buf = xmalloc(cap + 1);
  for (;;) {
    if (n == cap) {
      cap *= 2;
      buf = realloc(buf, cap + 1);
      if (!buf) die("out of memory");
    }
    ssize_t r = read(fd, buf + n, cap - n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    n += r;
  }
  close(fd);
  buf[n] = 0;
  *len = n;
  return buf;
}

static char* path2(const char* dir, const char* id, const char* ext) {
  size_t n = strlen(dir) + strlen(id) + strlen(ext) + 3;
  char* p = xmalloc(n);
  snprintf(p, n, "%s/%s%s", dir, id, ext);
  return p;
}

/* ------------------------------------------------------------------ tables */

struct row {
  char* id;
  char* cls;
  char* sub;
  int required;
  char* expect;
  char* addr;
  char* enc;
  char* disasm;
  char* init;
  /* jobs */
  char* stdin_path;
  int argc;
  char** argv;
  /* block jobs ("job ... end"): see load_jobs */
  int block;
  char* rootfs;
  double timeout;
  int nsteps, ninputs, noutputs, nenv;
  struct step* steps;
  char** inputs;
  char** outputs;
  char** env;
};

struct step {
  char* stdin_path; /* NULL: /dev/null; relative: to the job's working directory */
  int argc;
  char** argv;
  int may_fail; /* "try": a non-zero status doesn't stop the sequence */
};

struct table {
  struct row* rows;
  int n;
  int format;
};

static int split_tabs(char* line, char** out, int max) {
  int n = 0;
  char* s = line;
  while (n < max) {
    out[n++] = s;
    char* t = strchr(s, '\t');
    if (!t) break;
    *t = 0;
    s = t + 1;
  }
  return n;
}

static void table_push(struct table* t, struct row* r) {
  if ((t->n & 1023) == 0) {
    t->rows = realloc(t->rows, sizeof(struct row) * (t->n + 1024));
    if (!t->rows) die("out of memory");
  }
  t->rows[t->n++] = *r;
}

/* manifest.tsv: id class sub required expect insn_addr encodings disasm init */
static struct table load_manifest(const char* path) {
  size_t len;
  char* buf = slurp(path, &len);
  if (!buf) die("cannot read manifest %s", path);
  struct table t = {0};
  t.format = -1;
  for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
    if (line[0] == '#') {
      const char* f = strstr(line, "format=");
      if (f) t.format = atoi(f + 7);
      continue;
    }
    char* f[16];
    int n = split_tabs(line, f, 16);
    if (n < 9) die("%s: malformed line for %s", path, f[0]);
    struct row r = {0};
    r.id = f[0];
    r.cls = f[1];
    r.sub = f[2];
    r.required = atoi(f[3]);
    r.expect = f[4];
    r.addr = f[5];
    r.enc = f[6];
    r.disasm = f[7];
    r.init = f[8];
    table_push(&t, &r);
  }
  if (t.format != A64D_FORMAT_VERSION)
    die("%s: manifest format %d, this tool reads format %d (regenerate the bundle)", path, t.format, A64D_FORMAT_VERSION);
  return t;
}

/* jobs file: id class required stdin argv0 argv1 ...  ('-' stdin = /dev/null)
 * @ROOT@ in any field is replaced by --root. */
static char* subst_root(const char* s, const char* root) {
  if (!root) return xstrdup(s);
  size_t cap = strlen(s) + 1;
  for (const char* m = strstr(s, "@ROOT@"); m; m = strstr(m + 6, "@ROOT@")) cap += strlen(root);
  char* out = xmalloc(cap);
  char* o = out;
  for (const char* p = s; *p;) {
    if (!strncmp(p, "@ROOT@", 6)) {
      o = stpcpy(o, root);
      p += 6;
    } else {
      *o++ = *p++;
    }
  }
  *o = 0;
  return out;
}

static void push_str(char*** arr, int* n, char* v) {
  *arr = realloc(*arr, sizeof(char*) * (*n + 2));
  if (!*arr) die("out of memory");
  (*arr)[(*n)++] = v;
  (*arr)[*n] = NULL;
}

/* Jobs file, two forms (TAB-separated fields, '#' comments):
 *
 *   id class required stdin argv...          one command, stdout/stderr/status compared
 *
 *   job id class required                    a block, ended by "end"; directives
 *     rootfs NAME                            run inside the named AArch64 rootfs
 *     input PATH                             copy a file or a directory's contents into the working directory
 *     env KEY=VALUE                          added to every step's environment
 *     timeout SEC                            for the whole block
 *     run STDIN argv...                      a step; the block stops at the first non-zero status
 *     try STDIN argv...                      a step whose non-zero status doesn't stop the block
 *     output PATH                            a file (or, ending in '/', every file below a directory)
 *                                            in the working directory, byte-compared
 *   end
 *
 * Leading blanks before a directive are ignored.  @ROOT@ expands to --root
 * everywhere.  stdin '-' means /dev/null. */
static struct table load_jobs(const char* path, const char* root) {
  size_t len;
  char* buf = slurp(path, &len);
  if (!buf) die("cannot read jobs file %s", path);
  struct table t = {0};
  struct row cur = {0};
  int in_block = 0, lineno = 0;
  for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
    lineno++;
    while (*line == ' ' || *line == '\t') line++;
    if (line[0] == '#' || line[0] == 0) continue;
    char* f[64];
    int n = split_tabs(line, f, 64);
    for (int i = 0; i < n; i++) f[i] = subst_root(f[i], root);
    if (!in_block && !strcmp(f[0], "job")) {
      if (n < 4) die("%s: 'job' needs id class required", path);
      memset(&cur, 0, sizeof cur);
      cur.id = f[1];
      cur.cls = f[2];
      cur.sub = f[2];
      cur.required = atoi(f[3]);
      cur.block = 1;
      cur.disasm = "";
      in_block = 1;
      continue;
    }
    if (in_block) {
      const char* d = f[0];
      if (!strcmp(d, "end")) {
        if (cur.nsteps == 0) die("%s: job %s has no run/try step", path, cur.id);
        cur.argc = cur.steps[0].argc; /* for reports */
        cur.argv = cur.steps[0].argv;
        table_push(&t, &cur);
        in_block = 0;
      } else if (!strcmp(d, "rootfs") && n == 2) {
        cur.rootfs = f[1];
      } else if (!strcmp(d, "input") && n == 2) {
        push_str(&cur.inputs, &cur.ninputs, f[1]);
      } else if (!strcmp(d, "output") && n == 2) {
        push_str(&cur.outputs, &cur.noutputs, f[1]);
      } else if (!strcmp(d, "env") && n == 2 && strchr(f[1], '=')) {
        push_str(&cur.env, &cur.nenv, f[1]);
      } else if (!strcmp(d, "timeout") && n == 2) {
        cur.timeout = atof(f[1]);
      } else if ((!strcmp(d, "run") || !strcmp(d, "try")) && n >= 3) {
        cur.steps = realloc(cur.steps, sizeof(struct step) * (cur.nsteps + 1));
        if (!cur.steps) die("out of memory");
        struct step* st = &cur.steps[cur.nsteps++];
        st->may_fail = d[0] == 't';
        st->stdin_path = strcmp(f[1], "-") ? f[1] : NULL;
        st->argc = n - 2;
        st->argv = xmalloc(sizeof(char*) * (st->argc + 1));
        for (int i = 0; i < st->argc; i++) st->argv[i] = f[2 + i];
      } else {
        die("%s: job %s: bad directive '%s' (%d fields)", path, cur.id, d, n);
      }
      continue;
    }
    if (n < 5) die("%s: malformed job line %s", path, f[0]);
    struct row r = {0};
    r.id = f[0];
    r.cls = f[1];
    r.sub = f[1];
    r.required = atoi(f[2]);
    r.stdin_path = strcmp(f[3], "-") ? f[3] : NULL;
    r.argc = n - 4;
    r.argv = xmalloc(sizeof(char*) * (r.argc + 1));
    for (int i = 0; i < r.argc; i++) r.argv[i] = f[4 + i];
    r.disasm = "";
    table_push(&t, &r);
  }
  if (in_block) die("%s: job %s is missing 'end'", path, cur.id);
  (void)lineno;
  return t;
}

/* Class filter: --skip a,b and --only a,b */
static int in_list(const char* list, const char* cls) {
  if (!list) return 0;
  size_t n = strlen(cls);
  for (const char* p = list; *p;) {
    const char* e = strchr(p, ',');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l == n && !strncmp(p, cls, n)) return 1;
    if (!e) break;
    p = e + 1;
  }
  return 0;
}

struct opts {
  const char* manifest, *jobs, *root, *out, *golden, *actual, *report;
  const char* skip, *only, *require, *optional;
  int jobsn;
  double timeout, deadline;
  int max_detail;
  unsigned seed;
  char** prefix;
  int nprefix;
  /* rootfs jobs */
  char* rootfs_names[16];
  char* rootfs_paths[16];
  int nrootfs;
  const char* rootfs_exec; /* native: wrapper ROOTFS CWD -- argv... */
  int break_outputs;       /* test-only: damage declared outputs before hashing */
};

static int selected(const struct opts* o, const struct row* r) {
  if (o->only && !in_list(o->only, r->cls)) return 0;
  return !in_list(o->skip, r->cls);
}

static int required(const struct opts* o, const struct row* r) {
  if (in_list(o->require, r->cls)) return 1;
  if (in_list(o->optional, r->cls)) return 0;
  return r->required;
}

/* ------------------------------------------------------------------ run */

struct slot {
  pid_t pid;
  int row;
  double start;
  int timed_out;
};

static void write_status(const char* out, const char* id, const char* text, double secs) {
  char* p = path2(out, id, ".status");
  FILE* f = fopen(p, "w");
  if (!f) die("cannot write %s: %s", p, strerror(errno));
  fprintf(f, "%s\nms %.0f\n", text, secs * 1000.0);
  fclose(f);
  free(p);
}

/* Children get a fixed environment so goldens don't depend on the caller's
 * locale or PATH.  HOME and the emulator's own knobs (POWERARM_*, FEX_*) pass
 * through, as do the broken-runner knobs (A64DIFF_BREAK*). */
extern char** environ;
static char** child_env(void) {
  static char* env[256];
  static int built;
  if (built) return env;
  int n = 0;
  env[n++] = "PATH=/usr/local/bin:/usr/bin:/bin";
  env[n++] = "LC_ALL=C";
  env[n++] = "TZ=UTC";
  for (char** e = environ; *e && n < 250; e++)
    if (!strncmp(*e, "HOME=", 5) || !strncmp(*e, "POWERARM_", 9) || !strncmp(*e, "FEX_", 4) || !strncmp(*e, "A64DIFF_BREAK", 13))
      env[n++] = *e;
  env[n] = NULL;
  built = 1;
  return env;
}

static pid_t spawn(const struct opts* o, const struct row* r, const char* testdir) {
  char* outp = path2(o->out, r->id, ".out");
  char* errp = path2(o->out, r->id, ".err");
  char* cwd = NULL;
  if (r->argv) {
    cwd = path2(o->out, r->id, ".cwd");
    mkdir(cwd, 0755);
  }
  /* Build argv: prefix... target... */
  int n = o->nprefix + (r->argv ? r->argc : 1);
  char** argv = xmalloc(sizeof(char*) * (n + 1));
  int k = 0;
  for (int i = 0; i < o->nprefix; i++) argv[k++] = o->prefix[i];
  if (r->argv)
    for (int i = 0; i < r->argc; i++) argv[k++] = r->argv[i];
  else
    argv[k++] = path2(testdir, r->id, "");
  argv[k] = NULL;

  pid_t pid = fork();
  if (pid < 0) die("fork: %s", strerror(errno));
  if (pid == 0) {
    setpgid(0, 0);
    struct rlimit rl = {0, 0};
    setrlimit(RLIMIT_CORE, &rl);
    sigset_t all;
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);
    int in = open(r->stdin_path ? r->stdin_path : "/dev/null", O_RDONLY);
    int of = open(outp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ef = open(errp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || of < 0 || ef < 0) _exit(126);
    dup2(in, 0);
    dup2(of, 1);
    dup2(ef, 2);
    if (cwd && chdir(cwd)) _exit(126);
    umask(022);
    execvpe(argv[0], argv, child_env());
    dprintf(2, "a64diff: exec %s: %s\n", argv[0], strerror(errno));
    _exit(127);
  }
  free(outp);
  free(errp);
  free(cwd);
  return pid;
}


/* ------------------------------------------------------------------ sha256 */

struct sha256 {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t n;
};

static const uint32_t sha_k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
  0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
  0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
  0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
  0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(struct sha256* c, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
    uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_hex(const uint8_t* p, size_t n, char out[65]) {
  struct sha256 c = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
  size_t i = 0;
  for (; i + 64 <= n; i += 64) sha_block(&c, p + i);
  uint8_t tail[128] = {0};
  size_t r = n - i;
  memcpy(tail, p + i, r);
  tail[r] = 0x80;
  size_t tl = r + 1 + 8 <= 64 ? 64 : 128;
  uint64_t bits = (uint64_t)n * 8;
  for (int k = 0; k < 8; k++) tail[tl - 1 - k] = bits >> (8 * k);
  sha_block(&c, tail);
  if (tl == 128) sha_block(&c, tail + 64);
  for (int k = 0; k < 8; k++) sprintf(out + 8 * k, "%08x", c.h[k]);
}

/* ------------------------------------------------------------------ block jobs */

static const char* rootfs_path(const struct opts* o, const char* name) {
  for (int i = 0; i < o->nrootfs; i++)
    if (!strcmp(o->rootfs_names[i], name)) return o->rootfs_paths[i];
  return NULL;
}

static void mkdir_parents(const char* path) {
  char* p = xstrdup(path);
  for (char* q = p + 1; *q; q++)
    if (*q == '/') {
      *q = 0;
      mkdir(p, 0755);
      *q = '/';
    }
  free(p);
}

/* Copy SRC (a file, or a directory's contents) into DST. */
static int copy_tree(const char* src, const char* dst) {
  struct stat st;
  if (lstat(src, &st)) return -1;
  if (S_ISDIR(st.st_mode)) {
    mkdir(dst, st.st_mode & 07777);
    DIR* d = opendir(src);
    if (!d) return -1;
    struct dirent* e;
    int rc = 0;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      char* s2 = path2(src, e->d_name, ""), *d2 = path2(dst, e->d_name, "");
      rc |= copy_tree(s2, d2);
      free(s2);
      free(d2);
    }
    closedir(d);
    return rc;
  }
  if (S_ISLNK(st.st_mode)) {
    char tgt[4096];
    ssize_t n = readlink(src, tgt, sizeof tgt - 1);
    if (n < 0) return -1;
    tgt[n] = 0;
    return symlink(tgt, dst);
  }
  size_t len;
  char* b = slurp(src, &len);
  if (!b) return -1;
  int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
  int rc = fd < 0 || write(fd, b, len) != (ssize_t)len ? -1 : 0;
  if (fd >= 0) close(fd);
  free(b);
  return rc;
}

static int cmp_str(const void* a, const void* b) {
  return strcmp(*(char* const*)a, *(char* const*)b);
}

/* Every non-directory below DIR (relative to BASE), sorted. */
static void list_files(const char* base, const char* rel, char*** out, int* n) {
  char* dir = rel[0] ? path2(base, rel, "") : xstrdup(base);
  DIR* d = opendir(dir);
  if (!d) {
    free(dir);
    return;
  }
  char** names = NULL;
  int nn = 0;
  struct dirent* e;
  while ((e = readdir(d)))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) push_str(&names, &nn, xstrdup(e->d_name));
  closedir(d);
  if (nn) qsort(names, nn, sizeof(char*), cmp_str);
  for (int i = 0; i < nn; i++) {
    char* r2 = rel[0] ? path2(rel, names[i], "") : xstrdup(names[i]);
    char* full = path2(base, r2, "");
    struct stat st;
    if (!lstat(full, &st) && S_ISDIR(st.st_mode))
      list_files(base, r2, out, n);
    else
      push_str(out, n, xstrdup(r2));
    free(full);
  }
  free(dir);
}

/* Hash and keep a copy of every declared output: <id>.outputs has one line
 * per file, "<sha256> <size> <path>", "link <target> <path>" or "missing - <path>". */
static void collect_outputs(const struct opts* o, const struct row* r, const char* cwd) {
  char* manp = path2(o->out, r->id, ".outputs");
  FILE* man = fopen(manp, "w");
  if (!man) _exit(125);
  char* keep = path2(o->out, r->id, ".files");
  mkdir(keep, 0755);
  for (int i = 0; i < r->noutputs; i++) {
    const char* want = r->outputs[i];
    size_t wl = strlen(want);
    char** files = NULL;
    int nf = 0;
    if (wl && want[wl - 1] == '/') {
      char* rel = xstrdup(want);
      rel[wl - 1] = 0;
      list_files(cwd, rel, &files, &nf);
      if (nf == 0) fprintf(man, "missing - %s\n", want);
      free(rel);
    } else {
      push_str(&files, &nf, xstrdup(want));
    }
    for (int k = 0; k < nf; k++) {
      char* full = path2(cwd, files[k], "");
      struct stat st;
      if (lstat(full, &st)) {
        fprintf(man, "missing - %s\n", files[k]);
      } else if (S_ISLNK(st.st_mode)) {
        char tgt[4096];
        ssize_t n = readlink(full, tgt, sizeof tgt - 1);
        tgt[n < 0 ? 0 : n] = 0;
        fprintf(man, "link %s %s\n", tgt, files[k]);
      } else {
        size_t len;
        char* b = slurp(full, &len);
        if (b && len && o->break_outputs) b[len / 2] ^= 0x10;
        char hex[65];
        sha256_hex((const uint8_t*)(b ? b : ""), b ? len : 0, hex);
        fprintf(man, "%s %zu %s\n", hex, len, files[k]);
        char* dst = path2(keep, files[k], "");
        mkdir_parents(dst);
        int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
          if (b && write(fd, b, len) != (ssize_t)len) fprintf(man, "# short copy %s\n", files[k]);
          close(fd);
        }
        free(b);
      }
      free(full);
    }
  }
  fclose(man);
  free(manp);
  free(keep);
}

/* A block job runs in a supervisor process (its own process group, so a
 * timeout kills every step).  It prepares the working directory, runs the
 * steps, records "<k> exit N" / "<k> signal N" lines in <id>.steps, collects
 * the outputs and finally exits with the status of the last step it ran. */
static pid_t spawn_block(const struct opts* o, const struct row* r) {
  pid_t pid = fork();
  if (pid < 0) die("fork: %s", strerror(errno));
  if (pid) return pid;
  setpgid(0, 0);
  struct rlimit rl = {0, 0};
  setrlimit(RLIMIT_CORE, &rl);
  sigset_t all;
  sigemptyset(&all);
  sigprocmask(SIG_SETMASK, &all, NULL);
  umask(022);
  char* cwd = path2(o->out, r->id, ".cwd");
  char* stepsp = path2(o->out, r->id, ".steps");
  FILE* steps = fopen(stepsp, "w");
  if (!steps) _exit(125);
  setvbuf(steps, NULL, _IOLBF, 0);
  mkdir(cwd, 0755);
  char* err1 = path2(o->out, r->id, ".1.err");
  const char* rootfs = NULL;
  if (r->rootfs) {
    rootfs = rootfs_path(o, r->rootfs);
    if (!rootfs || (!o->rootfs_exec && !o->nprefix)) {
      FILE* e = fopen(err1, "w");
      if (e) {
        fprintf(e, "a64diff: job needs rootfs '%s' but %s\n", r->rootfs,
                !rootfs ? "no --rootfs NAME=PATH provides it" : "neither --rootfs-exec nor an emulator prefix was given");
        fclose(e);
      }
      fprintf(steps, "setup error\n");
      _exit(125);
    }
  }
  for (int i = 0; i < r->ninputs; i++)
    if (copy_tree(r->inputs[i], cwd)) {
      FILE* e = fopen(err1, "w");
      if (e) {
        fprintf(e, "a64diff: cannot copy input %s\n", r->inputs[i]);
        fclose(e);
      }
      fprintf(steps, "setup error\n");
      _exit(125);
    }
  /* environment: fixed base + job env (+ POWERARM_ROOTFS under an emulator) */
  char** base = child_env();
  char** env = NULL;
  int nenv = 0;
  for (char** e = base; *e; e++) {
    int over = 0;
    for (int i = 0; i < r->nenv; i++) {
      size_t kl = strchr(r->env[i], '=') - r->env[i] + 1;
      if (!strncmp(*e, r->env[i], kl)) over = 1;
    }
    if (rootfs && o->nprefix && !o->rootfs_exec && !strncmp(*e, "POWERARM_ROOTFS=", 16)) over = 1;
    if (!over) push_str(&env, &nenv, *e);
  }
  for (int i = 0; i < r->nenv; i++) push_str(&env, &nenv, r->env[i]);
  if (rootfs && o->nprefix && !o->rootfs_exec) {
    char* v = xmalloc(strlen(rootfs) + 20);
    sprintf(v, "POWERARM_ROOTFS=%s", rootfs);
    push_str(&env, &nenv, v);
  }
  if (chdir(cwd)) _exit(125);
  int last = 0;
  for (int k = 0; k < r->nsteps; k++) {
    const struct step* sp = &r->steps[k];
    char ext[32];
    snprintf(ext, sizeof ext, ".%d.out", k + 1);
    char* outp = path2(o->out, r->id, ext);
    snprintf(ext, sizeof ext, ".%d.err", k + 1);
    char* errp = path2(o->out, r->id, ext);
    char** argv = NULL;
    int na = 0;
    if (rootfs && o->rootfs_exec) {
      push_str(&argv, &na, (char*)o->rootfs_exec);
      push_str(&argv, &na, (char*)rootfs);
      push_str(&argv, &na, cwd);
      push_str(&argv, &na, "--");
    }
    for (int i = 0; i < o->nprefix; i++) push_str(&argv, &na, o->prefix[i]);
    for (int i = 0; i < sp->argc; i++) push_str(&argv, &na, sp->argv[i]);
    pid_t c = fork();
    if (c == 0) {
      int in = open(sp->stdin_path ? sp->stdin_path : "/dev/null", O_RDONLY);
      int of = open(outp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      int ef = open(errp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (in < 0 || of < 0 || ef < 0) _exit(126);
      dup2(in, 0);
      dup2(of, 1);
      dup2(ef, 2);
      execvpe(argv[0], argv, env);
      dprintf(2, "a64diff: exec %s: %s\n", argv[0], strerror(errno));
      _exit(127);
    }
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR) {
    }
    last = st;
    if (WIFEXITED(st))
      fprintf(steps, "%d exit %d\n", k + 1, WEXITSTATUS(st));
    else
      fprintf(steps, "%d signal %d\n", k + 1, WTERMSIG(st));
    free(outp);
    free(errp);
    if (!sp->may_fail && !(WIFEXITED(st) && WEXITSTATUS(st) == 0)) break;
  }
  collect_outputs(o, r, cwd);
  fclose(steps);
  if (WIFSIGNALED(last)) {
    signal(WTERMSIG(last), SIG_DFL);
    raise(WTERMSIG(last));
  }
  _exit(WIFEXITED(last) ? WEXITSTATUS(last) : 125);
}

static void mkdir_p(const char* path) {
  char* p = xstrdup(path);
  for (char* q = p + 1; *q; q++)
    if (*q == '/') {
      *q = 0;
      mkdir(p, 0755);
      *q = '/';
    }
  if (mkdir(p, 0755) && errno != EEXIST) die("cannot create %s: %s", p, strerror(errno));
  free(p);
}

static int cmd_run(struct opts* o) {
  struct table t;
  const char* testdir = NULL;
  if (o->manifest) {
    t = load_manifest(o->manifest);
    if (!o->root) die("run --manifest needs --root (directory holding tests/)");
    testdir = path2(o->root, "tests", "");
  } else if (o->jobs) {
    t = load_jobs(o->jobs, o->root);
  } else {
    die("run needs --manifest or --jobs");
  }
  if (!o->out) die("run needs --out");
  mkdir_p(o->out);
  /* Block jobs chdir into their working directory: keep every path absolute. */
  char* absout = realpath(o->out, NULL);
  if (!absout) die("cannot resolve %s", o->out);
  o->out = absout;
  int j = o->jobsn > 0 ? o->jobsn : 4;
  struct slot* slots = xmalloc(sizeof(struct slot) * j);
  int next = 0, running = 0, done = 0, total = 0, notrun = 0;
  for (int i = 0; i < t.n; i++) total += selected(o, &t.rows[i]);
  double t0 = now_s();
  int deadline_hit = 0;
  printf("a64diff run: %d tests, -j %d, timeout %.0fs%s%s\n", total, j, o->timeout,
         o->nprefix ? ", prefix " : ", native", o->nprefix ? o->prefix[0] : "");
  fflush(stdout);
  for (;;) {
    while (running < j && next < t.n && !deadline_hit) {
      if (!selected(o, &t.rows[next])) {
        next++;
        continue;
      }
      if (o->deadline > 0 && now_s() - t0 > o->deadline) {
        deadline_hit = 1;
        break;
      }
      int s = 0;
      while (slots[s].pid) s++;
      slots[s].row = next;
      slots[s].start = now_s();
      slots[s].timed_out = 0;
      slots[s].pid = t.rows[next].block ? spawn_block(o, &t.rows[next]) : spawn(o, &t.rows[next], testdir);
      running++;
      next++;
    }
    if (running == 0) break;
    int st;
    pid_t p = waitpid(-1, &st, WNOHANG);
    if (p > 0) {
      for (int s = 0; s < j; s++) {
        if (slots[s].pid != p) continue;
        struct row* r = &t.rows[slots[s].row];
        char text[64];
        if (slots[s].timed_out)
          snprintf(text, sizeof text, "timeout");
        else if (WIFEXITED(st))
          snprintf(text, sizeof text, "exit %d", WEXITSTATUS(st));
        else
          snprintf(text, sizeof text, "signal %d", WTERMSIG(st));
        write_status(o->out, r->id, text, now_s() - slots[s].start);
        slots[s].pid = 0;
        running--;
        done++;
        if (done % 500 == 0) {
          printf("a64diff run: %d/%d (%.0fs)\n", done, total, now_s() - t0);
          fflush(stdout);
        }
      }
      continue;
    }
    double tn = now_s();
    for (int s = 0; s < j; s++) {
      if (!slots[s].pid || slots[s].timed_out) continue;
      double lim = t.rows[slots[s].row].timeout > 0 ? t.rows[slots[s].row].timeout : o->timeout;
      int over = lim > 0 && tn - slots[s].start > lim;
      int dl = o->deadline > 0 && tn - t0 > o->deadline;
      if (over || dl) {
        kill(-slots[s].pid, SIGKILL);
        kill(slots[s].pid, SIGKILL);
        slots[s].timed_out = 1;
        deadline_hit |= dl;
      }
    }
    struct timespec ts = {0, 2000000};
    nanosleep(&ts, NULL);
  }
  for (int i = next; i < t.n; i++)
    if (selected(o, &t.rows[i])) notrun++;
  printf("a64diff run: finished %d/%d in %.1fs%s\n", done, total, now_s() - t0,
         notrun ? " (deadline reached; the rest were not run)" : "");
  return notrun ? 3 : 0;
}

/* ------------------------------------------------------------------ results */

struct status {
  int present;
  int exited, code, sig, timeout;
};

static struct status read_status(const char* dir, const char* id) {
  struct status s = {0};
  char* p = path2(dir, id, ".status");
  size_t len;
  char* b = slurp(p, &len);
  free(p);
  if (!b) return s;
  s.present = 1;
  if (!strncmp(b, "exit ", 5)) {
    s.exited = 1;
    s.code = atoi(b + 5);
  } else if (!strncmp(b, "signal ", 7)) {
    s.sig = atoi(b + 7);
  } else if (!strncmp(b, "timeout", 7)) {
    s.timeout = 1;
  }
  free(b);
  return s;
}

static int status_eq(struct status a, struct status b) {
  return a.exited == b.exited && a.code == b.code && a.sig == b.sig && a.timeout == b.timeout;
}

static const char* status_str(struct status s, char* buf, size_t n) {
  if (!s.present)
    snprintf(buf, n, "not run");
  else if (s.timeout)
    snprintf(buf, n, "timeout");
  else if (s.exited)
    snprintf(buf, n, "exit %d", s.code);
  else
    snprintf(buf, n, "killed by signal %d (%s)", s.sig, strsignal(s.sig));
  return buf;
}

enum cat { C_PASS, C_MISMATCH, C_UNIMPL, C_REFUSED, C_CRASH, C_TIMEOUT, C_NOTRUN, C_NCAT };
static const char* cat_text[] = {
  "pass",
  "mismatch",
  "emulator stopped: unimplemented instruction",
  "emulator refused to start",
  "no record (crash or early exit)",
  "timeout",
  "not run",
};

/* Diagnose a run that produced no usable result from its stderr and status. */
static enum cat classify_missing(struct status s, const char* err) {
  if (!s.present) return C_NOTRUN;
  if (s.timeout) return C_TIMEOUT;
  if (err && (strstr(err, "unimplemented A64 instruction") || strstr(err, "nimplemented instruction"))) return C_UNIMPL;
  if (err && strstr(err, "HOSTPAGEMODE") && !strstr(err, "continuing")) return C_REFUSED;
  return C_CRASH;
}

static void err_excerpt(FILE* f, const char* err) {
  if (!err || !*err) return;
  /* Last two non-empty lines, which is where emulators put the reason. */
  const char* end = err + strlen(err);
  while (end > err && (end[-1] == '\n' || end[-1] == ' ')) end--;
  const char* p = end;
  int lines = 0;
  while (p > err && lines < 2) {
    p--;
    if (*p == '\n') lines++;
  }
  if (*p == '\n') p++;
  fprintf(f, "    stderr: ");
  for (const char* q = p; q < end; q++) {
    if (*q == '\n')
      fputs("\n            ", f);
    else if ((unsigned char)*q >= 0x20 || *q == '\t')
      fputc(*q, f);
  }
  fputc('\n', f);
}

/* ------------------------------------------------------------------ record diff */

enum field { F_X0 = 0, F_SP = 31, F_PCMARK, F_NZCV, F_KIND, F_SIGNO, F_SIGCODE, F_SIGADDR, F_PAGE, F_SIMD, F_FPCR, F_FPSR, F_EXIT, F_NFIELDS };

static const char* field_name(int f, char* buf, size_t n) {
  static const char* names[] = {"sp", "pcmark", "nzcv", "kind", "signo", "si_code", "si_addr", "page", "v0-v31", "fpcr", "fpsr", "exit-status"};
  if (f < 31)
    snprintf(buf, n, "x%d", f);
  else
    snprintf(buf, n, "%s", names[f - 31]);
  return buf;
}

static int rec_valid(const char* b, size_t len) {
  if (len != A64D_REC_SIZE) return 0;
  const struct a64d_rec* r = (const struct a64d_rec*)b;
  return !memcmp(r->magic, A64D_MAGIC, 4) && r->version == A64D_FORMAT_VERSION && r->size == A64D_REC_SIZE &&
         (r->kind == A64D_KIND_STATE || r->kind == A64D_KIND_SIGNAL);
}

static uint64_t fnv(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

/* Test-only: --blind-field NAME makes the comparator ignore one field, to show
 * that the controls catch a comparator that cannot see a difference. */
static int blind_field = -1;

static int rec_diff_all(const struct a64d_rec* g, const struct a64d_rec* a, int* diff);

/* Fill diff[] with differing field ids; returns count. */
static int rec_diff(const struct a64d_rec* g, const struct a64d_rec* a, int* diff) {
  int n = rec_diff_all(g, a, diff), m = 0;
  for (int i = 0; i < n; i++)
    if (diff[i] != blind_field) diff[m++] = diff[i];
  return m;
}

static int rec_diff_all(const struct a64d_rec* g, const struct a64d_rec* a, int* diff) {
  int n = 0;
  for (int i = 0; i < 31; i++)
    if (g->x[i] != a->x[i]) diff[n++] = F_X0 + i;
  if (g->sp != a->sp) diff[n++] = F_SP;
  if (g->pcmark != a->pcmark) diff[n++] = F_PCMARK;
  if (g->nzcv != a->nzcv) diff[n++] = F_NZCV;
  if (g->kind != a->kind) diff[n++] = F_KIND;
  if (g->kind == A64D_KIND_SIGNAL || a->kind == A64D_KIND_SIGNAL) {
    if (g->signo != a->signo) diff[n++] = F_SIGNO;
    if (g->sigcode != a->sigcode) diff[n++] = F_SIGCODE;
    if (g->sigaddr != a->sigaddr) diff[n++] = F_SIGADDR;
  }
  if (memcmp(g->page, a->page, A64D_PAGE_SIZE)) diff[n++] = F_PAGE;
  if (g->flags & A64D_FLAG_SIMD) {
    if (!(a->flags & A64D_FLAG_SIMD) || memcmp(g->v, a->v, sizeof g->v)) diff[n++] = F_SIMD;
    if (g->fpcr != a->fpcr) diff[n++] = F_FPCR;
    if (g->fpsr != a->fpsr) diff[n++] = F_FPSR;
  }
  return n;
}

static uint64_t init_val(const char* init, int idx) {
  const char* p = init;
  for (int i = 0; i < idx && p; i++) {
    p = strchr(p, ',');
    if (p) p++;
  }
  return p ? strtoull(p, NULL, 16) : 0;
}

static void print_field(FILE* f, const struct row* r, int fld, const struct a64d_rec* g, const struct a64d_rec* a) {
  char nb[32];
  field_name(fld, nb, sizeof nb);
  uint64_t gv = 0, av = 0;
  int has_init = -1;
  switch (fld) {
  case F_SP: gv = g->sp; av = a->sp; has_init = 31; break;
  case F_PCMARK: gv = g->pcmark; av = a->pcmark; break;
  case F_NZCV: gv = g->nzcv; av = a->nzcv; has_init = 32; break;
  case F_KIND: gv = g->kind; av = a->kind; break;
  case F_SIGNO: gv = g->signo; av = a->signo; break;
  case F_SIGCODE: gv = g->sigcode; av = a->sigcode; break;
  case F_SIGADDR: gv = g->sigaddr; av = a->sigaddr; break;
  case F_FPCR: gv = g->fpcr; av = a->fpcr; break;
  case F_FPSR: gv = g->fpsr; av = a->fpsr; break;
  case F_PAGE: {
    int nd = 0, first[4];
    for (int i = 0; i < A64D_PAGE_SIZE; i++)
      if (g->page[i] != a->page[i]) {
        if (nd < 4) first[nd] = i;
        nd++;
      }
    fprintf(f, "    %-8s golden fnv %016" PRIx64 "  actual fnv %016" PRIx64 "  (%d bytes differ; first at", nb,
            fnv(g->page, A64D_PAGE_SIZE), fnv(a->page, A64D_PAGE_SIZE), nd);
    for (int i = 0; i < nd && i < 4; i++) fprintf(f, " +%d:%02x/%02x", first[i], g->page[first[i]], a->page[first[i]]);
    fprintf(f, ")\n");
    return;
  }
  case F_SIMD: fprintf(f, "    %-8s differ\n", nb); return;
  default:
    if (fld < 31) {
      gv = g->x[fld];
      av = a->x[fld];
      has_init = fld;
    }
  }
  fprintf(f, "    %-8s golden %016" PRIx64 "  actual %016" PRIx64, nb, gv, av);
  if (has_init >= 0 && r->init) fprintf(f, "  (initial %016" PRIx64 ")", init_val(r->init, has_init));
  fputc('\n', f);
}

static void print_header(FILE* f, const struct row* r, const char* what) {
  fprintf(f, "FAIL %s  [%s, %s] %s\n", r->id, r->cls, r->required ? "required" : "optional", what);
  if (r->addr) fprintf(f, "    insn @%s: %s  %s\n", r->addr, r->enc, r->disasm);
}

static void print_init(FILE* f, const struct row* r) {
  if (!r->init) return;
  fprintf(f, "    initial state:");
  for (int i = 0; i < 31; i++) {
    if (i % 4 == 0) fprintf(f, "\n     ");
    fprintf(f, " x%-2d=%016" PRIx64, i, init_val(r->init, i));
  }
  fprintf(f, "\n      sp =%016" PRIx64 " nzcv=%" PRIx64 "\n", init_val(r->init, 31), init_val(r->init, 32) >> 28);
}

/* ------------------------------------------------------------------ compare */

struct loaded {
  char* out;
  size_t outlen;
  char* err;
  struct status st;
};

static struct loaded load_result(const char* dir, const char* id) {
  struct loaded l = {0};
  char* p = path2(dir, id, ".out");
  l.out = slurp(p, &l.outlen);
  free(p);
  p = path2(dir, id, ".err");
  size_t n;
  l.err = slurp(p, &n);
  free(p);
  l.st = read_status(dir, id);
  return l;
}

static void free_result(struct loaded* l) {
  free(l->out);
  free(l->err);
}

struct classsum {
  const char* cls;
  int tests, required, ncat[C_NCAT];
  int ctl_total, ctl_fired;
};

static struct classsum* class_get(struct classsum* cs, int* ncs, const char* cls) {
  for (int i = 0; i < *ncs; i++)
    if (!strcmp(cs[i].cls, cls)) return &cs[i];
  cs[*ncs].cls = cls;
  return &cs[(*ncs)++];
}

static uint32_t hash32(const char* s, unsigned seed) {
  uint32_t h = 2166136261u ^ seed;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  h ^= h >> 15;
  h *= 0x2c1b3c6d;
  h ^= h >> 12;
  return h;
}

/* A control corrupts one field of a golden record.  Returns the field id.
 * The field is chosen by (h, variant); attempt k only changes the corrupting
 * value, so a caller can retry until the value differs from the actual result
 * too (otherwise a real bug that happens to produce the corrupted value would
 * make the control look blind). */
static int corrupt_record(struct a64d_rec* r, uint32_t h, int variant, int k) {
  uint64_t bit = 1ull << (((h >> 8) + (unsigned)k * 13) % 64);
  if (variant == 0) {
    int reg = h % 31;
    r->x[reg] ^= bit;
    return F_X0 + reg;
  }
  if (r->kind == A64D_KIND_SIGNAL) {
    switch ((h >> 4) % 3) {
    case 0: r->signo += 11 + k; return F_SIGNO;
    case 1: r->sigaddr ^= bit; return F_SIGADDR;
    default: r->sigcode += 7 + k; return F_SIGCODE;
    }
  }
  switch ((h >> 4) % 4) {
  case 0: r->nzcv ^= 0x10000000u << (k % 4); return F_NZCV;
  case 1: r->sp ^= bit; return F_SP;
  case 2: r->pcmark += 5 + k; return F_PCMARK;
  default: r->page[((h >> 12) + (unsigned)k * 97) % A64D_PAGE_SIZE] ^= 0x5a; return F_PAGE;
  }
}

static int diff_has(const int* d, int n, int f) {
  for (int i = 0; i < n; i++)
    if (d[i] == f) return 1;
  return 0;
}

static void print_summary(struct classsum* cs, int ncs, int ctl_fired, int ctl_total, int golden_bad, int prog) {
  int tot[C_NCAT] = {0}, tests = 0, req_fail = 0, opt_fail = 0;
  printf("\n%-10s %6s %6s %6s %8s %7s %7s %6s %7s %7s  %s\n", "class", "tests", "pass", "fail", "mismatch", "unimpl", "refused",
         "crash", "timeout", "not-run", "controls");
  for (int i = 0; i < ncs; i++) {
    struct classsum* c = &cs[i];
    int fail = c->tests - c->ncat[C_PASS];
    printf("%-10s %6d %6d %6d %8d %7d %7d %6d %7d %7d  %d/%d%s\n", c->cls, c->tests, c->ncat[C_PASS], fail,
           c->ncat[C_MISMATCH], c->ncat[C_UNIMPL], c->ncat[C_REFUSED], c->ncat[C_CRASH], c->ncat[C_TIMEOUT], c->ncat[C_NOTRUN],
           c->ctl_fired, c->ctl_total, c->required ? "" : "  (optional)");
    tests += c->tests;
    for (int k = 0; k < C_NCAT; k++) tot[k] += c->ncat[k];
    if (c->required)
      req_fail += fail;
    else
      opt_fail += fail;
  }
  int ok = req_fail == 0 && ctl_fired == ctl_total && ctl_total > 0 && !golden_bad && tests > 0;
  printf("\nA64DIFF %s SUMMARY: tests=%d pass=%d fail=%d (mismatch=%d unimplemented=%d refused=%d crash=%d timeout=%d not-run=%d) "
         "required-fail=%d optional-fail=%d controls=%d/%d-fired%s RESULT=%s\n",
         prog ? "PROGRAMS" : "INSN", tests, tot[C_PASS], tests - tot[C_PASS], tot[C_MISMATCH], tot[C_UNIMPL], tot[C_REFUSED],
         tot[C_CRASH], tot[C_TIMEOUT], tot[C_NOTRUN], req_fail, opt_fail, ctl_fired, ctl_total,
         golden_bad ? " GOLDEN-INVALID" : "", ok ? "PASS" : "FAIL");
}

static FILE* open_report(const struct opts* o) {
  if (!o->report) return NULL;
  FILE* f = fopen(o->report, "w");
  if (!f) die("cannot write report %s", o->report);
  return f;
}

/* Detail goes to stdout for the first max_detail failures, and always to the report. */
struct sink {
  FILE* report;
  int shown, max;
};

/* If the emulator reported "... at pc 0xADDR", place it relative to the
 * instruction under test: a stop in the prologue means the harness's own
 * bootstrap instructions (adrp/add/ldr/ldp/mov sp/msr nzcv) are the problem. */
static void where_stopped(FILE* f, const struct row* r, const char* err) {
  const char* p = err ? strstr(err, "at pc 0x") : NULL;
  if (!p || !r->addr || !r->enc) return;
  uint64_t pc = strtoull(p + 6, NULL, 16), insn = strtoull(r->addr, NULL, 16);
  uint64_t n = !strcmp(r->enc, "-") ? 0 : (strlen(r->enc) + 1) / 9;
  const char* where = pc < insn ? "before the instruction under test (harness prologue, or the backward landing pad)"
                      : pc < insn + 4 * n ? "at the instruction under test"
                                          : "after the instruction under test (landing pads / dump stub)";
  fprintf(f, "    stopped at pc 0x%" PRIx64 ": %s\n", pc, where);
}

static void emit_row_detail(struct sink* s, const struct row* r, const char* what, const struct a64d_rec* g, const struct a64d_rec* a,
                            const int* d, int nd, struct status gs, struct status as, const char* err) {
  FILE* outs[2] = {s->report, s->shown < s->max ? stdout : NULL};
  for (int k = 0; k < 2; k++) {
    FILE* f = outs[k];
    if (!f) continue;
    print_header(f, r, what);
    char b1[64], b2[64];
    if (!status_eq(gs, as)) fprintf(f, "    status:  golden %s, actual %s\n", status_str(gs, b1, sizeof b1), status_str(as, b2, sizeof b2));
    if (g && a)
      for (int i = 0; i < nd; i++) print_field(f, r, d[i], g, a);
    print_init(f, r);
    err_excerpt(f, err);
    where_stopped(f, r, err);
  }
  s->shown++;
}

static int cmd_compare(struct opts* o) {
  if (!o->manifest || !o->golden || !o->actual) die("compare needs --manifest --golden --actual");
  struct table t = load_manifest(o->manifest);
  struct classsum* cs = xmalloc(sizeof(struct classsum) * 64);
  int ncs = 0, golden_bad = 0, ctl_total = 0, ctl_fired = 0;
  struct sink s = {open_report(o), 0, o->max_detail};

  /* Pass 1: every selected test against its golden. */
  for (int i = 0; i < t.n; i++) {
    struct row* r = &t.rows[i];
    if (!selected(o, r)) continue;
    r->required = required(o, r);
    struct classsum* c = class_get(cs, &ncs, r->cls);
    c->tests++;
    c->required |= r->required;
    struct loaded g = load_result(o->golden, r->id);
    struct loaded a = load_result(o->actual, r->id);
    if (!rec_valid(g.out, g.outlen) || !g.st.exited || g.st.code != 0) {
      fprintf(stderr, "GOLDEN-INVALID %s: record %zu bytes, status present=%d exit=%d\n", r->id, g.outlen, g.st.present, g.st.code);
      golden_bad++;
      c->ncat[C_CRASH]++;
      free_result(&g);
      free_result(&a);
      continue;
    }
    const struct a64d_rec* gr = (const struct a64d_rec*)g.out;
    if (!rec_valid(a.out, a.outlen)) {
      enum cat k = classify_missing(a.st, a.err);
      c->ncat[k]++;
      emit_row_detail(&s, r, cat_text[k], NULL, NULL, NULL, 0, g.st, a.st, a.err);
      free_result(&g);
      free_result(&a);
      continue;
    }
    int d[F_NFIELDS];
    int nd = rec_diff(gr, (const struct a64d_rec*)a.out, d);
    if (!status_eq(g.st, a.st)) d[nd++] = F_EXIT;
    if (nd == 0) {
      c->ncat[C_PASS]++;
    } else {
      c->ncat[C_MISMATCH]++;
      emit_row_detail(&s, r, "mismatch", gr, (const struct a64d_rec*)a.out, d, nd, g.st, a.st, a.err);
    }
    free_result(&g);
    free_result(&a);
  }
  if (s.shown > s.max) printf("... %d more failures (see --report)\n", s.shown - s.max);

  /* Pass 2: positive controls.  For each class, corrupt one register field
   * and one other field in two golden records.  (a) The corrupted golden set
   * compared with the pristine golden set must flag exactly those records and
   * exactly the corrupted field.  (b) Each corrupted golden compared with the
   * actual result must be reported as a mismatch on that field. */
  printf("\n");
  for (int ci = 0; ci < ncs; ci++) {
    struct classsum* c = &cs[ci];
    int members[8192], nm = 0;
    for (int i = 0; i < t.n && nm < 8192; i++)
      if (selected(o, &t.rows[i]) && !strcmp(t.rows[i].cls, c->cls)) members[nm++] = i;
    for (int variant = 0; variant < 2; variant++) {
      uint32_t h = hash32(c->cls, o->seed + variant * 7919);
      int ctl = members[h % nm];
      struct row* cr = &t.rows[ctl];
      c->ctl_total++;
      ctl_total++;
      struct loaded g = load_result(o->golden, cr->id);
      if (!rec_valid(g.out, g.outlen)) {
        printf("CONTROL-FAIL %-10s %s: golden record invalid, control cannot be built\n", c->cls, cr->id);
        free_result(&g);
        continue;
      }
      struct loaded a = load_result(o->actual, cr->id);
      int have_actual = rec_valid(a.out, a.outlen);
      struct a64d_rec bad;
      int fld = 0;
      for (int k = 0; k < 16; k++) {
        memcpy(&bad, g.out, sizeof bad);
        fld = corrupt_record(&bad, h >> 3, variant, k);
        int d[F_NFIELDS];
        if (!have_actual || diff_has(d, rec_diff(&bad, (const struct a64d_rec*)a.out, d), fld)) break;
      }
      char fn[32];
      field_name(fld, fn, sizeof fn);
      /* (a) class-wide self check */
      int flagged = 0, wrong = 0, exact = 0;
      for (int m = 0; m < nm; m++) {
        struct row* r = &t.rows[members[m]];
        struct loaded gm = load_result(o->golden, r->id);
        if (!rec_valid(gm.out, gm.outlen)) {
          free_result(&gm);
          continue;
        }
        const struct a64d_rec* expect = members[m] == ctl ? &bad : (const struct a64d_rec*)gm.out;
        int d[F_NFIELDS];
        int nd = rec_diff(expect, (const struct a64d_rec*)gm.out, d);
        if (nd) {
          flagged++;
          if (members[m] != ctl) wrong++;
          else if (nd == 1 && d[0] == fld) exact = 1;
        }
        free_result(&gm);
      }
      int self_ok = flagged == 1 && wrong == 0 && exact;
      /* (b) against the actual run */
      int act_ok;
      const char* act_note;
      if (have_actual) {
        int d[F_NFIELDS];
        int nd = rec_diff(&bad, (const struct a64d_rec*)a.out, d);
        act_ok = diff_has(d, nd, fld);
        act_note = act_ok ? "flagged on that field" : "NOT flagged";
      } else {
        act_ok = 1;
        act_note = "flagged (actual has no record)";
      }
      int fired = self_ok && act_ok;
      c->ctl_fired += fired;
      ctl_fired += fired;
      printf("%s %-10s %s corrupt %-7s self-check flagged %d/%d records%s; vs actual: %s\n", fired ? "CONTROL-FIRED" : "CONTROL-FAIL ",
             c->cls, cr->id, fn, flagged, nm, self_ok ? " (exactly the control, exactly that field)" : " (WRONG SET OR FIELD)",
             act_note);
      free_result(&g);
      free_result(&a);
    }
  }
  if (s.report) fclose(s.report);
  print_summary(cs, ncs, ctl_fired, ctl_total, golden_bad, 0);
  int req_fail = 0;
  for (int i = 0; i < ncs; i++)
    if (cs[i].required) req_fail += cs[i].tests - cs[i].ncat[C_PASS];
  if (golden_bad || ctl_fired != ctl_total || ctl_total == 0) return 2;
  return req_fail ? 1 : 0;
}

/* check: every golden has a valid record of the expected kind and exit 0. */
static int cmd_check(struct opts* o) {
  if (!o->manifest || !o->golden) die("check needs --manifest --golden");
  struct table t = load_manifest(o->manifest);
  int bad = 0, n = 0;
  for (int i = 0; i < t.n; i++) {
    struct row* r = &t.rows[i];
    if (!selected(o, r)) continue;
    n++;
    struct loaded g = load_result(o->golden, r->id);
    int want = !strcmp(r->expect, "signal") ? A64D_KIND_SIGNAL : A64D_KIND_STATE;
    char b[64];
    if (!rec_valid(g.out, g.outlen)) {
      printf("CHECK-FAIL %s: no valid record (%zu bytes, %s)  %s  %s\n", r->id, g.outlen, status_str(g.st, b, sizeof b), r->enc, r->disasm);
      err_excerpt(stdout, g.err);
      bad++;
    } else if (((struct a64d_rec*)g.out)->kind != want || !g.st.exited || g.st.code) {
      printf("CHECK-FAIL %s: kind %u (want %d), %s  %s  %s\n", r->id, ((struct a64d_rec*)g.out)->kind, want, status_str(g.st, b, sizeof b),
             r->enc, r->disasm);
      bad++;
    }
    free_result(&g);
  }
  printf("a64diff check: %d goldens, %d invalid\n", n, bad);
  return bad ? 1 : 0;
}

/* ------------------------------------------------------------------ programs */

static void first_diff_line(FILE* f, const char* what, const char* g, size_t gl, const char* a, size_t al) {
  size_t i = 0, line = 1, ls = 0;
  while (i < gl && i < al && g[i] == a[i]) {
    if (g[i] == '\n') {
      line++;
      ls = i + 1;
    }
    i++;
  }
  fprintf(f, "    %s differs at byte %zu (line %zu); golden %zu bytes, actual %zu bytes\n", what, i, line, gl, al);
  for (int k = 0; k < 2; k++) {
    const char* b = k ? a : g;
    size_t bl = k ? al : gl;
    size_t e = ls;
    while (e < bl && b[e] != '\n' && e - ls < 160) e++;
    fprintf(f, "      %s: ", k ? "actual" : "golden");
    for (size_t q = ls; q < e; q++) fputc((unsigned char)b[q] >= 0x20 ? b[q] : '.', f);
    if (ls >= bl) fputs("<EOF>", f);
    fputc('\n', f);
  }
}

#define MAXSTEPS 32

/* One program job's results: legacy jobs have one step (<id>.out/.err); block
 * jobs have <id>.steps, <id>.<k>.out/.err and <id>.outputs. */
struct presult {
  struct status st;
  char* steps;
  int nsteps;
  char* out[MAXSTEPS], *err[MAXSTEPS];
  size_t outlen[MAXSTEPS], errlen[MAXSTEPS];
  char* outputs;
};

enum { PD_STATUS = 1, PD_STEPS = 2, PD_STDOUT = 4, PD_STDERR = 8, PD_OUTPUTS = 16 };

static struct presult load_presult(const char* dir, const struct row* r) {
  struct presult p = {0};
  p.st = read_status(dir, r->id);
  size_t n;
  if (!r->block) {
    char* q = path2(dir, r->id, ".out");
    p.out[0] = slurp(q, &p.outlen[0]);
    free(q);
    q = path2(dir, r->id, ".err");
    p.err[0] = slurp(q, &p.errlen[0]);
    free(q);
    p.nsteps = 1;
    return p;
  }
  char* q = path2(dir, r->id, ".steps");
  p.steps = slurp(q, &n);
  free(q);
  q = path2(dir, r->id, ".outputs");
  p.outputs = slurp(q, &n);
  free(q);
  for (int k = 0; k < MAXSTEPS && k < r->nsteps; k++) {
    char ext[32];
    snprintf(ext, sizeof ext, ".%d.out", k + 1);
    q = path2(dir, r->id, ext);
    p.out[k] = slurp(q, &p.outlen[k]);
    free(q);
    snprintf(ext, sizeof ext, ".%d.err", k + 1);
    q = path2(dir, r->id, ext);
    p.err[k] = slurp(q, &p.errlen[k]);
    free(q);
  }
  p.nsteps = r->nsteps < MAXSTEPS ? r->nsteps : MAXSTEPS;
  return p;
}

static void free_presult(struct presult* p) {
  free(p->steps);
  free(p->outputs);
  for (int k = 0; k < MAXSTEPS; k++) {
    free(p->out[k]);
    free(p->err[k]);
  }
}

static int buf_ne(const char* a, size_t al, const char* b, size_t bl) {
  return al != bl || (al && memcmp(a, b, al));
}

static int str_ne(const char* a, const char* b) {
  return strcmp(a ? a : "", b ? b : "");
}

static int prog_diff(const struct presult* g, const struct presult* a) {
  int d = 0;
  if (!status_eq(g->st, a->st)) d |= PD_STATUS;
  if (str_ne(g->steps, a->steps)) d |= PD_STEPS;
  for (int k = 0; k < g->nsteps; k++) {
    if (buf_ne(g->out[k], g->outlen[k], a->out[k], a->outlen[k])) d |= PD_STDOUT;
    if (buf_ne(g->err[k], g->errlen[k], a->err[k], a->errlen[k])) d |= PD_STDERR;
  }
  if (str_ne(g->outputs, a->outputs)) d |= PD_OUTPUTS;
  return d;
}

/* The manifest line for PATH in an <id>.outputs text, or NULL. */
static char* output_line(const char* text, const char* path, char* buf, size_t n) {
  if (!text) return NULL;
  size_t pl = strlen(path);
  for (const char* l = text; *l;) {
    const char* e = strchr(l, '\n');
    size_t ll = e ? (size_t)(e - l) : strlen(l);
    if (ll > pl + 1 && l[ll - pl - 1] == ' ' && !strncmp(l + ll - pl, path, pl)) {
      snprintf(buf, n, "%.*s", (int)ll, l);
      return buf;
    }
    if (!e) break;
    l = e + 1;
  }
  return NULL;
}

static void report_outputs(FILE* f, const struct row* r, const struct presult* g, const struct presult* a, const char* gdir, const char* adir) {
  /* Walk the union of paths in both manifests. */
  for (int side = 0; side < 2; side++) {
    const char* text = side ? a->outputs : g->outputs;
    if (!text) continue;
    char* copy = xstrdup(text);
    for (char* l = strtok(copy, "\n"); l; l = strtok(NULL, "\n")) {
      if (l[0] == '#') continue;
      const char* sp2 = strchr(l, ' ');
      sp2 = sp2 ? strchr(sp2 + 1, ' ') : NULL;
      if (!sp2) continue;
      const char* path = sp2 + 1;
      char gb[8192], ab[8192];
      char* gl = output_line(g->outputs, path, gb, sizeof gb), *al = output_line(a->outputs, path, ab, sizeof ab);
      if (side == 1 && gl) continue; /* already reported from the golden side */
      if (gl && al && !strcmp(gl, al)) continue;
      fprintf(f, "    output %s:\n      golden: %s\n      actual: %s\n", path, gl ? gl : "(not listed)", al ? al : "(not listed)");
      if (gl && al && strncmp(gl, "missing", 7) && strncmp(al, "missing", 7) && strncmp(gl, "link", 4) && strncmp(al, "link", 4)) {
        char* gp = path2(gdir, r->id, ".files/"), *ap = path2(adir, r->id, ".files/");
        char* gpf = path2(gp, path, ""), *apf = path2(ap, path, "");
        size_t gn, an;
        char* gc = slurp(gpf, &gn), *ac = slurp(apf, &an);
        if (gc && ac) {
          size_t i = 0;
          while (i < gn && i < an && gc[i] == ac[i]) i++;
          fprintf(f, "      first difference at byte %zu (golden %zu bytes, actual %zu bytes)\n", i, gn, an);
        }
        free(gc);
        free(ac);
        free(gp);
        free(ap);
        free(gpf);
        free(apf);
      }
    }
    free(copy);
  }
}

static void report_prog(FILE* f, const struct row* r, const char* what, const struct presult* g, const struct presult* a, int d,
                        const struct opts* o) {
  fprintf(f, "FAIL %s  [%s, %s] %s\n", r->id, r->cls, r->required ? "required" : "optional", what);
  if (r->rootfs) fprintf(f, "    rootfs: %s\n", r->rootfs);
  for (int k = 0; k < (r->block ? r->nsteps : 1); k++) {
    int argc = r->block ? r->steps[k].argc : r->argc;
    char** argv = r->block ? r->steps[k].argv : r->argv;
    const char* in = r->block ? r->steps[k].stdin_path : r->stdin_path;
    fprintf(f, "    %s%d:", r->block ? "step " : "argv", r->block ? k + 1 : 0);
    for (int ai = 0; ai < argc; ai++) fprintf(f, " '%s'", argv[ai]);
    fprintf(f, "%s%s\n", in ? " < " : "", in ? in : "");
  }
  char b1[64], b2[64];
  if (d & PD_STATUS) fprintf(f, "    status:  golden %s, actual %s\n", status_str(g->st, b1, sizeof b1), status_str(a->st, b2, sizeof b2));
  if (d & PD_STEPS) {
    fprintf(f, "    steps:   golden [");
    for (const char* c = g->steps ? g->steps : ""; *c; c++) fputc(*c == '\n' ? ';' : *c, f);
    fprintf(f, "] actual [");
    for (const char* c = a->steps ? a->steps : ""; *c; c++) fputc(*c == '\n' ? ';' : *c, f);
    fprintf(f, "]\n");
  }
  for (int k = 0; k < g->nsteps; k++) {
    char what2[32];
    if (buf_ne(g->out[k], g->outlen[k], a->out[k], a->outlen[k])) {
      snprintf(what2, sizeof what2, r->block ? "step %d stdout" : "stdout", k + 1);
      first_diff_line(f, what2, g->out[k] ? g->out[k] : "", g->outlen[k], a->out[k] ? a->out[k] : "", a->outlen[k]);
    }
    if (buf_ne(g->err[k], g->errlen[k], a->err[k], a->errlen[k])) {
      snprintf(what2, sizeof what2, r->block ? "step %d stderr" : "stderr", k + 1);
      first_diff_line(f, what2, g->err[k] ? g->err[k] : "", g->errlen[k], a->err[k] ? a->err[k] : "", a->errlen[k]);
    }
  }
  if (d & PD_OUTPUTS) report_outputs(f, r, g, a, o->golden, o->actual);
}

static enum cat classify_prog(const struct presult* a) {
  char* all = NULL;
  size_t n = 0;
  for (int k = 0; k < a->nsteps; k++)
    if (a->err[k]) {
      all = realloc(all, n + a->errlen[k] + 1);
      memcpy(all + n, a->err[k], a->errlen[k]);
      n += a->errlen[k];
      all[n] = 0;
    }
  enum cat k = C_MISMATCH;
  if (!a->st.present || a->st.timeout || (all && strstr(all, "unimplemented A64 instruction")) ||
      (all && strstr(all, "HOSTPAGEMODE") && !strstr(all, "continuing")))
    k = classify_missing(a->st, all);
  free(all);
  return k;
}

/* Replace the manifest line of the first hashed output with one for a copy of
 * the golden file that has one byte flipped.  Returns 0 if there is none. */
static int corrupt_output(struct presult* p, const struct row* r, const char* gdir, uint32_t h, const char** path_out) {
  if (!p->outputs) return 0;
  char* copy = xstrdup(p->outputs);
  int done = 0;
  for (char* l = strtok(copy, "\n"); l && !done; l = strtok(NULL, "\n")) {
    if (strlen(l) < 66 || l[64] != ' ') continue;
    char* sp2 = strchr(l + 65, ' ');
    if (!sp2) continue;
    const char* path = sp2 + 1;
    char* dirp = path2(gdir, r->id, ".files/"), *fp = path2(dirp, path, "");
    size_t n;
    char* b = slurp(fp, &n);
    free(dirp);
    free(fp);
    if (!b || !n) {
      free(b);
      continue;
    }
    b[(h >> 5) % n] ^= 0x01;
    char hex[65];
    sha256_hex((const uint8_t*)b, n, hex);
    free(b);
    char* pos = strstr(p->outputs, l);
    memcpy(pos, hex, 64);
    static char keep[4096];
    snprintf(keep, sizeof keep, "%s", path);
    *path_out = keep;
    done = 1;
  }
  free(copy);
  return done;
}

static int cmd_pcompare(struct opts* o) {
  if (!o->jobs || !o->golden || !o->actual) die("pcompare needs --jobs --golden --actual");
  struct table t = load_jobs(o->jobs, o->root);
  struct classsum* cs = xmalloc(sizeof(struct classsum) * 64);
  int ncs = 0, golden_bad = 0, ctl_total = 0, ctl_fired = 0;
  struct sink s = {open_report(o), 0, o->max_detail};
  for (int i = 0; i < t.n; i++) {
    struct row* r = &t.rows[i];
    if (!selected(o, r)) continue;
    r->required = required(o, r);
    struct classsum* c = class_get(cs, &ncs, r->cls);
    c->tests++;
    c->required |= r->required;
    struct presult g = load_presult(o->golden, r);
    struct presult a = load_presult(o->actual, r);
    if (!g.st.present || (r->block && (!g.steps || !g.outputs || strstr(g.steps, "setup error")))) {
      fprintf(stderr, "GOLDEN-INVALID %s: no golden status/steps/outputs, or setup failed\n", r->id);
      golden_bad++;
      c->ncat[C_CRASH]++;
    } else {
      int d = prog_diff(&g, &a);
      if (!d) {
        c->ncat[C_PASS]++;
      } else {
        enum cat k = classify_prog(&a);
        c->ncat[k]++;
        FILE* outs[2] = {s.report, s.shown < s.max ? stdout : NULL};
        for (int q = 0; q < 2; q++)
          if (outs[q]) report_prog(outs[q], r, cat_text[k], &g, &a, d, o);
        s.shown++;
      }
    }
    free_presult(&g);
    free_presult(&a);
  }
  if (s.shown > s.max) printf("... %d more failures (see --report)\n", s.shown - s.max);
  printf("\n");
  /* Controls, per class: corrupt one golden's stdout, one's exit status and,
   * when the class declares output files, one output file.  The corrupted
   * golden must differ from the pristine goldens in exactly that record and
   * exactly that part, and must be flagged against the actual result. */
  for (int ci = 0; ci < ncs; ci++) {
    struct classsum* c = &cs[ci];
    int members[4096], nm = 0, with_outputs = 0;
    for (int i = 0; i < t.n && nm < 4096; i++)
      if (selected(o, &t.rows[i]) && !strcmp(t.rows[i].cls, c->cls)) {
        members[nm++] = i;
        with_outputs |= t.rows[i].noutputs > 0;
      }
    for (int variant = 0; variant < (with_outputs ? 3 : 2); variant++) {
      uint32_t h = hash32(c->cls, o->seed + variant * 7919);
      int ctl = members[h % nm];
      if (variant == 2) /* pick a member that declares outputs */
        for (int m = 0; m < nm; m++) {
          int cand = members[(h + m) % nm];
          if (t.rows[cand].noutputs) {
            ctl = cand;
            break;
          }
        }
      struct row* cr = &t.rows[ctl];
      c->ctl_total++;
      ctl_total++;
      int flagged = 0, wrong = 0, exact = 0, act_ok = 0, want = variant == 0 ? PD_STDOUT : variant == 1 ? PD_STATUS : PD_OUTPUTS;
      char what[4200] = "";
      for (int m = 0; m < nm; m++) {
        struct row* r = &t.rows[members[m]];
        struct presult gm = load_presult(o->golden, r);
        struct presult expect = load_presult(o->golden, r);
        if (members[m] == ctl) {
          struct presult a = load_presult(o->actual, cr);
          if (variant == 0) {
            int k = 0;
            while (k < expect.nsteps - 1 && !expect.outlen[k]) k++;
            if (expect.outlen[k]) {
              expect.out[k][(h >> 8) % expect.outlen[k]] ^= 0x01;
            } else {
              free(expect.out[k]);
              expect.out[k] = xstrdup("x");
              expect.outlen[k] = 1;
            }
            snprintf(what, sizeof what, "stdout byte (step %d)", k + 1);
          } else if (variant == 1) {
            expect.st.code ^= 0x55;
            if (a.st.exited && a.st.code == expect.st.code) expect.st.code ^= 0x0f;
            snprintf(what, sizeof what, "exit status");
          } else {
            const char* path = "?";
            if (!corrupt_output(&expect, r, o->golden, h, &path)) snprintf(what, sizeof what, "output file (none hashed!)");
            else snprintf(what, sizeof what, "output file %s (one byte)", path);
          }
          int da = prog_diff(&expect, &a);
          act_ok = (da & want) != 0;
          free_presult(&a);
        }
        int dg = prog_diff(&expect, &gm);
        if (dg) {
          flagged++;
          if (members[m] != ctl) wrong++;
          else if (dg == want) exact = 1;
        }
        free_presult(&gm);
        free_presult(&expect);
      }
      int fired = flagged == 1 && wrong == 0 && exact && act_ok;
      c->ctl_fired += fired;
      ctl_fired += fired;
      printf("%s %-10s %s corrupt %s: flagged %d/%d%s\n", fired ? "CONTROL-FIRED" : "CONTROL-FAIL ", c->cls, cr->id, what, flagged, nm,
             fired ? " (exactly the control; also flagged against actual)" : " (WRONG SET, FIELD, OR NOT FLAGGED VS ACTUAL)");
    }
  }
  if (s.report) fclose(s.report);
  print_summary(cs, ncs, ctl_fired, ctl_total, golden_bad, 1);
  int req_fail = 0;
  for (int i = 0; i < ncs; i++)
    if (cs[i].required) req_fail += cs[i].tests - cs[i].ncat[C_PASS];
  if (golden_bad || ctl_fired != ctl_total || ctl_total == 0) return 2;
  return req_fail ? 1 : 0;
}

/* ------------------------------------------------------------------ main */

static void usage(void) {
  fputs("usage:\n"
        "  a64diff run      (--manifest M --root DIR | --jobs FILE [--root DIR]) --out DIR\n"
        "                   [-j N] [--timeout SEC] [--deadline SEC] [--only C,..] [--skip C,..] [-- PREFIX ARGS...]\n"
        "  a64diff check    --manifest M --golden DIR\n"
        "  a64diff compare  --manifest M --golden DIR --actual DIR [--report FILE] [--max-detail N]\n"
        "                   [--only C,..] [--skip C,..] [--require C,..] [--optional C,..] [--seed N]\n"
        "  a64diff pcompare --jobs FILE --golden DIR --actual DIR [same options]\n"
        "  program runs: [--rootfs NAME=PATH ...] [--rootfs-exec WRAPPER] [--break-outputs (test only)]\n"
        "                a rootfs job runs as WRAPPER PATH CWD -- argv (native), or as PREFIX argv with\n"
        "                POWERARM_ROOTFS=PATH (emulator prefix)\n"
        "exit: 0 pass, 1 required tests failed, 2 harness error or a control did not fire, 3 deadline hit\n",
        stderr);
  exit(2);
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc < 2) usage();
  struct opts o = {0};
  o.timeout = 30;
  o.max_detail = 10;
  o.seed = 1;
  const char* cmd = argv[1];
  int i;
  for (i = 2; i < argc; i++) {
    const char* a = argv[i];
#define ARG(name) (!strcmp(a, name) && i + 1 < argc)
    if (!strcmp(a, "--")) {
      i++;
      break;
    } else if (ARG("--manifest")) o.manifest = argv[++i];
    else if (ARG("--jobs")) o.jobs = argv[++i];
    else if (ARG("--root")) o.root = argv[++i];
    else if (ARG("--out")) o.out = argv[++i];
    else if (ARG("--golden")) o.golden = argv[++i];
    else if (ARG("--actual")) o.actual = argv[++i];
    else if (ARG("--report")) o.report = argv[++i];
    else if (ARG("--skip")) o.skip = argv[++i];
    else if (ARG("--only")) o.only = argv[++i];
    else if (ARG("--require")) o.require = argv[++i];
    else if (ARG("--optional")) o.optional = argv[++i];
    else if (ARG("-j")) o.jobsn = atoi(argv[++i]);
    else if (!strncmp(a, "-j", 2) && a[2]) o.jobsn = atoi(a + 2);
    else if (ARG("--timeout")) o.timeout = atof(argv[++i]);
    else if (ARG("--deadline")) o.deadline = atof(argv[++i]);
    else if (ARG("--max-detail")) o.max_detail = atoi(argv[++i]);
    else if (ARG("--seed")) o.seed = strtoul(argv[++i], NULL, 0);
    else if (ARG("--rootfs")) {
      char* v = argv[++i], *eq = strchr(v, '=');
      if (!eq || o.nrootfs == 16) die("--rootfs wants NAME=PATH");
      *eq = 0;
      o.rootfs_names[o.nrootfs] = v;
      char* abs = realpath(eq + 1, NULL); /* block jobs chdir: keep it absolute */
      o.rootfs_paths[o.nrootfs++] = abs ? abs : eq + 1;
    }
    else if (ARG("--rootfs-exec")) o.rootfs_exec = argv[++i];
    else if (!strcmp(a, "--break-outputs")) {
      o.break_outputs = 1;
      fprintf(stderr, "a64diff: TEST MODE: declared output files are damaged before hashing\n");
    }
    else if (ARG("--blind-field")) {
      const char* want = argv[++i];
      char nb[32];
      for (int f = 0; f < F_NFIELDS; f++)
        if (!strcmp(field_name(f, nb, sizeof nb), want)) blind_field = f;
      if (blind_field < 0) die("unknown field %s", want);
      fprintf(stderr, "a64diff: TEST MODE: comparator is blind to field %s\n", want);
    }
    else usage();
#undef ARG
  }
  o.prefix = argv + i;
  o.nprefix = argc - i;
  if (!strcmp(cmd, "run")) return cmd_run(&o);
  if (!strcmp(cmd, "check")) return cmd_check(&o);
  if (!strcmp(cmd, "compare")) return cmd_compare(&o);
  if (!strcmp(cmd, "pcompare")) return cmd_pcompare(&o);
  usage();
  return 2;
}
