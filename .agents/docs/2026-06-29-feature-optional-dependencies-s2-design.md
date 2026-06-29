# Feature System v2 — Stage 2: feature-activated optional dependencies (Design)

Date: 2026-06-29
Status: **S2a implemented**; S2b (feature unification) next.
Builds on: `.agents/docs/2026-06-29-feature-capability-model-design.md`
Scope: `src/manifest.cppm` (parse), `src/build/prepare.cppm` (worklist resolution
+ feature activation), `src/pm/dep_spec.cppm` (DepSpec reuse).

## Implementation status

- **S2a — DONE.** `Manifest.featureDeps` (`map<feature, map<depKey, DependencySpec>>`).
  Parsed from the TOML `[feature-deps.<name>]` section and from a Lua descriptor
  feature's nested `deps = { ["name"] = "ver" }`; Lua feature `implies` is now
  parsed too (was TOML-only). In `prepare_build`, two local lambdas
  `activateFeatures` / `mergeActiveFeatureDeps` merge a manifest's active
  feature-deps into its `dependencies` map — for the root before the worklist is
  seeded, and for each dependency right after its manifest loads (before its
  children are pushed). The existing worklist BFS then fetches/version-merges
  them, and Stage-3 capability binding finds a feature-pulled provider in the
  graph. Optional-by-default falls out for free (a dep declared only under a
  feature is never seen by the worklist unless the feature is active).
  Tests: `e2e/82_feature_optional_deps.sh`, `Manifest.FeatureDepsTomlSection`,
  `SynthesizeFromXpkgLua.FeatureDepsAndImplies`.

  > Implementation note: `activateFeatures`/`mergeActiveFeatureDeps` MUST be
  > local lambdas, not file-scope functions. As exported (inline) functions in
  > this module-interface unit their `std::map` instantiations leak into the
  > emitted BMI and trip a GCC-16 modules bug — *another* TU importing `std`
  > then fails with `fatal error: failed to load pendings for __normal_iterator`.
  > Keeping them local confines the instantiations to the implementation.

- **S2b — feature unification: NEXT.** Union feature requests per resolved
  package identity across the graph (today the first requester's features win).
  Needed for correct diamond behavior with feature-deps; called out separately
  below because it is the one genuine resolver-semantics change.

---

## 1. Problem

A feature cannot pull a dependency today. `[features]` entries parse `implies`,
`defines`, `requires`, `provides` — the `deps` key is explicitly reserved
("`requires`/`provides`/`deps` keys are reserved for later stages",
`manifest.cppm`). So:

- `requires = ["blas"]` (Stage 3) only **binds** a provider that is already in
  the dependency graph; it does not bring one in.
- There is no way to say "*activating feature X pulls dependency Y*", which is
  the natural way to express optional backends (e.g. Eigen's `use_blas` wanting
  an external OpenBLAS), optional codecs, GPU backends, etc.

This is **Stage 2** of the capability model. Implementing it makes the
provider/consumer story self-contained: a single feature can pull a provider
**and** bind the capability to it.

## 2. Goals

1. `features.<name>.deps` — a dependency listed under a feature is pulled **only
   when that feature is active**. A dependency under `[dependencies]` is always
   pulled (unchanged). Optionality is expressed by *where* the dep is declared,
   not a separate `optional = true` flag (simpler than Cargo).
2. Works for the **root package** (`mcpp build --features X` pulls X's deps) and
   **transitively** (a dependency's active feature pulls that dependency's deps).
3. **Composes with Stage 3**: a feature can `deps` a provider package **and**
   `requires` the capability it provides; the resolver then binds the just-pulled
   provider. This is the headline (`backend-openblas` below).
4. **Feature unification** (Stage 2b): when the same package is reached with
   different feature sets from different consumers, the sets are **unioned**, so
   all feature-deps are pulled and all feature effects apply (Cargo's additive
   model). Replaces today's first-requester-wins behavior.
5. No regression: packages with no feature-deps resolve exactly as today.

## 3. Key insight — it rides the existing worklist BFS

Dependency resolution in `prepare_build` is already a breadth-first worklist
(`prepare.cppm:849` `WorkItem`, `:1547` seed from the root manifest, `:1560`
`while (!worklist.empty())`). Each `WorkItem` carries the `DependencySpec`
(including its requested `features`), the requester, and the consumer slot.
Transitive deps are discovered by pushing a loaded manifest's `[dependencies]`
onto the same worklist.

So feature-deps need **no new resolution phase** and **no re-entrancy** (the
earlier worry). They are simply *more deps pushed onto the worklist* at the
moment a package's manifest is loaded and its active feature set is known:

```
seed worklist:
  root [dependencies]                                  (existing)
  + root active-feature deps   (default ∪ --features)  (NEW)

per worklist item (a dep whose manifest just loaded):
  push its [dependencies]                               (existing)
  + push its active-feature deps                        (NEW)
       active = item.spec.features ∪ dep.default ∪ implied(expanded)
```

The BFS then fetches/resolves/version-merges the feature-deps exactly like any
other dep. Stage 3 capability binding (already implemented, `prepare.cppm`
`capProviders`/`capRequires`) runs after resolution and now finds the
feature-pulled provider in `packages`, binding the capability to it — no Stage 3
change required.

## 4. Data model

Add to `Manifest` (next to `featuresMap` / `featureRequires`):

```cpp
// feature name → dependencies activated by that feature. A dep that appears
// ONLY here (not in [dependencies]) is optional: resolved only when the
// feature is active. Each entry is a full DependencySpec (version/path/git +
// its own features/backend), so a feature-dep can itself request features.
std::map<std::string, std::map<std::string, DependencySpec>> featureDeps;
```

A `map<depKey, DependencySpec>` per feature mirrors `Manifest::dependencies`, so
the same parse/merge/fetch code applies unchanged.

## 5. Syntax

### Lua descriptor (index packages — the primary surface)

```lua
features = {
    -- consumer capability switch (Stage 1+3, already supported)
    ["use_blas"]         = { defines = { "EIGEN_USE_BLAS" }, requires = { "blas" } },

    -- Stage 2: a backend convenience feature pulls a provider AND turns on the
    -- consumer switch. `deps` mirrors the top-level `deps` table shape.
    ["backend-openblas"] = {
        implies = { "use_blas" },
        deps    = { ["compat.openblas"] = "0.3.x" },
    },
}
```

### TOML project manifest (a project's own features)

```toml
[features]
use_blas         = { defines = ["EIGEN_USE_BLAS"], requires = ["blas"] }
backend-openblas = { implies = ["use_blas"] }

# Nested dep tables don't fit cleanly in a feature inline-table, so feature deps
# get their own section, keyed by feature name. (Parser: read [feature-deps.*]
# into Manifest.featureDeps with the existing dependency loader.)
[feature-deps.backend-openblas]
compat.openblas = "0.3.x"
```

Rationale: the Lua surface accepts a nested `deps = { ... }` inside the feature
table (the descriptor parser already walks nested tables). The TOML surface uses
a dedicated `[feature-deps.<name>]` section because TOML inline tables nested
inside a feature inline-table are awkward and the existing dependency loader
(`load_deps`) can be pointed at `[feature-deps.<name>]` verbatim.

## 6. Resolution flow (where it changes in `prepare_build`)

```
1. toolchain → workspace                                   (existing)
2. Compute ROOT active features early                      (NEW, small)
     active_root = expand(default ∪ --features)
3. Seed worklist:
     root [dependencies]                                   (existing, :1547)
     + for f in active_root: root.featureDeps[f]            (NEW)
4. Worklist BFS (existing, :1560):
     for each item: fetch + load manifest                  (existing)
     compute the dep's active features:                    (NEW)
       active = expand(item.spec.features ∪ dep.default ∪ implied)
     push dep [dependencies]                                (existing)
     + push dep.featureDeps[active]                         (NEW)
     SemVer-merge / dedupe by identity                      (existing)
5. Feature activation (defines/sources/MCPP_FEATURE_, :2049) (existing)
6. Capability binding (0/1/many over in-graph providers)    (existing, Stage 3)
     — now finds feature-pulled providers
7. modgraph → plan → lockfile                               (existing)
```

`expand(...)` is the existing `activate()` closure (`prepare.cppm:2064`) factored
out so it can run during the worklist, not only at step 5.

## 7. Feature unification (Stage 2b)

Today, when a dependency is requested by more than one consumer, only the first
requester's `features` are applied (`prepare.cppm` dep loop uses the first match
then `break`). With feature-deps this is incorrect: consumer A may request
`compat.eigen[backend-openblas]` while consumer B requests `compat.eigen[use_lapacke]`;
both feature-deps must be pulled.

Fix: accumulate the **union** of feature requests per resolved package identity
across the whole worklist, and:
- seed feature-deps for the union (so all optional deps are pulled), and
- at step 5, activate the union (so all defines/sources/capabilities apply).

The worklist already dedupes packages by identity and merges versions; unifying
the feature set is the analogous merge on the feature axis. This is the one piece
that is genuinely a resolver-semantics change (everything else is additive), so
it is called out as its own sub-stage with its own tests.

## 8. Worked example — OpenBLAS + Eigen (the headline)

```lua
-- compat.openblas  (a real provider package)
package = {
    name     = "compat.openblas",
    provides = { "blas", "lapack" },          -- Stage 3 capability
    mcpp     = { /* build that exposes -lopenblas, headers */ },
}
```

```lua
-- compat.eigen
features = {
    eigen_blas       = { sources = {"*/blas/*.cpp","*/blas/f2c/*.c"}, provides = {"blas"} },
    use_blas         = { defines = {"EIGEN_USE_BLAS"},    requires = {"blas"}   },
    use_lapacke      = { defines = {"EIGEN_USE_LAPACKE"}, requires = {"lapack"} },
    mpl2only         = { defines = {"EIGEN_MPL2_ONLY"} },
    -- Stage 2: one-liner backend that PULLS the provider and turns on the switch.
    ["backend-openblas"] = {
        implies = { "use_blas", "use_lapacke" },
        deps    = { ["compat.openblas"] = "0.3.x" },
    },
}
```

Consumer's `mcpp.toml`:

```toml
[dependencies]
compat.eigen = { version = "5.0.1", features = ["backend-openblas"] }
```

Resolution walk-through:

1. Worklist seeds `compat.eigen` (with `features=["backend-openblas"]`).
2. `compat.eigen` manifest loads. Active features expand:
   `backend-openblas` → `implies` → `use_blas`, `use_lapacke`.
3. `backend-openblas.deps` → **push `compat.openblas@0.3.x`** onto the worklist.
4. Worklist resolves `compat.openblas` → it `provides = ["blas","lapack"]`.
5. Feature activation: `use_blas`/`use_lapacke` contribute `-DEIGEN_USE_BLAS`
   / `-DEIGEN_USE_LAPACKE` and `requires = ["blas"]` / `["lapack"]`.
6. Capability binding (Stage 3): `blas`/`lapack` each have exactly one provider
   in the graph (compat.openblas) → bound. Its `-lopenblas` link/include flow to
   the consumer via usage requirements.

Result: a single `features = ["backend-openblas"]` pulls OpenBLAS, defines the
Eigen macros, binds the capability, and links the library — the full
provider/consumer loop with no manual `[dependencies]` entry and no
`[capabilities]` pin (one provider ⇒ unambiguous).

Note the mutual-exclusion rule still holds: `backend-openblas` must NOT also
imply `eigen_blas` (compiling Eigen's own BLAS while defining `EIGEN_USE_BLAS`
is self-contradictory — see the v2 design doc). `backend-openblas` is the
external-provider path; `eigen_blas` is the self-provider path; pick one.

## 9. Edge cases

- **Version conflict**: a feature-dep colliding with a top-level dep on a
  different version is handled by the existing SemVer merge across the worklist
  (no new logic).
- **Optional-only dep absent when feature off**: a package referenced *only* in
  `featureDeps` and never activated is never fetched (the worklist never sees
  it) — the desired optional behavior, for free.
- **Transitive feature-deps**: a feature-dep can itself carry `features=[...]`,
  whose feature-deps are pushed when that package is processed — natural BFS
  recursion.
- **Cycles**: the worklist's existing identity seen-set breaks cycles.
- **Dev-deps**: feature-deps are normal (non-dev) deps; they are not propagated
  through `[dev-dependencies]` rules.
- **`--strict`**: requesting an undeclared feature already errors under strict;
  a feature-dep that fails to resolve surfaces the existing fetch error.

## 10. Staging

| Stage | Content | Resolver change | Unlocks |
|---|---|---|---|
| **S2a** | Parse `featureDeps`; seed root + push per-dep feature-deps onto the worklist; factor `activate()` for reuse. | Additive (push more onto the existing worklist). | `--features X` and dep `features=[X]` pull X's deps; composes with Stage 3 to auto-bind a pulled provider. |
| **S2b** | Feature **unification**: union feature requests per package identity across the graph; activate + seed feature-deps for the union. | Semantics change (union vs first-wins). | Correct diamond behavior; multiple consumers' features all apply. |

S2a alone already delivers the OpenBLAS+Eigen example (single consumer, single
requester). S2b hardens multi-consumer graphs.

## 11. Testing

- **Parse** (`test_manifest`): `featureDeps` from the Lua descriptor and from
  `[feature-deps.<name>]`; a feature with no deps yields no entry.
- **S2a e2e**: a root feature pulls a path-dep only when active (and not when
  inactive — assert the dep is absent from the build/lockfile); a dep's feature
  pulls a transitive path-dep.
- **Composition e2e**: a `backend-*` feature that `deps` a provider + `requires`
  its capability resolves and binds with no explicit dependency/pin (the
  OpenBLAS+Eigen shape, using small synthetic provider/consumer packages like the
  existing `81_capability_binding.sh`).
- **S2b e2e**: two consumers request the same package with different features;
  both feature-deps are pulled and both defines applied.

## 12. Deliberately deferred

- **Mutually-exclusive feature groups / `conflicts`** (e.g. forbidding
  `eigen_blas` + `use_blas`): documented in the recipe for now; a declarative
  `conflicts` is a separate, later addition (the single-valued capability slot
  already covers the backend case).
- **`optional = true` on top-level deps + same-named auto-feature** (Cargo's
  other style): the `featureDeps` table covers the same need more directly;
  revisit only if a real case wants a top-level dep gated by an unrelated
  feature name.
- **Weak features (`dep?/feat`)**: not needed until a concrete case appears.
