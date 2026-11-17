# 13 — Code and Project Style

**Reader:** someone starting a module, a library or a wrapper of an existing library,
and choosing how to lay it out.

**The question this chapter answers:** which shape a module takes, where headers and
macros go, and which of those choices mcpp's build makes cheaper.

**Not here:** what mcpp does to a build, which is
[26 — Build Optimizations](26-build-optimizations.md); a library's published interface,
which is [11 — Publishing a Library](11-publishing-a-library.md); shipping a library
without its sources, which is [12 — Distributing a Prebuilt Library](12-binary-distribution.md).

The practices below are C++ modules practice. A line marked **mcpp** states what mcpp's
build adds to it; the practice holds without that.

## The shape of a module

| Shape | Use it for | An edit of a function body compiles |
|---|---|---|
| One file, bodies after `module :private;` | A module of an application, or of a library published as source | That file's fragment |
| An interface file (`.cppm`) and implementation files (`.cpp`, `module m;`) | A library shipped without its sources; a module too large for one file | The implementation file |
| An interface with partitions (`m:part`) | A module whose interface is itself large | The partition, and what imports it |
| One file, bodies in the interface | Templates and `inline` functions only | Every importer |

- Write one file with a private fragment unless something asks for more: one file to
  read, and an edit below the line reaches no importer.
  **mcpp:** the fragment is built as a unit of its own, so this holds with every compiler,
  GCC included, which does not build a fragment as written ([26](26-build-optimizations.md)).
- Ship an interface file and a prebuilt library where the sources are not published: the
  interface is the only source a consumer compiles ([12](12-binary-distribution.md)).
- A module of one file whose bodies are in the interface compiles its importers with every
  edit. **mcpp:** a build says so once when the importers are many.

## The contents of an interface

| In the interface | After `module :private;`, or in an implementation unit |
|---|---|
| Exported declarations | Definitions of functions that are not inline |
| Templates and `inline` / `constexpr` functions, with their bodies | Helpers no importer names |
| Types an importer needs complete | The definition of a type exported incomplete |

- Export the least: a name that is not exported cannot be depended on.
- Keep what the fragment reads visible to it: a helper in an unnamed namespace above the
  fragment is local to the interface. Declare it in the module (not exported) and define
  it in the fragment.
  **mcpp:** a fragment that reads such a helper is built as written, and the build names
  the line.

## Headers

- A module unit includes headers in its global module fragment and nowhere else:

  ```cpp
  module;
  #include <sqlite3.h>          // the only place this header is read
  export module store.db;
  ```

- A header library is wrapped once, in a module of its own, and every other unit imports
  that module. The wrapper exports the names with `export using`, and nothing downstream
  includes the header: its macros and its parse cost stop at the wrapper.
- A wrapper of a library is a package of its own, versioned with the library it wraps
  (`mcpplibs.mwrap.<library>` in the index). **mcpp:** `mcpp add` resolves it as any
  dependency, and its build flags travel with it.
- `import std;` replaces the standard headers in a module unit.

## Macros

A macro does not cross a module boundary: an importer never sees one. Each use has a
form that does.

| A macro used for | Write |
|---|---|
| A constant | `export inline constexpr` |
| A function-like expression | A function template, or a `constexpr` function |
| Choosing code by platform or feature | `if constexpr` over a `constexpr` fact |
| A library's configuration (`#define LIB_OPTION`) | A flag of the package that builds the library, stated once in its `mcpp.toml` |

```cpp
export module net.io;
import std;

export inline constexpr bool has_epoll = /* a fact of the target */ true;

export void poll_once() {
    if constexpr (has_epoll) { /* ... */ } else { /* ... */ }
}
```

Both branches are parsed, so the code that is not taken still has to name things that
exist; a branch that cannot be written for a target goes in a source file selected for
that target (`[target.'cfg(...)'.build] sources`, [04](04-mcpp-toml.md)).

A macro that has to exist — an assertion that names its line, a library's own API —
stays in a header, included in the global module fragment of the units that use it.

## Names and layout

- A module's name starts with the package's namespace: `acme.net.io` in package
  `acme.net`. **mcpp:** a top-level name that says nothing (`core`, `util`, `common`) is
  refused at validation.
- One module per directory level reads best: `src/net/io.cppm` provides `acme.net.io`.
- A test names the module it tests and imports it as a consumer does
  ([08](08-testing.md)).
