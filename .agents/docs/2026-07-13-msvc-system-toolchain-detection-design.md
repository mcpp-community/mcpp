# MSVC System-Toolchain Detection — Design

Date: 2026-07-13
Status: approved for implementation (target release: 0.0.88)

## 1. Problem & scope

mcpp on Windows today builds exclusively with MSVC-ABI Clang (`llvm@20.1.7`),
which *silently borrows* the MSVC STL (`std.ixx`) and Windows SDK from an
installed Visual Studio via `mcpp.toolchain.msvc` discovery. But MSVC itself
is not a selectable toolchain: `mcpp toolchain default msvc` falls into the
xim-package path and fails with a confusing index error, and a machine
*without* VS fails deep inside the Clang std-module build with no actionable
message.

**Goal of this change (detection-first):** make MSVC a first-class *system*
toolchain that a developer can switch to / configure, where mcpp

1. checks whether the system has a usable MSVC installation,
2. auto-locates it (vswhere → env vars → well-known paths) and identifies its
   version (VS product, VC tools version, `cl.exe` compiler version),
3. and, when MSVC is absent, prints clear installation guidance —
   **mcpp never installs MSVC itself.**

**Explicit non-goal (phase 2, separate PR):** building with `cl.exe`
(`/std:c++23`, `.ifc` modules, `link.exe`/`lib.exe`, `/scanDependencies`).
Selecting MSVC and then running `mcpp build` must fail with a precise
"native cl.exe builds not yet supported" message that names the detected
version and points at `llvm@20.1.7` as the working alternative — never with
an incidental downstream error.

## 2. Existing scaffold (what we build on)

| Layer | Symbol | Status |
|---|---|---|
| model | `CompilerId::MSVC`, `is_msvc_target()` (`src/toolchain/model.cppm`) | exists |
| discovery | `mcpp.toolchain.msvc`: `find_vs_install_path()` (vswhere/env/paths), `find_msvc_tools_dir()`, `find_cl()`, `find_std_module_source()` (`src/toolchain/msvc.cppm`) | exists, used only by clang.cppm for std.ixx |
| capabilities | `capabilities_for()` MSVC case: `msvc-stl` / `lib.exe` (`src/toolchain/provider.cppm`) | stub exists |
| abi | msvcrt / msvc-stl / msvc mappings (`src/toolchain/abi.cppm`) | exists |
| spec | `frontend_candidates_for("msvc") → {"cl.exe"}` (`src/toolchain/registry.cppm`) | exists |
| manifest | `[toolchain] windows = "msvc@system"` documented in `src/manifest/types.cppm` comment | schema ready, unimplemented |
| escape hatch | `tcSpec == "system"` skips xim resolution (`src/build/prepare.cppm:685`) | precedent for system toolchains |

Missing: `detect()` cannot classify `cl.exe`; no cl version identification;
no lifecycle (`default`/`list`/`install`/`remove`) branches; no
prepare-time resolution of `msvc@system`; no doctor reporting; no
absent-MSVC guidance anywhere.

## 3. Design

### 3.1 The `msvc@system` spec

MSVC is a **system toolchain**: mcpp locates it, never installs/removes it.
Canonical spec string is `msvc@system`. Accepted user inputs (CLI and
manifest) and their normalization:

- `msvc`, `msvc@system` → `msvc@system`
- `msvc@<ver-prefix>` (e.g. `msvc@19.44`) → detect, then require the detected
  `cl.exe` version to start with `<ver-prefix>`; mismatch is an error that
  prints the detected version. (Pin-verify, not select-among-many: we always
  use the newest VC tools of the newest VS, same policy as vswhere `-latest`.)

The persisted global default (`~/.mcpp/config.toml [toolchain] default`) and
the manifest value are always the *stable* form `msvc@system` — never a
concrete version — so config survives VS updates. Concrete versions are
displayed, not stored.

Registry additions (`registry.cppm`):

- `bool is_system_toolchain(const ToolchainSpec&)` — true for `msvc` today
  (the concept is deliberately general; `system` (PATH compiler) stays as-is).
- `parse_toolchain_spec` unchanged; msvc branching happens in callers via
  `is_system_toolchain` (keeps parse pure).

### 3.2 Discovery + identification (`msvc.cppm` extension)

New exported surface:

```cpp
struct MsvcInstallation {
    std::filesystem::path vsRoot;          // C:\Program Files\Microsoft Visual Studio\2022\BuildTools
    std::string           vsProduct;       // "2022 BuildTools" (derived from path segments)
    std::string           toolsVersion;    // "14.44.35207" (VC\Tools\MSVC\<dir>)
    std::filesystem::path clPath;          // ...\bin\Hostx64\x64\cl.exe
    std::string           clVersion;       // "19.44.35211" (banner; falls back to toolsVersion-derived)
    std::string           arch;            // "x64" | "x86" | "arm64" (host-native target dir)
    bool                  hasStdModules;   // modules\std.ixx present
};

// Locate the best (newest) usable installation. nullopt = MSVC absent.
std::optional<MsvcInstallation> detect_installation();

// Pure, cross-platform, unit-testable:
// parse "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35211 for x64"
// (also localized banners: match a 19.xx.xxxxx token + arch token anywhere).
std::optional<std::pair<std::string,std::string>> parse_cl_banner(std::string_view);

// Multi-line installation guidance used everywhere MSVC is required but absent.
std::string install_guidance();
```

Implementation notes:

- `detect_installation()` composes the existing `find_vs_install_path()` +
  `find_latest_msvc_tools()`; adds cl banner capture. `cl.exe` is executed
  bare with stderr merged; the banner prints regardless of the "usage" exit
  status, so **ignore the exit code** and parse the output
  (`platform::process::capture`, not probe's `run_capture` which errors on
  non-zero exit). If execution or parsing fails, fall back to
  `clVersion = ""` and report `toolsVersion` — never fail detection just
  because the banner didn't parse.
- `parse_cl_banner` must not assume the English banner: scan for a
  `\d+\.\d+\.\d+`-shaped token (first match) and an arch token among
  `x64|x86|arm64|ARM64` (last match). Lives outside `#ifdef _WIN32` so Linux
  unit tests cover it.
- `install_guidance()` (also outside `#ifdef`) says, in this order: what was
  searched (vswhere / VSINSTALLDIR / standard paths), that mcpp does not
  install MSVC, and how to get it:
  - Visual Studio Installer → workload **Desktop development with C++**
    (component `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`)
  - or Build Tools only: `winget install Microsoft.VisualStudio.2022.BuildTools`
    then add the C++ workload in the installer
  - then re-run `mcpp toolchain default msvc`.
- `arch`: host-native pair only (`Hostx64\x64` on x64, `Hostarm64\arm64` on
  ARM64 hosts, falling back across the two). Cross target dirs are phase 2.

### 3.3 Lifecycle commands (`lifecycle.cppm`)

All four subcommands gain an msvc branch **before** the xim-package path,
gated on `is_system_toolchain(spec)`:

- `mcpp toolchain default msvc[@…]`
  - non-Windows → error: `the msvc toolchain is only available on Windows hosts`.
  - Windows, detected → verify optional version pin, then
    `write_default_toolchain(cfg, "msvc@system")` and print e.g.
    ```
    Detected   msvc 19.44.35211 (VS 2022 BuildTools, VC tools 14.44.35207)
               cl: C:\...\bin\Hostx64\x64\cl.exe
               import std: available (std.ixx)
    Default    set to msvc@system (was: llvm@20.1.7)
    note: `mcpp build` with native MSVC (cl.exe) is not yet supported — coming in a later release.
    ```
  - Windows, absent → error + `install_guidance()`, exit 1.
- `mcpp toolchain install msvc` — never installs. If detected: print the
  detection summary + hint `mcpp toolchain default msvc`, exit 0. If absent:
  `install_guidance()`, exit 1. Non-Windows: same error as `default`.
- `mcpp toolchain list` — on Windows, after the xim "Installed" rows, run
  detection and append a `System:` section:
  ```
  System:
     msvc 19.44.35211 (VS 2022 BuildTools)   C:\...\cl.exe
  ```
  with `*` when the effective default is `msvc@system`
  (`matches_default_toolchain` gains: `configuredDefault == "msvc@system"`
  matches compiler `msvc`, any version). Absent MSVC adds one hint line:
  `(msvc: not detected — run 'mcpp toolchain default msvc' for setup guidance)`.
  Non-Windows: section omitted entirely.
- `mcpp toolchain remove msvc` — error: system toolchain, remove via the
  Visual Studio Installer.

### 3.4 Build-time resolution (`prepare.cppm`)

In the toolchain-resolution chain (before the xim `resolve_xpkg_path`
branch): if `tcSpec` parses to an msvc system spec →

- non-Windows: error `toolchain 'msvc@system' is only available on Windows`.
- Windows: `msvc::detect_installation()`; absent → error + guidance;
  found → `explicit_compiler = clPath` (no xim resolution, no post-install
  fixup), `ui::info("Resolved", "msvc@system → <cl path>")`.

After `detect()` returns (ctx.tc populated), a single gate:
`tc.compiler == CompilerId::MSVC` → return error:

```
native MSVC (cl.exe) builds are not yet supported by mcpp.
       detected: msvc 19.44.35211 at C:\...\cl.exe (selection & detection work today)
       for building on Windows use the MSVC-ABI Clang toolchain instead:
         mcpp toolchain default llvm@20.1.7
```

This keeps every command that only *resolves/inspects* the toolchain
(`toolchain …`, `doctor`, `self env`) fully working while `build`/`run`/
`test` fail early with one owned message.

### 3.5 Detection classification (`detect.cppm`)

`detect()` classifies by driver **filename first** for MSVC: stem `cl`
(case-insensitive) short-circuits before the `--version` probe (cl.exe has no
`--version`; running it with GNU flags produces garbage). The MSVC path:

- run bare `cl.exe` (stderr merged), `parse_cl_banner` → `tc.version`, arch.
- `tc.compiler = CompilerId::MSVC`; `tc.targetTriple` = `x86_64-pc-windows-msvc`
  / `i686-…` / `aarch64-…` from the banner arch (fallback: path segment
  `Hostx64\x64`, then host arch).
- `tc.driverIdent` = normalized banner (fingerprint input — MSVC patch
  updates change the banner, invalidating BMI caches correctly).
- `tc.stdModuleSource` = `msvc::find_std_module_source()`;
  `tc.hasImportStd` accordingly. Skip `-dumpmachine` / `-print-sysroot` /
  payload probing (meaningless for cl.exe).
- `bmi_traits()` gains the MSVC branch (`ifc.cache` / `.ifc` /
  `needsExplicitModuleOutput=true`) so phase 2 doesn't silently reuse GCC
  defaults; unreachable in builds this release because of the 3.4 gate.

### 3.6 Doctor (`doctor.cppm`)

Windows-only section "Checking msvc (system)": detected → print product /
tools version / cl version / std.ixx presence as ok-lines; absent → a
warning (not a failure) with the one-line hint to run
`mcpp toolchain default msvc` for guidance. When the *effective default* is
`msvc@system`, doctor's existing toolchain check must treat the missing
std-module BMI as expected (build support pending), not an error.

### 3.7 User docs

`docs/03-toolchains.md`: new "MSVC (system toolchain, Windows)" section —
what detection does, the not-installed guidance, the build-support status,
`[toolchain] windows = "msvc@system"` manifest example (finally matching the
types.cppm comment).

## 4. Testing

Unit (`tests/unit/test_toolchain_msvc.cpp`, cross-platform):

- `parse_cl_banner`: English banner, localized banner (arch token + version
  token only), garbage → nullopt, arm64 variant.
- `install_guidance()` non-empty, mentions `winget` and does not claim mcpp
  installs MSVC.
- spec normalization: `msvc` / `msvc@system` / `msvc@19.44` →
  `is_system_toolchain` true; `gcc@16.1.0` → false.
- `matches_default_toolchain("msvc@system", "msvc", <any>)` true.
- `bmi_traits` MSVC branch.

E2E:

- `95_msvc_system_toolchain.sh` — `# requires: msvc` (new capability in
  `run_all.sh`, set on Windows when vswhere+VC tools present):
  `toolchain default msvc` succeeds & prints `Detected` + `msvc@system`;
  `toolchain list` shows the System section with `*`; `mcpp build` on a
  scratch project fails with `not yet supported` + detected version;
  `toolchain remove msvc` fails with the system-toolchain message;
  `toolchain install msvc` exits 0 with the already-installed summary.
- `96_msvc_unavailable_nonwindows.sh` — `# requires: unix-shell`:
  `toolchain default msvc` fails with `only available on Windows`;
  `toolchain list` shows no System section.

CI: `ci-windows.yml` gains one step "Toolchain: MSVC — detection & selection"
running the e2e positive flow against `$MCPP_SELF` (windows-latest ships VS
2022 Enterprise, so detection must succeed; treat failure as regression).

## 5. Rollout plan

1. Temp branch `tmp/msvc-windows-ci`: implementation + **all workflows except
   `ci-windows.yml` deleted** (CI-time economy while iterating), draft PR,
   iterate to green.
2. Real branch `feat/msvc-system-toolchain` from the green tree with
   workflows restored, version bumped to **0.0.88**
   (`src/toolchain/fingerprint.cppm` `MCPP_VERSION` + `mcpp.toml` version +
   `CHANGELOG.md`), real PR, full CI green, merge.
3. Tag `v0.0.88` → `release.yml` → mirror assets to xlings-res (gh + gtc
   both ends) → xim-pkgindex mcpp package update (gitee auto-mirrors) →
   `xlings install mcpp` verification → bump bootstrap pin.

## 6. Risks

- **cl.exe bare-run exit code/output shape varies by VS version** — mitigated:
  ignore exit code, tolerate parse failure (fall back to tools-dir version).
- **windows-latest runner image changes VS layout** — mitigated: detection has
  three strategies; CI step asserts detection so a silent regression surfaces.
- **Non-English banners** — mitigated: token-based parsing, unit-tested.
- **Users interpret `msvc` selection as build support** — mitigated: the
  explicit note at selection time + the owned build-time error.
