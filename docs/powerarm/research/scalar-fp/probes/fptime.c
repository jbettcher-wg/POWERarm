/* fptime.c -- latency and throughput of candidate POWER lowerings for the
 * A64 scalar FP paths, one kernel per question.
 *
 *   ./fptime list
 *   ./fptime <kernel> lat|tput [ops]     -> prints ns/op; wrap in perf stat
 *
 * Every kernel is a closed asm loop (mtctr/bdnz) so the compiler cannot
 * reorder or hoist anything. "lat" chains each op's result into the next
 * op (one dependency chain); "tput" runs four independent chains. Each
 * loop iteration executes UNROLL ops per chain. Cycles per op come from
 * perf stat cycles / (ops), see fptime.sh.
 *
 * Build: clang -O2 -mcpu=power9 -DP9 -o fptime-p9 fptime.c
 *        clang -O2 -mcpu=power8       -o fptime-p8 fptime.c
 * Branches in the kernels are never taken on the measured path (the data
 * is ordered) unless the kernel name says "mispred".
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef double v2d __attribute__((vector_size(16)));
typedef uint64_t v2u __attribute__((vector_size(16)));
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }

#define UNROLL 4
/* Loop shells. A/B/C/D are VMX-register ("v") vector accumulators so that
 * %[a] names the VR in VMX-form instructions and %x[a] the VSR (32+n) in
 * VSX-form ones; X/Y/Z/K inputs likewise,
 * G0..G3 GPR temporaries, F0 an FPR temp. Kernel bodies use %x[..] for
 * VSX numbering and %[..] for FPR/GPR numbering. */
#define LOOP_LAT(body) __asm__ volatile( \
  "mtctr %[n]\n1:\n" body body body body "bdnz 1b\n" \
  : [a]"+v"(a), [b]"+v"(b), [c]"+v"(c), [d]"+v"(d), [g0]"=&r"(g0), [g1]"=&r"(g1), [g2]"=&r"(g2), [g3]"=&r"(g3), [f0]"=&d"(f0), [t]"=&v"(t), [u]"=&v"(u) \
  : [x]"v"(x), [y]"v"(y), [z]"v"(z), [k]"v"(k), [n]"r"(n), [mem]"r"(mem), [zero]"v"(zero) : "ctr", "cr1", "cr5", "cr6", "cr7", "xer", "memory", "f12", "f13", "f14", "f15", "v8", "v9", "v10", "v11", "v30", "v31")
#define K(name) static void k_##name(long n, int tput)

/* common register set for all kernels */
#define VARS \
  v2d a = (v2d){ 1.0, 1.0 }, b = (v2d){ 1.0000001, 1.0000001 }, c = (v2d){ 2.0, 2.0 }, d = (v2d){ 3.0, 3.0 }; \
  v2d x = (v2d){ 0.999999, 0.999999 }, y = (v2d){ 1.5, 1.5 }, z = (v2d){ 0.5, 0.5 }, k = (v2d){ 0.25, 0.25 }, t, u; \
  v2d zero = (v2d){ 0, 0 }; uint64_t g0, g1, g2, g3; double f0; \
  static uint64_t membuf[64] __attribute__((aligned(64))); uint64_t* mem = membuf; (void)mem; (void)zero; (void)tput;

/* ---- group A: NaN precedence around a scalar add ---- */
K(add_xs)          { VARS LOOP_LAT("xsadddp %x[a], %x[a], %x[x]\n"); }
K(add_xs_t)        { VARS LOOP_LAT("xsadddp %x[a], %x[a], %x[x]\nxsadddp %x[b], %x[b], %x[x]\nxsadddp %x[c], %x[c], %x[x]\nxsadddp %x[d], %x[d], %x[x]\n"); }
K(add_xv)          { VARS LOOP_LAT("xvadddp %x[a], %x[a], %x[x]\n"); }
K(add_xv_t)        { VARS LOOP_LAT("xvadddp %x[a], %x[a], %x[x]\nxvadddp %x[b], %x[b], %x[x]\nxvadddp %x[c], %x[c], %x[x]\nxvadddp %x[d], %x[d], %x[x]\n"); }
/* today's zero-upper tail (VMov i64: vspltisw + 2 vsldoi; VTMP1/2 = v30/v31) */
K(add_xv_jitzero)  { VARS LOOP_LAT("xvadddp %x[a], %x[a], %x[x]\nvspltisw 30, 0\nvsldoi 31, %[a], 30, 8\nvsldoi %[a], 30, 31, 8\n"); }
K(add_xv_jitzero_t){ VARS LOOP_LAT("xvadddp %x[a], %x[a], %x[x]\nvspltisw 30, 0\nvsldoi 31, %[a], 30, 8\nvsldoi %[a], 30, 31, 8\n"
                                    "xvadddp %x[b], %x[b], %x[x]\nvspltisw 30, 0\nvsldoi 31, %[b], 30, 8\nvsldoi %[b], 30, 31, 8\n"
                                    "xvadddp %x[c], %x[c], %x[x]\nvspltisw 30, 0\nvsldoi 31, %[c], 30, 8\nvsldoi %[c], 30, 31, 8\n"
                                    "xvadddp %x[d], %x[d], %x[x]\nvspltisw 30, 0\nvsldoi 31, %[d], 30, 8\nvsldoi %[d], 30, 31, 8\n"); }
/* one xxpermdi with a pinned zero register (vs14 in the backend) */
K(add_xv_permzero) { VARS LOOP_LAT("xvadddp %x[a], %x[a], %x[x]\nxxpermdi %x[a], %x[zero], %x[a], 1\n"); }
K(add_xv_permzero_t){ VARS LOOP_LAT("xvadddp %x[a], %x[a], %x[x]\nxxpermdi %x[a], %x[zero], %x[a], 1\nxvadddp %x[b], %x[b], %x[x]\nxxpermdi %x[b], %x[zero], %x[b], 1\n"
                                     "xvadddp %x[c], %x[c], %x[x]\nxxpermdi %x[c], %x[zero], %x[c], 1\nxvadddp %x[d], %x[d], %x[x]\nxxpermdi %x[d], %x[zero], %x[d], 1\n"); }
/* pre-check branch: compare the inputs, branch never taken */
K(add_pre)         { VARS LOOP_LAT("xscmpudp 1, %x[a], %x[x]\nbso 1, 9f\nxsadddp %x[a], %x[a], %x[x]\n9:\n"); }
K(add_pre_t)       { VARS LOOP_LAT("xscmpudp 1, %x[a], %x[x]\nbso 1, 9f\nxsadddp %x[a], %x[a], %x[x]\n9:\nxscmpudp 5, %x[b], %x[x]\nbso 5, 8f\nxsadddp %x[b], %x[b], %x[x]\n8:\n"
                                    "xscmpudp 6, %x[c], %x[x]\nbso 6, 7f\nxsadddp %x[c], %x[c], %x[x]\n7:\nxscmpudp 7, %x[d], %x[x]\nbso 7, 6f\nxsadddp %x[d], %x[d], %x[x]\n6:\n"); }
/* post-check branch: compare the result with itself */
K(add_post)        { VARS LOOP_LAT("xsadddp %x[a], %x[a], %x[x]\nxscmpudp 1, %x[a], %x[a]\nbso 1, 9f\n9:\n"); }
K(add_post_t)      { VARS LOOP_LAT("xsadddp %x[a], %x[a], %x[x]\nxscmpudp 1, %x[a], %x[a]\nbso 1, 9f\n9:\nxsadddp %x[b], %x[b], %x[x]\nxscmpudp 5, %x[b], %x[b]\nbso 5, 8f\n8:\n"
                                    "xsadddp %x[c], %x[c], %x[x]\nxscmpudp 6, %x[c], %x[c]\nbso 6, 7f\n7:\nxsadddp %x[d], %x[d], %x[x]\nxscmpudp 7, %x[d], %x[d]\nbso 7, 6f\n6:\n"); }
/* branch-free vector fix (9 ops) + xvadddp, the shape of today's frontend fix-up */
#define BFFIX(A) "xvcmpeqdp %x[t], %x[" A "], %x[" A "]\nxvcmpeqdp %x[u], %x[x], %x[x]\nxxland 30, %x[" A "], %x[k]\nxxland 31, %x[x], %x[k]\nvcmpequd 30, 30, %[zero]\nvcmpequd 31, 31, %[zero]\nxxlor %x[t], %x[t], %x[u]\nxxlor %x[t], %x[t], 30\nxxlandc %x[t], 31, %x[t]\nxxsel %x[" A "], %x[" A "], %x[x], %x[t]\nxvadddp %x[" A "], %x[" A "], %x[x]\n"
K(add_bffix)       { VARS LOOP_LAT(BFFIX("a")); }
K(add_bffix_t)     { VARS LOOP_LAT(BFFIX("a") BFFIX("b") BFFIX("c") BFFIX("d")); }
/* f32: vector lane add, and the pre-check with two xscvspdpn */
K(add32_xv)        { VARS LOOP_LAT("xvaddsp %x[a], %x[a], %x[x]\n"); }
K(add32_pre)       { VARS LOOP_LAT("xxsldwi %x[t], %x[a], %x[a], 3\nxxsldwi %x[u], %x[x], %x[x], 3\nxscvspdpn %x[t], %x[t]\nxscvspdpn %x[u], %x[u]\nxscmpudp 1, %x[t], %x[u]\nbso 1, 9f\nxvaddsp %x[a], %x[a], %x[x]\n9:\n"); }
K(add32_pre_t)     { VARS LOOP_LAT("xxsldwi %x[t], %x[a], %x[a], 3\nxxsldwi %x[u], %x[x], %x[x], 3\nxscvspdpn %x[t], %x[t]\nxscvspdpn %x[u], %x[u]\nxscmpudp 1, %x[t], %x[u]\nbso 1, 9f\nxvaddsp %x[a], %x[a], %x[x]\n9:\n"
                                    "xxsldwi %x[t], %x[b], %x[b], 3\nxxsldwi %x[u], %x[x], %x[x], 3\nxscvspdpn %x[t], %x[t]\nxscvspdpn %x[u], %x[u]\nxscmpudp 5, %x[t], %x[u]\nbso 5, 8f\nxvaddsp %x[b], %x[b], %x[x]\n8:\n"
                                    "xxsldwi %x[t], %x[c], %x[c], 3\nxxsldwi %x[u], %x[x], %x[x], 3\nxscvspdpn %x[t], %x[t]\nxscvspdpn %x[u], %x[u]\nxscmpudp 6, %x[t], %x[u]\nbso 6, 7f\nxvaddsp %x[c], %x[c], %x[x]\n7:\n"
                                    "xxsldwi %x[t], %x[d], %x[d], 3\nxxsldwi %x[u], %x[x], %x[x], 3\nxscvspdpn %x[t], %x[t]\nxscvspdpn %x[u], %x[u]\nxscmpudp 7, %x[t], %x[u]\nbso 7, 6f\nxvaddsp %x[d], %x[d], %x[x]\n6:\n"); }
/* f32 post-check on the result: the result lane widened once */
K(add32_post)      { VARS LOOP_LAT("xvaddsp %x[a], %x[a], %x[x]\nxxsldwi %x[t], %x[a], %x[a], 3\nxscvspdpn %x[t], %x[t]\nxscmpudp 1, %x[t], %x[t]\nbso 1, 9f\n9:\n"); }
/* the xvcmpeqsp. recording form sets CR6 (all-equal / all-unequal over four lanes) */
K(add32_cr6)       { VARS LOOP_LAT("xvaddsp %x[a], %x[a], %x[x]\nxvcmpeqsp. %x[t], %x[a], %x[a]\nbc 4, 24, 9f\n9:\n"); }

/* ---- group B: scalar vs vector forms of the long-latency ops ---- */
K(div_xs)          { VARS LOOP_LAT("xsdivdp %x[a], %x[a], %x[x]\n"); }
K(div_xs_t)        { VARS LOOP_LAT("xsdivdp %x[a], %x[a], %x[x]\nxsdivdp %x[b], %x[b], %x[x]\nxsdivdp %x[c], %x[c], %x[x]\nxsdivdp %x[d], %x[d], %x[x]\n"); }
K(div_xv)          { VARS LOOP_LAT("xvdivdp %x[a], %x[a], %x[x]\n"); }
K(div_xv_t)        { VARS LOOP_LAT("xvdivdp %x[a], %x[a], %x[x]\nxvdivdp %x[b], %x[b], %x[x]\nxvdivdp %x[c], %x[c], %x[x]\nxvdivdp %x[d], %x[d], %x[x]\n"); }
K(div_xv_perm)     { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 2\nxsdivdp %x[t], %x[t], %x[x]\nxxpermdi %x[a], %x[zero], %x[t], 0\n"); }
K(div32_xv)        { VARS LOOP_LAT("xvdivsp %x[a], %x[a], %x[x]\n"); }
K(div32_xv_t)      { VARS LOOP_LAT("xvdivsp %x[a], %x[a], %x[x]\nxvdivsp %x[b], %x[b], %x[x]\nxvdivsp %x[c], %x[c], %x[x]\nxvdivsp %x[d], %x[d], %x[x]\n"); }
K(div32_xs)        { VARS LOOP_LAT("xsdivsp %x[a], %x[a], %x[x]\n"); }
K(div32_xs_t)      { VARS LOOP_LAT("xsdivsp %x[a], %x[a], %x[x]\nxsdivsp %x[b], %x[b], %x[x]\nxsdivsp %x[c], %x[c], %x[x]\nxsdivsp %x[d], %x[d], %x[x]\n"); }
K(sqrt_xs)         { VARS LOOP_LAT("xssqrtdp %x[a], %x[a]\nxsadddp %x[a], %x[a], %x[x]\n"); }
K(sqrt_xv)         { VARS LOOP_LAT("xvsqrtdp %x[a], %x[a]\nxvadddp %x[a], %x[a], %x[x]\n"); }
K(sqrt_xs_t)       { VARS LOOP_LAT("xssqrtdp %x[a], %x[a]\nxssqrtdp %x[b], %x[b]\nxssqrtdp %x[c], %x[c]\nxssqrtdp %x[d], %x[d]\n"); }
K(sqrt_xv_t)       { VARS LOOP_LAT("xvsqrtdp %x[a], %x[a]\nxvsqrtdp %x[b], %x[b]\nxvsqrtdp %x[c], %x[c]\nxvsqrtdp %x[d], %x[d]\n"); }
K(sqrt32_xv_t)     { VARS LOOP_LAT("xvsqrtsp %x[a], %x[a]\nxvsqrtsp %x[b], %x[b]\nxvsqrtsp %x[c], %x[c]\nxvsqrtsp %x[d], %x[d]\n"); }
K(sqrt32_xs_t)     { VARS LOOP_LAT("xssqrtsp %x[a], %x[a]\nxssqrtsp %x[b], %x[b]\nxssqrtsp %x[c], %x[c]\nxssqrtsp %x[d], %x[d]\n"); }
K(fma_xs)          { VARS LOOP_LAT("xsmaddadp %x[a], %x[x], %x[y]\n"); }
K(fma_xv)          { VARS LOOP_LAT("xvmaddadp %x[a], %x[x], %x[y]\n"); }
K(mul_xs)          { VARS LOOP_LAT("xsmuldp %x[a], %x[a], %x[x]\n"); }
K(mul_xv)          { VARS LOOP_LAT("xvmuldp %x[a], %x[a], %x[x]\n"); }
/* the fused multiply-add family with the negated operand and post-check */
K(fnmadd_neg_post) { VARS LOOP_LAT("xsnegdp %x[t], %x[x]\nxsmsubadp %x[a], %x[t], %x[y]\nxscmpudp 1, %x[a], %x[a]\nbso 1, 9f\n9:\n"); }
K(fma_copy_post)   { VARS LOOP_LAT("xxlor %x[t], %x[a], %x[a]\nxsmaddadp %x[a], %x[x], %x[y]\nxscmpudp 1, %x[a], %x[a]\nbso 1, 9f\n9:\n"); }
K(fma_pre)         { VARS LOOP_LAT("xscmpudp 1, %x[x], %x[y]\nxscmpudp 5, %x[a], %x[a]\ncror 7, 7, 23\nbso 1, 9f\nxsmaddadp %x[a], %x[x], %x[y]\n9:\n"); }
/* denormal operands: is there a penalty? (a = 1, x = min denormal * 2) */
K(mul_xs_denorm)   { VARS x = (v2d){ 4.9e-324, 4.9e-324 }; a = (v2d){ 0.5, 0.5 }; LOOP_LAT("xsmuldp %x[t], %x[a], %x[x]\nxsadddp %x[a], %x[a], %x[t]\n"); }
K(mul_xs_normal)   { VARS x = (v2d){ 1e-300, 1e-300 }; a = (v2d){ 0.5, 0.5 }; LOOP_LAT("xsmuldp %x[t], %x[a], %x[x]\nxsadddp %x[a], %x[a], %x[t]\n"); }
K(add_xs_denorm_t) { VARS x = (v2d){ 4.9e-324, 4.9e-324 }; a = b = c = d = (v2d){ 4.9e-324, 4.9e-324 }; LOOP_LAT("xsadddp %x[a], %x[a], %x[x]\nxsadddp %x[b], %x[b], %x[x]\nxsadddp %x[c], %x[c], %x[x]\nxsadddp %x[d], %x[d], %x[x]\n"); }
/* garbage in the other lane of a vector op: NaN and denormal */
K(div_xv_nanlane)  { VARS a = (v2d){ 1.0, __builtin_nan("") }; LOOP_LAT("xvdivdp %x[a], %x[a], %x[x]\n"); }
K(div_xv_denlane)  { VARS a = (v2d){ 1.0, 4.9e-324 }; x = (v2d){ 0.999999, 3.0 }; LOOP_LAT("xvdivdp %x[a], %x[a], %x[x]\n"); }

/* ---- group C: FPSCR ---- */
K(mffs)            { VARS LOOP_LAT("mffs %[f0]\n"); }
K(mffs_dep)        { VARS LOOP_LAT("mffs %[f0]\nmfvsrd %[g0], %x[f0]\nxsadddp %x[a], %x[a], %x[x]\n"); }
K(mtfsf_full)      { VARS LOOP_LAT("mtfsf 0xFF, %[f0]\n"); }
K(mtfsf_ctrl)      { VARS LOOP_LAT("mtfsf 1, %[f0]\n"); }
K(mtfsfi_ctrl)     { VARS LOOP_LAT("mtfsfi 7, 0\n"); }
K(mtfsfi_sticky)   { VARS LOOP_LAT("mtfsfi 0, 0\n"); }
K(mtfsb0)          { VARS LOOP_LAT("mtfsb0 30\n"); }
K(mtfsb0_sticky)   { VARS LOOP_LAT("mtfsb0 3\n"); }
#ifdef P9
K(mffsl)           { VARS LOOP_LAT("mffsl %[f0]\n"); }
K(mffscrni)        { VARS LOOP_LAT("mffscrni %[f0], 0\n"); }
K(sandwich_p9)     { VARS LOOP_LAT("mffscrni %[f0], 0\nxsrdpic %x[a], %x[a]\nmffscrn %[f0], %[f0]\nxsadddp %x[a], %x[a], %x[x]\n"); }
K(sandwich_p9_t)   { VARS LOOP_LAT("mffscrni %[f0], 0\nxsrdpic %x[a], %x[a]\nmffscrn %[f0], %[f0]\nmffscrni %[f0], 0\nxsrdpic %x[b], %x[b]\nmffscrn %[f0], %[f0]\nmffscrni %[f0], 0\nxsrdpic %x[c], %x[c]\nmffscrn %[f0], %[f0]\nmffscrni %[f0], 0\nxsrdpic %x[d], %x[d]\nmffscrn %[f0], %[f0]\n"); }
#endif
/* the JIT's tie-even sandwich (A64FloatToGPR case 0) */
K(sandwich_p8)     { VARS LOOP_LAT("mffs %[f0]\nmtfsb0 30\nmtfsb0 31\nxsrdpic %x[a], %x[a]\nmtfsf 1, %[f0]\nxsadddp %x[a], %x[a], %x[x]\n"); }
K(sandwich_p8_t)   { VARS LOOP_LAT("mffs %[f0]\nmtfsb0 30\nmtfsb0 31\nxsrdpic %x[a], %x[a]\nmtfsf 1, %[f0]\nmffs %[f0]\nmtfsb0 30\nmtfsb0 31\nxsrdpic %x[b], %x[b]\nmtfsf 1, %[f0]\nmffs %[f0]\nmtfsb0 30\nmtfsb0 31\nxsrdpic %x[c], %x[c]\nmtfsf 1, %[f0]\nmffs %[f0]\nmtfsb0 30\nmtfsb0 31\nxsrdpic %x[d], %x[d]\nmtfsf 1, %[f0]\n"); }
K(xsrdpic_only)    { VARS LOOP_LAT("xsrdpic %x[a], %x[a]\nxsadddp %x[a], %x[a], %x[x]\n"); }
/* shadow-mode check: load the guest FPCR from memory, compare, branch (never taken) */
K(sandwich_shadow) { VARS LOOP_LAT("lwz %[g0], 0(%[mem])\nrlwinm %[g0], %[g0], 0, 8, 9\ncmplwi 1, %[g0], 0\nbne 1, 9f\nxsrdpic %x[a], %x[a]\n9:\nxsadddp %x[a], %x[a], %x[x]\n"); }
/* the JIT's MSR FPCR path (SetRoundingMode): mffs, mfvsrd, rldicr, or, mtvsrd, mtfsf 0xFF */
K(setrm_jit)       { VARS LOOP_LAT("mffs %[f0]\nmfvsrd %[g0], %x[f0]\nrldicr %[g0], %[g0], 0, 61\nori %[g0], %[g0], 0\nmtvsrd %x[f0], %[g0]\nmtfsf 0xFF, %[f0]\n"); }
/* interference: independent adds with an FPSCR write in the stream */
K(add4_t_plain)    { VARS LOOP_LAT("xsadddp %x[a], %x[a], %x[x]\nxsadddp %x[b], %x[b], %x[x]\nxsadddp %x[c], %x[c], %x[x]\nxsadddp %x[d], %x[d], %x[x]\n"); }
K(add4_t_mtfsf)    { VARS LOOP_LAT("xsadddp %x[a], %x[a], %x[x]\nxsadddp %x[b], %x[b], %x[x]\nmtfsf 0xFF, %[f0]\nxsadddp %x[c], %x[c], %x[x]\nxsadddp %x[d], %x[d], %x[x]\n"); }
K(add4_t_mtfsfctl) { VARS LOOP_LAT("xsadddp %x[a], %x[a], %x[x]\nxsadddp %x[b], %x[b], %x[x]\nmtfsf 1, %[f0]\nxsadddp %x[c], %x[c], %x[x]\nxsadddp %x[d], %x[d], %x[x]\n"); }
K(add4_t_mffs)     { VARS LOOP_LAT("xsadddp %x[a], %x[a], %x[x]\nxsadddp %x[b], %x[b], %x[x]\nmffs %[f0]\nxsadddp %x[c], %x[c], %x[x]\nxsadddp %x[d], %x[d], %x[x]\n"); }
K(add4_t_mtfsb0)   { VARS LOOP_LAT("xsadddp %x[a], %x[a], %x[x]\nxsadddp %x[b], %x[b], %x[x]\nmtfsb0 30\nxsadddp %x[c], %x[c], %x[x]\nxsadddp %x[d], %x[d], %x[x]\n"); }
/* inexact results (XX sticky set on every op) vs exact: does a sticky
 * change alter anything for the reader? (a sticky bit that is already set
 * does not change) */
K(mffs_after_inexact){ VARS LOOP_LAT("xsadddp %x[a], %x[a], %x[x]\nmffs %[f0]\n"); }

/* ---- group D: conversions ---- */
/* FCVTZS x,d as the JIT does it: xxpermdi, xscmpudp, xscvdpsxds, mfvsrd, bc, li; then back via mtvsrd/xscvsxddp */
#define BACK "mtvsrd %x[a], %[g0]\nxscvsxddp %x[a], %x[a]\nxsadddp %x[a], %x[a], %x[y]\n"
K(cvtzs_jit)       { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 3\nxscmpudp 1, %x[t], %x[t]\nxscvdpsxds %x[u], %x[t]\nmfvsrd %[g0], %x[u]\nbns 1, 9f\nli %[g0], 0\n9:\n" BACK); }
K(cvtzs_isel)      { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 3\nxscmpudp 1, %x[t], %x[t]\nxscvdpsxds %x[u], %x[t]\nmfvsrd %[g0], %x[u]\nisel %[g0], 0, %[g0], 7\n" BACK); }
K(cvtzs_mask)      { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 3\nxvcmpeqdp %x[u], %x[t], %x[t]\nxscvdpsxds %x[t], %x[t]\nxxland %x[t], %x[t], %x[u]\nmfvsrd %[g0], %x[t]\n" BACK); }
K(cvtzs_raw)       { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 3\nxscvdpsxds %x[u], %x[t]\nmfvsrd %[g0], %x[u]\n" BACK); }
K(cvtzs_noperm)    { VARS LOOP_LAT("xscmpudp 1, %x[a], %x[a]\nxscvdpsxds %x[u], %x[a]\nmfvsrd %[g0], %x[u]\nisel %[g0], 0, %[g0], 7\n" BACK); }
K(cvtzu_raw)       { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 3\nxscvdpuxds %x[u], %x[t]\nmfvsrd %[g0], %x[u]\n" BACK); }
K(cvtns_p8)        { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 3\nmffs %[f0]\nmtfsb0 30\nmtfsb0 31\nxsrdpic %x[t], %x[t]\nmtfsf 1, %[f0]\nxscmpudp 1, %x[t], %x[t]\nxscvdpsxds %x[u], %x[t]\nmfvsrd %[g0], %x[u]\nisel %[g0], 0, %[g0], 7\n" BACK); }
#ifdef P9
K(cvtns_p9)        { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 3\nmffscrni %[f0], 0\nxsrdpic %x[t], %x[t]\nmffscrn %[f0], %[f0]\nxscmpudp 1, %x[t], %x[t]\nxscvdpsxds %x[u], %x[t]\nmfvsrd %[g0], %x[u]\nisel %[g0], 0, %[g0], 7\n" BACK); }
#endif
K(cvtns_fctid)     { VARS LOOP_LAT("xxpermdi 12, %x[a], %x[a], 3\nfcmpu 1, 12, 12\nfctid 12, 12\nmfvsrd %[g0], 12\nisel %[g0], 0, %[g0], 7\n" BACK); }
K(cvtns_shadow)    { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 3\nlwz %[g1], 0(%[mem])\nrlwinm %[g1], %[g1], 0, 8, 9\ncmplwi 1, %[g1], 0\nbne 1, 8f\n8:\nxsrdpic %x[t], %x[t]\nxscmpudp 1, %x[t], %x[t]\nxscvdpsxds %x[u], %x[t]\nmfvsrd %[g0], %x[u]\nisel %[g0], 0, %[g0], 7\n" BACK); }
K(cvtas_xsrdpi)    { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 3\nxsrdpi %x[t], %x[t]\nxscmpudp 1, %x[t], %x[t]\nxscvdpsxds %x[u], %x[t]\nmfvsrd %[g0], %x[u]\nisel %[g0], 0, %[g0], 7\n" BACK); }
/* SCVTF d,x: the JIT (mtvsrd, xscvsxddp, vspltisw, xxpermdi) vs pinned zero */
#define TOINT "xscvdpsxds %x[t], %x[a]\nmfvsrd %[g0], %x[t]\naddi %[g0], %[g0], 1\n"
K(scvtf_jit)       { VARS LOOP_LAT(TOINT "mtvsrd %x[t], %[g0]\nxscvsxddp %x[t], %x[t]\nvspltisw 31, 0\nxxpermdi %x[a], 63, %x[t], 0\n"); }
K(scvtf_pinzero)   { VARS LOOP_LAT(TOINT "mtvsrd %x[t], %[g0]\nxscvsxddp %x[t], %x[t]\nxxpermdi %x[a], %x[zero], %x[t], 0\n"); }
K(scvtf_w_extsw)   { VARS LOOP_LAT(TOINT "extsw %[g0], %[g0]\nmtvsrd %x[t], %[g0]\nxscvsxddp %x[a], %x[t]\n"); }
K(scvtf_w_mtvsrwa) { VARS LOOP_LAT(TOINT "mtvsrwa %x[t], %[g0]\nxscvsxddp %x[a], %x[t]\n"); }
K(scvtf_stack)     { VARS LOOP_LAT(TOINT "std %[g0], 0(%[mem])\nlfd 12, 0(%[mem])\nfcfid 12, 12\nxxlor %x[a], 12, 12\n"); }
/* FCVT d,s: the JIT (xxsldwi, xscvspdpn, xscmpudp, bc, [const, mtvsrd, vor], vspltisw, xxpermdi) vs xscvspdp + one xxpermdi */
K(fcvt_ds_jit)     { VARS LOOP_LAT("xxsldwi %x[t], %x[a], %x[a], 3\nxscvspdpn %x[t], %x[t]\nxscmpudp 1, %x[t], %x[t]\nbns 1, 9f\nli %[g0], 0\n9:\nvspltisw 31, 0\nxxpermdi %x[a], 63, %x[t], 0\nxscvdpsp %x[a], %x[a]\n"); }
K(fcvt_ds_min)     { VARS LOOP_LAT("xxsldwi %x[t], %x[a], %x[a], 3\nxscvspdp %x[t], %x[t]\nxxpermdi %x[a], %x[zero], %x[t], 0\nxscvdpsp %x[a], %x[a]\n"); }
/* forwarding-restriction pairs from the POWER9 UM 25.1.6.2: count pm_flush */
K(fwd_cvt2fp_direct){ VARS LOOP_LAT("xscvdpsxds %x[t], %x[a]\nxscvsxddp %x[a], %x[t]\nxsadddp %x[a], %x[a], %x[y]\n"); }
K(fwd_cvt2fp_gpr)  { VARS LOOP_LAT("xscvdpsxds %x[t], %x[a]\nmfvsrd %[g0], %x[t]\nmtvsrd %x[t], %[g0]\nxscvsxddp %x[a], %x[t]\nxsadddp %x[a], %x[a], %x[y]\n"); }
K(fwd_sp2dp)       { VARS LOOP_LAT("xscvdpsp %x[t], %x[a]\nxsadddp %x[a], %x[t], %x[y]\n"); }
K(fwd_spn2dp)      { VARS LOOP_LAT("xscvdpspn %x[t], %x[a]\nxscvspdpn %x[t], %x[t]\nxsadddp %x[a], %x[t], %x[y]\n"); }
K(fwd_dp2sp)       { VARS LOOP_LAT("xvadddp %x[t], %x[a], %x[y]\nxvcvdpsp %x[t], %x[t]\nxvcvspdp %x[a], %x[t]\n"); }
K(fwd_int2fp_perm) { VARS LOOP_LAT("xscvdpsxds %x[t], %x[a]\nxxpermdi %x[t], %x[t], %x[t], 0\nxscvsxddp %x[a], %x[t]\nxsadddp %x[a], %x[a], %x[y]\n"); }

/* ---- group E: compare and select ---- */
/* FCMP as the JIT lowers it, then the XER projection a consumer needs (P8: mfxer; P9: mcrxrx), cror, bc */
K(fcmp_jit_p8)     { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 2\nxxpermdi %x[u], %x[x], %x[x], 2\nxscmpudp 0, %x[t], %x[u]\nmfocrf %[g0], 0x80\nrlwinm %[g2], %[g0], 1, 31, 31\nxori %[g2], %[g2], 1\naddic %[g1], %[g2], -1\nrlwinm %[g2], %[g0], 4, 31, 31\nsldi %[g1], %[g2], 62\naddo %[g1], %[g1], %[g1]\n"
                                    "mfxer %[g3]\nrlwinm %[g3], %[g3], 0, 1, 2\nmtocrf 0x40, %[g3]\ncror 28, 1, 2\nbc 12, 28, 9f\n9:\nxsadddp %x[a], %x[a], %x[k]\n"); }
#ifdef P9
K(fcmp_jit_p9)     { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 2\nxxpermdi %x[u], %x[x], %x[x], 2\nxscmpudp 0, %x[t], %x[u]\nmfocrf %[g0], 0x80\nrlwinm %[g2], %[g0], 1, 31, 31\nxori %[g2], %[g2], 1\naddic %[g1], %[g2], -1\nrlwinm %[g2], %[g0], 4, 31, 31\nsldi %[g1], %[g2], 62\naddo %[g1], %[g1], %[g1]\n"
                                    "mcrxrx 1\ncror 28, 1, 2\nbc 12, 28, 9f\n9:\nxsadddp %x[a], %x[a], %x[k]\n"); }
#endif
K(fcmp_direct)     { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 2\nxxpermdi %x[u], %x[x], %x[x], 2\nxscmpudp 0, %x[t], %x[u]\ncror 28, 1, 2\nbc 12, 28, 9f\n9:\nxsadddp %x[a], %x[a], %x[k]\n"); }
K(fcmp_direct_noperm){ VARS LOOP_LAT("xscmpudp 0, %x[a], %x[x]\ncror 28, 1, 2\nbc 12, 28, 9f\n9:\nxsadddp %x[a], %x[a], %x[k]\n"); }
/* the XER lift alone, and the SO speculation question: OV=1 sets SO (sticky) */
K(xerlift_only)    { VARS LOOP_LAT("mfocrf %[g0], 0x80\nrlwinm %[g2], %[g0], 1, 31, 31\nxori %[g2], %[g2], 1\naddic %[g1], %[g2], -1\nrlwinm %[g2], %[g0], 4, 31, 31\nsldi %[g1], %[g2], 62\naddo %[g1], %[g1], %[g1]\n"); }
K(so_toggle)       { VARS LOOP_LAT("li %[g0], 1\nsldi %[g0], %[g0], 62\naddo %[g1], %[g0], %[g0]\nli %[g2], 0\nmtxer %[g2]\n"); }
K(so_set_only)     { VARS LOOP_LAT("li %[g0], 1\nsldi %[g0], %[g0], 62\naddo %[g1], %[g0], %[g0]\n"); }
K(so_clear_only)   { VARS LOOP_LAT("li %[g2], 0\nmtxer %[g2]\n"); }
/* FCSEL: condition from a data-dependent compare of a stream of random doubles */
static v2d rnddata[4096] __attribute__((aligned(65536)));
#define SELPRE "lxvd2x %x[t], 0, %[mem]\naddi %[mem], %[mem], 16\nrldicl %[mem], %[mem], 0, 48\noris %[mem], %[mem], %[hi]\nxscmpudp 0, %x[t], %x[z]\n"
#define LOOP_SEL(body) __asm__ volatile( \
  "mtctr %[n]\n1:\n" body "bdnz 1b\n" \
  : [a]"+wa"(a), [b]"+wa"(b), [c]"+wa"(c), [d]"+wa"(d), [g0]"=&r"(g0), [g1]"=&r"(g1), [t]"=&wa"(t), [u]"=&wa"(u), [mem]"+r"(mem) \
  : [x]"wa"(x), [y]"wa"(y), [z]"wa"(z), [n]"r"(n), [hi]"i"(0), [zero]"wa"(zero) : "ctr", "cr0", "cr1", "xer", "memory")
static void selinit(void) { uint64_t s = 88172645463325252ull; for (int i = 0; i < 4096; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; double v = (double)(s >> 11) * (1.0 / 9007199254740992.0); rnddata[i] = (v2d){ v, v }; } }
K(fcsel_branch)    { VARS selinit(); uint64_t* base = (uint64_t*)rnddata; mem = base; (void)base;
                     __asm__ volatile("mtctr %[n]\n1:\nlxvd2x %x[t], 0, %[m]\naddi %[m], %[m], 16\nandi. %[g0], %[m], 0xFFF0\nrlwinm %[g0], %[m], 0, 16, 27\nor %[m], %[base], %[g0]\n"
                       "xscmpudp 0, %x[t], %x[z]\nvor %[a], %[x], %[x]\nbc 4, 1, 9f\nvor %[a], %[y], %[y]\n9:\nxsadddp %x[b], %x[b], %x[a]\nbdnz 1b\n"
                       : [a]"+v"(a), [b]"+wa"(b), [t]"=&wa"(t), [g0]"=&r"(g0), [m]"+&r"(mem) : [x]"v"(x), [y]"v"(y), [z]"wa"(z), [n]"r"(n), [base]"r"((uint64_t)base & ~0xFFFFull) : "ctr", "cr0", "memory"); }
K(fcsel_fused)     { VARS selinit(); uint64_t* base = (uint64_t*)rnddata; mem = base; (void)base;
                     __asm__ volatile("mtctr %[n]\n1:\nlxvd2x %x[t], 0, %[m]\naddi %[m], %[m], 16\nrlwinm %[g0], %[m], 0, 16, 27\nor %[m], %[base], %[g0]\n"
                       "xvcmpgtdp %x[u], %x[t], %x[z]\nxxsel %x[a], %x[y], %x[x], %x[u]\nxsadddp %x[b], %x[b], %x[a]\nbdnz 1b\n"
                       : [a]"+wa"(a), [b]"+wa"(b), [t]"=&wa"(t), [u]"=&wa"(u), [g0]"=&r"(g0), [m]"+&r"(mem) : [x]"wa"(x), [y]"wa"(y), [z]"wa"(z), [n]"r"(n), [base]"r"((uint64_t)base & ~0xFFFFull) : "ctr", "cr0", "memory"); }
K(fcsel_mfocrf)    { VARS selinit(); uint64_t* base = (uint64_t*)rnddata; mem = base; (void)base;
                     __asm__ volatile("mtctr %[n]\n1:\nlxvd2x %x[t], 0, %[m]\naddi %[m], %[m], 16\nrlwinm %[g0], %[m], 0, 16, 27\nor %[m], %[base], %[g0]\n"
                       "xscmpudp 0, %x[t], %x[z]\nmfocrf %[g0], 0x80\nrlwinm %[g0], %[g0], 2, 31, 31\nneg %[g0], %[g0]\nmtvsrd %x[u], %[g0]\nxxpermdi %x[u], %x[u], %x[u], 0\nxxsel %x[a], %x[y], %x[x], %x[u]\nxsadddp %x[b], %x[b], %x[a]\nbdnz 1b\n"
                       : [a]"+wa"(a), [b]"+wa"(b), [t]"=&wa"(t), [u]"=&wa"(u), [g0]"=&r"(g0), [m]"+&r"(mem) : [x]"wa"(x), [y]"wa"(y), [z]"wa"(z), [n]"r"(n), [base]"r"((uint64_t)base & ~0xFFFFull) : "ctr", "cr0", "memory"); }
#ifdef P9
K(fcsel_setb)      { VARS selinit(); uint64_t* base = (uint64_t*)rnddata; mem = base; (void)base;
                     __asm__ volatile("mtctr %[n]\n1:\nlxvd2x %x[t], 0, %[m]\naddi %[m], %[m], 16\nrlwinm %[g0], %[m], 0, 16, 27\nor %[m], %[base], %[g0]\n"
                       "xscmpudp 0, %x[t], %x[z]\nsetb %[g0], 0\nmtvsrdd %x[u], %[g0], %[g0]\nxxsel %x[a], %x[y], %x[x], %x[u]\nxsadddp %x[b], %x[b], %x[a]\nbdnz 1b\n"
                       : [a]"+wa"(a), [b]"+wa"(b), [t]"=&wa"(t), [u]"=&wa"(u), [g0]"=&r"(g0), [m]"+&r"(mem) : [x]"wa"(x), [y]"wa"(y), [z]"wa"(z), [n]"r"(n), [base]"r"((uint64_t)base & ~0xFFFFull) : "ctr", "cr0", "memory"); }
#endif
K(fcsel_predictable){ VARS selinit(); uint64_t* base = (uint64_t*)rnddata; mem = base; (void)base;
                     __asm__ volatile("mtctr %[n]\n1:\nlxvd2x %x[t], 0, %[m]\naddi %[m], %[m], 16\nrlwinm %[g0], %[m], 0, 16, 27\nor %[m], %[base], %[g0]\n"
                       "xscmpudp 0, %x[t], %x[y]\nvor %[a], %[x], %[x]\nbc 4, 1, 9f\nvor %[a], %[y], %[y]\n9:\nxsadddp %x[b], %x[b], %x[a]\nbdnz 1b\n"
                       : [a]"+v"(a), [b]"+wa"(b), [t]"=&wa"(t), [g0]"=&r"(g0), [m]"+&r"(mem) : [x]"v"(x), [y]"v"(y), [z]"wa"(z), [n]"r"(n), [base]"r"((uint64_t)base & ~0xFFFFull) : "ctr", "cr0", "memory"); }

/* ---- group F: register file and moves ---- */
K(add_fpr)         { VARS LOOP_LAT("fadd 12, 12, %[f0]\n"); }
K(add_fpr_t)       { VARS LOOP_LAT("fadd 12, 12, %[f0]\nfadd 13, 13, %[f0]\nfadd 14, 14, %[f0]\nfadd 15, 15, %[f0]\n"); }
K(add_vsxlo)       { VARS LOOP_LAT("xsadddp 12, 12, %x[x]\n"); }
K(add_vsxlo_t)     { VARS LOOP_LAT("xsadddp 12, 12, %x[x]\nxsadddp 13, 13, %x[x]\nxsadddp 14, 14, %x[x]\nxsadddp 15, 15, %x[x]\n"); }
K(add_vsxhi)       { VARS LOOP_LAT("xsadddp 40, 40, %x[x]\n"); }
K(add_vsxhi_t)     { VARS LOOP_LAT("xsadddp 40, 40, %x[x]\nxsadddp 41, 41, %x[x]\nxsadddp 42, 42, %x[x]\nxsadddp 43, 43, %x[x]\n"); }
K(add_cross_halves){ VARS LOOP_LAT("xsadddp 12, 40, %x[x]\nxsadddp 40, 12, %x[x]\n"); }
K(mov_hi_lo_lat)   { VARS LOOP_LAT("xxlor 12, 40, 40\nxxlor 40, 12, 12\n"); }
K(mtvsrd_mfvsrd)   { VARS LOOP_LAT("mtvsrd %x[t], %[g0]\nmfvsrd %[g0], %x[t]\naddi %[g0], %[g0], 1\n"); }
K(std_lfd)         { VARS LOOP_LAT("std %[g0], 0(%[mem])\nlfd 12, 0(%[mem])\nmfvsrd %[g0], 12\naddi %[g0], %[g0], 1\n"); }
K(stfd_ld)         { VARS LOOP_LAT("stfd 12, 0(%[mem])\nld %[g0], 0(%[mem])\naddi %[g0], %[g0], 1\nmtvsrd 12, %[g0]\n"); }
K(stfs_lwz)        { VARS LOOP_LAT("stfs 12, 0(%[mem])\nlwz %[g0], 0(%[mem])\naddi %[g0], %[g0], 1\nmtvsrd 12, %[g0]\n"); }
K(mtvsrd_xsadd)    { VARS LOOP_LAT("mtvsrd %x[t], %[g0]\nxsadddp %x[t], %x[t], %x[x]\nmfvsrd %[g0], %x[t]\n"); }
K(mtvsrd_xvadd)    { VARS LOOP_LAT("mtvsrd %x[t], %[g0]\nxvadddp %x[t], %x[t], %x[x]\nmfvsrd %[g0], %x[t]\n"); }
K(xxpermdi_lat)    { VARS LOOP_LAT("xxpermdi %x[a], %x[a], %x[a], 2\n"); }
K(xxsldwi_lat)     { VARS LOOP_LAT("xxsldwi %x[a], %x[a], %x[a], 3\n"); }
K(vsldoi_lat)      { VARS LOOP_LAT("vsldoi %[a], %[a], %[a], 8\n"); }
K(xxlor_lat)       { VARS LOOP_LAT("xxlor %x[a], %x[a], %x[x]\n"); }
K(perm_add_perm)   { VARS LOOP_LAT("xxpermdi %x[t], %x[a], %x[a], 2\nxsadddp %x[t], %x[t], %x[x]\nxxpermdi %x[a], %x[zero], %x[t], 0\n"); }
K(nop_loop)        { VARS LOOP_LAT("nop\n"); }
K(addi_lat)        { VARS LOOP_LAT("addi %[g0], %[g0], 1\n"); }

typedef struct { const char* name; void (*fn)(long, int); } kern_t;
#define E(n) { #n, k_##n }
static const kern_t kernels[] = {
  E(add_xs), E(add_xs_t), E(add_xv), E(add_xv_t), E(add_xv_jitzero), E(add_xv_jitzero_t), E(add_xv_permzero), E(add_xv_permzero_t),
  E(add_pre), E(add_pre_t), E(add_post), E(add_post_t), E(add_bffix), E(add_bffix_t),
  E(add32_xv), E(add32_pre), E(add32_pre_t), E(add32_post), E(add32_cr6),
  E(div_xs), E(div_xs_t), E(div_xv), E(div_xv_t), E(div_xv_perm), E(div32_xv), E(div32_xv_t), E(div32_xs), E(div32_xs_t),
  E(sqrt_xs), E(sqrt_xv), E(sqrt_xs_t), E(sqrt_xv_t), E(sqrt32_xv_t), E(sqrt32_xs_t), E(fma_xs), E(fma_xv), E(mul_xs), E(mul_xv),
  E(fnmadd_neg_post), E(fma_copy_post), E(fma_pre), E(mul_xs_denorm), E(mul_xs_normal), E(add_xs_denorm_t), E(div_xv_nanlane), E(div_xv_denlane),
  E(mffs), E(mffs_dep), E(mtfsf_full), E(mtfsf_ctrl), E(mtfsfi_ctrl), E(mtfsfi_sticky), E(mtfsb0), E(mtfsb0_sticky),
#ifdef P9
  E(mffsl), E(mffscrni), E(sandwich_p9), E(sandwich_p9_t),
#endif
  E(sandwich_p8), E(sandwich_p8_t), E(xsrdpic_only), E(sandwich_shadow), E(setrm_jit),
  E(add4_t_plain), E(add4_t_mtfsf), E(add4_t_mtfsfctl), E(add4_t_mffs), E(add4_t_mtfsb0), E(mffs_after_inexact),
  E(cvtzs_jit), E(cvtzs_isel), E(cvtzs_mask), E(cvtzs_raw), E(cvtzs_noperm), E(cvtzu_raw), E(cvtns_p8),
#ifdef P9
  E(cvtns_p9),
#endif
  E(cvtns_fctid), E(cvtns_shadow), E(cvtas_xsrdpi), E(scvtf_jit), E(scvtf_pinzero), E(scvtf_w_extsw), E(scvtf_w_mtvsrwa), E(scvtf_stack),
  E(fcvt_ds_jit), E(fcvt_ds_min), E(fwd_cvt2fp_direct), E(fwd_cvt2fp_gpr), E(fwd_sp2dp), E(fwd_spn2dp), E(fwd_dp2sp), E(fwd_int2fp_perm),
  E(fcmp_jit_p8),
#ifdef P9
  E(fcmp_jit_p9),
#endif
  E(fcmp_direct), E(fcmp_direct_noperm), E(xerlift_only), E(so_toggle), E(so_set_only), E(so_clear_only),
  E(fcsel_branch), E(fcsel_fused), E(fcsel_mfocrf),
#ifdef P9
  E(fcsel_setb),
#endif
  E(fcsel_predictable),
  E(add_fpr), E(add_fpr_t), E(add_vsxlo), E(add_vsxlo_t), E(add_vsxhi), E(add_vsxhi_t), E(add_cross_halves), E(mov_hi_lo_lat),
  E(mtvsrd_mfvsrd), E(std_lfd), E(stfd_ld), E(stfs_lwz), E(mtvsrd_xsadd), E(mtvsrd_xvadd), E(xxpermdi_lat), E(xxsldwi_lat), E(vsldoi_lat), E(xxlor_lat),
  E(perm_add_perm), E(nop_loop), E(addi_lat),
};

int main(int argc, char** argv) {
  if (argc < 2 || !strcmp(argv[1], "list")) { for (unsigned i = 0; i < sizeof kernels / sizeof kernels[0]; i++) printf("%s\n", kernels[i].name); return 0; }
  const kern_t* k = NULL;
  for (unsigned i = 0; i < sizeof kernels / sizeof kernels[0]; i++) if (!strcmp(kernels[i].name, argv[1])) k = &kernels[i];
  if (!k) { fprintf(stderr, "unknown kernel %s\n", argv[1]); return 2; }
  long ops = argc > 3 ? atol(argv[3]) : 20000000; /* ops per chain */
  long iters = ops / UNROLL;
  k->fn(iters / 8, 0); /* warm-up */
  uint64_t best = ~0ull;
  for (int r = 0; r < 5; r++) { uint64_t t0 = now_ns(); k->fn(iters, 0); uint64_t t1 = now_ns(); if (t1 - t0 < best) best = t1 - t0; }
  printf("%s iters=%ld unroll=%d ns_per_iter=%.3f\n", k->name, iters, UNROLL, (double)best / iters);
  return 0;
}
