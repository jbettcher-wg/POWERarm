# POWERarm rootfs tools

Scripts that build and run the aarch64 root filesystems POWERarm's M2 milestone uses
(`docs/powerarm/M2-PLAN.md`, tasks A and B). None of them needs root, and all of them run on
both the POWER9 host and the Raspberry Pi 5.

| File | Purpose |
|---|---|
| `build-alarm-sysroot.sh` | builds the pinned Arch Linux ARM GCC sysroot |
| `alarm_sysroot.py` | its engine: `resolve`, `fetch`, `extract`, `hash`, `align` |
| `alarm-m2.manifest` | the pins: packages, keyring, source tarballs, optional base tarball |
| `run-in-sysroot.sh` | runs a command natively inside a rootfs (Pi reference side) |
| `fetch-m2-projects.sh` | downloads the pinned zlib and Lua tarballs |
| `oci-extract.sh`, `oci_extract.py` | extracts an OCI/Docker registry image into a rootfs |

Needs Python 3 (stdlib only), `gpg` and `gpgv` for signature checks, `zstd` if a package or
layer is zstd-compressed, `curl` for `fetch-m2-projects.sh`, and `bwrap` (or `unshare` and
`chroot`) for the runner.

## Building the sysroot

```sh
Scripts/powerarm/rootfs/build-alarm-sysroot.sh            # same command on the Pi and the POWER9
```

1. Every pinned package and its detached `.sig` is downloaded into
   `${XDG_CACHE_HOME:-~/.cache}/powerarm/alarm-pkgs/`. A cached file that already matches its
   pin isn't downloaded again.
2. **sha256:** every file must match its manifest pin. A mismatch is fatal, and the bad file is
   kept as `*.bad`.
3. **Signatures:** these are verified without root. `archlinuxarm-keyring` is itself pinned by
   sha256, and `archlinuxarm.gpg` is taken from it and imported into a throwaway `GNUPGHOME`.
   Only the key whose fingerprint is pinned in the manifest
   (`68B3537F39A313B3E574D06777193F152BDBE6A6`, Arch Linux ARM Build System) is exported to a
   `gpgv` keyring. Every package, including the keyring package, must carry a `VALIDSIG` from
   that key or one of its subkeys. `--no-verify-signatures` falls back to the sha256 pins alone.
   Revocations and expiry aren't evaluated beyond what `gpgv` reports.
4. **Extraction:** packages go into
   `${XDG_DATA_HOME:-~/.local/share}/powerarm/RootFS/ArchLinuxARM-m2/` (`--dest` to change it,
   `--force` to replace it), in manifest (name) order.
   - `.PKGINFO`, `.INSTALL`, `.MTREE`, `.BUILDINFO` and `.CHANGELOG` are skipped.
   - Modes are kept, including setuid. Ownership isn't kept: everything belongs to the building
     user.
   - Package hard links become copies. Special files, of which there are none in these
     packages, are skipped.
   - A package path that goes through a symlink is refused, and file collisions between
     packages are reported.
   - Extraction is Python `tarfile`, not the host `tar`, so the result doesn't depend on which
     `tar` or `bsdtar` a machine has.
5. The script prints the content hash, writes the per-entry listing to
   `~/.cache/powerarm/ArchLinuxARM-m2.contents` (diff two machines' listings to find a
   divergence), and prints the `p_align` report.

Useful commands:

- `alarm_sysroot.py hash <dir>` recomputes a content hash.
- `alarm_sysroot.py align <dir>` reprints the page-alignment report.

**Content hash.** This is the sha256 over the lines `d MODE path`, `f MODE SHA256 path` and
`l TARGET path` for every entry below the root, sorted bytewise by path. mtimes and ownership
aren't included.

**Result (2026-09-16 pins):**

- 90 packages: 118.6 MB downloaded, 602.9 MB installed, 27,361 entries.
- GCC 16.1.1, binutils 2.46, glibc 2.43, make 4.4.1, linux-api-headers 7.2.
- The content hash was identical on both machines (see "Reproducibility of the sysroot").

### Reproducibility of the sysroot

Built independently from the same manifest on the Pi 5 (Raspberry Pi OS, 16K kernel, Python
3.13) and on the POWER9 (Arch POWER, 64K kernel), each with its own downloads:

```
entries 27361
content-hash sha256:0f4a923096d21697dab39bc13172118a5db96b6ce3c2a6e998a270001bf577d5
```

### Install scriptlets and hooks that are not run

pacman would run the following `.INSTALL` scriptlets. None of them is needed for the toolchain:

| Package | Scriptlet does | Why it is not needed |
|---|---|---|
| glibc | `locale-gen`, `ldconfig -r .`, `iconvconfig` | No `/etc/ld.so.cache`: `ld.so` falls back to its built-in `/usr/lib` search, and every library is there. Only the `C` locale is used (`LC_ALL=C`). There is no `gconv-modules.cache`, so iconv reads the text config instead. |
| bash | appends shells to `/etc/shells` (upgrade only) | `filesystem` already ships `/etc/shells` |
| p11-kit | enables a systemd user socket | no systemd |
| ca-certificates-utils, libxcrypt, tar | upgrade messages or migrations | fresh install |

pacman's post-transaction hooks (`update-ca-trust`, `systemd-sysusers`, `systemd-tmpfiles`,
the info and man page index) aren't run either. As a result, there's no generated CA bundle,
which only matters for TLS, and no pacman local database under `/var/lib/pacman/local`. The
`bin`, `lib` and `sbin` → `usr/...` symlinks come straight from the `filesystem` package's
tar, so no scriptlet is needed for them.

### Why the closure has 90 packages

The closure is computed from the repo databases the way pacman would, with no hand-pruning.
`coreutils` pulls in `openssl` and `acl`, `make` pulls in `guile`, and `binutils` pulls in
`jansson` and `libelf`. Through `libelf`/`curl`, the tree also brings in `krb5`, `gnutls` and
`systemd-libs`. Roots are the `root` lines in the manifest. When a dependency has several
providers, the first one in repo order wins, and each such choice is logged. Use
`--prefer PROVIDE=PKG` to override it.

### Re-pinning

```sh
build-alarm-sysroot.sh --resolve     # rewrites alarm-m2.manifest from today's core/extra/alarm dbs
git diff Scripts/powerarm/rootfs/alarm-m2.manifest
```

Arch Linux ARM mirrors keep **only the current version** of each package, and the community
archive (tardis.tiny-vps.com/aarm) shut down in June 2026. There's no snapshot mirror to pin
against. The manifest therefore pins exact filenames plus sha256, and it records the sha256
and `Last-Modified` time of each repo database the pins were resolved from, for information.
**Keep the package cache:** once upstream moves on, `fetch` can only succeed from a cache that
already holds the files. Copy `~/.cache/powerarm/alarm-pkgs/` between machines to share it
(for example with `rsync`). `--mirror URL` can also point at any `http://` or `file://` tree laid out as `<repo>/<file>`.

## Manifest format (`alarm-m2.manifest`)

The manifest has one whitespace-separated record per line, and `#` starts a comment. The header
comment in the file documents every key. In short:

```
format 1
arch aarch64
mirror http://mirror.archlinuxarm.org/aarch64
keyring-fingerprint <40-hex fingerprint>
db <repo> <sha256> <Last-Modified>                 informational
root <pkg>...                                      closure roots
basetar <url> <sha256> <size>                      only for --base-tarball
keyring <name> <ver> <repo> <file> <sha256> <csize> <isize>
pkg <name> <ver> <repo> <file> <sha256> <csize> <isize>
project <name> <version> <url> <sha256>            M2 upstream sources
```

Download URLs are `<mirror>/<repo>/<file>` and `<mirror>/<repo>/<file>.sig`.

## Native runner (`run-in-sysroot.sh`)

```sh
run-in-sysroot.sh [--cwd-as PATH] [--env K=V] [--bind SRC DST] <sysroot> <cwd> -- cmd args...
```

- **Sandbox:** the command runs with the sysroot as `/`, mounted **read-only**. It gets new
  user, mount, pid, ipc, uts and net namespaces, so there's no network, and hostname
  `powerarm`.
- **Mounts:** there's a private `/proc`, a minimal `/dev` (null, zero, full, random, urandom,
  tty, pts, shm) and an empty tmpfs `/tmp`.
- **Working directory:** `<cwd>` is bound read-write. By default it appears at the **same
  absolute path** as on the host, and the top-level directory above it (e.g. `/home`) is
  covered by a tmpfs to hold the mount point. That's the path a program running under POWERarm
  sees, so the two runs agree on `$PWD`. `--cwd-as /work` chooses a fixed path instead.
- **Backends:** `bwrap` is the default. `--backend unshare` uses `unshare -r` with private
  mounts and `chroot`; it has no extra binds, and the top-level directory of the in-sandbox
  cwd must exist in the sysroot.
- The exit status of the command is passed through. The sysroot is never modified, and its
  content hash doesn't change after runs.

**Fixed environment** (the caller's environment is cleared):

```
PATH=/usr/local/sbin:/usr/local/bin:/usr/bin  LC_ALL=C  LANG=C  TZ=UTC  SOURCE_DATE_EPOCH=0
HOME=/tmp  TMPDIR=/tmp  SHELL=/bin/sh  TERM=dumb  PWD=<in-sandbox cwd>   umask 022
```

**Proof on the Pi 5** (16K kernel, bwrap and unshare backends give the same result):

```
$ run-in-sysroot.sh ~/.local/share/powerarm/RootFS/ArchLinuxARM-m2 ~/powerarm-m2-proof -- \
    sh -c 'gcc --version | head -1; gcc -c hello.c && gcc hello.o -o hello && ./hello; make --version | head -1'
gcc (GCC) 16.1.1 20260430
hello from alarm gcc
GNU Make 4.4.1
```

## Determinism

This was measured on the Pi with a two-file C program (`a.c` calls `add()` in `b.c`) built
twice with `make`: once in two separate directories with the same in-sandbox path, and once
at a different path. The outputs were an object, a static archive, a shared library and
executables.

| Artifact | Same path, 2 runs | Different path |
|---|---|---|
| `a.o`, `b.o`, `libadd.a`, `libadd.so`, `prog` (`-O2`) | identical | identical |
| `prog-g` (`-g -O2`) | identical | **differs** (DW_AT_comp_dir / DW_AT_name) |
| `-g -ffile-prefix-map=$PWD=.` (C and C++ with an anonymous namespace) | identical | identical |
| `-flto` executable | identical | identical |

Findings:

- **`ar`:** Arch's binutils is built with deterministic archives as the default. `ar --help`
  says `[D] - use zero for timestamps and uids/gids (default)`, and `ar tv` shows
  `rw-r--r-- 0/0 ... Jan 1 00:00 1970`. `ar rcu`/`rcs` need no extra flag, but `U` would turn
  it off.
- **`__DATE__`/`__TIME__`** follow `SOURCE_DATE_EPOCH`; the runner sets 0. Without the
  variable they embed the wall clock.
- **Build IDs** (`--build-id`, on by default) hash the output contents, so they're stable.
- **Paths:** without `-g`, nothing path-dependent was embedded. `__FILE__` is the path as
  given on the command line, which is relative in these Makefiles. With `-g`, the
  compilation directory is embedded. Either run the Pi reference and POWERarm with the same
  absolute path (the runner's default, provided the harness uses the same work path on both
  machines) or add `-ffile-prefix-map=$PWD=.`.
- **`-frandom-seed`** wasn't needed. GCC derives the random seed from the output file name,
  and C++ anonymous-namespace symbols and LTO were stable across runs and paths. Set
  `-frandom-seed=<object name>` only if a difference shows up.
- **The GCC defaults are identical on both sides** because they come from the sysroot's
  `specs`: `--enable-default-pie`, `--enable-default-ssp` and `-fstack-protector-strong`.
- **Still to watch for:**
  - `uname -r`/`-m` and `/proc/cpuinfo` differ between the Pi and POWERarm. zlib's
    `configure` and Lua's Makefile don't embed them, but check any `config.log`-derived
    header.
  - Under `make -j`, archive member order is fixed by the Makefile's variables, not by
    completion order. Only wildcard-derived lists depend on `readdir` order.
  - The hostname is fixed at `powerarm` in the runner.

## M2 upstream sources (`fetch-m2-projects.sh`)

```sh
fetch-m2-projects.sh [--cache DIR] [zlib|lua]   # default cache ~/.cache/powerarm/m2-sources
```

| Project | Version | URL | sha256 |
|---|---|---|---|
| zlib | 1.3.2 | https://zlib.net/fossils/zlib-1.3.2.tar.gz | `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16` |
| Lua | 5.4.9 | https://www.lua.org/ftp/lua-5.4.9.tar.gz | `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6` |

Both hashes match the values published on zlib.net and lua.org. Nothing is built or unpacked.

## How POWERarm uses the sysroot

- **Selecting it:** POWERarm's `RootFS` option (`Emulation.RootFS` in
  `FEXCore/Source/Interface/Config/Config.json.in`, environment `POWERARM_ROOTFS`) takes either
  a path or a **name** that's looked up under `$XDG_DATA_HOME/powerarm/RootFS/<name>/`.
  `POWERARM_ROOTFS=ArchLinuxARM-m2` and
  `POWERARM_ROOTFS=$HOME/.local/share/powerarm/RootFS/ArchLinuxARM-m2` are equivalent
  (DESIGN §6.2a).
- **Running a program:** guest absolute paths resolve inside the rootfs first. The harness runs
  e.g. `POWERARM_ROOTFS=ArchLinuxARM-m2 powerarm /usr/bin/make` in the work directory. On the
  Pi, the reference side is
  `run-in-sysroot.sh ~/.local/share/powerarm/RootFS/ArchLinuxARM-m2 <workdir> -- make`.
- **Checking the tree:** a harness can confirm it has the expected tree with
  `alarm_sysroot.py hash <sysroot>`. It must print the content hash recorded for the manifest
  in use; see the commit that pinned it.
- **Page size:** every aarch64 ELF with a PT_LOAD is **64K-aligned**. That's 881 files (859
  DYN, 22 EXEC) with 1,762 PT_LOAD segments, all with `p_align` 0x10000; none is below 64K.
  The 18 `ET_REL` objects (`crt*.o` and similar) have no PT_LOAD. The 344 `.go` files are Guile
  bytecode with `e_machine` 0 and are never loaded by the kernel or `ld.so`. The counts were
  cross-checked with `readelf -lW`. This confirms DESIGN §4.9/§6.2a: `POWERARM_HOSTPAGEMODE=auto`
  runs the whole toolchain without granule emulation on both 4K and 64K hosts, and one image
  serves both.
- **4K KVM guest:** the tree is a plain directory, so share it as-is (virtiofs/9p) or pack it
  (`mkfs.erofs`/`mksquashfs`). Setuid bits are present on some files (e.g. `su`); the toolchain
  doesn't need them.

## Official rootfs tarball as a base layer (evaluated)

`build-alarm-sysroot.sh --base-tarball --dest <dir>` first lays down the official
`ArchLinuxARM-aarch64-latest.tar.gz` (pinned in the `basetar` line), then extracts the pinned
packages on top of it.

This mode was measured on the Pi on 2026-09-16, with the tarball dated 2026-08-05. **The
package-closure path remains the default**:

| | Package closure (default) | Official tarball + pinned packages |
|---|---|---|
| Pinning | exact filename plus sha256 for every package | sha256 of one 829 MB `-latest` file |
| Archive of old versions | none (the mirror keeps only the current version; keep the cache) | **none**: only `-latest` is published, with no dated names, and the tardis archive is gone. The pin breaks at the next rebuild (roughly monthly), and the old image can't be downloaded again. |
| Signature without root | yes, per package, `gpgv` with the pinned key | yes: the `.sig` is made by the same build key and verified the same way (the published `.md5` is also OK) |
| Download / tree size | 118.6 MB / 646 MB, 27,361 entries | 829 MB + 118.6 MB / 2.4 GB, 52,949 entries (166 packages, including kernel, firmware, systemd, dbus, iproute2) |
| Toolchain in it | complete | none: the tarball has no `gcc`, `make` or headers beyond its own packages; the pinned packages supply them |
| Rootless extraction | clean | works, but some files are root-only (e.g. `dbus-daemon-launch-helper` mode 4110), so the hasher has to chmod them to read them |
| Consistency | exactly the pinned closure | mixed versions: 23 of the 83 overlapping packages are older in the tarball, and overlaying leaves **stale files** that the newer versions removed. Among them are 12 old `usr/include/linux` headers from linux-api-headers 7.1 and old sonames from pcre2, xz, libelf and libldap. `var/lib/pacman/local` then describes the old versions. |

**Effect on the toolchain files:** none. All 27,361 entries of the package-only tree are
byte-identical in the tarball-based tree, because pinned packages overwrite the tarball.
There are still 25,588 **extra** entries, including 926 headers under `usr/include` (e.g.
`glib-2.0`, `libnl3`, `libxml2`, stale `linux/` headers) and 241 extra `usr/lib` libraries.
These extras can change `configure`-style feature probes, so builds aren't guaranteed to
match the package-only sysroot.

The tarball would only be better for a general-purpose Arch userland (pacman, systemd, a
login-ready `/etc`). For byte-compared M2 builds it's larger, it can't be re-downloaded once
superseded, and it adds uncontrolled headers and libraries.

## OCI/Docker images (`oci-extract.sh`)

```sh
oci-extract.sh debian:trixie-slim ~/.local/share/powerarm/RootFS/debian-trixie-slim
oci-extract.sh docker.io/library/debian@sha256:<index digest> <dest>      # pinned
```

- **Registry access:** the script talks to the registry HTTP API directly, with no Docker and
  no root. It uses the anonymous bearer-token flow from `WWW-Authenticate`, which covers Docker
  Hub, ghcr.io, quay.io and token-less registries. The token isn't forwarded on blob
  redirects to CDNs.
- **Manifest selection:** it picks `linux/arm64` from an OCI index or a Docker manifest list
  (`--platform` to change), preferring variant `v8`/none.
- **Verification:** it checks the sha256 of the index, the manifest, the config and every
  layer blob. A `@sha256:` reference must match the fetched index or manifest. Blobs are cached
  in `~/.cache/powerarm/oci/blobs/sha256/`.
- **Layers:** layers (`tar`, `tar+gzip`, `tar+zstd`) are applied in order. `.wh.<name>`
  deletes `<name>` from lower layers. `.wh..wh..opq` empties a directory of lower-layer
  entries but keeps the ones the same layer adds.
- **Paths:** they're resolved like a chroot, so symlinks such as Debian's `/bin → usr/bin`
  stay inside the tree.
- **Not preserved:** ownership isn't kept, and device nodes and FIFOs are skipped and counted.
- **Record:** `<dest>.oci-manifest` records the index digest, manifest digest, config digest,
  layers, a pinned `registry/repo@sha256:` reference and the tree's content hash.

**Proof on the Pi:**

- The pinned image was `debian:trixie-slim`, index
  `sha256:d7e12182ce18b85b93007c1dedf31f2d29e01ccf3182cc4017c709b6259bc132` (arm64 manifest
  `sha256:7215f78f35ffe58fe13f244fac9c4f21326d55187271fbb3e1a8aa5cc7e387ab`, one layer).
- It extracted to 3,256 entries with content hash
  `sha256:7c3db726e5f40ec82b7622ffe81437d92ad49d5b74dafa83bbf8b6bffcb11739`. Extracting by tag
  and by digest gave the same hash.
- `run-in-sysroot.sh <dest> <dir> -- /bin/ls --version` prints `ls (GNU coreutils) 9.7`, and
  `/etc/debian_version` is 13.6.
- A synthetic three-layer test covered an opaque directory, file and directory whiteouts, and
  a write through a lower-layer symlink.
