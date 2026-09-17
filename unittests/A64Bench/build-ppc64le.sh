#!/bin/sh
# Build the same A64Bench sources natively for ppc64le, integer-only (no FP, VMX or VSX),
# once per -mcpu level. This is the "native" side of the POWERarm/native ratio.
# Output: out/ppc64le-power8/<name>, out/ppc64le-power9/<name>.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-clang}
OBJDUMP=${OBJDUMP:-objdump}
for cpu in ${CPUS:-power8 power9}; do
  out=$here/out/ppc64le-$cpu
  mkdir -p "$out"
  CFLAGS="-O2 -mcpu=$cpu -static -nostdlib -ffreestanding -fno-builtin -mno-altivec -mno-vsx \
    -msoft-float -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
    -Wall -Wextra -Wno-unused-function"
  for src in "$here"/src/*.c; do
    name=$(basename "$src" .c)
    # shellcheck disable=SC2086
    $CC $CFLAGS -o "$out/$name" "$src"
    # FP (f0-f31), VMX (v0-v31) or VSX (vs0-vs63) register use, or FP/vector mnemonics.
    if $OBJDUMP -d --no-show-raw-insn "$out/$name" | awk -F'\t' 'NF>=2 {print $2" "$3}' |
       grep -Eq '(^|[ ,(])(f|v|vs)[0-9]+([ ,)]|$)|^(lfd|stfd|lfs|stfs|mffs|mtfs|fmr|xxl|xxs|vp|vc)'; then
      echo "error: $name ($cpu) uses FP/vector registers" >&2
      exit 1
    fi
    echo "built $out/$name"
  done
  { echo "compiler: $($CC --version | head -1)"; echo "cflags: $CFLAGS"; } > "$out/BUILDINFO"
done
