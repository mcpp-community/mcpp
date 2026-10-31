---
name: mcpp-build-programs
description: Write mcpp build.mcpp configuration programs using the bundled public module, declared rerun inputs, diagnostics and phase-specific contracts.
---

# Build Programs

Use a build program when declarative configuration cannot express a probe or
graph extension. Do not add one to repeat constants already expressible in TOML.

## Minimal program

```cpp
// build.mcpp at the package root
import mcpp.core;
int main() {
    mcpp::define("APP_CONFIGURED=1");
    mcpp::rerun_if_changed("config.txt");
    mcpp::rerun_if_env_changed("APP_MODE");
}
```

Provide the referenced input or choose an existing project input. The engine
supplies the public module; `import mcpp;` is an equivalent entry. Do not import
internal `mcpp.build.*` modules into a user's build program.

## Separate configuration from construction

The program configures the graph. Code generation, compilation, installation
and validation work belong in declared actions. Do not write generated sources
into `src/` during planning. A source action declares outputs before they exist;
a prepare action supplies an installation directory; a check action validates.

## Rerun contract

Declare files whose contents affect configuration and environment variables
whose values are read. `rerun_if_changed_glob` tracks changes to the matching
path set; it does not replace content dependencies on those files.

The program has a bounded run time (600 seconds by default at this baseline)
and distinct ordinary/toolchain/platform phases. Inspect `phase()` and use only
the directives allowed in the current phase. Protocol version and cache epoch
are different values, not the mcpp release number.

## Diagnostics

Use `warning` or structured `report` for messages that must be visible on a
successful run and replay from cache. Plain captured stdout/stderr is not the
same user-facing diagnostic channel. Return nonzero for a failed probe that
prevents a valid graph, and state the failed prerequisite.

The build program cannot add registry dependencies dynamically. Keep package
requirements declarative, including conditional and optional requirements.

## References

- [30-build-mcpp.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/30-build-mcpp.md)
- [specs/build-plugins.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/build-plugins.md)
