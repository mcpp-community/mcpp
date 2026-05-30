# Index Refresh And Dependency Cache Label Fix

> Date: 2026-05-31 | Status: active | Branch:
> `codex/quiet-index-refresh-cache-labels`

Tracking issue: https://github.com/mcpp-community/mcpp/issues/90

## Scope

This document tracks the mcpp-side fixes found while validating `mcpp build`
against the `xlings` project.

The current work is intentionally limited to build-tool behavior:

- make automatic package-index refresh quiet from the mcpp user's perspective;
- make index freshness use a reliable mcpp-owned timestamp marker;
- make dependency cache status use canonical dependency identities;
- make large xpkg/toolchain installs retry through direct `xlings install`
  when the NDJSON interface path fails;
- keep local xlings validation as the integration proof.

## Findings

### 1. Auto-refresh leaks xlings update output

`mcpp build` calls `ensure_index_fresh()` when a project has version-source
dependencies. That path currently calls `xlings update` with `quiet=false`.

Observed output:

```text
Updating package index (auto-refresh)
[1/7] awesome::/home/speak/.mcpp/registry/data/xim-index-repos/...
...
index updated
```

The `[N/M] awesome::...` lines are xlings internals. mcpp users should only see
the mcpp-level status line unless they explicitly run a verbose/manual index
update command.

### 2. Freshness check uses an unstable directory mtime

`is_index_fresh()` currently checks:

```text
~/.mcpp/registry/data/mcpplibs/pkgs
```

Local evidence showed:

```text
mcpplibs/pkgs             2026-05-31 01:55:40
mcpplibs/.git/FETCH_HEAD  2026-05-31 04:31:24
xim-indexrepos.json       2026-05-31 04:31:27
```

So `xlings update` can refresh the repository without updating the `pkgs/`
directory mtime. Once the TTL expires, every full `prepare_build()` can decide
that the index is stale again.

### 3. Dependency status labels can say `Compiling` for cached deps

For xlings:

```toml
[dependencies.mcpplibs]
xpkg = "0.0.41"
tinyhttps = "0.2.3"
```

The UI prints dependency names as `mcpplibs.xpkg` and `mcpplibs.tinyhttps`.
The cache hit label is currently built from the dependency package's own
`mcpp.toml` name. For tinyhttps that manifest uses:

```toml
namespace = "mcpplibs"
name = "tinyhttps"
version = "0.2.3"
```

The cache directory is therefore:

```text
~/.mcpp/bmi/<fingerprint>/deps/mcpplibs/tinyhttps@0.2.3
```

The UI compares `tinyhttps` against `mcpplibs.tinyhttps`, so it can print
`Compiling mcpplibs.tinyhttps` even when the cache entry exists.

### 4. libxpkg 0.0.41 has stale embedded mcpp metadata

The installed `mcpplibs.xpkg@0.0.41` archive contains:

```toml
name = "xpkg"
version = "0.0.39"
```

This causes mcpp to cache the resolved package as:

```text
deps/mcpplibs/xpkg@0.0.39
```

while the consumer dependency remains `mcpplibs.xpkg v0.0.41`.

### 5. Cold Linux CI can fail musl-gcc via xlings interface install

PR #91 inherited a main-branch Linux CI failure in:

```text
Toolchain: musl-gcc - build mcpp (--target)
```

The failing command was:

```text
mcpp build --target x86_64-linux-musl
```

The visible error was only:

```text
xlings install of 'xim:musl-gcc@15.1.0' failed (exit 1)
```

The same workflow's e2e suite had already proven that fresh-home musl-gcc
installation can work, and recent CI history showed this as a cold-cache
failure on main too. The weak point is that `Fetcher::resolve_xpkg_path()`
uses only the xlings NDJSON interface path for regular xpkg installs, while
`mcpp.xlings::install_with_progress()` already documents the direct CLI path
as more reliable for large package installs.

## Implementation Plan

- [x] Add focused regression coverage for default-index refresh quietness.
- [x] Add a reliable mcpp-owned index freshness marker after successful update.
- [x] Make automatic refresh call `update_index(..., quiet=true)`.
- [x] Keep explicit/manual index update output available for diagnostics.
- [x] Canonicalize dependency identity used for cache-hit labels.
- [x] Avoid using stale embedded package version as the user-facing resolved
      dependency version when the index resolution already knows the requested
      version.
- [x] Add a direct `xlings install <target> -y` fallback after interface
      install failure, preserving visible direct-install output for CI
      diagnostics.
- [x] Validate with the local xlings checkout using the new mcpp binary.
- [x] Push a draft PR and use it as the multi-commit checkpoint.

## Verification Plan

- [x] Run targeted unit/e2e coverage in mcpp.
- [x] Build mcpp itself.
- [x] Run `mcpp build` in `/home/speak/test/tmp/xlings` with the new binary.
- [x] Confirm auto-refresh no longer prints xlings `[N/M] index::path` lines.
- [x] Confirm repeated xlings full prepare does not refresh the index again
      while the marker is inside TTL.
- [x] Confirm cached dependencies display as `Cached` when their artifacts are
      staged.
- [ ] Record CI status on the PR after the second checkpoint commit.

## Dynamic Notes

- `mcpp build` has a 0.01s fast path when `build.ninja` is newer than source
  files; the noisy path happens when full `prepare_build()` runs.
- A no-change `ninja -C target/... -n` in the local xlings checkout reported
  `ninja: no work to do`, so the status-line issue is partly UI/cache identity
  rather than always an actual compiler invocation.
- Added regression tests:
  - `tests/unit/test_xlings.cpp` now requires `.mcpp-index-updated` for a
    fresh default index.
  - `tests/e2e/53_namespaced_cache_label.sh` verifies quiet auto-refresh,
    marker creation, and canonical `Cached compat.widget v1.0.0` output.
- Local mcpp verification:
  - `mcpp test -- --gtest_filter=XlingsIndexFreshness.*` passed; the command
    built and ran all 16 test binaries, with `test_xlings` covering the filter.
  - `mcpp build --no-cache` passed and produced
    `target/x86_64-linux-gnu/4d24c8b57fdbbbb4/bin/mcpp`.
  - `MCPP=.../bin/mcpp bash tests/e2e/49_bmi_cache_nested_custom_index.sh`
    passed.
  - `MCPP=.../bin/mcpp bash tests/e2e/52_local_path_namespaced_index.sh`
    passed.
  - `MCPP=.../bin/mcpp bash tests/e2e/53_namespaced_cache_label.sh` passed.
- Local xlings verification with the new mcpp binary:
  - First `build --print-fingerprint`: exit `0`, displayed only
    `Updating package index (auto-refresh)` with no xlings `[N/M]` lines.
  - Second `build --print-fingerprint`: exit `0`, no index refresh line, and
    `mcpplibs.tinyhttps` / `mcpplibs.xpkg` displayed as `Cached`.
  - Marker was written at
    `~/.mcpp/registry/data/mcpplibs/.mcpp-index-updated`.
- CI follow-up:
  - Linux CI on PR #91 failed in `Toolchain: musl-gcc - build mcpp
    (--target)`.
  - The same step had already failed on `origin/main` run `26691717542`, so
    this is not introduced solely by PR #91.
  - Added direct-install fallback in `src/xlings.cppm` and
    `src/pm/package_fetcher.cppm`.
  - Re-ran local `mcpp build --no-cache`, `mcpp test --
    --gtest_filter=XlingsIndexFreshness.*`, and the three related e2e tests.
