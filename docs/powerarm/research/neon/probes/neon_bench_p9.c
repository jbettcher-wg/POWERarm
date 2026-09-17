/* neon_bench_p9.c -- latency and throughput of POWER instruction sequences
 * used by the NEON lowering catalogue, on POWER9 (and any later core).
 *
 * Every sequence is one asm block with symbolic operands: [a] the accumulator
 * (in/out), [b] [c] inputs, [t1] [t2] [t3] vector temporaries, [g] a GPR
 * temporary, [p] a pointer to 64 bytes of readable/writable scratch.
 *   latency    : one chain, the block repeated back to back on [a]
 *   throughput : four independent chains interleaved
 * Time comes from the time base (mftb, 512 MHz) and is converted to core
 * ns; core cycles use the cpufreq clock read after a warm-up.  A dependent
 * `add` chain is measured the same way as the reference point (it comes out
 * as 2 core cycles on POWER9).
 *
 * Build: gcc -O2 -mcpu=power9 -DP9=1 -o neon_bench_p9 neon_bench_p9.c   (P9=0 for the POWER8 subset)
 * Run pinned: taskset -c 108 ./neon_bench_p9
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#ifndef P9
#define P9 0
#endif
typedef unsigned char v16 __attribute__((vector_size(16)));
static inline uint64_t tb(void) { uint64_t t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
#define ITER 400000
static unsigned char scratch[128] __attribute__((aligned(64)));
static double tb_per_add; /* time-base ticks per dependent add */
static double ghz; /* core clock read from cpufreq while running */
static double read_ghz(void) { FILE* f = fopen("/sys/devices/system/cpu/cpu108/cpufreq/scaling_cur_freq", "r"); if (!f) return 0; double k = 0; if (fscanf(f, "%lf", &k) != 1) k = 0; fclose(f); return k / 1e6; }

#define OPS(acc) [a] "+v"(acc), [t1] "=&v"(t1), [t2] "=&v"(t2), [t3] "=&v"(t3), [g] "=&r"(g)
#define INS [b] "v"(b), [c] "v"(c), [p] "r"(p), [z] "v"(z)
#define BENCH(name, ninsn, str) \
static double lat_##name(void) { v16 a = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}, b = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}, c = {0}, z = {0}, t1, t2, t3; uint64_t g; unsigned char* p = scratch; \
  uint64_t t0 = tb(); for (int i = 0; i < ITER; i++) { __asm__ volatile(str : OPS(a) : INS : "cr6", "memory"); __asm__ volatile(str : OPS(a) : INS : "cr6", "memory"); __asm__ volatile(str : OPS(a) : INS : "cr6", "memory"); __asm__ volatile(str : OPS(a) : INS : "cr6", "memory"); } \
  uint64_t t1_ = tb(); __asm__ volatile("" :: "v"(a)); return (double)(t1_ - t0) / (4.0 * ITER) / tb_per_add; } \
static double tp_##name(void) { v16 a0 = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}, a1 = a0, a2 = a0, a3 = a0, b = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}, c = {0}, z = {0}, t1, t2, t3; uint64_t g; unsigned char* p = scratch; \
  uint64_t t0 = tb(); for (int i = 0; i < ITER; i++) { __asm__ volatile(str : OPS(a0) : INS : "cr6", "memory"); __asm__ volatile(str : OPS(a1) : INS : "cr6", "memory"); __asm__ volatile(str : OPS(a2) : INS : "cr6", "memory"); __asm__ volatile(str : OPS(a3) : INS : "cr6", "memory"); } \
  uint64_t t1_ = tb(); __asm__ volatile("" :: "v"(a0), "v"(a1), "v"(a2), "v"(a3)); return (double)(t1_ - t0) / (4.0 * ITER) / tb_per_add; } \
static const struct benchent name##_ent = { #name, ninsn, lat_##name, tp_##name };
struct benchent { const char* name; int ninsn; double (*lat)(void); double (*tp)(void); };

/* ---- single instructions: VMX vs VSX forms, pipelines ---- */
BENCH(vaddubm, 1, "vaddubm %[a],%[a],%[b]")
BENCH(vand, 1, "vand %[a],%[a],%[b]")
BENCH(xxland, 1, "xxland %x[a],%x[a],%x[b]")
BENCH(vor, 1, "vor %[a],%[a],%[b]")
BENCH(xxlor, 1, "xxlor %x[a],%x[a],%x[b]")
BENCH(vsel, 1, "vsel %[a],%[a],%[b],%[c]")
BENCH(xxsel, 1, "xxsel %x[a],%x[a],%x[b],%x[c]")
BENCH(vperm, 1, "vperm %[a],%[a],%[b],%[c]")
BENCH(vsldoi, 1, "vsldoi %[a],%[a],%[b],4")
BENCH(xxpermdi, 1, "xxpermdi %x[a],%x[a],%x[b],1")
BENCH(vpkuhum, 1, "vpkuhum %[a],%[a],%[b]")
BENCH(vmrglb, 1, "vmrglb %[a],%[a],%[b]")
BENCH(vspltb, 1, "vspltb %[a],%[a],3")
BENCH(vmaxub, 1, "vmaxub %[a],%[a],%[b]")
BENCH(vcmpequb, 1, "vcmpequb %[a],%[a],%[b]")
BENCH(vcmpgtub, 1, "vcmpgtub %[a],%[a],%[b]")
BENCH(vaddubs_sat, 1, "vaddubs %[a],%[a],%[b]")   /* writes VSCR.SAT */
BENCH(vslb, 1, "vslb %[a],%[a],%[b]")
BENCH(vsrab, 1, "vsrab %[a],%[a],%[b]")
BENCH(vsld, 1, "vsld %[a],%[a],%[b]")
BENCH(vrlw, 1, "vrlw %[a],%[a],%[b]")
BENCH(vmladduhm, 1, "vmladduhm %[a],%[a],%[b],%[c]")
BENCH(vmuluwm, 1, "vmuluwm %[a],%[a],%[b]")
BENCH(vmulesb, 1, "vmulesb %[a],%[a],%[b]")
BENCH(vmsumubm, 1, "vmsumubm %[a],%[a],%[b],%[c]")
BENCH(vmhraddshs, 1, "vmhraddshs %[a],%[a],%[b],%[c]")
BENCH(vsum4ubs, 1, "vsum4ubs %[a],%[a],%[b]")
BENCH(vsumsws, 1, "vsumsws %[a],%[a],%[b]")
BENCH(vpopcntb, 1, "vpopcntb %[a],%[a]")
BENCH(vclzb, 1, "vclzb %[a],%[a]")
BENCH(vbpermq, 1, "vbpermq %[a],%[a],%[b]")
BENCH(vpmsumd, 1, "vpmsumd %[a],%[a],%[b]")
BENCH(vpmsumb, 1, "vpmsumb %[a],%[a],%[b]")
BENCH(vcipher, 1, "vcipher %[a],%[a],%[b]")
BENCH(vcipherlast, 1, "vcipherlast %[a],%[a],%[b]")
BENCH(vncipher, 1, "vncipher %[a],%[a],%[b]")
BENCH(vshasigmaw, 1, "vshasigmaw %[a],%[a],1,15")
BENCH(xvaddsp, 1, "xvaddsp %x[a],%x[a],%x[b]")
BENCH(xvmulsp, 1, "xvmulsp %x[a],%x[a],%x[b]")
BENCH(xvmaddasp, 1, "xvmaddasp %x[a],%x[b],%x[c]")
BENCH(xvcmpeqsp, 1, "xvcmpeqsp %x[a],%x[a],%x[b]")
BENCH(xvcmpeqsp_rc, 1, "xvcmpeqsp. %x[a],%x[a],%x[b]")
BENCH(xvmaxsp, 1, "xvmaxsp %x[a],%x[a],%x[b]")
BENCH(xvcvspsxws, 1, "xvcvspsxws %x[a],%x[a]")
BENCH(xvcvsxwsp, 1, "xvcvsxwsp %x[a],%x[a]")
BENCH(xvrspi, 1, "xvrspi %x[a],%x[a]")
BENCH(vrfin, 1, "vrfin %[a],%[a]")
BENCH(xvresp, 1, "xvresp %x[a],%x[a]")
BENCH(xvdivsp, 1, "xvdivsp %x[a],%x[a],%x[b]")
BENCH(xvsqrtsp, 1, "xvsqrtsp %x[a],%x[a]")
BENCH(xvdivdp, 1, "xvdivdp %x[a],%x[a],%x[b]")
BENCH(xvnegsp, 1, "xvnegsp %x[a],%x[a]")
/* VSU <-> GPR crossings */
BENCH(mfvsrd, 1, "mfvsrd %[g],%x[a]")
BENCH(mfvsrd_mtvsrd, 2, "mfvsrd %[g],%x[a]\n\tmtvsrd %x[a],%[g]")
BENCH(mtvsrd_xxpermdi_vspltb, 3, "mfvsrd %[g],%x[a]\n\tmtvsrd %x[t1],%[g]\n\txxpermdi %x[t1],%x[t1],%x[t1],0\n\tvspltb %[a],%[t1],15")
BENCH(vextract_umov_b_p8, 3, "vspltb %[t1],%[a],15\n\tmfvsrd %[g],%x[t1]\n\tsrdi %[g],%[g],56\n\tmtvsrd %x[a],%[g]")
/* constants */
BENCH(vspltisb, 1, "vspltisb %[t1],7\n\tvaddubm %[a],%[a],%[t1]")
BENCH(const_gpr_splat, 4, "li %[g],0x5A\n\tmtvsrd %x[t1],%[g]\n\txxpermdi %x[t1],%x[t1],%x[t1],0\n\tvspltb %[t1],%[t1],15\n\tvaddubm %[a],%[a],%[t1]")
BENCH(const_pool_lvx, 2, "li %[g],32\n\tlvx %[t1],%[p],%[g]\n\tvaddubm %[a],%[a],%[t1]")
BENCH(const_pool_lxvd2x, 3, "li %[g],32\n\tlxvd2x %x[t1],%[p],%[g]\n\txxpermdi %x[t1],%x[t1],%x[t1],2\n\tvaddubm %[a],%[a],%[t1]")
BENCH(const_lvsl_vslb, 3, "li %[g],0\n\tlvsl %[t1],0,%[g]\n\tvspltisb %[t2],3\n\tvslb %[t1],%[t1],%[t2]\n\tvaddubm %[a],%[a],%[t1]")
/* stores and forwarding: 64-bit scalar store then 128-bit vector load (D-then-Q view) and the reverse */
BENCH(stvx_lvx_fwd, 2, "stvx %[a],0,%[p]\n\tlvx %[a],0,%[p]")
BENCH(std_lvx_partial, 3, "mfvsrd %[g],%x[a]\n\tstd %[g],0(%[p])\n\tlvx %[a],0,%[p]")
BENCH(std_std_lvx, 4, "mfvsrd %[g],%x[a]\n\tstd %[g],0(%[p])\n\tstd %[g],8(%[p])\n\tlvx %[a],0,%[p]")
BENCH(stvx_ld_mtvsrd, 3, "stvx %[a],0,%[p]\n\tld %[g],0(%[p])\n\tmtvsrd %x[a],%[g]")
BENCH(ctx_roundtrip_stxvd2x_lxvd2x, 2, "stxvd2x %x[a],0,%[p]\n\tlxvd2x %x[a],0,%[p]")
/* VSCR */
BENCH(mfvscr, 1, "mfvscr %[a]")
BENCH(vaddubs_mfvscr, 2, "vaddubs %[a],%[a],%[b]\n\tmfvscr %[t1]\n\tvor %[a],%[a],%[t1]")
BENCH(mtvscr, 1, "mtvscr %[b]\n\tvaddubm %[a],%[a],%[b]")
/* record-form compare + CR6 branch (the early-exit idiom) */
BENCH(vcmpequb_rc_bc, 2, "vcmpequb. %[t1],%[a],%[z]\n\tbc 12,26,1f\n\tvaddubm %[a],%[a],%[b]\n1:")
BENCH(vcmpequb_rc_mfocrf, 4, "vcmpequb. %[t1],%[a],%[z]\n\tmfocrf %[g],2\n\trlwinm %[g],%[g],25,31,31\n\tmtvsrd %x[a],%[g]")
BENCH(movemask_bpermq, 3, "lvsl %[t1],0,%[p]\n\tvslb %[t1],%[t1],%[c]\n\tvbpermq %[t1],%[a],%[t1]\n\tmfvsrd %[g],%x[t1]\n\tmtvsrd %x[a],%[g]")
/* ---- catalogue sequences ---- */
BENCH(cmhi_1, 1, "vcmpgtub %[a],%[a],%[b]")
BENCH(cmhi_cur3, 3, "vmaxub %[t1],%[a],%[b]\n\tvcmpequb %[t1],%[t1],%[b]\n\tvnor %[a],%[t1],%[t1]")
BENCH(cmtst_2, 3, "vand %[t1],%[a],%[b]\n\tvspltisw %[t2],0\n\tvcmpgtub %[a],%[t1],%[t2]")
BENCH(cmtst_cur, 4, "vand %[t1],%[a],%[b]\n\tvspltisw %[t2],0\n\tvcmpequb %[t1],%[t1],%[t2]\n\tvnor %[a],%[t1],%[t1]")
BENCH(vmov64_cur3, 3, "vspltisw %[t1],0\n\tvsldoi %[t2],%[a],%[t1],8\n\tvsldoi %[a],%[t1],%[t2],8")
BENCH(vmov64_xxpermdi, 2, "vspltisw %[t1],0\n\txxpermdi %x[a],%x[t1],%x[a],1")
BENCH(umaxp_cur6, 6, "vpkuhum %[t1],%[a],%[a]\n\tvspltisw %[t2],0\n\tvsldoi %[t3],%[t2],%[a],15\n\tvsldoi %[t2],%[t2],%[a],15\n\tvpkuhum %[t2],%[t3],%[t2]\n\tvmaxub %[a],%[t1],%[t2]")
BENCH(umaxp_same4, 4, "vspltish %[t1],8\n\tvsrh %[t2],%[a],%[t1]\n\tvmaxub %[t2],%[a],%[t2]\n\tvpkuhum %[a],%[t2],%[t2]")
BENCH(umaxv16_fold10, 10, "vsldoi %[t1],%[a],%[a],8\n\tvmaxub %[a],%[a],%[t1]\n\tvsldoi %[t1],%[a],%[a],4\n\tvmaxub %[a],%[a],%[t1]\n\tvsldoi %[t1],%[a],%[a],2\n\tvmaxub %[a],%[a],%[t1]\n\tvsldoi %[t1],%[a],%[a],1\n\tvmaxub %[a],%[a],%[t1]\n\tvspltisw %[t2],0\n\tvsldoi %[a],%[t2],%[a],1")
BENCH(addv16_3, 3, "vspltisw %[t1],0\n\tvsum4ubs %[t2],%[a],%[t1]\n\tvsumsws %[a],%[t2],%[t1]")
BENCH(addv32_rot4, 4, "vsldoi %[t1],%[a],%[a],8\n\tvadduwm %[a],%[a],%[t1]\n\tvsldoi %[t1],%[a],%[a],4\n\tvadduwm %[a],%[a],%[t1]")
BENCH(shrn_cur4, 4, "vspltish %[t1],4\n\tvsrh %[t2],%[a],%[t1]\n\tvspltisw %[t1],0\n\tvpkuhum %[a],%[t1],%[t2]")
BENCH(srshr_imm5, 5, "vspltisb %[t1],3\n\tvsrab %[t2],%[a],%[t1]\n\tvspltisb %[t1],2\n\tvsrab %[t1],%[a],%[t1]\n\tvspltisb %[t3],1\n\tvand %[t1],%[t1],%[t3]\n\tvaddubm %[a],%[t2],%[t1]")
BENCH(sqshl_imm8, 8, "vspltisb %[t1],3\n\tvslb %[t2],%[a],%[t1]\n\tvsrab %[t3],%[t2],%[t1]\n\tvcmpequb %[t3],%[t3],%[a]\n\tvspltisb %[t1],7\n\tvsrab %[t1],%[a],%[t1]\n\tvxor %[t1],%[t1],%[c]\n\tvsel %[a],%[t1],%[t2],%[t3]")
BENCH(sshl_reg13, 13, "vspltisw %[t3],0\n\tvsububm %[t1],%[t3],%[b]\n\tvslb %[t2],%[a],%[b]\n\tvspltisb %[t3],7\n\tvminub %[t1],%[t1],%[t3]\n\tvsrab %[t1],%[a],%[t1]\n\tvspltisb %[t3],8\n\tvcmpgtub %[t3],%[t3],%[b]\n\tvand %[t2],%[t2],%[t3]\n\tvspltisw %[t3],0\n\tvcmpgtsb %[t3],%[t3],%[b]\n\tvsel %[a],%[t2],%[t1],%[t3]")
BENCH(tbl1_p8_6, 6, "vspltisb %[t2],15\n\tvcmpgtub %[t2],%[b],%[t2]\n\tvspltisb %[t1],15\n\tvxor %[t1],%[b],%[t1]\n\tvperm %[a],%[a],%[a],%[t1]\n\tvandc %[a],%[a],%[t2]")
BENCH(uqadd64_3, 3, "vaddudm %[t1],%[a],%[b]\n\tvcmpgtud %[t2],%[a],%[t1]\n\tvor %[a],%[t1],%[t2]")
BENCH(sqadd64_9, 9, "vaddudm %[t1],%[a],%[b]\n\tvxor %[t2],%[a],%[b]\n\tvxor %[t3],%[a],%[t1]\n\tvandc %[t2],%[t3],%[t2]\n\tvspltisb %[t3],-1\n\tvsrad %[t2],%[t2],%[t3]\n\tvsrad %[t3],%[a],%[t3]\n\tvxor %[t3],%[t3],%[c]\n\tvsel %[a],%[t1],%[t3],%[t2]")
BENCH(sqdmulh16_2, 2, "vspltisw %[t1],0\n\tvmhaddshs %[a],%[a],%[b],%[t1]")
BENCH(sqdmulh32_9, 9, "vmulesw %[t1],%[a],%[b]\n\tvmulosw %[t2],%[a],%[b]\n\tvspltisb %[t3],-1\n\tvsrad %[t1],%[t1],%[t3]\n\tvsrad %[t2],%[t2],%[t3]\n\tvmrgow %[t1],%[t1],%[t2]\n\tvcmpequw %[t2],%[a],%[c]\n\tvcmpequw %[t3],%[b],%[c]\n\tvand %[t2],%[t2],%[t3]\n\tvxor %[a],%[t1],%[t2]")
BENCH(smull8_3, 3, "vmulesb %[t1],%[a],%[b]\n\tvmulosb %[t2],%[a],%[b]\n\tvmrglh %[a],%[t1],%[t2]")
BENCH(mul8_5, 5, "vmulesb %[t1],%[a],%[b]\n\tvmulosb %[t2],%[a],%[b]\n\tvspltish %[t3],8\n\tvslh %[t1],%[t1],%[t3]\n\tvsel %[a],%[t1],%[t2],%[c]")
BENCH(mul64_8, 8, "vrld %[t1],%[a],%[c]\n\tvrld %[t2],%[b],%[c]\n\tvmulouw %[t3],%[a],%[b]\n\tvmulouw %[t1],%[t1],%[b]\n\tvmulouw %[t2],%[a],%[t2]\n\tvaddudm %[t1],%[t1],%[t2]\n\tvsld %[t1],%[t1],%[c]\n\tvaddudm %[a],%[t3],%[t1]")
BENCH(uabd_p8_3, 3, "vmaxub %[t1],%[a],%[b]\n\tvminub %[t2],%[a],%[b]\n\tvsububm %[a],%[t1],%[t2]")
BENCH(udot_1, 1, "vmsumubm %[a],%[a],%[b],%[a]")
BENCH(sdot_6, 6, "vxor %[t1],%[b],%[c]\n\tvmsummbm %[t2],%[a],%[t1],%[a]\n\tvspltisb %[t3],1\n\tvmsummbm %[t3],%[a],%[t3],%[z]\n\tvspltisb %[t1],7\n\tvslw %[t3],%[t3],%[t1]\n\tvsubuwm %[a],%[t2],%[t3]")
BENCH(pmull_2, 2, "xxmrgld %x[t1],%x[z],%x[a]\n\tvpmsumd %[a],%[t1],%[b]")
BENCH(aese_3, 3, "vxor %[t1],%[a],%[b]\n\tvcipherlast %[t1],%[t1],%[z]\n\tvsldoi %[a],%[t1],%[t1],4")
BENCH(aes_round_fused_2, 2, "vxor %[t1],%[a],%[b]\n\tvcipher %[a],%[t1],%[z]")
BENCH(fmla_gated_3, 3, "xvmaddasp %x[a],%x[b],%x[c]\n\txvcmpeqsp. %x[t1],%x[a],%x[a]\n\tbc 12,24,1f\n\txvnegsp %x[a],%x[a]\n1:")
BENCH(fcvtzs_3, 3, "xvcvspsxws %x[t1],%x[a]\n\txvcmpeqsp %x[t2],%x[a],%x[a]\n\txxland %x[a],%x[t1],%x[t2]")
BENCH(rbit_8, 8, "vspltisb %[t1],4\n\tvsrb %[t2],%[a],%[t1]\n\tvand %[t3],%[a],%[c]\n\tvperm %[t2],%[b],%[b],%[t2]\n\tvperm %[t3],%[b],%[b],%[t3]\n\tvslb %[t3],%[t3],%[t1]\n\tvor %[a],%[t3],%[t2]")
BENCH(zip1_1, 1, "vmrglb %[a],%[b],%[a]")
BENCH(uzp2_3, 3, "vspltish %[t1],8\n\tvsrh %[t2],%[a],%[t1]\n\tvsrh %[t3],%[b],%[t1]\n\tvpkuhum %[a],%[t3],%[t2]")
BENCH(ext_1, 1, "vsldoi %[a],%[b],%[a],11")
BENCH(rev32_p8_2, 2, "vspltisw %[t1],-16\n\tvrlw %[a],%[a],%[t1]\n\tvspltish %[t1],8\n\tvrlh %[a],%[a],%[t1]")
/* register-half placement: the same chain entirely in the FPR half (vs0-31) via VSX forms */
BENCH(xxland_lowhalf, 3, "xxlor 0,%x[a],%x[a]\n\txxland 0,0,%x[b]\n\txxlor %x[a],0,0")
BENCH(xxlor_move_halves, 2, "xxlor 1,%x[a],%x[a]\n\txxlor %x[a],1,1")
#if P9
BENCH(vabsdub, 1, "vabsdub %[a],%[a],%[b]")
BENCH(vpermr, 1, "vpermr %[a],%[a],%[b],%[c]")
BENCH(xxperm, 1, "xxperm %x[a],%x[b],%x[c]")
BENCH(xxspltib, 1, "xxspltib %x[t1],90\n\tvaddubm %[a],%[a],%[t1]")
BENCH(mtvsrws, 2, "mfvsrd %[g],%x[a]\n\tmtvsrws %x[a],%[g]")
BENCH(mtvsrdd, 2, "mfvsrd %[g],%x[a]\n\tmtvsrdd %x[a],%[g],%[g]")
BENCH(vextubrx, 2, "li %[g],3\n\tvextubrx %[g],%[g],%[a]\n\tmtvsrd %x[a],%[g]")
BENCH(vctzlsbb, 1, "vctzlsbb %[g],%[a]\n\tmtvsrd %x[a],%[g]")
BENCH(vclzlsbb, 1, "vclzlsbb %[g],%[a]\n\tmtvsrd %x[a],%[g]")
BENCH(xxbrq, 1, "xxbrq %x[a],%x[a]")
BENCH(xxbrw, 1, "xxbrw %x[a],%x[a]")
BENCH(vinsertb, 2, "mfvsrd %[g],%x[a]\n\tmtvsrd %x[t1],%[g]\n\tvinsertb %[a],%[t1],5")
BENCH(vextsb2w, 1, "vextsb2w %[a],%[a]")
BENCH(lxv_const, 1, "lxv %x[t1],32(%[p])\n\tvaddubm %[a],%[a],%[t1]")
BENCH(tbl1_p9_3, 3, "xxspltib %x[t1],31\n\tvminub %[t1],%[b],%[t1]\n\tvpermr %[a],%[z],%[a],%[t1]")
BENCH(tbl1_p9_4, 4, "vspltisb %[t2],15\n\tvcmpgtub %[t2],%[b],%[t2]\n\tvpermr %[a],%[a],%[a],%[b]\n\tvandc %[a],%[a],%[t2]")
BENCH(vmulld_p10_unavailable, 0, "nop")
BENCH(ctx_roundtrip_stxv_lxv, 2, "stxv %x[a],0(%[p])\n\tlxv %x[a],0(%[p])")
BENCH(xscvhpdp, 1, "xscvhpdp %x[a],%x[a]")
BENCH(xvcvsphp, 1, "xvcvsphp %x[a],%x[a]")
#endif

int main(void) {
  memset(scratch, 1, sizeof scratch);
  /* warm up so the governor boosts before anything is measured */
  { uint64_t g = 1, t0 = tb(); while (tb() - t0 < 512000000ull / 4) { __asm__ volatile("add %0,%0,%0" : "+r"(g)); } __asm__ volatile("" :: "r"(g)); }
  /* calibrate: dependent add chain (2 cycles per add on POWER9, see the cpufreq line) */
  { uint64_t g = 1, t0 = tb(); for (int i = 0; i < ITER; i++) { __asm__ volatile("add %0,%0,%0\n\tadd %0,%0,%0\n\tadd %0,%0,%0\n\tadd %0,%0,%0\n\tadd %0,%0,%0\n\tadd %0,%0,%0\n\tadd %0,%0,%0\n\tadd %0,%0,%0" : "+r"(g)); } uint64_t t1 = tb(); tb_per_add = (double)(t1 - t0) / (8.0 * ITER); __asm__ volatile("" :: "r"(g)); }
  ghz = read_ghz();
  double ns_per_add = tb_per_add / 0.512;
  printf("# time-base ticks per dependent add: %.4f = %.3f ns; cpufreq says %.2f GHz -> a dependent add is %.2f core cycles\n", tb_per_add, ns_per_add, ghz, ns_per_add * ghz);
  printf("# %-30s %5s %8s %8s %8s %8s  (lat/tp in ns per sequence and in core cycles at the cpufreq clock; thruput = 4 independent chains)\n", "sequence", "insns", "lat ns", "tp ns", "lat cyc", "tp cyc");
  const struct benchent* all[] = {
    &vaddubm_ent, &vand_ent, &xxland_ent, &vor_ent, &xxlor_ent, &vsel_ent, &xxsel_ent, &vperm_ent, &vsldoi_ent, &xxpermdi_ent, &vpkuhum_ent, &vmrglb_ent, &vspltb_ent, &vmaxub_ent, &vcmpequb_ent, &vcmpgtub_ent, &vaddubs_sat_ent,
    &vslb_ent, &vsrab_ent, &vsld_ent, &vrlw_ent, &vmladduhm_ent, &vmuluwm_ent, &vmulesb_ent, &vmsumubm_ent, &vmhraddshs_ent, &vsum4ubs_ent, &vsumsws_ent, &vpopcntb_ent, &vclzb_ent, &vbpermq_ent, &vpmsumd_ent, &vpmsumb_ent,
    &vcipher_ent, &vcipherlast_ent, &vncipher_ent, &vshasigmaw_ent, &xvaddsp_ent, &xvmulsp_ent, &xvmaddasp_ent, &xvcmpeqsp_ent, &xvcmpeqsp_rc_ent, &xvmaxsp_ent, &xvcvspsxws_ent, &xvcvsxwsp_ent, &xvrspi_ent, &vrfin_ent, &xvresp_ent, &xvdivsp_ent, &xvsqrtsp_ent, &xvdivdp_ent, &xvnegsp_ent,
    &mfvsrd_ent, &mfvsrd_mtvsrd_ent, &mtvsrd_xxpermdi_vspltb_ent, &vextract_umov_b_p8_ent,
    &vspltisb_ent, &const_gpr_splat_ent, &const_pool_lvx_ent, &const_pool_lxvd2x_ent, &const_lvsl_vslb_ent,
    &stvx_lvx_fwd_ent, &std_lvx_partial_ent, &std_std_lvx_ent, &stvx_ld_mtvsrd_ent, &ctx_roundtrip_stxvd2x_lxvd2x_ent,
    &mfvscr_ent, &vaddubs_mfvscr_ent, &mtvscr_ent,
    &vcmpequb_rc_bc_ent, &vcmpequb_rc_mfocrf_ent, &movemask_bpermq_ent,
    &cmhi_1_ent, &cmhi_cur3_ent, &cmtst_2_ent, &cmtst_cur_ent, &vmov64_cur3_ent, &vmov64_xxpermdi_ent, &umaxp_cur6_ent, &umaxp_same4_ent, &umaxv16_fold10_ent, &addv16_3_ent, &addv32_rot4_ent,
    &shrn_cur4_ent, &srshr_imm5_ent, &sqshl_imm8_ent, &sshl_reg13_ent, &tbl1_p8_6_ent, &uqadd64_3_ent, &sqadd64_9_ent, &sqdmulh16_2_ent, &sqdmulh32_9_ent, &smull8_3_ent, &mul8_5_ent, &mul64_8_ent, &uabd_p8_3_ent,
    &udot_1_ent, &sdot_6_ent, &pmull_2_ent, &aese_3_ent, &aes_round_fused_2_ent, &fmla_gated_3_ent, &fcvtzs_3_ent, &rbit_8_ent, &zip1_1_ent, &uzp2_3_ent, &ext_1_ent, &rev32_p8_2_ent,
    &xxland_lowhalf_ent, &xxlor_move_halves_ent,
#if P9
    &vabsdub_ent, &vpermr_ent, &xxperm_ent, &xxspltib_ent, &mtvsrws_ent, &mtvsrdd_ent, &vextubrx_ent, &vctzlsbb_ent, &vclzlsbb_ent, &xxbrq_ent, &xxbrw_ent, &vinsertb_ent, &vextsb2w_ent, &lxv_const_ent, &tbl1_p9_3_ent, &tbl1_p9_4_ent, &ctx_roundtrip_stxv_lxv_ent, &xscvhpdp_ent, &xvcvsphp_ent,
#endif
  };
  for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
    const struct benchent* e = all[i]; if (e->ninsn == 0) continue;
    double l = 1e9, t = 1e9; for (int rep = 0; rep < 3; rep++) { double x = e->lat(); if (x < l) l = x; double y = e->tp(); if (y < t) t = y; }
    printf("%-32s %5d %8.3f %8.3f %8.2f %8.2f\n", e->name, e->ninsn, l * ns_per_add, t * ns_per_add, l * ns_per_add * ghz, t * ns_per_add * ghz);
  }
  printf("# cpufreq at end: %.2f GHz\n", read_ghz());
  return 0;
}
