# Package Resolution Architecture — Identity-First Locator Design

**Date:** 2026-06-20
**Status:** Proposal
**Trigger:** `compat.zlib` dependency intermittently fails on fresh CI with
`index entry has no mcpp field` while passing locally — same binary, same
descriptor bytes. Root cause is not parsing; it is **descriptor resolution by
filename guessing across an unordered, unscoped index scan, with no identity
verification.**

---

## 1. The incident, distilled

`compat.zlib` declares identity `(namespace="compat", name="compat.zlib")`,
which normalizes to coordinate `(compat, zlib)`. The descriptor lives at
`data/mcpplibs/pkgs/c/compat.zlib.lua` and contains a valid `mcpp = { ... }`
block.

`Fetcher::read_xpkg_lua(ns, shortName)` resolves it like this
(`package_fetcher.cppm:416-445`):

1. `xpkg_lua_candidates("compat","zlib")` →
   `["compat.zlib.lua", "zlib.lua"]` — the bare `zlib.lua` is a
   **compat fallback** (`compat.cppm:189-191`).
2. `for (entry : directory_iterator(xlingsHome/data))` — iterate **all** index
   dirs in **unspecified filesystem order**.
3. For each dir, try each candidate; **return the first file that exists**,
   with **no check that the file is actually the requested package**.

A bare `zlib.lua` also exists as an *unrelated upstream package*
(`data/xim-pkgindex/pkgs/z/zlib.lua`: declares `name = "zlib"`, **no namespace
field**, **no mcpp block**). Its resolved identity is *not* `(compat, zlib)` —
an absent namespace is filled by the owning index's default (`xim`), giving
`(xim, zlib)` (see §4.1). When `directory_iterator` happens to visit
`xim-pkgindex` before
`mcpplibs` (CI's filesystem order), step 3 returns that file → `extract_mcpp_field`
finds nothing → `prepare.cppm:1294` reports *"index entry has no mcpp field."*

Only `zlib` is affected because it is the only one of the four compression
libs whose bare name collides with a package in a **directly-scanned** index
dir (`bzip2.lua` exists only in a nested `xim-index-repos/*` dir the scan does
not descend into; `lz4`/`xz` have no bare collision).

**The "same bytes, different result" is fully explained:** the file the
incident report sha256'd (the cache's `path` entry → the correct mcpplibs file)
is *not* the file `read_xpkg_lua` returns on CI. Resolution never consults that
path; it re-derives its own by scanning.

---

## 2. The essential problem (not the proximate bug)

This is a **design** problem, in three compounding layers:

### L1 — Identity is inferred from filename instead of read from the file
A package's identity is its declared `package.{namespace, name}`. The resolver
instead treats a **filename** as proof of identity. But filenames are not
namespace-scoped: `zlib.lua` is a valid filename for both `(compat, zlib)`'s
legacy form **and** the unrelated `(mcpplibs, zlib)`. A non-unique key is used
as if it were unique.

> **Filename is a location *hint*. Declared `(namespace, name)` is the identity
> *proof*. The bug is using the hint as the proof.**

### L2 — No namespace→index binding for builtin indexes
`findIndexForNs` (`prepare.cppm:853`) maps a namespace to an index **only for
custom `[indices]`**. Builtin/default/`compat` namespaces return `nullptr` and
fall into a **scan-all-directories** path. Worse, one builtin directory
(`mcpplibs`) hosts *multiple* namespaces (`mcpplibs.*` **and** `compat.*`), so a
naive "namespace → one directory" map cannot exist. Ownership is
**many-namespaces-to-one-index**, and today it is expressed by *nothing* — just
a global scan.

### L3 — Resolution order is nondeterministic
`std::filesystem::directory_iterator` order is unspecified and
filesystem-dependent. Cross-index resolution therefore depends on disk layout,
which differs between a warm local checkout and a fresh CI runner. **The same
inputs do not produce the same output across machines.**

L1 is the proximate cause; L2 and L3 are what make it *latent* and
*environment-sensitive*. The fuzzy candidate generator (`xpkg_lua_candidates`,
`install_dir_candidates`) is itself a *symptom*: it is compensating for the
absence of one enforced canonical layout — hence its pile of
`// COMPAT, remove in 1.0.0` fallbacks.

---

## 3. The same defect, twelve times

Resolution-by-blind-first-hit is not isolated to `read_xpkg_lua`. It is the
house style for both **descriptor** and **payload** location:

| # | Location | What it does | Verifies identity? |
|---|----------|--------------|--------------------|
| 1 | `package_fetcher.cppm:416` `read_xpkg_lua(ns,short)` | scan `data/*/pkgs`, first `.lua` hit | No |
| 2 | `package_fetcher.cppm:453` `read_xpkg_lua_from_path` | probe `indexPath/pkgs`, first hit | No |
| 3 | `package_fetcher.cppm:483` `read_xpkg_lua_from_project_data` | scan project data roots, first hit | No |
| 4 | `package_fetcher.cppm:790` `install_path` | probe candidate verdirs, then legacy scan | No |
| 5 | `package_fetcher.cppm:518` `install_path_from_project_data` | probe project xpkgs candidates | No |
| 6 | `package_fetcher.cppm:601` `resolve_xpkg_path` | single guessed `<idx>-x-<name>/<ver>` | No |
| 7 | `fallback/legacy_dirs.cppm:15` `scan_legacy_install_dirs` | suffix-match dir scan, first hit | No (loosest) |
| 8 | `scaffold/create.cppm:44` | loop ns guesses `{"", "compat"}`, first hit | re-derives coords after |
| 9 | `prepare.cppm:1043` `readLuaContent` | routes to 1–3 | No |
| 10 | `prepare.cppm:1064` `findRawInstalled` | routes to 4–5 | No |
| 11 | `prepare.cppm:988` `selectDependencyCandidate` | uses `xpkgLuaMatchesCandidate` | **Yes — but only when >1 candidate** |
| 12 | `prepare.cppm:2010` manifest name-match | post-load name check | **Yes — but post-hoc, after payload loaded** |

Two facts jump out:

- **The cure already exists, half-applied.** `xpkgLuaMatchesCandidate`
  (`prepare.cppm:944`) verifies a descriptor's declared `(name, namespace)`
  against the requested coordinate. `canonicalXpkgLuaFilename` +
  `readStrictLuaFromPkgsDir` + `readStrictLuaForCandidate`
  (`prepare.cppm:868-936`) already do a **canonical-filename-only** read. But
  these are wired in **only** for multi-candidate disambiguation
  (`selectDependencyCandidate`, gated on `candidates.size() > 1`). The common
  single-candidate **load** path (`loadVersionDep`→`readLuaContent`) bypasses
  them entirely and calls the fuzzy `read_xpkg_lua`.
- **xlings owns bytes; mcpp owns resolution.** mcpp never asks xlings to map
  identity→path — xlings only `search`/`plan`/`install`/`manage-repos`
  (NDJSON). The `.xlings-index-cache.json` is xlings-written and mcpp only
  substring-probes it for freshness (`xlings.cppm:349`); it never reads the
  `path` field structurally. So the identity→path convention is *entirely
  mcpp's*, and fixing it is entirely within mcpp.

The refactor is therefore **convergence, not invention**: finish migrating the
load and payload paths onto the strict, identity-verified resolution that
already exists, and delete the legacy fuzzy path.

---

## 4. Design principles (the invariants to enforce)

1. **Identity is the declared `(namespace, name)` — and the *only* key.**
   The filename is *not* the identity and *not* a key. A file named
   `xyz123.lua` that declares `(compat, zlib)` **is** `compat.zlib`; a file
   named `zlib.lua` that declares `(mcpplibs, zlib)` is **not**. Resolution
   keys on what the file *says it is*, never on what it is *called*.
2. **The canonical filename is a lookup *accelerator*, never authoritative.**
   `<ns>.<shortName>.lua` may be used as a fast-path hint to avoid a scan, but
   **the system must resolve correctly even when filenames are arbitrary** — the
   filename is droppable. (Stronger than "filename is the unique key": that
   phrasing smuggles filename back in as authority and re-breaks on legacy
   non-canonically-named files.)
3. **Every hit is identity-verified before return**, including the fast-path
   hit. The fast path only *skips a scan*; it never *skips verification*.
4. **Index precedence is explicit and deterministic.** When an identity could
   resolve in multiple indexes, an ordered precedence decides — never
   `directory_iterator` order.
5. **No filename guessing — ever.** When the canonical-filename fast path
   misses, the fallback is an *identity-indexed scan* (read each descriptor's
   declared `(ns,name)`, look up the exact coordinate), **not** a list of
   guessed alternative filenames. Non-canonical filenames still resolve, and
   emit a one-time deprecation warning so the data migrates; the fuzzy candidate
   *generators* are deleted, not demoted.
6. **One choke point.** All descriptor/payload location funnels through a single
   resolver returning a verified record. No caller runs `directory_iterator`
   itself.

### 4.1 Normalizing identity (totalize the namespace)

Identity-as-key only works if the `(namespace, name)` of *every* descriptor is
unambiguous. Today it is not — two cases need a single, explicit rule:

- **Prefix-embedded equivalence.** `package.namespace = "compat"` with
  `package.name = "compat.zlib"` and the same namespace with `name = "zlib"`
  denote the **same** package. The redundant `<namespace>.` prefix on the name
  is stripped (`descriptor_coordinates`, `compat.cppm:98-105`), so both
  normalize to coordinate `(compat, zlib)`. The identity map keys on the
  *normalized* coordinate, so the two forms collapse to one key. This is the
  legacy-name compatibility, and it lives in normalization — never in filename
  guessing.

- **Namespace is an index-owned property, not a descriptor-or-filename one.**
  Each index repo *has* a default namespace — this already exists in the system,
  it is not a new rule:
  - `xim-pkgindex` → `xim` (`parse_target` defaults `indexName="xim"`,
    `package_fetcher.cppm:606`, comment *"Default to xim namespace for
    tools/toolchain"*; install layout on disk is `xpkgs/xim-x-zlib`).
  - `mcpplibs` → `mcpplibs` (`kDefaultNamespace`; layout `mcpplibs-x-...`).
  - a custom `[indices]` entry → its key name (the index name *is* its
    namespace, `index_spec.cppm`).

  So a descriptor's namespace = its declared `package.namespace` **if present**
  (this lets one index host several namespaces — e.g. `compat.*` packages
  physically in the `mcpplibs` index declare `namespace="compat"` →
  `compat-x-compat.zlib` on disk), **else the owning index's default
  namespace**. `xim-pkgindex/pkgs/z/zlib.lua` declares only `name="zlib"`, so its
  namespace comes from its index → `(xim, zlib)`. The bare `name`, and the
  filename, never determine the namespace.

  **Invariant: a resolved namespace is never empty.** There is no `("", zlib)`
  identity. An absent `package.namespace` does not mean "root/empty" — it means
  "use the owning index's default namespace." On disk this already holds: a
  root-style package like `imgui` (declares no namespace) installs as
  `mcpplibs-x-imgui`, i.e. it took the `mcpplibs` index default — *not* an empty
  namespace. So both current normalizers are wrong for no-namespace descriptors:
  `resolve_package_name` hardcodes `kDefaultNamespace="mcpplibs"` regardless of
  index → `(mcpplibs, zlib)`, and `descriptor_coordinates` leaves an empty
  `("", zlib)`. The index-owned answer is `(xim, zlib)`, and "empty" is never a
  terminal state.

  The install/runtime side *already* attributes namespace this way
  (`xim-x-zlib`, `mcpplibs-x-imgui`); the bug is that the **descriptor-reading**
  path (`read_xpkg_lua`) dropped the index attribution and fell back to filename
  guessing.

  This matters beyond tidiness: if no-namespace descriptors defaulted to
  `mcpplibs`, a **bare** default-namespace request `zlib` would resolve to the
  blockless xim `zlib.lua` and reproduce the same "no mcpp field" failure for a
  *different* request. Index-owned namespace (`(xim, zlib)`) closes that second
  latent collision: a default-namespace request never silently swallows a
  foreign index's package.

> The incident is collision-free under *any* of these readings, because the
> request is `(compat, zlib)` and the xim file is never `(compat, *)`. But only
> the index-imparts-namespace rule makes the **whole** identity space
> collision-free, including the bare/default-namespace path.

---

### 4.2 Canonical package identity (the unified model)

Every package has exactly one canonical identity: a **2-tuple `(ns, name)`**
derived by normalizing the descriptor's declared `package.namespace` +
`package.name`. There are no other dimensions — filename, install-dir name, and
candidate spellings are all derived from (or hints toward) this tuple, never part
of it.

- **`ns` is a namespace *path*** — it supports sub-namespaces and is therefore
  hierarchical/dotted: `compat`, `mcpplibs`, `a.b.c`.
- **`name` is a single, atomic segment** — it has no internal structure. The
  descriptor *may write* `name = "a.b"` as a convenience/legacy spelling, but its
  essence is `(ns=a, name=b)`: the leading dotted segments belong to the
  namespace, only the final segment is the name.

**Normalization — declared `(declaredNs, declaredName)` → canonical `(ns, name)`:**

1. **Owning-index namespace.** If `declaredNs` is empty, it is the namespace of
   the index the descriptor lives in (index-owned namespace, §4.1). A descriptor
   is never left namespace-less.
2. **Fully-qualified name (FQN).**
   - If `declaredName` is dotted, it is already the FQN (e.g. `compat.zlib`,
     `a.b`). A dotted name is expected to be consistent with — i.e. prefixed by —
     the namespace.
   - If `declaredName` is a single segment, `FQN = ns + "." + declaredName`
     (e.g. `name=cmdline`, `ns=mcpplibs` → `mcpplibs.cmdline`; `name=zlib` in the
     `xim` index → `xim.zlib`).
3. **Split on the last dot.** Everything before the final `.` is the (hierarchical)
   `ns`; the final segment is the single `name`.

Worked examples:

| declared `namespace` | declared `name` | owning index | canonical `(ns, name)` |
|---|---|---|---|
| `compat` | `compat.zlib` | mcpplibs | `(compat, zlib)` |
| `mcpplibs` | `cmdline` | mcpplibs | `(mcpplibs, cmdline)` |
| `mcpplibs` | `mcpplibs.cmdline` | mcpplibs | `(mcpplibs, cmdline)` |
| *(none)* | `zlib` | xim-pkgindex | `(xim, zlib)` |
| *(none)* | `tinycfg` | `local-dev` index | `(local-dev, tinycfg)` |
| `a.b` | `c` | — | `(a.b, c)` |
| *(none)* | `a.b` | — | `(a, b)` |

The qualified serialization of `(ns, name)` is simply `ns + "." + name`
(`compat.zlib`, `a.b.c`). That string is what a `[dependencies]` entry, an install
dir (`<ns>-x-<ns>.<name>`), and a canonical filename (`<ns>.<name>.lua`) all
encode — different *serializations of the same tuple*, never independent keys.

**Matching** then reduces to:
- **Qualified request** → canonical `(ns, name)` **exact equality**. (No special
  cases; cross-namespace collisions are structurally impossible because the
  namespaces differ.)
- **Unqualified/bare request** → resolve the single `name` against an ordered
  **namespace search path** (currently `[mcpplibs, compat]`). `compat` is a *data
  entry* in that path — the wrapper namespace for upstream libraries — not a
  branch in the matching logic.

The codebase has the pieces of this normalization spread across
`descriptor_coordinates` (strip redundant `ns.` prefix from name),
`normalize_nested_namespace` (fold dotted `name` segments into `ns`), and the new
index-owned-namespace attribution. The target is a single `canonical_identity()`
that applies rules 1–3 uniformly, after which all matching is tuple equality plus
the search path — and every remaining `compat`/filename special-case dissolves.

---

## 5. Target architecture: an identity-first `PackageLocator`

Introduce one component that owns **all** filesystem resolution. Every one of
the 12 spots delegates to it.

```
struct PackageIdentity { std::string namespace_; std::string shortName; };

struct ResolvedDescriptor {
    std::filesystem::path  path;       // the .lua actually chosen
    PackageIdentity        identity;   // its DECLARED, normalized identity
    std::string            content;    // raw .lua text
    std::string            indexName;  // which index won (for payload reuse)
};

class PackageLocator {
    // Ordered index roots to search, most-specific first:
    //   1. custom [indices] match via findIndexForNs  (already scoped)
    //   2. builtin precedence list: mcpplibs, xim-pkgindex, <xlings extras...>
    //      — an explicit, sorted vector, NOT directory_iterator order.
    std::optional<ResolvedDescriptor>
    locate(const PackageIdentity& want, const IndexRoutingContext& ctx) const;

    std::optional<std::filesystem::path>
    locatePayload(const ResolvedDescriptor& d, std::string_view version) const;
};
```

`locate()` algorithm — **identity is the key; filename is only a fast-path
accelerator:**

```
for root in orderedIndexRoots(want, ctx):              # deterministic order (principle 4)
    # Fast path: canonical filename as a HINT (no scan), still identity-verified.
    f = root/pkgs/<first>/<canonicalFilename(want)>
    if exists(f) and declaredIdentity(read(f)) == want:    # principle 1,3
        return ResolvedDescriptor{f, want, ..., root.name}

    # Slow path: do NOT guess other filenames. Consult this index's
    # (ns,name)->path identity map (built by reading each descriptor's declared
    # package{} header once, then cached against the index's refresh marker).
    if hit = identityIndexOf(root).get(want):          # principle 1; no filename guessing
        warnIfNonCanonicalFilename(hit.path, want)     # one-time deprecation
        return ResolvedDescriptor{hit.path, want, ..., root.name}
return nullopt
```

The slow path is what replaces the fuzzy candidate list: instead of *guessing*
alternative filenames (`zlib.lua`, `compat.zlib.lua`, …) and hoping, it *reads
declared identities* and looks up the exact `(ns, name)`. A legacy file with an
arbitrary name still resolves — because it is keyed by what it declares, not by
what it is called. Filenames could be stripped from the algorithm entirely (drop
the fast path) and resolution would remain 100% correct, only slower — that is
the litmus test for "filename is not a key."

`identityIndexOf(root)` is built by scanning `root/pkgs/**` once, reading only
each descriptor's `package{}` header (`extract_xpkg_identity`), and is cached
keyed by the index's existing refresh marker
(`.mcpp-index-updated` / cache mtime) so it is rebuilt only when the index
changes. Same-identity collisions across two files in one index surface as a map
insert conflict → resolved by precedence or reported, never decided by scan
order.

Supporting helpers (small, mostly already present):

- `canonicalFilename(want)` — exists as `canonicalXpkgLuaFilename`
  (`prepare.cppm:868`); promote to `mcpp.pm.compat`.
- `declaredIdentity(content)` — **new 3-line helper**
  `extract_xpkg_identity(lua) -> PackageIdentity`: one `package`-body scan, then
  `name`+`namespace`, normalized through `descriptor_coordinates`. Today
  `extract_xpkg_name` and `extract_xpkg_namespace` each re-scan the body
  separately (`manifest.cppm:1558,1564`); unify them.
- `identityMatches` — generalize `xpkgLuaMatchesCandidate` (`prepare.cppm:944`),
  which already encodes the legacy-equivalence rules (prefix-embedded vs bare
  name, default-namespace tolerance via `allowLegacyBareDefault`).
- `orderedIndexRoots` — replaces the bare `directory_iterator(data)` with an
  explicit precedence (custom index → mcpplibs → xim-pkgindex → xlings extras).

**Payload resolution** mirrors this: `locatePayload` reuses the *already
resolved* `indexName` from the descriptor instead of re-guessing dirnames, tries
the canonical `<ns>-x-<ns>.<short>/<version>` dir first, and keeps the existing
completion-marker / `installedLayoutMatchesIndex` check
(`prepare.cppm:1074-1115`) as the payload's identity gate. Legacy dir candidates
become an identity-gated, deprecating fallback (retiring
`scan_legacy_install_dirs`'s loose suffix match).

The key structural win: **descriptor and payload resolution share the resolved
index**, so they can no longer disagree about *which* package they found.

---

## 6. Why not the alternatives

- **Use the cache's `path` field as authoritative.** The cache is
  *xlings-written*, can be stale/missing (the substring freshness probe exists
  precisely because it lags), and is a foreign format mcpp only treats as opaque
  text today. Coupling build-critical resolution to it trades a determinism bug
  for a staleness bug. Reject as primary; revisit only if mcpp owns its own
  index.
- **Just sort `directory_iterator`.** Removes nondeterminism but can be
  *deterministically wrong* (a foreign index sorted first still wins via the
  bare-name fallback). Does not address L1. Necessary but not sufficient —
  subsumed by principle 4 + identity gate.
- **Just delete the bare-name fallback.** Breaks legacy indexes whose files are
  genuinely bare-named, with no migration. The identity gate (principle 3) lets
  the fallback stay *safe* until data migrates, then it can be deleted cleanly
  (principle 5).

---

## 7. Migration plan (incremental, CI-fix-first)

- **Step 0 — Hotfix (unblock CI today).** In `loadVersionDep`'s `readLuaContent`
  (`prepare.cppm:1043`), route the builtin branch through the existing strict
  canonical read + `xpkgLuaMatchesCandidate` instead of fuzzy `read_xpkg_lua`.
  This is the minimal subset of the target design. Canonical `compat.zlib.lua`
  is unique ⇒ no collision; even with fuzzy retained, the identity gate rejects
  the foreign `zlib.lua`. Low risk, surgical.
- **Step 1 — Extract the choke point.** Introduce `PackageLocator`; make
  `read_xpkg_lua*` and `install_path*` thin delegates. Behavior-preserving;
  centralizes the 12 spots.
- **Step 2 — Universal identity gate.** Every hit verified; fuzzy candidates
  become gated fallbacks with one-time deprecation warnings naming the
  non-canonical file.
- **Step 3 — Deterministic precedence.** Replace `directory_iterator` order with
  `orderedIndexRoots`; remove all disk-order dependence.
- **Step 4 — 1.0.0 cleanup.** Migrate index data to canonical filenames/dirs;
  delete `xpkg_lua_candidates`/`install_dir_candidates` fallback arms and
  `scan_legacy_install_dirs`. Settles the standing `// remove in 1.0.0` debt.

---

## 8. Testing strategy

- **Regression (the incident):** stage two index dirs —
  `mcpplibs/pkgs/c/compat.zlib.lua` (with mcpp block) and
  `xim-pkgindex/pkgs/z/zlib.lua` (no block) — and assert
  `locate((compat, zlib))` returns the mcpplibs descriptor **regardless of index
  order** (parameterize the precedence / iteration to prove order-independence).
- **Invariant property:** for every `(ns, shortName)`, the resolver never
  returns a descriptor whose declared identity ≠ requested coordinate. Fuzz over
  candidate-colliding fixtures.
- **Unit:** `extract_xpkg_identity` (prefix-embedded, bare, default-ns,
  legacy-dotted forms); `identityMatches` truth table; `orderedIndexRoots`
  precedence.
- **Legacy compat:** a genuinely bare-named legacy descriptor still resolves
  *and* emits exactly one deprecation warning.

---

## 9. One-paragraph summary

mcpp resolves package descriptors by guessing filenames and taking the first
filesystem hit across an unordered, unscoped scan of all index directories,
without ever confirming the file it found is the package it asked for. That is a
resolution-by-guessing design where a non-unique filename is used as an identity
key; it is latent because the canonical filename usually wins, and it surfaces
whenever a bare-name fallback collides with an unrelated package in another
directory and disk order puts that directory first — exactly the `compat.zlib`
vs upstream `zlib` case on CI. The fix is to make resolution **identity-first**:
look up by the unique namespace-qualified canonical filename, verify every hit
against the file's declared identity, search indexes in an explicit deterministic
order, and demote the fuzzy candidates to an identity-gated, deprecating compat
shim — converging onto the strict resolver that already exists half-built in
`prepare.cppm` and routing all twelve blind-hit sites through a single
`PackageLocator` choke point.

---

## 10. Implementation plan & progress (living)

Branch: `fix/pkg-resolution-identity-first`. This section is updated as the work
lands. The full §5 `PackageLocator` is the north star; this PR delivers the
**identity gate at the descriptor-read sites** — the load-bearing subset that
closes the incident — and makes resolution order-deterministic. The broader
choke-point consolidation (payload locators, `resolve_xpkg_path`,
`scan_legacy_install_dirs`) is sequenced as follow-ups.

### Scope of THIS PR (root-cause fix + determinism)

| Task | File | Status |
|------|------|--------|
| `xpkg_lua_identity_matches()` — shared identity gate (declared `(ns,name)` vs request) | `src/manifest.cppm` | ☑ done |
| Route all 3 `read_xpkg_lua*` readers through the gate (skip non-matching hits) | `src/pm/package_fetcher.cppm` | ☑ done |
| Deterministic, sorted index-dir iteration (`sorted_index_dirs`) | `src/pm/package_fetcher.cppm` | ☑ done |
| De-duplicate: `prepare.cppm`'s `xpkgLuaMatchesCandidate` delegates to the shared gate | `src/build/prepare.cppm` | ☑ done |
| Unit tests: identity-gate truth table | `tests/unit/test_manifest.cpp` | ☑ done |
| Regression test: cross-index collision (`compat.zlib` vs bare `zlib`) | `tests/unit/test_pm_package_fetcher.cpp` | ☑ done |
| Local build + `mcpp test` green (21/21 binaries, 8 new tests) | — | ☑ done |
| Local e2e green: dep-resolution (27/31/62/63), scaffold (02), custom/local index (42/49/51/52), path dep (09), preinstall (58), dev-deps (18), compat flows (50/55/60/61) | — | ☑ done |
| De-hardcode `compat` → shared `kCompatNamespace` constant (gate + candidate generator) | `dep_spec.cppm`, `manifest.cppm`, `compat.cppm` | ☑ done |
| Index-owned-namespace attribution for **scoped** (`[indices]` path) indexes — fixes e2e 49/51 the unified-model way (no-ns descriptor inherits its index's ns) | `manifest.cppm`, `package_fetcher.cppm` | ☑ done |
| **Unified `canonical_xpkg_identity()` normalizer** (§4.2): owning-index ns → FQN → split-on-last-dot → `(ns, name)`. Matcher rewritten on top: name equality + (qualified) exact ns / (unqualified) search path. Behavior-preserving over the prior branchy gate | `manifest.cppm` | ☑ done |
| Comprehensive `CanonicalIdentity` unit suite (every §4.2 paradigm: prefix-embedded, bare+combine, index-attribution, hierarchical/nested ns, dotted-name split, from-lua) | `tests/unit/test_manifest.cpp` | ☑ done |
| CI green (linux/macos/windows) | — | ◐ macOS+Windows green; Linux re-running |

**Unified-model note (per review):** `compat` is not a special namespace in the
*matching logic*. The model is: a descriptor's effective namespace = its declared
`package.namespace`, else **its owning index's namespace** (index-owned
namespace); qualified requests then match by exact `(ns, shortName)` equality.
`compat` survives only as a *data* entry in the default/unqualified-name search
path (`kCompatNamespace`), not as a logic branch. This PR lands index attribution
for **scoped** reads (`read_xpkg_lua_from_path`, where the owning index's
namespace is known = the request ns). The **builtin global scan**
(`read_xpkg_lua`) still uses the content-only proxy + `compat` search-path entry
because it lacks a per-file `index-dir → namespace` map (`xim-pkgindex → xim`,
etc.) — that map is the remaining §4.1 work, after which the builtin path also
becomes pure exact-match and the `compat` matching branch disappears.

**Fix mechanism (this PR):** a candidate filename is only a hint; every hit is
verified against the descriptor's declared `(ns,name)` before acceptance, and
index dirs are scanned in sorted order. `compat.zlib.lua` (declares
`(compat, compat.zlib)`) matches; a foreign bare `zlib.lua` (declares
`(_, zlib)`) is rejected for a `compat.zlib` request — independent of disk order.
This realizes principles 1 (identity is the key), 3 (verify every hit), and 4
(deterministic order) at the read sites.

### Deliberately deferred (follow-up PRs)

These are real but out of this PR's blast radius; tracked here so they are not
lost:

- **Payload locators** (`install_path`, `install_path_from_project_data`,
  `resolve_xpkg_path`, `scan_legacy_install_dirs`) — same blind-first-hit
  pattern (inventory §3, spots #4–7). Add the payload identity/layout gate and
  reuse the descriptor's resolved index.
- **Index-owned namespace totalization (§4.1)** — make a no-namespace descriptor
  resolve to `(<owning-index>, name)` (e.g. `(xim, zlib)`) instead of the
  inconsistent `mcpplibs` / `""`. Needed to close the *bare/default-namespace*
  collision variant; not required for the `compat.zlib` incident.
- **Identity-indexed slow path + cache (§5)** — replace the fuzzy candidate
  *generators* with an `(ns,name)→path` map built from declared identities.
- **`PackageLocator` choke point** — funnel all 12 sites through one resolver
  returning a verified record; delete `xpkg_lua_candidates` /
  `install_dir_candidates` fallback generators at 1.0.0.
