# AUR packaging

Arch Linux packaging for mcpp. Two packages, same runtime layout:

| Package | What it installs | Pick it when |
| --- | --- | --- |
| [`mcpp-bin`](mcpp-bin/) | the **prebuilt** release binary (what [`install.sh`](../../install.sh) downloads) | you just want mcpp, fast |
| [`mcpp-m`](mcpp-m/) | mcpp **built from source**, bootstrapped with `mcpp-bin` | you want a from-source build |

```sh
yay -S mcpp-bin      # prebuilt
yay -S mcpp-m        # from source (pulls mcpp-bin as a build dep)
```

The two `conflict` with each other, so only one can be installed at a time.
Supported architectures: `x86_64`, `aarch64`.

### Why not just `mcpp`?

The name `mcpp` is already taken by **`extra/mcpp`** — Matsui's C preprocessor,
an unrelated long-standing official Arch package that owns `/usr/bin/mcpp`. The
AUR refuses to host any package whose `pkgname` (or `provides`) collides with an
official-repo package, so our packages are `mcpp-bin` / `mcpp-m`. They still
install the `mcpp` command at `/usr/bin/mcpp`, so both `conflicts=('mcpp')` with
that preprocessor — you can have our mcpp or the preprocessor, not both.

## Layout & why the wrapper exists

mcpp ships as a single self-contained tree. At runtime it **writes** into
`MCPP_HOME` — the registry sandbox, BMI/metadata caches, logs, and every
toolchain it downloads. That has to be per-user and writable, so it cannot live
under a root-owned system prefix.

mcpp resolves `MCPP_HOME` from the running binary's *real* path
(`/proc/self/exe`, which resolves symlinks). A plain `/usr/bin/mcpp` symlink
would therefore make `MCPP_HOME` resolve into the read-only install dir and
every command would fail to write. So both packages split the tree:

| Path | Contents | Mode |
| --- | --- | --- |
| `/opt/mcpp/bin/mcpp` | the mcpp binary | shared, read-only |
| `/opt/mcpp/registry/bin/xlings` | bundled xlings | shared, read-only |
| `/usr/bin/mcpp` | [`mcpp.sh`](mcpp-bin/mcpp.sh) launcher | on PATH |
| `~/.mcpp/` | registry sandbox, caches, toolchains | per-user, writable |

`mcpp.sh` exports `MCPP_HOME=${MCPP_HOME:-$HOME/.mcpp}` and
`MCPP_VENDORED_XLINGS=${MCPP_VENDORED_XLINGS:-/opt/mcpp/registry/bin/xlings}`,
then execs the real binary. mcpp copies the vendored xlings into
`~/.mcpp/registry/bin/xlings` on first run. Both env vars defer to a value the
user already exported, so a custom home or xlings still works.

First `mcpp build`/`mcpp run` bootstraps the sandbox (downloads ninja, patchelf
and the default toolchain into `~/.mcpp`) — expected, and only once per user.

### How the `mcpp-m` source package builds

mcpp is self-hosting. The `mcpp-m` PKGBUILD uses the installed `mcpp-bin` as the
bootstrap compiler and runs `mcpp build --target <arch>-linux-musl` — the same
path [`release.yml`](../../.github/workflows/release.yml) ships. mcpp downloads
its own pinned toolchain (it does **not** use the host gcc), so the build needs
network access, like the upstream release build.

## Files

```
scripts/aur/
  README.md            this file
  update.sh            bump BOTH packages to a release version
  mcpp-bin/{PKGBUILD, .SRCINFO, mcpp.sh}
  mcpp-m/{PKGBUILD, .SRCINFO, mcpp.sh}
```

`mcpp.sh` is identical in both dirs (each AUR repo must be self-contained);
`update.sh` keeps them in sync.

## Releasing a new version

After a GitHub release is published (and mirrored), bump both packages:

```sh
scripts/aur/update.sh            # uses [package].version from mcpp.toml
# or pin: scripts/aur/update.sh 0.0.66
```

`update.sh` pulls the per-arch `.sha256` sidecars (for `mcpp-bin`) and hashes
the source archive (for `mcpp-m`), rewrites `pkgver` + checksums, resets
`pkgrel=1`, and regenerates both `.SRCINFO` files.

### Test locally (on Arch)

```sh
cd scripts/aur/mcpp-bin && makepkg -si        # prebuilt
cd scripts/aur/mcpp-m   && makepkg -si        # from source (slow: builds mcpp)
mcpp --version
```

## Automated publishing (CI)

[`.github/workflows/aur-publish.yml`](../../.github/workflows/aur-publish.yml)
publishes both packages automatically. It runs when the `release` workflow
**completes successfully** (not on `release: published` — the aarch64 asset the
`mcpp-bin` checksum needs is uploaded by a later release job), refreshes the
PKGBUILDs via `update.sh`, and pushes each package to its AUR git repo over SSH.

> The AUR has no "watch upstream" feature — packages only update when their git
> repo is pushed. This workflow is that push.

### One-time setup you need to do

1. **AUR account** — sign in at <https://aur.archlinux.org> with the account
   that will own `mcpp-bin` / `mcpp-m`.

2. **Generate a dedicated SSH key** (no passphrase, it's for CI):

   ```sh
   ssh-keygen -t ed25519 -C "mcpp-aur-ci" -f aur_ci -N ""
   ```

3. **Register the public key** on the AUR: *My Account → Edit → SSH Public Key*
   → paste the contents of `aur_ci.pub` → Update.

4. **Add the private key as a GitHub secret** (repo *Settings → Secrets and
   variables → Actions → New repository secret*):

   - Name: `AUR_SSH_PRIVATE_KEY`
   - Value: the full contents of `aur_ci` (the private key)

   Then delete the local `aur_ci` / `aur_ci.pub` files.

That's the only secret required — AUR auth is SSH-key based, there is **no API
token**. The default `GITHUB_TOKEN` is *not* used (we push to the AUR, not to
GitHub).

### First publish

The first time, the package names must be free. The workflow auto-creates each
AUR repo on first push (AUR does this for a valid, available name). If you
prefer to claim them by hand first, push an initial commit manually:

```sh
git clone ssh://aur@aur.archlinux.org/mcpp-bin.git
cp scripts/aur/mcpp-bin/{PKGBUILD,.SRCINFO,mcpp.sh} mcpp-bin/ && cd mcpp-bin
git add -A && git commit -m "initial mcpp-bin" && git push   # repeat for mcpp-m
```

After that, every release publishes both packages with no manual step. You can
also run it on demand from the Actions tab (*aur-publish → Run workflow*,
optional version input).
