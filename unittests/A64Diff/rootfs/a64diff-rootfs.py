#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rootfs identity for A64Diff bundles.

  hash DIR [--contents-out FILE]   print "entries N" and "content-hash sha256:HEX"
  verify DIR sha256:HEX            exit 0 if DIR has that content hash

The content hash is the same definition the Arch Linux ARM sysroot builder
uses (Scripts/powerarm/rootfs, "hash"): one line per entry below DIR, sorted
by path bytes, "d MODE path", "f MODE SHA256 path", "l TARGET path" or
"? MODE path", hashed with SHA-256.  Owners and times are not part of it, so
an unprivileged extraction hashes the same everywhere.
"""
import argparse
import hashlib
import os
import stat
import sys


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tree_entries(root):
    out = []
    for dp, dns, fns in os.walk(root):
        dns.sort()
        for n in dns + fns:
            p = os.path.join(dp, n)
            rel = os.path.relpath(p, root)
            st = os.lstat(p)
            mode = stat.S_IMODE(st.st_mode)
            if stat.S_ISLNK(st.st_mode):
                out.append((rel, f"l {os.readlink(p)}"))
            elif stat.S_ISDIR(st.st_mode):
                out.append((rel, f"d {mode:04o}"))
            elif stat.S_ISREG(st.st_mode):
                out.append((rel, f"f {mode:04o} {sha256_file(p)}"))
            else:
                out.append((rel, f"? {mode:04o}"))
    out.sort(key=lambda e: e[0].encode("utf-8", "surrogateescape"))
    return out


def tree_hash(root, contents_out=None):
    h = hashlib.sha256()
    lines = []
    ents = tree_entries(root)
    for rel, desc in ents:
        line = f"{desc} {rel}\n"
        lines.append(line)
        h.update(line.encode("utf-8", "surrogateescape"))
    if contents_out:
        with open(contents_out, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.writelines(lines)
    return "sha256:" + h.hexdigest(), len(ents)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    h = sub.add_parser("hash")
    h.add_argument("dir")
    h.add_argument("--contents-out")
    v = sub.add_parser("verify")
    v.add_argument("dir")
    v.add_argument("expected")
    a = ap.parse_args()
    if not os.path.isdir(a.dir):
        print(f"a64diff-rootfs: {a.dir} is not a directory", file=sys.stderr)
        return 2
    if a.cmd == "hash":
        digest, n = tree_hash(a.dir, a.contents_out)
        print(f"entries {n}")
        print(f"content-hash {digest}")
        return 0
    digest, n = tree_hash(a.dir)
    if digest != a.expected:
        print(f"a64diff-rootfs: {a.dir}: content hash {digest}, bundle expects {a.expected}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
