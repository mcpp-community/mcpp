# Dotted Dependency Selector Architecture

Date: 2026-06-02
Branch: `codex/dotted-dependency-analysis`
Status: locally verified on feature branch; PR/CI/release still pending.

## Scope

This document evaluates how `mcpp` should support dotted dependency selectors
such as:

```toml
[dependencies]
cmdline = "0.0.1"
capi.lua = "0.0.3"
compat.gtest = "1.15.2"
imgui.core = "0.0.1"
imgui.backend.glfw_opengl3 = "0.0.1"
```

The goal is not only to make one manifest example pass. The goal is a coherent
dependency model for a module-oriented package manager while preserving current
manifests, xpkg compatibility, workspace behavior, and CLI ergonomics.

Local verification environment is a separate concern. If the local shell picks
an old `mcpp`, switch it explicitly through xlings:

```bash
xlings use mcpp 0.0.42
```

That fixes local tool selection. It does not define the dependency selector
architecture.

## Requirements

1. A single `[dependencies]` table can express both package namespaces and
   multi-level package/module names.
2. `mcpplibs` may be omitted by users as a priority alias, but it is not the
   only root namespace.
3. Explicit namespaces such as `compat` still work.
4. Multi-level names such as `imgui.backend.glfw_opengl3` are first-class.
5. Root dependencies, dev/build dependencies, workspace dependencies, xpkg
   `mcpp.deps`, and CLI add/remove must share the same selector rules.
6. Existing manifests and quoted legacy dotted keys remain accepted.
7. The resolver must be deterministic in candidate ordering. Package lookup may
   then select the first available candidate from local/index metadata.

## Current Code Findings

The current implementation is close in some places but is not yet one
architecture:

- `src/libs/toml.cppm` already parses unquoted TOML dotted keys into nested
  tables. This is why `[dependencies] compat.gtest = "..."` can be distinguished
  from a quoted flat key.
- `src/libs/toml.cppm` also tracks explicit tables. That distinction matters:
  `[dependencies.imgui] core = "..."` is not the same user intent as
  `[dependencies] imgui.core = "..."`.
- `src/manifest.cppm` has namespace-aware logic for part of root dependency
  parsing, but other dependency surfaces still split strings differently.
- Workspace dependency parsing and synthesized xpkg `mcpp.deps` parsing do not
  currently share the same selector model.
- `src/pm/compat.cppm` already owns package lookup compatibility and index/file
  naming fallback logic. It is a reasonable temporary home for compatibility
  helpers, but canonical selector parsing should be conceptually separate from
  legacy fallback behavior.
- `src/pm/package_fetcher.cppm` and `src/pm/resolver.cppm` already consume a
  structured package namespace plus package name. The missing piece is upstream
  normalization before dependencies reach those layers.
- `src/pm/commands.cppm` still needs the same user selector rules for add/remove
  so CLI behavior matches manifest behavior.

## Design Decision

Use one canonical dependency selector resolver at manifest/command ingestion
time. The resolver should produce ordered package coordinate candidates:

```text
candidate(namespace + shortName)[] + stableMapKey
```

The important rule is that `mcpplibs` is an optional prefix with priority, not a
forced root. `imgui`, `compat`, `mcpplibs`, and custom index namespaces are
peer roots. For selector `a.b`, lookup should first try the omitted-mcpplibs
candidate, then fall back to the literal peer-root candidate.

This keeps the interpretation explicit and ordered. The resolver itself should
not perform network work; package lookup can resolve the ordered candidates
against already available index metadata.

## Selector Semantics

Omitted `mcpplibs` priority:

| selector | candidate priority | stable map key |
| --- | --- | --- |
| `cmdline` | `mcpplibs/cmdline` | `cmdline` |
| `capi.lua` | `mcpplibs.capi/lua`, then `capi/lua` | `capi.lua` |
| `imgui.core` | `mcpplibs.imgui/core`, then `imgui/core` | `imgui.core` |
| `imgui.backend.glfw_opengl3` | `mcpplibs.imgui.backend/glfw_opengl3`, then `imgui.backend/glfw_opengl3` | `imgui.backend.glfw_opengl3` |

Fully explicit prefix and peer-root fallback:

| selector | candidate priority | stable map key |
| --- | --- | --- |
| `mcpplibs.capi.lua` | `mcpplibs.capi/lua` | `mcpplibs.capi.lua` |
| `compat.gtest` | `mcpplibs.compat/gtest`, then `compat/gtest` | `compat.gtest` |

Explicit subtable:

```toml
[dependencies.compat]
gtest = "1.15.2"
```

This remains a direct explicit namespace form and should resolve to
`compat/gtest`, not to an omitted-mcpplibs candidate. Explicit subtables are the
clearest form for non-default or custom index namespaces when ambiguity matters.

## Namespace Roots

Peer namespace roots include:

- `mcpplibs`
- `compat`
- `imgui` and future module family roots if they exist as package indexes
- explicit names declared in `[indices]`
- explicit dependency table roots such as `[dependencies.acme]`

Unquoted dotted selectors in the single `[dependencies]` table do not create a
new namespace unconditionally. They create an ordered lookup:

```text
a.b.c -> mcpplibs.a.b/c, then a.b/c
```

This is the key rule: `mcpplibs` has priority because it may be omitted, but
`a.b` remains a valid peer namespace fallback.

For custom index names, the deterministic rule should be:

- If an explicit subtable is used, treat the subtable name as the root.
- If a single-table dotted selector is used, try the omitted-mcpplibs candidate
  first, then the literal peer-root candidate.
- If users need to bypass priority matching, they can use an explicit subtable
  such as `[dependencies.compat]` or a fully explicit selector such as
  `mcpplibs.<name>...`.

This avoids unordered probing while still giving users an escape hatch.

## Resolver Shape

Recommended API shape:

```cpp
struct DependencySelectorContext {
    std::unordered_set<std::string> explicitNamespaceRoots;
    std::string defaultNamespace = "mcpplibs";
};

struct DependencySelector {
    std::vector<PackageCoordinate> candidates;
    std::string stableMapKey;
    bool explicitRoot = false;
};

DependencySelector resolve_dependency_selector(
    std::span<const std::string> segments,
    const DependencySelectorContext& context);
```

The important design point is not the exact C++ names. The important point is
that all dependency entry points call one resolver instead of each splitting on
`.` independently.

## Ingestion Points

Apply the resolver at the edges:

- `src/manifest.cppm`: root dependencies, dev-dependencies, build-dependencies.
- `src/manifest.cppm`: workspace dependencies.
- `src/manifest.cppm`: synthesized dependencies from Lua xpkg `mcpp.deps`.
- `src/pm/commands.cppm`: `mcpp add`, `mcpp remove`, and related CLI parsing.
- Documentation examples in `docs/05-mcpp-toml.md`.

After candidate generation, package fetch/build/resolution should select the
first available structured package coordinate. That keeps user-facing selector
syntax out of lower build layers while preserving the requested priority
matching behavior.

## Compatibility Policy

Keep all existing supported forms:

- `[dependencies] cmdline = "..."`
- `[dependencies.mcpplibs] cmdline = "..."`
- `[dependencies.compat] gtest = "..."`
- quoted `"mcpplibs.cmdline" = "..."`
- quoted legacy dotted keys where currently accepted
- path/git inline specs
- visibility/use requirements from the existing dependency model

For one-segment omitted-mcpplibs dependencies, keep the stable map key as the
old bare key (`cmdline`) to avoid unnecessary lockfile or update churn. For
multi-segment selectors, preserving user spelling such as `imgui.core` is more
appropriate than rewriting everything to `mcpplibs.imgui.core`, because the
selector is an ordered match rather than a forced namespace rewrite.

Quoted flat dotted keys should remain compatibility input, not the primary new
syntax. The canonical user-facing syntax should be unquoted TOML dotted keys or
explicit dependency subtables.

## Alternatives Considered

### A. Canonical Selector Resolver

Recommended. It gives one deterministic rule set and keeps compatibility logic
at the ingestion boundary.

### B. Patch Each Parser Site Independently

Rejected. It may fix the first failing test but preserves divergent behavior
between root deps, workspace deps, xpkg deps, and CLI commands.

### C. Unordered Index-Probing Fallback

Rejected. Arbitrary probing that changes priority based on current index
contents is fragile. Ordered candidate lookup is different: the selector has a
fixed priority list, and package resolution selects the first candidate that is
available.

## Verification Plan

Unit tests:

- Selector matrix in the PM layer.
- Root dependency dotted selectors.
- Dev/build dependency dotted selectors.
- Workspace dependency dotted selectors.
- Synthesized xpkg `mcpp.deps` dotted selectors.
- CLI add/remove selector normalization.
- Quoted legacy dotted key compatibility.
- Explicit custom index namespace behavior.

End-to-end tests:

- A local-index package using `compat.gtest`.
- A local-index package using `imgui.core`.
- A local-index package using `imgui.backend.glfw_opengl3`.

Before running local tests, pin the shell to the intended mcpp version:

```bash
xlings use mcpp 0.0.42
mcpp test
```

## Implementation Progress

- 2026-06-02: Added `mcpp.pm.dependency_selector` with ordered candidates.
- 2026-06-02: Extended `DependencySpec` with ordered candidate coordinates.
- 2026-06-02: Updated root dependencies, dev/build dependencies, workspace
  dependencies, and xpkg `mcpp.deps` synthesis to preserve dotted selector
  spelling while recording candidate priority.
- 2026-06-02: Updated dependency resolution to select the first candidate whose
  strict canonical xpkg.lua entry exists. This avoids legacy fallback lookup
  accidentally treating `compat.gtest` as `mcpplibs.compat/gtest`.
- 2026-06-02: Added unit coverage for selector candidates and manifest parsing.
- 2026-06-02: Added e2e coverage for `imgui.core` falling back from
  `mcpplibs.imgui/core` to peer-root `imgui/core`.
- 2026-06-02: Updated `mcpp add` so dotted input stays in the single
  `[dependencies]` table; `ns:name` remains the explicit subtable syntax.
- 2026-06-02: Full unit tests and targeted local-index/preinstall e2e checks
  pass locally.
- 2026-06-02: Bumped the pending release version to `0.0.43` and added the
  changelog entry for dotted dependency selectors.
- 2026-06-02: CI exposed that `--version` also uses the hardcoded
  `MCPP_VERSION` in `src/toolchain/fingerprint.cppm`; synchronized it to
  `0.0.43`.

Current local checks:

```bash
xlings use mcpp 0.0.42
mcpp test -- --gtest_filter='DependencySelector.*:Manifest.DependenciesDottedSelectorPreservesUserKeyAndCandidates:Manifest.DependenciesNamespacedSubtableNestedDottedKeyIsCanonical:SynthesizeFromXpkgLua.DepsDottedSelectorsUseManifestRules:Manifest.WorkspaceDependenciesUseDottedSelectorRules'
mcpp build
MCPP=target/.../bin/mcpp bash tests/e2e/62_dotted_dependency_selector_priority.sh
MCPP=target/.../bin/mcpp bash tests/e2e/12_add_command.sh
MCPP=target/.../bin/mcpp bash tests/e2e/52_local_path_namespaced_index.sh
MCPP=target/.../bin/mcpp bash tests/e2e/58_preinstall_mcpp_deps_for_hooks.sh
```

## Resolved Decisions

1. Declared custom `[indices]` roots do not turn single-table dotted selectors
   into explicit roots. Single-table selectors still use omitted-mcpplibs
   priority: try `mcpplibs.<path>` first, then `<peer-root>.<path>`.
2. The selector resolver lives in `src/pm/dependency_selector.cppm`.
   `compat.cppm` remains focused on legacy dotted-key and xpkg filename
   compatibility.
3. CLI add/remove preserve user spelling for dotted selectors. Explicit
   namespace subtables remain available through `ns:name`.
