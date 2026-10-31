---
name: mcpp-cxx-modules
description: Configure module source discovery, public exports, scan overrides, generated interfaces, and imports of wrapped C/C++ libraries in mcpp.
---

# C++ Modules and Source Discovery

## A minimal module project

```cpp
// src/greeting.cppm
export module greeting;
export int answer() { return 42; }
```

```cpp
// src/main.cpp
import greeting;
int main() { return answer() == 42 ? 0 : 1; }
```

Run `mcpp build` and `mcpp run`. Avoid manually ordering compiler invocations;
mcpp derives module dependencies and BMI edges from the graph.

## Configure discovery deliberately

`[build] sources` chooses source paths. `[build] module_extensions` extends
module-interface suffixes; it does not register a foreign-language compiler.
`[modules] sources`, `exports`, `strict` and scan overrides have separate roles;
consult the local field reference before using them to narrow an interface.

Use source-set exclusions to remove foreign platform implementations. Do not
list an unknown extension and expect it to disappear: unclassified sources
are refused. A rule feature can add device extensions, but cannot take over
an extension already owned by the engine.

## Generated modules

Declare generated module names and imports during planning with an action's
`provides()` and `imports()`. The source need not exist yet, but the graph must
know its interface identity. Use `mcpp-build-actions` for the complete workflow.

## Library boundaries

Distinguish public modules from implementation units and partitions. A BMI is
not a portable replacement for the source interface: compiler, dialect, flags
and runtime identity matter. Do not copy arbitrary BMIs across toolchains.

For traditional libraries, a published mwrap package can provide imports while
retaining the upstream C++ namespace. Confirm actual package versions and
features in the index. Module imports do not export preprocessor macros; a
`.macros` module can only map constructs whose meaning can be preserved.

If scanning assertions are added, check both a build and an importing consumer.
Static source inspection alone does not establish that the module builds.

## References

[Interface reference](references/interfaces.md).

- [04-mcpp-toml.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/04-mcpp-toml.md)
- [11-publishing-a-library.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/11-publishing-a-library.md)
- [specs/library-interface.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/library-interface.md)
