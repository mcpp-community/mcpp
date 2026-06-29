# Feature System v2 — Capability-Oriented Model (Design)

Date: 2026-06-29
Status: **S1 + S3 implemented & shipped** (see Implementation Status below);
S2 scoped as the documented next stage.
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
- **Stage 2 — optional-dep activation + feature-union unification: NEXT.**
  Deliberately deferred from this release. Rationale: activating a *new*
  dependency from a feature requires moving feature computation ahead of
  dependency resolution (resolution-phase reordering) — a deeper, higher-risk
  change. It is also **not required** for the capability/Eigen use case, which
  binds over providers that are explicitly declared as dependencies. Shipping
  S1+S3 first matches this doc's "each stage independently shippable" intent and
  keeps the release low-risk.

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
