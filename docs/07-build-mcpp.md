# `build.mcpp` — a native build program

**English** | [简体中文](zh/07-build-mcpp.md)

Most projects need nothing more than `mcpp.toml`. When build-time logic is required —
probe the host, generate a source, decide a flag from the environment — put a
`build.mcpp` in the project root. It is the mcpp analog of Zig's `build.zig` and
Cargo's `build.rs`, but written in **C++**: no second language, and it dogfoods
mcpp itself.

mcpp compiles `build.mcpp` with the project's toolchain and runs it **before** the main
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
is ignored, so diagnostics may be logged freely.

| Directive | Effect |
|---|---|
| `mcpp:cxxflag=<flag>`              | add `<flag>` to the C++ compile flags |
| `mcpp:cflag=<flag>`                | add `<flag>` to the C compile flags |
| `mcpp:link-lib=<name>`             | link `-l<name>` |
| `mcpp:link-search=<dir>`           | add a library search dir (`-L`; relative dirs resolve against the project root) |
| `mcpp:cfg=<name>`                  | define `-D<name>` for both C and C++ |
| `mcpp:generated=<path>`            | add a generated source to the build. **A relative path resolves against the project root for the root package, but against `MCPP_OUT_DIR` for a dependency's build.mcpp** — emit an absolute path if the package is both (see below) |
| `mcpp:source=<path>` *(0.0.100+)*  | select a **pre-existing** source file into the build (absolute, or relative to the package root). Same downstream effect as `generated=`; use it for files the program *chose* (payload/vendored tree) rather than wrote — e.g. a per-target source selection over a large tarball |
| `mcpp:include-dir=<dir>` *(0.0.100+)* | add a **private** include directory (`-I`) for this package's own TUs (absolute, or relative to the package root; normalized). Replaces the `cxxflag=-I` + `cflag=-I` double emission |
| `mcpp:include-dir-after=<dir>` *(0.0.100+)* | like `include-dir`, but searched **after** the system directories (`-idirafter`) — for payload trees that shadow system headers |
| `mcpp:runner=<token>` *(2026.8.19.2+)* | one argv token of the command that EXECUTES this build's artifact, when the host cannot. Emitted once per token, in order; the artifact path is appended (or substituted for `{}`). Reaches the **consumer**. ⚠️ Emit the executable as an ABSOLUTE path, and only **one** dependency may supply it |
| `mcpp:link-script=<path>` *(2026.8.19+)* | link with this **linker script** (`-T`; relative resolves against the package root, and the emitted path is absolute because the link runs in the build directory). Reaches the **consumer**, unlike `include-dir` — a board's memory layout is the one thing a consumer cannot write for itself |
| `mcpp:warning=<text>` *(2026.8.21.2+)* | say something to the user and **keep going**. The one directive that changes no compile line, no link line and no source set. Survives the build cache — see below |
| `mcpp:rerun-if-changed=<path>`     | re-run `build.mcpp` when this file changes |
| `mcpp:rerun-if-env-changed=<VAR>`  | re-run `build.mcpp` when this env var changes |

The program **requests** build edges (flags, libraries, sources). It cannot add a
registry dependency — the dependency graph stays declarative in `mcpp.toml`
(including platform-conditional `[target.windows.dependencies]`). `build.mcpp`
is for *leaf* decisions: flags, codegen, link requirements.

`include-dir`/`include-dir-after` are deliberately **private** (Cargo
discipline): they color only this package's own TUs and are never propagated
to consumers. An include directory consumers must see is part of the public
interface and belongs in the declarative manifest/descriptor
(`[build] include_dirs`), not in a build-time program.

## Typed API: `import mcpp;` (recommended)

Instead of printing raw strings, `build.mcpp` can be written **modules-first** —
`import mcpp;`, no `#include` needed. The `mcpp` module is bundled in the
mcpp binary (so it always matches that mcpp's protocol) and is compiled on demand;
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
| `mcpp::source(p)` | `mcpp:source=` |
| `mcpp::include_dir(d)` / `mcpp::include_dir_after(d)` | `mcpp:include-dir=` / `mcpp:include-dir-after=` |
| `mcpp::rerun_if_changed(p)` / `mcpp::rerun_if_env_changed(v)` | the matching `rerun-*` directives |
| `mcpp::rerun_if_changed_glob(pat)` *(2026.8.6.2+)* | `mcpp:rerun-if-changed-glob=` — re-run when the **set** of files matching `pat` changes (see below) |
| `mcpp::dep_bin(pkg, tool)` *(2026.8.5.1+)* | reads `MCPP_DEP_<PKG>_BIN_<TOOL>` — the absolute path of a **host tool** built by a dependency (see below) |
| `mcpp::link_script(p)` *(2026.8.19+)* | `mcpp:link-script=` |
| `mcpp::runner(tok)` *(2026.8.19.2+)* | `mcpp:runner=` — see below |
| `mcpp::xpkg_dir(ns, name)` / `mcpp::xpkg_dir(name)` *(2026.8.19+)* | the payload directory of a package this manifest declared in `[xlings] deps`; `""` when it was not declared or is not installed (see below) |
| `mcpp::warning(text)` *(2026.8.21.2+)* | `mcpp:warning=` — see below |
| `mcpp::action{…}.submit()` *(2026.8.5.1+)* | `mcpp:action=` — declares a **build-graph node** instead of doing the work here (see below) |

### `warning` — succeeding and still being heard (2026.8.21.2+)

A build program's output reaches the user **only when the program exits
non-zero**: mcpp captures it and prints what it captured on failure. So a
`std::printf` or `std::fprintf(stderr, ...)` note is invisible on exactly the
successful builds that needed it.

```cpp
if (const char* dir = mcpp::xpkg_dir("xim", "qemu-riscv"); dir && *dir) {
    mcpp::runner(std::format("{}/bin/qemu-system-riscv64", dir).c_str());
    // … the rest of the argv …
} else {
    mcpp::warning("qemu-riscv is not installed, so `mcpp run` has no runner. "
                  "Install it once:  xlings install qemu-riscv -y");
}
```

⚠️ **This exists because the alternatives are worse, and both were tried.** A
note on stderr printed nothing on a successful build. Exiting non-zero would be
wrong too: `mcpp build` has no need of an emulator, and failing a build that is
correct trades a missing sentence for a broken command.

Use it for a condition the program **handled correctly** but the user would want
to know about — most often *"I could not find X, so I configured nothing that
depends on it."* For an error, exit non-zero; that output is printed already.

**It does not fail the build.** `mcpp build` still exits 0.

**It is attributed.** The line appears as `<package>: <text>`, because in a
workspace several programs may speak and the reader needs to know which manifest
to open.

⭐ **It survives the build cache.** A build program's result is cached, and a
cache hit does not re-run it — so an advisory that lived only on the run path
would appear on a project's first build and never again, which reads as *"the
condition was resolved"*. mcpp replays it on every hit.

⚠️ **A whole-project no-op build prints nothing at all, including this.** When
there is nothing to do the build never reaches the `build.mcpp` stage — it also
does not report which target it built or which sources it inferred. Touch a
source and the advisory returns.

### `runner` — how the artifact is executed (2026.8.19.2+)

A board-support package knows the emulator, its machine model and its firmware
mode. It also knows where the emulator IS, which a static manifest cannot: the
payload path carries a home and a version.

```cpp
const char* qemu = mcpp::xpkg_dir("xim", "qemu-riscv");
mcpp::runner(std::format("{}/bin/qemu-system-riscv64", qemu).c_str());
for (auto a : {"-machine","virt","-nographic","-no-reboot","-kernel"})
    mcpp::runner(a);
```

The consumer then needs no `[target.<triple>]` section at all. If it writes one
anyway, **it wins** — swapping `-bios default` for `-bios none -semihosting`
while debugging is a legitimate thing to want — and mcpp says which dependency
it overrode.

⚠️ **Emit the executable as an absolute path.** A bare name resolves through
`PATH` to a shim that dispatches against its own owner home, which is not
necessarily the home this build uses.

⚠️ **Exactly one dependency may supply a runner.** Two board-support packages
both claiming to know how to run the artifact is a configuration error, and
mcpp reports it naming both rather than merging them into an argv that is
neither one's.

### Asking instead of declaring: `toolchain_dir` / `sysroot_dir` (2026.8.19.4+)

```cpp
const char* tc = mcpp::toolchain_dir();   // the resolved toolchain's payload root
const char* sr = mcpp::sysroot_dir();     // the TARGET's C library root, or ""
```

A package that needs headers shipped by the toolchain — libc++'s, for a
freestanding standard-library subset — or a file inside the target's C library
— a linker script, for a board-support package — asks for the directory rather
than declaring a dependency on the thing that provides it.

⚠️ The difference is not cosmetic. Declaring `xim:llvm` pins a package to one
standard-library implementation; declaring `xim:picolibc-riscv@1.8.12` pins it
to one C library, one architecture and one version. Neither is a property of a
package whose content is implementation-neutral. Asking follows whatever
`[toolchain]` and `--target` actually resolved.

`sysroot_dir()` is empty on a hosted target: there the C library arrives with
the compiler payload or through the runtime binding, and nothing has to look
for it.

### Finding an `[xlings] deps` payload: `xpkg_dir` (2026.8.19+)

`dep_dir` answers for **mcpp** dependencies. An xlings package is a different
namespace with a different store layout, and `xpkg_dir` is the interface for it:

```cpp
// mcpp.toml
//   [xlings]
//   deps = ["xim:picolibc-riscv@1.8.12"]

const char* sysroot = mcpp::xpkg_dir("xim", "picolibc-riscv");   // exact
const char* same    = mcpp::xpkg_dir("picolibc-riscv");          // bare name
```

The namespaced form answers only for a package declared under that namespace
and is the one to prefer; the bare form is a convenience for the common single
declaration, and when two namespaces claim one name it answers for the first
**declared**. Both return `""` when the package was not declared or is not
installed — a program that needs it should say so itself, because only it knows
whether the absence is fatal.

It is an interface rather than a documented path because the alternative is a
build program encoding `<home>/data/xpkgs/<ns>-x-<name>/<version>`, which is
store internals mcpp is free to change — the same reason `dep_dir` exists.

⚠️ A **pinned** reference resolves to exactly that version or to nothing. A
build that asked for `1.8.12` and silently got `1.9.0` is an answer only
discovered later, in the artifact.

### Host tools from a dependency (2026.8.5.1+)

Declare the need in `mcpp.toml`, then call it:

```toml
[dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

```cpp
// build.mcpp
import mcpp;
int main() {
    const char* protoc = mcpp::dep_bin("protobuf", "protoc");
    // … invoke it, then declare what it produced …
}
```

mcpp builds that `kind = "bin"` target **for the build machine** (even under
`--target`), caches it globally, and returns the path. The request lives in
`mcpp.toml` rather than here for the same reason a dependency does: asking the
graph for an extra artifact is a graph-level request, and the graph stays
statically analysable. See [05 §2.14](05-mcpp-toml.md) for the full contract,
including `[tools.overrides]` and `reexport = true` (which is how a library
provides the whole toolchain, so a project declares **one** dependency instead of
four).

### Globbing inputs: `rerun_if_changed_glob` (2026.8.6.2+)

The re-run key is built from *declared* inputs. Declare files and it works;
glob a directory and it does not — adding a `.proto` changes no declared file's
hash, so the program never re-runs and the new file is silently never
generated. `rerun_if_changed_glob` is how a program says "my output depends on
which files are here":

```cpp
import mcpp;
int main() {
    mcpp::rerun_if_changed_glob("proto/**/*.proto");
    // … scan the directory, declare one action per file …
}
```

The pattern is relative to the manifest directory and uses the same `*` / `**`
grammar as `sources = [...]`. Its fingerprint is the **sorted set of matching
paths** and nothing else:

- **not contents** — a file whose bytes matter is an ordinary
  `rerun_if_changed` input, which already hashes them;
- **not mtime or size** — mtime is unstable across `git checkout`, container
  builds and `rsync`, and size is a weaker signal than the hash above.

The build output tree and `.git` are never part of the set, so a wide pattern
cannot make the program re-run forever against its own outputs.

### Declaring work instead of doing it: `mcpp::action` (2026.8.5.1+)

Generating a source by writing it *here* is the easy path and the wrong one
past a certain size: it happens once per prepare, for the whole set, serially,
and a failure is reported as "build.mcpp exited 1". **Declare** the work and it
becomes an edge in the build graph — incremental, parallel, and attributable to
the edge that failed.

```cpp
import mcpp;
int main() {
    const std::string out = std::string(mcpp::out_dir()) + "/foo.pb.cc";
    mcpp::action a;
    a.id = "protoc:foo";
    a.role = "source";              // "source" | "check" | "object" | "artifact"
    a.arg(mcpp::dep_bin("protobuf", "protoc"))
     .arg("--cpp_out=...").arg("proto/foo.proto")
     .input("proto/foo.proto")
     .output(out.c_str())
     .submit();
}
```

Four roles, one primitive — `role` only decides where the edge's outputs
attach:

| `role` | Outputs | Ordering | Typical |
|---|---|---|---|
| `source` | join the compile set | the compile edge consumes them | protoc, a transpiler |
| `check` | a stamp file | runs **alongside** compilation (set `blocking = true` to gate it) | clang-tidy, a format or ABI check |
| `object` | join the **link** set | the link edge consumes them | a resource compiler, `objcopy` embedding a blob, a generated `.def`, a pre-built `.o` |
| `artifact` | a new file | its *inputs* are link outputs, so it runs after the link | codesign, packaging, size budgets |

No phase machinery is involved: ninja's own file dependencies do the
sequencing, which is also why an `artifact` action cannot double-apply itself
the way a naive "post-build hook" would.

`object` (2026.8.7.1+) takes an optional `.target("name")`, repeatable. It needs
a name at all because, unlike `artifact`, it runs *before* the link and so has
no `${mcpp.target_file:…}` to infer one from; every name that matches no link
unit is an error, including one written next to a name that does match.

**Prefer omitting it.** With no target, the outputs attach to every image the
declaring package produces in this build — binary, shared library **and test
binary**. Test binaries are in that set because they link the same library code:
leave them out and `mcpp build` succeeds while `mcpp test` dies with `undefined
symbol` on the very symbol the action exists to provide. Naming them instead is
not an option — test link units are discovered from `tests/*.cpp`, so their
names are not in `mcpp.toml`, and a `build.mcpp` that spells one stops building
under plain `mcpp build`, where that unit does not exist.

If nothing in the build can receive the outputs (an archive-only package), mcpp
reports a degradation: the edge is reachable only through a link, so with no
link the command would never run and the build would say nothing.

> Naming a pre-built object in `[build].ldflags` also reaches the linker, and
> should not be used for anything the build produces: ldflags is a flat string
> in the link command, not a file in the graph, so nothing tracks it and editing
> it reports `ninja: no work to do`. For Windows resources specifically, use
> [`[resources]`](05-mcpp-toml.md) —
> `object` is the escape hatch for everything else.

**You must name the output files.** mcpp fixes the source set, the fingerprint
and the module graph during prepare, so an output whose *name* is unknown
cannot be built. Content may arrive later; names may not. A malformed action is
a hard error, never a silent skip.

For a generated **module interface**, declare its interface too:

```cpp
a.output(gen.c_str()).provides("my.generated").imports("std").submit();
```

mcpp seeds a placeholder carrying exactly that declaration so the prepare-time
scan agrees with what the generator will emit — the same assertion-plus-
verification trade `[modules].scan_overrides` makes, and the compiler's own
P1689 output checks it at build time.

Commands are an **argv, not a shell string** (no shell is assumed — Windows has
none to rely on), and the only interpolations are a closed set:

| Variable | Value |
|---|---|
| `${mcpp.out_dir}` | the build output directory |
| `${mcpp.bin_dir}` | where produced binaries land |
| `${mcpp.compile_db}` | path to `compile_commands.json` (what clang-tidy's `-p` wants) |
| `${mcpp.target_file:<name>}` | the built file of target `<name>` |

The raw stdout protocol above remains the low-level substrate; `import mcpp;`
is the typed layer over it.

### `import mcpp;` is the surface that evolves (mcpp 2026.8.5.1+)

Two ways to talk to mcpp, and they carry **different compatibility promises**:

| | `import mcpp;` | hand-written `printf("mcpp:…")` |
|---|---|---|
| Compatibility | The module is **bundled in the mcpp binary** and recompiled by the mcpp that runs it, so program and engine can never disagree | Your string is frozen text; nothing checks it against the engine |
| New directives | Arrive as new functions | **Will not be added** |
| Unknown directive | **Hard error** | Warning, then ignored |

Programs using `import mcpp;` automatically announce the protocol version they
were built against (`mcpp:protocol=<N>`, emitted before `main` runs — it never
write it yourself). mcpp uses that two ways:

- A program announcing a **newer** protocol than mcpp understands is **refused**,
  with an upgrade hint. Continuing would silently drop directives the build
  depends on — and "the build succeeded but the flag never arrived" is the
  worst class of build bug.
- An **unrecognized directive is an error** rather than a warning, and the error
  names *both* possible causes. It cannot name one: the protocol number is
  stamped by whichever mcpp **compiled** the program, not carried by the
  package, so a package written for a newer mcpp arrives at an older one
  wearing the older engine's number. Two matching numbers therefore say nothing
  about whether the key came from the future.

A `printf`-style program announces nothing, so it keeps the historical
warn-and-ignore behaviour. That surface is **frozen at the eleven directives in
the table above** — it still works and will keep working, but new capabilities
land only in the typed API. Prefer `import mcpp;` for anything intended to
maintain.

#### A package that needs a newer mcpp

When a published package calls a typed function this mcpp does not have, the
compile error naming it is followed by:

```
       The `mcpp` build module this engine bundles does not have that name.
       Either the package was written for a newer mcpp (try `mcpp self update`;
       this is mcpp 2026.8.19.2), or the name is misspelled …
```

The package cannot handle this itself, and it is worth knowing why — the
obvious guard does not compile:

```cpp
if constexpr (requires { mcpp::runner("qemu"); })   // ✗ hard error when absent
    mcpp::runner("qemu");
```

A `requires`-expression over a **qualified name that does not exist** is
ill-formed, not `false`. So there is no in-language feature probe, and a
package that adopts a new directive states its floor in prose (its README) and
relies on the diagnostic above. Such a package should name the mcpp version it
requires.

### `import std;` (mcpp 2026.8.2.1+)

A `build.mcpp` may `import std;` (and `import std.compat;`), alone or together
with `import mcpp;`:

```cpp
// build.mcpp
import std;
import mcpp;

int main() {
    for (auto const& f : std::vector<std::string>{"FOO", "BAR"})
        mcpp::define(f.c_str());
}
```

mcpp stages the **same** std module its own build uses, keyed on
(toolchain × standard × dialect) — so for an ordinary build this costs
nothing, the artifact is already there. A cross build (`--target …`) pays for
one extra std module, because `build.mcpp` compiles and runs on the *host*
while the project targets something else.

`#include` still works and stays the right choice for a program that only
needs `std::fopen`; there is no requirement to modularize a build script.

Every toolchain mcpp can build a host program with can build a `build.mcpp`,
including native MSVC — the module handling reads the same tables the main
build does, so `cl.exe`'s `.ifc` + `/reference` needs no separate support.

## Environment contract (mcpp 0.0.95+)

The running program receives the build context as `MCPP_*` variables
(Cargo's env-family equivalent), also exposed through typed readers:

| Variable | Typed reader | Value |
|---|---|---|
| `MCPP_TARGET` | `mcpp::target()` | resolved canonical triple (the `--target` triple under cross; the host triple natively) |
| `MCPP_TARGET_OS` *(0.0.100+)* | `mcpp::target_os()` | the target's OS segment (`linux`/`macos`/`windows`) — no need to hand-split `MCPP_TARGET` |
| `MCPP_TARGET_ARCH` *(0.0.100+)* | `mcpp::target_arch()` | the target's arch segment (GNU spelling: `x86_64`, `aarch64`, …) |
| `MCPP_TARGET_ENV` *(0.0.100+)* | `mcpp::target_env()` | the target's env segment (`gnu`/`musl`/`msvc`); empty string when the triple has none (macOS) |
| `MCPP_HOST` | `mcpp::host()` | the host triple |
| `MCPP_PROFILE` | `mcpp::profile()` | effective profile name (`dev`/`release`/…) |
| `MCPP_OUT_DIR` | `mcpp::out_dir()` | a writable scratch/output dir owned by mcpp |
| `MCPP_MANIFEST_DIR` | `mcpp::manifest_dir()` | the package root (= CWD) |
| `MCPP_FEATURE_<NAME>` | `mcpp::has_feature("name")` | set to `1` per active feature (same `<NAME>` sanitization as the `MCPP_FEATURE_` compile macro) |
| `MCPP_FEATURES` | — | comma-separated active feature list |
| `MCPP_DEP_<NAME>_DIR` | `mcpp::dep_dir("name")` | the resolved install dir of each declared dependency (canonical **and** namespace-stripped name spellings; same `<NAME>` sanitization as `MCPP_FEATURE_`). Received by dependencies' build.mcpp **and** the root project's (the root runs after dependency resolution, 0.0.100+) |

These values are folded into the re-run key **unconditionally** — changing the
target, profile, or feature set re-runs the program without any
`rerun-if-env-changed` declaration.

## Dependencies' build.mcpp (mcpp 0.0.95+)

A dependency that ships a `build.mcpp` gets it compiled and run too (the
Cargo `build.rs` model — building a package means trusting its build program),
after its features are resolved and before the source scan. Scope follows
Cargo: `cxxflag`/`cflag`/`cfg` directives color **only that package's own
TUs**; `link-lib`/`link-search` reach the final link. Its artifacts (binary,
cache, `MCPP_OUT_DIR`) live in the **consuming project's**
`target/.build-mcpp/deps/<pkg>@<ver>/` — a registry package root is shared
across projects (and may be read-only), so it is never written to; relative
`generated=` paths resolve against `MCPP_OUT_DIR`, not the package root.

### A library that is also built standalone: emit an absolute path

Those two rules — project root for the root package, `MCPP_OUT_DIR` for a
dependency — mean a *relative* `generated=` cannot be right in both roles. A
library is built standalone by its own CI and consumed from the registry by
everyone else, so it plays both.

Writing into `MCPP_OUT_DIR` and emitting the bare filename works as a
dependency and fails at the root with:

```
error: build.mcpp declared generated source 'foo.cppm' but it does not exist after the run
```

Write to `MCPP_OUT_DIR` (the package root may be read-only) and emit the
**absolute** path:

```cpp
const auto out = std::filesystem::path(mcpp::out_dir()) / "foo.cppm";
// ... write it ...
mcpp::generated(out.string().c_str());
```

`mcpp::out_dir()` is always absolute, so this is correct in both roles and
needs no branch on which of the two applies.

A generated **module interface** is fine here: `.cppm` goes through the same
scan as any other source, so a generated file declaring `export module …` can
be imported by the package's own TUs.

## Incremental: declared inputs (no needless re-runs)

mcpp does **not** re-run `build.mcpp` on every build. It caches the program's
directives and re-runs only when something it depends on changed:

- the `build.mcpp` source itself,
- the toolchain,
- any file declared with `rerun-if-changed`,
- any env var declared with `rerun-if-env-changed`,
- (or a `generated` output / `source=` selection went missing),
- (or the cache was written by an mcpp that interpreted a directive differently
  — the entry carries a format **epoch**, and a foreign one re-runs the program
  once instead of replaying values under the wrong meaning).

So **declare the inputs**: if the program reads `config.h` or the `USE_FAST`
variable, emit `mcpp:rerun-if-changed=config.h` / `mcpp:rerun-if-env-changed=USE_FAST`.
This replaces the old "process exited 0, so assume it's fine" guesswork with an
explicit input/output contract — incremental builds stay correct.

When nothing changed the output is `build.mcpp up to date (cached)`; otherwise
`build.mcpp compiling` / `running`.

## Notes & limits

- **Runs on the host — including under cross** (mcpp 0.0.95+). Under
  `mcpp build --target <triple>` the program is compiled with a host-resolved
  toolchain, runs on the host, and sees `MCPP_TARGET` = the cross triple.
  For purely declarative target gating, `[target.'cfg(...)']` tables remain
  the first choice — see [05 - mcpp.toml Manifest Guide](05-mcpp-toml.md).
- **CWD is the project root**, so relative paths (`src/generated.cpp`) land where
  expected.
- A non-zero exit from `build.mcpp` aborts the build and prints its output.
- **The run is bounded** (mcpp 2026.8.5.1+): a build program gets **600 s** by
  default, after which mcpp kills it and fails the build naming the package.
  Configure it per package:

  ```toml
  [build]
  build_program_timeout = 1800   # seconds; 0 = no limit
  ```

  Precedence, highest first — the same shape `macos_deployment_target` uses:

  ```
  MCPP_BUILD_PROGRAM_TIMEOUT=<seconds>   this invocation only
    > [build] build_program_timeout      the manifest of the package that OWNS the build.mcpp
    > 600                                built-in default
  ```

  The value comes from the **owning package's** manifest, because its author is
  the one who knows how long the generator takes. When a dependency's build
  program times out, the error names the exact `mcpp.toml` to edit — editing
  a hand-written one would change nothing.

  Omitting the key is not the same as `0`: unset means "use the default bound",
  `0` means "no bound at all".

  **The bound is enforced on every platform** as of mcpp 2026.8.11.1. It used
  to be POSIX-only: the Windows launcher fell through to an unbounded path, so
  this knob — and `mcpp test --timeout`, and `--build-timeout` — silently did
  nothing there. Windows now runs the child in a Job object and closes it on
  expiry, which takes the whole process tree rather than just the direct child
  (a grandchild left holding the capture pipe would otherwise hang the drain
  after the kill).

  The **compile** is deliberately *not* bounded — the same asymmetry `mcpp test`
  uses: a long compile is usually legitimate (a first-run `std` module build is
  minutes) and killing it produces a baffling failure, while a long-running
  build *program* is usually stuck, and an unbounded one hangs the whole build
  with no diagnostic at all.

  > **Why not "ask the user instead of aborting"** ([#410](https://github.com/mcpp-community/mcpp/issues/410)):
  > the program's stdout is already dup2'd into a pipe that carries the `mcpp:`
  > directive protocol, so there is no interaction channel; most builds run
  > where nobody is watching (CI, a pipeline, ninja's child), and a build
  > blocked on a prompt is harder to diagnose than one that failed; and a build
  > whose outcome depends on a keystroke is not reproducible. The configurable
  > bound plus an error that names the file to edit answers the same need.
