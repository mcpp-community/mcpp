# mcpp: GL Runtime Closure Plan

> 状态: active
> 分支: `codex/gl-runtime-closure-mcpp`
> PR: pending
> Last updated: 2026-06-03
> 目标: 让 mcpp 以标准工具链方式表达、解析、诊断并注入运行时闭包,使 GLFW/OpenGL 这类通过 `dlopen` 加载的运行库不再依赖用户手写环境变量。

## Scope

This repository owns the tool behavior. It should not hard-code one OpenGL
vendor or one package index workaround. The expected model is closer to
Conan/vcpkg run environments plus Nix-style runtime closure diagnostics:

- package metadata can declare runtime library directories, `dlopen` library
  names, and required system capabilities;
- dependency resolution carries runtime requirements separately from compile
  includes and link flags;
- `mcpp run`, `mcpp test`, `mcpp doctor`, and `mcpp pack` consume the same
  runtime model;
- missing system capabilities produce actionable errors before a user only sees
  a failed GUI window.

## Current Problem

- `mcpp run` builds the selected binary and executes it directly.
- Build/link propagation covers link-time shared libraries, but `dlopen`
  libraries such as `libGLX.so.0` and `libGL.so.1` do not appear in `DT_NEEDED`.
- `mcpp pack` already has runtime closure logic, but it is oriented around
  loader-visible ELF dependencies. GLX/EGL/Mesa/vendor-driver cases need an
  explicit runtime metadata path rather than guessing from one executable.

## Proposed Manifest Surface

Keep compile, link, and runtime requirements separate. Initial names can be
adjusted during implementation, but the semantics should remain stable:

```toml
[runtime]
library_dirs = ["relative/or/generated/runtime/lib"]
dlopen_libs = ["libGLX.so.0", "libGL.so.1", "libGL.so"]
capabilities = ["x11.display", "opengl.glx.driver"]
```

For package descriptors coming from an index, the same data should be accepted
from the package `mcpp` table:

```lua
mcpp = {
    runtime = {
        library_dirs = {"mcpp_generated/runtime/lib"},
        dlopen_libs = {"libGLX.so.0", "libGL.so.1", "libGL.so"},
        capabilities = {"x11.display", "opengl.glx.driver"},
    },
}
```

Compatibility rule: packages that do not declare runtime metadata keep current
behavior.

## Implementation Plan

- [x] Create this repository-level plan checkpoint.
- [x] Add manifest/runtime metadata parsing and validation.
  - Candidate files: `src/manifest.cppm`, manifest tests.
  - Invalid entries should fail early: empty library name, absolute path in
    package metadata unless explicitly allowed, duplicate capability strings.
- [x] Carry runtime requirements through the resolved package graph.
  - Candidate files: dependency resolution and `PackageRoot`/graph structures.
  - Runtime requirements must not be mixed into public include usage.
- [x] Teach `mcpp run` and `mcpp test` to build a run environment.
  - Candidate file: `src/cli.cppm`.
  - Done: `mcpp run` consumes resolved runtime library directories.
  - Done: `mcpp test` uses the same runtime environment for test binaries.
  - Linux: prepend resolved runtime directories to `LD_LIBRARY_PATH`.
  - macOS: use `DYLD_LIBRARY_PATH` only for local tool execution where allowed,
    otherwise prefer rpath/install-name behavior.
  - Windows: prepend resolved runtime directories to `PATH`.
- [ ] Add runtime diagnostics.
  - Candidate commands: `mcpp self doctor`, or a new target-aware runtime
    doctor path if the existing command shape supports it.
  - Diagnostics should list the target, the package that required the runtime
    item, unresolved `dlopen` names, and missing capabilities.
- [ ] Extend `mcpp pack` to consume runtime metadata.
  - Candidate file: `src/pack/pack.cppm`.
  - `pack` should include declared runtime directories/files when the mode
    requests a runnable bundle.
  - Keep system capabilities explicit; do not silently bundle host GPU drivers
    unless a package declares a redistributable runtime.
- [x] Add regression coverage with a small `dlopen` fixture.
  - Test should prove that a library loaded only via `dlopen` is found through
    mcpp runtime metadata during `mcpp run`.
  - A second pack-oriented test should prove runtime metadata is represented in
    the bundled executable environment.
- [ ] Update docs.
  - Candidate files: `docs/02-pack-and-release.md`,
    `docs/05-mcpp-toml.md`, README snippets if needed.

## Verification

- [x] `mcpp build`
- [x] `mcpp run -- --version`
- [x] `mcpp test`
- [ ] `MCPP=<built-mcpp> bash tests/e2e/run_all.sh`
- [x] Focused runtime metadata e2e for `dlopen` resolution
- [ ] Focused pack e2e for runtime metadata inclusion

## PR / CI / Merge Notes

- [x] Commit this plan as the first checkpoint.
- [ ] Open a PR with sanitized paths and no local machine details.
- [ ] Include a test plan in the PR body.
- [ ] Wait for Linux/macOS/Windows CI.
- [ ] Squash merge after required checks pass.

## Cross-Repository Dependencies

- `mcpp-index` can only fully validate `compat.glfw` GLX runtime metadata after
  this repository supports runtime requirements in `mcpp run`.
- `imgui-m` should not own tool runtime behavior; it only consumes the fixed
  behavior through its minimal window example.
- `xim-pkgindex` participates only after a released mcpp version is needed by
  xlings or users.
