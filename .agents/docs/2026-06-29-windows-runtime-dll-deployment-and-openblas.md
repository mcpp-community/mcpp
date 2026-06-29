# Windows Runtime-DLL Deployment & `compat.openblas` Windows Support (Design)

Date: 2026-06-29
Status: **Phase A implemented in mcpp 0.0.73.** Phases B–D (release + recipe +
Windows CI) track in the same effort. Staged follow-up to the feature/capability
work (mcpp 0.0.72) and the `compat.openblas` package (mcpp-index #54). Scope:
`src/build/{plan,ninja_backend}.cppm` (mcpp); `pkgs/c/compat.openblas.lua`,
`.github/workflows/validate.yml` (mcpp-index).

### Implementation note (deviation from the original design)

The implemented mechanism is **gated by the `*.dll` file extension, not by
`if constexpr(is_windows)` and not by a schema change.** During build planning,
each `*.dll` found in a linked dependency's `[runtime] library_dirs` is staged
into `bin/` beside the produced executable via a ninja `cp_bmi` copy edge that
the executable target takes as an implicit dependency. Consequences:

- **No `manifest.cppm` schema change** (the original Phase-A-step-1). A recipe
  declares `[runtime] library_dirs` *globally*; on Linux/macOS the dependency
  ships `.so`/`.dylib` (never `.dll`), so the glob matches nothing and the build
  is byte-for-byte unchanged. This is exactly the §7 "declare it globally —
  harmless on Linux" option, made safe by the extension filter. Per-OS scoping
  under `mcpp.<os>` is therefore unnecessary (and already supported for free by
  the existing per-OS textual merge if a recipe ever wants it).
- **The deploy path is exercised on a Linux host** (test
  `tests/e2e/84_runtime_dll_deploy.sh`) by a dummy dependency shipping a stub
  `libdummy.dll` — the same code that runs on Windows, validated without a
  Windows runner. The Windows link/run half is Phase D (mcpp-index CI).
- **`mcpp pack`** needs no change here: Windows PE packaging is separately
  stubbed (`src/pack/pack.cppm`, see `2026-05-19-pack-windows-design.md`); when
  implemented it will pick up the staged `bin/*.dll`. On Linux `mcpp pack` uses
  `ldd`, which never sees a `.dll`, so the deploy is invisible to it.

## 1. Problem

`compat.openblas` is a first-class BLAS **library**, not merely an Eigen backend:
a consumer can depend on it directly, `#include <cblas.h>`, and call
`cblas_dgemm` (proven — a standalone consumer links and runs, producing the
correct `[19 22; 43 50]`). It therefore deserves cross-platform support.

It currently ships **linux/macosx only**. The two platforms build OpenBLAS from
source through its GNU Make system, driven by the xpkg `install()` hook
(build-dep `xim:make`, `CC=gcc`), producing a fully static `libopenblas.a` that
links with no runtime artifact. Windows has no equivalent path:

- mcpp links Windows with **MSVC-ABI Clang** (msvcrt / msvc-stl / MSVC C++ ABI).
- OpenBLAS's prebuilt `OpenBLAS-<ver>-x64.zip` contains a **mingw** static
  `libopenblas.a` (Itanium ABI + libgcc/mingw-libc) that does not link cleanly
  into an MSVC-ABI binary, plus an MSVC import library `libopenblas.lib` and a
  runtime `libopenblas.dll`. The import-lib path links, but then the produced
  executable needs `libopenblas.dll` present at launch.
- Building OpenBLAS statically with clang-cl/CMake inside the xim sandbox is not
  available (no MSVC toolchain there) and would be a heavy CMake build.

So the only viable Windows route is **import-lib + runtime DLL**, and that is
blocked by a genuine mcpp capability gap: **mcpp does not deploy a dependency's
runtime DLL alongside the produced executable.** A Windows `compat.openblas`
would link but fail to launch (missing DLL).

## 2. Background — mcpp's existing runtime model

mcpp already models "what a built binary needs at launch" via the `[runtime]`
section (`manifest::RuntimeConfig`):

- `library_dirs` — directories (relative to the package root) holding the
  package's runtime shared libraries. These flow into
  `BuildPlan::depRuntimeLibraryDirs`.
- `dlopen_libs`, `capabilities`, `provides`, provider overrides.

On platforms where `mcpp::platform::supports_rpath` is true (ELF / Mach-O —
Linux, macOS), `src/build/flags.cppm` turns each runtime library dir into
`-Wl,-rpath,<dir>` so the loader finds the `.so`/`.dylib` at runtime. `mcpp run`
additionally exports the platform runtime path variable
(`LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH` / `PATH`) from the same list.

On Windows (PE), `supports_rpath` is **false**. There is no general RPATH
mechanism, so `depRuntimeLibraryDirs` is currently a no-op at link time: a
directly-launched `.exe` cannot find a dependency's DLL.

## 3. Design decision

**Reuse `[runtime].library_dirs`; add a Windows deployment backend. No new schema
key, no top-level per-OS block.**

The recipe schema already carries platform structure at three layers, each with a
distinct responsibility:

| Layer | Responsibility | Platform split |
|---|---|---|
| `xpm.<os>` | download source (url / sha256) | `xpm.{linux,macosx,windows}` |
| `mcpp.<os>` | compile/link keys (`cflags`/`sources`/`ldflags`) | `mcpp.{linux,macosx,windows}` (cf. compat.glfw) |
| `[runtime]` | launch-time requirements (`library_dirs`, `dlopen_libs`, …) | global today |

A dependency's runtime DLL is a **launch-time requirement**, so it belongs to
`[runtime].library_dirs` (pointing at the directory that holds the DLL, e.g.
`bin/`). The abstraction *already exists*; Windows is simply the platform on
which its deployment backend is unimplemented.

The implementation mirrors the existing RPATH branch. Where `flags.cppm` today
does, in effect:

```
if constexpr (supports_rpath)        // ELF / Mach-O
    for dir in depRuntimeLibraryDirs: ldflags += "-Wl,-rpath," + dir
```

it gains the symmetric PE branch:

```
else if constexpr (is_windows)       // PE
    for dir in depRuntimeLibraryDirs: deploy *.dll from dir  →  <output>/bin/
```

The same declaration ("this dependency's runtime libraries live in dir X") is
dispatched, at compile time, to the platform-appropriate mechanism: RPATH on
ELF/Mach-O, copy-beside-executable on PE. Rejected alternatives:

- **A top-level `windows = {}` recipe block** — redundant; `mcpp.<os>` and
  `[runtime]` already provide the platform and runtime axes.
- **A new `runtime_libs` key** — redundant with `[runtime].library_dirs`; adds a
  second way to say the same thing.

## 4. Architecture evaluation

| Dimension | Rating | Rationale |
|---|---|---|
| Soundness | High | Completes an abstraction already designed but unimplemented on PE; RPATH and copy are both "make the runtime library locatable" — semantically unified. |
| Compatibility | High | Purely additive, dispatched by `if constexpr`; non-Windows behavior is byte-for-byte unchanged; no new schema key, so existing recipes are untouched. |
| Generality | High | Benefits every Windows compat library that ships a DLL (future fftw / hdf5 / any prebuilt-DLL package), not an OpenBLAS-specific patch. |
| Complexity | Low–Medium | One ninja `copy` edge plus exposing the DLLs of `depRuntimeLibraryDirs` to the backend. No new concept, no new key. ninja handles incrementality/timestamps. |
| Elegance | High | Symmetric with the RPATH path; reuses `[runtime]`; zero new abstraction. |
| Stability | High (deploy side) | The link side is unchanged (still import-lib); the addition is an order-only file copy at the end of the build — small failure surface. |

The residual risk is **not** in this feature's deploy mechanism but in the
Windows-specific link/run that only a Windows host exercises (see §7).

## 5. Implementation phases

### Phase A — mcpp: Windows runtime-DLL deployment (the core feature)

1. **Schema** (`manifest.cppm`): allow `[runtime].library_dirs` to be declared
   per-OS under `mcpp.<os>` (today `mcpp.<os>` parses `cflags`/`sources`/`ldflags`;
   extend it to also read a runtime library-dir list), so a recipe can scope the
   directory to Windows. No new key is introduced.
2. **Propagation** (`prepare.cppm` / `plan.cppm`): the runtime library dirs of a
   dependency in an executable's link closure already reach
   `BuildPlan::depRuntimeLibraryDirs`; ensure they are resolved to absolute
   source paths (reuse the `-L<rel>` → `<depRoot>/<rel>` normalization) and
   de-duplicated.
3. **Backend** (`flags.cppm` + `ninja_backend.cppm`): add the PE branch — for
   each runtime library dir of a linked dependency, emit
   `build <output>/bin/<dll> : copy <abs-src>` and make the executable target
   take an order-only dependency on it. Guard by `is_windows` so other platforms
   are unaffected.
4. **Packaging** (`mcpp pack`): include the deployed `bin/*.dll` so a packaged
   artifact is self-contained off the build machine.
5. **Local test** (achievable without Windows): an e2e on Linux using a *dummy*
   shared library — a dependency declares a runtime library dir; assert
   `mcpp build` deploys the file beside the executable. This validates the deploy
   mechanism mechanically. The Windows-specific link/run is verified in Phase D.

### Phase B — release mcpp 0.0.73

The feature must ship in a released mcpp before the recipe can rely on it (an
older mcpp would link the import lib but never deploy the DLL → runtime failure).
Run the established pipeline: version bump (`mcpp.toml` + `fingerprint.cppm`),
design-doc + tests, PR → CI (5 suites) → squash-merge → `release.yml` (4
platforms) → mirror `xlings-res/mcpp` (GitHub + GitCode) → bump
`xim-pkgindex/pkgs/m/mcpp.lua` → `xlings install mcpp@0.0.73` verification → bump
the workspace bootstrap pin.

### Phase C — `compat.openblas` Windows recipe

1. `xpm.windows`: `url` → `OpenBLAS-0.3.33-x64.zip` (GLOBAL = OpenMathLib GitHub;
   CN = gitcode `mcpp-res/openblas`); `sha256`. The zip unpacks to
   `bin/ lib/ include/` (no wrapper directory).
2. `mcpp.windows`:
   - `include_dirs` resolves the shipped `include/` (carries `cblas.h`).
   - `ldflags` link the import library `libopenblas.lib` (clang-cl form to be
     finalized via CI).
   - `[runtime] library_dirs = ["bin"]` (the existing key) so Phase A deploys
     `libopenblas.dll` beside the consumer's executable.
   - anchor strategy: Windows has no build step, so the anchor TU is supplied via
     `generated_files` (mcpp writes it; `install()` does not run). Linux/macOS keep
     the "`install()` produces the anchor → triggers the Make build" trigger.
3. `install()` branches on `os.host()`: Windows returns immediately (prebuilt — no
   Make); linux/macosx run the existing source build.
4. CN mirror: upload `OpenBLAS-0.3.33-x64.zip` (~40 MB) to the gitcode
   `mcpp-res/openblas` 0.3.33 release; verify byte-equality via a GET download.

The linux/windows asymmetry is intentional and semantically natural: Linux/macOS
link a fully static `libopenblas.a` (no runtime artifact, so no runtime dir is
declared); Windows uses the prebuilt import-lib + DLL (MSVC-ABI Clang has no
static option from the upstream zip), so only Windows declares a runtime library
directory.

### Phase D — verification via CI (the only Windows-host validation)

The Windows runtime half (clang-cl linking `libopenblas.lib`, the DLL loading at
launch, `cblas_dgemm` producing the correct result) can be verified **only** on a
real Windows runner. The current mcpp-index CI does **not** cover it as-is:

- `smoke-examples` is **ubuntu-only** and does build **and** run
  (`tests/run_example.sh` = `mcpp build` + `mcpp run`), but never on Windows.
- `smoke-windows` runs on `windows-latest` but is **build-only**
  (`smoke_compat_*.sh` invoke `mcpp build`, not `mcpp run`) and is **pinned to
  mcpp 0.0.68**.

So Phase D must, in `validate.yml`:

1. Bump the Windows smoke job's mcpp pin to ≥ 0.0.73 (the release carrying the
   deployment feature).
2. Add a Windows **build-and-run** step for an OpenBLAS example that asserts the
   computed result — only `mcpp run` exercises DLL loading; a build-only check
   would pass even if the DLL were never deployed.

Iterate against the runner logs until green (no interactive debugging is possible
on the runner; correction is log-driven).

## 6. Verification boundary

| Item | Local (Linux host) | CI (`windows-latest`) |
|---|---|---|
| recipe parse / schema / Linux deploy mechanism | yes (dummy-lib e2e) | — |
| clang-cl linking `libopenblas.lib` | no (no MSVC env) | yes |
| DLL deployed beside the exe + loaded + `cblas_dgemm` runs | no | yes — once the Windows smoke is bumped to ≥ 0.0.73 and extended to run+assert (Phase D) |

## 7. Open questions / risks

- **Whole-directory `*.dll` vs an explicit list.** OpenBLAS's `bin/` holds a
  single DLL, so copying `*.dll` from each runtime library dir is sufficient and
  simple (preferred — YAGNI). If a future package's `bin/` mixes unrelated DLLs,
  an optional explicit list can narrow it later.
- **clang-cl link form and symbol decoration.** The exact flag to link
  `libopenblas.lib` and the cdecl `cblas_*` symbol names must match what the
  consumer references; only CI confirms this.
- **`mcpp.<os>` runtime parsing.** If the parser reads `[runtime]` only at global
  scope, allowing `library_dirs` under `mcpp.windows` is a small parser addition
  (alternatively declare it globally — harmless on Linux, which links statically
  and ships no DLL).
