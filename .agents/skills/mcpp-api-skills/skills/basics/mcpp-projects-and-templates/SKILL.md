---
name: mcpp-projects-and-templates
description: Create mcpp projects with new and package-provided templates, list template names, and validate generated files.
---

# Projects and Templates

## Create and inspect

```sh
mcpp new hello
mcpp new --list-templates mcpplibs.mwrap.opencv@5.0.0.1
mcpp new vision --template mcpplibs.mwrap.opencv@5.0.0.1
```

The last two commands are selector examples, not a guarantee that this version
is currently published in every configured index. Confirm the package first.
Template selectors are `[namespace.]name[@version][:template]`. A package with
one default template need not specify the suffix. List names before guessing.

## Validate the result

Inspect the generated manifest and source, then run `mcpp build`, `mcpp run`
and `mcpp test` in the new project. A template that renders successfully can
still contain an invalid dependency or need an unavailable device.

For templates authored in a package, `{{self.name}}` denotes its complete
package selector. Keep it as one quoted TOML dependency key; do not wrap an
already namespaced selector in a second namespace table.

Do not overwrite an existing project to retry template discovery. Select a
new output directory and keep the user's existing files.

## Boundaries

Package templates may fetch packages. A template's existence does not establish
that its selected feature, runtime backend or device is available. For wrappers,
import names and C++ namespaces remain separate: an mwrap module may export the
upstream namespace. Macros do not propagate through module imports.

## References

[Interface reference](references/interfaces.md).

- [01-getting-started.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/01-getting-started.md)
- [03-examples.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/03-examples.md)
