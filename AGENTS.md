# AGENTS.md

mcpp is a build tool and package manager for C++23 modules, written in C++23 modules and built by
itself. This file states the rules that hold across the repository, in their shortest form; each
links to the document that states it in full. Where this file and a linked document differ, the
linked document holds.

## 1. Where a fact lives

| Tree | States | Rule |
|---|---|---|
| [`docs/`](docs/README.md), [`docs/zh/`](docs/zh/README.md) | how to use what mcpp implements | describes the current implementation only; English and Chinese stay in parity |
| [`docs/specs/`](docs/specs/README.md) | semantics and constraints (SPEC-001 to SPEC-016) | RFC 2119 wording; every clause carries its implementation status and the version it holds from |
| [`.agents/docs/`](.agents/docs/README.md) | design records: measurements, decisions, what was refuted | front matter `subject`, `status`; the index is generated (`.github/tools/gen_agents_index.py`) |
| [`.agents/skills/`](.agents/skills/) | procedures: [contributing](.agents/skills/mcpp-contributing/SKILL.md), [docs style](.agents/skills/mcpp-docs-style/SKILL.md), [release](.agents/skills/mcpp-release/SKILL.md), [usage](.agents/skills/mcpp-usage/SKILL.md) | ordered steps with criteria |
| [`CHANGELOG.md`](CHANGELOG.md) | one section per release; release.yml publishes it as the release notes | English, Keep a Changelog |

A change of behaviour updates the spec clause, the usage chapter in both languages and the
CHANGELOG in the same pull request.

## 2. Architecture

- **Mechanism in the engine, facts in data.** The engine names no operating system and no
  implementation. Target rows and built-in providers are data
  ([`modules/toolchain-model/data/builtin.toml`](modules/toolchain-model/data/builtin.toml),
  [SPEC-016](docs/specs/targets-and-providers.md)); platforms, sysroots, runners and layer
  suppliers are packages ([SPEC-013](docs/specs/platforms.md)).
- **The target side is five layers** — `compiler`, `compiler-runtime`, `kernel-abi`, `c-abi`,
  `c++-abi` — a closed set. Packages state `provides` and `requires` in the `mcpp:` namespace; each
  layer has exactly one supplier; the target side is resolved once, after dependency resolution
  ([SPEC-002](docs/specs/target-side.md)). A sysroot that carries a manifest is a package like any
  other: its `provides` answers its layers and its `[build]` keys mean what they mean for any
  supplier (SPEC-002 §2.5).
- **Every default is data, and every default has an explicit override.** A key's value is taken
  from four tiers in order: override (command line, environment), project (`mcpp.toml`), machine
  (`config.toml`), engine ([SPEC-004 §2](docs/specs/manifest-semantics.md)).
- **Hermetic by default.** Toolchains, sysroots and tools come from the package indices
  (xim-pkgindex, mcpp-index); the host is used only where a row's data says so or the user opts in
  (`MCPP_ALLOW_HOST_LIBS`). A host dependency that cannot be avoided is wrapped in an xpkg.
- **Doing less is never silent.** A branch that does less because a precondition fails returns an
  error or reports through `diag::degraded()` with its impact ([`src/diag.cppm`](src/diag.cppm)).
  A refusal names the cause and the remedy and carries a reason token
  ([docs/50](docs/50-machine-output.md)).

## 3. Manifest vocabulary ([SPEC-004](docs/specs/manifest-semantics.md))

- **One conditional shape:** `[target.<selector>.<section>]`, the selector a triple or `cfg(...)`.
  A condition is never written as a trailing key or as sibling keys of a value.
- **Case by surface:** keys mcpp owns are `snake_case` (`include_dirs`, `cxx_runtime`); keys
  inherited from Cargo are `kebab-case` (`dev-dependencies`, `default-features`).
- **One concept, one spelling.** A new key reuses the name of an existing key with the same meaning;
  a published key is never renamed, and a historical inconsistency is stated as a rule.
- **A key whose effect exists on one platform carries its prefix** (`windows_subsystem`).
- An unknown key is reported, never ignored silently.

## 4. Code

- Module layout: `modules/<x>/` are independent packages linked into the binary; `src/` is the rest,
  and shrinking `src/` is the direction. Platform-specific code lives in `modules/platform/`, not
  behind `#if defined` elsewhere. A new module is wired in four places, which
  `.github/tools/check_modules_wiring.sh` checks.
- `import std;` and `import mcpp.<x>;`. An import names the module that defines a type, not one that
  forwards it.
- Follow the neighbouring code; change only what the task needs. Loops nest at most two levels.
  Files under the prepare decomposition stay under 2,500 lines and functions under ~400.
- A comment states why, from what was measured, not what the next line does.
- A path from a directory walk is never narrowed with `.string()`: compare as `path`, hash
  `u8string()`, hand to tools through `try_narrow` ([contributing](.agents/skills/mcpp-contributing/SKILL.md)).

## 5. Build and test

```bash
mcpp build                                   # self-hosted; the bootstrap version is pinned in .xlings.json
<fresh>/mcpp test                            # unit tests: tests/unit/, modules/<x>/tests/ (mtest cases)
MCPP=<abs path of fresh mcpp> bash tests/e2e/<NNN>_<sentence>.sh
```

- An e2e script is named by the behaviour it asserts, states on line 2 the capabilities it requires
  (`# requires: llvm wine`), opens with the design or issue it verifies and its criteria, and uses
  an isolated home. A script that can hold nothing on a machine prints `SKIP: <reason>` and exits
  77: the run reports a skip, never a pass. A script that skips itself on every host is listed in
  `tests/e2e/coverage-debt.tsv`, which only shrinks. Every e2e script runs on some host or is listed in
  `tests/e2e/coverage-exceptions.tsv`. The scripts run as mtest programs
  ([`tests/e2e/mtest.toml`](tests/e2e/mtest.toml)).
- A subsystem's unit tests live in `modules/<x>/tests/` and build against that package alone;
  cross-layer tests live in `tests/unit/`.
- Two canary projects (`tests/canary/c`, `tests/canary/cxx`) are built, linked statically, run,
  tested and packed for every identity of `tests/canary/matrix.tsv`, nightly and on a pull request
  labelled `ci:canaries` (`canaries.yml`). A row states `ok`, `fails` with its reason, or `n/a`; a
  cell that differs either way fails, and a change of behaviour corrects its row.

## 6. Change workflow

- Every change goes through a pull request from a branch; `main` is never pushed to directly.
- Commits, pull-request text, CHANGELOG entries and release notes are English, declarative, without
  emoji or attribution trailers. Documentation is academic and declarative, and states only what is
  implemented ([docs style](.agents/skills/mcpp-docs-style/SKILL.md)).
- One pull request does one thing; all required checks pass before merge.
- A release changes `mcpp.toml` and `modules/versioning/src/version.cppm` in one commit;
  `.xlings.json` names only a version already released, mirrored and indexed
  ([release](.agents/skills/mcpp-release/SKILL.md)).
