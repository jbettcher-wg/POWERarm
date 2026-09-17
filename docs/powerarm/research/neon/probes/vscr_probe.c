/* vscr_probe.c -- does VSCR.SAT track FPSR.QC?  For each candidate lowering
 * that uses a saturating VMX instruction for a NON-saturating guest op, run
 * the corpus and check SAT stays clear; for the saturating guest ops, check
 * SAT is set exactly when the ARM reference saturates (QC).  Also records the
 * cost of clearing/reading VSCR.  Build: gcc -O2 -mcpu=power8 -o vscr_probe vscr_probe.c */
#include "neon_ref.h"
typedef unsigned char v16 __attribute__((vector_size(16)));
static inline v16 LDV(const V* p) { v16 t; memcpy(&t, p->b, 16); return t; }
static inline void vscr_clear(void) { v16 z = {0}; __asm__ volatile("mtvscr %0" :: "v"(z)); }
static inline int vscr_sat(void) { v16 s; __asm__ volatile("mfvscr %0" : "=v"(s)); V t; memcpy(t.b, &s, 16); return t.w[0] & 1; }
static int qc_ref(const V* r_ref, const V* a, const V* b, int W, int op) { /* 1 if the ARM op would saturate on some lane */
  for (int i = 0; i < 128 / W; i++) { uint64_t x = getl(a, i, W), y = getl(b, i, W); __int128 s;
    switch (op) { case 0: s = (__int128)x + y; if (s != (__int128)sat_u(s, W)) return 1; break; case 1: s = (__int128)sext(x, W) + sext(y, W); if (s != sat_s(s, W)) return 1; break; } }
  return 0; }
int main(void) {
  int bad = 0, sat_seen = 0, n = 0;
  /* 1. vaddubs (UQADD.16B) exhaustive: SAT iff QC */
  for (unsigned k = 0; k < N8PAIRS; k++) { V a, b; corpus8(k, &a, &b); v16 x = LDV(&a), y = LDV(&b), r; vscr_clear(); __asm__ volatile("vaddubs %0,%1,%2" : "=v"(r) : "v"(x), "v"(y)); int s = vscr_sat(); int q = qc_ref(NULL, &a, &b, 8, 0); n++; if (s != q) { bad++; if (bad < 4) printf("  vaddubs SAT=%d QC=%d k=%u\n", s, q, k); } sat_seen += s; }
  printf("vaddubs (UQADD.16B): %d pairs-of-vectors, SAT==QC on all: %s (SAT set in %d)\n", n, bad ? "NO" : "yes", sat_seen);
  /* 2. vaddsbs (SQADD.16B) exhaustive */
  bad = 0; n = 0; sat_seen = 0;
  for (unsigned k = 0; k < N8PAIRS; k++) { V a, b; corpus8(k, &a, &b); v16 x = LDV(&a), y = LDV(&b), r; vscr_clear(); __asm__ volatile("vaddsbs %0,%1,%2" : "=v"(r) : "v"(x), "v"(y)); int s = vscr_sat(); int q = qc_ref(NULL, &a, &b, 8, 1); n++; if (s != q) bad++; sat_seen += s; }
  printf("vaddsbs (SQADD.16B): SAT==QC on all: %s (SAT set in %d of %d)\n", bad ? "NO" : "yes", sat_seen, n);
  /* 3. ADDV.16B / .8H / SADDLV / UADDLV via vsum4ubs/vsum4sbs/vsum4shs + vsumsws must never set SAT */
  rng_seed(7); int hits[4] = {0};
  for (int k = 0; k < 200000; k++) { V a; randv(&a); v16 x = LDV(&a), z = {0}, t, r;
    vscr_clear(); __asm__ volatile("vsum4ubs %0,%1,%2\n\tvsumsws %0,%0,%2" : "=&v"(t) : "v"(x), "v"(z)); hits[0] += vscr_sat();
    vscr_clear(); __asm__ volatile("vsum4sbs %0,%1,%2\n\tvsumsws %0,%0,%2" : "=&v"(t) : "v"(x), "v"(z)); hits[1] += vscr_sat();
    vscr_clear(); __asm__ volatile("vsum4shs %0,%1,%2\n\tvsumsws %0,%0,%2" : "=&v"(t) : "v"(x), "v"(z)); hits[2] += vscr_sat();
    vscr_clear(); __asm__ volatile("vsumsws %0,%1,%2" : "=&v"(t) : "v"(x), "v"(z)); hits[3] += vscr_sat(); (void)r; }
  printf("ADDV.16B (vsum4ubs+vsumsws) SAT hits: %d; SADDLV.16B (vsum4sbs+vsumsws): %d; ADDV/SADDLV.8H (vsum4shs+vsumsws): %d; vsumsws alone on 4 random words (the VAddV i32 bug): %d of 200000\n", hits[0], hits[1], hits[2], hits[3]);
  /* 4. worst cases by construction */
  { V a; for (int i = 0; i < 16; i++) a.b[i] = 0xFF; v16 x = LDV(&a), z = {0}, t; vscr_clear(); __asm__ volatile("vsum4ubs %0,%1,%2\n\tvsumsws %0,%0,%2" : "=&v"(t) : "v"(x), "v"(z)); printf("ADDV.16B all-0xFF: SAT=%d\n", vscr_sat());
    for (int i = 0; i < 8; i++) a.h[i] = 0x8000; x = LDV(&a); vscr_clear(); __asm__ volatile("vsum4shs %0,%1,%2\n\tvsumsws %0,%0,%2" : "=&v"(t) : "v"(x), "v"(z)); printf("ADDV.8H all-0x8000: SAT=%d\n", vscr_sat());
    for (int i = 0; i < 4; i++) a.w[i] = 0x7FFFFFFF; x = LDV(&a); vscr_clear(); __asm__ volatile("vsumsws %0,%1,%2" : "=&v"(t) : "v"(x), "v"(z)); printf("vsumsws on 4x INT_MAX (ADDV.4S via vsumsws): SAT=%d (ARM ADDV wraps: QC must stay 0)\n", vscr_sat()); }
  /* 5. modulo packs (XTN/SHRN/UZP1) and vmhaddshs/vmhraddshs (SQDMULH: ARM sets QC only for 0x8000*0x8000) */
  rng_seed(9); int pk = 0, mh = 0, mhq = 0, mhw = 0;
  for (int k = 0; k < 100000; k++) { V a, b; randv16(&a); randv16(&b); v16 x = LDV(&a), y = LDV(&b), z = {0}, t;
    vscr_clear(); __asm__ volatile("vpkuhum %0,%1,%2\n\tvpkuwum %0,%0,%2" : "=&v"(t) : "v"(x), "v"(y)); pk += vscr_sat();
    int q = 0; for (int i = 0; i < 8; i++) if (a.h[i] == 0x8000 && b.h[i] == 0x8000) q = 1;
    vscr_clear(); __asm__ volatile("vmhaddshs %0,%1,%2,%3" : "=v"(t) : "v"(x), "v"(y), "v"(z)); int s = vscr_sat(); mh += s; if (s != q) mhw++;
    vscr_clear(); __asm__ volatile("vmhraddshs %0,%1,%2,%3" : "=v"(t) : "v"(x), "v"(y), "v"(z)); s = vscr_sat(); mhq += (s != q); }
  printf("vpkuhum/vpkuwum (XTN/SHRN/UZP1) SAT hits: %d of 100000; vmhaddshs (SQDMULH.8H) SAT!=QC: %d, vmhraddshs: %d (SAT set %d times)\n", pk, mhw, mhq, mh);
  /* 6. cost of the QC bookkeeping: mfvscr at MRS FPSR only vs per op */
  { uint64_t t0, t1; v16 x = {1}, y = {2}, r; __asm__ volatile("mftb %0" : "=r"(t0)); for (int i = 0; i < 100000; i++) __asm__ volatile("vaddubs %0,%1,%2" : "=v"(r) : "v"(x), "v"(y)); __asm__ volatile("mftb %0" : "=r"(t1)); double a1 = (t1 - t0) / 100000.0;
    __asm__ volatile("mftb %0" : "=r"(t0)); for (int i = 0; i < 100000; i++) { v16 s; __asm__ volatile("vaddubs %0,%1,%2\n\tmfvscr %3" : "=v"(r), "=v"(s) : "v"(x), "v"(y)); } __asm__ volatile("mftb %0" : "=r"(t1)); double a2 = (t1 - t0) / 100000.0;
    printf("time-base ticks per op: vaddubs alone %.2f, vaddubs+mfvscr %.2f (%.1fx)\n", a1, a2, a2 / a1); }
  return 0;
}
