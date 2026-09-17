/* A tiny register bytecode interpreter running a Collatz step counter with CALL/RET.
 * Dispatch is a switch compiled to a jump table, so every bytecode is one host
 * indirect branch whose target depends on the bytecode stream. Stresses: indirect
 * branch prediction (count cache), return prediction for guest CALL/RET is not
 * involved (those are data), short dependency chains through the reg[] array. */
static const char BENCH_NAME[] = "vm";
static const unsigned long BENCH_DEFAULT_SCALE = 150000; /* outer iterations */
#include "bench.h"

enum { HALT, LOADI, MOV, ADD, ADDI, ANDI, SHRI, MULI, JMP, JNZ, JEQ, JLT, CALL, RET, NOPS };
struct insn {
  u8 op, a, b, c;
  i32 imm;
};
static struct insn prog[64];

static void bench_setup(u64 scale) {
  (void)scale;
  int pc = 0;
#define E(o, A, B, C, I) prog[pc++] = (struct insn){o, A, B, C, I}
  /* main: r1 = n (set by host), r2 = acc, r0 = i */
  E(LOADI, 2, 0, 0, 0);          /* 0 */
  E(LOADI, 0, 0, 0, 0);          /* 1 */
  E(MOV, 3, 0, 0, 0);            /* 2 loop: r3 = i */
  E(CALL, 0, 0, 0, 16);          /* 3 */
  E(ADD, 2, 2, 4, 0);            /* 4 acc += steps */
  E(ADDI, 0, 0, 0, 1);           /* 5 */
  E(JLT, 0, 1, 0, 2);            /* 6 if i < n goto loop */
  E(HALT, 0, 0, 0, 0);           /* 7 */
  pc = 16;
  /* sub: r4 = collatz steps of (r3 + 27) */
  E(ADDI, 3, 3, 0, 27);          /* 16 */
  E(LOADI, 4, 0, 0, 0);          /* 17 */
  E(LOADI, 5, 0, 0, 1);          /* 18 */
  E(JEQ, 3, 5, 0, 29);           /* 19 top: if r3 == 1 return */
  E(ANDI, 6, 3, 0, 1);           /* 20 */
  E(JNZ, 6, 0, 0, 24);           /* 21 */
  E(SHRI, 3, 3, 0, 1);           /* 22 */
  E(JMP, 0, 0, 0, 26);           /* 23 */
  E(MULI, 3, 3, 0, 3);           /* 24 */
  E(ADDI, 3, 3, 0, 1);           /* 25 */
  E(ADDI, 4, 4, 0, 1);           /* 26 */
  E(NOPS, 0, 0, 0, 0);           /* 27 */
  E(JMP, 0, 0, 0, 19);           /* 28 */
  E(RET, 0, 0, 0, 0);            /* 29 */
#undef E
}

static u64 bench_run(u64 scale) {
  u64 reg[16] = {0};
  u32 cstack[64];
  u32 csp = 0, pc = 0;
  u64 dispatched = 0;
  reg[1] = scale;
  for (;;) {
    const struct insn *in = &prog[pc++];
    dispatched++;
    switch (in->op) {
    case HALT: return reg[2] ^ (dispatched << 20);
    case LOADI: reg[in->a] = (u64)(i64)in->imm; break;
    case MOV: reg[in->a] = reg[in->b]; break;
    case ADD: reg[in->a] = reg[in->b] + reg[in->c]; break;
    case ADDI: reg[in->a] = reg[in->b] + (u64)(i64)in->imm; break;
    case ANDI: reg[in->a] = reg[in->b] & (u64)(i64)in->imm; break;
    case SHRI: reg[in->a] = reg[in->b] >> in->imm; break;
    case MULI: reg[in->a] = reg[in->b] * (u64)(i64)in->imm; break;
    case JMP: pc = (u32)in->imm; break;
    case JNZ: if (reg[in->a]) pc = (u32)in->imm; break;
    case JEQ: if (reg[in->a] == reg[in->b]) pc = (u32)in->imm; break;
    case JLT: if (reg[in->a] < reg[in->b]) pc = (u32)in->imm; break;
    case CALL: cstack[csp++ & 63] = pc; pc = (u32)in->imm; break;
    case RET: pc = cstack[--csp & 63]; break;
    case NOPS: break;
    default: return ~0ul;
    }
  }
}
