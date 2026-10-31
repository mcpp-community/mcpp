---
name: mcpp-basics
description: Install mcpp and create, build, and run a first C++ module project. Use focused skills for advanced configuration or integration protocols.
---

# Start a Project with mcpp

Use the project's installed mcpp when one is pinned. Check `mcpp --version`
before selecting syntax; this suite was source-reviewed against 2026.10.31.3.

## First working project

Install through xlings when mcpp is absent:

```sh
xlings install mcpp -y
mcpp --version
mcpp new hello
cd hello
mcpp build
mcpp run
mcpp test
```

`new` supplies a manifest, a program and a smoke test. Do not add an explicit
target just to repeat the inferred `src/main.cpp` target. Initial builds may
provision a toolchain and dependencies; they are not inherently offline.

The Unix release install script is not a Windows installer. On Windows use
PowerShell for the xlings command, reopen the terminal if PATH changed, and
check the resolved executable before debugging a different installation.

## Minimal manifest

```toml
[package]
name = "hello"
version = "0.1.0"
standard = "c++23"
license = "Apache-2.0"
```

Use `mcpp run -- argument` to pass program arguments. Check the exit status and
actual program output; a created directory alone does not prove a working build.

## Choose the next task

- Manifest or product: `mcpp-manifest`, `mcpp-targets`.
- Dependencies and features: `mcpp-dependencies`, `mcpp-features`.
- Build/run selection: `mcpp-build-and-run`; tests: `mcpp-tests`.
- Automation output: `mcpp-machine-interface`.

## Boundaries

Do not copy compiler versions from examples as universal defaults. The default
depends on the host, available providers, target row and project overrides.
An unavailable toolchain, registry or runner is a prerequisite failure, not
evidence that the C++ source is incorrect.

## References

- [01-getting-started.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/01-getting-started.md)
