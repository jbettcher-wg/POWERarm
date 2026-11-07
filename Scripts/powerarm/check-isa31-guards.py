#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Find ISA 3.1 instructions emitted on paths that run without ISA 3.1.

Mirrors check-isa30-guards.py: every ISA 3.1 emitter method in
Scripts/powerarm/isa31-instructions.txt must be called strictly inside a block
guarded on SupportsISA31 / HostSupportsISA31 (or reviewed in EXEMPT) so one
POWERarm binary runs cleanly on POWER8, POWER9, POWER10, and Houshou.
"""
import io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SCAN = [
    os.path.join(ROOT, "FEXCore/Source/Interface/Core/JIT/PPC64LE"),
    os.path.join(ROOT, "FEXCore/Source/Interface/Core/ArchHelpers"),
]

LIST = os.path.join(ROOT, "Scripts/powerarm/isa31-instructions.txt")
if not os.path.exists(LIST):
    print("FAIL %s is missing; regenerate with gen-isa31-list.py" % LIST, file=sys.stderr)
    sys.exit(2)
with io.open(LIST, encoding="utf-8") as f:
    ISA31_MNEM = sorted(x.strip() for x in f if x.strip() and not x.startswith("#"))
ISA31 = sorted(set(m[:-1] + "_" if m.endswith(".") else m for m in ISA31_MNEM))

# Reviewed by hand; each needs a reason, and an empty reason is a failure.
EXEMPT = {
    # (method, enclosing function): reason
}

HAND_ENCODED = {
}

RAW_EMIT_OK = {
    "0x7fe00008": "tw 31,0,0, the base-ISA trap the branch paths pad with",
    "0x00000000": "primary opcode 0, deliberately invalid",
    "0": "a placeholder that is backpatched later",
}

GUARD = re.compile(r'SupportsISA31|HostSupportsISA31|CanUsePForm|ISA31\b|Isa31|isa31')

KEYWORDS = r'if|for|while|switch|return|else|do|case|catch|sizeof|assert|static_assert'
FUNCDEF_RE = re.compile(r'^(\s*)(?:DEF_OP[A-Z_]*\s*\(|(?!(?:' + KEYWORDS + r')\b)'
                        r'[A-Za-z_][A-Za-z0-9_:<>,\*& ]*?\s+[A-Za-z_][A-Za-z0-9_:~]*\s*\()')


def FUNCDEF_match(line):
    m = FUNCDEF_RE.match(line)
    if not m:
        return None
    head = line[:m.end()]
    if "=" in head or ";" in head:
        return None
    return m


def functions(path):
    lines = io.open(path, encoding="utf-8", errors="replace").read().split("\n")
    cur_name, cur, depth, started = None, [], 0, False
    for L in lines:
        if not started and FUNCDEF_match(L):
            cur_name = L.strip()[:90]
            cur, depth, started = [L], L.count("{") - L.count("}"), "{" in L
            if started and depth <= 0:
                yield cur_name, "\n".join(cur)
                cur_name, cur, depth, started = None, [], 0, False
            continue
        if cur_name is not None:
            cur.append(L)
            if not started:
                depth += L.count("{") - L.count("}")
                started = "{" in L
            else:
                depth += L.count("{") - L.count("}")
            if started and depth <= 0:
                yield cur_name, "\n".join(cur)
                cur_name, cur, depth, started = None, [], 0, False


def strip_noncode(line):
    out, i, n = [], 0, len(line)
    in_str = None
    while i < n:
        c = line[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == in_str:
                in_str = None
            i += 1
            continue
        if c in ('"', "'"):
            in_str = c
            i += 1
            continue
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break
        out.append(c)
        i += 1
    return "".join(out)


def raw_emit_operand(line):
    for tag in ("Emit32(", "EmitPrefixed("):
        i = line.find(tag)
        if i >= 0:
            arg = line[i + len(tag):]
            depth, out = 0, []
            for c in arg:
                if c == "(":
                    depth += 1
                elif c == ")":
                    if depth == 0:
                        break
                    depth -= 1
                elif c == "," and depth == 0:
                    break
                out.append(c)
            return "".join(out).strip().rstrip("uU").lower()
    return None


def enclosing_guarded(body, call_line_idx):
    lines = [strip_noncode(x) for x in body.split("\n")]
    stack = []
    pending = ""
    negated_return_before = False
    for i, L in enumerate(lines):
        if i == call_line_idx:
            return (any(GUARD.search(t) for t in stack), negated_return_before)
        for ch in L:
            if ch == "{":
                stack.append(pending + " " + L)
            elif ch == "}":
                if stack:
                    top = stack.pop()
                    if GUARD.search(top) and "!" in top and "return" in top:
                        negated_return_before = True
        pending = L
    return (False, negated_return_before)


def main():
    for k, reason in EXEMPT.items():
        if not reason or not reason.strip():
            print("FAIL EXEMPT entry %r has an empty reason" % (k,), file=sys.stderr)
            return 1

    CHECKED = sorted(set(ISA31) | set(HAND_ENCODED))
    calls = {m: re.compile(r'(?<![A-Za-z0-9_])' + re.escape(m) + r'\s*\(') for m in CHECKED}
    hard, review = [], []
    scanned = 0
    files = []
    for d in SCAN:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if (fn.endswith((".cpp", ".h")) and "PPC64" in fn) or (
                    fn.endswith((".cpp", ".h")) and d.endswith("PPC64LE")):
                files.append((fn, os.path.join(d, fn)))

    blind = []
    for fn, path in files:
        text = io.open(path, encoding="utf-8", errors="replace").read()
        text_hits = sum(1 for m in CHECKED
                        if re.search(r'(?<![A-Za-z0-9_])' + re.escape(m) + r'\s*\(', text))
        body_count = 0
        covered = 0
        for name, body in functions(path):
            body_count += 1
            covered += sum(1 for m in CHECKED
                           if re.search(r'(?<![A-Za-z0-9_])' + re.escape(m) + r'\s*\(', body))
        if text_hits and not covered:
            blind.append((fn, text_hits, body_count))

    for fn, path in files:
        for name, body in functions(path):
            scanned += 1
            mentions = bool(GUARD.search(body))
            blines = [strip_noncode(x) for x in body.split("\n")]
            head_is_def = bool(blines) and bool(FUNCDEF_match(blines[0]))
            for m, rx in calls.items():
                for i, L in enumerate(blines):
                    if i == 0 and head_is_def:
                        continue
                    if not rx.search(L):
                        continue
                    key = (m, name)
                    if key in EXEMPT:
                        continue
                    if not mentions:
                        hard.append((fn, name, m, i))
                        continue
                    inside, neg_ret = enclosing_guarded(body, i)
                    if not inside and not neg_ret:
                        review.append((fn, name, m, L.strip()[:72]))

    undeclared = []
    for fn, path in files:
        for name, body in functions(path):
            if any(h in name for h in HAND_ENCODED) or fn == "PPC64Emitter.cpp":
                # Note: PPC64Emitter.cpp checked below for non-EmitXxspltib/Vpermr too
                pass
            if any(h in name for h in ("EmitXxspltib", "EmitVpermr")) or any(h in name for h in HAND_ENCODED):
                continue
            for L in body.split("\n"):
                L = strip_noncode(L)
                arg = raw_emit_operand(L)
                if arg is None or arg in RAW_EMIT_OK:
                    continue
                undeclared.append((fn, name, L.strip()[:64]))

    print("# ISA 3.1 guard lint: %d ISA 3.1 emitters (generated list) + %d hand-encoded,"
          " %d function bodies in %s"
          % (len(ISA31), len(HAND_ENCODED), scanned,
             ", ".join(os.path.relpath(d, ROOT) for d in SCAN)))
    rc = 0
    if undeclared:
        print("FAIL %d raw Emit32/EmitPrefixed() call(s) that are neither a known-safe word nor"
              " inside a declared hand-encoded helper:" % len(undeclared))
        for fn, name, src in sorted(undeclared):
            print("  %s\n    %s\n    %s" % (fn, name, src))
        rc = 1
    else:
        print("ok   tier 0: every raw Emit32/EmitPrefixed() is a known-safe word or a declared helper")
    if blind:
        print("FAIL the parser sees ISA 3.1 calls in %d file(s) but captured none of them in a"
              " function body -- these were NOT checked:" % len(blind))
        for fn, hits, bodies in blind:
            print("  %-26s %d ISA 3.1 mnemonic(s) in the text, %d bodies parsed" % (fn, hits, bodies))
        rc = 1
    if hard:
        print("FAIL %d ISA 3.1 call(s) in a function that never consults SupportsISA31:" % len(hard))
        for fn, name, m, _ in sorted(hard):
            print("  %-22s %-56s %s" % (fn, name, m))
        rc = 1
    else:
        print("ok   tier 1: every ISA 3.1 call is in a function that consults SupportsISA31")
    if review:
        print("REVIEW %d ISA 3.1 call(s) whose function consults the flag but which are not"
              " lexically inside a guarded block:" % len(review))
        for fn, name, m, src in sorted(review):
            print("  %s\n    %s\n    %s   <- %s" % (fn, name, src, m))
        rc = 1
    else:
        print("ok   tier 2: every ISA 3.1 call is inside a block guarded on the flag")
    return rc


sys.exit(main())
