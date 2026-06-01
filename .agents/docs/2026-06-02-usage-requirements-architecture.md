# Module-First Usage Requirements Architecture

## Context

The ImGui module experiment exposed two separate issues in mcpp:

1. C++ compile flags computed `include_flags` but did not place them in the
   final `cxxflags` string. That is a correctness bug in flag assembly.
2. The current dependency include model is too coarse for a module-first
   package ecosystem. `build.include_dirs` is used both to build a package and
   as the package's consumer-visible include surface.

For traditional header packages the symmetric behavior is convenient. For
module packages it is too broad: a package can include upstream headers
internally while exposing only `import package.module;` to consumers.

This document is the live target for the current PR. The architecture should
not be a thin wrapper around `BuildConfig.includeDirs`; it should make package
usage requirements explicit in the resolved package graph and move consumers
onto that graph incrementally inside this PR.

## Design Goals

- Keep existing package descriptors working during migration.
- Treat modules as the primary public surface for module packages.
- Distinguish package-private build requirements from consumer-visible usage
  requirements.
- Make scanner, P1689 scan, compile commands, and real compile rules consume
  the same resolved usage model.
- Keep link/runtime propagation separate from header/include propagation.
- Avoid forcing users of module packages to see or declare implementation
  headers.

## Terms

- Private build requirements:
  - Requirements needed to compile a package's own sources.
  - Example: `mcpplibs.imgui` wrappers need Dear ImGui backend headers
    internally.
- Public usage requirements:
  - Requirements that consumers inherit when depending on the package.
  - Example: `compat.glfw` exports `GLFW/glfw3.h` as a traditional header
    surface.
- Link requirements:
  - Libraries and linker flags that follow objects into final link units.
  - These are independent from whether headers are public.

## Target Data Model

The resolved build graph should own usage requirements as first-class data
rather than mutating manifests during dependency resolution:

```cpp
enum class Visibility {
    Private,
    Public,
    Interface,
};

struct UsageRequirements {
    std::vector<std::filesystem::path> includeDirs;
    std::vector<std::string> cflags;
    std::vector<std::string> cxxflags;
    std::vector<std::string> ldflags;
    std::vector<std::string> modules;
};

struct PackageNode {
    Manifest manifest;
    UsageRequirements privateBuild;
    UsageRequirements publicUsage;
    UsageRequirements linkUsage;
};

struct DependencyEdge {
    PackageId from;
    PackageId to;
    Visibility visibility;
};
```

The important property is that `Manifest` remains the parsed package
description. Resolved requirements live in the package graph and build plan,
not by appending dependency state back into the manifest.

## Dependency Visibility Semantics

Use CMake-like visibility:

- `private`
  - The dependency is needed to compile/link the package itself.
  - Its usage requirements are not propagated to consumers.
- `public`
  - The dependency is needed to compile/link the package itself.
  - Its public usage requirements are propagated to consumers.
- `interface`
  - The package itself does not compile/link against the dependency.
  - Consumers inherit the dependency's public usage requirements.

Future manifest surface:

```toml
[dependencies.compat]
imgui = { version = "1.92.8", visibility = "private" }
glfw = { version = "3.4", visibility = "private" }
```

Compatibility rule: old dependency declarations default to `public`, matching
current mcpp behavior.

## Build Graph Behavior

Resolution should build a package graph:

1. Parse manifests without rewriting them.
2. Create `PackageNode` entries for root and dependencies.
3. Create `DependencyEdge` entries with resolved visibility.
4. Compute each node's effective private build requirements from:
   - the package's own build requirements,
   - private/public dependency requirements,
   - platform/toolchain requirements.
5. Compute each node's public usage requirements from:
   - the package's declared public/interface requirements,
   - public/interface dependency requirements.
6. Compute link closure independently from include/header closure.

Scanner, P1689 scan, compile commands, and Ninja compile rules should all derive
from each compile unit's resolved private build requirements. A module import
edge should require BMI/object/link availability, not implementation header
visibility.

## ImGui Implications

`imgui-private` should model upstream headers as private implementation
requirements:

- `compat.imgui`: private build/link requirement for wrappers and backend
  implementation files.
- `compat.glfw`: private build/link requirement for GLFW backend modules.
- `compat.opengl`: private build requirement for OpenGL backend headers.

Consumers of the module package should only need:

```cpp
import imgui.core;
import imgui.backend.glfw_opengl3;
```

Consumers that want to write direct GLFW/OpenGL header code should declare
`compat.glfw` or `compat.opengl` themselves.

## Current PR Plan

1. Parse dependency visibility:
   - Add `visibility = "private" | "public" | "interface"` to dependency
     specs.
   - Default omitted visibility to `public`, preserving existing packages.
2. Introduce resolved usage graph data:
   - Add `UsageRequirements` and dependency edge visibility to the package
     graph surface currently represented by `PackageRoot`.
   - Keep parsed `Manifest` immutable during dependency resolution for include
     propagation.
3. Compute usage requirements during dependency resolution:
   - Map a package's legacy `build.include_dirs` to its own private build
     requirements and public usage requirements.
   - For `private` and `public` edges, make dependency public usage visible to
     the dependent package's private build.
   - For `public` and `interface` edges, propagate dependency public usage to
     the dependent package's public usage.
4. Make build consumers use resolved usage:
   - Regex scanner, P1689 scanner, compile commands, and Ninja compile rules
     should read the same per-package resolved private build include dirs.
   - Keep link flag propagation in the existing path for this PR; link usage is
     a separate axis in the model and should be migrated after include usage is
     proven by tests.
   - Keep `build.cflags` and `build.cxxflags` package-private in this PR;
     public compile definitions/options need a dedicated manifest surface
     before they should propagate to consumers.
5. Preserve the already discovered fixes:
   - Keep the C++ `include_flags` assembly regression fix.
   - Keep stale unmarked xpkg install recovery.
6. Release path:
   - Open PR, wait for CI, and admin squash merge after approval.
   - Do not trigger the `0.0.43` release from this PR.
   - Defer version bump / release / package-index / xlings-res rollout until
     the next requested scheme is implemented and released together.

## Current PR Scope

Implement:

- The C++ `include_flags` assembly fix.
- Focused tests for the C++ include flag behavior.
- The stale unmarked xpkg install recovery already discovered during ImGui
  validation.
- Dependency visibility parsing and validation.
- Resolved include usage requirements for `PackageRoot`.
- Dependency-edge include propagation without mutating parsed manifests.
- Scanner/build-plan consumption of resolved include usage.
- PR CI and admin squash merge.

Leave for follow-up after this PR:

- Full `linkUsage` migration away from `Manifest::buildConfig.ldflags`
  mutation.
- `0.0.43` version bump and release trigger.
- package-index / xlings-res rollout for the released mcpp version.
- Generated module wrapper surface expansion for ImGui.
- mcpp-index publication of ImGui after the module package has a public source
  repository and the required mcpp release is available.

## Live Status

- Done: dependency specs carry a `visibility` field with parser validation for
  TOML descriptors.
- Done: C++ `include_flags` assembly fix and unit coverage.
- Done: stale unmarked xpkg cache recovery path and e2e coverage.
- Done: resolved include usage graph and scanner/build-plan consumers.
- Done: targeted unit/e2e coverage for dependency visibility propagation.
- Done: compatibility checks for existing public transitive include behavior.
- Done: local verification with `mcpp test` and full e2e suite.
- Pending: PR, CI, and admin squash merge.
- Deferred by user decision: `0.0.43` version bump, release trigger,
  xim-pkgindex update, and xlings-res rollout.

## Local Verification

- `mcpp build`: passed.
- `mcpp test`: passed (`18 passed; 0 failed`).
- `tests/e2e/run_all.sh`: passed (`65 passed, 0 failed, 0 skipped`).
- Targeted checks:
  - `tests/e2e/31_transitive_deps.sh`: passed.
  - `tests/e2e/50_package_owned_build_flags.sh`: passed.
  - `tests/e2e/60_stale_xpkg_cache_reinstall.sh`: passed.
  - `tests/e2e/61_dependency_visibility.sh`: passed.
