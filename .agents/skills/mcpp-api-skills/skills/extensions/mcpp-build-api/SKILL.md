---
name: mcpp-build-api
description: Look up the actual bundled mcpp.core public C++ functions, action methods, diagnostics and build-program directive contracts; excludes internal engine modules.
---

# Public Build-Program API

Use [the interface reference](references/interfaces.md) for source-extracted
signatures and directive scope. Use `mcpp-build-programs` for lifecycle and
`mcpp-build-actions` when constructing graph work.

## Public entry point

```cpp
import mcpp.core;
int main() {
    mcpp::define("APP_HAS_CONFIG=1");
    mcpp::rerun_if_env_changed("APP_MODE");
}
```

`import mcpp;` is equivalent. The module is materialized from the engine's
bundled source. Do not hard-code its store path or depend on an internal
`export module` merely because it exists in the mcpp checkout.

## API groups

| Need | Public surface |
|---|---|
| Compile inputs | cxxflag, cflag, define, include_dir, include_dir_after |
| Link requirements | link_lib, link_search, link_script, link_flag, link_start |
| Graph work | action, roles, generated/source declarations |
| Incremental configuration | rerun_if_changed, rerun_if_changed_glob, rerun_if_env_changed |
| Diagnostics/probes | warning, diagnostic, report, fact, floor |
| Configuration facts | target/host/profile and toolchain/runtime helpers |
| Dependencies/tools | dep_dir, dep_linkage, dep_bin, xpkg helpers |
| Runtime/device | deploy, runtime_search_dir, runner, runner_longlived, run_exclusive |
| Platform/package | toolchain, link_driver, link_input, artifact_name, seal, run_host, pack helpers |

Use exact signatures rather than inventing overloads. The simple wrappers take
C strings; they do not imply `std::string`, path or list overloads. Empty lookup
results require checking. Helpers with trailing underscores are implementation
machinery even when visible in the generated source.

## Scope and versions

Compile directives are generally package-private; link requirements can reach
consumers. Do not use computed includes to widen a public library interface.
The current directive protocol is 17; a newer API may be absent in an older
engine at compile time, before any protocol handshake. Check the mcpp floor.

Facts must describe measured host conditions. Declare the inputs that can change
them; a cached fact must not outlive the machine state it describes. A floor
without a matching fact is not proof that the requirement was verified.

## References

[Interface reference](references/interfaces.md).

- [30-build-mcpp.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/30-build-mcpp.md)
- [specs/build-plugins.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/build-plugins.md)
