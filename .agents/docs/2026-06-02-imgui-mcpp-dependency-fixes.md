# 2026-06-02 imgui mcpp dependency fixes

## Scope

Branch: `codex/imgui-mcpp-dependency-fixes`

This document tracks the mcpp-side changes discovered while validating the
`imgui-private` package and its `compat.imgui` / `compat.glfw` /
`compat.opengl` dependency chain. The imgui package work stays outside this
branch; this branch is only for mcpp fixes.

## Root Cause Chain

The imgui validation exposed two separate mcpp-side problems.

1. C++ build flags dropped package include dirs.

   `src/build/flags.cppm` built `CompileFlags::cxx` with fewer `{}` format
   placeholders than arguments. The final `include_flags` argument was not
   emitted into the C++ baseline flags, while `CompileFlags::cc` did include it.

   Impact: module scanning and C++ compilation for the current package could
   miss include dirs propagated from direct dependencies. In imgui, this showed
   up as scanner failures like `imgui.h` or backend `.cpp` files not found.

   Why gtest did not prove this path: gtest's existing success path did not
   exercise the same current-package C++ baseline include propagation shape. It
   could compile through its own package metadata while imgui needed direct
   dependency include dirs during current-package module/backend scans.

2. Version dependency lookup reused stale unmarked xpkg directories.

   `loadVersionDep()` accepted an installed path if `install_path()` found a
   directory. That bypassed the stricter `.mcpp_ok` install marker semantics
   already used in `resolve_xpkg_path()`.

   Impact: an old `compat.glfw@3.4` sandbox directory with layout
   `3.4/glfw-3.4/include` and `3.4/glfw-3.4/src` was reused even though the
   current index metadata expects `include` and `src` at the version root after
   the install hook. mcpp then skipped GLFW's own source files, linked only
   imgui and GLFW consumers, and failed with undefined `glfwInit`,
   `glfwCreateWindow`, etc.

## Changes In This Branch

- `src/build/flags.cppm`
  - Fixed the `std::format` placeholder count for `CompileFlags::cxx`, so
    `include_flags` is emitted for C++ scan/compile rules.

- `tests/unit/test_ninja_backend.cpp`
  - Added `NinjaBackend.CxxFlagsIncludeBuildIncludeDirs` to lock the C++ include
    propagation behavior.

- `src/cli.cppm`
  - In version dependency loading, require either `.mcpp_ok` or a layout that
    matches the current index entry's `mcpp` metadata before reusing an existing
    xpkg directory.
  - Adopt and mark compatible pre-marker installs; clean stale unmarked
    directories whose layout no longer matches the index before reinstall.
  - Mark the install path complete after a successful version dependency
    install.

- `tests/e2e/60_stale_xpkg_cache_reinstall.sh`
  - Added a stale-cache regression that creates an old unmarked xpkg layout and
    verifies mcpp reinstalls it instead of linking against the stale contents.

## Evidence So Far

- Red test for the include bug:
  - `NinjaBackend.CxxFlagsIncludeBuildIncludeDirs` failed before the flags fix
    because `flags.cxx` was `-std=c++23 -fmodules -O2` without `-I...`.

- Green unit suite after the flags fix and current resolver change:
  - Command: `mcpp test -- --gtest_filter=NinjaBackend.CxxFlagsIncludeBuildIncludeDirs`
  - Result: full mcpp test run completed with `18 passed; 0 failed`.

- imgui-private validation after clearing stale `compat.glfw@3.4` cache:
  - `backend_test ... ok`
  - `imgui_test ... ok`
  - `test result ok. 2 passed; 0 failed`

- Red e2e for stale-cache behavior before the resolver fix:
  - `tests/e2e/60_stale_xpkg_cache_reinstall.sh` failed with
    `undefined reference to stale_value`, proving the stale dependency sources
    were not linked.

- Green e2e after rebuilding the mcpp CLI with the resolver fix:
  - Command:
    `MCPP=target/x86_64-linux-gnu/85da010ca4e7d6e2/bin/mcpp bash tests/e2e/60_stale_xpkg_cache_reinstall.sh`
  - Result: `OK`

- Boundary regression caught and fixed:
  - `tests/e2e/52_local_path_namespaced_index.sh` initially failed after a
    marker-only implementation because it intentionally pre-populates a
    compatible project-local xpkg layout without `.mcpp_ok`.
  - The resolver now adopts an unmarked directory only when the current index's
    `mcpp` metadata can resolve the package source/manifest from that root.

- Latest focused verification with rebuilt CLI:
  - `mcpp test`: `18 passed; 0 failed`
  - `tests/e2e/52_local_path_namespaced_index.sh`: `OK`
  - `tests/e2e/54_package_owned_ldflags.sh`: `OK`
  - `tests/e2e/55_dependency_shared_artifact.sh`: `OK`
  - `tests/e2e/56_transitive_shared_artifact.sh`: `OK`
  - `tests/e2e/57_static_dep_shared_artifact.sh`: `OK`
  - `tests/e2e/60_stale_xpkg_cache_reinstall.sh`: `OK`

## Pending Verification

- Re-run imgui-private with the rebuilt mcpp binary without manually deleting
  package directories.
- Before opening a PR, decide whether to also run the full e2e suite or leave
  that to CI after the focused dependency set above.

## Notes

- The imgui package itself is currently not the blocker after a fresh GLFW
  install. The observed backend tests pass once the dependency cache layout is
  corrected.
- The mcpp fixes should be submitted as a separate PR from imgui package/index
  updates.
