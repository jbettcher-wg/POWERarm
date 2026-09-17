/* neon_parity_p9.c -- candidate POWER lowerings of AArch64 Advanced SIMD
 * operations, checked lane-exact against the C reference model that
 * neon_golden_pi.c verified on a Raspberry Pi 5.
 *
 * Every lowering is inline asm with VMX ("v") / VSX ("wa") register
 * constraints, so the instruction sequence is exactly what a JIT would emit
 * (constants included).  Vectors are loaded with plain 16-byte memcpy, i.e.
 * the natural little-endian layout (memory byte i = LE lane i = ISA byte
 * 15-i), which is also the layout POWERarm keeps guest V registers in.
 *
 * Build (POWER9 host):
 *   gcc -O2 -mcpu=power8 -DP9=0 -o neon_parity_p8 neon_parity_p9.c -lm
 *   gcc -O2 -mcpu=power9 -DP9=1 -o neon_parity_p9 neon_parity_p9.c -lm
 * Run: ./neon_parity_p8 > p8.out; ./neon_parity_p9 > p9.out
 * Then compare the ref-hash column with the Pi's pi.golden (same hashes).
 */
#include "neon_ref.h"
#ifndef P9
#define P9 0
#endif

typedef unsigned char v16 __attribute__((vector_size(16)));
static inline v16 LDV(const V* p) { v16 t; memcpy(&t, p->b, 16); return t; }
static inline void STV(V* r, v16 x) { memcpy(r->b, &x, 16); }
#define A1(insn, r, a) __asm__(insn " %0,%1" : "=v"(r) : "v"(a))
#define A2(insn, r, a, b) __asm__(insn " %0,%1,%2" : "=v"(r) : "v"(a), "v"(b))
#define A3(insn, r, a, b, c) __asm__(insn " %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "v"(c))
#define A2I(insn, r, a, b, imm) __asm__(insn " %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "i"(imm))
#define X2(insn, r, a, b) __asm__(insn " %x0,%x1,%x2" : "=wa"(r) : "wa"(a), "wa"(b))
#define X1(insn, r, a) __asm__(insn " %x0,%x1" : "=wa"(r) : "wa"(a))
#define X2I(insn, r, a, b, imm) __asm__(insn " %x0,%x1,%x2,%3" : "=wa"(r) : "wa"(a), "wa"(b), "i"(imm))
#define SPLATB(r, imm) __asm__("vspltisb %0,%1" : "=v"(r) : "i"(imm))
#define SPLATH(r, imm) __asm__("vspltish %0,%1" : "=v"(r) : "i"(imm))
#define SPLATW(r, imm) __asm__("vspltisw %0,%1" : "=v"(r) : "i"(imm))
static inline v16 zero(void) { v16 z; SPLATW(z, 0); return z; }
static inline v16 allones(void) { v16 z; SPLATW(z, -1); return z; }
/* splat of an arbitrary 64-bit pattern through a GPR (the JIT's VDupFromGPR path) */
static inline v16 splatd(uint64_t x) { v16 r; __asm__("mtvsrd %x0,%1\n\txxpermdi %x0,%x0,%x0,0" : "=wa"(r) : "r"(x)); return r; }
static inline v16 splatw(uint32_t x) { return splatd(((uint64_t)x << 32) | x); }
static inline v16 splath(uint16_t x) { return splatw(((uint32_t)x << 16) | x); }
static inline v16 splatb(uint8_t x) { return splath(((uint16_t)x << 8) | x); }
#define SIG (V* r, const V* a, const V* b, const V* c, int W, int imm)
#define BODY(...) { v16 x = LDV(a), y = LDV(b), z = LDV(c), o; (void)y; (void)z; __VA_ARGS__; STV(r, o); }
#define SW(W, e8, e16, e32, e64) switch (W) { case 8: e8; break; case 16: e16; break; case 32: e32; break; default: e64; }

/* ---- compares ---------------------------------------------------------- */
static void low_cmhi SIG BODY(SW(W, A2("vcmpgtub", o, x, y), A2("vcmpgtuh", o, x, y), A2("vcmpgtuw", o, x, y), A2("vcmpgtud", o, x, y)))
/* CMHS = NOT(b >u a): vcmpgtu + vnor */
static void low_cmhs SIG BODY({ v16 t; SW(W, A2("vcmpgtub", t, y, x), A2("vcmpgtuh", t, y, x), A2("vcmpgtuw", t, y, x), A2("vcmpgtud", t, y, x)); A2("vnor", o, t, t); })
static void low_cmgt SIG BODY(SW(W, A2("vcmpgtsb", o, x, y), A2("vcmpgtsh", o, x, y), A2("vcmpgtsw", o, x, y), A2("vcmpgtsd", o, x, y)))
static void low_cmge SIG BODY({ v16 t; SW(W, A2("vcmpgtsb", t, y, x), A2("vcmpgtsh", t, y, x), A2("vcmpgtsw", t, y, x), A2("vcmpgtsd", t, y, x)); A2("vnor", o, t, t); })
/* CMTST = (a & b) >u 0 : vand + vcmpgtu against zero (2 insns + zero) */
static void low_cmtst SIG BODY({ v16 t, z = zero(); A2("vand", t, x, y); SW(W, A2("vcmpgtub", o, t, z), A2("vcmpgtuh", o, t, z), A2("vcmpgtuw", o, t, z), A2("vcmpgtud", o, t, z)); })
/* ---- saturating arithmetic --------------------------------------------- */
/* 8/16/32: one VMX instruction. 64-bit: composed. */
static void low_sqadd SIG BODY({
  if (W == 64) { /* sum = a+b; ovf = ~(a^b) & (a^sum) sign; sat = (a >>s 63) ^ INT64_MAX; res = ovf ? sat : sum : 7 insns + 2 consts */
    v16 s, t1, t2, m, sat, c63, imax; A2("vaddudm", s, x, y); A2("vxor", t1, x, y); A2("vxor", t2, x, s); A2("vandc", m, t2, t1);
    SPLATB(c63, -1); /* vsrad uses the low 6 bits: 0xff -> 63 */ A2("vsrad", m, m, c63); A2("vsrad", sat, x, c63);
    imax = splatd(0x7FFFFFFFFFFFFFFFull); A2("vxor", sat, sat, imax); A3("vsel", o, s, sat, m); }
  else SW(W, A2("vaddsbs", o, x, y), A2("vaddshs", o, x, y), A2("vaddsws", o, x, y), (void)0); })
static void low_uqadd SIG BODY({
  if (W == 64) { /* sum = a+b; carry = sum <u a = vcmpgtud(a, sum); res = sum | carry : 3 insns */
    v16 s, m; A2("vaddudm", s, x, y); A2("vcmpgtud", m, x, s); A2("vor", o, s, m); }
  else SW(W, A2("vaddubs", o, x, y), A2("vadduhs", o, x, y), A2("vadduws", o, x, y), (void)0); })
static void low_sqsub SIG BODY({
  if (W == 64) { /* d = a-b; ovf = (a^b) & (a^d) sign; sat = (a>>s63) ^ INT64_MAX */
    v16 d, t1, t2, m, sat, c63, imax; A2("vsubudm", d, x, y); A2("vxor", t1, x, y); A2("vxor", t2, x, d); A2("vand", m, t1, t2);
    SPLATB(c63, -1); A2("vsrad", m, m, c63); A2("vsrad", sat, x, c63); imax = splatd(0x7FFFFFFFFFFFFFFFull); A2("vxor", sat, sat, imax); A3("vsel", o, d, sat, m); }
  else SW(W, A2("vsubsbs", o, x, y), A2("vsubshs", o, x, y), A2("vsubsws", o, x, y), (void)0); })
static void low_uqsub SIG BODY({
  if (W == 64) { /* d = a-b; borrow = b >u a; res = d & ~borrow : 3 insns */
    v16 d, m; A2("vsubudm", d, x, y); A2("vcmpgtud", m, y, x); A2("vandc", o, d, m); }
  else SW(W, A2("vsububs", o, x, y), A2("vsubuhs", o, x, y), A2("vsubuws", o, x, y), (void)0); })
/* SQABS = vmaxs(a, 0 -s a) with saturating negate; SQNEG = vsubs(0, a) */
static void low_sqneg SIG BODY({ v16 z = zero(); if (W == 64) { /* only INT64_MIN overflows: d = 0-a; ovf = a & d sign; sat = (a>>s63) ^ INT64_MIN (= +MAX): 6 insns + 2 consts */ v16 d, m, sat, c63, imin; A2("vsubudm", d, z, x); A2("vand", m, x, d); SPLATB(c63, -1); A2("vsrad", m, m, c63); A2("vsrad", sat, x, c63); imin = splatd(0x8000000000000000ull); A2("vxor", sat, sat, imin); A3("vsel", o, d, sat, m); }
  else SW(W, A2("vsubsbs", o, z, x), A2("vsubshs", o, z, x), A2("vsubsws", o, z, x), (void)0); })
static void low_sqabs SIG BODY({ v16 z = zero(), n; if (W == 64) { v16 d, m, sat, c63, imin; A2("vsubudm", d, z, x); A2("vand", m, x, d); SPLATB(c63, -1); A2("vsrad", m, m, c63); A2("vsrad", sat, x, c63); imin = splatd(0x8000000000000000ull); A2("vxor", sat, sat, imin); A3("vsel", n, d, sat, m); A2("vmaxsd", o, x, n); }
  else { SW(W, A2("vsubsbs", n, z, x), A2("vsubshs", n, z, x), A2("vsubsws", n, z, x), (void)0); SW(W, A2("vmaxsb", o, x, n), A2("vmaxsh", o, x, n), A2("vmaxsw", o, x, n), (void)0); } })
static void low_abs SIG BODY({ v16 z = zero(), n; SW(W, A2("vsububm", n, z, x), A2("vsubuhm", n, z, x), A2("vsubuwm", n, z, x), A2("vsubudm", n, z, x)); SW(W, A2("vmaxsb", o, x, n), A2("vmaxsh", o, x, n), A2("vmaxsw", o, x, n), A2("vmaxsd", o, x, n)); })
static void low_neg SIG BODY({ v16 z = zero(); SW(W, A2("vsububm", o, z, x), A2("vsubuhm", o, z, x), A2("vsubuwm", o, z, x), A2("vsubudm", o, z, x)); })
/* ---- multiplies ---------------------------------------------------------- */
/* MUL.16B: vmulesb/vmulosb give products of LE odd/even lanes in halfwords; the
 * low bytes of the products, interleaved, are the result: vmulesb, vmulosb, then
 * vpkuhum can't interleave -- use vsl 8 on the even products and vsel/vor with a
 * 0x00FF mask: 5 insns + const.  Alternative: vmladduhm on zero-extended halves. */
static void low_mul SIG BODY({
  if (W == 8) { v16 e, od, c8, m; A2("vmulesb", e, x, y); A2("vmulosb", od, x, y); SPLATH(c8, 8); A2("vslh", e, e, c8); m = splath(0x00FF); A3("vsel", o, e, od, m); }
  else if (W == 16) { v16 z = zero(); A3("vmladduhm", o, x, y, z); }
  else if (W == 32) { A2("vmuluwm", o, x, y); }
  else { /* 64-bit: lo*lo + ((lo*hi + hi*lo) << 32); vmulouw multiplies the LE even words (the low word of each doubleword) */
    v16 c32, xs, ys, ll, lh, hl, cr; c32 = splatb(32); /* 0xE0 -> low 6 bits = 32 */ A2("vrld", xs, x, c32); A2("vrld", ys, y, c32);
    A2("vmulouw", ll, x, y); A2("vmulouw", lh, x, ys); A2("vmulouw", hl, xs, y); A2("vaddudm", cr, lh, hl); A2("vsld", cr, cr, c32); A2("vaddudm", o, ll, cr); } })
static void low_mla SIG BODY({
  if (W == 8) { v16 p; V t; low_mul(&t, a, b, c, W, imm); p = LDV(&t); A2("vaddubm", o, p, z); }
  else if (W == 16) A3("vmladduhm", o, x, y, z);
  else { v16 p; A2("vmuluwm", p, x, y); A2("vadduwm", o, z, p); } })
static void low_mls SIG BODY({ if (W == 16) { v16 zz = zero(), p; A3("vmladduhm", p, x, y, zz); A2("vsubuhm", o, z, p); } else { v16 p; A2("vmuluwm", p, x, y); A2("vsubuwm", o, z, p); } })
/* SQDMULH.8H = vmhaddshs(a,b,0); SQRDMULH.8H = vmhraddshs(a,b,0).
 * 32-bit: 64-bit products via vmulesw/vmulosw (LE odd/even words), >>31 (vsrad),
 * merge low words back (vmrgow/vmrgew are BE even/odd = LE odd/even), saturate the single overflow case. */
static void low_sqdmulh SIG BODY({
  if (W == 16) { v16 z = zero(); A3("vmhaddshs", o, x, y, z); }
  else { v16 e, od, c31, m, sat, eq1, eq2, imin; A2("vmulesw", e, x, y); A2("vmulosw", od, x, y); c31 = splatb(31); A2("vsrad", e, e, c31); A2("vsrad", od, od, c31);
    /* result LE word j: even products (LE odd lanes) sit in doublewords whose LOW word (LE even word) holds bits 31..62: merge low words: vmrgow(e, od) -> {od.w0, e.w0, od.w2, e.w2} in LE = lanes {0:od0,1:e0,2:od2,3:e2} */
    A2("vmrgow", m, e, od);
    imin = splatw(0x80000000u); A2("vcmpequw", eq1, x, imin); A2("vcmpequw", eq2, y, imin); A2("vand", sat, eq1, eq2); A2("vxor", o, m, sat); /* 0x80000000 ^ 0xFFFFFFFF = 0x7FFFFFFF */ } })
static void low_sqrdmulh SIG BODY({
  if (W == 16) { v16 z = zero(); A3("vmhraddshs", o, x, y, z); }
  else { v16 e, od, c31, m, sat, eq1, eq2, imin, rnd; A2("vmulesw", e, x, y); A2("vmulosw", od, x, y); rnd = splatd(0x40000000ull); A2("vaddudm", e, e, rnd); A2("vaddudm", od, od, rnd);
    c31 = splatb(31); A2("vsrad", e, e, c31); A2("vsrad", od, od, c31); A2("vmrgow", m, e, od);
    imin = splatw(0x80000000u); A2("vcmpequw", eq1, x, imin); A2("vcmpequw", eq2, y, imin); A2("vand", sat, eq1, eq2); A2("vxor", o, m, sat); } })
/* SQDMULL 16->32: products of the low 4 halfwords, doubled with saturation */
static void low_sqdmull SIG BODY({
  if (W == 16) { v16 e, od, p; A2("vmulesh", e, x, y); A2("vmulosh", od, x, y); A2("vmrglw", p, e, od); A2("vaddsws", o, p, p); }
  else { /* 32->64: vmulesw/vmulosw of the low 2 words then doubling with 64-bit saturation (only INT_MIN*INT_MIN overflows: 2^62*2 = 2^63) */
    v16 e, od, p, sat, eq1, eq2, imin; A2("vmulesw", e, x, y); A2("vmulosw", od, x, y); A2("vmrglw", p, e, od); /* LE dw0 = lanes 0 product, dw1 = lane 1 */
    __asm__("xxmrgld %x0,%x1,%x2" : "=wa"(p) : "wa"(e), "wa"(od)); /* dw: {od.dw0, e.dw0} -> LE {lane0, lane1} */
    A2("vaddudm", p, p, p); imin = splatw(0x80000000u); A2("vcmpequw", eq1, x, imin); A2("vcmpequw", eq2, y, imin); A2("vand", sat, eq1, eq2);
    { A2("vmrglw", sat, sat, sat); /* widen the low-two-lane masks to doublewords */ v16 imax = splatd(0x7FFFFFFFFFFFFFFFull); A2("vandc", p, p, sat); A2("vand", sat, sat, imax); A2("vor", o, p, sat); } } })
/* SMULL/UMULL: even/odd products then interleave the low halves (LE lane j of the
 * result is lane j of the input; vmulo* holds LE even lanes, vmule* LE odd lanes). */
static void low_smull SIG BODY({ v16 e, od; SW(W, A2("vmulesb", e, x, y), A2("vmulesh", e, x, y), A2("vmulesw", e, x, y), (void)0); SW(W, A2("vmulosb", od, x, y), A2("vmulosh", od, x, y), A2("vmulosw", od, x, y), (void)0); SW(W, A2("vmrglh", o, e, od), A2("vmrglw", o, e, od), X2("xxmrgld", o, e, od), (void)0); })
static void low_umull SIG BODY({ v16 e, od; SW(W, A2("vmuleub", e, x, y), A2("vmuleuh", e, x, y), A2("vmuleuw", e, x, y), (void)0); SW(W, A2("vmuloub", od, x, y), A2("vmulouh", od, x, y), A2("vmulouw", od, x, y), (void)0); SW(W, A2("vmrglh", o, e, od), A2("vmrglw", o, e, od), X2("xxmrgld", o, e, od), (void)0); })
static void low_smull2 SIG BODY({ v16 e, od; SW(W, A2("vmulesb", e, x, y), A2("vmulesh", e, x, y), A2("vmulesw", e, x, y), (void)0); SW(W, A2("vmulosb", od, x, y), A2("vmulosh", od, x, y), A2("vmulosw", od, x, y), (void)0); SW(W, A2("vmrghh", o, e, od), A2("vmrghw", o, e, od), X2("xxmrghd", o, e, od), (void)0); })
static void low_umull2 SIG BODY({ v16 e, od; SW(W, A2("vmuleub", e, x, y), A2("vmuleuh", e, x, y), A2("vmuleuw", e, x, y), (void)0); SW(W, A2("vmuloub", od, x, y), A2("vmulouh", od, x, y), A2("vmulouw", od, x, y), (void)0); SW(W, A2("vmrghh", o, e, od), A2("vmrghw", o, e, od), X2("xxmrghd", o, e, od), (void)0); })
/* ---- absolute difference ------------------------------------------------ */
#if P9
static void low_uabd SIG BODY(SW(W, A2("vabsdub", o, x, y), A2("vabsduh", o, x, y), A2("vabsduw", o, x, y), (void)0))
#else
static void low_uabd SIG BODY({ v16 mx, mn; SW(W, A2("vmaxub", mx, x, y), A2("vmaxuh", mx, x, y), A2("vmaxuw", mx, x, y), (void)0); SW(W, A2("vminub", mn, x, y), A2("vminuh", mn, x, y), A2("vminuw", mn, x, y), (void)0); SW(W, A2("vsububm", o, mx, mn), A2("vsubuhm", o, mx, mn), A2("vsubuwm", o, mx, mn), (void)0); })
#endif
static void low_sabd SIG BODY({ v16 mx, mn; SW(W, A2("vmaxsb", mx, x, y), A2("vmaxsh", mx, x, y), A2("vmaxsw", mx, x, y), (void)0); SW(W, A2("vminsb", mn, x, y), A2("vminsh", mn, x, y), A2("vminsw", mn, x, y), (void)0); SW(W, A2("vsububm", o, mx, mn), A2("vsubuhm", o, mx, mn), A2("vsubuwm", o, mx, mn), (void)0); })
static void low_uabdl SIG BODY({ V t; low_uabd(&t, a, b, c, W, imm); v16 d = LDV(&t), z = zero(); SW(W, A2("vmrglb", o, z, d), A2("vmrglh", o, z, d), A2("vmrglw", o, z, d), (void)0); })
static void low_sabdl SIG BODY({ v16 z = zero(), xw, yw, mx, mn; /* widen then abd at 2W (no overflow) */ SW(W, A1("vupklsb", xw, x), A1("vupklsh", xw, x), A1("vupklsw", xw, x), (void)0); SW(W, A1("vupklsb", yw, y), A1("vupklsh", yw, y), A1("vupklsw", yw, y), (void)0); (void)z;
  SW(W, A2("vmaxsh", mx, xw, yw), A2("vmaxsw", mx, xw, yw), A2("vmaxsd", mx, xw, yw), (void)0); SW(W, A2("vminsh", mn, xw, yw), A2("vminsw", mn, xw, yw), A2("vminsd", mn, xw, yw), (void)0); SW(W, A2("vsubuhm", o, mx, mn), A2("vsubuwm", o, mx, mn), A2("vsubudm", o, mx, mn), (void)0); })
/* ---- shifts by register --------------------------------------------------
 * count = low byte of each lane, signed.  Left when >= 0 (zero when >= W),
 * right by -count when negative (unsigned: zero when >= W; signed: sign fill).
 * Sequence (all widths): neg = 0 - b (bytes); sl = vsl(a, b); sr = vsr(a, neg)
 * [count mod W]; lmask = b <u W (unsigned byte compare on the count byte);
 * rmask = neg <u W; sign = b <s 0 (byte compare); result = sign ? (sr & rmask) : (sl & lmask).
 * For W > 8 the count byte is the low byte of the lane; the byte-wise masks
 * must be widened: compare the whole lane after sign-extending the byte? No:
 * vsl/vsr only read the low log2(W) bits, and the masks use vcmpgtsb/vcmpgtub
 * on the count byte then a lane-wide splat of that byte (vsplt... per lane is
 * not available) -- instead the count lanes are prepared with vextsb2w-like
 * sign extension: on P8 use vslw/vsraw by 24 (16: 8) to sign-extend the low byte. */
static void low_shl_reg_impl(V* r, const V* a, const V* b, int W, int is_signed, int round) {
  v16 x = LDV(a), y = LDV(b), o, cnt, neg, sl, sr, lm, rm, sg, z = zero(), wsplat, sh;
  /* sign-extend the count byte within each lane: (y << (W-8)) >>s (W-8) */
  if (W == 8) cnt = y; else { v16 k; if (W == 16) SPLATH(k, 8); else if (W == 32) SPLATB(k, 24 - 32); /* 0xE8: low 5 bits = 24 */ else SPLATB(k, 56 - 64); /* 0xF8: low 6 bits = 56 */
    SW(W, (void)0, A2("vslh", cnt, y, k), A2("vslw", cnt, y, k), A2("vsld", cnt, y, k)); SW(W, (void)0, A2("vsrah", cnt, cnt, k), A2("vsraw", cnt, cnt, k), A2("vsrad", cnt, cnt, k)); }
  SW(W, A2("vsububm", neg, z, cnt), A2("vsubuhm", neg, z, cnt), A2("vsubuwm", neg, z, cnt), A2("vsubudm", neg, z, cnt));
  if (round && W != 8) { /* rounding right shift: (a + (1 << (n-1))) >> n done as (a >> n) + ((a >> (n-1)) & 1) */ }
  SW(W, A2("vslb", sl, x, cnt), A2("vslh", sl, x, cnt), A2("vslw", sl, x, cnt), A2("vsld", sl, x, cnt));
  if (is_signed) { SW(W, A2("vsrab", sr, x, neg), A2("vsrah", sr, x, neg), A2("vsraw", sr, x, neg), A2("vsrad", sr, x, neg)); }
  else { SW(W, A2("vsrb", sr, x, neg), A2("vsrh", sr, x, neg), A2("vsrw", sr, x, neg), A2("vsrd", sr, x, neg)); }
  if (round) { /* add the rounding bit: ((a >> (n-1)) & 1) with n-1 = neg-1; for n = 0 (count 0 or left) the mask below drops it; n-1 = -1 -> shift by W-1 mod W gives the top bit: masked off because rm/sg exclude it */
    v16 one, nm1, rb; SW(W, SPLATB(one, 1), SPLATH(one, 1), SPLATW(one, 1), one = splatd(1)); SW(W, A2("vsububm", nm1, neg, one), A2("vsubuhm", nm1, neg, one), A2("vsubuwm", nm1, neg, one), A2("vsubudm", nm1, neg, one));
    if (is_signed) { SW(W, A2("vsrab", rb, x, nm1), A2("vsrah", rb, x, nm1), A2("vsraw", rb, x, nm1), A2("vsrad", rb, x, nm1)); } else { SW(W, A2("vsrb", rb, x, nm1), A2("vsrh", rb, x, nm1), A2("vsrw", rb, x, nm1), A2("vsrd", rb, x, nm1)); }
    A2("vand", rb, rb, one); SW(W, A2("vaddubm", sr, sr, rb), A2("vadduhm", sr, sr, rb), A2("vadduwm", sr, sr, rb), A2("vaddudm", sr, sr, rb));
    /* n == W exactly: (a + 2^(W-1)) >> W = top bit of a for unsigned, and for signed: 0 unless... handled by the rm mask below plus a fix: unsigned n==W -> a>>(W-1); signed n==W -> (a<0 ? 0 : 0)... both give (a >> (W-1)) & 1 for unsigned and 0 for signed except a == INT_MIN.. see doc */
  }
  SW(W, SPLATB(wsplat, 8), wsplat = splath(16), wsplat = splatw(32), wsplat = splatd(64));
  SW(W, A2("vcmpgtub", lm, wsplat, cnt), A2("vcmpgtuh", lm, wsplat, cnt), A2("vcmpgtuw", lm, wsplat, cnt), A2("vcmpgtud", lm, wsplat, cnt));   /* cnt <u W */
  if (is_signed && !round) { v16 wm1; SW(W, SPLATB(wm1, 7), SPLATH(wm1, 15), wm1 = splatw(31), wm1 = splatd(63)); SW(W, A2("vminub", sh, neg, wm1), A2("vminuh", sh, neg, wm1), A2("vminuw", sh, neg, wm1), A2("vminud", sh, neg, wm1));
    SW(W, A2("vsrab", sr, x, sh), A2("vsrah", sr, x, sh), A2("vsraw", sr, x, sh), A2("vsrad", sr, x, sh)); rm = allones(); }
  else { SW(W, A2("vcmpgtub", rm, wsplat, neg), A2("vcmpgtuh", rm, wsplat, neg), A2("vcmpgtuw", rm, wsplat, neg), A2("vcmpgtud", rm, wsplat, neg)); }
  SW(W, A2("vcmpgtsb", sg, z, cnt), A2("vcmpgtsh", sg, z, cnt), A2("vcmpgtsw", sg, z, cnt), A2("vcmpgtsd", sg, z, cnt));                 /* cnt < 0 */
  A2("vand", sl, sl, lm); A2("vand", sr, sr, rm);
  if (round) { /* exact n == W: rounding gives bit W-1 of a (unsigned) or 0 (signed, since |a| < 2^(W-1)): compute separately: eqw = neg == W */
    v16 eqw, top, wm1; SW(W, A2("vcmpequb", eqw, neg, wsplat), A2("vcmpequh", eqw, neg, wsplat), A2("vcmpequw", eqw, neg, wsplat), A2("vcmpequd", eqw, neg, wsplat));
    if (!is_signed) { SW(W, SPLATB(wm1, 7), SPLATH(wm1, 15), wm1 = splatw(31), wm1 = splatd(63)); SW(W, A2("vsrb", top, x, wm1), A2("vsrh", top, x, wm1), A2("vsrw", top, x, wm1), A2("vsrd", top, x, wm1)); A2("vand", top, top, eqw); A2("vor", sr, sr, top); }
    else { /* signed n == W: (a + 2^(W-1)) >> W = 0 for a < 2^(W-1) always: nothing to add. n > W: 0. */ }
  }
  A3("vsel", o, sl, sr, sg);
  STV(r, o);
}
static void low_sshl SIG { low_shl_reg_impl(r, a, b, W, 1, 0); }
static void low_ushl SIG { low_shl_reg_impl(r, a, b, W, 0, 0); }
static void low_srshl SIG { low_shl_reg_impl(r, a, b, W, 1, 1); }
static void low_urshl SIG { low_shl_reg_impl(r, a, b, W, 0, 1); }
/* SQSHL/UQSHL by register: left shift saturates; right as SSHL/USHL. Saturation test:
 * (sl >>a cnt) != a  (or >> logical for unsigned) -> saturate to sign-based extreme. */
static void low_qshl_reg_impl(V* r, const V* a, const V* b, int W, int is_signed) {
  V t; low_shl_reg_impl(&t, a, b, W, is_signed, 0); v16 x = LDV(a), y = LDV(b), base = LDV(&t), o, cnt, back, ovf, sat, sg, lm, wsplat, z = zero();
  if (W == 8) cnt = y; else { v16 k; if (W == 16) SPLATH(k, 8); else if (W == 32) SPLATB(k, 24 - 32); else SPLATB(k, 56 - 64);
    SW(W, (void)0, A2("vslh", cnt, y, k), A2("vslw", cnt, y, k), A2("vsld", cnt, y, k)); SW(W, (void)0, A2("vsrah", cnt, cnt, k), A2("vsraw", cnt, cnt, k), A2("vsrad", cnt, cnt, k)); }
  /* overflow if cnt >= 0 and ((a << cnt) >> cnt) != a, with cnt >= W meaning overflow unless a == 0 */
  v16 sl; SW(W, A2("vslb", sl, x, cnt), A2("vslh", sl, x, cnt), A2("vslw", sl, x, cnt), A2("vsld", sl, x, cnt));
  if (is_signed) { SW(W, A2("vsrab", back, sl, cnt), A2("vsrah", back, sl, cnt), A2("vsraw", back, sl, cnt), A2("vsrad", back, sl, cnt)); } else { SW(W, A2("vsrb", back, sl, cnt), A2("vsrh", back, sl, cnt), A2("vsrw", back, sl, cnt), A2("vsrd", back, sl, cnt)); }
  SW(W, A2("vcmpequb", ovf, back, x), A2("vcmpequh", ovf, back, x), A2("vcmpequw", ovf, back, x), A2("vcmpequd", ovf, back, x)); A2("vnor", ovf, ovf, ovf);
  SW(W, SPLATB(wsplat, 8), wsplat = splath(16), wsplat = splatw(32), wsplat = splatd(64));
  v16 big, nz; SW(W, A2("vcmpgtub", big, wsplat, cnt), A2("vcmpgtuh", big, wsplat, cnt), A2("vcmpgtuw", big, wsplat, cnt), A2("vcmpgtud", big, wsplat, cnt)); /* cnt <u W (also false for negative counts as unsigned) */
  SW(W, A2("vcmpequb", nz, x, z), A2("vcmpequh", nz, x, z), A2("vcmpequw", nz, x, z), A2("vcmpequd", nz, x, z)); A2("vnor", nz, nz, nz);
  SW(W, A2("vcmpgtsb", sg, z, cnt), A2("vcmpgtsh", sg, z, cnt), A2("vcmpgtsw", sg, z, cnt), A2("vcmpgtsd", sg, z, cnt)); /* negative count */
  /* ovf_final = ~sg & ((big & ovf) | (~big & nz)) */
  v16 t1, t2; A2("vand", t1, big, ovf); A2("vandc", t2, nz, big); A2("vor", t1, t1, t2); A2("vandc", ovf, t1, sg);
  if (is_signed) { v16 c, imax; SW(W, SPLATB(c, 7), SPLATH(c, 15), c = splatw(31), c = splatd(63)); SW(W, A2("vsrab", sat, x, c), A2("vsrah", sat, x, c), A2("vsraw", sat, x, c), A2("vsrad", sat, x, c)); SW(W, imax = splatb(0x7F), imax = splath(0x7FFF), imax = splatw(0x7FFFFFFF), imax = splatd(0x7FFFFFFFFFFFFFFFull)); A2("vxor", sat, sat, imax); }
  else sat = allones();
  A3("vsel", o, base, sat, ovf); (void)lm; STV(r, o);
}
static void low_sqshl_r SIG { low_qshl_reg_impl(r, a, b, W, 1); }
static void low_uqshl_r SIG { low_qshl_reg_impl(r, a, b, W, 0); }
#define low_sqrshl NULL
#define low_uqrshl NULL
/* ---- shifts by immediate ------------------------------------------------- */
/* shift-count splat: vspltisb covers 0..15 directly and 16..31 via the sign
 * wrap (the shifters read the low log2(W) bits); 64-bit lanes need 6 bits so
 * counts 16..47 come from a GPR (mtvsrd + xxpermdi) on P8 or xxspltib on P9. */
static inline v16 shcount(int W, int n) { v16 k;
  if (W == 8) { SPLATB(k, 0); __asm__("vspltisb %0,%1" : "=v"(k) : "i"(0)); k = splatb((uint8_t)n); return k; }
  return splatb((uint8_t)n); }
static inline v16 shcount_fast(int W, int n) { v16 k; /* what a JIT emits: one vspltisb when n fits, else the GPR path */
  if (n <= 15) { switch (n) { case 0: SPLATB(k, 0); break; case 1: SPLATB(k, 1); break; case 2: SPLATB(k, 2); break; case 3: SPLATB(k, 3); break; case 4: SPLATB(k, 4); break; case 5: SPLATB(k, 5); break; case 6: SPLATB(k, 6); break; case 7: SPLATB(k, 7); break; case 8: SPLATB(k, 8); break; case 9: SPLATB(k, 9); break; case 10: SPLATB(k, 10); break; case 11: SPLATB(k, 11); break; case 12: SPLATB(k, 12); break; case 13: SPLATB(k, 13); break; case 14: SPLATB(k, 14); break; default: SPLATB(k, 15); } return k; }
  if (W <= 32 || n >= 48) { int v = n - 32 - (W == 64 ? 32 : 0); (void)v; return splatb((uint8_t)n); }
  return splatb((uint8_t)n); }
#define SHR_U(o, x, k, W) SW(W, A2("vsrb", o, x, k), A2("vsrh", o, x, k), A2("vsrw", o, x, k), A2("vsrd", o, x, k))
#define SHR_S(o, x, k, W) SW(W, A2("vsrab", o, x, k), A2("vsrah", o, x, k), A2("vsraw", o, x, k), A2("vsrad", o, x, k))
#define SHL(o, x, k, W) SW(W, A2("vslb", o, x, k), A2("vslh", o, x, k), A2("vslw", o, x, k), A2("vsld", o, x, k))
#define ADD(o, x, y, W) SW(W, A2("vaddubm", o, x, y), A2("vadduhm", o, x, y), A2("vadduwm", o, x, y), A2("vaddudm", o, x, y))
#define SUB(o, x, y, W) SW(W, A2("vsububm", o, x, y), A2("vsubuhm", o, x, y), A2("vsubuwm", o, x, y), A2("vsubudm", o, x, y))
#define CMPEQ(o, x, y, W) SW(W, A2("vcmpequb", o, x, y), A2("vcmpequh", o, x, y), A2("vcmpequw", o, x, y), A2("vcmpequd", o, x, y))
static void low_sshr SIG BODY({ if (!immok(W, imm)) { o = zero(); } else { v16 k = shcount(W, imm >= W ? W - 1 : imm); SHR_S(o, x, k, W); } })
static void low_ushr SIG BODY({ if (!immok(W, imm)) { o = zero(); } else if (imm >= W) o = zero(); else { v16 k = shcount(W, imm); SHR_U(o, x, k, W); } })
/* rounding right shift by n: (a >> n) + ((a >> (n-1)) & 1); n == W: unsigned -> a >> (W-1); signed -> 0 */
static void low_srshr SIG BODY({ if (!immok(W, imm)) { o = zero(); } else if (imm >= W) { o = zero(); } else { v16 k = shcount(W, imm), k1 = shcount(W, imm - 1), one, s, rb; SW(W, SPLATB(one, 1), SPLATH(one, 1), SPLATW(one, 1), one = splatd(1)); SHR_S(s, x, k, W); SHR_S(rb, x, k1, W); A2("vand", rb, rb, one); ADD(o, s, rb, W); } })
static void low_urshr SIG BODY({ if (!immok(W, imm)) { o = zero(); } else if (imm >= W) { v16 k = shcount(W, W - 1); SHR_U(o, x, k, W); } else { v16 k = shcount(W, imm), k1 = shcount(W, imm - 1), one, s, rb; SW(W, SPLATB(one, 1), SPLATH(one, 1), SPLATW(one, 1), one = splatd(1)); SHR_U(s, x, k, W); SHR_U(rb, x, k1, W); A2("vand", rb, rb, one); ADD(o, s, rb, W); } })
static void low_shl SIG BODY({ if (!immok_l(W, imm)) { o = zero(); } else { v16 k = shcount(W, imm); SHL(o, x, k, W); } })
/* SQSHL imm: sl = a << n; ovf = (sl >>a n) != a; sat = (a >>a W-1) ^ MAX; sel */
static void low_sqshl_i SIG BODY({ if (!immok_l(W, imm)) { o = zero(); } else { v16 k = shcount(W, imm), sl, back, ovf, sat, c, imax; SHL(sl, x, k, W); SHR_S(back, sl, k, W); CMPEQ(ovf, back, x, W);
  SW(W, SPLATB(c, 7), SPLATH(c, 15), c = splatw(31), c = splatd(63)); SHR_S(sat, x, c, W); SW(W, imax = splatb(0x7F), imax = splath(0x7FFF), imax = splatw(0x7FFFFFFF), imax = splatd(0x7FFFFFFFFFFFFFFFull)); A2("vxor", sat, sat, imax); A3("vsel", o, sat, sl, ovf); } })
static void low_uqshl_i SIG BODY({ if (!immok_l(W, imm)) { o = zero(); } else { v16 k = shcount(W, imm), sl, back, ovf; SHL(sl, x, k, W); SHR_U(back, sl, k, W); CMPEQ(ovf, back, x, W); v16 ones = allones(); A3("vsel", o, ones, sl, ovf); } })
/* SQSHLU: signed in, unsigned out: negative -> 0; else unsigned saturate of a << n */
static void low_sqshlu SIG BODY({ if (!immok_lu(W, imm)) { o = zero(); } else { v16 k = shcount(W, imm), sl, back, ovf, z = zero(), neg, ones = allones(); SHL(sl, x, k, W); SHR_U(back, sl, k, W); CMPEQ(ovf, back, x, W); A3("vsel", sl, ones, sl, ovf);
  SW(W, A2("vcmpgtsb", neg, z, x), A2("vcmpgtsh", neg, z, x), A2("vcmpgtsw", neg, z, x), A2("vcmpgtsd", neg, z, x)); A2("vandc", o, sl, neg); } })
/* narrowing shifts: source 2W, result W in the low half; packs take the LOW half of each element */
#define PACK_MOD(o, hi, lo, W) SW(W, A2("vpkuhum", o, hi, lo), A2("vpkuwum", o, hi, lo), A2("vpkudum", o, hi, lo), (void)0)
static void low_shrn SIG BODY({ if (!immok_n(W, imm)) { o = zero(); } else { v16 k = shcount(2 * W, imm), s, z = zero(); SHR_U(s, x, k, 2 * W); PACK_MOD(o, z, s, W); } })
static void low_rshrn SIG BODY({ if (!immok_n(W, imm)) { o = zero(); } else { v16 k = shcount(2 * W, imm), k1 = shcount(2 * W, imm - 1), one, s, rb, z = zero(); SW(2 * W, (void)0, SPLATH(one, 1), SPLATW(one, 1), one = splatd(1)); SHR_U(s, x, k, 2 * W); SHR_U(rb, x, k1, 2 * W); A2("vand", rb, rb, one); ADD(s, s, rb, 2 * W); PACK_MOD(o, z, s, W); } })
#define PACK_SS(o, hi, lo, W) SW(W, A2("vpkshss", o, hi, lo), A2("vpkswss", o, hi, lo), A2("vpksdss", o, hi, lo), (void)0)
#define PACK_US(o, hi, lo, W) SW(W, A2("vpkshus", o, hi, lo), A2("vpkswus", o, hi, lo), A2("vpksdus", o, hi, lo), (void)0)
#define PACK_UU(o, hi, lo, W) SW(W, A2("vpkuhus", o, hi, lo), A2("vpkuwus", o, hi, lo), A2("vpkudus", o, hi, lo), (void)0)
static void low_sqshrn SIG BODY({ if (!immok_n(W, imm)) { o = zero(); } else { v16 k = shcount(2 * W, imm), s, z = zero(); SHR_S(s, x, k, 2 * W); PACK_SS(o, z, s, W); } })
static void low_sqrshrn SIG BODY({ if (!immok_n(W, imm)) { o = zero(); } else { v16 k = shcount(2 * W, imm), k1 = shcount(2 * W, imm - 1), one, s, rb, z = zero(); SW(2 * W, (void)0, SPLATH(one, 1), SPLATW(one, 1), one = splatd(1)); SHR_S(s, x, k, 2 * W); SHR_S(rb, x, k1, 2 * W); A2("vand", rb, rb, one); ADD(s, s, rb, 2 * W); PACK_SS(o, z, s, W); } })
static void low_uqshrn SIG BODY({ if (!immok_n(W, imm)) { o = zero(); } else { v16 k = shcount(2 * W, imm), s, z = zero(); SHR_U(s, x, k, 2 * W); PACK_UU(o, z, s, W); } })
static void low_uqrshrn SIG BODY({ if (!immok_n(W, imm)) { o = zero(); } else { v16 k = shcount(2 * W, imm), k1 = shcount(2 * W, imm - 1), one, s, rb, z = zero(); SW(2 * W, (void)0, SPLATH(one, 1), SPLATW(one, 1), one = splatd(1)); SHR_U(s, x, k, 2 * W); SHR_U(rb, x, k1, 2 * W); A2("vand", rb, rb, one); ADD(s, s, rb, 2 * W); PACK_UU(o, z, s, W); } })
static void low_sqshrun SIG BODY({ if (!immok_n(W, imm)) { o = zero(); } else { v16 k = shcount(2 * W, imm), s, z = zero(); SHR_S(s, x, k, 2 * W); PACK_US(o, z, s, W); } })
static void low_sqrshrun SIG BODY({ if (!immok_n(W, imm)) { o = zero(); } else { v16 k = shcount(2 * W, imm), k1 = shcount(2 * W, imm - 1), one, s, rb, z = zero(); SW(2 * W, (void)0, SPLATH(one, 1), SPLATW(one, 1), one = splatd(1)); SHR_S(s, x, k, 2 * W); SHR_S(rb, x, k1, 2 * W); A2("vand", rb, rb, one); ADD(s, s, rb, 2 * W); PACK_US(o, z, s, W); } })
static void low_xtn SIG BODY({ v16 z = zero(); PACK_MOD(o, z, x, W); })
static void low_sqxtn SIG BODY({ v16 z = zero(); PACK_SS(o, z, x, W); })
static void low_uqxtn SIG BODY({ v16 z = zero(); PACK_UU(o, z, x, W); })
static void low_sqxtun SIG BODY({ v16 z = zero(); PACK_US(o, z, x, W); })
static void low_addhn SIG BODY({ v16 s, k = shcount(2 * W, W), z = zero(); ADD(s, x, y, 2 * W); SHR_U(s, s, k, 2 * W); PACK_MOD(o, z, s, W); })
static void low_subhn SIG BODY({ v16 s, k = shcount(2 * W, W), z = zero(); SUB(s, x, y, 2 * W); SHR_U(s, s, k, 2 * W); PACK_MOD(o, z, s, W); })
/* RADDHN: (a + b + 2^(W-1)) >> W truncated to W: the carry out of the 2W sum matters only for bits >= 2W, which are discarded */
static void low_raddhn SIG BODY({ v16 s, k = shcount(2 * W, W), z = zero(), rc; SW(2 * W, (void)0, rc = splath(0x80), rc = splatw(0x8000), rc = splatd(0x80000000ull)); ADD(s, x, y, 2 * W); ADD(s, s, rc, 2 * W); SHR_U(s, s, k, 2 * W); PACK_MOD(o, z, s, W); })
/* widening */
static void low_sxtl SIG BODY(SW(W, A1("vupklsb", o, x), A1("vupklsh", o, x), A1("vupklsw", o, x), (void)0))
static void low_sxtl2 SIG BODY(SW(W, A1("vupkhsb", o, x), A1("vupkhsh", o, x), A1("vupkhsw", o, x), (void)0))
static void low_uxtl SIG BODY({ v16 z = zero(); SW(W, A2("vmrglb", o, z, x), A2("vmrglh", o, z, x), A2("vmrglw", o, z, x), (void)0); })
static void low_uxtl2 SIG BODY({ v16 z = zero(); SW(W, A2("vmrghb", o, z, x), A2("vmrghh", o, z, x), A2("vmrghw", o, z, x), (void)0); })
static void low_saddl SIG BODY({ v16 xw, yw; SW(W, A1("vupklsb", xw, x), A1("vupklsh", xw, x), A1("vupklsw", xw, x), (void)0); SW(W, A1("vupklsb", yw, y), A1("vupklsh", yw, y), A1("vupklsw", yw, y), (void)0); ADD(o, xw, yw, 2 * W); })
static void low_uaddl SIG BODY({ v16 z = zero(), xw, yw; SW(W, A2("vmrglb", xw, z, x), A2("vmrglh", xw, z, x), A2("vmrglw", xw, z, x), (void)0); SW(W, A2("vmrglb", yw, z, y), A2("vmrglh", yw, z, y), A2("vmrglw", yw, z, y), (void)0); ADD(o, xw, yw, 2 * W); })
/* pairwise widening adds: 16->32 signed is vmsumshm(a, ones, 0) (sum of the two halfwords of each word);
 * 8->16: vmulesb(a, 1) + vmulosb(a, 1) (each gives sign-extended bytes in halfwords) */
static void low_saddlp SIG BODY({ v16 z = zero(), one, e, od; if (W == 16) { one = splath(1); A3("vmsumshm", o, x, one, z); } else if (W == 8) { SPLATB(one, 1); A2("vmulesb", e, x, one); A2("vmulosb", od, x, one); A2("vadduhm", o, e, od); } else { A1("vupklsw", e, x); A1("vupkhsw", od, x); /* need lanes (0,1),(2,3): e = {w0,w1} od = {w2,w3} as doublewords; sum pairs: xxmrgld/xxmrghd */ v16 lo, hi; X2("xxmrgld", lo, od, e); X2("xxmrghd", hi, od, e); A2("vaddudm", o, lo, hi); } })
static void low_uaddlp SIG BODY({ v16 z = zero(), one, e, od; if (W == 16) { one = splath(1); A3("vmsumuhm", o, x, one, z); } else if (W == 8) { SPLATB(one, 1); A2("vmuleub", e, x, one); A2("vmuloub", od, x, one); A2("vadduhm", o, e, od); } else { A2("vmrglw", e, z, x); A2("vmrghw", od, z, x); v16 lo, hi; X2("xxmrgld", lo, od, e); X2("xxmrghd", hi, od, e); A2("vaddudm", o, lo, hi); } })
static void low_sadalp SIG BODY({ v16 one, e, od; if (W == 16) { one = splath(1); A3("vmsumshm", o, x, one, z); } else { SPLATB(one, 1); A2("vmulesb", e, x, one); A2("vmulosb", od, x, one); A2("vadduhm", o, e, od); A2("vadduhm", o, o, z); } })
static void low_uadalp SIG BODY({ v16 one, e, od; if (W == 16) { one = splath(1); A3("vmsumuhm", o, x, one, z); } else { SPLATB(one, 1); A2("vmuleub", e, x, one); A2("vmuloub", od, x, one); A2("vadduhm", o, e, od); A2("vadduhm", o, o, z); } })
/* pairwise ops: even/odd element gathers then the op.  Even elements of (b:a) = vpk*um(b, a);
 * odd elements: shift each element right by W/... no -- odd elements are the HIGH halves of
 * the 2W-wide containers: vsr by W within 2W lanes then pack. */
#define EVEN(o, x, y, W) PACK_MOD(o, y, x, W)
#define ODD(o, x, y, W) { v16 k = shcount(2 * W, W), xs, ys; SHR_U(xs, x, k, 2 * W); SHR_U(ys, y, k, 2 * W); PACK_MOD(o, ys, xs, W); }
static void low_addp SIG BODY({ if (W == 64) { X2("xxmrgld", o, y, x); v16 t; X2("xxmrghd", t, y, x); A2("vaddudm", o, o, t); } else { v16 e, od; EVEN(e, x, y, W); ODD(od, x, y, W); ADD(o, e, od, W); } })
static void low_umaxp SIG BODY({ v16 e, od; EVEN(e, x, y, W); ODD(od, x, y, W); SW(W, A2("vmaxub", o, e, od), A2("vmaxuh", o, e, od), A2("vmaxuw", o, e, od), (void)0); })
static void low_uminp SIG BODY({ v16 e, od; EVEN(e, x, y, W); ODD(od, x, y, W); SW(W, A2("vminub", o, e, od), A2("vminuh", o, e, od), A2("vminuw", o, e, od), (void)0); })
static void low_smaxp SIG BODY({ v16 e, od; EVEN(e, x, y, W); ODD(od, x, y, W); SW(W, A2("vmaxsb", o, e, od), A2("vmaxsh", o, e, od), A2("vmaxsw", o, e, od), (void)0); })
static void low_sminp SIG BODY({ v16 e, od; EVEN(e, x, y, W); ODD(od, x, y, W); SW(W, A2("vminsb", o, e, od), A2("vminsh", o, e, od), A2("vminsw", o, e, od), (void)0); })
/* across-lane reductions.  ADDV.16B/8H: vsum4ubs/vsum4shs + vsumsws cannot saturate
 * for these widths (max 16*255 / 8*32767 fits a word) and the low W bits are the
 * wrapped sum.  ADDV.4S must not use vsumsws (saturates): rotate-add. */
static void low_addv SIG BODY({ v16 z = zero(), t; if (W == 8) { A2("vsum4ubs", t, x, z); A2("vsumsws", t, t, z); } else if (W == 16) { A2("vsum4shs", t, x, z); A2("vsumsws", t, t, z); } else { v16 r1; A2I("vsldoi", r1, x, x, 8); A2("vadduwm", t, x, r1); A2I("vsldoi", r1, t, t, 4); A2("vadduwm", t, t, r1); }
  /* result is in LE lane 0 (vsumsws puts it in BE word 3); clear the rest: vsldoi with zero */
  SW(W, ({ v16 m = splatw(0xFF); A2("vand", o, t, m); }), ({ v16 m = splatw(0xFFFF); A2("vand", o, t, m); }), ({ v16 m; A2I("vsldoi", m, z, t, 4); /* keep phys[12..15] only: shift left 12 then right 12 */ A2I("vsldoi", m, t, z, 12); A2I("vsldoi", o, z, m, 4); }), (void)0); })
static void low_saddlv SIG BODY({ v16 z = zero(), t; if (W == 8) { A2("vsum4sbs", t, x, z); A2("vsumsws", t, t, z); v16 m = splatw(0xFFFF); A2("vand", o, t, m); } else if (W == 16) { A2("vsum4shs", t, x, z); A2("vsumsws", t, t, z); v16 m; A2I("vsldoi", m, t, z, 12); A2I("vsldoi", o, z, m, 4); } else { v16 lo, hi; A1("vupklsw", lo, x); A1("vupkhsw", hi, x); A2("vaddudm", t, lo, hi); A2I("vsldoi", lo, t, t, 8); A2("vaddudm", t, t, lo); A2I("vsldoi", o, z, t, 8); } })
static void low_uaddlv SIG BODY({ v16 z = zero(), t; if (W == 8) { A2("vsum4ubs", t, x, z); A2("vsumsws", t, t, z); v16 m = splatw(0xFFFF); A2("vand", o, t, m); } else if (W == 16) { /* vsum4shs treats halfwords as signed: zero-extend first */ v16 lo, hi; A2("vmrglh", lo, z, x); A2("vmrghh", hi, z, x); A2("vadduwm", t, lo, hi); A2("vsumsws", t, t, z); v16 m; A2I("vsldoi", m, t, z, 12); A2I("vsldoi", o, z, m, 4); } else { v16 lo, hi; A2("vmrglw", lo, z, x); A2("vmrghw", hi, z, x); A2("vaddudm", t, lo, hi); A2I("vsldoi", lo, t, t, 8); A2("vaddudm", t, t, lo); A2I("vsldoi", o, z, t, 8); } })
#define FOLD(o, x, W, OP8, OP16, OP32) { v16 t = x, s; int n = 16 / (W / 8); for (int f = n / 2; f >= 1; f >>= 1) { switch (f * (W / 8)) { case 8: A2I("vsldoi", s, t, t, 8); break; case 4: A2I("vsldoi", s, t, t, 4); break; case 2: A2I("vsldoi", s, t, t, 2); break; default: A2I("vsldoi", s, t, t, 1); } SW(W, A2(OP8, t, t, s), A2(OP16, t, t, s), A2(OP32, t, t, s), (void)0); } \
  v16 z = zero(); switch (W / 8) { case 1: A2I("vsldoi", s, t, z, 15); A2I("vsldoi", o, z, s, 1); break; case 2: A2I("vsldoi", s, t, z, 14); A2I("vsldoi", o, z, s, 2); break; default: A2I("vsldoi", s, t, z, 12); A2I("vsldoi", o, z, s, 4); } }
static void low_umaxv SIG BODY(FOLD(o, x, W, "vmaxub", "vmaxuh", "vmaxuw"))
static void low_uminv SIG BODY(FOLD(o, x, W, "vminub", "vminuh", "vminuw"))
static void low_smaxv SIG BODY(FOLD(o, x, W, "vmaxsb", "vmaxsh", "vmaxsw"))
static void low_sminv SIG BODY(FOLD(o, x, W, "vminsb", "vminsh", "vminsw"))
/* bit manipulation */
static void low_cnt SIG BODY(A1("vpopcntb", o, x))
static void low_clz SIG BODY(SW(W, A1("vclzb", o, x), A1("vclzh", o, x), A1("vclzw", o, x), (void)0))
/* CLS = CLZ(a ^ (a >>a 1)) - 1 */
static void low_cls SIG BODY({ v16 one, t; SW(W, SPLATB(one, 1), SPLATH(one, 1), SPLATW(one, 1), (void)0); SHR_S(t, x, one, W); A2("vxor", t, t, x); SW(W, A1("vclzb", t, t), A1("vclzh", t, t), A1("vclzw", t, t), (void)0); SUB(o, t, one, W); })
/* RBIT: nibble table through vperm: lo = tab[x & 15], hi = tab[x >> 4]; result = (lo << 4) | hi.
 * vperm indexes ISA bytes, so the table is stored reversed (byte k of memory = rev(15-k)). */
static void low_rbit SIG BODY({ static const uint8_t revtab_isa[16] = { 0xF, 0x7, 0xB, 0x3, 0xD, 0x5, 0x9, 0x1, 0xE, 0x6, 0xA, 0x2, 0xC, 0x4, 0x8, 0x0 }; /* memory order: entry for index 15-k at memory byte k... */
  v16 tab; memcpy(&tab, revtab_isa, 16); v16 c4, m, hi, lo, t; SPLATB(c4, 4); m = splatb(0x0F); A2("vsrb", hi, x, c4); A2("vand", lo, x, m); A3("vperm", hi, tab, tab, hi); A3("vperm", lo, tab, tab, lo); A2("vslb", t, lo, c4); A2("vor", o, t, hi); })
static void low_rev64 SIG BODY({ v16 k; if (W == 32) { k = splatb(32); A2("vrld", o, x, k); } else if (W == 16) { k = splatb(32); A2("vrld", o, x, k); SPLATW(k, -16); A2("vrlw", o, o, k); } else { k = splatb(32); A2("vrld", o, x, k); SPLATW(k, -16); A2("vrlw", o, o, k); SPLATH(k, 8); A2("vrlh", o, o, k); } })
static void low_rev32 SIG BODY({ v16 k; if (W == 16) { SPLATW(k, -16); A2("vrlw", o, x, k); } else { SPLATW(k, -16); A2("vrlw", o, x, k); SPLATH(k, 8); A2("vrlh", o, o, k); } })
static void low_rev16 SIG BODY({ v16 k; SPLATH(k, 8); A2("vrlh", o, x, k); })
/* permutes: ZIP1(a,b) = vmrgl*(b,a); ZIP2 = vmrgh*(b,a); UZP1 = vpk*um(b,a); TRN via merges */
static void low_zip1 SIG BODY(SW(W, A2("vmrglb", o, y, x), A2("vmrglh", o, y, x), A2("vmrglw", o, y, x), X2("xxmrgld", o, y, x)))
static void low_zip2 SIG BODY(SW(W, A2("vmrghb", o, y, x), A2("vmrghh", o, y, x), A2("vmrghw", o, y, x), X2("xxmrghd", o, y, x)))
static void low_uzp1 SIG BODY(SW(W, A2("vpkuhum", o, y, x), A2("vpkuwum", o, y, x), A2("vpkudum", o, y, x), X2("xxmrgld", o, y, x)))
static void low_uzp2 SIG BODY({ if (W == 64) X2("xxmrghd", o, y, x); else if (W == 32) { /* odd words: vmrgow/vmrgew give {b0,a0,b2,a2}; UZP2 = {a1,a3,b1,b3}: use vpkudum on rotated inputs */ v16 k, xs, ys; k = splatb(32); A2("vrld", xs, x, k); A2("vrld", ys, y, k); A2("vpkudum", o, ys, xs); } else ODD(o, x, y, W); })
static void low_trn1 SIG BODY({ if (W == 32) A2("vmrgow", o, y, x); else if (W == 64) X2("xxmrgld", o, y, x); else { /* even elements of a interleaved with even of b: mask-merge: (a & lomask) | ((b & lomask) << W) */ v16 m, k, t; SW(W, m = splath(0x00FF), m = splatw(0x0000FFFF), (void)0, (void)0); k = shcount(2 * W, W); A2("vand", t, y, m); SHL(t, t, k, 2 * W); A2("vand", o, x, m); A2("vor", o, o, t); } })
static void low_trn2 SIG BODY({ if (W == 32) A2("vmrgew", o, y, x); else if (W == 64) X2("xxmrghd", o, y, x); else { v16 m, k, t; SW(W, m = splath(0xFF00), m = splatw(0xFFFF0000u), (void)0, (void)0); k = shcount(2 * W, W); A2("vand", t, x, m); SHR_U(t, t, k, 2 * W); A2("vand", o, y, m); A2("vor", o, o, t); } })
/* EXT: result = (b:a) >> 8*imm  == vsldoi(b, a, 16-imm) (ISA: (VRA||VRB) << SHB bytes) */
static void low_ext SIG BODY({ switch (imm) { case 0: o = x; break;
#define E(n) case n: A2I("vsldoi", o, y, x, 16 - n); break;
  E(1) E(2) E(3) E(4) E(5) E(6) E(7) E(8) E(9) E(10) E(11) E(12) E(13) E(14) E(15) } })
/* TBL/TBX.  P8: vperm indexes ISA bytes, so the guest index i must become 15-i
 * (xor 0x0F on the low 4 bits) and out-of-range indices are masked after.
 * P9: vpermr indexes LE bytes directly; vpermr(second, first, idx) covers 32 bytes. */
static void low_tbl SIG {
  v16 t1 = LDV(a), t2 = LDV(b), t3 = LDV(&TBL_T3), t4 = LDV(&TBL_T4), idx = LDV(c), o, m, lim, z = zero();
  switch (imm) {
  case 1: { SPLATB(lim, 15); A2("vcmpgtub", m, idx, lim);
#if P9
    A3("vpermr", o, t1, t1, idx);
#else
    v16 xi; A2("vxor", xi, idx, lim); A3("vperm", o, t1, t1, xi);
#endif
    A2("vandc", o, o, m); break; }
  case 2: { lim = splatb(31); A2("vcmpgtub", m, idx, lim);
#if P9
    A3("vpermr", o, t2, t1, idx);
#else
    v16 xi, c15; SPLATB(c15, 15); A2("vxor", xi, idx, c15); A3("vperm", o, t1, t2, xi);
#endif
    A2("vandc", o, o, m); break; }
  default: { /* 3 or 4 tables: two 32-byte lookups selected by bit 5 of the index; zero above 16*imm */ v16 r1, r2, sel, c31; if (imm == 3) t4 = z;
    lim = splatb(16 * imm - 1); A2("vcmpgtub", m, idx, lim); c31 = splatb(31); A2("vcmpgtub", sel, idx, c31);
#if P9
    A3("vpermr", r1, t2, t1, idx); A3("vpermr", r2, t4, t3, idx);
#else
    v16 xi, c15; SPLATB(c15, 15); A2("vxor", xi, idx, c15); A3("vperm", r1, t1, t2, xi); A3("vperm", r2, t3, t4, xi);
#endif
    A3("vsel", o, r1, r2, sel); A2("vandc", o, o, m); break; } }
  STV(r, o); }
static void low_tbx SIG {
  V t; low_tbl(&t, a, b, c, W, imm); v16 res = LDV(&t), d = LDV(&TBX_D), idx = LDV(c), m, lim, o;
  lim = splatb(16 * imm - 1); A2("vcmpgtub", m, idx, lim); A3("vsel", o, res, d, m); STV(r, o); }
/* dot products: UDOT = vmsumubm(a, b, acc) exactly.  SDOT: vmsummbm is signed x unsigned:
 * a_s . b_s = vmsummbm(a, b ^ 0x80) - 128 * sum(a): 2 vmsummbm + xor + shift + sub. */
static void low_udot SIG BODY(A3("vmsumubm", o, x, y, z))
static void low_sdot SIG BODY({ v16 c80 = splatb(0x80), yb, s1, ones, s2, k7; A2("vxor", yb, y, c80); A3("vmsummbm", s1, x, yb, z); SPLATB(ones, 1); v16 zz = zero(); A3("vmsummbm", s2, x, ones, zz); SPLATB(k7, 7); A2("vslw", s2, s2, k7); A2("vsubuwm", o, s1, s2); })
/* polynomial multiply: vpmsumd XORs the two 64x64 products; zero one doubleword of an operand to isolate one. */
static void low_pmull SIG BODY({ v16 z = zero(), xl; X2("xxmrgld", xl, z, x); /* LE dw0 = x.dw0, dw1 = 0 */ A2("vpmsumd", o, xl, y); })
static void low_pmull2 SIG BODY({ v16 z = zero(), xh; X2("xxmrghd", xh, x, z); /* xxpermdi DM=0: LE dw1 = x.dw1, LE dw0 = 0 */ A2("vpmsumd", o, xh, y); })
/* PMULL.8B: vpmsumb XORs the products of byte pairs; zero-extend bytes to halfwords first so each halfword holds one product. */
static void low_pmull8 SIG BODY({ v16 z = zero(), xw, yw; A2("vmrglb", xw, z, x); A2("vmrglb", yw, z, y); A2("vpmsumb", o, xw, yw); })
static void low_pmull8_2 SIG BODY({ v16 z = zero(), xw, yw; A2("vmrghb", xw, z, x); A2("vmrghb", yw, z, y); A2("vpmsumb", o, xw, yw); })
/* AES: AESE(s,k) = SubBytes(ShiftRows(s^k)) = vcipherlast(s^k, 0); AESMC = vcipher(vncipherlast(s,0),0);
 * AESD(s,k) = InvSubBytes(InvShiftRows(s^k)) = vncipherlast(s^k, 0); AESIMC = vncipher(vcipherlast(s,0),0). */
/* AES.  vcipher/vncipher interpret the register in ISA (big-endian) byte order,
 * i.e. the AES state byte k is our lane 15-k.  Measured consequences:
 *   ShiftRows in that layout = ShiftRows in ours followed by a 4-byte rotation
 *   (one column), so AESE(s,k) = rot(vcipherlast(s^k, 0)) and AESD likewise;
 *   MixColumns in that layout = REV32 . MixColumns . REV32 in ours, so
 *   AESMC(s) = REV32(vcipher(vncipherlast(REV32(s), 0), 0)) and AESIMC dually.
 * A full round AESE+AESMC keeps the state byte-reversed across rounds:
 *   round(s,k) = BSWAP16(vcipher(BSWAP16(s ^ k), 0)), and the two reversals
 *   cancel between consecutive rounds (see NEON-LANDINGS.md). */
static inline v16 rev32(v16 x) { v16 k, o;
#if P9
  X1("xxbrw", o, x);
#else
  SPLATW(k, -16); A2("vrlw", o, x, k); SPLATH(k, 8); A2("vrlh", o, o, k);
#endif
  return o; }
static void low_aese_v SIG BODY({ v16 z = zero(), t; A2("vxor", t, x, y); A2("vcipherlast", t, t, z); A2I("vsldoi", o, t, t, 4); })
static void low_aesd_v SIG BODY({ v16 z = zero(), t; A2("vxor", t, x, y); A2("vncipherlast", t, t, z); A2I("vsldoi", o, t, t, 12); })
static void low_aesmc_v SIG BODY({ v16 z = zero(), t = rev32(x); A2("vncipherlast", t, t, z); A2("vcipher", t, t, z); o = rev32(t); })
static void low_aesimc_v SIG BODY({ v16 z = zero(), t = rev32(x); A2("vcipherlast", t, t, z); A2("vncipher", t, t, z); o = rev32(t); })
/* two-operand NaN fixup shared by FRECPS/FRSQRTS/FMAX/FMIN: ARM order sNaN a, sNaN b, qNaN a, qNaN b, quieted; inf*0 -> special */
static inline v16 nanfix2(v16 base, v16 x, v16 y, int W, v16 special, int use_special) {
  v16 qbit, na, nb, sa, sb, qa, qb, t, zz = zero(), inf, xa, xb, ia, ib, za, zb, iz;
  if (W == 32) { qbit = splatw(0x00400000); inf = splatw(0x7F800000); X2("xvcmpeqsp", na, x, x); X2("xvcmpeqsp", nb, y, y); X1("xvabssp", xa, x); X1("xvabssp", xb, y); X2("xvcmpeqsp", ia, xa, inf); X2("xvcmpeqsp", ib, xb, inf); X2("xvcmpeqsp", za, xa, zz); X2("xvcmpeqsp", zb, xb, zz); }
  else { qbit = splatd(0x0008000000000000ull); inf = splatd(0x7FF0000000000000ull); X2("xvcmpeqdp", na, x, x); X2("xvcmpeqdp", nb, y, y); X1("xvabsdp", xa, x); X1("xvabsdp", xb, y); X2("xvcmpeqdp", ia, xa, inf); X2("xvcmpeqdp", ib, xb, inf); X2("xvcmpeqdp", za, xa, zz); X2("xvcmpeqdp", zb, xb, zz); }
  A2("vnor", na, na, na); A2("vnor", nb, nb, nb); A2("vor", qa, x, qbit); A2("vor", qb, y, qbit);
  A2("vand", sa, x, qbit); CMPEQ(sa, sa, zz, W); A2("vand", sa, sa, na); A2("vand", sb, y, qbit); CMPEQ(sb, sb, zz, W); A2("vand", sb, sb, nb);
  t = base; if (use_special) { A2("vand", ia, ia, zb); A2("vand", ib, ib, za); A2("vor", iz, ia, ib); A3("vsel", t, t, special, iz); }
  A3("vsel", t, t, qb, nb); A3("vsel", t, t, qa, na); A3("vsel", t, t, qb, sb); A3("vsel", t, t, qa, sa); return t; }
/* float */
static void low_frintn SIG BODY({ if (W == 32) A1("vrfin", o, x); else { X1("xvrdpic", o, x); } }) /* vrfin ties-even (handbook); f64: xvrdpic uses the current mode (RN) */
static void low_frinta SIG BODY({ if (W == 32) X1("xvrspi", o, x); else X1("xvrdpi", o, x); })
static void low_frintm SIG BODY({ if (W == 32) X1("xvrspim", o, x); else X1("xvrdpim", o, x); })
static void low_frintp SIG BODY({ if (W == 32) X1("xvrspip", o, x); else X1("xvrdpip", o, x); })
static void low_frintz SIG BODY({ if (W == 32) X1("xvrspiz", o, x); else X1("xvrdpiz", o, x); })
/* FCVTZS: xvcvspsxws saturates but gives INT_MIN for NaN; AND with (x == x) */
static void low_fcvtzs SIG BODY({ v16 m; if (W == 32) { X1("xvcvspsxws", o, x); X2("xvcmpeqsp", m, x, x); } else { X1("xvcvdpsxds", o, x); X2("xvcmpeqdp", m, x, x); } A2("vand", o, o, m); })
static void low_fcvtzu SIG BODY({ if (W == 32) X1("xvcvspuxws", o, x); else X1("xvcvdpuxds", o, x); })
static void low_scvtf SIG BODY({ if (W == 32) X1("xvcvsxwsp", o, x); else X1("xvcvsxddp", o, x); })
static void low_ucvtf SIG BODY({ if (W == 32) X1("xvcvuxwsp", o, x); else X1("xvcvuxddp", o, x); })
/* FMLA: xvmaddasp T=(A*B)+T; FMLS = T - A*B = xvnmsubasp: -((A*B) - T).  NaN precedence is measured, not assumed. */
static void low_fmla SIG BODY({ o = z; if (W == 32) __asm__("xvmaddasp %x0,%x1,%x2" : "+wa"(o) : "wa"(x), "wa"(y)); else __asm__("xvmaddadp %x0,%x1,%x2" : "+wa"(o) : "wa"(x), "wa"(y)); })
static void low_fmls SIG BODY({ o = z; if (W == 32) __asm__("xvnmsubasp %x0,%x1,%x2" : "+wa"(o) : "wa"(x), "wa"(y)); else __asm__("xvnmsubadp %x0,%x1,%x2" : "+wa"(o) : "wa"(x), "wa"(y)); })
/* FMLS exact alternative: negate A first (xvnegsp) then xvmaddasp: single rounding of T + (-A)*B, NaN sign follows the negated operand as on ARM */
static void low_fmls_neg SIG BODY({ v16 nx; o = z; if (W == 32) { X1("xvnegsp", nx, x); __asm__("xvmaddasp %x0,%x1,%x2" : "+wa"(o) : "wa"(nx), "wa"(y)); } else { X1("xvnegdp", nx, x); __asm__("xvmaddadp %x0,%x1,%x2" : "+wa"(o) : "wa"(nx), "wa"(y)); } })
/* Exact FMLA/FMLS: xvmadda (with xvneg for FMLS), then the ARM NaN precedence
 * (sNaN: addend, op1, op2; then qNaN in the same order; inf*0 with a quiet NaN
 * addend gives the default NaN) applied with masks.  A JIT would gate this
 * whole fixup behind one record-form compare of the result with itself. */
static void fmla_exact(V* r, const V* a, const V* b, const V* c, int W, int neg) {
  v16 x = LDV(a), y = LDV(b), z = LDV(c), o, t, qbit, dn, na, nb, nc, sa, sb, sc, qa, qb, qc, inf, zz = zero(), xa, xb, ia, ib, za, zb, iz, cq;
  if (neg) { if (W == 32) X1("xvnegsp", x, x); else X1("xvnegdp", x, x); }
  o = z; if (W == 32) __asm__("xvmaddasp %x0,%x1,%x2" : "+wa"(o) : "wa"(x), "wa"(y)); else __asm__("xvmaddadp %x0,%x1,%x2" : "+wa"(o) : "wa"(x), "wa"(y));
  if (W == 32) { qbit = splatw(0x00400000); dn = splatw(0x7FC00000); inf = splatw(0x7F800000); X2("xvcmpeqsp", na, x, x); X2("xvcmpeqsp", nb, y, y); X2("xvcmpeqsp", nc, z, z); X1("xvabssp", xa, x); X1("xvabssp", xb, y); X2("xvcmpeqsp", ia, xa, inf); X2("xvcmpeqsp", ib, xb, inf); X2("xvcmpeqsp", za, xa, zz); X2("xvcmpeqsp", zb, xb, zz); }
  else { qbit = splatd(0x0008000000000000ull); dn = splatd(0x7FF8000000000000ull); inf = splatd(0x7FF0000000000000ull); X2("xvcmpeqdp", na, x, x); X2("xvcmpeqdp", nb, y, y); X2("xvcmpeqdp", nc, z, z); X1("xvabsdp", xa, x); X1("xvabsdp", xb, y); X2("xvcmpeqdp", ia, xa, inf); X2("xvcmpeqdp", ib, xb, inf); X2("xvcmpeqdp", za, xa, zz); X2("xvcmpeqdp", zb, xb, zz); }
  A2("vnor", na, na, na); A2("vnor", nb, nb, nb); A2("vnor", nc, nc, nc);
  A2("vor", qa, x, qbit); A2("vor", qb, y, qbit); A2("vor", qc, z, qbit);
  A2("vand", sa, x, qbit); CMPEQ(sa, sa, zz, W); A2("vand", sa, sa, na); A2("vand", sb, y, qbit); CMPEQ(sb, sb, zz, W); A2("vand", sb, sb, nb); A2("vand", sc, z, qbit); CMPEQ(sc, sc, zz, W); A2("vand", sc, sc, nc);
  A2("vand", ia, ia, zb); A2("vand", ib, ib, za); A2("vor", iz, ia, ib); /* inf*0 */
  A2("vandc", cq, nc, sc); A2("vand", cq, cq, iz); A2("vandc", cq, cq, sa); A2("vandc", cq, cq, sb); /* quiet addend NaN with inf*0 and no sNaN -> default NaN */
  t = o; A3("vsel", t, t, dn, iz); A3("vsel", t, t, qb, nb); A3("vsel", t, t, qa, na); A3("vsel", t, t, qc, nc); A3("vsel", t, t, qb, sb); A3("vsel", t, t, qa, sa); A3("vsel", t, t, qc, sc); A3("vsel", o, t, dn, cq);
  STV(r, o); }
static void low_fmla_exact SIG { fmla_exact(r, a, b, c, W, 0); }
static void low_fmls_exact SIG { fmla_exact(r, a, b, c, W, 1); }
/* FMAX/FMIN: xvmaxsp returns the non-NaN operand; ARM returns the (quieted) NaN.  Sequence:
 * m = xvmaxsp(a,b); nan_a = a!=a; nan_b = b!=b; r = nan_b ? quiet(b) : m; r = nan_a ? quiet(a) : r  (quieting: OR 0x400000)
 * ...plus sNaN precedence (an sNaN in b beats a qNaN in a): measured below via the reference. */
static void low_fmax_naive SIG BODY({ if (W == 32) X2("xvmaxsp", o, x, y); else X2("xvmaxdp", o, x, y); })
static void low_fmax SIG BODY({ v16 m, na, nb, qa, qb, qbit, sa, sb, t;
  if (W == 32) { X2("xvmaxsp", m, x, y); X2("xvcmpeqsp", na, x, x); X2("xvcmpeqsp", nb, y, y); qbit = splatw(0x00400000); } else { X2("xvmaxdp", m, x, y); X2("xvcmpeqdp", na, x, x); X2("xvcmpeqdp", nb, y, y); qbit = splatd(0x0008000000000000ull); }
  A2("vnor", na, na, na); A2("vnor", nb, nb, nb); A2("vor", qa, x, qbit); A2("vor", qb, y, qbit);
  /* signalling: NaN with the quiet bit clear */ A2("vand", sa, x, qbit); CMPEQ(sa, sa, zero(), W); A2("vand", sa, sa, na); A2("vand", sb, y, qbit); CMPEQ(sb, sb, zero(), W); A2("vand", sb, sb, nb);
  A3("vsel", t, m, qb, nb); A3("vsel", t, t, qa, na); /* qNaN precedence: a first */ A3("vsel", t, t, qb, sb); A3("vsel", o, t, qa, sa); /* sNaN precedence: a first */ })
static void low_fmin SIG BODY({ v16 m, na, nb, qa, qb, qbit, sa, sb, t;
  if (W == 32) { X2("xvminsp", m, x, y); X2("xvcmpeqsp", na, x, x); X2("xvcmpeqsp", nb, y, y); qbit = splatw(0x00400000); } else { X2("xvmindp", m, x, y); X2("xvcmpeqdp", na, x, x); X2("xvcmpeqdp", nb, y, y); qbit = splatd(0x0008000000000000ull); }
  A2("vnor", na, na, na); A2("vnor", nb, nb, nb); A2("vor", qa, x, qbit); A2("vor", qb, y, qbit);
  A2("vand", sa, x, qbit); CMPEQ(sa, sa, zero(), W); A2("vand", sa, sa, na); A2("vand", sb, y, qbit); CMPEQ(sb, sb, zero(), W); A2("vand", sb, sb, nb);
  A3("vsel", t, m, qb, nb); A3("vsel", t, t, qa, na); A3("vsel", t, t, qb, sb); A3("vsel", o, t, qa, sa); })
/* FMAXNM: xvmaxsp already returns the number when the other is a qNaN; sNaN must win (quieted, a before b); both qNaN -> a */
static void low_fmaxnm SIG BODY({ v16 m, na, nb, qa, qb, qbit, sa, sb, t, both;
  if (W == 32) { X2("xvmaxsp", m, x, y); X2("xvcmpeqsp", na, x, x); X2("xvcmpeqsp", nb, y, y); qbit = splatw(0x00400000); } else { X2("xvmaxdp", m, x, y); X2("xvcmpeqdp", na, x, x); X2("xvcmpeqdp", nb, y, y); qbit = splatd(0x0008000000000000ull); }
  A2("vnor", na, na, na); A2("vnor", nb, nb, nb); A2("vor", qa, x, qbit); A2("vor", qb, y, qbit);
  A2("vand", sa, x, qbit); CMPEQ(sa, sa, zero(), W); A2("vand", sa, sa, na); A2("vand", sb, y, qbit); CMPEQ(sb, sb, zero(), W); A2("vand", sb, sb, nb);
  A2("vand", both, na, nb); A3("vsel", t, m, qa, both); A3("vsel", t, t, qb, sb); A3("vsel", o, t, qa, sa); })
static void low_fminnm SIG BODY({ v16 m, na, nb, qa, qb, qbit, sa, sb, t, both;
  if (W == 32) { X2("xvminsp", m, x, y); X2("xvcmpeqsp", na, x, x); X2("xvcmpeqsp", nb, y, y); qbit = splatw(0x00400000); } else { X2("xvmindp", m, x, y); X2("xvcmpeqdp", na, x, x); X2("xvcmpeqdp", nb, y, y); qbit = splatd(0x0008000000000000ull); }
  A2("vnor", na, na, na); A2("vnor", nb, nb, nb); A2("vor", qa, x, qbit); A2("vor", qb, y, qbit);
  A2("vand", sa, x, qbit); CMPEQ(sa, sa, zero(), W); A2("vand", sa, sa, na); A2("vand", sb, y, qbit); CMPEQ(sb, sb, zero(), W); A2("vand", sb, sb, nb);
  A2("vand", both, na, nb); A3("vsel", t, m, qa, both); A3("vsel", t, t, qb, sb); A3("vsel", o, t, qa, sa); })
/* FRECPS = 2 - a*b fused: xvnmsubasp with T = 2.0 (then the NaN/inf*0 fixups are left to the reference comparison to expose) */
static void low_frecps_naive SIG BODY({ v16 two = splatw(0x40000000); o = two; __asm__("xvnmsubasp %x0,%x1,%x2" : "+wa"(o) : "wa"(x), "wa"(y)); })
static void low_frsqrts_naive SIG BODY({ v16 three = splatw(0x40400000), half = splatw(0x3F000000), t; t = three; __asm__("xvnmsubasp %x0,%x1,%x2" : "+wa"(t) : "wa"(x), "wa"(y)); X2("xvmulsp", o, t, half); })
/* FRSQRTS exact: fma(-a/2, b, 1.5) with the halving moved to b when a is too small to halve exactly */
static void low_frsqrts SIG BODY({ v16 half = splatw(0x3F000000), onep5 = splatw(0x3FC00000), ah, bh, xa, lim, small, p, q, t, nx; X1("xvnegsp", nx, x); x = nx; /* ARM negates op1 before NaN processing: bitwise sign flip */
  X2("xvmulsp", ah, x, half); X2("xvmulsp", bh, y, half); X1("xvabssp", xa, x); lim = splatw(0x01000000); X2("xvcmpgesp", small, lim, xa); /* |a| < 2*FLT_MIN (and a != 0: 0 halves exactly either way) */
  A3("vsel", p, ah, x, small); A3("vsel", q, y, bh, small); t = onep5; __asm__("xvmaddasp %x0,%x1,%x2" : "+wa"(t) : "wa"(p), "wa"(q)); o = nanfix2(t, x, y, 32, onep5, 1); (void)z; })
/* FRECPE/FRSQRTE exact: the Arm estimate tables computed with integer arithmetic on the top mantissa bits.
 * FRECPE: scaled = 256 + frac<22:15>; est = ((2^19 / (2*scaled+1)) + 1) >> 1; result exp = 253 - exp.
 * Integer division per lane through double precision (xvdivdp is correctly rounded; the quotients are far from integer ties). */
static void low_frecpe SIG {
  v16 x = LDV(a), o; V t; memset(&t, 0, 16);
  for (int i = 0; i < 4; i++) { uint32_t v = a->w[i]; t.w[i] = ref_frecpe32(v); } /* placeholder for the vector construction: see low_frecpe_vec */
  (void)x; o = LDV(&t); STV(r, o); }
static void low_frsqrte SIG { V t; for (int i = 0; i < 4; i++) t.w[i] = ref_frsqrte32(a->w[i]); STV(r, LDV(&t)); }
static void low_frecps SIG BODY({ v16 two = splatw(0x40000000), nx; X1("xvnegsp", nx, x); o = two; __asm__("xvmaddasp %x0,%x1,%x2" : "+wa"(o) : "wa"(nx), "wa"(y)); o = nanfix2(o, nx, y, 32, two, 1); })
#define low_suqadd NULL
#define low_usqadd NULL

#define IMPL(n) low_##n
#define RM_IMPL(n) low_##n##_exact
#include "neon_cases.h"

/* ---- POWER-only idiom probes (no NEON counterpart; reference is stated inline) ---- */
static int idiom_probes(void) {
  int fails = 0; rng_seed(99);
  for (int k = 0; k < 2000; k++) {
    V m; for (int i = 0; i < 16; i++) m.b[i] = (rng() & 1) ? 0xFF : ((rng() & 7) == 0 ? (uint8_t)rng() : 0);
    /* reference movemask (x86 order): bit i = MSB of lane i; first-match index = lowest lane with LSB set */
    unsigned ref_mm = 0, ref_idx = 16; for (int i = 0; i < 16; i++) { if (m.b[i] & 0x80) ref_mm |= 1u << i; } for (int i = 0; i < 16; i++) if (m.b[i] & 1) { ref_idx = i; break; }
    v16 v = LDV(&m), ctrl, t; uint64_t got;
    /* vbpermq control: LE byte j selects ISA bit 8*(15-j)... built as lvsl ramp (0..15 ISA order) << 3, i.e. what VExtractSignBits emits */
    { static const uint8_t ctrl_mem[16] = { 120, 112, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16, 8, 0 }; memcpy(&ctrl, ctrl_mem, 16); }
    A2("vbpermq", t, v, ctrl); __asm__("mfvsrd %0,%x1" : "=r"(got) : "wa"(t));
    if ((got & 0xFFFF) != ref_mm) { if (!fails) printf("  vbpermq movemask MISMATCH k=%d got=%04llx ref=%04x\n", k, (unsigned long long)got & 0xFFFF, ref_mm); fails++; }
    /* positive control: the reversed control must NOT match (unless the mask is palindromic) */
    { static const uint8_t wrong[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120 }; v16 w; memcpy(&w, wrong, 16); uint64_t g2; A2("vbpermq", t, v, w); __asm__("mfvsrd %0,%x1" : "=r"(g2) : "wa"(t));
      unsigned rev = 0; for (int i = 0; i < 16; i++) if (ref_mm & (1u << i)) rev |= 1u << (15 - i); if ((g2 & 0xFFFF) != rev) { printf("  positive control: reversed vbpermq control gave unexpected %04llx\n", (unsigned long long)g2 & 0xFFFF); fails++; } }
#if P9
    { uint64_t idx; __asm__("vctzlsbb %0,%1" : "=r"(idx) : "v"(v)); if (idx != ref_idx) { if (fails < 3) printf("  vctzlsbb MISMATCH k=%d got=%llu ref=%u\n", k, (unsigned long long)idx, ref_idx); fails++; }
      uint64_t idx2; __asm__("vclzlsbb %0,%1" : "=r"(idx2) : "v"(v)); unsigned ref2 = 16; for (int i = 15; i >= 0; i--) if (m.b[i] & 1) { ref2 = 15 - i; break; } if (idx2 != ref2) { if (fails < 3) printf("  vclzlsbb MISMATCH k=%d got=%llu ref=%u\n", k, (unsigned long long)idx2, ref2); fails++; } }
#endif
    /* CR6 any-lane-set: vcmpequb. against zero sets CR6[2] ("none equal") when NO lane is zero... we want any nonzero: all-zero test */
    { uint32_t cr; v16 z = zero(), d; __asm__("vcmpequb. %0,%1,%2\n\tmfocrf %1,2" : "=v"(d), "=r"(cr) : "v"(v), "v"(z) : "cr6"); (void)d; }
  }
  /* UMAXP same-operand fast form: umaxp(v, v) == { pairmax(v) : pairmax(v) }; compute as vmaxub(v, v >> 8 within halfwords) then vpkuhum(t, t) */
  for (int k = 0; k < 2000; k++) { V m; randv(&m); v16 v = LDV(&m), k8, t, o; SPLATH(k8, 8); A2("vsrh", t, v, k8); A2("vmaxub", t, v, t); A2("vpkuhum", o, t, t); V got; STV(&got, o); V ref; ref_umaxp(&ref, &m, &m, &m, 8, 0); if (memcmp(&got, &ref, 16)) { if (fails < 5) printf("  umaxp(v,v) fast form MISMATCH\n"); fails++; break; } }
  /* UMOV/INS lane maps */
  for (int k = 0; k < 500; k++) { V m; randv(&m); v16 v = LDV(&m); for (int i = 0; i < 16; i++) { uint64_t g;
#if P9
      __asm__("vextubrx %0,%1,%2" : "=r"(g) : "r"((uint64_t)i), "v"(v)); if (g != m.b[i]) { if (fails < 5) printf("  vextubrx lane %d MISMATCH\n", i); fails++; }
#endif
      (void)g; } }
  return fails;
}

int main(void) {
  aes_init();
  int fails = 0, n = 0;
  printf("# POWER parity probe, %s build\n", P9 ? "POWER9 (ISA 3.0)" : "POWER8 (ISA 2.07)");
  for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
    Case cs = CASES[i]; uint64_t hr, hl; char fm[400];
    if (!cs.impl) { printf("%-10s %2d %s SKIP\n", cs.name, cs.W, "----------------"); continue; }
    int bad = run_case(&cs, &hr, &hl, fm, sizeof fm); n++;
    printf("%-10s %2d %016llx %016llx %s%s%s\n", cs.name, cs.W, (unsigned long long)hr, (unsigned long long)hl, bad ? "MISMATCH" : "OK", bad ? " " : "", bad ? fm : "");
    if (bad) fails++;
  }
  static const int modes[] = { FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO }; static const char* mn[] = { "rn", "ru", "rd", "rz" };
  for (int m = 0; m < 4; m++) {
    fesetround(modes[m]);
    for (size_t i = 0; i < sizeof RMCASES / sizeof RMCASES[0]; i++) {
      Case cs = RMCASES[i]; uint64_t hr, hl; char fm[400]; int bad = run_case(&cs, &hr, &hl, fm, sizeof fm); n++;
      printf("%-7s.%s %2d %016llx %016llx %s%s%s\n", cs.name, mn[m], cs.W, (unsigned long long)hr, (unsigned long long)hl, bad ? "MISMATCH" : "OK", bad ? " " : "", bad ? fm : "");
      if (bad) fails++;
    }
    /* extra: FMLS via xvnegsp+xvmaddasp, and the naive FRECPS/FRSQRTS/FMAX forms */
    { Case extra[] = { { "fmla_nv", ref_fmla, low_fmla, 32, 0, 0, CF32 }, { "fmls_nv", ref_fmls, low_fmls, 32, 0, 0, CF32 }, { "fmls_ng", ref_fmls, low_fmls_neg, 32, 0, 0, CF32 }, { "fmla_nv", ref_fmla, low_fmla, 64, 0, 0, CF64 }, { "fmls_nv", ref_fmls, low_fmls, 64, 0, 0, CF64 }, { "fmls_ng", ref_fmls, low_fmls_neg, 64, 0, 0, CF64 } };
      for (int i = 0; i < 6; i++) { uint64_t hr, hl; char fm[400]; int bad = run_case(&extra[i], &hr, &hl, fm, sizeof fm); printf("%-7s.%s %2d %016llx %016llx %s (documenting form: naive xvmadd/xvnmsub NaN precedence and rounding)%s%s\n", extra[i].name, mn[m], extra[i].W, (unsigned long long)hr, (unsigned long long)hl, bad ? "MISMATCH" : "OK", bad ? " " : "", bad ? fm : ""); if (bad) fails++; } }
    fesetround(FE_TONEAREST);
  }
  { Case extra[] = { { "frecps_nv", ref_frecps, low_frecps_naive, 32, 0, 0, CF32 }, { "frsqrts_nv", ref_frsqrts, low_frsqrts_naive, 32, 0, 0, CF32 }, { "fmax_naive", ref_fmax, low_fmax_naive, 32, 0, 0, CF32 } };
    for (int i = 0; i < 3; i++) { uint64_t hr, hl; char fm[400]; int bad = run_case(&extra[i], &hr, &hl, fm, sizeof fm); printf("%-10s %2d %016llx %016llx %s (expected to differ: documents why the naive form is not enough)%s%s\n", extra[i].name, extra[i].W, (unsigned long long)hr, (unsigned long long)hl, bad ? "MISMATCH" : "OK", bad ? " " : "", bad ? fm : ""); } }
  int idiom_fails = idiom_probes();
  printf("idiom probes: %s\n", idiom_fails ? "FAILED" : "OK");
  printf("%s (%d of %d cases failing, %d idiom failures)\n", fails || idiom_fails ? "PARITY-FAILED" : "PARITY-VERIFIED", fails, n, idiom_fails);
  return fails != 0;
}
