---
name: mcpp-build-actions
description: Declare incremental mcpp actions with explicit argv, files, tools, depfiles and generated module identities using the source/check/object/artifact/prepare roles.
---

# Build Actions

## Submit an explicit graph edge

The following assumes the project already has `tools/generate.py`, an input
schema and a declared python payload:

```cpp
import mcpp.core;
int main() {
    const char* python = mcpp::xpkg_program("python");
    if (!python[0]) return 1;
    mcpp::action a;
    a.id = "generate-model";
    a.role = mcpp::roles::source;
    a.input(python).input("tools/generate.py").input("schema.txt");
    a.output("generated/model.cpp");
    a.arg(python).arg("tools/generate.py").arg("schema.txt")
     .arg("generated/model.cpp").cwd(".");
    a.submit();
}
```

The generator must actually produce the declared output. Use `.env(name,value)`
and `.cwd(dir)` instead of shell prefixes. Commands are argv arrays: `&&`, `>`
and `NAME=value` are not shell syntax unless a shell is explicitly the program.

## Select the role

| Role | Contract |
|---|---|
| source | Known generated source outputs join this package's compilation |
| check | Validation; `blocking` can gate compilation |
| object | Outputs join selected link units; omitted target includes tests |
| artifact | Post-link artifact work, including packaging |
| prepare | Populates one declared output_dir used as a directory |

Declare generated module `provides` and `imports` at planning time. Names must
be known even when content is produced later. A prepare action is for directory
installation whose individual file names are not planning inputs.

## Incrementality

Include the tool binary as an input. Declare direct inputs; set `depfile` when
the command discovers additional dependencies and writes a Make-style depfile.
Keep outputs deterministic and unique. Declaration order is not a replacement
for file dependencies between actions.

Action placeholders such as `${mcpp.self}` and target-file/stage handles have
specific availability. Read [the action contract](references/action-contract.md).
The `stage` helper is public in its action use, not a general installation API.

## Validate

Run once, run unchanged, change an input, and check that only required work
reruns. Also verify missing outputs and generator failure. A successful command
that did not produce its declared result is an invalid action.

## References

[Interface reference](references/interfaces.md).

- [30-build-mcpp.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/30-build-mcpp.md)
- [specs/build-plugins.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/build-plugins.md)
