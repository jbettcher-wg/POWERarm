/* spin.c -- a sibling-thread load: register-only ALU work forever */
#include <stdint.h>
int main(void){ uint64_t a=1,b=3; for(;;) __asm__ volatile("add %0,%0,%1\n\tadd %0,%0,%1\n\tadd %0,%0,%1\n\tadd %0,%0,%1":"+r"(a):"r"(b)); }
