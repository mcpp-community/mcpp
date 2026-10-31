---
name: mcpp-targets
description: Configure mcpp product targets, library interfaces, executable entries, resources, exports, and platform-specific product properties.
---

# Product Targets

## Declare only the products needed

```toml
[targets.server]
kind = "bin"
main = "src/server.cpp"

[targets.client]
kind = "bin"
main = "src/client.cpp"
required_features = ["client"]
```

`required_features` gates a product; it does not activate those features in an
ordinary build. Explicitly activate them when the product is requested.

## Kinds and entries

| Kind | Use |
|---|---|
| `bin` | Executable product |
| `lib` | Static library product |
| `shared` | Shared library product |
| `app` | User-launchable product; Android uses a shared-library shape |
| `entry` | Reusable entry translation unit, selected through `main-from` |

`[targets.<name>]` names a product. `[target.<selector>]` conditions machine
configuration; these singular/plural tables are different axes.

For reusable libraries, declare the public module surface with `[lib]` and
`[modules]` rather than exposing every implementation file. Packaging must
preserve the interface closure in source and prebuilt forms.

## Platform-specific settings

Use target keys rather than graph-wide linker flags for executable-only
properties: `windows_subsystem`, `windows_entry`, `windows_code_page`,
`windows_long_paths`. Library/executable applicability is validated.
`soname` names an ELF shared-library ABI file, not an arbitrary path.
Use `exports` / `windows_auto_export` according to the target's export contract.
`start` is the freestanding image entry symbol; it is not the C++ function `main`.

For multiple products, entry-specific `defines`, `cxxflags` and `cflags` do not
replace package-wide inputs. Put resource scripts in `[resources] files`; test
code-page settings belong under `[test]`.

## Validate the actual consumer

Build every gated product you changed. For shared libraries, test a separate
consumer package that loads or links the library; an executable in the same
package does not prove that it consumes that package's shared artifact.

## References

[Interface reference](references/interfaces.md).

- [04-mcpp-toml.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/04-mcpp-toml.md)
- [12-binary-distribution.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/12-binary-distribution.md)
- [specs/library-interface.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/library-interface.md)
