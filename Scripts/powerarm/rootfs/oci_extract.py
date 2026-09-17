#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rootless OCI/Docker image extractor: registry HTTP API to a plain rootfs directory.

  oci_extract.py [options] IMAGE[:TAG|@sha256:DIGEST] DEST

No Docker, no root.  Anonymous bearer-token flow (Docker Hub, ghcr.io, quay.io, ...),
manifest-list/OCI-index platform selection, sha256 verification of every manifest and
blob, ordered layer extraction with .wh.* whiteouts and .wh..wh..opq opaque directories.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from alarm_sysroot import tree_hash, make_writable_tree  # noqa: E402

MT_INDEX = ("application/vnd.oci.image.index.v1+json",
            "application/vnd.docker.distribution.manifest.list.v2+json")
MT_MANIFEST = ("application/vnd.oci.image.manifest.v1+json",
               "application/vnd.docker.distribution.manifest.v2+json")
DOCKER_HUB = "registry-1.docker.io"


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def die(msg):
    log("error: " + msg)
    sys.exit(1)


def parse_ref(ref):
    digest = None
    if "@" in ref:
        ref, digest = ref.split("@", 1)
        if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
            die(f"unsupported digest {digest!r}")
    first = ref.split("/", 1)[0]
    if "/" in ref and ("." in first or ":" in first or first == "localhost"):
        registry, repo = ref.split("/", 1)
    else:
        registry, repo = DOCKER_HUB, ref
    tag = None
    if ":" in repo.rsplit("/", 1)[-1]:
        repo, tag = repo.rsplit(":", 1)
    if registry in (DOCKER_HUB, "docker.io", "index.docker.io"):
        registry = DOCKER_HUB
        if "/" not in repo:
            repo = "library/" + repo
    if not tag and not digest:
        tag = "latest"
    return registry, repo, tag, digest


class Registry:
    def __init__(self, registry, repo):
        self.base = f"https://{registry}/v2/{repo}"
        self.repo = repo
        self.token = None

    def _auth(self, www):
        m = re.match(r"Bearer\s+(.*)", www or "", re.I)
        if not m:
            die(f"registry wants unsupported auth: {www!r}")
        params = dict(re.findall(r'(\w+)="([^"]*)"', m.group(1)))
        realm = params.pop("realm")
        params.setdefault("scope", f"repository:{self.repo}:pull")
        url = realm + "?" + urllib.parse.urlencode(params)
        with urllib.request.urlopen(url, timeout=60) as r:
            j = json.load(r)
        self.token = j.get("token") or j.get("access_token")

    def get(self, path, accept=None, stream=False):
        for attempt in range(2):
            req = urllib.request.Request(self.base + path)
            req.add_header("User-Agent", "powerarm-oci-extract/1")
            if accept:
                req.add_header("Accept", ", ".join(accept))
            if self.token:
                # unredirected: blob redirects go to signed CDN URLs that reject extra auth
                req.add_unredirected_header("Authorization", "Bearer " + self.token)
            try:
                return urllib.request.urlopen(req, timeout=300)
            except urllib.error.HTTPError as e:
                if e.code == 401 and attempt == 0:
                    self._auth(e.headers.get("WWW-Authenticate"))
                    continue
                die(f"GET {self.base + path}: HTTP {e.code} {e.reason}")


def fetch_manifest(reg, ref):
    with reg.get(f"/manifests/{ref}", accept=MT_INDEX + MT_MANIFEST) as r:
        body = r.read()
        mt = r.headers.get("Content-Type", "").split(";")[0]
    digest = "sha256:" + hashlib.sha256(body).hexdigest()
    if ref.startswith("sha256:") and digest != ref:
        die(f"manifest digest mismatch: asked {ref}, got {digest}")
    j = json.loads(body)
    mt = j.get("mediaType") or mt
    return j, mt, digest, body


def fetch_blob(reg, digest, cache):
    hexd = digest.split(":", 1)[1]
    path = os.path.join(cache, "blobs", "sha256", hexd)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for c in iter(lambda: f.read(1 << 20), b""):
                h.update(c)
        if h.hexdigest() == hexd:
            return path
        os.replace(path, path + ".bad")
    log(f"  fetch blob {digest}")
    h = hashlib.sha256()
    with reg.get(f"/blobs/{digest}") as r, open(path + ".part", "wb") as f:
        for c in iter(lambda: r.read(1 << 20), b""):
            h.update(c)
            f.write(c)
    if h.hexdigest() != hexd:
        os.replace(path + ".part", path + ".bad")
        die(f"blob {digest}: sha256 mismatch (got sha256:{h.hexdigest()}, kept as .bad)")
    os.replace(path + ".part", path)
    return path


def open_layer(path, mediatype):
    if mediatype.endswith("zstd"):
        proc = subprocess.Popen(["zstd", "-dcq", path], stdout=subprocess.PIPE)
        return tarfile.open(fileobj=proc.stdout, mode="r|"), proc
    if mediatype.endswith("gzip"):
        return tarfile.open(path, mode="r|gz"), None
    if mediatype.endswith("tar") or mediatype.endswith(".tar"):
        return tarfile.open(path, mode="r|"), None
    with open(path, "rb") as f:            # unknown type: sniff
        magic = f.read(4)
    if magic[:2] == b"\x1f\x8b":
        return tarfile.open(path, mode="r|gz"), None
    if magic == b"\x28\xb5\x2f\xfd":
        return open_layer(path, "zstd")
    return tarfile.open(path, mode="r|"), None


class Tree:
    """Rootfs being assembled; paths resolve like a chroot (symlinks stay inside)."""

    def __init__(self, root):
        self.root = root
        self.dirmodes = {}
        self.skipped = []

    def resolve_parent(self, rel):
        """Host path of rel's parent directory, following symlinks inside the root."""
        parts = [p for p in rel.split("/") if p not in ("", ".")]
        out = []
        hops = 0
        todo = parts[:-1]
        while todo:
            comp = todo.pop(0)
            if comp == "..":
                if out:
                    out.pop()
                continue
            cand = os.path.join(self.root, *out, comp)
            if os.path.islink(cand):
                hops += 1
                if hops > 40:
                    die(f"symlink loop resolving {rel}")
                tgt = os.readlink(cand)
                if tgt.startswith("/"):
                    out = []
                todo = [p for p in tgt.split("/") if p not in ("", ".")] + todo
                continue
            if not os.path.exists(cand):
                os.mkdir(cand, 0o755)
            elif not os.path.isdir(cand):
                die(f"{rel}: parent component {comp} is not a directory")
            out.append(comp)
        return os.path.join(self.root, *out), parts[-1] if parts else ""

    def remove(self, path):
        if os.path.islink(path) or not os.path.isdir(path):
            if os.path.lexists(path):
                try:
                    os.unlink(path)
                except PermissionError:
                    os.chmod(os.path.dirname(path), 0o755)
                    os.unlink(path)
        else:
            make_writable_tree(path)
            os.chmod(path, 0o755)
            shutil.rmtree(path)

    def apply_layer(self, tf):
        created = set()
        links = []
        for mem in tf:
            name = mem.name
            while name.startswith("./"):
                name = name[2:]
            name = name.strip("/")
            if not name or any(p == ".." for p in name.split("/")):
                continue
            parent, base = self.resolve_parent(name)
            if base == ".wh..wh..opq":
                for child in os.listdir(parent):
                    cp = os.path.join(parent, child)
                    if cp not in created:
                        self.remove(cp)
                continue
            if base.startswith(".wh."):
                self.remove(os.path.join(parent, base[4:]))
                continue
            full = os.path.join(parent, base)
            if mem.isdir():
                if os.path.lexists(full) and not os.path.isdir(full):
                    self.remove(full)
                if not os.path.isdir(full):
                    os.mkdir(full, 0o755)
                else:
                    os.chmod(full, 0o755 | (os.lstat(full).st_mode & 0o700))
                self.dirmodes[full] = mem.mode & 0o7777
                created.add(full)
                continue
            if os.path.lexists(full):
                self.remove(full)
            created.add(full)
            if mem.issym():
                os.symlink(mem.linkname, full)
            elif mem.islnk():
                links.append((mem.linkname, full, mem.mode & 0o7777))
            elif mem.isreg():
                with open(full, "wb") as f:
                    shutil.copyfileobj(tf.extractfile(mem), f, 1 << 20)
                os.chmod(full, mem.mode & 0o7777)
                os.utime(full, (mem.mtime, mem.mtime))
            else:
                self.skipped.append(name)   # device nodes, fifos: need root
        for target, full, mode in links:
            tparent, tbase = self.resolve_parent(target.strip("/"))
            src = os.path.join(tparent, tbase)
            try:
                os.link(src, full)
            except OSError:
                shutil.copy2(src, full)
                os.chmod(full, mode)

    def finish(self):
        for p in sorted(self.dirmodes, key=lambda p: -p.count("/")):
            if os.path.isdir(p) and not os.path.islink(p):
                os.chmod(p, self.dirmodes[p])
        os.chmod(self.root, 0o755)


def main():
    xdg_cache = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("dest")
    ap.add_argument("--platform", default="linux/arm64",
                    help="OS/ARCH[/VARIANT] to select from a manifest list (default linux/arm64)")
    ap.add_argument("--cache", default=os.path.join(xdg_cache, "powerarm", "oci"))
    ap.add_argument("--record", help="image record file (default DEST.oci-manifest)")
    ap.add_argument("--force", action="store_true", help="replace a non-empty DEST")
    a = ap.parse_args()

    registry, repo, tag, digest = parse_ref(a.image)
    reg = Registry(registry, repo)
    j, mt, top_digest, _ = fetch_manifest(reg, digest or tag)
    log(f"{registry}/{repo}{':' + tag if tag else ''} -> {top_digest} ({mt})")
    plat = a.platform.split("/")
    index_digest = None
    if mt in MT_INDEX or "manifests" in j:
        index_digest = top_digest
        want_os, want_arch = plat[0], plat[1]
        want_var = plat[2] if len(plat) > 2 else None
        cands = [m for m in j["manifests"]
                 if m.get("platform", {}).get("os") == want_os
                 and m.get("platform", {}).get("architecture") == want_arch]
        if want_var:
            cands = [m for m in cands if m["platform"].get("variant") == want_var]
        elif want_arch == "arm64":
            cands.sort(key=lambda m: m["platform"].get("variant") not in (None, "v8"))
        if not cands:
            die(f"no {a.platform} manifest in index; platforms: "
                + ", ".join(f"{m.get('platform', {}).get('os')}/"
                            f"{m.get('platform', {}).get('architecture')}" for m in j["manifests"]))
        j, mt, man_digest, _ = fetch_manifest(reg, cands[0]["digest"])
    else:
        man_digest = top_digest
    if mt not in MT_MANIFEST and "layers" not in j:
        die(f"unsupported manifest media type {mt}")

    cfg_path = fetch_blob(reg, j["config"]["digest"], a.cache)
    with open(cfg_path) as f:
        cfg = json.load(f)
    if cfg.get("architecture") and cfg["architecture"] != plat[1]:
        die(f"config architecture {cfg['architecture']} != {plat[1]}")
    layers = [(L["digest"], L.get("mediaType", ""), fetch_blob(reg, L["digest"], a.cache))
              for L in j["layers"]]

    dest = os.path.abspath(a.dest)
    if os.path.lexists(dest):
        if os.listdir(dest) and not a.force:
            die(f"{dest} exists and is not empty (use --force)")
        make_writable_tree(dest)
        shutil.rmtree(dest)
    os.makedirs(dest)
    tree = Tree(dest)
    for d, lmt, path in layers:
        log(f"  apply layer {d}")
        tf, proc = open_layer(path, lmt)
        tree.apply_layer(tf)
        tf.close()
        if proc:
            proc.stdout.close()
            if proc.wait():
                die(f"zstd failed on {d}")
    tree.finish()
    if tree.skipped:
        log(f"  skipped {len(tree.skipped)} special files (device nodes/fifos): "
            + " ".join(tree.skipped[:8]))
    chash, count = tree_hash(dest)

    record = a.record or dest.rstrip("/") + ".oci-manifest"
    pinned = f"{registry}/{repo}@{index_digest or man_digest}"
    with open(record, "w") as f:
        f.write("# oci-extract record: whitespace-separated key value\n")
        f.write(f"image {a.image}\n")
        f.write(f"registry {registry}\nrepository {repo}\n")
        if tag:
            f.write(f"tag {tag}\n")
        if index_digest:
            f.write(f"index-digest {index_digest}\n")
        f.write(f"platform {a.platform}\nmanifest-digest {man_digest}\n")
        f.write(f"config-digest {j['config']['digest']}\n")
        for d, lmt, _ in layers:
            f.write(f"layer {d} {lmt}\n")
        f.write(f"pinned-reference {pinned}\n")
        f.write(f"skipped-special-files {len(tree.skipped)}\n")
        f.write(f"content-hash sha256:{chash}\n")
    print(f"rootfs {dest}")
    print(f"pinned-reference {pinned}")
    print(f"manifest-digest {man_digest}")
    print(f"entries {count}")
    print(f"content-hash sha256:{chash}")
    print(f"record {record}")


if __name__ == "__main__":
    main()
