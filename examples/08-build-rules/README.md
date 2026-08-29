# 08 — build rules as packages

Two rule packages and one project that uses both. Together they cover the three
`mcpp::action` roles that had no consumer in the ecosystem when they were
written: `check`, `object`, and — through `tidy`'s own dependency — a rule that
depends on another rule.

```
rules-tidy/    role = "check"   — analysis that runs beside the compile
rules-embed/   role = "object"  — a generated object joins the link
app/           uses both, by path
```

Run it:

```sh
cd app && mcpp build && mcpp run
```

## What each one demonstrates

**`rules-embed`** turns a file into a linkable object. The role exists because
the alternative — naming a pre-built object in `[build].ldflags` — puts a
string in the link command rather than a file in the graph, so nothing tracks
it and editing the input reports `ninja: no work to do`.

It deliberately does **not** call `.target(...)`. With no target the outputs
attach to every image the package produces, test binaries included. Naming one
instead makes `mcpp build` succeed and `mcpp test` fail with an undefined
symbol on the very symbol the action exists to provide.

**`rules-tidy`** declares a `check` action per source file. A check's output is
a stamp and the command has to create it, which is the ergonomic gap that makes
this worth packaging: without a rule, every project writes the same wrapper
script. `${mcpp.compile_db}` is the path clang-tidy's `-p` wants.

Checks run **beside** compilation by default. Serialising the whole build
behind a linter costs more than it saves, and a failing check fails the build
either way; `blocking = true` is for the case where a failure means the compile
was wasted.

**`rules-tidy` depends on `rules-embed`** through its own
`[build-dependencies]`, which is what lets one rule reuse another instead of
copying it. The consumer cannot import `rules-embed` through that edge — build
time provisions cross one further edge only when the edge says `reexport`.
