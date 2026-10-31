---
name: mcpp-cross-compilation
description: Build mcpp projects for another target triple or target set with correct host/target graphs, sysroot, conditional settings and execution expectations.
---

# Cross-Compilation

## State the target explicitly

```sh
mcpp build --target x86_64-linux-musl
mcpp why toolchain --target x86_64-linux-musl
```

A target row is more than an ISA. The OS/environment, ABI, runtime layers,
toolchain and platform determine what is built. Normalize through mcpp's target
model rather than hand-splitting triples into presumed positions.

```toml
[target.x86_64-linux-musl]
linkage = "static"

[target.windows.build]
defines = ["APP_WINDOWS=1"]
```

Use recognized selectors and `cfg(...)` predicates. Conditions involving a
resolved layer cannot decide the dependencies or library form that creates that
layer; such circular placement is not a useful gate.

## Keep host work native

Code generators, rule modules and tools run on the host. Product objects and
libraries target the selected machine. Use build-dependencies/tools and the
host graph; do not compile a generator for the target and then execute it here.

`MCPP_TARGET` is resolved. `MCPP_TARGET_REQUESTED` can be empty for a native
request; use the resolved target helpers in build programs.

## Platform and base

`--sysroot` / `MCPP_SYSROOT` select a target base (package, `path:...`, `none`,
or supported system form). `--platform` and `--host-platform` select different
graphs. Use `mcpp-platform-packages` for layers, tools and carriers.

Target sets expand into legs. Verify each build leg; a run picks a reachable
leg, while tests may build all and run only where reachable. For compile-only
verification use `mcpp test --target ... --no-run`. Executing a foreign artifact
requires an actual runner or target machine.

## References

[Interface reference](references/interfaces.md).

- [21-the-target-triple.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/21-the-target-triple.md)
- [22-target-side.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/22-target-side.md)
- [24-openkal-cross.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/24-openkal-cross.md)
- [specs/target-side.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/target-side.md)
