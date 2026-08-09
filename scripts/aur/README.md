# AUR packaging

Arch Linux packaging for mcpp. Three packages, same runtime layout:

| Package | What it installs | Pick it when |
| --- | --- | --- |
| [`mcpp-bin`](mcpp-bin/) | the **prebuilt** release binary (what [`install.sh`](../../install.sh) downloads) | you just want mcpp, fast |
| [`mcpp-m`](mcpp-m/) | mcpp **built from source**, bootstrapped with `mcpp-bin` | you want a from-source build of a release |
| [`mcpp-git`](mcpp-git/) | mcpp **built from the git master**, bootstrapped with `mcpp-bin` | you want to track master (rolling) and never lag the index floor |

```sh
yay -S mcpp-bin      # prebuilt
yay -S mcpp-m        # from source (pulls mcpp-bin as a build dep)
yay -S mcpp-git      # from git master (pulls mcpp-bin as a build dep)
```

The three `conflict` with each other, so only one can be installed at a time.
Supported architectures: `x86_64`, `aarch64`.

### Why not just `mcpp`?

The name `mcpp` is already taken by **`extra/mcpp`** — Matsui's C preprocessor,
an unrelated long-standing official Arch package that owns `/usr/bin/mcpp`. The
AUR refuses to host any package whose `pkgname` (or `provides`) collides with an
official-repo package, so our packages are `mcpp-bin` / `mcpp-m` / `mcpp-git`.
They still install the `mcpp` command at `/usr/bin/mcpp`, so all three
`conflicts=('mcpp')` with that preprocessor — you can have our mcpp or the
preprocessor, not both.

## Layout & why the wrapper exists

mcpp ships as a single self-contained tree. At runtime it **writes** into
`MCPP_HOME` — the registry sandbox, BMI/metadata caches, logs, and every
toolchain it downloads. That has to be per-user and writable, so it cannot live
under a root-owned system prefix.

mcpp resolves `MCPP_HOME` from the running binary's *real* path
(`/proc/self/exe`, which resolves symlinks). A plain `/usr/bin/mcpp` symlink
would therefore make `MCPP_HOME` resolve into the read-only install dir and
every command would fail to write. So all three packages split the tree:

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

### How the `mcpp-m` / `mcpp-git` source packages build

mcpp is self-hosting. Both source PKGBUILDs use the installed `mcpp-bin` as the
bootstrap compiler and run `mcpp build --target <arch>-linux-musl` — the same
path [`release.yml`](../../.github/workflows/release.yml) ships. mcpp downloads
its own pinned toolchain (it does **not** use the host gcc), so the build needs
network access, like the upstream release build.

`mcpp-m` builds a pinned release tarball; `mcpp-git` checks out `master` from
the official repo (`source = mcpp::git+…`, `sha256sums = SKIP`) and derives a
monotonic `pkgver` from `git describe` (tag + commit distance). Because it
tracks master, `mcpp-git` can never lag the index floor the way `mcpp-bin` does
between releases — at the cost of tracking bleeding-edge master.

## Files

```
scripts/aur/
  README.md            this file
  update.sh            render + verify mcpp-bin from mcpp-release.json
  render_mcpp_bin.py   pure manifest-to-PKGBUILD renderer
  reconcile_mcpp_bin.py desired/observed-state reconciler
  aur.archlinux.org.known_hosts pinned production host key
  mcpp-bin/{PKGBUILD, .SRCINFO, mcpp.sh}
  mcpp-m/{PKGBUILD, .SRCINFO, mcpp.sh}
  mcpp-git/{PKGBUILD, .SRCINFO, mcpp.sh}
```

`mcpp.sh` is currently identical in all three dirs because each AUR repo must
be self-contained. Automation in this phase owns **only `mcpp-bin`**.
`mcpp-m` is frozen and is neither read, rendered, staged, nor pushed by the
reconciler; `mcpp-git` is also outside this release workflow.

## Releasing a new version

Every complete stable GitHub release publishes immutable `mcpp-release.json`.
Render the checked-in `mcpp-bin` package from that desired state (Docker is
required so `.SRCINFO` comes from a non-root Arch `makepkg`, not a handwritten
fallback):

```sh
scripts/aur/update.sh                 # latest complete stable release
scripts/aur/update.sh 2026.8.10.1      # accepted only if it is that exact latest tag
```

The renderer downloads both Linux payloads and sidecars, recomputes their
SHA256 values against the manifest, rewrites only `mcpp-bin`, runs
`makepkg --printsrcinfo`, and then runs `makepkg --verifysource`. There is no
version-from-worktree fallback and no downgrade override.

For a read-only live reconciliation (including AUR RPC and HTTPS git state):

```sh
python3 scripts/aur/reconcile_mcpp_bin.py \
  --trigger local \
  --report-json /tmp/mcpp-aur-report.json
```

Without `--publish`, an upgrade is rendered, source-verified, and shown as an
exact dry-run diff but never pushed.

### Test locally (on Arch)

```sh
cd scripts/aur/mcpp-bin && makepkg -si        # prebuilt
cd scripts/aur/mcpp-m   && makepkg -si        # from source (slow: builds mcpp)
mcpp --version
```

## Automated publishing (CI)

[`.github/workflows/aur-publish.yml`](../../.github/workflows/aur-publish.yml)
reconciles `mcpp-bin` only. It runs after a successful `release` workflow, every
six hours to recover from transient AUR outages, and on manual dispatch. Manual
dispatch defaults to dry-run and requires `publish=true` to push; the two
automatic triggers plan and report but do **not** push until the repository
variable `AUR_AUTOPUBLISH` is set — see
[Arming the automatic triggers](#arming-the-automatic-triggers).

Every trigger follows the same state machine:

1. Select the latest complete, non-draft, non-prerelease manifest.
2. Validate both Linux payloads and sidecars and render in a clean directory.
3. Generate `.SRCINFO` and verify sources as a non-root user in Arch.
4. Query AUR RPC and clone the known `mcpp-bin` repo over HTTPS.
5. Compare versions with Arch `vercmp` and print the exact diff before the SSH
   secret is loaded.
6. Refuse downgrades; otherwise use a normal fast-forward SSH push with bounded
   maintenance retry and a pinned AUR host key.
7. Verify public git HEAD, bounded-poll RPC, then install and run
   `mcpp --version` in a clean Arch container.

The result is classified as `noop`, `updated`, `transient`, `permanent`, or
`refused-downgrade`, and the Actions summary records versions, hashes, commit,
retry count, and drift age. AUR runs are downstream workflows: a failure is
visible and retried by schedule but cannot rewrite the completed GitHub release
conclusion.

> The AUR has no "watch upstream" feature — packages only update when their git
> repo is pushed. This workflow is that push.

### One-time setup you need to do

1. **AUR account** — sign in at <https://aur.archlinux.org> with the account
   that owns `mcpp-bin`.

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

That's the only publishing secret required — AUR auth is SSH-key based, there
is **no API token**. GitHub's read-only token is used only to fetch the release
manifest/assets. The private key is not loaded during the inspect/dry-run step.
The server host key is checked against the vendored ED25519 key sourced from
Arch Linux's infrastructure repository; the workflow never uses
`ssh-keyscan` as a trust decision.

### Arming the automatic triggers

`schedule` fires every six hours off the default branch. That means merging
this workflow is, by itself, enough to start writing to a third-party service
unattended — potentially before anyone has watched the reconciler complete a
real push even once. Merging is a decision about code; publishing to the AUR is
a decision about the outside world, and the two should not be the same act.

So both automatic triggers (`workflow_run` after a release, and `schedule`)
stop after the plan-and-report step unless the repository variable
`AUR_AUTOPUBLISH` is set to `true`. Dry runs still validate payloads, render
`.SRCINFO`, query the AUR and print the exact diff, so the reporting value is
unchanged — only the push is withheld.

To arm it, once:

1. Run the workflow manually with `publish=false` and read the summary: it must
   show the intended version and a clean diff.
2. Run it manually with `publish=true` and confirm the push, the AUR RPC row,
   and a clean install in an Arch container.
3. Only then set *Settings → Secrets and variables → Actions → Variables →*
   `AUR_AUTOPUBLISH = true`.

Unsetting the variable is the kill switch: automatic runs immediately fall back
to reporting without publishing, with no code change and no revert.

### First publish

The reconciler treats `mcpp-bin` as a known existing package. A failed clone or
missing RPC row is an error and **never** becomes an empty-repository first
publish. If a new package ever needs to be claimed, do that explicitly and
review the initial history by hand:

```sh
git clone ssh://aur@aur.archlinux.org/mcpp-bin.git
cp scripts/aur/mcpp-bin/{PKGBUILD,.SRCINFO,mcpp.sh} mcpp-bin/ && cd mcpp-bin
git add PKGBUILD .SRCINFO mcpp.sh
git commit -m "initial mcpp-bin" && git push
```

After the known repository exists, the reconciler never force-pushes or
reinitializes it. Manual recovery is available from *aur-publish → Run
workflow*: first leave `publish=false` to inspect, then set `publish=true` only
for the exact latest complete stable tag.
