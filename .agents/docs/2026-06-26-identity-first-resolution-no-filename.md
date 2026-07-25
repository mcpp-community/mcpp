# Identity-First Package Resolution — Filename Is Not a Key

**Date:** 2026-06-26
**Status:** Step 0 landed in v0.0.67 (candidate selection is now identity-first);
§5 `PackageLocator` / `IdentityIndex` choke-point consolidation remains follow-up.
**Partly superseded by #278 (v0.0.105):** §4.5's emit half landed (`mcpp emit xpkg`
now writes both `namespace` and the FQN `name`); its "catalog keys on canonical
(ns,name)" half is **unachievable as written** — the catalog belongs to xlings, is
keyed by the literal `package.name` with exact `find()`, and mcpp's normalization
has no authority over it, so the split/FQN equivalence is instead resolved by
constraining the wire key's spelling (INV-NAME). §4.6(c)'s discovery rung is
narrowed — see the SUPERSEDED note at that row.
**Extends:** [`2026-06-20-package-resolution-architecture.md`](2026-06-20-package-resolution-architecture.md)
(realizes its deferred §5 `PackageLocator` / identity-indexed slow path),
[`2026-05-11-namespace-field-design.md`](2026-05-11-namespace-field-design.md),
[`2026-06-02-dotted-dependency-selectors.md`](2026-06-02-dotted-dependency-selectors.md)
**Trigger:** A user's package `aimol.tensorvia-cpu` (`package.name="tensorvia-cpu"`,
`package.namespace="aimol"`, hosted in the `mcpplibs` index) fails to resolve with
`error: dependency 'aimol.tensorvia-cpu': index entry not found in local clone`,
while the **bare** form `tensorvia-cpu` resolves, installs as `aimol-x-tensorvia-cpu`,
and builds. The package is correct; mcpp is wrong.

---

## 一、摘要(Chinese summary)

**核心论点:包的身份只有 `(pkg.ns, pkg.name)` 两个字段,文件名/目录名不参与身份判定 —— 一个字符都不参与。**

调查证明:`aimol.tensorvia-cpu` 解析失败**不是包的问题**,而是 mcpp 的「候选消歧」阶段
用**规范文件名 `<ns>.<name>.lua` 是否存在**来判定候选,而该描述符以裸文件名
`tensorvia-cpu.lua` 落盘 —— 于是正确的 peer-root 候选 `(aimol, tensorvia-cpu)` 对消歧器
**隐形**,请求被钉死在错误的首选候选 `(mcpplibs.aimol, tensorvia-cpu)` 上,再被身份门合理拒绝。
裸名能过,仅因为它的回退候选命名空间为空(按短名通配),与文件名是否规范无关。

这正是 [`2026-06-20`](2026-06-20-package-resolution-architecture.md) 文档点名的 L1 反模式
(「用文件名当身份证明」),也是它 §10 明确**推迟**的 §5 工作。本文给出**根治架构**(非补丁):
建立**唯一的身份索引 `IdentityIndex: (ns,name) → record`**,由读取每个描述符声明的
`package.{namespace,name}` 构建;文件名仅作为可选的、永远要被身份校验的加速提示,甚至可整体丢弃而
解析结果不变。所有 12 处解析点(描述符读取、候选消歧、载荷定位、SemVer、emit、索引缓存键)
统一收口到一个 `PackageLocator`,全部以 `(ns,name)` 为唯一键。

---

## 二、Research — the investigation, distilled

### 2.1 The incident & isolated reproduction

`helloworld/mcpp.toml`:

```toml
[dependencies]
mcpplibs.cmdline    = "0.0.1"
aimol.tensorvia-cpu = "0.1.1"
```

Isolated each dependency against the installed `mcpp 0.0.66`:

| `[dependencies]` entry | result | failing stage |
|---|---|---|
| `tensorvia-cpu = "0.1.1"` (bare) | ✅ resolves, installs `aimol-x-tensorvia-cpu/0.1.1`, builds | — |
| `mcpplibs.cmdline` / `mcpplibs.tinyhttps` | ✅ | — |
| `aimol.tensorvia-cpu = "0.1.1"` | ❌ `index entry not found in local clone` | **candidate selection** |
| `aimol.tensorvia-cpu = "*"` | ❌ error leaks name **`mcpplibs.aimol.tensorvia-cpu`** | candidate selection (kept wrong front) |
| `mcpplibs.tensorvia-cpu = "0.1.1"` | ❌ `index entry not found` | load identity-gate (`mcpplibs ≠ aimol`; correct rejection) |

Two facts immediately exonerate the package and the namespace:

- It is **not** the `aimol` prefix — `mcpplibs.tensorvia-cpu` fails too, and
  `mcpplibs.cmdline` (same `mcpplibs` index) works.
- The identity `(aimol, tensorvia-cpu)` is **honored end-to-end on the install
  side**: the bare form installs to `xpkgs/aimol-x-tensorvia-cpu/0.1.1/`. The
  payload layer reads the descriptor's `namespace="aimol"` correctly. Only the
  *qualified-request descriptor read* is broken.

### 2.2 The descriptors

```lua
-- mcpplibs/pkgs/c/cmdline.lua          (works)
package = { namespace = "mcpplibs", name = "mcpplibs.cmdline", ... }   -- FQN in name

-- mcpplibs/pkgs/t/tensorvia-cpu.lua    (fails on qualified request)
package = { namespace = "aimol",    name = "tensorvia-cpu",     ... }   -- split form
```

Both forms are **explicitly legal** per the canonical model
([`2026-06-20`](2026-06-20-package-resolution-architecture.md) §4.2):
`canonical_xpkg_identity` normalizes `(ns="aimol", name="tensorvia-cpu")` →
`(aimol, tensorvia-cpu)`, and `(ns="mcpplibs", name="mcpplibs.cmdline")` →
`(mcpplibs, cmdline)`. The model says these two spellings must be **equivalent**.
They are not, in practice — because of where identity is read from.

The index cache (`.xlings-index-cache.json`) mirrors the asymmetry, keying by the
declared `name` verbatim:

| descriptor | `name` | `namespace` | cache key |
|---|---|---|---|
| cmdline | `mcpplibs.cmdline` | `mcpplibs` | `mcpplibs.cmdline` |
| tensorvia-cpu | `tensorvia-cpu` | `aimol` | **`tensorvia-cpu`** ← `aimol` dropped |

### 2.3 The dotted-selector candidate ladder

`resolve_dependency_selector` (`src/pm/dependency_selector.cppm:59`) emits ordered
candidates (`kDefaultNamespace = "mcpplibs"`, `dep_spec.cppm:55`):

| selector | candidates (front → back) |
|---|---|
| `tensorvia-cpu` (1 seg) | `(mcpplibs, tensorvia-cpu)`, **`(∅, tensorvia-cpu)`** |
| `aimol.tensorvia-cpu` (2 seg) | `(mcpplibs.aimol, tensorvia-cpu)`, `(aimol, tensorvia-cpu)` |
| `mcpplibs.cmdline` (front == default) | `(mcpplibs, cmdline)` — single |

This is the omitted-`mcpplibs`-priority rule from
[`2026-06-02`](2026-06-02-dotted-dependency-selectors.md). It is **correct**: the
peer-root candidate `(aimol, tensorvia-cpu)` is in the list. The bug is in how the
list is **disambiguated**.

### 2.4 Root cause — candidate selection proves identity by filename existence

`selectDependencyCandidate` (`src/build/prepare.cppm:997`, active when
`candidates.size() > 1`) picks the first candidate whose descriptor "exists", via
`readStrictLuaForCandidate` → `readStrictLuaFromPkgsDir` (`prepare.cppm:910`):

```cpp
auto fname = canonicalXpkgLuaFilename(ns, shortName);     // "<ns>.<short>.lua" | "<short>.lua"
auto candidate = pkgsDir / first_letter(fname) / fname;   // a single exists() probe
if (!exists(candidate)) return std::nullopt;              // ← filename IS the identity test
```

`canonicalXpkgLuaFilename` (`prepare.cppm:902`): default-ns → `<short>.lua`,
otherwise `<ns>.<short>.lua`. The descriptor's *declared* identity is never read
during selection — **only the filename is consulted.** Tracing both cases against
the real on-disk file `t/tensorvia-cpu.lua`:

**`aimol.tensorvia-cpu` (fails):**
- cand① `(mcpplibs.aimol, tensorvia-cpu)` → probe `m/mcpplibs.aimol.tensorvia-cpu.lua` → absent.
- cand② `(aimol, tensorvia-cpu)` → probe `a/aimol.tensorvia-cpu.lua` → **absent** (file is `t/tensorvia-cpu.lua`).
- No candidate "exists" → `selected` stays at `front()` = cand① `(mcpplibs.aimol, …)`.
- `loadVersionDep` reads cand① via `read_xpkg_lua` (which *does* have a bare-name
  fallback), finds `t/tensorvia-cpu.lua`, but the identity gate computes its
  identity as `(aimol, tensorvia-cpu)` ≠ requested `(mcpplibs.aimol, tensorvia-cpu)`
  → rejected → `luaContent` empty → `prepare.cppm:1263` throws
  `index entry not found in local clone`. (With `*`, the SemVer path
  `resolver.cppm:96` reports the kept front qname → the leaked
  `mcpplibs.aimol.tensorvia-cpu`.)

**`tensorvia-cpu` (works) — and why:**
- cand① `(mcpplibs, tensorvia-cpu)` → canonical filename = bare `tensorvia-cpu.lua`
  → `t/tensorvia-cpu.lua` exists → read → identity `(aimol, …)` ≠ `(mcpplibs, …)` → not matched.
- cand② **`(∅, tensorvia-cpu)`** → canonical filename = bare `tensorvia-cpu.lua` →
  exists → read → request ns is **empty** → identity gate takes the *discovery
  branch* (`manifest.cppm:1679`: empty request ns ⇒ match by short name alone) →
  **matched.** `selected = (∅, tensorvia-cpu)`; load + install succeed.

The bare form survives by accident: its fallback candidate has an **empty
namespace**, which the canonical-filename happens to spell as the bare filename
*and* which the matcher treats as a name-only wildcard. The qualified form has no
such escape hatch — its peer-root candidate carries a real namespace, so its
canonical filename `<ns>.<short>.lua` must exist on disk, and here it does not.

> **The defect, in one line:** candidate selection uses the **filename** as proof of
> identity. A descriptor whose bytes live under a non-canonical filename is invisible
> to any candidate that carries a real namespace — even when its declared
> `(pkg.ns, pkg.name)` is an exact match.

This is exactly the **L1 anti-pattern** named in
[`2026-06-20`](2026-06-20-package-resolution-architecture.md) §2 ("identity is
inferred from filename instead of read from the file"). That PR (#136) closed L1 at
the **load** reader (`read_xpkg_lua`) but left the **selection** reader
(`readStrictLuaForCandidate`) — and emit, and the cache key — still filename-shaped.
§10 of that doc explicitly defers the cure: *"Identity-indexed slow path + cache
(§5) — replace the fuzzy candidate generators with an `(ns,name)→path` map built
from declared identities."* This document specifies that cure.

---

## 三、Design principles — identity is two fields, nothing else

> **P0. A package's identity is the 2-tuple `(pkg.ns, pkg.name)` — and nothing
> else.** It is read from the descriptor's declared `package.namespace` +
> `package.name`, normalized by `canonical_xpkg_identity` (§4.2). The filename, the
> first-letter bucket, the directory, and the install-dir name are **byte
> locations**, never identity and never keys.

The remaining principles follow from P0 and restate
[`2026-06-20`](2026-06-20-package-resolution-architecture.md) §4 with the filename
demoted all the way out of the key space:

- **P1. One key everywhere.** Selector candidates, descriptor reads, payload
  locates, SemVer version lists, emit output, and the index cache all key on the
  *normalized* `(ns, name)`. The qualified serialization `ns + "." + name` and the
  install dir `<ns>-x-<ns>.<name>` are *renderings* of that tuple, never independent
  keys.
- **P2. Filename is droppable.** Resolution must be 100% correct if every
  descriptor file were renamed to a random string. The canonical filename may be
  used as a *fast-path hint* to avoid a directory scan, but **every hit — including
  the fast-path hit — is identity-verified against the file's declared
  `(ns, name)`**, and a miss falls through to an identity scan, never to a list of
  guessed alternative filenames.
- **P3. Namespace is total.** A descriptor with no `package.namespace` inherits its
  **owning index's** default namespace (§4.1: `xim-pkgindex → xim`,
  `mcpplibs → mcpplibs`, custom `[indices]` → its key). There is no empty namespace
  in a *resolved* identity. (The selector's `(∅, name)` discovery candidate is an
  *ingestion-time* wildcard, not a resolved identity — see §四.4.)
- **P4. One choke point.** All filesystem resolution funnels through a single
  `PackageLocator`. No caller runs `directory_iterator`, builds a filename, or
  probes `exists()` itself.
- **P5. Deterministic precedence.** When an identity could resolve in multiple
  indexes, an explicit ordered precedence decides — never filesystem iteration
  order.

---

## 四、Target architecture

### 4.1 The single source of truth: `IdentityIndex`

Each index root owns one lazily-built, cached map from **declared identity** to a
located record. It is built by reading **only** each descriptor's `package{}`
header — the filename is never parsed.

```cpp
struct PackageIdentity { std::string ns; std::string name; };   // normalized (§4.2)

struct DescriptorRecord {
    PackageIdentity        identity;   // declared + normalized — the key
    std::filesystem::path  path;       // where the bytes happen to live (opaque)
    std::string            indexName;  // owning index (for payload reuse + precedence)
};

class IdentityIndex {                  // one per index root, cached on the root's
                                       // refresh marker (rebuilt only when index changes)
public:
    // Built by: for each *.lua under root/pkgs/**, read package{namespace,name},
    // normalize via canonical_xpkg_identity(declaredNs, declaredName, rootDefaultNs),
    // insert {identity -> record}. Filename/bucket are NOT inputs.
    const DescriptorRecord* get(const PackageIdentity& id) const;   // exact (ns,name) lookup
};
```

Collisions (two files in one index normalizing to the same identity) surface as an
insert conflict — reported or precedence-resolved, **never** decided by scan order.
A descriptor named `xyz123.lua` that declares `(aimol, tensorvia-cpu)` **is**
`aimol.tensorvia-cpu`; a file named `tensorvia-cpu.lua` that declares
`(aimol, tensorvia-cpu)` is the *same* entry. The on-disk name is irrelevant.

### 4.2 The choke point: `PackageLocator`

```cpp
class PackageLocator {
    // Ordered index roots, most-specific first (P5):
    //   1. custom [indices] match (findIndexForNs) — scoped to one ns
    //   2. builtin precedence: mcpplibs, xim-pkgindex, <xlings extras...>
    //      (an explicit sorted vector, NOT directory_iterator order)
    std::optional<DescriptorRecord> locate(const PackageIdentity& want) const;
    std::optional<std::filesystem::path>
        locatePayload(const DescriptorRecord&, std::string_view version) const;
};

std::optional<DescriptorRecord>
PackageLocator::locate(const PackageIdentity& want) const {
    for (auto& root : orderedIndexRoots(want)) {              // deterministic (P5)
        // OPTIONAL fast path: canonical filename as a hint, still identity-verified.
        if (auto f = root.canonicalProbe(want);              // <ns>.<name>.lua | <name>.lua
            f && declaredIdentity(read(*f)) == want)          // P2: verify even the hint
            return DescriptorRecord{want, *f, root.name};
        // AUTHORITATIVE path: exact (ns,name) lookup in the identity map (P0/P1).
        if (auto* hit = root.identityIndex().get(want))       // no filename guessing (P2)
            return *hit;
    }
    return std::nullopt;
}
```

Drop the fast path entirely and `locate` is still 100% correct — only slower. That
is the litmus test for P2.

### 4.3 Every layer becomes a thin delegate

The 12 blind-first-hit sites inventoried in
[`2026-06-20`](2026-06-20-package-resolution-architecture.md) §3 all reduce to
`PackageLocator` calls:

| layer | today | target |
|---|---|---|
| candidate selection (`selectDependencyCandidate`, the bug) | `exists(<ns>.<short>.lua)` probe | for each candidate `c`: `locate(c)` and verify declared identity == `c`; pick first hit |
| descriptor read (`read_xpkg_lua*` ×3) | candidate-filename scan + gate | `locate(want).content` |
| payload (`install_path*`, `resolve_xpkg_path`, `scan_legacy_install_dirs`) | dir-name guessing | `locatePayload(record, ver)`, reusing the resolved `indexName` |
| SemVer (`resolve_semver`) | `read_xpkg_lua` then list versions | `locate(want)` then list versions |
| **emit** (`pipeline.cppm:89`, `xpkg_emit`) | filename/key = `pkg.name` verbatim | filename/key = canonical render of `(ns,name)`; declare both `namespace` + `name` |
| **index cache key** | declared `name` verbatim | canonical `(ns,name)` (folds `namespace` in) |

The structural win is unchanged from §5: **descriptor and payload share the
resolved `indexName`, so they can never disagree about which package they found** —
and now selection shares it too, so it can never pick a candidate the loader will
reject.

### 4.4 Candidate selection, corrected

The dotted-selector ladder (§2.3) stays exactly as designed — it is an *ordered
list of identities to try*, which is correct. Only the **disambiguator** changes,
from "does the canonical filename exist?" to "does an entry with this declared
identity exist?":

```cpp
for (auto& c : selector.candidates) {                 // mcpplibs.aimol/…, then aimol/…
    auto want = normalize(c);                         // §4.2
    if (want.ns.empty()) {                            // discovery candidate (bare ingestion)
        if (auto r = locator.locateByName(want.name)) // name-only, across precedence
            { select(r->identity); break; }           // resolves to a REAL (ns,name) — P3
    } else if (auto r = locator.locate(want)) {       // exact identity lookup — P0
        select(r->identity); break;
    }
}
```

Re-tracing the bug under this design, with the file still at the
non-canonical `t/tensorvia-cpu.lua`:

- `aimol.tensorvia-cpu`: cand① `locate(mcpplibs.aimol, tensorvia-cpu)` →
  `IdentityIndex` has no such declared identity → miss. cand②
  `locate(aimol, tensorvia-cpu)` → the map (built from the descriptor's declared
  `namespace="aimol"`, `name="tensorvia-cpu"`) **hits**, regardless of filename →
  **selected**. ✔ Fixed.
- `tensorvia-cpu`: cand① `(mcpplibs, tensorvia-cpu)` → miss; cand② `(∅, …)` →
  `locateByName("tensorvia-cpu")` → resolves to the real `(aimol, tensorvia-cpu)`
  (P3) → selected. ✔ Still works, now for a principled reason.
- `mcpplibs.tensorvia-cpu`: single candidate `(mcpplibs, tensorvia-cpu)` → no
  declared `(mcpplibs, tensorvia-cpu)` exists → clean "not found". ✔ Correct
  rejection (the package genuinely is not in the `mcpplibs` namespace).

The discovery candidate `(∅, name)` is the **only** place an empty namespace
appears, and it is resolved to a real `(ns, name)` before anything downstream sees
it (P3). No empty namespace ever reaches the locator's exact path, the lockfile, or
the install layer.

### 4.5 Emit & cache: stop minting filename-shaped keys

`mcpp emit xpkg` (`src/publish/pipeline.cppm`) currently derives both the output
filename and the de-facto index key from `pkg.name` verbatim (`pipeline.cppm:89`,
`:159`). Under P1 it must render from the canonical identity:

- always write **both** `package.namespace` and `package.name` (short form),
- choose the file path as the canonical render `pkgs/<first(ns)>/<ns>.<name>.lua`
  *as a convention only* — never as a key,
- key the index/cache on canonical `(ns, name)`, so the split form
  (`name=tensorvia-cpu`, `namespace=aimol`) and the FQN form
  (`name=aimol.tensorvia-cpu`) produce **identical** keys and identical resolution.

This makes the producer (emit), the catalog (cache), and the consumer (locate)
agree on one key. It also means a non-canonically-named legacy descriptor still
resolves (P2) while new emits self-heal toward the canonical layout.

### 4.6 The complete matching-rules table

Identity has exactly two fields. Everything a user writes is a *selector* that
expands to an ordered list of candidate identities; every candidate resolves by one
rule. This table is the single normative reference.

**(a) What a user may write in `mcpp.toml` → ordered candidates**

`resolve_dependency_selector` (`src/pm/dependency_selector.cppm:59`),
`kDefaultNamespace = "mcpplibs"`. `∅` = empty namespace (discovery wildcard, never a
resolved identity — P3).

| # | user-writable form | example | ordered candidates `(ns, name)` | notes |
|---|---|---|---|---|
| 1 | bare, 1 segment | `cmdline = "…"` | `(mcpplibs, cmdline)` → `(∅, cmdline)` | mcpplibs first, then peer-root discovery |
| 2 | dotted, front ≠ `mcpplibs` | `aimol.tensorvia-cpu = "…"` | `(mcpplibs.aimol, tensorvia-cpu)` → `(aimol, tensorvia-cpu)` | **the incident**; peer-root is 2nd |
| 3 | dotted, front = `mcpplibs` | `mcpplibs.cmdline = "…"` | `(mcpplibs, cmdline)` | explicit prefix; single candidate |
| 4 | deep dotted | `imgui.backend.glfw = "…"` | `(mcpplibs.imgui.backend, glfw)` → `(imgui.backend, glfw)` | split on **last** dot |
| 5 | explicit subtable | `[dependencies.compat]`<br>`gtest = "…"` | `(compat, gtest)` | subtable root is authoritative; **no** mcpplibs priority |
| 6 | explicit default subtable | `[dependencies.mcpplibs]`<br>`cmdline = "…"` | `(mcpplibs, cmdline)` | — |
| 7 | quoted legacy dotted | `"aimol.tensorvia-cpu" = "…"` | same as #2 | compat input, not preferred spelling |
| 8 | path / git inline | `x = { path = "…" }` | — | bypasses the index entirely |

**(b) What a package may declare in `package{}` → canonical `(ns, name)`**

`canonical_xpkg_identity(declaredNs, declaredName, owningIndexNs)` (§4.2,
`manifest.cppm:1628`). The filename is **not** an input.

| declared `namespace` | declared `name` | owning index | canonical `(ns, name)` |
|---|---|---|---|
| `aimol` | `tensorvia-cpu` | mcpplibs | **`(aimol, tensorvia-cpu)`** ← the incident package |
| `mcpplibs` | `cmdline` | mcpplibs | `(mcpplibs, cmdline)` |
| `mcpplibs` | `mcpplibs.cmdline` | mcpplibs | `(mcpplibs, cmdline)` (prefix-embedded == bare) |
| `compat` | `compat.zlib` | mcpplibs | `(compat, zlib)` |
| *(none)* | `zlib` | xim-pkgindex | `(xim, zlib)` (index-owned ns) |
| *(none)* | `tinycfg` | `local-dev` | `(local-dev, tinycfg)` |
| `a.b` | `c` | — | `(a.b, c)` |
| *(none)* | `a.b.c` | — | `(a.b, c)` |

Equivalence (must all resolve identically): `(a.b, c)` ≡ declared `(a, b.c)` ≡
declared `(∅, a.b.c)` in the `a.b`-owned index. The user's point, encoded.

**(c) Resolving one candidate against one declared identity**

| candidate kind | rule | matches `(aimol, tensorvia-cpu)`? |
|---|---|---|
| qualified, ns non-empty | **exact tuple equality** `cand == declared` | `(aimol, tensorvia-cpu)` ✅ · `(mcpplibs.aimol, …)` ❌ · `(mcpplibs, …)` ❌ |
| discovery, ns = `∅` | ~~match by `name` alone across the precedence path~~ **SUPERSEDED — see the note below**; **resolve to the declared `(ns, name)`** before returning | ~~`(∅, tensorvia-cpu)` ✅ → resolves to `(aimol, tensorvia-cpu)`~~ |

> **SUPERSEDED by #278 (mcpp 0.0.105).** The discovery rung is **not** a
> cross-namespace wildcard. It is the "upstream package that declares no
> `namespace`" rung, and a hit whose descriptor declares a non-empty namespace is
> now REJECTED at the dependency-resolution call site (`selectDependencyCandidate`
> in `prepare.cppm`; the gate itself is unchanged, because `mcpp new --template X`
> still discovers by short name). A bare `[dependencies]` key therefore resolves in
> exactly three places: `mcpplibs`, `compat`, and no-namespace upstream packages.
>
> Why the reversal: "match by name alone across the precedence path" makes
> resolution depend on which indices happen to be present. Two namespaces owning
> the same short name would be settled by an index ordering that has **no total
> order** across user-added `[indices]`, and adding an index could silently
> retarget an existing dependency. Reproducibility beats the convenience.
>
> The **P3 half of this row still stands and is now actually implemented**: a
> discovery hit writes back the namespace the DESCRIPTOR declares, not the
> candidate's empty one. Caveat: an empty declared namespace is a legal identity
> for upstream bare packages, not a hole to fill — the "no empty namespace ever
> reaches the install layer" claim in §4.4 presumes §4.1 index-owned namespace
> attribution, which remains unimplemented.
>
> See `.agents/docs/2026-07-25-issue278-descriptor-name-form-canonicalization-design.md` §3.2.

**Selection** = first candidate (in the §(a) order) that finds a declared identity by
rule §(c). Crucially, "finds" means **an entry with that declared identity exists in
the index** (`IdentityIndex.get`) — *not* "a file with the candidate's canonical name
exists on disk." That one-word difference (declared-identity vs filename) is the
entire bug and the entire fix.

Worked, for `aimol.tensorvia-cpu` (file at the non-canonical `pkgs/t/tensorvia-cpu.lua`):
cand① `(mcpplibs.aimol, tensorvia-cpu)` → no such declared identity → skip; cand②
`(aimol, tensorvia-cpu)` → declared identity exists (read from the descriptor header,
filename irrelevant) → **selected**. Under today's filename-probe selection, cand②'s
canonical file `a/aimol.tensorvia-cpu.lua` is absent, so cand② is skipped and cand①
is wrongly kept — the failure.

---

## 五、Why this is the architecture, not a patch

- **A patch** would special-case the failing shape — e.g. add `tensorvia-cpu.lua`
  as another guessed filename in the strict reader, or rename the file in the
  index. That re-encodes "filename is identity" and breaks again on the next
  package whose bytes don't sit at the guessed path.
- **This design deletes the premise.** Identity is `(pkg.ns, pkg.name)`, read from
  the file's declared header; the filename is demoted to an optional, always-verified
  accelerator that can be removed without changing any result. Selection, load,
  payload, SemVer, emit, and cache converge on one `(ns,name)` key through one
  `PackageLocator`. The whole class of "same bytes, different result" / "works bare,
  fails qualified" / "works in index A, not index B" defects dissolves, because none
  of them can be expressed once the filename is not a key.

It is also **convergence, not invention**: `canonical_xpkg_identity`
(`manifest.cppm:1628`), the identity gate (`xpkg_lua_identity_matches`), sorted
index dirs, and the scoped index-owned-namespace attribution already exist (#136,
`443b7d3`, `c55efbd`). This document finishes wiring them into the **selection**,
**payload**, **emit**, and **cache** layers that #136 left filename-shaped, behind
the §5 `PackageLocator` choke point.

---

## 六、Migration plan (incremental, behavior-preserving)

- **Step 0 — Hotfix (unblock the user today).** In `selectDependencyCandidate`,
  replace the `readStrictLuaForCandidate` canonical-filename probe with an
  identity-verified read: for each candidate, read the descriptor by *any* filename
  under the relevant index and accept on declared-identity equality (reuse
  `read_xpkg_lua` + `xpkg_lua_identity_matches`, which already exist). Minimal,
  surgical, closes the `aimol.tensorvia-cpu` case. Add e2e: a descriptor filed under
  a **non-canonical** name resolving for a **qualified custom-ns** request.
- **Step 1 — Extract `IdentityIndex`.** Build the `(ns,name)→record` map per index
  root from declared headers; cache on the existing refresh marker. Make
  `read_xpkg_lua*` thin delegates. Behavior-preserving.
- **Step 2 — `PackageLocator` choke point.** Route all 12 sites (selection, reads,
  payload, SemVer) through `locate`/`locatePayload`. Remove every caller-side
  `directory_iterator` / filename build / `exists()` probe.
- **Step 3 — Emit & cache on canonical identity.** `mcpp emit xpkg` writes both
  `namespace`+`name` and keys on canonical `(ns,name)`; index cache key folds
  `namespace` in. One-time deprecation warning when a descriptor's filename is
  non-canonical, so data migrates.
- **Step 4 — 1.0.0 cleanup.** Delete `xpkg_lua_candidates` /
  `install_dir_candidates` fuzzy generators, `scan_legacy_install_dirs`, and the
  canonical-filename fast path if profiling allows. Settles the standing
  `// remove in 1.0.0` debt.

---

## 七、Test coverage — why it shipped, and what to add

### 7.1 Why the existing suite was green while the feature was broken

The dotted-selector feature (#37cbc83) and the identity gate (#136) *did* ship with
tests. They were green because **every fixture sits at its canonical filename and/or
routes through a `[indices]`-scoped index** — i.e. they exercise exactly the paths
that route *around* the bug.

| existing test | what it covers | why it misses this bug |
|---|---|---|
| `CanonicalIdentity.*` (27 cases, `test_manifest.cpp`) | the `(ns,name)` *normalization* — incl. `BareNameCombinesWithNamespace`, hierarchical, prefix-embedded | tests the pure function; never touches **selection-against-filesystem** |
| `DependencySelector.*` (`test_pm_compat.cpp:50`) | candidate *generation* (the §(a) ladder) | asserts the candidate list is right; never resolves it against descriptors |
| `PmPackageFetcher.ResolvesCompatZlib…` (`test_pm_package_fetcher.cpp`) | identity gate in `read_xpkg_lua*`; cross-index order | fixture `compat.zlib.lua` is at its **canonical** path; never a non-canonical filename |
| `PmPackageFetcher.LocalPathIndex…` | index-owned namespace | `read_xpkg_lua_from_path` (**scoped**), canonical `tinycfg.lua` |
| e2e `62_dotted_dependency_selector_priority` | dotted selector fallback `imgui.core` | `[indices]` **path index** (scoped, not builtin) **and** canonical filename `i/imgui.core.lua` |
| e2e `63_bare_dependency_peer_root_priority` | bare → peer-root `(∅, imgui)` | canonical filename `i/imgui.lua` |

**The uncovered intersection — the production scenario — is all three at once:**
(1) a **builtin** index (namespace *not* in `[indices]`, so the multi-candidate
`selectDependencyCandidate` path runs), (2) a descriptor filed under a
**non-canonical** filename, (3) a **qualified** request. No unit or e2e test puts
those together, so the canonical-filename-only candidate reader
(`readStrictLuaFromPkgsDir`) was never observed failing. The bug lives precisely in
the seam none of the axes crossed.

A second structural gap: `selectDependencyCandidate` (and its strict reader) is an
in-`prepare.cppm` lambda with **zero direct unit coverage** — its behavior is only
ever exercised end-to-end, and only on canonical fixtures.

### 7.2 Required additions (this is the coverage that should have existed)

**Unit — lock the read-layer invariant (green today; guards against regressing the
already-correct layer):**
- `PmPackageFetcher.ResolvesCustomNamespaceDescriptorUnderNonCanonicalFilename`:
  stage a descriptor declaring `(aimol, tensorvia-cpu)` at the **bare** path
  `mcpplibs/pkgs/t/tensorvia-cpu.lua`; assert `read_xpkg_lua_from_project_data(…,
  "aimol", "tensorvia-cpu")` resolves it. Proves the read layer keys on declared
  identity, not filename — scoping the defect to *selection*. *(Added.)*

**e2e — the real regression (red until Step 0 lands):**
- `tests/e2e/76_qualified_custom_ns_noncanonical_filename.sh`: a **builtin** mcpplibs
  index (no `[indices]` entry), a descriptor declaring `(aimol, tensorvia-cpu)` filed
  at the non-canonical `pkgs/t/tensorvia-cpu.lua`, requested as
  `aimol.tensorvia-cpu`. Asserts `mcpp build` resolves it and the lock records
  `namespace = "aimol"`. Also asserts bare `tensorvia-cpu` and rejects
  `mcpplibs.tensorvia-cpu` (clean not-found) per §4.4. *(Added, gated behind
  `MCPP_E2E_INCLUDE_PENDING=1` until the Step 0 fix lands, then un-gate.)*

**Future, once the identity-first selector exists (P2 litmus):**
- **Filename-droppability property:** rename every resolver fixture to a random
  string; the full suite must stay green.
- **Identity invariant fuzz:** the resolver never returns a record whose declared
  `(ns,name)` ≠ the resolved coordinate, over filename/bucket-colliding fixtures.
- **Selector matrix × filename matrix:** every §4.6(a) row against descriptors filed
  at canonical *and* arbitrary paths — the cross-product that was missing.
- **Emit round-trip:** `emit xpkg` for split-form and FQN-form inputs yields
  byte-identical canonical `(ns,name)` keys and mutually resolvable output.

---

## 八、One-paragraph summary

`aimol.tensorvia-cpu` fails because mcpp's candidate-selection layer proves a
package's identity by probing whether its **canonical filename** `<ns>.<name>.lua`
exists, while the descriptor's bytes live under a non-canonical filename — so the
correct peer-root candidate `(aimol, tensorvia-cpu)` is invisible and the request is
pinned to the wrong front candidate `(mcpplibs.aimol, …)` and rejected; the bare form
survives only because its fallback candidate has an empty namespace that the matcher
treats as a name-only wildcard. The fix is not another guessed filename but the
removal of filename from the key space entirely: identity is the 2-tuple
`(pkg.ns, pkg.name)` read from each descriptor's declared header, indexed into one
`(ns,name)→record` `IdentityIndex` per root, served through a single
`PackageLocator` that selection, descriptor read, payload, SemVer, emit, and the
index cache all delegate to — realizing the deferred §5 of the 2026-06-20
architecture so that resolution is correct even if every descriptor file were
renamed to a random string.
