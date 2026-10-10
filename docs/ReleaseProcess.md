# Release process

How POWERarm ships. Releases are cut when there is something worth shipping;
there is no cadence, no PPA, no website and no rootfs bucket operated here.

## Branch model

`main` is the **rolling** development branch. Work lands there directly.

A release is a `release/x.y` branch pinned off `main`, plus an annotated
`x.y.z` tag on the same commit. The release branch exists so the released state
has a name that does not move, and so `POWERARM_GIT_BRANCH=release/x.y` builds
something reproducible. Nothing is merged back from a release branch unless it
carries a fix made on it.

Existing:

| ref | at |
| --- | --- |
| `release/0.10` | the `0.10.0` tag |
| `release/0.12` | the `0.12` release point |

The remote is `https://github.com/jbettcher-wg/POWERarm.git`, `origin` in the
maintainer's checkout.

Tags are semver, annotated, named `x.y.z` with no prefix — `0.10.0` is the
precedent. Package versions are independent: `pkgver()` in the PKGBUILD derives
one from the checkout and does not read tags (see step 4).

## 1. Pre-flight

From a clean checkout of `main`.

```sh
git status --short                  # must be empty
```

The gates, which are the actual pre-flight. Run all of them and compare against
the recorded baseline in `HANDOVER.md`:

```sh
G=~/Development/.powerarm-golden/claude-simd
E=$PWD/build-powerarm/Bin/POWERarm

sh unittests/A64Frontend/run.sh "$E" "$G"                              # default
POWERARM_MAXINST=1               sh unittests/A64Frontend/run.sh "$E" "$G"
POWERARM_HOSTFEATURES=disableisa30 sh unittests/A64Frontend/run.sh "$E" "$G"
POWERARM_VSXCLASSES=0            sh unittests/A64Frontend/run.sh "$E" "$G"

sh Scripts/powerarm/check-code-cache.sh   "$PWD/build-powerarm"   # run ALONE
sh Scripts/powerarm/check-config-errors.sh "$PWD/build-powerarm"
sh Scripts/powerarm/check-rootfs-server.sh
sh Scripts/powerarm/check-user-strings.sh
sh unittests/MemoryModel/check.sh
```

Two things about that list. `POWERARM_VSXCLASSES=0` is a mode in its own right
because the machine's `~/.config/powerarm/Config.json` turns that switch **on**,
so the default run is a switch-on run and the off path is otherwise never
exercised. And `check-code-cache.sh` must not run beside the suite — they
contend for the same cache directory.

Refresh the generated source outline, which is derived from `$info$` tags and
drifts as files are added or removed:

```sh
Scripts/generate_doc_outline.sh > docs/SourceOutline.md
git commit docs/SourceOutline.md -m "docs: regenerate SourceOutline"
```

## 2. Install and smoke-test what you are about to tag

The package build is not the development build, so a tag should not be the first
time the release configuration runs. Promote and register the candidate and use
it:

```sh
sh Scripts/powerarm/promote-stable.sh -n        # check, then without -n
sudo sh Scripts/powerarm/register-binfmt.sh     # or: systemctl restart systemd-binfmt
bash Scripts/powerarm/check-binfmt-inode.sh     # proves WHICH inode binfmt runs
```

That last step is not optional and its output is the only honest one. The
binfmt entry carries the `F` flag, so it opens the interpreter at registration
time, while promoting replaces the install by moving directories and therefore
changes the inode. Skip the re-register and binfmt keeps running the previous
emulator while still printing the new path.

Then launch something real — a browser and an editor are the useful cases — and
close them. Closing matters: the teardown path is where shutdown bugs show up.

## 3. Cut the branch and tag

```sh
VER=0.12            # x.y
TAG=$VER.0          # x.y.z

git push origin main:refs/heads/release/$VER
git tag -a "$TAG" -m "POWERarm $TAG"
git push origin "$TAG"
```

Both are fast-forwards or new refs; nothing here rewrites history. Push the
branch before the tag so the tag never names a commit with no branch on it.

## 4. Package version metadata

`pkgver()` recomputes the version from the checkout at build time, so the
committed `pkgver` is metadata only — but `.SRCINFO` should not ship stale.

```sh
cd packaging/archpower
# the value a fresh checkout produces:
#   printf 'r%s.%s' "$(git rev-list --count HEAD)" "$(git rev-parse --short=7 HEAD)"
makepkg --printsrcinfo > .SRCINFO
git commit PKGBUILD .SRCINFO -m "packaging: refresh version metadata for $TAG"
```

Reset `pkgrel=1` whenever `pkgver` changes; bump `pkgrel` and leave `pkgver`
alone when only the packaging changed. Regenerate `.SRCINFO` with the default
`POWERARM_GIT_URL` so no machine-local path is baked in.

`pkgver()` still uses the commit-count form and its comment still says no
release tags exist. They do now, so switching it to `git describe --long --tags`
is an open decision rather than a step here.

## 5. Build and check the package

On an Arch POWER host, from a clean checkout:

```sh
cd packaging/archpower
POWERARM_GIT_BRANCH=release/$VER makepkg -s
```

**Use the host `makepkg`.** A shell inside the AArch64 rootfs resolves
`makepkg` and its `bash` out of the rootfs, which runs the whole build under
emulation against the wrong architecture. `uname -m` is the check —
`powerpc64le` is the host, `aarch64` means the rootfs. `arch=('powerpc64le')`
refuses the rootfs copy, but only after sourcing the PKGBUILD, so read the
architecture named in the error rather than just the exit code.

Inspect without installing:

```sh
namcap powerarm-*.pkg.tar.zst
pacman -Qlp powerarm-*.pkg.tar.zst
```

Check the host thunks are present — they are `dlopen`'d, so nothing else
notices when they are missing:

```sh
pacman -Qlp powerarm-*.pkg.tar.zst | grep HostThunks
```

There is no `GuestThunks/` to check: this recipe sets
`BUILD_GUEST_THUNKS=OFF`.

Then install it, `systemctl restart systemd-binfmt`, and launch one known-good
guest.

## 6. GitHub release

```sh
gh release create "$TAG" --repo jbettcher-wg/POWERarm --title "POWERarm $TAG" --generate-notes
```

`--generate-notes` builds notes from merged pull requests. This history is
mostly linear commits rather than PRs, so expect that to be thin.
`Scripts/generate_changelog.sh <previous-tag> "$TAG"` is the commit-level
alternative, but note it only reads `--merges` and so comes back nearly empty on
a linear branch. `Scripts/generate_release.sh` is upstream's monthly `FEX-YYMM`
script and is **not** used here.

## Post-release

Nothing to merge back: `main` kept rolling and the release branch was cut from
it. If a fix is made *on* a release branch, cherry-pick or merge it to `main`
so the two do not diverge silently.
