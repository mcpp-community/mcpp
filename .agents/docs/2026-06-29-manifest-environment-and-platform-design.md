# mcpp.toml: Build Environment, Platform-Conditional Config, and `build.mcpp` (Design)

Date: 2026-06-29
Status: **Phasing — L1 (flags) shipped in mcpp 0.0.74.** Remaining phases tracked
in §"Phasing" below. Synthesizes a multi-tool survey (Cargo, Zig,
vcpkg, Bazel, xmake, Conan) and the build-systems literature (*Build Systems à la
Carte*, PubGrub, hermeticity/SLSA) against mcpp's current internals. Scope:
`src/manifest.cppm`, `src/config.cppm`, `src/xlings.cppm`, `src/toolchain/*`,
`src/build/{prepare,plan,flags}.cppm`, `src/libs/toml.cppm`. Companion (consumer
side): mcpp-index `.agents/docs/2026-06-29-mcpp-native-workspace-ci-design.md`.

## 0. Organizing principle

From *Build Systems à la Carte* (Mokhov/Mitchell/Peyton Jones, ICFP 2018): a build
system's task graph is either **applicative** (dependencies known *before* running
a task → statically analyzable, lockable, cacheable, parallel-schedulable) or
**monadic** (dependencies discovered only by *executing* → maximally flexible, not
analyzable up front). The whole design follows one rule:

> **Keep the dependency graph applicative. Confine imperative logic to bounded
> leaves.** Anything that decides *which dependencies/inputs exist* is declarative
> (TOML); per-package flag/source/codegen choices may be imperative — but never
> gate the top-level graph.

Five layers result, from the host environment up to the produced artifact:

| Layer | Decides | Evaluated on | Today | Industry anchor |
|---|---|---|---|---|
| **L-1 Environment** | toolchains + host build-tools + env vars + per-project sandbox | host | only `index_repos` written; xlings `deps`/`workspace`/`envs`/`subos` unused | xlings (already); venv / nix-shell |
| **L0 Dependency categories** | normal / dev / build | normal·dev = target; build = host | all three parsed; `[build-dependencies]` inert | Cargo, Conan |
| **L1 Conditional graph** | per-OS/arch deps & features (+ lazy fetch) | resolved **target** triple | none in `mcpp.toml`; recipe-only host-keyed `mcpp.<os>` | Cargo `cfg()`, Zig `.lazy` + hash |
| **L2 Static codegen** | synthesized headers/sources | — | `generated_files` (exists) | — |
| **L3 `build.mcpp`** | host probing / dynamic codegen (**leaf only**) | host build, cfg gated on target | none (recipe `install()` is the *third-party* analog) | Zig `build.zig`, Cargo `build.rs` |

---

## L-1 — Build environment: align `mcpp.toml` to xlings' `.xlings.json`

### Current state
mcpp's only writer of `.xlings.json` is `seed_xlings_json` (`src/xlings.cppm:1070-1088`)
and the project variant `ensure_project_index_dir` (`src/config.cppm:661-705`); both
write **only** `index_repos` / `lang` / `mirror`. Toolchains come from `[toolchain]`
(`manifest.cppm:1072-1079`) → `xim:gcc@16.1.0` (`toolchain/registry.cppm:146-183`) →
`xlings install` into the `~/.mcpp/registry` sandbox. Host tools (ninja, patchelf)
are hard-pinned in `xlings.cppm:33-37`. The `workspace.<pkg>` pin you see in
`.xlings.json` is written by **xlings/xvm**, not mcpp.

Meanwhile xlings *already* models a full per-project environment (`xvm/README.md:84-92`,
`core/config.cppm`): `deps` (a project package-set, installed by bare `xlings install`),
`workspace` (target→version, **per-OS keyed**, resolved project > subos > global),
per-version `envs`, and named/anonymous `subos` (a project sandbox at
`<proj>/.xlings/subos/...` with its own `bin/lib/usr`). **mcpp surfaces none of it.**

### Design — `[environment]` materializes 1:1 onto xlings keys
Do **not** invent new vocabulary. A `mcpp.toml` `[environment]` block maps directly
to the xlings `.xlings.json` schema and is written into mcpp's existing project file
`<proj>/.mcpp/.xlings.json` (extend the `index_repos`-only writer at
`config.cppm:699-705` to also emit `deps`/`workspace`/`envs`/`subos`):

```toml
[environment]
subos = "dev"                          # → .xlings.json "subos" (named project sandbox)
tools = ["make@4", "python@3.13.1"]    # → "deps" (host build-tools; bare `xlings install` provisions)
[environment.workspace]                # → "workspace" (pin tool versions; per-OS values allowed)
gcc = "16.1.0"
[environment.env]                      # → per-tool "envs" applied by xvm shims
OPENBLAS_NUM_THREADS = "1"
```

- **`[toolchain]` folds in**: today it installs globally; route it into the project
  `workspace` so two checkouts on one machine pin different compilers via xvm shims
  (xlings supports this; mcpp just never seeds it). `[toolchain]` stays as the
  ergonomic shorthand; `[environment.workspace]` is the general form.
- **Provisioning** = the already-wired project-mode `build_command_prefix`
  (`xlings.cppm:716-724`) + bare `xlings install` over the emitted `deps`.
- **This is purely "surface + writer"** — no new resolution machinery; the
  materialization target is wired but unused.

### Gap closed
Per-project toolchain pinning, host build-tools (`make`/`python`/`cmake`) as
declared deps, per-tool env vars, and a reproducible per-project sandbox — all
expressible, all backed by existing xlings behavior.

---

## L0 — Dependency categories: keep three axes, wire `build`

Keep Cargo/Conan's **normal / dev / build** trichotomy (mcpp already declares all
three, `manifest.cppm:251-253`; `[build-dependencies]` is parsed but **inert** — no
consumer in `prepare`/`plan`). They answer two orthogonal questions:

- **normal vs dev** = *when* linked (always / test-only). Both evaluated against the
  **target**; both link into a runnable artifact (`mcpp test` resolves dev-deps —
  `manifest.cppm:44-46`).
- **normal vs build** = *which machine* it runs on (target / **host**). build-deps
  do not enter the product; they feed the build itself.

**Three distinct "build-tool" homes — do not conflate:**
1. **Recipe source-build tools** — `xpm.<os>.deps` in an xpkg recipe (`xim:python`,
   `xim:make`); host, install-time, resolved by xim/`pkginfo.build_dep`. *This is
   where `compat.xcb`'s Python lives.*
2. **Project environment tools** — L-1 `[environment].tools` (host tools the project
   needs available: cmake, protoc).
3. **`build.mcpp` libraries** — `[build-dependencies]` (host libraries the native
   build program links). **Wire this here** — it gains a real consumer at L3.

---

## L1 — Conditional dependency graph (Cargo-style `cfg`, target-evaluated)

### Decision: Cargo-style `[target.'cfg(...)']` tables, trimmed token set
A declarative table keeps the graph applicative (L0 principle). vcpkg's inline
`platform=` has no home for flags; a flat `[<os>]` block can't express arch /
combinations / negation. Cargo's table shape wins and — unlike Cargo — mcpp lets
the **same predicate namespace carry both deps and flags** (Cargo can't put flags
in the manifest at all):

```toml
# exact-triple escape hatch (already exists, highest precedence)
[target.x86_64-pc-windows-msvc.dependencies]
detours = "4.0"

# predicate tables — primary mechanism; deps AND flags share the namespace
[target.'cfg(windows)'.dependencies.compat]
openblas = { version = "0.3.33", lazy = true }

[target.'cfg(windows)'.build]
ldflags = ["-Llib", "-llibopenblas"]

[target.'cfg(all(linux, not(arch = "aarch64")))'.build]
cxxflags = ["-march=x86-64-v2"]
```

- **Grammar**: Cargo's `all()/any()/not()` over `key = "value"` plus bare aliases
  `windows`/`unix`/`linux`/`macos`. **Token set trimmed to mcpp's existing
  dimensions**: `os`, `arch`, `family`, `env` — these *are* `AbiDim`
  (`toolchain/abi.cppm:34-38`); `parse_abi_capability` already parses
  `abi:arch=aarch64`. Defer `pointer_width`/`endian`/`vendor` until asked.
- **Evaluate against the resolved TARGET triple, not host.** `abi_profile(triple)`
  (`abi.cppm:67-91`) derives `os`/`arch` from any triple. Timing is already correct:
  `--target` + `[target.<triple>]` resolve at `prepare.cppm:497-560`, *before* dep
  resolution at `~731` — merge the matching `cfg` tables into `m->dependencies` /
  `buildConfig.*` in that window. (The recipe-side `mcpp.<os>` merge keys on **host**
  `xpkg_platform` — wrong for cross-compiles; the `mcpp.toml` feature must not copy
  that.)
- **Parser hooks**: extend the `[target]` loop (`manifest.cppm:1154-1171`, today only
  `toolchain`+`linkage`) to also read `dependencies`/`dev-dependencies`/
  `build-dependencies`/`build`. The TOML reader already lexes quoted keys
  (`toml.cppm:191-203`), so `[target.'cfg(windows)'.dependencies]` parses — but
  iterate `get_table("target")` entries manually (dotted `get_*` mis-splits quoted
  segments).
- **Precedence**: exact triple > cfg; multiple matching cfg tables → flags
  concatenate, conflicting scalar (e.g. linker) → error (Cargo's rule).

### Lazy + content-hash identity (from Zig)
- **`lazy = true`** on a dependency → fetched only when a build path (after cfg/feature
  gating) actually requests it (Zig `b.lazyDependency`). A Linux build then **never
  downloads** a `cfg(windows)` dependency. Generalizes mcpp's platform-specific compat
  packages directly.
- **Hash is identity; url is a mirror** (Zig `build.zig.zon`: *"packages come from a
  hash, not a url"*). Adopting this **root-causes two items in mcpp's history**: mirror
  auto-detection (GitHub/GitCode/CN are interchangeable *locations* of one hash) and
  the stale-index-shard "fetch false-failure" (content identity can't be impersonated
  by a moved shard path). Pair fetch with a content-addressed store.

---

## L2 — Static codegen
`generated_files` (`manifest.cppm:1975`) already covers codegen-as-data (synthesized
headers/sources, e.g. compat.zlib's config header, the openblas Windows anchor). No
change; it is the preferred mechanism whenever the generated content is static.

---

## L3 — `build.mcpp`: a native imperative build program (do it directly)

### Two distinct mechanisms, two audiences
- **`install()`** (Lua, in an xpkg recipe) — the **third-party package** build hook
  (builds a dependency from source; `compat.openblas`/`compat.xcb`). Stays.
- **`build.mcpp`** (C++, in the project) — the **mcpp-native project** build program.
  **New, built directly** (not deferred). Both get the same two disciplines below.

### Form — Zig's in-language model, in C++
A `build.mcpp` (e.g. `build/main.cpp`) is itself a tiny mcpp build compiled with the
**host** toolchain and run before the main build — Zig's `build.zig` model, but in the
project's own language (C++), so no second language and it dogfoods mcpp. Its
dependencies are exactly `[build-dependencies]` (L0 item 3 — now with a consumer).

### Discipline 1 — structured output protocol (not global mutation)
The program communicates only via stdout directives the engine consumes (Cargo's
`cargo::` model):
```
mcpp:link-lib=openblas
mcpp:link-search=<abs-dir>
mcpp:cxxflag=-DHAVE_FOO
mcpp:cfg=has_avx2
mcpp:generated=<path>          # a source/header to add to the graph
mcpp:rerun-if-changed=<path>
mcpp:rerun-if-env-changed=VAR
```
It *requests* graph edges; it never silently mutates build state.

### Discipline 2 — explicit declared inputs/outputs (fixes the `.mcpp_ok` blind spot)
`rerun-if-changed`/`rerun-if-env-changed` give the opaque program declared inputs, so
incremental builds stay correct. This is the documented fix for mcpp's `.mcpp_ok`
gap ("process exited 0 ≠ outputs correct"): replace the bare success marker with a
**declared-input/declared-output contract** — the program (or recipe) records what it
read and what it produced; the build re-runs iff a declared input changed and treats
missing declared outputs as failure.

### Constraints (à la carte + supply-chain)
- **Leaf only**: `build.mcpp` chooses flags/sources/codegen and emits link
  requirements — it must **not** gate the top-level dependency graph (that stays in
  L1 cfg tables, applicative). cfg gating of `build.mcpp` itself evaluates on the
  **target**, but it compiles/runs on the **host**.
- **Isolation**: treat its execution as a build *action* with declared inputs/outputs;
  run sandboxed; prefer platform-level isolation (SLSA) over trusting program code —
  the consensus is script-level sandboxing alone is insufficient. The existing xim
  Lua sandbox (no `os.curdir`/`files`/`trymkdir`) is the right instinct; extend the
  same declared-I/O contract to `install()`.

---

## Phasing (status)

- **✅ Phase 1 — L1 conditional build flags (mcpp 0.0.74).** `[target.'cfg(...)'.build]`
  `cflags`/`cxxflags`/`ldflags`, parsed deferred into `Manifest::conditionalConfigs`
  (`manifest.cppm`), evaluated against the resolved target by a recursive `cfg()`
  predicate evaluator (`prepare.cppm` `cfgpred::`) and merged into `buildConfig` right
  after `--target` resolution. Grammar: `all/any/not` over `os`/`arch`/`family`/`env`
  + bare `windows`/`unix`/`linux`/`macos`; native build → host coords, `--target` →
  target coords. Test: `tests/e2e/85_target_cfg_build_flags.sh`.
- **Phase 1b — L1 conditional dependencies + `lazy` fetch.** Same `[target.'cfg(...)']`
  namespace, `.dependencies`/`.dev-dependencies`/`.build-dependencies`; merge into
  `m->dependencies` in the same window (before dep resolution at `prepare.cppm:~731`).
  Add `lazy = true` (fetch only when a gated path requests it) + content-hash identity.
- **Phase 2 — L-1 environment.** Surface `[environment]` → extend the project
  `.xlings.json` writer (`config.cppm:699-705`) to emit `deps`/`workspace`/`envs`/`subos`;
  fold `[toolchain]` into `workspace`; wire `[build-dependencies]`.
- **Phase 3 — mcpp-index workspace** (companion doc) — first real consumer of Phase 1/1b.
- **Phase 4 — L3 `build.mcpp`** — native build program (structured output + declared-I/O);
  backport the declared-I/O contract to recipe `install()`.

## Appendix — cross-tool summary
- **Declarative-table vs imperative-script**: TOML is static data → declarative tables
  for the graph (Cargo/vcpkg/Bazel); reserve imperative for a separate file
  (Cargo `build.rs`, Zig `build.zig`). 
- **Conditional grammar**: de-facto token set across all six tools = os / arch /
  family / env; Cargo's `all/any/not` is the composable standard.
- **Host vs target**: Cargo/Zig/Bazel evaluate manifest conditionals on the **target**;
  build/tool deps run on the **host**. Conflating them breaks cross-compile (cf. mcpp's
  aarch64/musl bootstrap hazards — bootstrap tools must be host-static).
- **Resolution**: the field is converging on PubGrub (complete + explainable); consider
  it (or MVS / content-addressed identity) over ad-hoc backtracking long-term.

### Sources
Build Systems à la Carte (ICFP 2018); Cargo reference (specifying-dependencies,
build-scripts, config); Rust Reference (cfg); Zig build system + build.zig.zon +
cross-compilation docs; vcpkg manifest reference; Bazel configurable attributes;
Meson reference tables; Conan 2 requirements; PubGrub; Bazel hermeticity; SLSA.
