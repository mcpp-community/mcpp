# 09 — Releasing mcpp

How a release of **mcpp itself** reaches users. This is maintainer-facing; for
packaging *your own* project see [02 — Packaging for Release](02-pack-and-release.md).

Until now this process lived only in commit messages and workflow comments. One
of those commit messages contains a misdiagnosis that is corrected in §5.

## 1. The four version sites are two groups

| Site | Group | Moves when |
|---|---|---|
| `mcpp.toml` `[package].version` | **being built** | you start work on a new version |
| `src/toolchain/fingerprint.cppm` `MCPP_VERSION` | **being built** | same commit as above (compiled-in copy) |
| `.xlings.json` `[workspace].mcpp` | **bootstrapped from** | separately, *after* a release is installable |
| `ci-fresh-install.yml` `MCPP_PIN` | ~~bootstrapped from~~ | **nothing — it is derived at run time** (§4) |

`.github/tools/check_version_pins.sh` enforces what is left mechanically. The two
"being built" sites must be equal; the bootstrap pin must never be **newer** than
the version being built.

The two groups are deliberately allowed to differ. Bumping them together is what
an earlier revision of the pin checker required, and it sent every CI job to
install a version that did not exist yet.

## 2. The pipeline

`release.yml` (tag push, or `workflow_dispatch` with no input, which derives the
tag from `mcpp.toml`) does all of this:

```
build ×4 (linux x86_64 / linux aarch64 / macOS ARM64 / Windows x64)
  → GitHub Release v<version> with tarballs + .sha256 sidecars
  → mirror to xlings-res/mcpp on BOTH GitHub and GitCode
  → open the version-bump PR against openxlings/xim-pkgindex
  → workflow_run hook fires ci-fresh-install
```

Two steps are **not** automated:

- **merging the xim-pkgindex bump PR** — a maintainer does it. Until it lands,
  the released version is downloadable but not installable via `xlings install`.
- **bumping `.xlings.json`** — see §4.

## 3. Verifying a release

The mirror script verifies its own uploads, but the checks worth doing by hand
are the ones that do not trust a sidecar:

```bash
V=<version>
# both hosts serve every platform, byte-exact
for a in linux-x86_64.tar.gz linux-aarch64.tar.gz macosx-arm64.tar.gz windows-x86_64.zip; do
  for h in github.com gitcode.com; do
    curl -fsSL -o /dev/null -w "$h $a %{http_code} %{size_download}\n" \
      "https://$h/xlings-res/mcpp/releases/download/$V/mcpp-$V-$a"
  done
done
# the index's sha256 values match the payloads (recompute; do not read the sidecar)
curl -fsSL -o /tmp/p.tgz "https://github.com/xlings-res/mcpp/releases/download/$V/mcpp-$V-linux-x86_64.tar.gz"
sha256sum /tmp/p.tgz   # compare against pkgs/m/mcpp.lua in xim-pkgindex
```

Then a real install, in a **clean-room `XLINGS_HOME`** — never the machine's own
`~/.xlings`, which can mask a broken index with cached state:

```bash
export XLINGS_HOME=$(mktemp -d)
xlings update
xlings install mcpp@$V -y
$(find "$XLINGS_HOME" -name mcpp -type f -path '*/bin/*' | head -1) --version
```

**Index propagation is not instant.** `xim-pkgindex` reaches clients as a CDN
artifact, not a git clone, so a freshly merged bump is invisible for a while
(measured at ~5 min on 2026-07-30; the documented worst case is ~40 min). A
clean-room that still reports the old `latest` has not failed — it has not caught
up. `ci-fresh-install`'s `wait-index` job encodes exactly this with a bounded
15-minute wait.

## 4. The bootstrap pin: what it is, and when to bump it

`.xlings.json`'s `[workspace].mcpp` is the **starting point of self-hosting** —
the released mcpp that `xlings install mcpp` puts in the workspace so CI can build
mcpp from source. Its only requirement is that it can build the **current** tree.

**It does not have to move with every release.** The index retains every
published version (105 entries at the time of writing, back to the 0.0.x series),
so an older pin keeps resolving indefinitely — verified by installing a
two-releases-old version against the current index.

Bumping it anyway is reasonable and is what this repository does in practice: a
green CI round on the bumped pin is a direct proof that the new release can build
mcpp itself on every platform. Treat it as a *useful check*, not a prerequisite.

**The one hard constraint is direction**: the pin must never name a version that
is not yet installable. Bump it only after the release is published, mirrored,
**and merged into xim-pkgindex** — otherwise every CI job fails with
`package 'mcpp@<unreleased>' not found`. `check_version_pins.sh` enforces the
weaker "never newer than the version being built"; the index condition is on you.

## 5. `MCPP_PIN` is derived, and why that matters

`ci-fresh-install.yml` used to carry a second hand-edited copy of the pin. It was
never the same thing: `MCPP_PIN` is the version **under test** — always the newest
published release — while `.xlings.json` is the version **bootstrapped from**.

It is now derived once, by the `wait-index` job, from the releases API, and every
install job consumes that single output. Two properties had to hold, and the
hardcoded literal only bought the first:

1. **The version must be explicit.** Bare `xlings install mcpp` resolves "newest
   in the runner's index copy", so a lagging runner silently tests an *older*
   binary and reports green. Naming the version makes a lagging index fail loudly
   with `version not found`. A derived string is every bit as explicit as a
   literal one.
2. **The guard and the jobs must name the same version.** On 2026-07-21 they did
   not: the index guard reported "index tracks 0.0.102" while the jobs installed
   0.0.100 ten seconds later, meeting an index whose floor was 0.0.101 (#265).
   The guard was already deriving the right answer and throwing it away. Feeding
   both from one value makes that disagreement structurally impossible.

`check_version_pins.sh` fails if a literal `MCPP_PIN:` reappears.

> **Correction.** Commit `3b1cb6b` ("bootstrap pin -> 2026.7.29.2") states *"the
> index no longer serves .1"* and quotes `version '2026.7.29.1' not found`. That
> diagnosis is wrong: `2026.7.29.1` installs fine from the current index. The real
> cause was a **stale local index copy** — the same propagation lag described in
> §3, seen from the other side. Nothing about a release removes older versions,
> and no reasoning should be built on the idea that it does.

## 6. Checklist

```
[ ] version bumped in mcpp.toml + fingerprint.cppm (one commit)
[ ] CHANGELOG entry
[ ] bash .github/tools/check_version_pins.sh
[ ] merge to main, CI green
[ ] gh workflow run release.yml --ref main
[ ] release.yml green (4 builds + publish-ecosystem)
[ ] mirrors serve all four platforms on BOTH hosts, sha256 recomputed
[ ] merge the xim-pkgindex bump PR
[ ] clean-room XLINGS_HOME: xlings install mcpp@<version> succeeds
[ ] (optional) bump .xlings.json — only now, never earlier
```
