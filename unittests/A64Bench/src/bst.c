/* Pointer chasing: unbalanced binary search tree with random keys, insert then lookup,
 * then an explicit-stack in-order walk. Stresses: data cache misses, unpredictable
 * compare-and-branch on loaded values. */
static const char BENCH_NAME[] = "bst";
static const unsigned long BENCH_DEFAULT_SCALE = 400000; /* nodes */
#include "bench.h"

#define MAX_NODES (4u << 20)
struct node {
  u64 key;
  struct node *l, *r;
};
static struct node *pool;
static struct node pool_storage[MAX_NODES];
static struct node *stack[4096];

static void bench_setup(u64 scale) {
  (void)scale;
  pool = pool_storage;
}

static u64 bench_run(u64 scale) {
  if (scale > MAX_NODES) scale = MAX_NODES;
  bench_rng_state = 0x243F6A8885A308D3ul; /* identical tree every rep */
  u64 used = 0;
  struct node *root = 0;
  for (u64 i = 0; i < scale; i++) {
    u64 k = bench_rand() >> 1;
    struct node **pp = &root;
    while (*pp) {
      if (k == (*pp)->key) goto dup;
      pp = k < (*pp)->key ? &(*pp)->l : &(*pp)->r;
    }
    struct node *n = &pool[used++];
    n->key = k;
    n->l = n->r = 0;
    *pp = n;
  dup:;
  }
  /* Lookups: replay the insert stream for hits, interleaved with fresh keys for misses. */
  u64 rs = bench_rng_state;
  bench_rng_state = 0x243F6A8885A308D3ul;
  u64 hits = 0, depth = 0;
  for (u64 i = 0; i < scale; i++) {
    u64 k = bench_rand() >> 1;
    if (i & 1) {
      u64 save = bench_rng_state;
      bench_rng_state = rs;
      k = bench_rand() >> 1;
      rs = bench_rng_state;
      bench_rng_state = save;
    }
    struct node *n = root;
    while (n && n->key != k) {
      n = k < n->key ? n->l : n->r;
      depth++;
    }
    hits += n != 0;
  }
  /* In-order walk with an explicit stack: order-sensitive hash. */
  u64 h = 0, sp = 0;
  struct node *n = root;
  while (n || sp) {
    while (n) {
      stack[sp++] = n;
      n = n->l;
    }
    n = stack[--sp];
    h = (h ^ n->key) * 0x100000001B3ul;
    n = n->r;
  }
  return h ^ (hits << 40) ^ (depth << 1) ^ used;
}
