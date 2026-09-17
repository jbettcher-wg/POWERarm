/* armref.h -- portable C model of the Arm ARM scalar floating-point
 * pseudocode, used as the oracle for the POWER lowering probes.
 *
 * Modelled functions (Arm ARM, shared/functions/float):
 *   FPProcessNaNs, FPProcessNaNs3, FPProcessNaN, FPDefaultNaN
 *   FPAdd, FPSub, FPMul, FPDiv, FPSqrt, FPMulAdd
 *   FPMax, FPMin, FPMaxNum, FPMinNum
 *   FPCompare (-> NZCV)
 *   FPToFixed (all FPRounding values, saturating, NaN -> 0)
 *   FixedToFP
 *   FPConvert (double <-> single, double <-> half), with FPConvertNaN
 *
 * FPCR.DN = 0, FPCR.FZ = 0, FPCR.FZ16 = 0, FPCR.AHP = 0 throughout: that is
 * what glibc programs run with and what the POWERarm frontend emulates.
 *
 * Only the NaN, infinity and zero cases are decided here; finite IEEE
 * arithmetic is delegated to the host C compiler under fesetround(), which
 * every IEEE 754 machine computes identically. The integer conversions and
 * the narrowing conversions are done with integer arithmetic so that the
 * model does not depend on any host conversion instruction.
 *
 * Positive control: armsel.c on the Raspberry Pi 5 runs the same corpus
 * through the real A64 instructions and must agree with this model on every
 * case before the model is used as an oracle on POWER.
 */
#ifndef ARMREF_H
#define ARMREF_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <fenv.h>

enum { RM_TIEEVEN = 0, RM_POSINF = 1, RM_NEGINF = 2, RM_ZERO = 3, RM_TIEAWAY = 4 };

static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* Set the C rounding mode from an FPCR.RMode value (0..3). */
static inline void set_c_rounding(int rm) {
  switch (rm) {
  case RM_TIEEVEN: fesetround(FE_TONEAREST); break;
  case RM_POSINF:  fesetround(FE_UPWARD); break;
  case RM_NEGINF:  fesetround(FE_DOWNWARD); break;
  default:         fesetround(FE_TOWARDZERO); break;
  }
}

/* ---- classification ------------------------------------------------- */
enum fptype { T_NONZERO, T_ZERO, T_INF, T_QNAN, T_SNAN };

static inline enum fptype cls64(uint64_t u) {
  uint64_t e = (u >> 52) & 0x7FF, f = u & 0xFFFFFFFFFFFFFull;
  if (e == 0x7FF) return f == 0 ? T_INF : ((f >> 51) & 1) ? T_QNAN : T_SNAN;
  if (e == 0 && f == 0) return T_ZERO;
  return T_NONZERO;
}
static inline enum fptype cls32(uint32_t u) {
  uint32_t e = (u >> 23) & 0xFF, f = u & 0x7FFFFF;
  if (e == 0xFF) return f == 0 ? T_INF : ((f >> 22) & 1) ? T_QNAN : T_SNAN;
  if (e == 0 && f == 0) return T_ZERO;
  return T_NONZERO;
}
static inline enum fptype cls16(uint16_t u) {
  uint32_t e = (u >> 10) & 0x1F, f = u & 0x3FF;
  if (e == 0x1F) return f == 0 ? T_INF : ((f >> 9) & 1) ? T_QNAN : T_SNAN;
  if (e == 0 && f == 0) return T_ZERO;
  return T_NONZERO;
}
static inline int isnan_t(enum fptype t) { return t == T_QNAN || t == T_SNAN; }

#define DNAN64 0x7FF8000000000000ull
#define DNAN32 0x7FC00000u
#define DNAN16 0x7E00u

/* FPProcessNaN: quiet the NaN (DN = 0). */
static inline uint64_t qnan64(uint64_t u) { return u | (1ull << 51); }
static inline uint32_t qnan32(uint32_t u) { return u | (1u << 22); }

/* FPProcessNaNs: first SNaN, else first QNaN. Returns 1 when done. */
static inline int procnans64(uint64_t a, uint64_t b, uint64_t* r) {
  enum fptype ta = cls64(a), tb = cls64(b);
  if (ta == T_SNAN) { *r = qnan64(a); return 1; }
  if (tb == T_SNAN) { *r = qnan64(b); return 1; }
  if (ta == T_QNAN) { *r = a; return 1; }
  if (tb == T_QNAN) { *r = b; return 1; }
  return 0;
}
static inline int procnans32(uint32_t a, uint32_t b, uint32_t* r) {
  enum fptype ta = cls32(a), tb = cls32(b);
  if (ta == T_SNAN) { *r = qnan32(a); return 1; }
  if (tb == T_SNAN) { *r = qnan32(b); return 1; }
  if (ta == T_QNAN) { *r = a; return 1; }
  if (tb == T_QNAN) { *r = b; return 1; }
  return 0;
}
/* FPProcessNaNs3 for FPMulAdd: operands in the order (addend, op1, op2). */
static inline int procnans3_64(uint64_t a, uint64_t n, uint64_t m, uint64_t* r) {
  enum fptype ta = cls64(a), tn = cls64(n), tm = cls64(m);
  if (ta == T_SNAN) { *r = qnan64(a); return 1; }
  if (tn == T_SNAN) { *r = qnan64(n); return 1; }
  if (tm == T_SNAN) { *r = qnan64(m); return 1; }
  if (ta == T_QNAN) { *r = a; return 1; }
  if (tn == T_QNAN) { *r = n; return 1; }
  if (tm == T_QNAN) { *r = m; return 1; }
  return 0;
}
static inline int procnans3_32(uint32_t a, uint32_t n, uint32_t m, uint32_t* r) {
  enum fptype ta = cls32(a), tn = cls32(n), tm = cls32(m);
  if (ta == T_SNAN) { *r = qnan32(a); return 1; }
  if (tn == T_SNAN) { *r = qnan32(n); return 1; }
  if (tm == T_SNAN) { *r = qnan32(m); return 1; }
  if (ta == T_QNAN) { *r = a; return 1; }
  if (tn == T_QNAN) { *r = n; return 1; }
  if (tm == T_QNAN) { *r = m; return 1; }
  return 0;
}

/* ---- binary arithmetic ----------------------------------------------
 * The finite cases are IEEE and delegated to the host under the C rounding
 * mode set by the caller; only the special cases (which is where ARM and
 * POWER differ) are decided here. */
enum binop { OP_ADD, OP_SUB, OP_MUL, OP_DIV };

static inline uint64_t arm_binop64(enum binop op, uint64_t a, uint64_t b) {
  uint64_t r;
  if (procnans64(a, b, &r)) return r;
  enum fptype ta = cls64(a), tb = cls64(b);
  int sa = a >> 63, sb = b >> 63;
  double x = u2d(a), y = u2d(b);
  switch (op) {
  case OP_ADD: case OP_SUB: {
    if (op == OP_SUB) { sb ^= 1; y = -y; }
    if (ta == T_INF && tb == T_INF && sa != sb) return DNAN64;
    return d2u(x + y);
  }
  case OP_MUL:
    if ((ta == T_INF && tb == T_ZERO) || (ta == T_ZERO && tb == T_INF)) return DNAN64;
    return d2u(x * y);
  default:
    if ((ta == T_INF && tb == T_INF) || (ta == T_ZERO && tb == T_ZERO)) return DNAN64;
    return d2u(x / y);
  }
}
static inline uint32_t arm_binop32(enum binop op, uint32_t a, uint32_t b) {
  uint32_t r;
  if (procnans32(a, b, &r)) return r;
  enum fptype ta = cls32(a), tb = cls32(b);
  int sa = a >> 31, sb = b >> 31;
  float x = u2f(a), y = u2f(b);
  switch (op) {
  case OP_ADD: case OP_SUB: {
    if (op == OP_SUB) { sb ^= 1; y = -y; }
    if (ta == T_INF && tb == T_INF && sa != sb) return DNAN32;
    return f2u(x + y);
  }
  case OP_MUL:
    if ((ta == T_INF && tb == T_ZERO) || (ta == T_ZERO && tb == T_INF)) return DNAN32;
    return f2u(x * y);
  default:
    if ((ta == T_INF && tb == T_INF) || (ta == T_ZERO && tb == T_ZERO)) return DNAN32;
    return f2u(x / y);
  }
}

/* ---- FPMax / FPMin / FPMaxNum / FPMinNum ------------------------------ */
static inline uint64_t arm_minmax64(uint64_t a, uint64_t b, int ismax, int isnum) {
  if (isnum) {
    enum fptype ta = cls64(a), tb = cls64(b);
    uint64_t inf = ismax ? 0xFFF0000000000000ull : 0x7FF0000000000000ull;
    if (ta == T_QNAN && tb != T_QNAN) a = inf;
    else if (ta != T_QNAN && tb == T_QNAN) b = inf;
  }
  uint64_t r;
  if (procnans64(a, b, &r)) return r;
  enum fptype ta = cls64(a), tb = cls64(b);
  if (ta == T_ZERO && tb == T_ZERO) {
    uint64_t sign = ismax ? ((a & b) >> 63) : ((a | b) >> 63);
    return sign << 63;
  }
  double x = u2d(a), y = u2d(b);
  if (ismax) return (x > y) ? a : b;
  return (x < y) ? a : b;
}
static inline uint32_t arm_minmax32(uint32_t a, uint32_t b, int ismax, int isnum) {
  if (isnum) {
    enum fptype ta = cls32(a), tb = cls32(b);
    uint32_t inf = ismax ? 0xFF800000u : 0x7F800000u;
    if (ta == T_QNAN && tb != T_QNAN) a = inf;
    else if (ta != T_QNAN && tb == T_QNAN) b = inf;
  }
  uint32_t r;
  if (procnans32(a, b, &r)) return r;
  enum fptype ta = cls32(a), tb = cls32(b);
  if (ta == T_ZERO && tb == T_ZERO) {
    uint32_t sign = ismax ? ((a & b) >> 31) : ((a | b) >> 31);
    return sign << 31;
  }
  float x = u2f(a), y = u2f(b);
  if (ismax) return (x > y) ? a : b;
  return (x < y) ? a : b;
}

/* ---- FPMulAdd(addend, op1, op2): addend + op1*op2, one rounding --------
 * The A64 instructions are FMADD a+n*m, FMSUB a+(-n)*m, FNMADD (-a)+(-n)*m,
 * FNMSUB (-a)+n*m; the caller negates the operands (FPNeg flips the sign
 * bit of a NaN too, which matters for which NaN is returned). */
static inline uint64_t arm_muladd64(uint64_t a, uint64_t n, uint64_t m) {
  enum fptype ta = cls64(a), tn = cls64(n), tm = cls64(m);
  int infn = tn == T_INF, zn = tn == T_ZERO, infm = tm == T_INF, zm = tm == T_ZERO;
  uint64_t r;
  int done = procnans3_64(a, n, m, &r);
  if (ta == T_QNAN && ((infn && zm) || (zn && infm))) return DNAN64;
  if (done) return r;
  int infa = ta == T_INF, za = ta == T_ZERO;
  int sa = a >> 63, sp = (n >> 63) ^ (m >> 63);
  int infp = infn || infm, zp = zn || zm;
  if ((infn && zm) || (zn && infm) || (infa && infp && sa != sp)) return DNAN64;
  if ((infa && !sa) || (infp && !sp)) return 0x7FF0000000000000ull;
  if ((infa && sa) || (infp && sp)) return 0xFFF0000000000000ull;
  if (za && zp && sa == sp) return (uint64_t)sa << 63;
  return d2u(fma(u2d(n), u2d(m), u2d(a)));
}
static inline uint32_t arm_muladd32(uint32_t a, uint32_t n, uint32_t m) {
  enum fptype ta = cls32(a), tn = cls32(n), tm = cls32(m);
  int infn = tn == T_INF, zn = tn == T_ZERO, infm = tm == T_INF, zm = tm == T_ZERO;
  uint32_t r;
  int done = procnans3_32(a, n, m, &r);
  if (ta == T_QNAN && ((infn && zm) || (zn && infm))) return DNAN32;
  if (done) return r;
  int infa = ta == T_INF, za = ta == T_ZERO;
  int sa = a >> 31, sp = (n >> 31) ^ (m >> 31);
  int infp = infn || infm, zp = zn || zm;
  if ((infn && zm) || (zn && infm) || (infa && infp && sa != sp)) return DNAN32;
  if ((infa && !sa) || (infp && !sp)) return 0x7F800000u;
  if ((infa && sa) || (infp && sp)) return 0xFF800000u;
  if (za && zp && sa == sp) return (uint32_t)sa << 31;
  return f2u(fmaf(u2f(n), u2f(m), u2f(a)));
}

/* ---- FPSqrt ------------------------------------------------------------ */
static inline uint64_t arm_sqrt64(uint64_t a) {
  enum fptype t = cls64(a);
  if (t == T_SNAN) return qnan64(a);
  if (t == T_QNAN) return a;
  if (t == T_ZERO) return a;
  if (a >> 63) return DNAN64;
  return d2u(sqrt(u2d(a)));
}
static inline uint32_t arm_sqrt32(uint32_t a) {
  enum fptype t = cls32(a);
  if (t == T_SNAN) return qnan32(a);
  if (t == T_QNAN) return a;
  if (t == T_ZERO) return a;
  if (a >> 31) return DNAN32;
  return f2u(sqrtf(u2f(a)));
}

/* ---- FPCompare -> NZCV (bit3 = N, bit2 = Z, bit1 = C, bit0 = V) -------- */
static inline unsigned arm_fcmp64(uint64_t a, uint64_t b) {
  if (isnan_t(cls64(a)) || isnan_t(cls64(b))) return 0x3;
  double x = u2d(a), y = u2d(b);
  if (x == y) return 0x6;
  if (x < y) return 0x8;
  return 0x2;
}
static inline unsigned arm_fcmp32(uint32_t a, uint32_t b) {
  if (isnan_t(cls32(a)) || isnan_t(cls32(b))) return 0x3;
  float x = u2f(a), y = u2f(b);
  if (x == y) return 0x6;
  if (x < y) return 0x8;
  return 0x2;
}

/* ---- FPToFixed ----------------------------------------------------------
 * Integer-arithmetic implementation: decompose the double into
 * sign / exponent / 53-bit significand, scale by 2^fbits, round per the
 * FPRounding value, saturate to the destination width. Never calls a host
 * conversion. Returns the (possibly saturated) integer as uint64_t bits. */
static inline uint64_t arm_fptofixed64(uint64_t u, int fbits, int isunsigned, int rounding, int bits) {
  enum fptype t = cls64(u);
  uint64_t smax = isunsigned ? (bits == 64 ? ~0ull : ((1ull << bits) - 1))
                             : ((1ull << (bits - 1)) - 1);
  uint64_t smin = isunsigned ? 0 : ((0ull - (1ull << (bits - 1))) & (bits == 64 ? ~0ull : ((1ull << bits) - 1)));
  if (isnan_t(t)) return 0;
  int sign = u >> 63;
  if (t == T_INF) return sign ? smin : smax;
  if (t == T_ZERO) return 0;
  int e = (int)((u >> 52) & 0x7FF);
  uint64_t sig = u & 0xFFFFFFFFFFFFFull;
  if (e == 0) e = 1; else sig |= 1ull << 52;
  /* value = sig * 2^(e - 1075); scaled = sig * 2^(e - 1075 + fbits) */
  int sh = e - 1075 + fbits; /* integer part = sig << sh */
  uint64_t ip;      /* integer magnitude before rounding */
  int inc = 0;
  if (sh >= 0) {
    if (sh > 11) {
      /* sig < 2^53, so magnitude >= 2^(53+11) > any 64-bit range: saturate */
      return sign ? smin : smax;
    }
    ip = sig << sh;
  } else {
    int rs = -sh;
    uint64_t rem, half;
    if (rs >= 64) { ip = 0; rem = sig ? 1 : 0; half = 2; /* rem < half: below half */
      if (rs == 64) { /* exactly: value < 1, compare sig with 2^63? sig < 2^53 so < half */ }
    } else {
      ip = sig >> rs;
      rem = sig & ((1ull << rs) - 1);
      half = 1ull << (rs - 1);
    }
    switch (rounding) {
    case RM_TIEEVEN: inc = rem > half || (rem == half && (ip & 1)); break;
    case RM_TIEAWAY: inc = rem >= half; break;
    case RM_POSINF:  inc = rem != 0 && !sign; break;
    case RM_NEGINF:  inc = rem != 0 && sign; break;
    default:         inc = 0; break;
    }
    ip += inc;
  }
  /* saturate */
  if (isunsigned) {
    if (sign) return ip ? 0 : 0; /* negative magnitude: saturates at 0 */
    if (bits < 64 && ip > smax) return smax;
    return ip;
  }
  if (!sign) {
    if (ip > smax) return smax;
    return ip;
  }
  if (ip > (1ull << (bits - 1))) return smin;
  return (0ull - ip) & (bits == 64 ? ~0ull : ((1ull << bits) - 1));
}

/* ---- FixedToFP: integer -> double / float, one rounding per mode --------
 * Integer arithmetic: normalize the 64-bit magnitude to p bits, round. */
static inline uint64_t round_mag_to_fp(uint64_t mag, int sign, int p, int expbits, int rm) {
  /* value = mag (an integer); build a p-bit significand */
  int bias = (1 << (expbits - 1)) - 1;
  if (mag == 0) return (uint64_t)sign << (p - 1 + expbits);
  int msb = 63 - __builtin_clzll(mag);
  int e = msb; /* value in [2^msb, 2^(msb+1)) */
  uint64_t sig; int inc = 0;
  if (msb + 1 <= p) {
    sig = mag << (p - 1 - msb);
  } else {
    int rs = msb + 1 - p;
    sig = mag >> rs;
    uint64_t rem = mag & ((1ull << rs) - 1), half = 1ull << (rs - 1);
    switch (rm) {
    case RM_TIEEVEN: inc = rem > half || (rem == half && (sig & 1)); break;
    case RM_POSINF:  inc = rem != 0 && !sign; break;
    case RM_NEGINF:  inc = rem != 0 && sign; break;
    default:         inc = 0; break;
    }
    sig += inc;
    if (sig >> p) { sig >>= 1; e++; }
  }
  uint64_t frac = sig & ((1ull << (p - 1)) - 1);
  return ((uint64_t)sign << (p - 1 + expbits)) | ((uint64_t)(e + bias) << (p - 1)) | frac;
}
static inline uint64_t arm_fixedtofp64(uint64_t i, int isunsigned, int rm) {
  int sign = 0; uint64_t mag = i;
  if (!isunsigned && (int64_t)i < 0) { sign = 1; mag = 0ull - i; }
  return round_mag_to_fp(mag, sign, 53, 11, rm);
}
static inline uint32_t arm_fixedtofp32(uint64_t i, int isunsigned, int rm) {
  int sign = 0; uint64_t mag = i;
  if (!isunsigned && (int64_t)i < 0) { sign = 1; mag = 0ull - i; }
  return (uint32_t)round_mag_to_fp(mag, sign, 24, 8, rm);
}

/* ---- FPConvert (narrowing): double -> single / half, per FPCR mode -----
 * Bit-exact re-rounding with subnormal handling (FZ = 0). */
static inline uint64_t narrow_bits(uint64_t u, int rm, int p, int expbits) {
  /* source is double: 53-bit significand, exponent bias 1023 */
  int sign = u >> 63;
  enum fptype t = cls64(u);
  int tbias = (1 << (expbits - 1)) - 1, tw = p - 1 + expbits; /* total bits */
  uint64_t signbit = (uint64_t)sign << tw;
  uint64_t infbits = signbit | (((1ull << expbits) - 1) << (p - 1));
  if (t == T_INF) return infbits;
  if (t == T_ZERO) return signbit;
  if (isnan_t(t)) {
    /* FPConvertNaN: keep the top payload bits, force the quiet bit */
    uint64_t frac = (u & 0xFFFFFFFFFFFFFull) >> (52 - (p - 1));
    frac |= 1ull << (p - 2);
    return infbits | frac;
  }
  int e = (int)((u >> 52) & 0x7FF);
  uint64_t sig = u & 0xFFFFFFFFFFFFFull;
  if (e == 0) { /* double subnormal: normalize (only reachable for tiny values) */
    int lz = __builtin_clzll(sig) - 11;
    sig <<= lz; e = 1 - lz;
  } else sig |= 1ull << 52;
  int ue = e - 1023; /* unbiased exponent, value = 1.sig * 2^ue */
  int emin = 1 - tbias, emax = tbias;
  /* target significand: p bits. Shift right by (53 - p) normally; more when subnormal */
  int rs = 53 - p;
  int te = ue;
  if (ue < emin) { rs += emin - ue; te = emin; }
  uint64_t tsig, rem, half;
  if (rs >= 64) { tsig = 0; rem = sig; half = ~0ull; /* rem < half */ }
  else { tsig = sig >> rs; rem = sig & ((1ull << rs) - 1); half = 1ull << (rs - 1); }
  int inc;
  switch (rm) {
  case RM_TIEEVEN: inc = rem > half || (rem == half && (tsig & 1)); break;
  case RM_POSINF:  inc = rem != 0 && !sign; break;
  case RM_NEGINF:  inc = rem != 0 && sign; break;
  default:         inc = 0; break;
  }
  tsig += inc;
  if (tsig >> p) { tsig >>= 1; te++; }
  if (te < emin || !(tsig >> (p - 1))) {
    /* subnormal or zero result */
    return signbit | tsig;
  }
  if (te > emax) {
    /* overflow: Inf for nearest and the matching directed mode, else max finite */
    int toinf = rm == RM_TIEEVEN || (rm == RM_POSINF && !sign) || (rm == RM_NEGINF && sign);
    if (toinf) return infbits;
    return signbit | ((uint64_t)((1 << expbits) - 2) << (p - 1)) | ((1ull << (p - 1)) - 1);
  }
  return signbit | ((uint64_t)(te + tbias) << (p - 1)) | (tsig & ((1ull << (p - 1)) - 1));
}
static inline uint32_t arm_d2s(uint64_t u, int rm) { return (uint32_t)narrow_bits(u, rm, 24, 8); }
static inline uint16_t arm_d2h(uint64_t u, int rm) { return (uint16_t)narrow_bits(u, rm, 11, 5); }
/* single -> half is FPConvert too; go through the exact double widening. */
static inline uint64_t widen_s2d(uint32_t u) {
  enum fptype t = cls32(u);
  uint64_t sign = (uint64_t)(u >> 31) << 63;
  if (t == T_INF) return sign | 0x7FF0000000000000ull;
  if (t == T_ZERO) return sign;
  if (isnan_t(t)) return sign | 0x7FF0000000000000ull | ((uint64_t)(u & 0x7FFFFF) << 29) | (1ull << 51);
  int e = (u >> 23) & 0xFF; uint64_t f = u & 0x7FFFFF;
  if (e == 0) { int lz = __builtin_clzll(f) - 40; f = (f << lz) & 0x7FFFFF; e = 1 - lz; }
  return sign | ((uint64_t)(e - 127 + 1023) << 52) | (f << 29);
}
static inline uint64_t widen_h2d(uint16_t u) {
  enum fptype t = cls16(u);
  uint64_t sign = (uint64_t)(u >> 15) << 63;
  if (t == T_INF) return sign | 0x7FF0000000000000ull;
  if (t == T_ZERO) return sign;
  if (isnan_t(t)) return sign | 0x7FF0000000000000ull | ((uint64_t)(u & 0x3FF) << 42) | (1ull << 51);
  int e = (u >> 10) & 0x1F; uint64_t f = u & 0x3FF;
  if (e == 0) { int lz = __builtin_clzll(f) - 53; f = (f << lz) & 0x3FF; e = 1 - lz; }
  return sign | ((uint64_t)(e - 15 + 1023) << 52) | (f << 42);
}
static inline uint32_t arm_h2s(uint16_t u) { /* exact, NaN quieted with payload kept */
  return (uint32_t)narrow_bits(widen_h2d(u), RM_ZERO, 24, 8); /* exact: mode irrelevant */
}
#endif
