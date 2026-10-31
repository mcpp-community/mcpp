---
name: mcpp-publishing
description: Prepare mcpp library releases and index descriptors, inspect xpkg and SBOM output, and validate source/prebuilt consumer interfaces before publication.
---

# Publishing Packages

## Prepare without publishing

```sh
mcpp publish --dry-run
mcpp emit xpkg -o xpkg.lua
mcpp xpkg parse xpkg.lua --format json
mcpp emit sbom
```

Dry-run and emit are review steps; `publish` performs an external publication
workflow. Confirm the requested repository/index and release intent before
performing that external action. Do not bypass a dirty-tree check to hide
uncommitted sources.

## Public library surface

Declare the package identity, version, license and repository. State exported
modules and the required source interface closure, including partitions and
implementation dependencies. Inspect feature gates in both default and explicit
forms. Publishing every source file is not a substitute for a valid interface.

## Source and prebuilt forms

`pack` for a library includes the public interface and compiled products. Check
the ABI tag and target/runtime/dialect requirements. Prebuilt objects do not
make BMIs interchangeable across compiler releases or language modes.

Test another package importing the released interface, once from source and
once from the intended prebuilt form. Do not validate solely by rebuilding the
publisher's workspace where private paths remain accessible.

## Descriptor and index

Inspect generated xpkg rather than hand-inventing its resolver grammar. The
legacy `xpkg parse --json` payload is distinct from `--format json`'s envelope.
Use an exact source revision and correct namespace/registry identity. SBOM
describes resolved components; verify the supported format in command help.

This skill covers user packages, not releasing the mcpp executable itself.
mcpp's own contribution/release procedures remain in the repository's existing
maintenance skills and apply when changing that project.

## References

[Interface reference](references/interfaces.md).

- [11-publishing-a-library.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/11-publishing-a-library.md)
- [12-binary-distribution.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/12-binary-distribution.md)
- [specs/package-identity.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/package-identity.md)
- [specs/library-interface.md](https://github.com/mcpp-community/mcpp/blob/ce4dd4b001161f6635bec2a901b18381d6c6548c/docs/specs/library-interface.md)
