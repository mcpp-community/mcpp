# MSVC System-Toolchain Detection — Implementation Plan

Companion to `2026-07-13-msvc-system-toolchain-detection-design.md`.
Target release: 0.0.88. Each step compiles + `mcpp test` passes on its own.

## Step 1 — discovery & identification core (`src/toolchain/msvc.cppm`)

- Add `MsvcInstallation`, `detect_installation()`, `parse_cl_banner()`,
  `install_guidance()` per design §3.2.
- `parse_cl_banner` + `install_guidance` live outside `#ifdef _WIN32`.
- `vsProduct` derived from the two path segments after
  `Microsoft Visual Studio` (`"2022 BuildTools"`); empty if layout unusual.
- cl banner capture: `platform::process::capture` on bare cl.exe, merged
  stderr, exit code ignored.

## Step 2 — spec layer (`src/toolchain/registry.cppm`)

- `is_system_toolchain(const ToolchainSpec&)` (compiler == "msvc").
- `matches_default_toolchain`: `"msvc@system"` matches (`"msvc"`, any ver).
- `display_label("msvc", …)` unchanged (generic path already fine).

## Step 3 — model traits (`src/toolchain/model.cppm`)

- `bmi_traits`: MSVC branch → `{ifc.cache, .ifc, ifc, true, false, false}`.

## Step 4 — detection (`src/toolchain/detect.cppm`)

- Filename short-circuit (stem `cl`, case-insensitive) before `--version`
  probe → `enrich` MSVC path per design §3.5 (banner → version/arch/triple,
  driverIdent, std.ixx, skip dumpmachine/sysroot/payloads).

## Step 5 — lifecycle (`src/toolchain/lifecycle.cppm`)

- `toolchain_set_default` / `toolchain_install` / `toolchain_remove` /
  `toolchain_list`: msvc branches per design §3.3 (non-Windows error;
  detect-report-persist; guidance on absence; System section in list).

## Step 6 — build-time resolution + gate (`src/build/prepare.cppm`)

- msvc spec branch before xim resolution (design §3.4).
- post-detect gate on `CompilerId::MSVC` with the owned
  "not yet supported" error.

## Step 7 — doctor (`src/doctor.cppm`)

- Windows "Checking msvc (system)" section; msvc-default-aware std-BMI check.

## Step 8 — tests

- `tests/unit/test_toolchain_msvc.cpp` (banner/guidance/spec/traits, §4).
- `tests/e2e/95_msvc_system_toolchain.sh` (`# requires: msvc`),
  `tests/e2e/96_msvc_unavailable_nonwindows.sh` (`# requires: unix-shell`).
- `tests/e2e/run_all.sh`: `msvc` capability (Windows + vswhere + VC tools).

## Step 9 — docs

- `docs/03-toolchains.md` MSVC section; CHANGELOG entry.

## Step 10 — CI

- `ci-windows.yml`: step "Toolchain: MSVC — detection & selection" driving
  the 95 e2e flow via `$MCPP_SELF`.

## Rollout (design §5)

1. `tmp/msvc-windows-ci` branch: steps 1-10 + delete all workflows except
   `ci-windows.yml`; draft PR; iterate until green.
2. `feat/msvc-system-toolchain`: green tree + workflows restored + version
   bump (fingerprint.cppm, mcpp.toml, CHANGELOG) → real PR → full CI → merge.
3. Release v0.0.88 → xlings-res mirror (gh + gtc) → xim-pkgindex →
   `xlings install mcpp` verify → bootstrap pin bump.

## Verification per step

- Local: `mcpp build && mcpp test` (Linux) after every step; unit tests for
  the pure functions run on Linux.
- Windows behavior only verifiable in CI — keep Windows-only logic thin and
  push the parseable/testable parts into pure functions.
