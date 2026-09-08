# 00 — How mcpp Works

**Reader:** anyone, before anything else. Every other chapter assumes this one.

**The question this chapter answers:** what are the moving parts, and which one
is failing when something fails.

mcpp is three tools in one program — a build system, a package manager, and a
toolchain provisioner. That is unusual, and it is the reason a reader who starts
with a command reference finds a list of verbs with nothing to attach them to.
This chapter is the model. It names no field and no flag; those are the
reference chapters, which this one makes readable.

**Not here:** every field, flag and command. A model that also tries to be a
reference is neither, so each noun below names the chapter that owns it.

Next: [01 — Getting Started](01-getting-started.md) puts a program on the screen.

## The five nouns

Everything in this documentation is about five things and the seams between
them.

### package

A directory with an `mcpp.toml`. It states a name, a version, what it is built
from and what it depends on. Its identity is the pair `(namespace, name)` — not
its directory, and not its file name — which is why two packages called
`cmdline` from different namespaces can coexist.

A package is also the unit of everything else: the unit that is published, that
is cached, that declares features, and that a build program belongs to.

Reference: [04 — The mcpp.toml Manifest](04-mcpp-toml.md).

### graph

A build is a graph. Sources become objects, objects become an artifact, and
C++20 module interfaces add edges between sources because one unit must be
compiled before another that imports it. mcpp computes that ordering by scanning
the sources; nothing in the manifest states it.

Two things extend the graph rather than sitting outside it: a **build program**
(`build.mcpp`) declares additional edges, and a **rule package** supplies those
declarations for a whole class of projects. Both add nodes to the same graph;
neither is a pre-build script.

Reference: [30 — Build Programs](30-build-mcpp.md).

### toolchain

The compiler mcpp uses is a **payload it installs and pins**, not a program
found on the machine. This is the property most other build systems do not have,
and it decides the shape of much else: a build is reproducible because the
compiler is part of what was resolved, and a project can state which compiler it
needs rather than documenting it in a README.

Reference: [20 — Toolchain Management](20-toolchains.md).

### target

The machine the artifact runs on. It is named by a triple and it is not the
host: a build on Linux can produce a Windows executable, a bare-metal image, or
an object for a GPU. Everything conditioned on "where this runs" hangs off the
target, and the manifest can say "only there" without a second manifest.

Reference: [21 — The Target Triple](21-the-target-triple.md).

### index

Where packages come from. An index holds **descriptors** — a package's identity,
its versions, and where each version's source or artifact is fetched from. A
descriptor is data; mcpp is the program that reads it.

Reference: [11 — Publishing a Library to mcpp-index](11-publishing-a-library.md).

## What a build does, end to end

```
  mcpp.toml ──▶ resolve ──▶ provision ──▶ scan ──▶ compile ──▶ link
     │            │             │           │         │          │
  package     index +       toolchain    graph    toolchain   artifact
              versions      + tools      edges

              ╰── three seams a first build crosses ──╯
```

**Seam 1 — manifest to resolution.** What the manifest names becomes a set of
exact versions. A failure here is about the index or a version constraint, and
nothing has been compiled.

**Seam 2 — resolution to environment.** What was resolved becomes payloads on
disk: the toolchain, and any tool a package declared. A failure here is about a
download, a platform that has no such payload, or a version floor.

**Seam 3 — sources to graph.** The sources are scanned for `import` and the
edges are computed. A failure here names a module, not a file.

Compilation and linking come after all three, which is why "it did not compile"
is one of four quite different situations.

## Where the state lives

Nothing mcpp writes is hidden, and each store answers a different question.

| store | scope | holds | emptied by |
|---|---|---|---|
| `target/<triple>/<fingerprint>/` | one project | objects, module interfaces, the artifact | `mcpp clean` |
| the build cache | the machine | compiled dependencies and `std` | `mcpp cache gc` |
| the package store | the machine | toolchain payloads and declared tools | the package manager |
| `mcpp.lock` | one project, checked in | what a resolution produced | rewritten by `mcpp update` |

`mcpp self env` prints where each of these is on this machine.

The fingerprint in the build directory is why two configurations do not fight:
a debug build, a release build and a cross build occupy different directories
and none invalidates the others.

## Which noun a failure is about

The single most useful thing this model buys. A message names a noun, and the
noun names the chapter.

| a message about | the noun | where to look |
|---|---|---|
| a package name, a version, or "no candidate" | index | [11](11-publishing-a-library.md), [05](05-dependencies.md) |
| a download, a payload, or a version floor | toolchain | [20](20-toolchains.md), [23](23-the-project-environment.md) |
| a triple, or "unsupported target" | target | [21](21-the-target-triple.md) |
| a module that cannot be read or is not provided | graph | [30](30-build-mcpp.md) |
| a compile or link error in a file of the project | none of them | the compiler's own message |

The last row is the useful one: when a compiler error is about the code, none of
mcpp's parts is involved, and reading mcpp's documentation will not help.

## What this chapter leaves out

Everything operational. No field of the manifest, no flag, and no command other
than `mcpp build` appears here on purpose — a model that also tries to be a
reference is neither. Each noun's section names the chapter that owns it.
