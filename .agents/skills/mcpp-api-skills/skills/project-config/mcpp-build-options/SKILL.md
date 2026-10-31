---
name: mcpp-build-options
description: Set mcpp build and profile options with correct package, usage, and graph scope, including dialect flags and workspace merge behavior.
---

# Build Options and Profiles

## Select a profile

```sh
mcpp build --profile release
mcpp run --release
```

Ordinary builds default to `dev` unless `[build] default-profile` says otherwise;
pack has its own release-oriented default. Read the root manifest before
claiming what an unqualified command does.

```toml
[build]
default-profile = "dev"
jobs = "auto"
cxxflags = ["-Wall"]
defines = ["APP_MODE=1"]
include_dirs = ["src/detail"]
private_include_dirs = ["src/detail"]

[profile.release]
cxxflags = ["-O2"]
```

Preserve argv token boundaries. An option requiring two tokens needs two array
entries. Compiler-specific flags need matching target/toolchain conditions;
do not unconditionally send GCC flags to an MSVC-family driver.

## Scope is part of the option

- Package inputs affect this package's units.
- Usage inputs propagate from a dependency to consumers.
- Configuration inputs are graph-wide and read from the build root.
- Metadata does not enter compiler commands.

`include_dirs` adds header search paths for this package and its consumers.
`private_include_dirs` selects entries of `include_dirs` that stop at this
package's boundary. It does not add search paths: a private header directory
must appear in both lists, as in the example above. Matching is performed after
glob expansion. A private entry absent from `include_dirs` withholds nothing.

The local reference describes which inputs can be shared or conditional.
Only supported keys participate in `[workspace.build]`; member scalars,
lists and defines follow their respective merge rules.

## Dialect and standard library BMIs

Use `[package] standard`, not a `-std=` flag. Use `dialect_cxxflags` for flags
that alter the graph's language/library dialect, including `-fno-exceptions`
when importing `std`. Ordinary `cxxflags` cannot safely change a std BMI's
dialect. Effective profile and conditional flags are checked too.

`dialect_cxxflags` comes from the root/shared/conditional configuration, not a
dependency's opinion. `bmi_schedule`, cache policy and runtime linkage are
configuration decisions; read their target constraints before changing them.
Prefer defaults until a measured task requires an override.

## References

[Interface reference](references/interfaces.md).

- [04-mcpp-toml.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/04-mcpp-toml.md)
- [specs/manifest-semantics.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/manifest-semantics.md)
