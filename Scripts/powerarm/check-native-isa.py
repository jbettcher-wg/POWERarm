#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check a BUILT binary's own text against the ISA level it was built for.

WHY THIS EXISTS, and how it differs from check-isa30-guards.py. There are two
separate ways an ISA 3.0 instruction can reach a POWER8 user, and they need two
separate checks:

  * EMITTED guest code -- the bytes the JIT writes at run time. Those are data,
    not text, so no compiler flag affects them; they are gated at run time on
    HostFeatures.SupportsISA30 (AT_HWCAP2). check-isa30-guards.py is the source
    lint for that, and it is build-independent.

  * The emulator's OWN text -- what the compiler generated from our C++. That is
    what -mcpu= controls. A POWER8 build whose own text contains an ISA 3.0
    instruction traps on a real POWER8 immediately, long before any runtime
    feature gate is consulted. Nothing checked this, and this is that check.

The POWER8 build (-mcpu=power8 -mtune=power8) is the one that must come back
clean. A native or POWER9 build is EXPECTED to be full of ISA 3.0 and is
reported, not failed: on this box a TUNE_CPU=native build carries thousands,
which is the intended POWER9 codegen (POWER-STRENGTHS item 5) and arrived with
the TUNE_CPU=native fix in 88d645f52 -- before it, -march=native was probed on a
target that has no -march at all, so a native ppc64le build got no CPU flag and
compiled at the ISA 2.07 baseline.

ORACLE. The host's ppc64le GNU assembler, -mpower8 against -mpower9, exactly as
gen-isa30-list.py uses it and for the same reason: clang assembles `lxv` happily
under -mcpu=power8 and llvm-objdump decodes it at --mcpu=pwr8, so neither is an
oracle. Mnemonics are classified with the same operand-template search rather
than by round-tripping objdump's own operands, because objdump prints branch
targets as addresses and symbols that gas will not take -- which would file half
the text as unclassifiable and hide anything real in the noise.

A mnemonic the templates cannot place is reported as UNCLASSIFIED and fails the
run. Passing while unable to classify part of the text is the failure this
project keeps relearning.
"""
import io, os, re, subprocess, sys

AS = "/mnt/arch/usr/bin/as"
OBJDUMP = "/mnt/arch/usr/bin/objdump"

# Operand templates, tried in order until one assembles at power9. Same list as
# gen-isa30-list.py; keep them in step.
T = ["3,4,5", "3,4", "32,33,34", "32,33", "2,3,4", "2,3", "32,0(3)", "3,0(4)",
     "3,4,5,6", "2,3,4,5", "32,3,4", "3,32", "32,3", "3,4,5,6,7", "2,3,4,1",
     "0", "3,0", "3", "", "2,3,4,0", "32,33,34,0", "32,33,0", "1,3,4",
     "3,4,0,0", "3,4,0,31", "2,0,3", "32,0,3", "3,0,4", "2,3,0"]


def run_as(level, lines):
    """Line numbers (1-based) that the assembler rejected at -m<level>."""
    src = "\n".join(lines) + "\n"
    p = subprocess.run([AS, "-m" + level, "-o", "/dev/null", "-"],
                       input=src.encode(), capture_output=True)
    bad = set()
    for L in p.stderr.decode(errors="replace").split("\n"):
        m = re.search(r':(\d+): (?:Error|Fatal)', L)
        if m:
            bad.add(int(m.group(1)))
    return bad


def text_mnemonics(binary):
    """{mnemonic: count} over every executable section objdump disassembles."""
    p = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", binary],
                       capture_output=True)
    if p.returncode != 0:
        sys.stderr.write(p.stderr.decode(errors="replace"))
        return None
    counts = {}
    for L in p.stdout.decode(errors="replace").split("\n"):
        # "  1234ab:\tmnemonic opnds"  -- a disassembled line and nothing else.
        # The trailing `.' of a record form is part of the mnemonic. A `\b'
        # after the group silently drops it (`andi.' arrives as `andi'), and
        # then the assembler cannot place it and the run fails as unclassified.
        m = re.match(r'^\s*[0-9a-f]+:\s+([a-z][a-z0-9_]*\.?)(?:\s|$)', L)
        if m:
            counts[m.group(1)] = counts.get(m.group(1), 0) + 1
    return counts


def classify(mnems):
    """(isa30, unclassified) over the given mnemonics, by the assembler."""
    cands = [(mn, (mn + " " + t).strip()) for mn in sorted(mnems) for t in T]
    bad9 = run_as("power9", [c[1] for c in cands])
    form = {}
    for i, (mn, txt) in enumerate(cands, start=1):
        if i not in bad9 and mn not in form:
            form[mn] = txt
    items = sorted(form.items())
    bad8 = run_as("power8", [t for _, t in items])
    isa30 = sorted(mn for i, (mn, _) in enumerate(items, start=1) if i in bad8)
    return isa30, sorted(set(mnems) - set(form))


def tune_of(build_dir):
    """TUNE_CPU from a build directory's cache, or None."""
    cache = os.path.join(build_dir, "CMakeCache.txt")
    if not os.path.isfile(cache):
        return None
    for L in io.open(cache, encoding="utf-8", errors="replace"):
        m = re.match(r'^TUNE_CPU:STRING=(.*)$', L.strip())
        if m:
            return m.group(1).strip()
    return None


def main(argv):
    if len(argv) < 2:
        print("usage: check-native-isa.py <build-dir|binary> [--expect power8|any]",
              file=sys.stderr)
        return 2
    target = argv[1]
    expect = None
    for a in argv[2:]:
        if a.startswith("--expect="):
            expect = a.split("=", 1)[1]
        elif a == "--expect" :
            pass
    for t in (AS, OBJDUMP):
        if not os.path.exists(t):
            print("SKIP check-native-isa: no %s (native binutils required; the rootfs copies"
                  " target the GUEST)" % t)
            return 0

    binaries = []
    if os.path.isdir(target):
        tune = tune_of(target)
        bindir = os.path.join(target, "Bin")
        if not os.path.isdir(bindir):
            print("FAIL check-native-isa: no Bin/ in %s; pass a build directory or a binary"
                  % target)
            return 1
        for fn in sorted(os.listdir(bindir)):
            p = os.path.join(bindir, fn)
            if os.path.isfile(p) and os.access(p, os.X_OK):
                binaries.append(p)
    else:
        tune = None
        binaries = [target]

    # What the build says it is decides whether ISA 3.0 is a failure. An
    # explicit --expect wins; otherwise power8 must be clean and anything else
    # is reported.
    if expect is None:
        expect = "power8" if (tune or "").lower() in ("power8", "pwr8") else "any"

    print("# native-text ISA check: %d binar%s, TUNE_CPU=%s, expect=%s"
          % (len(binaries), "y" if len(binaries) == 1 else "ies", tune or "unknown", expect))
    rc = 0
    for b in binaries:
        counts = text_mnemonics(b)
        if counts is None:
            print("FAIL %s: objdump could not disassemble it" % os.path.basename(b))
            rc = 1
            continue
        isa30, unknown = classify(counts.keys())
        total = sum(counts[m] for m in isa30)
        name = os.path.basename(b)
        if unknown:
            print("FAIL %-18s %d mnemonic(s) the assembler could not place, so this text was"
                  " NOT fully checked: %s" % (name, len(unknown), " ".join(unknown)))
            print("     Add an operand form to T (keep it in step with gen-isa30-list.py).")
            rc = 1
        if not isa30:
            print("ok   %-18s %d distinct mnemonics, 0 ISA 3.0 instructions in its own text"
                  % (name, len(counts)))
            continue
        line = ", ".join("%s x%d" % (m, counts[m]) for m in
                         sorted(isa30, key=lambda m: -counts[m])[:8])
        if expect == "power8":
            print("FAIL %-18s %d ISA 3.0 instruction(s) in the text of a POWER8 build -- this"
                  " traps on a real POWER8: %s" % (name, total, line))
            rc = 1
        else:
            print("ok   %-18s %d ISA 3.0 instruction(s), expected for a %s build: %s"
                  % (name, total, tune or "non-POWER8", line))
    return rc


sys.exit(main(sys.argv))
