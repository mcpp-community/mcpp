# Library / Component Download Progress — Design

> Status: approved, in implementation
> Target release: mcpp 0.0.53
> Scope: make library / component downloads show the same live progress + speed
> that toolchain downloads already show, across Linux / macOS / Windows.

## 1. Problem

`mcpp toolchain install` shows a live progress bar with percent, bytes and
transfer speed. Installing **library / component dependencies** (during
`mcpp build`, dependency resolution, or `mcpp new --template`) prints a single
`Downloading <pkg> v<ver>` line and then **appears to hang** for a long time with
no percent, no speed and no sign of life — especially for large packages on slow
mirrors.

## 2. Root cause (evidence-based)

There are **three** download code paths. They were not equally instrumented.

| Path | Trigger | Transport | Symptom |
|------|---------|-----------|---------|
| ① Toolchain | `mcpp toolchain install` | NDJSON `interface install_packages` + `CliInstallProgress` | progress OK |
| ② Builtin-index deps | dep resolves via builtin index (`useProjectEnv = false`) | same as ① | progress OK *once bytes flow* |
| ③ Custom/project-index deps | dep resolves via a project-added index (`useProjectEnv = true`) | **`install_direct(projEnv, target, quiet=true)`** | **fully silent** |

Two independent defects fall out of this:

### Defect A — path ③ is silenced

`src/cli.cppm` routes project/custom-index dependency installs through
`mcpp::xlings::install_direct(projEnv, target, /*quiet=*/true)`. `quiet=true`
redirects xlings' stdout/stderr to the platform null device, so xlings' own
progress output is discarded and **no NDJSON events are parsed** (the direct CLI
does not speak NDJSON). The `Downloading <pkg>` line is printed by mcpp *before*
this call, then the whole download runs dark.

This path was switched to the direct CLI in a prior change to guarantee that
project-index packages land in the **project-local** xlings data root (so a
package's install hook can find sibling packages from the same index). That
change traded away progress as a side effect.

**Why the NDJSON interface is in fact safe here (verified against xlings
source):** in the pinned xlings, the `install_packages` *capability* and the
`install` *CLI* both call the same `xim::cmd_install(...)`. The install
destination is chosen by **package scope** — `storeRoot = (scope == Project ?
project_data_dir() : global_data_dir()) / "xpkgs"` — which is derived from
*which index the package came from*, not from interface-vs-CLI. `download_progress`
events are emitted unconditionally (no scope gate). So the NDJSON interface
installs project-scoped packages into the project-local data root **and** streams
progress. The earlier switch was working around index *exposure*, which is now
handled separately (the project index is symlinked/exposed into the project data
dir and targets are passed as `indexName:fqname@version`).

### Defect B — the "connecting" phase freezes the line (all NDJSON paths)

Captured NDJSON for a real package download shows the downloader emits
`download_progress` events with `totalBytes = 0` (and often `downloadedBytes = 0`)
during the initial connect / TLS / redirect / pre-sizing window, before the
transfer's total size is known:

```
elapsed 0.2s–1.4s : downloadedBytes=0  totalBytes=0  sizesReady=false   (connecting)
elapsed 1.6s      : downloadedBytes=57344    totalBytes=1754511         (bytes flow)
elapsed 2.0s      : downloadedBytes=1754511  totalBytes=1754511  finished
```

Both `CliInstallProgress::on_data` and `make_bootstrap_progress_callback` only
call `ProgressBar::update_bytes` when `total > 0`, so during the connecting
window the line shows nothing. For a large file on a slow mirror this window can
last many seconds and reads as a hang. Toolchain downloads hide this because the
connect window is tiny relative to a multi-minute transfer; library downloads do
not.

## 3. Design

Three changes, all behind existing abstractions, all cross-platform.

### 3.1 Fix 1 — path ③ uses the NDJSON interface (mcpp-style bar)

In `src/cli.cppm`, the `useProjectEnv` branch of the dependency install lambda
calls the NDJSON interface with the project env and an `CliInstallProgress`
handler, instead of the silenced direct CLI:

```cpp
if (useProjectEnv) {
    auto projEnv  = mcpp::config::make_project_xlings_env(**cfg, *root);
    auto argsJson = std::format(R"({{"targets":["{}"],"yes":true}})", target);
    CliInstallProgress progress;
    auto r = mcpp::xlings::call(projEnv, "install_packages", argsJson, &progress);
    if (!r) return std::unexpected(mcpp::pm::CallError{r.error()});
    return *r;
}
```

This yields the same cyan `Downloading … [bar] X/Y  Z/s` UI on **all** three
paths. Package scope (and therefore install location) is unchanged — packages
from a project index still install into the project-local data root.

### 3.2 Fix 2 — indeterminate rendering while size is unknown

Add `ProgressBar::update_indeterminate(current_bytes, elapsed_sec)`: render a
swept/indeterminate bar plus an info suffix that ticks — `connecting…  Ns`, or a
byte counter `X.Y MB  Ns` once bytes arrive without a known total. This also
covers servers that stream without `Content-Length`.

Wire both progress consumers (`CliInstallProgress::on_data` and
`make_bootstrap_progress_callback`) to call `update_indeterminate` when the
active file has `total == 0` (and `update_bytes` once `total > 0`).

Refactor `render_line` so the bar string is produced by a small callable; the
percent path and the indeterminate path share the terminal-width budgeting.

### 3.3 Fix 3 — pin xlings to the latest release

Bump `pinned::kXlingsVersion` in `src/xlings.cppm` to the latest xlings release
(the version that ships the unified `cmd_install` + scope-routed install +
unconditional `download_progress`). This removes any dependence on older xlings
behavior for the path ③ install location.

## 4. Cross-platform

- Progress rendering already runs on Linux/macOS/Windows (toolchain path uses it
  today); no new platform branches are introduced.
- `mcpp::xlings::call` / `build_interface_command` already handle the Windows vs
  POSIX command shape and env propagation; path ③ inherits that.
- `update_indeterminate` uses the same `render_line` budgeting (TIOCGWINSZ /
  `$COLUMNS` / 80-col fallback) as the existing bar; non-TTY / `--quiet` paths
  remain silent via the existing `g_quiet` guard.

## 5. Testing

- **Unit-ish / build:** project builds clean with the existing warning gate.
- **E2E regression:** `tests/e2e/52_local_path_namespaced_index.sh` and
  `tests/e2e/58_preinstall_mcpp_deps_for_hooks.sh` currently assert path ③ uses
  the *direct* CLI. They are updated to assert the **interface** transport while
  keeping their existing invariants (project-local install location, and
  `mcpp.deps` installed before a dependent package's install hook runs).
- **Real integration check:** with a real project-local custom index and a real
  installable package, confirm (a) the package installs into the project-local
  data root, (b) the install hook still sees its `mcpp.deps`, and (c) the
  `Downloading … [bar] X/Y Z/s` UI renders. This is the empirical gate before
  merge.

## 6. Rollout

1. Implement Fix 1–3; update e2e tests.
2. Local build + e2e + real integration check.
3. Bump `MCPP_VERSION` (`src/toolchain/fingerprint.cppm`) and `mcpp.toml` to
   `0.0.53`; CHANGELOG entry.
4. PR → CI green → squash merge → trigger release `0.0.53`.
5. Ecosystem: add mcpp `0.0.53` to the package index (mcpp xpkg descriptor:
   version + per-platform sha256), publish release artifacts to the
   release-asset mirrors, PR + merge, verify an end-to-end install of the new
   version.

## 7. Out of scope / follow-ups

- No change to xlings' own native CLI progress UI.
- No change to the dependency-resolution / preinstall ordering logic (it already
  installs deps before dependents; only the per-install transport on path ③
  changes).
