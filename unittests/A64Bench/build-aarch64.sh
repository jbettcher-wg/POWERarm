#!/bin/sh
# Build the A64Bench workloads as freestanding, integer-only, static AArch64 ELFs.
# Run natively on an AArch64 Linux machine (the reference is a Raspberry Pi 5).
# Output: out/aarch64/<name>. Fails if any binary uses an FP/SIMD register.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-clang}
OBJDUMP=${OBJDUMP:-objdump}
out=$here/out/aarch64
mkdir -p "$out"
CFLAGS="-O2 -static -nostdlib -ffreestanding -fno-builtin -mgeneral-regs-only \
  -march=armv8-a -mtune=cortex-a76 -fno-pie -fno-stack-protector \
  -fno-asynchronous-unwind-tables -Wall -Wextra -Wno-unused-function"
for src in "$here"/src/*.c; do
  name=$(basename "$src" .c)
  # shellcheck disable=SC2086
  $CC $CFLAGS -o "$out/$name" "$src"
  # Any d/q/v/s/b-register or FP/NEON mnemonic means the frontend would need FP/SIMD.
  if $OBJDUMP -d --no-show-raw-insn "$out/$name" | awk -F'\t' 'NF>=2 {print $2" "$3}' |
     grep -Eq '(^|[ ,\[])(v|q|d|s|h|b)[0-9]+([., \]]|$)|fmov|fcvt|fcmp|movi'; then
    echo "error: $name uses FP/SIMD registers" >&2
    $OBJDUMP -d --no-show-raw-insn "$out/$name" | grep -E '\b(v|q|d)[0-9]+\b' | head >&2
    exit 1
  fi
  echo "built $out/$name ($($CC --version | head -1))"
done
{ echo "compiler: $($CC --version | head -1)"; echo "cflags: $CFLAGS"; } > "$out/BUILDINFO"
