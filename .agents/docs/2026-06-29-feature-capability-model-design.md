# Feature System v2 — Capability-Oriented Model (Design)

Date: 2026-06-29
Status: **S1 + S2a + S3 + interface-define propagation implemented & shipped**
(see Implementation Status below); the full eigen[backend-openblas] ecosystem
closed loop is validated end-to-end.
Scope: `src/manifest.cppm` (parse), `src/build/prepare.cppm` (feature activation +
resolver), `src/cli.cppm` / `src/cli/cmd_build.cppm` (`--cap`), mcpp-index recipe schema.

## Implementation Status

- **Stage 1 — feature `defines`: DONE.** `[features]` table form
  (`name = { defines = [...], implies = [...] }`) on both the TOML and Lua
  surfaces; active features contribute package-owned macros (bare name → `-D<x>`)
  next to the automatic `-DMCPP_FEATURE_<NAME>`. Tests: `e2e/80_feature_defines.sh`,
  `Manifest.FeatureTableFormDefinesAndImplies`.
- **Stage 3 — capabilities: DONE.** `provides` (package-level + per-feature),
  `requires` (per-feature), the 0/1/many binding over in-graph providers,
  `[capabilities]` pins and `--cap`. Tests: `e2e/81_capability_binding.sh`
  (6 cases), `Manifest.CapabilitiesProvidesRequiresAndPins`,
  `SynthesizeFromXpkgLua.CapabilitiesAndFeatureDefines`.
- **Stage 2a — feature-activated optional dependencies: DONE.** A dependency
  declared under `[feature-deps.<name>]` (TOML) or a feature's nested `deps`
  (Lua) is pulled into the worklist only when that feature is active. Feature
  activation (including transitive `implies`) is computed ahead of the
  resolution worklist via local lambdas in `prepare.cppm` (kept local to avoid a
  GCC-16 modules-BMI bug). Tests: `e2e/82_feature_optional_deps.sh`,
  `Manifest.FeatureDepsTomlSection`, `SynthesizeFromXpkgLua.FeatureDepsAndImplies`.
- **Interface-define propagation (header-only providers): DONE.** A dependency's
  active-feature `defines` are **interface requirements**: they flow into every
  consumer's own compile flags along Public/Interface dependency edges, mirroring
  `include_dirs`. This is required for header-only libraries whose feature switch
  only takes effect in the TU that includes their headers — the canonical case is
  Eigen's `use_blas` → `EIGEN_USE_BLAS`, which must be defined when the
  *consumer* compiles `a * b`, not only when Eigen's own anchor TU compiles. The
  automatic `MCPP_FEATURE_<NAME>` macro stays private to the owning package (it
  is a build signal, not a public contract). Implemented by routing feature
  defines through `PackageRoot::publicUsage` and extending the
  `computeUsageRequirements()` fixpoint to propagate `cflags`/`cxxflags`. Test:
  `e2e/83_feature_defines_propagate.sh`.
- **Stage 2b — feature-union unification across multiple consumers: NEXT.**
  Deliberately deferred. When two consumers request different feature sets on the
  same dependency, the activated set should be their union (single resolved
  instance). The current model activates per the first-seen consumer's request;
  divergent transitive feature requests are not yet unified. Not required for the
  validated Eigen/OpenBLAS use case.

### Validated closed loop (eigen[backend-openblas])

`mcpp build` of a consumer declaring
`compat.eigen = { features = ["backend-openblas"] }` exercises every stage:
`backend-openblas` → (implies) `use_blas` → `-DEIGEN_USE_BLAS` propagated to the
consumer's TUs + `requires "blas"`; `[feature-deps]` pulls `compat.openblas`,
whose xpkg `install()` hook builds `libopenblas.a` from source (BLAS-only,
`TARGET=GENERIC`, no Fortran) via the `xim:make` build-dep; `provides "blas"`
binds the capability; the provider's `-lopenblas` links. Verified: the produced
binary pulls OpenBLAS's `dgemm_` (not Eigen's built-in GEMM) and runs.

---

## 1. Problem

Today a mcpp feature, when activated, does exactly two things
(`src/build/prepare.cppm:2080-2119`):

1. injects `-DMCPP_FEATURE_<NAME>` into the package's cflags/cxxflags, and
2. gates source globs declared under `features.<name>.sources`.

The Lua descriptor parser only reads `sources` from a feature sub-table; every
other key is silently skipped (`src/manifest.cppm:1988-1999`). The TOML
`[features]` table is only "name → implied-feature array"
(`src/manifest.cppm:613-625`). There is **no way** for a feature to:

- contribute a preprocessor define the upstream library actually recognizes,
- pull in an optional dependency,
- select one backend implementation among mutually-exclusive alternatives.

### Trigger case — `compat.eigen` `blas`

Enabling Eigen's `blas` feature only produced `-DMCPP_FEATURE_BLAS` in
`compile_commands.json`; the macro Eigen actually reads, `-DEIGEN_USE_BLAS`, was
absent — so the BLAS backend was never enabled. Root cause is the limitation
above, already documented in the recipe header
(`mcpp-index/pkgs/c/compat.eigen.lua:30-39`).

> Note on Eigen semantics (used as the running example throughout): Eigen's
> `blas/` directory builds `eigen_blas`, an artifact that **provides** the
> standard BLAS ABI for *other* code to link. `-DEIGEN_USE_BLAS` is the
> opposite direction — it makes Eigen **consume** an external BLAS for its own
> matrix kernels. These are two distinct capabilities and must not be conflated
> under one feature name. The current `blas` feature implements the provider;
> the user expected the consumer.

## 2. Goals

1. A feature can express the real-world needs above while staying **simple by
   default** — header-only / no-feature builds are byte-for-byte unchanged.
2. **Full coverage**: optional dependencies, package-owned defines, and
   "pick exactly one backend" with structural mutual exclusion and a user
   override.
3. **Less is more**: the smallest primitive set that covers the above. Validated
   against industry practice (§3) — we deliberately drop mechanisms that other
   ecosystems regret.
4. **Backward compatible**: existing array-form `[features]` and Lua
   `features = { x = { sources = ... } }` keep their meaning; the automatic
   `-DMCPP_FEATURE_<NAME>` is retained.

## 3. Industry synthesis

Surveyed Cargo, Gentoo, Conan, vcpkg, Bazel, CMake, Meson, Debian/RPM, conda,
Spack, Nixpkgs, pkg-config. Distilled lessons that shaped this design:

| Source | Lesson taken |
|---|---|
| Cargo | Features are **purely additive**, unified as a **union** across the graph; mutually-exclusive features are an anti-pattern. |
| vcpkg | Features should **activate dependencies, not inject free-form compile flags** — arbitrary flags leak into the ABI and break composition. *(Sharpest constraint we adopt.)* |
| Gentoo | Constraints belong in a **declarative, solver-checked** form, never in imperative recipe code. |
| Conan | A **single multi-valued axis** expresses "pick one of N backends" natively — no boolean soup. |
| Bazel / CMake | Separate the **capability** (need BLAS) from the **backend selector** (`BLA_VENDOR` / `constraint_setting`); consumers code against an abstract target. |
| Debian / conda / Spack / Nix | A capability is just a **shared string** (`Provides`/`provides`); making the slot **single-valued** yields mutual exclusion **for free** (conda's same-name trick). Selection must be **deterministic** (default → single-candidate → pin), never a soft preference or silent first-match. |

**Two load-bearing rules:**

1. A feature that must affect compilation may contribute only a **package-owned,
   namespaced define** (e.g. `EIGEN_USE_BLAS`, `EIGEN_MPL2_ONLY`). Link flags and
   include paths come from the **bound provider package's own build config**, not
   invented by the feature.
2. "Pick one backend" is a **single-valued capability slot**, which gives
   mutual exclusion structurally — no constraint DSL, no `backend-*` boolean
   pile.

### Corollary — a feature define is an *interface* requirement

Rule 1 names *what* a feature may contribute to compilation (a package-owned,
namespaced define); it does not, by itself, fix *where* that define applies. For
a **header-only** provider the answer is forced: the library has no sources of
its own, so its feature switch only takes effect in the translation unit that
*includes its headers* — i.e. in the **consumer**. `EIGEN_USE_BLAS` must be
defined when the consumer compiles `a * b`, not (only) when Eigen's anchor TU
compiles. Therefore a feature's `defines` are treated as **interface
requirements**: they propagate to consumers along Public/Interface dependency
edges, on exactly the same machinery and visibility discipline as a dependency's
public `include_dirs` (`PackageRoot::publicUsage`, the `computeUsageRequirements`
fixpoint). This is the realization of Rule 1 for header-only providers, not an
exception to it — the define stays package-owned and namespaced; only its scope
is corrected.

Why this does **not** reintroduce the vcpkg failure mode ("flags leak into the
ABI, break composition"):

- **Only the namespaced, library-owned define crosses the boundary** — never
  free-form flags. Link flags / include paths still come from the bound
  provider's own build config (Rule 1 intact).
- **Visibility-bounded.** Propagation follows the same Public/Interface edges as
  include dirs; a `private` dependency edge keeps the define off the consumer's
  public interface.
- **ODR-safe by single-instance propagation.** Activation is unioned onto a
  single shared provider instance (Cargo model); propagation flows outward from
  that one `publicUsage`, so every consumer of the provider sees the *same*
  define set. A header-only library compiled with the switch in one TU and
  without it in another would be an ODR violation — single-instance propagation
  structurally prevents that split.
- **The automatic `MCPP_FEATURE_<NAME>` macro is deliberately NOT propagated.**
  It is not namespaced by the library (two packages may each declare a
  `use_blas` feature → colliding `MCPP_FEATURE_USE_BLAS`), so it stays private to
  the owning package as a local build signal. Only the namespaced user define is
  an interface contract — which reinforces, rather than relaxes, Rule 1.

Simplicity note (少即是多): *all* feature defines are interface defines; mcpp does
**not** add a CMake-style PUBLIC/PRIVATE/INTERFACE tri-state for defines — that is
precisely the complexity this design avoids. A define that happens to matter only
to the provider's own `.cpp` still propagates, but lands in consumers as an
unused, namespaced `-D` (harmless). Should a genuinely provider-private feature
define ever be needed, a `private-defines` key is the future-proofing escape
hatch; it is YAGNI today.

## 4. The model — two primitives

### Primitive ① Feature — additive, composable, does only this

```
FeatureDef {
  implies  : [feature]      // transitive activation (existing "implied")
  deps     : [DepSpec]      // optional dependencies activated by this feature
  defines  : [string]       // ONLY package-owned namespaced macros
  sources  : [glob]         // feature-gated sources (existing)
  requires : [capability]   // capabilities this feature needs   → Primitive ②
  provides : [capability]   // capabilities this feature satisfies → Primitive ②
}
```

- Activation is **additive** and **unioned** across the whole graph (Cargo
  model): if any consumer activates a feature on a package, it is on for that
  package in the single shared build. Model the *presence* of a capability,
  never its absence.
- `defines` is restricted by review/lint to the package's own macro namespace.
  Free-form `cflags` / `cxxflags` / `ldflags` / `includes` are **not** part of
  `FeatureDef` (see §7, dropped on purpose).
- `-DMCPP_FEATURE_<NAME>` is still injected for every active feature
  (back-compat + lets code `#ifdef`).

### Primitive ② Capability — shared string + single-valued binding

- **Declared symmetrically.** Providers: `provides = ["blas"]`. Consumers (a
  package, or one of its features): `requires = ["blas"]`. No separate registry
  — the provider set is derived by scanning declared `provides`.
- **The resolver's entire algorithm**, per required capability, over the solve
  closure:

  ```
  0 providers         → hard error: "no package provides 'blas'"
  1 provider          → bind it                      (zero-config common path)
  already in graph    → reuse it (never add a second)
  ≥2, none forced     → hard error listing every candidate (never silent)
  explicit pin        → always wins, collapses the ambiguity
  ```

- **Override / pin** (deterministic, mirrors Spack `providers:` / conda
  `blas=*=mkl`):
  - workspace/project: `[capabilities]` table in `mcpp.toml`, e.g.
    `blas = "openblas"`;
  - CLI: `--cap blas=openblas`.
- **Single-valued slot** = one bound provider per capability per build closure →
  mutual exclusion is structural; no `Conflicts` matrix needed.
- The bound provider is a **real package**; its `ldflags` / `includes` flow to
  the requirer through the existing usage-requirements path
  (`computeUsageRequirements`, `prepare.cppm:2046`). The feature itself never
  fabricates link flags.

## 5. Worked example — Eigen (end state)

```lua
-- compat.openblas  (a provider package; carries its own ldflags/includes)
package = {
    name     = "compat.openblas",
    provides = { "blas", "lapack" },
    -- ... xpm sources/build that expose -lopenblas etc.
}
```

```lua
-- compat.eigen
package = {
    name = "compat.eigen",
    mcpp = {
        include_dirs = { "*" },
        generated_files = { ["mcpp_generated/eigen_anchor.c"] = "int mcpp_compat_eigen_headers_anchor(void){return 0;}\n" },
        sources = { "mcpp_generated/eigen_anchor.c" },
        targets = { ["eigen"] = { kind = "lib" } },

        features = {
            -- PROVIDER: Eigen's own reference BLAS (eigen_blas). Self-contained,
            -- sources-only; Eigen can itself satisfy the `blas` capability.
            ["eigen_blas"]  = {
                sources  = { "*/blas/*.cpp", "*/blas/f2c/*.c" },
                provides = { "blas" },
            },
            -- CONSUMER: Eigen delegates its kernels to an external BLAS.
            ["use_blas"]    = {
                defines  = { "-DEIGEN_USE_BLAS" },   -- Eigen-owned macro
                requires = { "blas" },               -- resolver binds a provider
            },
            ["use_lapacke"] = {
                defines  = { "-DEIGEN_USE_LAPACKE" },
                requires = { "lapack" },
            },
            -- Pure package-owned define, no capability involved.
            ["mpl2only"]    = { defines = { "-DEIGEN_MPL2_ONLY" } },
        },
    },
}
```

Consumer's `mcpp.toml`:

```toml
[dependencies]
compat.eigen = { version = "5.0.1", features = ["use_blas"] }

# Optional: pin the backend. Omit when exactly one provider is in the graph.
[capabilities]
blas = "openblas"
```

Resolution walk-through:

1. `use_blas` is activated on `compat.eigen` → contributes `-DEIGEN_USE_BLAS`
   and `requires blas`.
2. Capability `blas`: the resolver scans `provides`. If only `compat.openblas`
   is in the closure → bind it (no `[capabilities]` needed). If both
   `compat.openblas` and `compat.mkl` are present and unpinned → hard error
   listing both, asking for `--cap blas=<impl>`. The `[capabilities] blas`
   pin collapses that.
3. The bound provider's `-lopenblas` (its own `ldflags`) flows into the link via
   usage requirements. The feature contributed only the one Eigen-owned macro.

The naming also resolves the original confusion: `eigen_blas` (provider) vs
`use_blas` (consumer) each map to a concept the user already knows (upstream's
`eigen_blas` target / the `EIGEN_USE_BLAS` macro). The ambiguous bare name
`blas` is retired.

## 6. Syntax & resolver placement

### 6.1 Lua descriptor (index packages)

- Extend the feature sub-table parser (`src/manifest.cppm:1983`) to accept
  `implies`, `deps`, `defines`, `requires`, `provides` in addition to `sources`.
  Unknown keys keep the current skip-with-warning behavior.
- Add a package-level `provides = { ... }` field.

### 6.2 TOML project manifest (`mcpp.toml`)

- `[features]` keeps the array shorthand (`name = ["implied", ...]`) **and**
  gains a table form: `name = { implies = [...], deps = [...], defines = [...],
  requires = [...], provides = [...] }`.
- New `[capabilities]` table: `blas = "openblas"` for user provider pins.
- `[package] provides = [...]` for packages that act as providers.

### 6.3 Resolver pipeline (inside `prepare_build`)

```
1. toolchain → workspace → dependency resolution            (existing)
2. Feature unification (new)  — union active feature set per package
                                across root --features, every dep spec's
                                features=[...], and transitive `implies`
3. Optional-dep activation (new) — pull deps referenced by active features;
                                re-run the resolution closure for new deps
4. Capability binding (new)   — for each required capability, apply the
                                0/1/many algorithm; error on conflict/missing
5. Contribution merge (generalized apply()) — merge each active feature's
                                defines + sources; bound provider's
                                ldflags/includes flow via usage requirements
6. required_features target gate → modgraph → plan → compile_commands (existing)
```

Step 2 fixes the long-standing gap that transitive dep→dep feature requests are
not propagated (`prepare.cppm:2053`).

## 7. Deliberately NOT in scope (less-is-more)

Each is something a surveyed ecosystem added and regrets, or that the
two-primitive model makes redundant:

- **Free-form `cflags` / `cxxflags` / `ldflags` / `includes` on a feature** —
  breaks composition (vcpkg). Link/include come from provider packages; compile
  tuning belongs in `[profile.*]` / `[build]`.
- **A general constraint DSL** (Gentoo `REQUIRED_USE` `^^`/`??`) — the
  single-valued capability slot already gives "exactly one backend." If two
  ordinary features genuinely conflict later, add a minimal `conflicts = [...]`;
  not in v1.
- **Versioned / ranged capabilities** (`requires blas >= 3.9`) — Debian allows
  only `=` here for good reason. Version the concrete package, not the abstract
  name.
- **Soft provider-preference lists** (Spack's chronic complaint) — selection is
  deterministic: default → single-candidate → pin, else hard error. No
  probabilistic preference.
- **Silent first-match** (pkg-config footgun) — ambiguity is always a loud
  error.
- **A separate capability object type / `replaces` / `obsoletes`** — a string
  plus `provides`/`requires` plus the 0/1/many rule is sufficient.
- **`backend-*` boolean features** — superseded by the capability slot + pin.

## 8. Staging (each stage independently shippable)

| Stage | Content | Touches resolver? | Unlocks |
|---|---|---|---|
| **S1 (P0)** | `FeatureDef` gains `defines` (package-owned macros); keep `sources`/`implies`. Parse `requires`/`provides` into the model but no binding yet (inert placeholders). | No — pure compile-flag layer | `EIGEN_USE_BLAS`, `EIGEN_MPL2_ONLY` immediately (BLAS still hand-linked by the user) |
| **S2 (P1)** | Optional `deps` activation + feature **union** unification / transitive propagation. | Yes | features that pull dependencies; correct transitive features |
| **S3 (P2)** | Capability `provides`/`requires` + the 0/1/many binding + `[capabilities]` / `--cap`. | Yes | backend pick-one with structural exclusion + override |

## 9. Backward compatibility

- Array-form `[features]` and Lua `sources`-only features are unchanged.
- `-DMCPP_FEATURE_<NAME>` is still emitted for every active feature.
- New keys are additive and optional; recipes that don't use them are
  unaffected.
- The strict-schema check (`prepare.cppm:2131`) extends naturally: a requested
  feature must still exist in `[features]`; a required capability with no
  provider is the new error class.

## 10. Testing

- **S1**: unit — a feature with `defines` emits the define into
  `compile_commands.json`; an inactive feature does not. e2e on
  `compat.eigen` `mpl2only`.
- **S2**: a feature's `deps` appear only when the feature is active; transitive
  `features=[...]` on a dep-of-dep is honored after unification; union of two
  consumers requesting different features.
- **S3**: 0-provider error; 1-provider auto-bind; 2-provider unpinned error
  listing candidates; pin via `[capabilities]` and `--cap`; bound provider's
  ldflags reach the requirer's link. e2e: `compat.eigen[use_blas]` +
  `compat.openblas` → `-DEIGEN_USE_BLAS` and `-lopenblas` both present.
