# 10 - Publishing a Library to mcpp-index

**English** | [简体中文](zh/10-publishing-a-library.md)

How a library becomes something `[dependencies]` can name. This is the
*library author's* chain; [09 - Releasing mcpp](09-release.md) is about
releasing mcpp itself, and [02 - Packaging & Release](02-pack-and-release.md)
is about `mcpp pack` bundling an application.

## The chain, in the only order that works

```
your repo          merge → git tag → GitHub auto-generates the tag tarball
      ↓
gitcode mirror     a byte-identical copy, for the CN region
      ↓
mcpplibs/mcpp-index   pkgs/<x>/<name>.lua — GLOBAL + CN URLs + sha256
      ↓            publish-artifact.yml pushes a content-hash artifact
      ↓
consumers          bump the version in their mcpp.toml
```

Each arrow is a gate. Skipping one does not fail loudly — it fails as
"dependency not found" or a 404 in someone else's build, hours later.

## 1. Tag the release

The version in `mcpp.toml` and the tag must agree. GitHub generates
`archive/refs/tags/<tag>.tar.gz` automatically; that tarball **is** the
artifact — nothing needs uploading.

```bash
git tag 0.0.48 && git push origin 0.0.48
curl -fsSL -o pkg-0.0.48.tar.gz \
  https://github.com/<owner>/<repo>/archive/refs/tags/0.0.48.tar.gz
sha256sum pkg-0.0.48.tar.gz          # ← the digest the index will carry
```

The tarball extracts to `<repo>-<tag>/`, and mcpp looks for `mcpp.toml` inside
that wrapper directory. A repo that ships its own `mcpp.toml` needs no `mcpp`
field in the index entry.

## 2. Mirror to gitcode

The CN entry must be a **byte-identical copy** of the GitHub tarball, only
renamed. Anything else and the two regions disagree about what a pinned
`sha256` means.

```bash
gtc release publish mcpp-res/<name> --tag 0.0.48 --asset pkg-0.0.48.tar.gz
```

Then verify it, because the upload reporting success is not the same as the
asset being fetchable:

```bash
# GET, never HEAD — gitcode answers HEAD with 401 and GET with 302 → CDN 200
curl -fsSL -o cn.tar.gz \
  https://gitcode.com/mcpp-res/<name>/releases/download/0.0.48/pkg-0.0.48.tar.gz
cmp cn.tar.gz pkg-0.0.48.tar.gz      # must be identical, not merely present
```

## 3. Add the index entry

In `mcpplibs/mcpp-index`, `pkgs/<first-letter>/<name>.lua`:

```lua
["0.0.48"] = {
    url = {
        GLOBAL = "https://github.com/<owner>/<repo>/archive/refs/tags/0.0.48.tar.gz",
        CN     = "https://gitcode.com/mcpp-res/<name>/releases/download/0.0.48/pkg-0.0.48.tar.gz",
    },
    sha256 = "<the digest from step 1>",
},
```

**In all three platform blocks** — `linux`, `macosx`, `windows`. A source
tarball is the same bytes on every platform, and an entry present in only one
of them fails on the others as "no such version", which reads like a typo in
the consumer's manifest.

## 4. Wait for the artifact

**The index is an artifact, not a git clone.** Merging to `main` is not
enough: `publish-artifact.yml` has to run and push a content-hash artifact,
and clients hold a refresh TTL on top of that. Editing a cached `pkgs/**` by
hand does nothing.

```bash
gh run list --repo mcpplibs/mcpp-index --workflow publish-artifact.yml --limit 1
rm -rf ~/.mcpp/registry/data/<namespace>    # force a client refresh
```

## 5. Verify from a cold resolve, then bump consumers

The point of this step is that a local checkout of the library will mask every
mistake above. Resolve it the way a stranger would:

```bash
rm -rf ~/.mcpp/registry/data/xpkgs/<ns>-x-<name>/0.0.48
mcpp build                            # must download and compile 0.0.48
```

Only then bump `[dependencies]` in the consumers.

> Do **not** try to force a refresh with
> `find ~/.mcpp/registry -mindepth 1 -maxdepth 1 ! -name data -exec rm -rf {} +`
> alone. `data/xpkgs` sits at depth 2 and is not named `data`, so a careless
> second pass deletes the whole payload store (~800 MB of toolchains).
> Recovery is `mcpp self doctor`, which re-provisions, then `mcpp update`.

## Testing against an unreleased version

While the chain above is still in flight, seed the registry by hand so
consumers can compile against the library before it is published:

```bash
REG=~/.mcpp/registry/data/xpkgs/<ns>-x-<name>/0.0.48
mkdir -p "$REG"
git -C /path/to/library archive --format=tar --prefix=<repo>-0.0.48/ HEAD \
  | tar -x -C "$REG"
touch "$REG/.mcpp_ok"                 # the marker that says "resolved"
cp ../0.0.47/.xpkg.lua "$REG/.xpkg.lua"   # add a 0.0.48 entry to it
```

mcpp's build sandbox is network-isolated, so `file://` and
`http://127.0.0.1` index URLs cannot be fetched — seeding the cache is the way.

**Remove the seeded copy before believing the real thing works.** A seeded
0.0.48 and a published 0.0.48 are indistinguishable to the build, and the
seeded one is the copy that will still be there when the publish silently
failed.

## Checklist

- [ ] `mcpp.toml` version == git tag
- [ ] tag pushed; tarball downloads and its sha256 recorded
- [ ] gitcode asset verified with **GET**, byte-identical to GitHub's
- [ ] index entry in **all three** platform blocks
- [ ] `publish-artifact.yml` succeeded
- [ ] cold resolve (seeded copy deleted) downloads and compiles it
- [ ] consumers bumped
