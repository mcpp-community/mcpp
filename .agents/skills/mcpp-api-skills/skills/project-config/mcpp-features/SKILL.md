---
name: mcpp-features
description: Define and activate mcpp features, optional dependencies, feature forwarding and capability gates without confusing union semantics with backend selection.
---

# Features and Capabilities

## Define a small feature

```toml
[features]
default = []
logging = { defines = ["APP_LOGGING=1"] }
```

```sh
mcpp build --features logging
mcpp test --features logging
```

Features add sources, flags, defines and dependencies. A product's
`required_features` checks activation; it does not ordinarily turn features on.

## Dependency requests accumulate

A dependency table can request `features = [...]` and
`default-features = false`. The latter disables that edge's default request;
another edge may still request those features. Explicit features are not
subtracted. Do not invent a root `--no-default-features` switch.

`--features dependency/feature` targets a dependency feature. Resolve ambiguous
package identities before writing selectors. Gate an actual local package as follows:

```toml
[features]
default = []
image = []

[feature-deps.image]
codec = { path = "../codec" }
```

The codec directory must be a real package. `optional = true` is not an mcpp
dependency option. A feature table's `deps` key is reserved but not implemented;
use `feature-deps` rather than treating that retained spelling as functional.
The dependency graph stays declarative; a build program cannot mutate it.

## Rules and capability choices

Feature tables can declare `rule_module` and `device_extensions` for rules;
feature tool payloads belong to the associated feature-xlings declarations.
Keep expensive vendor dependencies behind the capability that needs them.

`provides`, `requires` and `exclusive` describe capabilities/constraints;
`--cap key=provider` selects providers where supported. A feature name and an
accelerator selection are different axes: `--accel` / `--no-accel` belong to
heterogeneous builds.

## Validate both branches

Test the default graph and the explicit feature graph. Verify selected sources
and dependency resolution, not just a macro spelling. Successfully importing
a module does not prove a window, inference backend or device path is enabled.

## References

[Interface reference](references/interfaces.md).

- [06-features-and-capabilities.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/06-features-and-capabilities.md)
- [05-dependencies.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/05-dependencies.md)
