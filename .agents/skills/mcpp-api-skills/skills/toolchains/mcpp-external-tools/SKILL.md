---
name: mcpp-external-tools
description: Declare xlings tools and payloads for mcpp, apply local overrides, and resolve on-request tools without confusing installed payloads with host-built binaries.
---

# External Tools and Payloads

## Declare tools with the graph

```toml
[xlings.workspace]
python = "3.12.12"
```

Choose an actually published version for the host. Package-provided tool
requirements can be gated under `[feature-xlings.<feature>]`; root tools and
dependency tools participate in the prepared environment.

Use `[xlings.overrides]` for a local development substitution rather than
rewriting every rule that consumes the tool. Check the exact accepted override
shape in the local reference before editing it.

## Ask through the public API

```cpp
import mcpp.core;
int main() {
    const char* program = mcpp::xpkg_program("python");
    if (!program[0]) {
        mcpp::warning("python payload has no registered program");
        return 1;
    }
    mcpp::rerun_if_env_changed("APP_CODEGEN_MODE");
}
```

`xpkg_dir`, `xpkg_source` and `xpkg_program` answer different questions. Empty
answers must be handled. `dep_bin` reaches a tool compiled from a source edge;
it is not a synonym for a prebuilt xpkg program.

## On-request provisioning

With `provision = "on-request"`, declare payloads first, request needed ones with
the public request API, and end the requesting invocation when pending. mcpp
provisions the batch and reruns the program; the requesting run's configuration
is discarded. Do not submit actions and expect them to survive that run.

An action must declare the tool binary among its inputs so tool changes cause
rebuilds. Passing a program name through a guessed global PATH can select another
home's shim; use the actual public resolver for the workflow.

## References

[Interface reference](references/interfaces.md).

- [23-the-project-environment.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/23-the-project-environment.md)
- [32-authoring-a-payload.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/32-authoring-a-payload.md)
