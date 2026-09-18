// Build-time check of the linked-callee convention (common/Guest.h,
// CallHostFunction): each invoker must copy X17 before any call and before
// anything writes IP0/IP1. check_linked_callee.py reads the disassembly of
// this object. The shapes cover what the guest compiler does differently: a
// few scalars, more arguments than registers, and by-value aggregates big
// enough that building the packed arguments takes memset/memcpy calls and,
// for the last, a frame too large for an immediate SP adjustment.
#include "common/Guest.h"

#define PROBE_HASH(n)                                                                                                                   \
  "0xfe, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, " \
  "0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, " n

struct Big {
  char bytes[5000];
};
struct Huge {
  char bytes[200000];
};

using Small = void (*)(int, long, void*);
using Many = long (*)(long, long, long, long, long, long, long, long, long, long, long, long);
using BigFn = int (*)(Big);
using HugeFn = int (*)(Huge, Huge);

MAKE_CALLBACK_THUNK(probe_small, void(int, long, void*), PROBE_HASH("0x01"))
MAKE_CALLBACK_THUNK(probe_many, long(long, long, long, long, long, long, long, long, long, long, long, long), PROBE_HASH("0x02"))
MAKE_CALLBACK_THUNK(probe_big, int(Big), PROBE_HASH("0x03"))
MAKE_CALLBACK_THUNK(probe_huge, int(Huge, Huge), PROBE_HASH("0x04"))

extern "C" void* linked_callee_probe(int Which) {
  switch (Which) {
  case 0: return (void*)GetCallerForHostFunction((Small) nullptr);
  case 1: return (void*)GetCallerForHostFunction((Many) nullptr);
  case 2: return (void*)GetCallerForHostFunction((BigFn) nullptr);
  default: return (void*)GetCallerForHostFunction((HugeFn) nullptr);
  }
}
