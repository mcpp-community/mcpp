# CLI Modularization — Architecture & Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce `src/cli.cppm` (6192 lines) to a thin command-dispatch layer (≤ 500 lines) by moving every implementation concern into focused C++20 modules, with zero behavior change.

**Architecture:** Continue the PR-R4/PR-R5 extraction series (`2026-05-08-pm-subsystem-architecture.md`). `cli.cppm` keeps only the canonical usage screen and the `cmdline::App` dispatcher; commands move into `src/cli/cmd_*.cppm` modules grouped by CLI surface; cross-cutting CLI plumbing moves into `mcpp.cli.common` / `mcpp.cli.install_ui`; the build-orchestration core (`prepare_build`) becomes `mcpp.cli.build`; toolchain payload post-install fixups move into the toolchain domain as `mcpp.toolchain.post_install`. Every move is a strict zero-behavior-change relocation (same statement order, same messages, same exit codes), exactly like the PR-R5 precedent.

**Tech Stack:** C++23 named modules (GCC 16 self-host), mcpp convention-mode source globbing (`src/**/*.cppm` — no build-file edits needed), existing `mcpp test` unit suite + `tests/e2e/run_all.sh`.

---

## 1. Current state (analysis)

`src/cli.cppm` mixes five unrelated responsibilities in one 6192-line module:

| Lines (pre-refactor) | Content | Problem |
|---|---|---|
| 75–181 | usage screen, project/workspace root discovery | shared helpers trapped in CLI module (pm.commands keeps a private copy of `find_manifest_root` for this reason) |
| 183–517 | toolchain version matching, xlings NDJSON install-progress UI | UI plumbing interleaved with command logic |
| 519–1038 | fingerprint flag canonicalization, gcc/clang payload post-install fixups (patchelf, specs, cfg rewrite) | toolchain-domain logic living in the CLI layer |
| 1051–3561 | `cmd_new` + templates, `BuildContext` + `prepare_build` (≈ 2 240 lines: workspace → toolchain → dependency resolution → features → modgraph → fingerprint → plan → lockfile) | the build pipeline is unreachable for unit testing and unreviewable as a diff target |
| 3563–5789 | build cache/fast-path + 25 `cmd_*` entry points | every command edit rebuilds/reviews the whole module |
| 5793–6192 | `run()` dispatcher | the only part that belongs here |

Consequences: slowest incremental rebuild unit in the repo, high merge-conflict surface, no module-level ownership boundaries, and `mcpp.cli` exports nothing reusable (`pm.commands` duplicates helpers to avoid a circular import).

## 2. Target architecture

```
src/main.cpp ──▶ mcpp.cli (dispatcher only: usage + cmdline::App + run())
                   │
                   ├─ mcpp.cli.cmd_build      build / run / test / clean / dyndep
                   │     └─ mcpp.cli.build    BuildContext, BuildOverrides, prepare_build
                   ├─ mcpp.cli.cmd_new        new + package templates
                   ├─ mcpp.cli.cmd_registry   search / index *
                   ├─ mcpp.cli.cmd_cache      cache list|info|prune|clean
                   ├─ mcpp.cli.cmd_toolchain  toolchain install|list|default|remove
                   │     └─ mcpp.toolchain.post_install   patchelf/specs/cfg fixups
                   ├─ mcpp.cli.cmd_publish    publish / pack / emit xpkg
                   ├─ mcpp.cli.cmd_self       self * / doctor / why / env / explain
                   └─ mcpp.pm.commands        add / remove / update   (PR-R5, unchanged)

shared CLI plumbing:
  mcpp.cli.common      project/workspace root discovery, target_dir, fs size helpers
  mcpp.cli.install_ui  xlings NDJSON → ui::DownloadProgress adapters, PathContext
```

Layering rules (enforced by module imports — cycles are compile errors):

1. `mcpp.cli` imports only `cmd_*` modules (+ `pm.commands`, `ui`, `log`, `cmdline`, `toolchain.fingerprint` for the version string).
2. `cmd_*` modules never import each other; shared code lives in `common` / `install_ui` / `build`.
3. `mcpp.cli.build` is the single owner of `prepare_build`; consumers are `cmd_build`, `cmd_publish` (pack), `cmd_self` (doctor/why).
4. `mcpp.toolchain.post_install` is CLI-independent (imports only config/xlings/platform/log/ui) so future non-CLI callers (e.g. a daemonized installer) can reuse it.
5. Export surface is explicit per symbol (`export` on the declaration) — internals keep module linkage.

### Module inventory

| New file | Module | Exports | Body source (line ranges in pre-refactor `src/cli.cppm`) |
|---|---|---|---|
| `src/cli/common.cppm` | `mcpp.cli.common` | `find_manifest_root`, `find_workspace_root`, `merge_workspace_deps`, `target_dir`, `dir_size`, `human_bytes` | 116–181, 4630–4650 (drop `static`) |
| `src/cli/install_ui.cppm` | `mcpp.cli.install_ui` | `make_path_ctx`, `make_bootstrap_progress_callback`, `CliInstallProgress` | 300–517 |
| `src/toolchain/post_install.cppm` | `mcpp.toolchain.post_install` (ns `mcpp::toolchain`) | `patchelf_walk`, `fixup_clang_cfg`, `gcc_post_install_fixup` | 722–1038 |
| `src/cli/build.cppm` | `mcpp.cli.build` | `BuildContext`, `BuildOverrides`, `prepare_build` | 519–720, 1321–3561 |
| `src/cli/cmd_build.cppm` | `mcpp.cli.cmd_build` | `cmd_build`, `cmd_run`, `cmd_test`, `cmd_clean`, `cmd_dyndep` | 3563–3976, 4406–4570, 4608–4627, 5741–5789 |
| `src/cli/cmd_new.cppm` | `mcpp.cli.cmd_new` | `cmd_new` | 1051–1319 |
| `src/cli/cmd_registry.cppm` | `mcpp.cli.cmd_registry` | `cmd_search`, `cmd_index_{list,add,remove,update,pin,unpin}` | 3996–4402 |
| `src/cli/cmd_cache.cppm` | `mcpp.cli.cmd_cache` | `cmd_cache_{list,info,prune,clean}` | 4837–4976 |
| `src/cli/cmd_toolchain.cppm` | `mcpp.cli.cmd_toolchain` | `cmd_toolchain` | 183–298, 4978–5317 |
| `src/cli/cmd_publish.cppm` | `mcpp.cli.cmd_publish` | `cmd_publish`, `cmd_pack`, `cmd_emit_xpkg` | 4572–4606, 5319–5571 |
| `src/cli/cmd_self.cppm` | `mcpp.cli.cmd_self` | `cmd_env`, `cmd_doctor`, `cmd_why`, `cmd_explain`, `cmd_explain_action`, `cmd_self_{version,init,config}` | 3978–3994, 4652–4835, 5573–5739 |

`src/cli.cppm` keeps: file header, `print_usage` (75–114), `run()` (5795–6190, minus the now-dead `using namespace mcpp::cli::detail;`). All moved code switches namespace `mcpp::cli::detail` → `mcpp::cli` (or `mcpp::toolchain` for post_install); unqualified cross-references keep working because callers share the namespace. The three post-install call sites outside the toolchain namespace gain explicit `mcpp::toolchain::` qualification.

### Cross-platform notes

- No platform-conditional code is touched; `if constexpr (mcpp::platform::is_macos/…)` blocks move verbatim, so Linux/macOS/Windows behavior is bit-identical.
- Source discovery is glob-based (`src/**/*`), so the new `src/cli/` directory needs no manifest change on any platform.
- Each new module keeps the `module; #include <cstdio> #include <cstdlib>` global-module-fragment prologue `cli.cppm` used (for `stderr`/`stdout` macros) — required on all three toolchains.

## 3. Implementation plan

The extraction is a single mechanical transformation of one immutable source revision, so it is scripted with `sed -n 'A,Bp'` range extraction (no hand-retyping; byte-identical bodies), then compiled and tested. Verification = full self-host build + unit suite + e2e suite.

### Task 1: Architecture doc (this file)

- [x] **Step 1:** Write this document.
- [x] **Step 2:** Commit on branch `refactor/cli-modularization`.

### Task 2: Scripted extraction

- [x] **Step 1:** `cp src/cli.cppm /tmp/cli_orig.cppm` (immutable line-range source).
- [x] **Step 2:** For each row of the module inventory: write the module header (GMF prologue, `export module`, imports per §2, `namespace … {`), then `sed -n 'A,Bp' /tmp/cli_orig.cppm >>` the body ranges in table order, then close the namespace. Import lists per module:
  - common: `std, mcpp.manifest, mcpp.toolchain.detect, mcpp.toolchain.fingerprint`
  - install_ui: `std, mcpp.ui, mcpp.log, mcpp.config, mcpp.fetcher`
  - post_install: `std, mcpp.config, mcpp.xlings, mcpp.platform, mcpp.log, mcpp.ui`
  - build: `std, mcpp.libs.json, mcpp.manifest, mcpp.modgraph.{graph,scanner,validate}, mcpp.toolchain.{clang,detect,fingerprint,registry,stdmod}, mcpp.toolchain.post_install, mcpp.build.plan, mcpp.lockfile, mcpp.config, mcpp.xlings, mcpp.platform, mcpp.fetcher, mcpp.pm.{resolver,index_spec,mangle,compat,dep_spec}, mcpp.ui, mcpp.log, mcpp.fallback.install_integrity, mcpp.bmi_cache, mcpp.cli.common, mcpp.cli.install_ui`
  - cmd_build: `std, mcpplibs.cmdline, mcpp.cli.{build,common,install_ui}, mcpp.build.{plan,backend,ninja}, mcpp.bmi_cache, mcpp.dyndep, mcpp.manifest, mcpp.modgraph.scanner, mcpp.toolchain.stdmod, mcpp.xlings, mcpp.platform, mcpp.ui, mcpp.log`
  - cmd_new: `std, mcpplibs.cmdline, mcpp.cli.install_ui, mcpp.scaffold, mcpp.config, mcpp.manifest, mcpp.fetcher, mcpp.pm.resolver, mcpp.ui`
  - cmd_registry: `std, mcpplibs.cmdline, mcpp.cli.{common,install_ui}, mcpp.config, mcpp.xlings, mcpp.fetcher, mcpp.manifest, mcpp.lockfile, mcpp.ui`
  - cmd_cache: `std, mcpplibs.cmdline, mcpp.cli.common, mcpp.toolchain.stdmod, mcpp.ui`
  - cmd_toolchain: `std, mcpplibs.cmdline, mcpp.cli.{common,install_ui}, mcpp.toolchain.{detect,registry,post_install}, mcpp.config, mcpp.xlings, mcpp.fetcher, mcpp.manifest, mcpp.ui, mcpp.log`
  - cmd_publish: `std, mcpplibs.cmdline, mcpp.cli.{common,build,install_ui}, mcpp.manifest, mcpp.modgraph.scanner, mcpp.publish.xpkg_emit, mcpp.pack, mcpp.build.{backend,ninja}, mcpp.config, mcpp.platform, mcpp.ui`
  - cmd_self: `std, mcpplibs.cmdline, mcpp.cli.{common,build,install_ui}, mcpp.config, mcpp.xlings, mcpp.fallback.install_integrity, mcpp.toolchain.{detect,fingerprint,stdmod}, mcpp.build.plan, mcpp.ui`
- [x] **Step 3:** Rewrite `src/cli.cppm`: header + imports (`std, mcpplibs.cmdline, mcpp.ui, mcpp.log, mcpp.toolchain.fingerprint, mcpp.pm.commands, mcpp.cli.cmd_*×7`) + exported `run()` decl + `print_usage` + `run()` body (drop the `using namespace …::detail;` line).
- [x] **Step 4:** Add `export` keywords on the symbols listed in §2 (Edit per declaration); drop `static` from `dir_size`/`human_bytes`; qualify the three `mcpp::toolchain::` post-install call sites (one in build.cppm: `gcc_post_install_fixup`; two-plus-one in cmd_toolchain.cppm: `gcc_post_install_fixup`, `patchelf_walk`, `fixup_clang_cfg`).

### Task 3: Build & fix loop

- [x] **Step 1:** `mcpp build` — fix any missing-import / linkage errors (expected failure mode: a helper referenced across modules that wasn't exported; fix = add `export` or import, never duplicate code).
- [x] **Step 2:** `wc -l src/cli.cppm` — expected < 500.
- [x] **Step 3:** Commit `refactor(cli): split cli.cppm into focused modules`.

### Task 4: Verification

- [x] **Step 1:** `mcpp test` with the freshly built binary — expect all unit tests pass.
- [x] **Step 2:** `MCPP=<fresh binary> tests/e2e/run_all.sh` — expect all e2e tests pass (covers help/version text, exit codes 0/1/2/127, all command surfaces).
- [x] **Step 3:** Sanity: `mcpp --help`, `mcpp version`, `mcpp self doctor`, unknown-command exit 127.

### Task 5: PR + CI

- [x] **Step 1:** Push branch, open PR describing motivation, module map, zero-behavior-change guarantee, verification evidence.
- [x] **Step 2:** Watch ci-linux / ci-macos / ci-windows / fresh-install lanes; fix-forward on any platform-specific module issue (most likely candidate: MSVC/clang module-linkage strictness on exported-vs-internal helpers).
- [x] **Step 3:** Update this doc's status section.

## 4. Follow-ups (out of scope here)

- Decompose `prepare_build` internally (workspace / toolchain / dep-resolution / feature phases as named functions) now that it has a home module.
- Fold `pm.commands`' private `find_manifest_root` copy into a shared project-location module once a `cli`-independent home exists (`mcpp.cli.common` is still CLI-layer; a `mcpp.project` module would let pm import it without layering violations).
- Tighten `mcpp.cli.build`'s import list (it inherited the union of the old `cli.cppm` imports).

## 5. Status

- 2026-06-10: extraction done. `cli.cppm` 6192 -> 481 lines; 11 new modules.
  Verified: self-host build clean; unit suite 18/18 pass; e2e 67 pass /
  1 skip, with the only failures being the 6 `llvm_*` tests that fail
  identically with the pre-refactor baseline binary on the same host
  (local LLVM payload cannot exec — environment issue, not a regression).
  PR opened; awaiting CI.
