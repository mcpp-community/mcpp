# 90 — Building from Source and Contributing

**Reader:** a contributor who will build and change mcpp itself.

**The question this chapter answers:** how is mcpp built from source, how are
its own tests organised, and what does a contribution have to satisfy.

**Not here:** how a user builds their own project, which is
[01 — Getting Started](01-getting-started.md), and how a release is cut, which
is [92 — Releasing mcpp](92-release.md).

> mcpp is self-hosting — mcpp builds mcpp from source using mcpp itself.
> Any environment that already has a working mcpp binary can build from source.

## Prerequisites

Follow [01 — Getting Started](01-getting-started.md) to install a working copy of mcpp, then clone the repository:

```bash
git clone https://github.com/mcpp-community/mcpp
cd mcpp
```

## Building and Testing

```bash
mcpp build              # compile the current source with the existing mcpp → ./target/.../bin/mcpp
mcpp run -- --version   # run the artifact you just built
mcpp test               # build and run C++ tests discovered under tests/**/*.cpp (including tests/unit)
```

`mcpp test` does not run the shell end-to-end suite under `tests/e2e/`; run
those separately against the freshly built binary.

The first build automatically fetches the default toolchain; see [20 — Toolchain Management](20-toolchains.md) for details.

To produce a fully static binary identical to a release (the path taken by `release.yml`):

```bash
mcpp build --target x86_64-linux-musl
# → target/x86_64-linux-musl/.../bin/mcpp is a fully static ELF
```

## Source Layout

mcpp is a **workspace**. `modules/` holds nine packages that the engine imports;
`src/` is the engine itself.

```
modules/                  workspace members, imported by src/
├── manifest/             manifest and descriptor parsing
├── platform/             operating-system abstraction
├── toolchain-model/      triples, dialects, fingerprints, the link model
├── buildmcpp/            the build.mcpp contract: protocol, directives, provisions
├── source-kind/          source-file role classification
├── versioning/           this binary's version, and SemVer requirements
├── dyndep/               ninja dyndep emission
├── libs/                 vendored text-format parsers
└── log/                  leveled logging

src/
├── main.cpp              entry point
├── cli.cppm  cli/        command dispatch and the commands
├── build/                build orchestration and the ninja backend
├── modgraph/             P1689 module scanning and the dependency graph
├── pm/                   the resolver and the package-management commands
├── toolchain/            detection, fingerprinting, the std module
├── pack/  publish/       mcpp pack, mcpp publish and xpkg generation
├── fetcher/  fallback/   download, installation, fallback resolution
├── bmi_cache/            the cross-project BMI cache
├── runtime/  xlings/     the runtime contract and the xlings bridge
├── freestanding/         bare-metal targets, link line and runner
└── scaffold/             `mcpp new` and templates

tests/
├── unit/                 108 C++ tests, discovered by `mcpp test`
└── e2e/                  370 shell scripts against a real binary
```

## Test Organization

Two layers, and they answer different questions.

**Unit tests** (`tests/unit/`, 108 files) are C++ programs `mcpp test`
discovers. They exercise a module's contract with no binary and no filesystem
state.

**End-to-end tests** (`tests/e2e/NN_<name>.sh`, 370 files) run a real `mcpp`
binary against a real project. `run_all.sh` is the CI entry point. `mcpp test`
does **not** run them.

```bash
MCPP=<fresh-mcpp-binary> bash tests/e2e/02_new_build_run.sh
```

**A capability gate decides which run.** The first lines of an e2e script
declare what it needs, and a runner without that capability skips it:

```bash
#!/usr/bin/env bash
# requires: elf gcc
```

`gcc` (80 scripts), `elf`, `unix-shell`, `llvm`, `jq` and `fresh-sandbox` are
the ones in use. A script that requires a capability no CI job provides **never
runs anywhere**, and its greenness means nothing — check that some job supplies
what a new script asks for.

## Writing a check that measures something

The most transferable rule in this repository, and the one that fails silently
when it is skipped:

> **After writing a check, remove the fix and run it once.** A check that has
> never been seen to fail is not known to measure anything.

Three shapes it catches, all of which have shipped here at least once:

| shape | what it looks like |
|---|---|
| the criterion never runs | a test gated on a capability no job provides |
| the criterion cannot fail | a substring search satisfied by any wording |
| the criterion measures the wrong object | a fixture whose directory accumulates across runs, so the search answers about an earlier build |

State the denominator too. "Every host in the table was scanned" is a check;
"the scan found nothing" is not, because an empty enumeration also finds
nothing.

## The checks CI runs beside the tests

`.github/tools/` holds eighteen scripts. Four are worth knowing before a first
PR:

| script | what it refuses |
|---|---|
| `check_docs_style.sh` | question headings, second person in a reference chapter, a 简体中文 page whose heading structure has fallen behind |
| `check_docs_structure.sh` | a chapter citing a design record, a `docs/NN-*.md` path that does not resolve, a translation missing a table |
| `check_version_pins.sh` | a version written in one place and not the others |
| `check_modules_wiring.sh` | a workspace member wired into some of the three places that must know about it and not the others |

## Issue and PR Guidelines



### Issues

File issues at [github.com/mcpp-community/mcpp/issues](https://github.com/mcpp-community/mcpp/issues), ideally including the following:

- The full output of `mcpp self env`
- The full output of the failing command (`MCPP_LOG_LEVEL=debug` gives more detail)
- Your operating system, distribution, and glibc version (check with `ldd --version`)

### Pull Requests

mcpp is in early iteration and its interfaces may change. Before submitting a PR, please note:

1. For changes touching the CLI or the `mcpp.toml` schema, open an issue first to align on direction.
2. Keep each PR focused on a single change; write commit titles in English imperative form (`fix: ...` / `feat: ...`).
3. For behavior changes or test documentation, run `mcpp test` and the relevant
   E2E scripts against a fresh binary before submitting. For documentation-only
   changes, recheck the examples and links; use `gh pr checks <pr-number>` for
   the PR's actual required checks.

## Community Resources

- [Community forum](https://forum.d2learn.org/category/20)
- Chat group QQ: 1067245099
- [mcpp-index](https://github.com/mcpplibs/mcpp-index) — the default package index
- [mcpplibs](https://github.com/mcpplibs) — the companion collection of modular C++ libraries

