---
name: mcpp-runtime
description: Configure C/C++ runtime and dependency linkage in mcpp, deploy dynamic libraries, and distinguish loader search paths from public link requirements.
---

# Runtime and Linkage

## Choose the contract before a flag

`[build] cxx_runtime`, target-row `cxx_runtime` and `linkage`, dependency linkage
and profile settings affect different scopes. Resolve the effective toolchain
and runtime with `mcpp why toolchain` / `mcpp why runtime` before adding linker
escape flags. A static C++ library is not the same as a fully static application.

Use the field reference for accepted values. The C++ ABI/library model must match
prebuilt dependencies; do not substitute a libc++ package into a libstdc++ graph
just because symbol names look similar.

## Link-time and run-time paths differ

```cpp
// Inside build.mcpp
import mcpp.core;
int main() {
    mcpp::runtime_search_dir("vendor/runtime");
    mcpp::deploy("assets/config.json", "config.json");
}
```

Declare real directories and files in the user's project. `runtime_search_dir`
affects runtime lookup/staging; `link_search` affects the linker's search.
`deploy` places runtime files and does not register a C++ source.

For constructed payload directories, declare a prepare action rather than
performing installation inside the planning program. Its output directory is
read by the declared runtime/include/link consumers.

## Deployment validation

Run the artifact from the packaged or intended deployment tree, outside the
build directory. A program finding a DLL through the developer's PATH does not
prove a relocatable bundle. Inspect loader requirements and actual staged files.

`[runtime]` also models requirements, provided libraries/frameworks and adapter
contracts. Retained `library_dirs` parsing is not a reason to use it in new
examples. Prefer the current runtime search API and the field's documented
replacement for the task. `why runtime` is human output; do not invent a JSON
shape for a topic that has none.

## References

[Interface reference](references/interfaces.md).

- [04-mcpp-toml.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/04-mcpp-toml.md)
- [33-authoring-an-adapter.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/33-authoring-an-adapter.md)
- [specs/target-side.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/target-side.md)
