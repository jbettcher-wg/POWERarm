#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pinned Arch Linux ARM aarch64 sysroot: resolve, fetch, verify, extract, hash.

Subcommands (see README.md next to this file):
  resolve  parse core.db/extra.db from the mirror, compute the depends closure of the
           manifest's roots and rewrite the manifest's package pins
  fetch    download pinned packages and signatures into a cache, check sha256 and
           OpenPGP signatures
  extract  extract the pinned packages into a sysroot directory
  hash     content hash of a tree (sorted paths, sha256, modes, symlink targets)
  align    PT_LOAD p_align distribution of every ELF file in a tree
  overlay-init  prepare a per-user rootfs overlay for guest pacman: a local package
           database describing the pinned base, and (--with-pacman) pacman, its
           missing dependencies and the Arch Linux ARM keyring extracted into it

Needs only Python 3 (stdlib), curl or urllib, zstd (for .zst packages) and gpg/gpgv
(for signature checks).  Never needs root.
"""

import argparse
import collections
import hashlib
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

FORMAT = "1"
SKIP_MEMBERS = {".PKGINFO", ".INSTALL", ".MTREE", ".BUILDINFO", ".CHANGELOG"}
KEYRING_MEMBER = "usr/share/pacman/keyrings/archlinuxarm.gpg"


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def die(msg, code=1):
    log("error: " + msg)
    sys.exit(code)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url, dest):
    tmp = dest + ".part"
    log(f"  fetch {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "powerarm-sysroot/1"})
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
        lastmod = r.headers.get("Last-Modified", "")
    os.replace(tmp, dest)
    return lastmod


# --------------------------------------------------------------------------- manifest

class Manifest:
    def __init__(self):
        self.header = {}          # key -> value (single-valued)
        self.dbs = []             # (repo, sha256, lastmod)
        self.roots = []
        self.keyring = None       # dict
        self.pkgs = []            # dicts
        self.projects = []        # dicts
        self.basetar = None       # dict or None
        self.comments = []

    @staticmethod
    def load(path):
        m = Manifest()
        with open(path) as f:
            for lineno, line in enumerate(f, 1):
                s = line.rstrip("\n")
                if not s.strip() or s.lstrip().startswith("#"):
                    if lineno <= 40 and s.startswith("#"):
                        m.comments.append(s)
                    continue
                t = s.split()
                k = t[0]
                if k in ("format", "arch", "mirror", "keyring-fingerprint", "snapshot"):
                    m.header[k] = " ".join(t[1:])
                elif k == "db":
                    m.dbs.append((t[1], t[2], " ".join(t[3:])))
                elif k == "root":
                    m.roots.extend(t[1:])
                elif k in ("pkg", "keyring"):
                    d = dict(name=t[1], version=t[2], repo=t[3], filename=t[4], sha256=t[5],
                             csize=int(t[6]), isize=int(t[7]))
                    if k == "pkg":
                        m.pkgs.append(d)
                    else:
                        m.keyring = d
                elif k == "project":
                    m.projects.append(dict(name=t[1], version=t[2], url=t[3], sha256=t[4]))
                elif k == "basetar":
                    m.basetar = dict(url=t[1], sha256=t[2], size=int(t[3]))
                else:
                    die(f"{path}:{lineno}: unknown manifest key {k!r}")
        if m.header.get("format") != FORMAT:
            die(f"{path}: manifest format {m.header.get('format')!r}, expected {FORMAT}")
        return m

    def save(self, path):
        out = []
        out.extend(self.comments)
        out.append(f"format {FORMAT}")
        for k in ("arch", "mirror", "keyring-fingerprint", "snapshot"):
            if k in self.header:
                out.append(f"{k} {self.header[k]}")
        for repo, sha, lm in self.dbs:
            out.append(f"db {repo} {sha} {lm}")
        for i in range(0, len(self.roots), 8):
            out.append("root " + " ".join(self.roots[i:i + 8]))
        if self.basetar:
            b = self.basetar
            out.append(f"basetar {b['url']} {b['sha256']} {b['size']}")
        def pl(k, d):
            return (f"{k} {d['name']} {d['version']} {d['repo']} {d['filename']} "
                    f"{d['sha256']} {d['csize']} {d['isize']}")
        if self.keyring:
            out.append(pl("keyring", self.keyring))
        for d in sorted(self.pkgs, key=lambda d: d["name"]):
            out.append(pl("pkg", d))
        for p in self.projects:
            out.append(f"project {p['name']} {p['version']} {p['url']} {p['sha256']}")
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            f.write("\n".join(out) + "\n")
        os.replace(tmp, path)


# --------------------------------------------------------------------------- resolve

def parse_db(path, repo):
    """Return {name: entry}.  Handles both the split (desc+depends) and the merged layout."""
    pkgs = {}
    fields = collections.defaultdict(dict)
    with tarfile.open(path, "r:*") as tf:
        for mem in tf:
            if not mem.isfile():
                continue
            d = mem.name.split("/")[0]
            data = tf.extractfile(mem).read().decode()
            key = None
            for line in data.split("\n"):
                if line.startswith("%") and line.endswith("%"):
                    key = line[1:-1]
                    fields[d].setdefault(key, [])
                elif line and key:
                    fields[d][key].append(line)
    bad = []
    for d, f in fields.items():
        if not f.get("NAME") or not f.get("SHA256SUM"):
            bad.append(d)          # upstream dbs occasionally carry zero-filled entries
            continue
        name = f["NAME"][0]
        pkgs[name] = dict(
            name=name, version=f["VERSION"][0], filename=f["FILENAME"][0],
            sha256=f["SHA256SUM"][0], csize=int(f["CSIZE"][0]),
            isize=int(f.get("ISIZE", ["0"])[0]), repo=repo,
            depends=f.get("DEPENDS", []), provides=f.get("PROVIDES", []),
            arch=f.get("ARCH", [""])[0], pgpsig=f.get("PGPSIG", [""])[0])
    if bad:
        log(f"  {repo}.db: skipped {len(bad)} malformed entries: {' '.join(sorted(bad)[:5])}")
    return pkgs


def depname(dep):
    return re.split(r"[<>=]", dep, maxsplit=1)[0]


def cmd_resolve(a):
    m = Manifest.load(a.manifest)
    mirror = a.mirror or m.header["mirror"]
    os.makedirs(a.cache, exist_ok=True)
    repos = a.repos.split(",")
    dbs = []
    m.dbs = []
    for repo in repos:
        p = os.path.join(a.cache, f"{repo}.db")
        lm = download(f"{mirror}/{repo}/{repo}.db", p)
        m.dbs.append((repo, sha256_file(p), lm.replace(" ", "_") if lm else "-"))
        dbs.append(parse_db(p, repo))
    byname = {}
    provides = collections.defaultdict(list)
    for db in dbs:                       # first repo wins, like pacman
        for name, e in db.items():
            if name not in byname:
                byname[name] = e
    for name in sorted(byname):
        for pv in byname[name]["provides"]:
            provides[depname(pv)].append(name)
    prefer = dict(x.split("=", 1) for x in (a.prefer or []))

    selected = {}
    queue = list(m.roots)
    while queue:
        dep = queue.pop(0)
        n = depname(dep)
        if n in selected:
            continue
        if n in byname:
            pick = n
        else:
            cands = provides.get(n, [])
            already = [c for c in cands if c in selected]
            if already:
                continue
            if not cands:
                die(f"unresolvable dependency {dep!r}")
            pick = prefer.get(n) or cands[0]
            if len(cands) > 1:
                log(f"  note: {n} provided by {cands}; picked {pick}")
        if pick in selected:
            continue
        e = byname[pick]
        selected[pick] = e
        queue.extend(e["depends"])
    m.pkgs = [dict((k, e[k]) for k in ("name", "version", "repo", "filename", "sha256",
                                        "csize", "isize")) for e in selected.values()]
    kr = byname.get("archlinuxarm-keyring")
    if not kr:
        die("archlinuxarm-keyring not found in repos")
    m.keyring = dict((k, kr[k]) for k in ("name", "version", "repo", "filename", "sha256",
                                         "csize", "isize"))
    m.header["mirror"] = mirror
    m.save(a.manifest)
    tot = sum(p["csize"] for p in m.pkgs)
    itot = sum(p["isize"] for p in m.pkgs)
    log(f"resolved {len(m.pkgs)} packages, {tot/1e6:.1f} MB download, "
        f"{itot/1e6:.1f} MB installed; manifest {a.manifest} rewritten")


# --------------------------------------------------------------------------- fetch/verify

def fetch_one(mirrors, repo, filename, sha, cache, want_sig=True):
    dest = os.path.join(cache, filename)
    if os.path.exists(dest) and sha256_file(dest) != sha:
        log(f"  {filename}: cached copy has wrong sha256, kept as .bad, refetching")
        os.replace(dest, dest + ".bad")
    last_err = None
    if not os.path.exists(dest):
        for mir in mirrors:
            try:
                download(f"{mir}/{repo}/{filename}", dest)
                break
            except Exception as e:           # try the next mirror
                last_err = e
                log(f"  {mir}: {e}")
        else:
            die(f"{filename}: not downloadable from any mirror ({last_err}). The Arch Linux "
                "ARM mirrors keep only the current version; restore the package cache "
                "or re-pin with 'resolve'.")
    got = sha256_file(dest)
    if got != sha:
        os.rename(dest, dest + ".bad")
        die(f"{filename}: sha256 mismatch: manifest {sha}, got {got} (kept as .bad)")
    if want_sig and not os.path.exists(dest + ".sig"):
        for mir in mirrors:
            try:
                download(f"{mir}/{repo}/{filename}.sig", dest + ".sig")
                break
            except Exception as e:
                last_err = e
        else:
            die(f"{filename}.sig: not downloadable ({last_err})")
    return dest


def member_bytes(pkgpath, member):
    for mem, fobj in iter_pkg(pkgpath):
        if mem.name.lstrip("./") == member and fobj is not None:
            return fobj.read()
    return None


class Verifier:
    def __init__(self, m, cache, keyring_pkg):
        self.fpr = m.header["keyring-fingerprint"].replace(" ", "").upper()
        self.home = tempfile.mkdtemp(prefix="powerarm-gpg-")
        os.chmod(self.home, 0o700)
        raw = member_bytes(keyring_pkg, KEYRING_MEMBER)
        if raw is None:
            die(f"{KEYRING_MEMBER} missing from {keyring_pkg}")
        kr = os.path.join(self.home, "archlinuxarm.gpg")
        with open(kr, "wb") as f:
            f.write(raw)
        gpg = ["gpg", "--homedir", self.home, "--batch", "--quiet"]
        subprocess.run(gpg + ["--import", kr], check=True, stderr=subprocess.DEVNULL)
        cols = subprocess.run(gpg + ["--with-colons", "--fingerprint", self.fpr],
                              capture_output=True, text=True)
        fprs = re.findall(r"^fpr:+([0-9A-F]+):", cols.stdout, re.M)
        if cols.returncode != 0 or self.fpr not in fprs:
            die(f"pinned key {self.fpr} not in {KEYRING_MEMBER}")
        self.subfprs = set(fprs)       # primary plus its subkeys
        self.gpgv_kr = os.path.join(self.home, "pinned.gpg")
        with open(self.gpgv_kr, "wb") as f:
            f.write(subprocess.run(gpg + ["--export", self.fpr], check=True,
                                   capture_output=True).stdout)

    def verify(self, path):
        r = subprocess.run(["gpgv", "--status-fd", "1", "--keyring", self.gpgv_kr,
                            path + ".sig", path], capture_output=True, text=True)
        valid = re.findall(r"^\[GNUPG:\] VALIDSIG (\S+) .* (\S+)$", r.stdout, re.M)
        ok = any(sig in self.subfprs or prim == self.fpr for sig, prim in valid)
        if r.returncode != 0 or not ok:
            die(f"{os.path.basename(path)}: signature verification failed\n{r.stdout}{r.stderr}")

    def close(self):
        shutil.rmtree(self.home, ignore_errors=True)


def mirrors_of(a, m):
    return a.mirror or [m.header["mirror"]]


def do_fetch(a, m):
    os.makedirs(a.cache, exist_ok=True)
    mirrors = mirrors_of(a, m)
    sig = not a.no_verify_signatures
    kp = fetch_one(mirrors, m.keyring["repo"], m.keyring["filename"], m.keyring["sha256"],
                   a.cache, sig)
    ver = None
    if sig:
        if not shutil.which("gpg") or not shutil.which("gpgv"):
            die("gpg/gpgv not found; install them or pass --no-verify-signatures")
        ver = Verifier(m, a.cache, kp)
        ver.verify(kp)
    paths = []
    for p in sorted(m.pkgs, key=lambda d: d["name"]):
        path = fetch_one(mirrors, p["repo"], p["filename"], p["sha256"], a.cache, sig)
        if ver:
            ver.verify(path)
        paths.append((p, path))
    if ver:
        ver.close()
    log(f"fetched {len(paths)} packages: sha256 OK, signatures "
        f"{'OK (key ' + m.header['keyring-fingerprint'] + ')' if sig else 'NOT CHECKED'}")
    return paths


def cmd_fetch(a):
    do_fetch(a, Manifest.load(a.manifest))


# --------------------------------------------------------------------------- extract

def iter_pkg(path):
    """Yield (TarInfo, fileobj-or-None) for a package, streaming."""
    proc = None
    if path.endswith(".zst"):
        if not shutil.which("zstd"):
            die("zstd not found (needed for .pkg.tar.zst)")
        proc = subprocess.Popen(["zstd", "-dcq", path], stdout=subprocess.PIPE)
        tf = tarfile.open(fileobj=proc.stdout, mode="r|")
    elif path.endswith(".xz"):
        tf = tarfile.open(path, mode="r|xz")
    elif path.endswith(".gz"):
        tf = tarfile.open(path, mode="r|gz")
    else:
        tf = tarfile.open(path, mode="r|")
    try:
        for mem in tf:
            yield mem, (tf.extractfile(mem) if mem.isreg() else None)
    finally:
        tf.close()
        if proc:
            proc.stdout.close()
            if proc.wait() != 0:
                die(f"zstd failed on {path}")


def safe_rel(name):
    n = name
    while n.startswith("./"):
        n = n[2:]
    n = n.strip("/")
    parts = [x for x in n.split("/") if x not in ("", ".")]
    if any(x == ".." for x in parts):
        die(f"refusing path with '..': {name}")
    return "/".join(parts)


def ensure_parent(root, rel, dirmodes):
    """Create parent directories, refusing to traverse symlinks."""
    cur = root
    for comp in rel.split("/")[:-1]:
        cur = os.path.join(cur, comp)
        if os.path.islink(cur):
            die(f"package path {rel} traverses symlink {cur}")
        if not os.path.isdir(cur):
            os.mkdir(cur, 0o755)
            dirmodes.setdefault(os.path.relpath(cur, root), 0o755)


def make_writable_tree(path):
    for dp, dns, fns in os.walk(path):
        for n in dns:
            p = os.path.join(dp, n)
            if not os.path.islink(p):
                os.chmod(p, (os.lstat(p).st_mode & 0o7777) | 0o700)


def extract(pkgs_paths, dest, force, basetar=None):
    if os.path.lexists(dest):
        if os.listdir(dest) and not force:
            die(f"{dest} exists and is not empty (use --force to replace it)")
        make_writable_tree(dest)
        shutil.rmtree(dest)
    os.makedirs(dest)
    dirmodes = {}
    owner = {}
    collisions = []
    scriptlets = []
    sources = []
    if basetar:
        sources.append(("<base tarball>", basetar))
    sources.extend((p["name"], path) for p, path in pkgs_paths)
    for pname, path in sources:
        links = []
        for mem, fobj in iter_pkg(path):
            rel = safe_rel(mem.name)
            if not rel:
                continue
            if rel in SKIP_MEMBERS:
                if rel == ".INSTALL":
                    scriptlets.append(pname)
                continue
            full = os.path.join(dest, rel)
            ensure_parent(dest, rel, dirmodes)
            if mem.isdir():
                if os.path.islink(full):
                    continue      # e.g. base tarball dir vs. symlink; keep existing symlink
                if not os.path.isdir(full):
                    os.mkdir(full, 0o700)
                dirmodes[rel] = mem.mode & 0o7777
                continue
            if os.path.lexists(full):
                if os.path.isdir(full) and not os.path.islink(full):
                    die(f"{pname}: {rel} is a directory in an earlier package")
                if owner.get(rel) not in (None, pname):
                    collisions.append((rel, owner[rel], pname))
                os.chmod(full, 0o600) if not os.path.islink(full) else None
                os.unlink(full)
            owner[rel] = pname
            if mem.issym():
                os.symlink(mem.linkname, full)
            elif mem.islnk():
                links.append((safe_rel(mem.linkname), rel, mem.mode & 0o7777))
                continue
            elif mem.isreg():
                with open(full, "wb") as f:
                    shutil.copyfileobj(fobj, f, 1 << 20)
                os.chmod(full, mem.mode & 0o7777)
                os.utime(full, (mem.mtime, mem.mtime))
            else:
                log(f"  skipped special file {rel} ({pname}, type {mem.type!r})")
                continue
        for target, rel, mode in links:
            full = os.path.join(dest, rel)
            ensure_parent(dest, rel, dirmodes)
            if os.path.lexists(full):
                os.unlink(full)
            shutil.copy2(os.path.join(dest, target), full)   # copy, not link: fs-agnostic
            os.chmod(full, mode)
    for rel in sorted(dirmodes, key=lambda r: -r.count("/")):
        os.chmod(os.path.join(dest, rel), dirmodes[rel])
    os.chmod(dest, 0o755)
    return collisions, scriptlets


def cmd_extract(a):
    m = Manifest.load(a.manifest)
    paths = do_fetch(a, m)
    base = None
    if a.base_tarball:
        base = fetch_basetar(a, m)
    dest = os.path.abspath(a.dest)
    log(f"extracting into {dest}")
    collisions, scriptlets = extract(paths, dest, a.force, base)
    for rel, o, n in collisions:
        log(f"  file collision: {rel} from {o} replaced by {n}")
    log(f"  install scriptlets NOT run: {', '.join(scriptlets) or 'none'}")
    digest, count = tree_hash(dest, a.contents_out)
    print(f"sysroot {dest}")
    print(f"packages {len(paths)} download_bytes {sum(p['csize'] for p, _ in paths)} "
          f"installed_bytes {sum(p['isize'] for p, _ in paths)}")
    print(f"entries {count}")
    print(f"content-hash sha256:{digest}")


def fetch_basetar(a, m):
    """Official ArchLinuxARM-aarch64 rootfs tarball, pinned by sha256 and signature-checked."""
    b = m.basetar
    if not b:
        die("manifest has no basetar line")
    cache = os.path.join(os.path.dirname(a.cache.rstrip("/")), "alarm-base")
    os.makedirs(cache, exist_ok=True)
    dest = os.path.join(cache, os.path.basename(b["url"]))
    if not os.path.exists(dest) or sha256_file(dest) != b["sha256"]:
        download(b["url"], dest)
        download(b["url"] + ".sig", dest + ".sig")
    got = sha256_file(dest)
    if got != b["sha256"]:
        die(f"base tarball sha256 mismatch: manifest {b['sha256']}, got {got}. The official "
            "tarball is only published as '-latest' and there is no archive of old ones.")
    if not a.no_verify_signatures:
        if not os.path.exists(dest + ".sig"):
            download(b["url"] + ".sig", dest + ".sig")
        kp = os.path.join(a.cache, m.keyring["filename"])
        ver = Verifier(m, a.cache, kp)
        ver.verify(dest)
        ver.close()
        log("base tarball: sha256 OK, signature OK")
    return dest


# --------------------------------------------------------------------------- hash

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
                if not mode & 0o400:          # e.g. 4110 helpers: readable by root only
                    os.chmod(p, mode | 0o400)
                    try:
                        digest = sha256_file(p)
                    finally:
                        os.chmod(p, mode)
                else:
                    digest = sha256_file(p)
                out.append((rel, f"f {mode:04o} {digest}"))
            else:
                out.append((rel, f"? {mode:04o}"))
    out.sort(key=lambda e: e[0].encode("utf-8", "surrogateescape"))
    return out


def tree_hash(root, contents_out=None):
    ents = tree_entries(root)
    h = hashlib.sha256()
    lines = []
    for rel, desc in ents:
        line = f"{desc} {rel}\n"
        lines.append(line)
        h.update(line.encode("utf-8", "surrogateescape"))
    if contents_out:
        with open(contents_out, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.writelines(lines)
    return h.hexdigest(), len(ents)


def cmd_hash(a):
    digest, count = tree_hash(a.root, a.contents_out)
    print(f"entries {count}")
    print(f"content-hash sha256:{digest}")


# --------------------------------------------------------------------------- align

def elf_loads(path):
    with open(path, "rb") as f:
        hdr = f.read(64)
        if len(hdr) < 64 or hdr[:4] != b"\x7fELF":
            return None
        if hdr[4] != 2 or hdr[5] != 1:
            return ("non-elf64le", None, [])
        e_type, e_machine = struct.unpack_from("<HH", hdr, 16)
        e_phoff, = struct.unpack_from("<Q", hdr, 32)
        e_phentsize, e_phnum = struct.unpack_from("<HH", hdr, 54)
        aligns = []
        if e_phoff and e_phnum:
            f.seek(e_phoff)
            ph = f.read(e_phentsize * e_phnum)
            for i in range(e_phnum):
                off = i * e_phentsize
                if off + 56 > len(ph):
                    break
                p_type, = struct.unpack_from("<I", ph, off)
                if p_type == 1:
                    aligns.append(struct.unpack_from("<Q", ph, off + 48)[0])
        tname = {1: "REL", 2: "EXEC", 3: "DYN", 4: "CORE"}.get(e_type, str(e_type))
        return (tname, e_machine, aligns)


def cmd_align(a):
    dist = collections.Counter()          # (e_type, min PT_LOAD p_align) -> files
    segs = collections.Counter()          # p_align -> PT_LOAD segments
    small = []
    nload = collections.Counter()
    other = collections.Counter()         # non-aarch64 ELF (e.g. guile .go bytecode)
    for dp, dns, fns in os.walk(a.root):
        for n in fns:
            p = os.path.join(dp, n)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            try:
                r = elf_loads(p)
            except OSError:
                continue
            if r is None:
                continue
            tname, mach, aligns = r
            if mach != 183:               # EM_AARCH64
                other[(mach, os.path.splitext(n)[1] or n)] += 1
                continue
            if not aligns:
                nload[tname] += 1
                continue
            for x in aligns:
                segs[x] += 1
            mn = min(aligns)
            dist[(tname, mn)] += 1
            if mn < a.threshold:
                small.append((os.path.relpath(p, a.root), tname, sorted(set(aligns))))
    total = sum(dist.values())
    print(f"aarch64 ELF files with PT_LOAD: {total} ({sum(segs.values())} PT_LOAD segments)")
    for (t, al), c in sorted(dist.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        print(f"  min p_align {al:#8x}  {t:4s}  {c} files")
    for al, c in sorted(segs.items()):
        print(f"  segments with p_align {al:#8x}: {c}")
    print("aarch64 ELF files without PT_LOAD: "
          + (", ".join(f"{t}={c}" for t, c in sorted(nload.items())) or "0"))
    if other:
        print("non-aarch64 ELF files (not loaded by the kernel or ld.so): "
              + ", ".join(f"e_machine={m} *{ext}={c}" for (m, ext), c in sorted(other.items(), key=str)))
    print(f"aarch64 ELF files with a PT_LOAD p_align below {a.threshold:#x}: {len(small)}")
    for rel, t, al in sorted(small):
        print(f"  {rel} {t} " + ",".join(f"{x:#x}" for x in al))


# --------------------------------------------------------------------------- overlay-init

# The per-user overlay (docs/powerarm/DESIGN.md section 6.2a) is where guest pacman
# installs. pacman needs a local database describing what is already installed, or
# it would try to install glibc and the rest over the base. The base is extracted
# from pinned packages, not installed by pacman, so this builds that database from
# the same cached package files. The base tree itself is never touched: everything
# is written below --dest, and the base's content hash does not change.

ALPM_DB_VERSION = "9"
META_MEMBERS = {".PKGINFO", ".MTREE", ".INSTALL", ".BUILDINFO", ".CHANGELOG"}


def parse_pkginfo(text):
    info = collections.defaultdict(list)
    for line in text.splitlines():
        if line.startswith("#") or " = " not in line:
            continue
        key, value = line.split(" = ", 1)
        info[key.strip()].append(value.strip())
    return info


def read_package(path, dest=None, base=None):
    """A package's .PKGINFO, file list, backup md5s, .MTREE and .INSTALL.

    With dest, the package's files are also extracted below dest (existing
    directories are kept, files and symlinks replaced). With base, a package
    path that runs through a symlink of the base (usr/sbin -> bin, say) is
    refused: a real directory of that name in the overlay would hide the link."""
    info = {}
    files = []
    md5s = {}
    mtree = install = None
    links = []
    dirmodes = {}
    for mem, fobj in iter_pkg(path):
        rel = safe_rel(mem.name)
        if not rel:
            continue
        if rel in META_MEMBERS:
            data = fobj.read() if fobj else b""
            if rel == ".PKGINFO":
                info = parse_pkginfo(data.decode())
            elif rel == ".MTREE":
                mtree = data
            elif rel == ".INSTALL":
                install = data
            continue
        files.append(rel + "/" if mem.isdir() else rel)
        # makepkg puts .PKGINFO before the payload; until it is seen, hash everything.
        want_md5 = mem.isreg() and (not info or rel in info.get("backup", []))
        if dest is None:
            if want_md5:
                md5s[rel] = hashlib.md5(fobj.read()).hexdigest()
            continue
        full = os.path.join(dest, rel)
        if base:
            parts = rel.split("/")
            for i in range(1, len(parts) + (1 if mem.isdir() else 0)):
                prefix = "/".join(parts[:i])
                if os.path.islink(os.path.join(base, prefix)) and not os.path.lexists(os.path.join(dest, prefix)):
                    die(f"{os.path.basename(path)}: {rel} runs through the base's symlink {prefix}")
        ensure_parent(dest, rel, dirmodes)
        if mem.isdir():
            if not os.path.isdir(full) or os.path.islink(full):
                if os.path.lexists(full):
                    die(f"{path}: {rel} is a directory in the package but not in {dest}")
                os.mkdir(full, 0o700)
                dirmodes[rel] = mem.mode & 0o7777
            continue
        if os.path.lexists(full):
            if os.path.isdir(full) and not os.path.islink(full):
                die(f"{path}: {rel} is a directory in {dest}")
            os.unlink(full)
        if mem.issym():
            os.symlink(mem.linkname, full)
        elif mem.islnk():
            links.append((safe_rel(mem.linkname), rel, mem.mode & 0o7777))
        elif mem.isreg():
            h = hashlib.md5()
            with open(full, "wb") as f:
                for chunk in iter(lambda: fobj.read(1 << 20), b""):
                    h.update(chunk)
                    f.write(chunk)
            if want_md5:
                md5s[rel] = h.hexdigest()
            os.chmod(full, mem.mode & 0o7777)
            os.utime(full, (mem.mtime, mem.mtime))
        else:
            log(f"  skipped special file {rel} ({os.path.basename(path)}, type {mem.type!r})")
    if dest is not None:
        for target, rel, mode in links:
            full = os.path.join(dest, rel)
            if os.path.lexists(full):
                os.unlink(full)
            shutil.copy2(os.path.join(dest, target), full)
            os.chmod(full, mode)
        for rel in sorted(dirmodes, key=lambda r: -r.count("/")):
            os.chmod(os.path.join(dest, rel), dirmodes[rel])
    if not info:
        die(f"{path}: no .PKGINFO")
    return info, files, md5s, mtree, install


def write_local_entry(local, info, files, md5s, mtree, install, explicit):
    """One package's entry in a pacman local database (libalpm be_local.c, version 9)."""
    name, version = info["pkgname"][0], info["pkgver"][0]
    entry = os.path.join(local, f"{name}-{version}")
    os.makedirs(entry, exist_ok=True)

    def section(key, values):
        values = [v for v in values if v]
        return f"%{key}%\n" + "".join(v + "\n" for v in values) + "\n" if values else ""

    builddate = info.get("builddate", ["0"])
    desc = "".join([
        section("NAME", [name]), section("VERSION", [version]), section("BASE", info.get("pkgbase", [])),
        section("DESC", info.get("pkgdesc", [])), section("URL", info.get("url", [])),
        section("ARCH", info.get("arch", [])), section("BUILDDATE", builddate),
        # The build date, not now: the same pins always give the same database.
        section("INSTALLDATE", builddate), section("PACKAGER", info.get("packager", [])),
        section("SIZE", info.get("size", [])), section("REASON", [] if explicit else ["1"]),
        section("GROUPS", info.get("group", [])), section("LICENSE", info.get("license", [])),
        section("VALIDATION", ["pgp"]), section("REPLACES", info.get("replaces", [])),
        section("DEPENDS", info.get("depend", [])), section("OPTDEPENDS", info.get("optdepend", [])),
        section("CONFLICTS", info.get("conflict", [])), section("PROVIDES", info.get("provides", [])),
        section("XDATA", info.get("xdata", [])),
    ])
    backup = [f"{b}\t{md5s[b]}" for b in info.get("backup", []) if b in md5s]
    # libalpm looks files up by binary search, so the list is byte-sorted.
    listing = section("FILES", sorted(set(files), key=lambda s: s.encode("utf-8", "surrogateescape"))) + section("BACKUP", backup)
    with open(os.path.join(entry, "desc"), "w") as f:
        f.write(desc)
    with open(os.path.join(entry, "files"), "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(listing)
    if mtree is not None:
        with open(os.path.join(entry, "mtree"), "wb") as f:
            f.write(mtree)
    if install is not None:
        with open(os.path.join(entry, "install"), "wb") as f:
            f.write(install)
    return name


def bootstrap_packages(a, m, dest, local, installed):
    """Install pacman's missing dependency closure into the overlay, from today's repos."""
    mirrors = mirrors_of(a, m)
    dbs = []
    for repo in a.repos.split(","):
        p = os.path.join(a.cache, f"{repo}.db")
        for mir in mirrors:
            try:
                download(f"{mir}/{repo}/{repo}.db", p)
                break
            except Exception as e:
                log(f"  {mir}: {e}")
        else:
            die(f"{repo}.db: not downloadable from any mirror")
        dbs.append(parse_db(p, repo))
    byname = {}
    for db in dbs:
        for name, e in db.items():
            byname.setdefault(name, e)
    provides = collections.defaultdict(list)
    for name in sorted(byname):
        for pv in byname[name]["provides"]:
            provides[depname(pv)].append(name)

    have = set()
    for name, info in installed.items():
        have.add(name)
        have.update(depname(pv) for pv in info.get("provides", []))
    exclude = set(a.exclude or [])
    selected, unmet = [], []
    queue = list(a.packages)
    while queue:
        dep = queue.pop(0)
        n = depname(dep)
        if n in have:
            continue
        if n in exclude:
            unmet.append(dep)
            continue
        pick = n if n in byname else (provides.get(n) or [None])[0]
        if pick is None:
            die(f"unresolvable dependency {dep!r}")
        if pick in have:
            continue
        have.add(pick)
        have.update(depname(pv) for pv in byname[pick]["provides"])
        selected.append(pick)
        queue.extend(byname[pick]["depends"])

    sig = not a.no_verify_signatures
    ver = None
    if sig:
        kp = fetch_one(mirrors, m.keyring["repo"], m.keyring["filename"], m.keyring["sha256"], a.cache, True)
        ver = Verifier(m, a.cache, kp)
    log(f"bootstrapping {len(selected)} packages into {dest}: {' '.join(selected)}")
    for name in selected:
        e = byname[name]
        path = fetch_one(mirrors, e["repo"], e["filename"], e["sha256"], a.cache, sig)
        if ver:
            ver.verify(path)
        info, files, md5s, mtree, install = read_package(path, dest, a.base)
        write_local_entry(local, info, files, md5s, mtree, install, explicit=name in a.packages)
        if install is not None:
            log(f"  {name}: install scriptlet NOT run")
    if ver:
        ver.close()
    if unmet:
        log(f"  not installed (--exclude), left unmet in the local database: {' '.join(sorted(set(unmet)))}")
    return selected


def adjust_pacman_conf(dest):
    """pacman.conf as the pacman package ships it, minus what the guest lacks."""
    conf = os.path.join(dest, "etc", "pacman.conf")
    if not os.path.exists(conf):
        return
    with open(conf) as f:
        lines = f.read().split("\n")
    out = []
    for line in lines:
        # Downloads run as user alpm, which pacman's sysusers entry creates; install
        # scriptlets and sysusers do not run here, so there is no such user.
        if line.startswith("DownloadUser"):
            out.append("# POWERarm overlay: no alpm user (sysusers did not run), downloads stay as the caller")
            out.append("#" + line)
            continue
        out.append(line)
    with open(conf, "w") as f:
        f.write("\n".join(out))


def cmd_overlay_init(a):
    m = Manifest.load(a.manifest)
    dest = os.path.abspath(a.dest)
    if not os.path.isdir(dest):
        die(f"{dest}: no such directory (create the overlay directory first)")
    local = os.path.join(dest, "var", "lib", "pacman", "local")
    if os.path.isdir(local) and set(os.listdir(local)) - {"ALPM_DB_VERSION"}:
        die(f"{local} already holds packages; refusing to register the base twice")
    os.makedirs(local, exist_ok=True)
    for sub in (("var", "lib", "pacman", "sync"), ("var", "cache", "pacman", "pkg")):
        os.makedirs(os.path.join(dest, *sub), exist_ok=True)
    with open(os.path.join(local, "ALPM_DB_VERSION"), "w") as f:
        f.write(ALPM_DB_VERSION + "\n")

    paths = do_fetch(a, m)
    roots = set(m.roots)
    installed = {}
    log(f"registering {len(paths)} base packages in {local}")
    for p, path in paths:
        info, files, md5s, mtree, install = read_package(path)
        installed[write_local_entry(local, info, files, md5s, mtree, install, explicit=p["name"] in roots)] = info
    added = []
    if a.with_pacman:
        added = bootstrap_packages(a, m, dest, local, installed)
        adjust_pacman_conf(dest)
    print(f"overlay {dest}")
    print(f"base packages registered {len(paths)}")
    print(f"packages installed {len(added)}{': ' + ' '.join(added) if added else ''}")


# --------------------------------------------------------------------------- main

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    xdg_data = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    xdg_cache = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, fetch=True):
        p.add_argument("--manifest", default=os.path.join(here, "alarm-m2.manifest"))
        if fetch:
            p.add_argument("--cache", default=os.path.join(xdg_cache, "powerarm", "alarm-pkgs"))
            p.add_argument("--mirror", action="append",
                           help="repo base URL ending in /aarch64 (repeatable; tried in order)")

    p = sub.add_parser("resolve")
    common(p)
    p.add_argument("--repos", default="core,extra,alarm")
    p.add_argument("--prefer", action="append", help="PROVIDE=PACKAGE to break provider ties")
    p.set_defaults(fn=cmd_resolve)

    p = sub.add_parser("fetch")
    common(p)
    p.add_argument("--no-verify-signatures", action="store_true")
    p.set_defaults(fn=cmd_fetch)

    p = sub.add_parser("extract")
    common(p)
    p.add_argument("--no-verify-signatures", action="store_true")
    p.add_argument("--dest", default=os.path.join(xdg_data, "powerarm", "RootFS",
                                                  "ArchLinuxARM-m2"))
    p.add_argument("--force", action="store_true")
    p.add_argument("--base-tarball", action="store_true",
                   help="lay the pinned official rootfs tarball down first (experimental)")
    p.add_argument("--contents-out")
    p.set_defaults(fn=cmd_extract)

    p = sub.add_parser("hash")
    p.add_argument("root")
    p.add_argument("--contents-out")
    p.set_defaults(fn=cmd_hash)

    p = sub.add_parser("align")
    p.add_argument("root")
    p.add_argument("--threshold", type=lambda s: int(s, 0), default=0x10000)
    p.set_defaults(fn=cmd_align)

    p = sub.add_parser("overlay-init")
    common(p)
    p.add_argument("--dest", required=True, help="the overlay directory (must exist)")
    p.add_argument("--base", default=os.path.join(xdg_data, "powerarm", "RootFS", "ArchLinuxARM-m2"),
                   help="the base rootfs the overlay sits on (only read)")
    p.add_argument("--no-verify-signatures", action="store_true")
    p.add_argument("--with-pacman", action="store_true",
                   help="also install pacman's missing dependency closure from today's repos")
    p.add_argument("--package", dest="packages", action="append",
                   help="closure root for --with-pacman (repeatable; default pacman, archlinuxarm-keyring)")
    p.add_argument("--exclude", action="append", help="dependency to leave uninstalled (repeatable)")
    p.add_argument("--repos", default="core,extra,alarm")
    p.set_defaults(fn=cmd_overlay_init)

    a = ap.parse_args()
    if a.cmd == "resolve" and a.mirror:
        a.mirror = a.mirror[0]
    if a.cmd == "overlay-init" and not a.packages:
        a.packages = ["pacman", "archlinuxarm-keyring"]
    a.fn(a)


if __name__ == "__main__":
    main()
