---
name: mcpp-toolchains
description: Inspect, install and choose mcpp toolchains with per-host and target overrides, local compiler paths, runtime constraints and provider diagnostics.
---

# Toolchain Selection

## Inspect and select

```sh
mcpp toolchain list
mcpp toolchain list --format json
mcpp toolchain install llvm 23.1.3
mcpp build --toolchain llvm@23.1.3
```

These are reviewed selector examples, not a promise of payload availability on
every host. Consult the installed toolchain/target status and resolved decision
before picking a version. Use `toolchain default SPEC` for a persistent default,
and `--toolchain` for one invocation.

```toml
[toolchain]
default = "gcc@16.1.0"
windows = "llvm@23.1.3"
macos = "llvm@23.1.3"

[target.x86_64-linux-musl]
linkage = "static"
```

Do not infer host availability from this illustrative cross-platform table.
Native Windows LLVM may require an MSVC installation; a MinGW ABI is a different
configuration, not a substitute flag for the same binary interface.

## Diagnose the resolved configuration

```sh
mcpp why toolchain
mcpp why toolchain --format json
mcpp build --print-fingerprint
```

Compiler family/version, sysroot, C++ library, ABI and target all matter to BMI
identity. Do not reuse cached interfaces merely because both drivers are Clang.
Version selection is not sufficient evidence that headers, linker and runtime
form a compatible target toolchain.

A local installation is a table-valued selector, for example
`default = { path = "/opt/gcc", family = "gcc" }` under `[toolchain]`.
Use the actual installation root. For a machine-local override, pass
`--toolchain "path:<directory>"` for one invocation or set `MCPP_TOOLCHAIN` in
the current process rather than committing that machine's path to the project.

A computed selection uses `default = { configure = "build.mcpp" }` and runs in
the toolchain phase. Use `mcpp-toolchain-providers` for bootstrap and tool roles.

## Build-tool discovery

The compiler and Ninja must both be reachable. If compiler detection succeeds
but the build reports `cannot start 'ninja'`, inspect the active home with
`mcpp self env`, locate its installed Ninja and run that executable's
`--version` by absolute path. Check whether the invoking process can resolve it.

With a local compiler outside the provider store, mcpp 2026.10.31.3 can fall
back to the bare `ninja` command instead of the installed home tool. If Ninja
exists but is absent from PATH, prepend its containing directory to PATH for
the current process and retry. Preserve the existing PATH; do not copy Ninja
into the compiler tree or change the persistent user/system PATH.

Removal changes toolchain state. Prefer a per-invocation selection when only
one project needs a different compiler; do not globally switch unrelated work.

## References

[Interface reference](references/interfaces.md).

- [20-toolchains.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/20-toolchains.md)
- [specs/toolchain-management.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/toolchain-management.md)
