# `build.mcpp` — a native build program

**English** | [简体中文](zh/07-build-mcpp.md)

Most projects need nothing more than `mcpp.toml`. When you need build-time logic —
probe the host, generate a source, decide a flag from the environment — put a
`build.mcpp` in your project root. It is the mcpp analog of Zig's `build.zig` and
Cargo's `build.rs`, but written in **C++**: no second language, and it dogfoods
mcpp itself.

mcpp compiles `build.mcpp` with your toolchain and runs it **before** the main
build. The program talks to mcpp by printing `mcpp:` directives to stdout; those
directives augment the build.

## Quick example

```cpp
// build.mcpp
#include <cstdio>
#include <fstream>

int main() {
    // Generate a source the main build will compile + link.
    std::ofstream("src/generated.cpp") << "const char* banner() { return \"hi\"; }\n";

    std::puts("mcpp:generated=src/generated.cpp");   // add it to the build
    std::puts("mcpp:cxxflag=-DHAVE_BANNER=1");        // define a macro for all C++ TUs

    if (std::getenv("USE_FAST")) std::puts("mcpp:cxxflag=-DFAST_PATH=1");
    std::puts("mcpp:rerun-if-env-changed=USE_FAST");  // re-run me when USE_FAST changes
    return 0;
}
```

```bash
mcpp build      # compiles + runs build.mcpp, then builds the project
```

## Directives

Print these to stdout (one per line). Any line that does not start with `mcpp:`
is ignored, so you can freely log diagnostics.

| Directive | Effect |
|---|---|
| `mcpp:cxxflag=<flag>`              | add `<flag>` to the C++ compile flags |
| `mcpp:cflag=<flag>`                | add `<flag>` to the C compile flags |
| `mcpp:link-lib=<name>`             | link `-l<name>` |
| `mcpp:link-search=<dir>`           | add a library search dir (`-L`; relative dirs resolve against the project root) |
| `mcpp:cfg=<name>`                  | define `-D<name>` for both C and C++ |
| `mcpp:generated=<path>`            | add a generated source (relative to the project root) to the build |
| `mcpp:rerun-if-changed=<path>`     | re-run `build.mcpp` when this file changes |
| `mcpp:rerun-if-env-changed=<VAR>`  | re-run `build.mcpp` when this env var changes |

The program **requests** build edges (flags, libraries, sources). It cannot add a
registry dependency — keep your dependency graph declarative in `mcpp.toml`
(including platform-conditional `[target.windows.dependencies]`). `build.mcpp`
is for *leaf* decisions: flags, codegen, link requirements.

## Typed API: `import mcpp;` (recommended)

Instead of printing raw strings you can write `build.mcpp` **modules-first** —
`import mcpp;`, no `#include`, no `import std;`. The `mcpp` module is bundled in the
mcpp binary (so it always matches your mcpp's protocol) and is compiled on demand;
its functions just emit the directives above:

```cpp
// build.mcpp
import mcpp;

int main() {
    mcpp::cxxflag("-DHAVE_BANNER=1");
    mcpp::link_lib("m");                 // -lm
    mcpp::link_search("vendor/lib");     // -L…
    mcpp::define("HAVE_FEATURE");         // == mcpp:cfg= → -DHAVE_FEATURE
    mcpp::generated("src/gen.cpp");
    mcpp::rerun_if_changed("config.h");
    mcpp::rerun_if_env_changed("USE_FAST");
}
```

| Function | Emits |
|---|---|
| `mcpp::cxxflag(s)` / `mcpp::cflag(s)` | `mcpp:cxxflag=` / `mcpp:cflag=` |
| `mcpp::link_lib(s)` / `mcpp::link_search(s)` | `mcpp:link-lib=` / `mcpp:link-search=` |
| `mcpp::define(s)` | `mcpp:cfg=` (i.e. `-D<s>`) |
| `mcpp::generated(p)` | `mcpp:generated=` |
| `mcpp::rerun_if_changed(p)` / `mcpp::rerun_if_env_changed(v)` | the matching `rerun-*` directives |

If your `build.mcpp` also needs to *write* a generated file, mix in a textual
`#include <fstream>` — that's fine; only `import std;` is unnecessary. The raw
stdout protocol above remains the low-level substrate; `import mcpp;` is the typed
layer over it.

## Incremental: declared inputs (no needless re-runs)

mcpp does **not** re-run `build.mcpp` on every build. It caches the program's
directives and re-runs only when something it depends on changed:

- the `build.mcpp` source itself,
- the toolchain,
- any file you declared with `rerun-if-changed`,
- any env var you declared with `rerun-if-env-changed`,
- (or a `generated` output went missing).

So **declare your inputs**: if your program reads `config.h` or the `USE_FAST`
variable, emit `mcpp:rerun-if-changed=config.h` / `mcpp:rerun-if-env-changed=USE_FAST`.
This replaces the old "process exited 0, so assume it's fine" guesswork with an
explicit input/output contract — incremental builds stay correct.

When nothing changed you'll see `build.mcpp up to date (cached)`; otherwise
`build.mcpp compiling` / `running`.

## Notes & limits

- **Runs on the host.** `build.mcpp` compiles and runs with the host toolchain.
  Under a cross build (`mcpp build --target <triple>`) it is **skipped with a
  warning** for now (host-toolchain-for-cross is a planned follow-up). Gate
  *dependencies* on the target with `[target.'cfg(...)']` tables instead — those
  evaluate on the resolved target. See [05 - mcpp.toml Manifest Guide](05-mcpp-toml.md).
- **CWD is the project root**, so relative paths (`src/generated.cpp`) land where
  you expect.
- A non-zero exit from `build.mcpp` aborts the build and prints its output.
