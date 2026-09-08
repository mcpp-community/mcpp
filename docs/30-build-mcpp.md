# 30 — Build Programs: `build.mcpp`

**English** | [简体中文](zh/30-build-mcpp.md)

**Reader:** an author whose build needs a step mcpp has no rule for — code
generation, an embedded asset, a check, a second compiler.

**The question this chapter answers:** how do I add work to the build graph, so
that it is ordered, fingerprinted and incremental like everything else.

**Not here:** packaging that step so other projects can use it, which is
[31 — Authoring a Rule Package](31-authoring-a-rule-package.md), and the tools
the step runs, which are [23 — The Project Environment](23-the-project-environment.md).

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
| `mcpp:runner=<token>` *(2026.8.19.2+)* | one argv token of the command that EXECUTES this build's artifact, when the host cannot. Emitted once per token, in order; the artifact path is appended (or substituted for `{}`). Reaches the **consumer**. Emit the executable as an ABSOLUTE path, and only **one** dependency may supply it |
| `mcpp:link-flag=<flag>` *(2026.9.6.5+)* | add a **linker flag** this program computed, verbatim. The outlet `link-lib` / `link-search` / `link-script` leave open: a generated version script (`-Wl,--version-script=`), `-Wl,--wrap=malloc` for a runtime that takes over a C-library symbol, `-Wl,--exclude-libs,ALL` so a statically absorbed third party does not become part of this package's ABI. Appended after `[build] ldflags`, in emission order. **Reaches the consumer**, exactly as `[build] ldflags` does — see below |
| `mcpp:link-script=<path>` *(2026.8.19+)* | link with this **linker script** (`-T`; relative resolves against the package root, and the emitted path is absolute because the link runs in the build directory). Reaches the **consumer**, unlike `include-dir` — a board's memory layout is the one thing a consumer cannot write for itself |
| `mcpp:warning=<text>` *(2026.8.21.2+)* | say something to the user and **keep going**. The one directive that changes no compile line, no link line and no source set. Survives the build cache — see below |
| `mcpp:fact=<name>=<version>` *(2026.9.5.2+)* | state something the program **established about the machine** (`cuda.driver=12.4`). Compared against floors before anything is compiled; see below |
| `mcpp:floor=<name> >= <version>` *(2026.9.5.2+)* | state what this package **needs** of that quantity. Unmet ⇒ the build is refused with both values (`version-floor-unmet`); a floor nobody stated a fact for is silent |
| `mcpp:rerun-if-changed=<path>`     | re-run `build.mcpp` when this file changes |
| `mcpp:rerun-if-env-changed=<VAR>`  | re-run `build.mcpp` when this env var changes |

The program **requests** build edges (flags, libraries, sources). It cannot add a
registry dependency — the dependency graph stays declarative in `mcpp.toml`
(including platform-conditional `[target.windows.dependencies]`). `build.mcpp`
is for *leaf* decisions: flags, codegen, link requirements.

`link-flag` is deliberately **not** private, and the reason is worth stating
because the opposite looks safer. A compile interface has a declarative public
counterpart (`[build] include_dirs`), so a build-time program widening it would
go behind the manifest's back — hence `include-dir`'s privateness. Link flags
have no such split: `[build] ldflags` already propagates to consumers, so a
private computed form would behave differently from its own declarative twin.

The consequence is stated rather than hidden. A dependency emitting
`-Wl,--version-script=` puts it on the consumer's link line too, which is
usually not what that dependency meant. That hazard is not new — a dependency
writing the same flag in `[build] ldflags` has always done this — so this
directive widens *who can compute the value*, not *what the value can reach*.

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
| `mcpp::link_flag(s)` *(2026.9.6.5+)* | `mcpp:link-flag=` |
| `mcpp::link_script(p)` *(2026.8.19+)* | `mcpp:link-script=` |
| `mcpp::runner(tok)` *(2026.8.19.2+)* | `mcpp:runner=` — see below |
| `mcpp::xpkg_dir(ns, name)` / `mcpp::xpkg_dir(name)` *(2026.8.19+)* | the payload directory of a package declared in `[xlings.workspace]` — by this manifest, or by a dependency compiled into this build program *(2026.9.6.6+)*; `""` when it was not declared or is not installed (see below) |
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

**This exists because the alternatives are worse, and both were tried.** A
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

**It survives the build cache.** A build program's result is cached, and a
cache hit does not re-run it — so an advisory that lived only on the run path
would appear on a project's first build and never again, which reads as *"the
condition was resolved"*. mcpp replays it on every hit.

**A whole-project no-op build prints nothing at all, including this.** When
there is nothing to do the build never reaches the `build.mcpp` stage — it also
does not report which target it built or which sources it inferred. Touch a
source and the advisory returns.

### The probe channel: `fact` / `floor` (2026.9.5.2+)

A rule package is the thing that knows how to ask a machine what it has —
which library to open, which function to call — and the engine is the thing
that must not. So the package **measures** and the engine **compares**:

```cpp
mcpp::fact("cuda.driver", "12.4");      // what this machine has
mcpp::floor("cuda.driver >= 12.0");     // what this package needs of it
```

Before anything is compiled, an unmet floor refuses the build and names the
quantity, both versions and who stated the fact; `mcpp why toolchain --format
json` classifies it as `reason: version-floor-unmet`. A floor for which nobody stated a fact is
**silent**: not knowing is not failing, and a refusal manufactured from
ignorance is the worse error.

The failure this prevents is not visible at build time on its own. A program
built against a device runtime newer than the driver it will meet compiles
cleanly, links cleanly and fails at first use with a message naming neither
side. The rule package that resolved the runtime knows both numbers before
the first compile.

**Neither string means anything to the engine.** `cuda.driver` is data
flowing through; the engine reads a name, a relation and a version, and a
second backend needs no engine change. The spelling of a fact matches what a
package could also have declared statically in `[runtime] provides`, and a
floor matches `[[runtime.requirements]]` with `kind = "version-floor"`: the
two channels land in one list.

**A fact is cached with the program's other output** and replayed on a
cache hit. Declare what would change it — `rerun_if_changed` on the library
the version was read from — or the fact outlives the machine it described.

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

**Emit the executable as an absolute path.** A bare name resolves through
`PATH` to a shim that dispatches against its own owner home, which is not
necessarily the home this build uses.

**Exactly one dependency may supply a runner.** Two board-support packages
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

The difference is not cosmetic. Declaring `xim:llvm` pins a package to one
standard-library implementation; declaring `xim:picolibc-riscv@1.8.12` pins it
to one C library, one architecture and one version. Neither is a property of a
package whose content is implementation-neutral. Asking follows whatever
`[toolchain]` and `--target` actually resolved.

`sysroot_dir()` is empty on a hosted target: there the C library arrives with
the compiler payload or through the runtime binding, and nothing has to look
for it.

### Driving a second compiler: `toolchain_sysroot` / `toolchain_binutils_dir` (2026.9.5.2+)

```cpp
const char* sr = mcpp::toolchain_sysroot();        // the `--sysroot` mcpp passes, or ""
const char* bu = mcpp::toolchain_binutils_dir();   // the dir mcpp names with `-B`, or ""
```

A rule package sometimes has to run a compiler mcpp did not resolve. `nvcc`
rejects a libc++ host compiler and fails inside GCC 16's `<type_traits>`, so a
CUDA rule package resolves a second host compiler from a declared payload;
`hipcc` and `-fsycl-host-compiler` pose the same question.

That compiler starts knowing nothing about the environment it was placed in.
Under a sub-OS the C library is not at `/usr/include` and the assembler is not
at `/usr/bin`, so the first `#include` it reaches fails:

```
crt/host_config.h:218: fatal error: features.h: No such file or directory
```

These two answers are the flags mcpp passes to its own compiler for the same
target. Forwarding them — `--sysroot=<value>` and `-B<value>`, through whatever
the outer tool spells host options with — makes the second compiler see what
the first one sees.

**Not `sysroot_dir()`.** That answers a question about the target's *tier*
and is empty on a hosted target, which is exactly the case this pair exists
for. Either of these two is empty when mcpp passes no such flag.

### The resolved C++ standard library: `cxx_stdlib` (2026.9.6.3+)

```cpp
const char* impl = mcpp::cxx_stdlib();   // "libstdc++" | "libc++" | "msvc-stl" | ""
```

`compiler()` does not answer this. clang links libc++ on one machine and
libstdc++ on another and reports `clang` in both cases, and the two
implementations differ in what they accept — a `unique_ptr` to an incomplete
type destroyed in a header compiles under libstdc++ and does not under libc++.
A build program that must refuse such a configuration by name cannot ask
`compiler()`, because that answer would also refuse the configuration that
works:

```cpp
if (std::string_view(mcpp::cxx_stdlib()) == "libc++") {
    std::fprintf(stderr,
        "this feature does not compile under libc++; select a libstdc++ "
        "toolchain, or turn the feature off\n");
    return 1;
}
```

`cxx` is in the name because `MCPP_TARGET_LIBC` is the *C* library. The two are
different questions and, in an ecosystem that names glibc and musl constantly,
must not share a word.

### Finding an `[xlings.workspace]` payload: `xpkg_dir` (2026.8.19+)

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

A **pinned** reference resolves to exactly that version or to nothing. A
build that asked for `1.8.12` and silently got `1.9.0` is an answer only
discovered later, in the artifact.

A **constrained** one (`>=8.5.0`, `^1.2`) resolves to the highest installed
version satisfying it *(2026.9.6.6+)*. Before that release the whole version
position was compared against a directory name, so a range installed a payload
and then answered that nothing was installed — which is why a rule package
could not state a floor and every project repeated its rule's package list.

**A package a DEPENDENCY declared is answered too** *(2026.9.6.6+)*, at the
version this build actually installed rather than the one the local manifest
wrote. One package means one version: where a project and a rule both name it,
the declaration nearer the artifact wins and both sides are told the same
answer. See *One package, one version* in [23 — The Project Environment](23-the-project-environment.md).

**`[feature-xlings.<f>]` is answered too, while `<f>` is active**
*(2026.9.6.2+)*. That table has provisioned its packages since it existed --
naming one downloads and installs it -- but the build program's environment was
filled from `[xlings.workspace]` alone, so `xpkg_dir` returned `""` for a
payload that was on disk. The only sensible thing a program can print then is
"declare this package", naming a declaration its author had already written.

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
statically analysable. See *Host tools from a dependency* in this chapter for the full contract,
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

**Every declared input is compared on the fast path too** (2026.9.5.4+). A
project whose sources are all older than `build.ninja` takes a fast path that
skips the phase where the program's cache is normally consulted, and until
2026.9.5.4 that path asked only about glob path sets. A data file a program
reads is neither under `src/` nor named with a C++ extension, so the mtime
sweep cannot see it either: editing it left the previous run's output in place
and the build reported `Finished dev in 0.00s`. The fast path now compares what
the cache records — a glob's path set, a declared file's content hash, and a
declared environment variable's value — so `rerun_if_changed` means the same
thing under both paths.

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
| `source` | compilable ones join the compile set; the rest are produced but not compiled | **every compile edge of the declaring package waits for them** | protoc, a transpiler, a protocol/IDL generator |
| `check` | a stamp file, written by mcpp | runs **alongside** compilation; `blocking = true` makes the package's compile edges wait for it | clang-tidy, a format or ABI check |
| `object` | join the **link** set | the link edge consumes them | a resource compiler, `objcopy` embedding a blob, a generated `.def`, a pre-built `.o` |
| `artifact` | a new file | its *inputs* are link outputs, so it runs after the link | codesign, packaging, size budgets |

No phase machinery is involved. `object` and `artifact` are sequenced by
ninja's own file dependencies — which is also why an `artifact` action cannot
double-apply itself the way a naive "post-build hook" would. `source` and a
blocking `check` are sequenced by an order-only edge from the declaring
package's compile edges to that package's action outputs.

> **Why `source` needs the edge (mcpp 2026.8.30.2+).** A generated `.cpp`
> becomes an input of the edge that compiles it, so it was ordered for free. A
> generated **header** never does: it is reached through `-I`, and the depfile
> that would record it does not exist until a compile has already succeeded.
> Before this, an action whose outputs were all headers had a node in
> `build.ninja` that nothing could reach — not `default`, not the goal set, no
> consuming edge — so it never ran, and what the compiler read was the empty
> placeholder mcpp writes for a declared output. The ordering is **per
> package**, because `include_dir` colours only the declaring package's own
> translation units.

**An action whose command discovers its own dependencies declares a depfile**
(mcpp 2026.9.7.1+). `input()` fixes the edge's inputs when `build.mcpp` runs,
before the command has executed, so a compiler that learns its `#include` graph
by parsing the source has no channel to report it — and editing a file the
command merely *read* reruns nothing, leaving `mcpp build` green over a stale
artifact.

```cpp
a.depfile = dep.c_str();        // a path the command writes
a.arg("--depfile").arg(dep.c_str());
```

mcpp emits `depfile =` and `deps = gcc` for that edge, so ninja reads the file
and folds what it names into the edge's dependencies. Every device compiler this
matters for already emits one: `glslangValidator --depfile`, `glslc -MD -MF`,
`slangc -depfile`, `nvcc`/`clang` `-MD -MF`.

> **Do not also declare the depfile as an `output()`.** `deps = gcc` makes ninja
> consume and delete it after reading, so an edge that promised it as an output
> would be permanently dirty.

**A check's command does not have to write its stamp** (mcpp 2026.8.29.1+).
The verdict is the exit code; the stamp is bookkeeping the graph needs, and
mcpp creates it when the command succeeds. Before this, every check needed a
wrapper script to touch the file — and a command is an argv with no shell
assumed, so that wrapper could not be written portably at all. A command that
already writes its own stamp is unaffected: an existing file is left alone.

> A missing stamp does **not** fail the build. ninja leaves the declared output
> absent and re-runs that edge on every build afterwards, which looks like a
> passing check that is quietly never satisfied.

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
> [`[resources]`](04-mcpp-toml.md) —
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
if constexpr (requires { mcpp::runner("qemu"); })   // hard error when absent
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
| `MCPP_TOOLCHAIN_SYSROOT` *(2026.9.5.2+)* | `mcpp::toolchain_sysroot()` | the `--sysroot` mcpp passes to its own compiler; empty when it passes none. For a rule package that runs a **second** compiler — see "Driving a second compiler" above |
| `MCPP_TOOLCHAIN_BINUTILS_DIR` *(2026.9.5.2+)* | `mcpp::toolchain_binutils_dir()` | the directory mcpp names with `-B`; empty when it names none (a musl or MinGW payload brings its own assembler and linker) |
| `MCPP_CXX_STDLIB` *(2026.9.6.3+)* | `mcpp::cxx_stdlib()` | the C++ standard library the resolved toolchain uses — `libstdc++`, `libc++`, `msvc-stl`; empty when no toolchain resolved. A different question from `MCPP_TARGET_LIBC`, which is the C library |
| `MCPP_ACCEL` *(2026.9.5.2+)* | `mcpp::accel()` | the device axis of this build, resolved — `--accel` / `--no-accel` over `[build] accel` — in the wire form `cuda12.9+{sm_89} ptx>=89`; empty when the build asks for no accelerator. A rule package derives its own flags (`-gencode`, `--offload-arch`) from it, so the architecture set is written once, in the manifest. The same value feeds the `cfg(accelerator = "…")` layer key |
| `MCPP_LANGUAGE_MODULES` *(2026.9.7.1+)* | -- | `1` when the declaring package sets `[language] modules`, `0` otherwise. A rule that GENERATES a consumer-facing declaration reads it to choose between a module interface and a header, so a project states that once and never again. An older engine leaves it absent, which a rule reads as `0` -- the behaviour every consumer had before the variable existed |
| `MCPP_PKG_NAME` *(2026.9.7.1+)* | -- | The `[package] name` of the package this program builds. Every name a rule generates is derived from it: the module a consumer imports, the namespace the accessors sit in, the symbols in a generated header. Before it existed the closest available answer was the leaf of `MCPP_MANIFEST_DIR`, which is a directory name -- so a package named `vulkan-saxpy` in a directory named `app` generated `app.shaders`, and every `<something>/app/` in a workspace claimed the same module. Absent under an older engine, which a rule reads as a signal to keep its previous derivation |
| `MCPP_PKG_NAMESPACE` *(2026.9.7.1+)* | -- | The `[package] namespace`. Empty when the package declares none. A rule that must produce a name unique across an index uses the pair rather than the name alone, because package identity is `(namespace, name)` |
| `MCPP_DEVICE_SOURCES` *(2026.9.5.2+)* | `mcpp::device_sources()` | the device-kind sources (`.cu`, `.hip`, …) the package's effective `sources` match, package-root-relative, one per line; empty when there are none. The engine compiles none of them — the rule package this program imports turns each into an `mcpp::action`. Already narrowed: a `{ glob, accel }` entry the build does not cover contributes nothing, so `--no-accel` yields an empty list |
| `MCPP_OUT_DIR` | `mcpp::out_dir()` | a writable scratch/output dir owned by mcpp |
| `MCPP_MANIFEST_DIR` | `mcpp::manifest_dir()` | the package root (= CWD) |
| `MCPP_FEATURE_<NAME>` | `mcpp::has_feature("name")` | set to `1` per active feature (same `<NAME>` sanitization as the `MCPP_FEATURE_` compile macro) |
| `MCPP_FEATURES` | — | comma-separated active feature list |
| `MCPP_DEP_<NAME>_DIR` | `mcpp::dep_dir("name")` | the resolved install dir of each declared dependency (canonical **and** namespace-stripped name spellings; same `<NAME>` sanitization as `MCPP_FEATURE_`). Received by dependencies' build.mcpp **and** the root project's (the root runs after dependency resolution, 0.0.100+) |

These values are folded into the re-run key **unconditionally** — changing the
target, profile, or feature set re-runs the program without any
`rerun-if-env-changed` declaration.

### `PATH` — the environment the project declared (mcpp 2026.8.25.1+)

A project that declares `[xlings].subos` runs its build programs with that
environment's `bin` at the front of `PATH`:

```
PATH=<the declared environment's bin>:<the PATH mcpp itself was started with>
```

so a bare command name in a build program resolves inside the environment the
project named, on every machine that builds it.

**Only for projects that declare one.** A project with no `[xlings].subos`
gets the `PATH` mcpp was started with, byte for byte. A shared directory in
front of every project would make what a build sees depend on what else had
been installed on that machine — two projects on one machine would agree with
each other, and the same project on two machines would not.

Why it is a prefix and not a replacement: a build program legitimately calls
`git`, `python3` or a shell, none of which live in a sub-OS. Front position
makes the declared environment the default answer; the host stays reachable
behind it.

**`command -v` answers about the machine, not about this build.** Before
this, a program asking `PATH` for a declared tool could get an unrelated one —
measured on `qemu-system-riscv64`, where the answer was a shim that reports
"is not installed in this subos" when executed, while a working copy sat in the
project's own environment and was not on `PATH` at all.

The selection is the one [chapter 8](91-toolchain-internals.md) already
describes — the same declaration that decides which C library the project links
against, delivered to one more consumer. See
[chapter 17](23-the-project-environment.md) for what a declared environment is
and when to want one; `examples/07-project-subos/` is a working project.

## Writing a rule package

A rule — "run protoc over these `.proto` files", "run clang-tidy over these
sources" — belongs in a package, not copy-pasted into every consumer's
`build.mcpp`. The mechanism is
[`host-module = true`](04-mcpp-toml.md); this section is about the shape of
what goes inside.

The guidance below generalises from `mcpplibs.grpcgen`, the first such package,
with each of its traits judged individually. It is guidance and not a rule
because none of it admits a criterion the engine could check.

**Every device source must reach an action** *(2026.9.5.2+ contract, enforced
from 2026.9.6.5)*. A device-kind file is the one source the engine has no
compile rule for: it is handed to the package's build program through
`MCPP_DEVICE_SOURCES` and comes back as an action, or it is not compiled at
all. mcpp refuses a build in which one did not, naming the files:

    error: `opkit`: device sources that no action compiles:
             src/backends/cuda/saxpy.cu
             src/backends/vulkan/saxpy.comp

The criterion is the action **inputs**, not that a build program ran: a program
that ran and claimed nothing is the common case, because a rule takes the
extensions it knows and leaves the rest. It is also the condition an action
needs anyway — one that compiles a file it does not declare as an input does
not rerun when that file changes — so a rule that satisfies it is a rule that
rebuilds correctly. What it replaces is an undefined reference at the link
naming a symbol and never the file, and for a `kind = "lib"` target not even
that, because an archive is not resolved.

**A rule takes the extensions it claims.** `mcpp::device_sources()` is the
package's whole device set, and every rule in one build program reads the same
value. A project with two backends puts a `.cu` and a `.comp` in that one list,
so a rule that consumes all of it hands its compiler a file the compiler does
not accept. A rule selects by extension, and returns without complaint when
this build names no backend it serves — a build program with several rules
calls them all.

**An import nothing provides is refused by name.** A build program may import
`std`, `std.compat`, the bundled `mcpp`, and the host modules its dependency
edges asked for. Anything else is refused before the compiler is reached, with
the key that would have made it importable:

    error: build.mcpp imports 'mcpp.rules.spirv', and no dependency provides it
    as a host module.
           ...
             [build-dependencies.<namespace>]
             <name> = { version = "...", host-module = true }
           declared without `host-module = true`: mcpp.plugins (in [build-dependencies])

**The module name is declared by the rule's source, and `mcpp.*` is reserved.**
A host module is registered under the name its interface unit declares, not
under the package name, so `export module mcpp.rules.spirv;` is what a consumer
then imports. Official plugins live in one package, `mcpp:plugins` (repository
`mcpp-community/mcpp-plugins`): rule packages are named `mcpp.rules.<x>`,
build-time utilities `mcpp.tools.<x>`, and each member is selected by a feature
of that package (see [`host-module = true`](04-mcpp-toml.md)). `mcpp.build.*`
is the engine's own module family and is not used for plugins. The engine
cannot tell who is official, so it keys the check on the package *namespace*
and warns when the two disagree —

    warning: build rule 'mcpplibs.plugins' declares the module
    'mcpp.rules.spirv'; the 'mcpp.' prefix is reserved for rules maintained by
    the mcpp project.

Nothing breaks; the name claims an origin the package does not have. A rule
outside the project picks its own prefix.

**A tool is not a rule.** A rule states how a translation unit is compiled by a
compiler mcpp does not drive: it submits an action and the engine schedules it.
A tool states something the build program needs that no compiler performs, and
does it while the program runs. `mcpp.tools.embed` (feature `tools-embed`,
mcpp 2026.9.5.4+) is the first: it writes a data file into a header the program
compiles in, as a byte array or a 32-bit word array, and rewrites nothing when
the content is unchanged, so calling it unconditionally costs no rebuild.

`examples/09-heterogeneous/cuda` and
`examples/09-heterogeneous/vulkan` consume `mcpp.rules.cuda` and `mcpp.rules.spirv`
from `mcpp:plugins`, the way any project does.

**Layers must not have a cliff, and each layer must be the composition of the
one below it.** `generate_all(opt)` *is* `submit(plan_all(opt))`, and
`.grpc = true` *is* `.plugins = {cpp()}`. Past two knobs a consumer who cannot
descend writes sixty lines by hand to work around the rule, and those sixty
lines then drift away from it silently.

**Expose a plan/submit pair.** The bottom layer has to hand back the planned
edges so a consumer can modify them and submit them again. This is the
mechanism that makes the previous paragraph true; it is not a naming
preference.

**Do not reproduce truth the engine already holds.** mcpp writes every action's
full argv into `build.ninja`, recoverable with `ninja -t commands`. A second
source of that would only drift. What a rule owns is the other half — which
knobs produced the command — and that belongs in each edge's `description`.

**Failure and advice use different channels.** mcpp prints what it captured
from a build program only when the program exits non-zero, so a failure writes
to stderr and returns non-zero. A message that must be seen on a *successful*
build has to go through [`mcpp::warning`](#warning--succeeding-and-still-being-heard-20268212);
stderr on success is discarded, which means the wrong channel is silent on
exactly the builds that needed the message.

**One `(name, version)` names one payload.** mcpp identifies an installed
package by that pair, so a repackaged rule that keeps its version string does
not trigger a reinstall and the consumer keeps running the old rule with no
diagnostic. Which numbering scheme to use is the author's call — versioning in
lock-step with the wrapped tool is legitimate when the two ship from one tag,
and tells a consumer something true — but a payload change is a version change.

**Test it through a consumer.** A rule is consumed only by a `build.mcpp`, so
compiling it proves nothing. Its test is an example project that depends on it,
builds, and asserts on the produced artefact.

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

## Host tools from a dependency (mcpp 2026.8.5.1+)

A package can build a binary its consumers need *at build time* — `protoc`, a
`grpc_cpp_plugin`, `flatc`, `moc`, a transpiler. Ask for it on the dependency:

```toml
[dependencies]
protobuf = { version = "35.1",   tools = ["protoc"] }
grpc     = { version = "1.83.0", tools = ["grpc_cpp_plugin"] }
```

Each name must be a `kind = "bin"` target of that package. mcpp builds it **for
the build machine** and hands `build.mcpp` its absolute path as
`MCPP_DEP_<PKG>_BIN_<TOOL>` — read it with `mcpp::dep_bin("protobuf", "protoc")`
(see [30 — build.mcpp](30-build-mcpp.md)).

Four properties worth knowing:

- **Always a host binary.** Under `mcpp build --target <triple>` the tool is
  still built for *this* machine, because a code generator has to run here. It
  is a separate, host-targeted sub-build — the tool package's own `[toolchain]`
  and its own dependency resolution apply, and none of it has to agree with
  the consuming build. That is safe precisely because an executable has no ABI
  contact with the consuming code.
- **One version axis.** The tool's version *is* the dependency's version, so
  a `protoc` that does not match its runtime is not expressible. (This is the
  problem with packaging the tool separately, and it is the failure mode that
  bites at run time rather than compile time.)
- **Default off.** Nothing is built unless someone asks; the cost is the
  consumer's to pay. A package gates the expensive part with
  `[features]` + `required_features` (protobuf's `protoc` needs libprotoc's
  ~157 extra TUs, which the runtime's users must not compile).
- **Cached globally**, keyed on package version × host toolchain × features ×
  its own dependency closure — built once per machine, not once per project.

### `[tools.overrides]` — use an existing binary

```toml
[tools.overrides]
"compat.protobuf:protoc" = "/usr/bin/protoc"
```

or, without editing the manifest (CI, distro packaging):

```bash
MCPP_TOOL_PROTOBUF_PROTOC=/usr/bin/protoc mcpp build
```

An override **skips the build entirely**. Every comparable system provides this
escape hatch (vcpkg's `VCPKG_HOST_TRIPLET`, CMake's `LLVM_NATIVE_TOOL_DIR`,
Qt's `QT_HOST_PATH`), and for the same reason: a tool that cannot be built from
source on this machine must not be a dead end. It is deliberately **not** part
of the cache key — an override is an escape hatch, not a reproducible input.

### `host-module = true` — reusable build rules as packages

A rule (say "run protoc over these `.proto` files") should be written once, not
copy-pasted into every consumer's `build.mcpp`. Ship it as an ordinary mcpp
library package and import it:

```toml
[dependencies]
protobufgen = { version = "0.1.0", host-module = true }
```

```cpp
// build.mcpp
import mcpp;
import protobufgen;
int main() { return protobufgen::generate({"schema"}) ? 0 : 1; }
```

mcpp compiles that package's lib-root module **for the host, in the same
command as `build.mcpp`** — which is what makes the BMI usable at all, since a
module interface is only importable by a compile that agrees with it on
standard, dialect and compiler identity.

Rules are therefore versioned, testable and distributable through the package
manager already in use, written in **C++** — no second language, which is the
whole point of `build.mcpp` existing.

**The module name is what the rule's source declares** (mcpp 2026.8.29.1+).
`export module acme.rules.protobuf;` is imported as `acme.rules.protobuf`,
whatever the package is called. Module names are authored API and do not mirror
package identity — the rule ordinary library packages have always followed.

Until 2026.8.29.1 the host-module path registered the bare `package.name`
instead, which made a divergent name build under GCC and fail under Clang and
MSVC: GCC's BMIs are implicit under `gcm.cache` and keyed by the declared name,
while the other two are handed an explicit `<name>=<bmi>` mapping. Package names
carry no C++ naming constraint as a result, and `grpc-rules` is a legal package
name again.

**Two rules may not declare one module name.** `import` addresses the module,
so two such packages are indistinguishable to the compiler, and their BMIs and
objects share a filename — the second overwrites the first and the surviving
object reaches the link twice. mcpp refuses this, naming both packages and both
interface paths. The check covers the rules one `build.mcpp` can see; it is not
an index-wide uniqueness rule, which `path` dependencies and private registries
would escape anyway.

**`mcpp.` is reserved for rules maintained by the mcpp project.** A module name
under that prefix from a package outside the `mcpp` namespace produces a
warning naming both, and the build proceeds. It is a warning because the engine
cannot decide who is official: a `path` dependency, a private mirror and an
internal fork are all legitimate and indistinguishable from here.

The lib root must be at `src/<name>.cppm` (or wherever `[lib] path` points); a
missing one is reported as *"host module 'x': no interface unit at …"*.

**A package may offer several rules, selected by features** (mcpp 2026.9.5.3+).
Every module interface unit among the package's resolved `[build] sources` —
including the sources a feature adds — is compiled as a host module under the
name it declares, the lib root first. A feature unit may import the lib root;
units are otherwise compiled alone, so they import `std`, `mcpp` and nothing
else. Only listed sources take part: the inferred `src/**` of a package that
declares no `sources` is not consulted, so a rule package published before this
release exposes exactly what it exposed then.

```toml
# the collection's manifest
[build]
sources = ["src/plugins.cppm"]                   # export module mcpp.plugins;

[features]
rules-cuda  = { sources = ["rules/cuda.cppm"] }  # export module mcpp.rules.cuda;
rules-spirv = { sources = ["rules/spirv.cppm"] } # export module mcpp.rules.spirv;
```

```toml
# a consumer
[build-dependencies.mcpp]
plugins = { version = "0.3.0", features = ["rules-spirv"], host-module = true }
```

**`[build-dependencies]`, not `[dependencies]`** — a rule package is the case
[04 §2.6.1](04-mcpp-toml.md) describes exactly: its library must never reach the target while its
rule is still wanted. The two axes are separate, so `host-module = true` says
*which build-time product* is wanted and the section says *whether the package
reaches the target*; a rule package answers "no" on the second axis, and the
section is where that is said. Written in `[dependencies]` it still works, and
that is precisely why the distinction has to be stated rather than enforced by
a failure.

The module set is the feature set: a unit whose feature is not active is not
compiled, and importing it fails as an unknown module. `mcpp:plugins` is the
collection the mcpp project maintains (repository `mcpp-community/mcpp-plugins`);
its members are named `mcpp.rules.<x>` for rule packages and `mcpp.tools.<x>`
for build-time utilities.

*Build-time only:* a `host-module = true` dependency is **not** compiled into
or linked with the target, and neither is anything it depends on. It exists to
run during `build.mcpp` and nowhere else. (Before 2026.8.5.2 it was also built
as an ordinary library, which made `import mcpp;` inside a rule fail: the
bundled module does not exist in that second compile. Until 2026.8.29.1 the
rule itself was excluded but its own `[dependencies]` were not, so they were
compiled and linked into the consumer's binary while the rule could not import
them.)

### A rule that depends on another rule (mcpp 2026.8.29.1+)

A rule declares what it needs in its own `[build-dependencies]`, and may import
any entry there marked `host-module = true`:

```toml
# inside the rule package's manifest
[build-dependencies]
globbing = { path = "../globbing", host-module = true }
```

```cpp
// the rule's own interface
export module tidyrule;
import std;
import mcpp;
import globbing;
```

mcpp compiles the inner rule first, in the same command and with the same
flags, so BMI agreement stays structural rather than checked.

The consumer may **not** import `globbing`: build-time provisions cross one
further edge only on a `reexport = true` edge, and mcpp enforces that rather
than leaving it to the compiler, which on GCC would allow the import and then
fail on someone else's machine.

*Limit:* one interface unit per host module. A library with implementation
units or several modules cannot yet be a rule's build dependency.

### `reexport = true` — a library standing up a toolchain for its user (2026.8.6.2+)

Everything above is declared by whoever *uses* the tool. That is the wrong
place when the knowledge belongs to a library: gRPC's code generation needs
protobuf's `protoc`, and no user of a gRPC package should have to know that.

`reexport = true` hands an edge's build-time provisions — its `tools`, its
`host-module`, and the dependency's directory — to **this package's own
consumers**:

```toml
# inside the grpc package's manifest
[feature-deps.codegen]
"compat.protobuf" = { version = "35.1",   tools = ["protoc"],            reexport = true }
grpc-plugin       = { version = "1.83.0", tools = ["grpc_cpp_plugin"],   reexport = true }
grpcgen           = { version = "1.83.0", host-module = true,            reexport = true }
```

Its user then writes one line, and imports the rule:

```toml
[dependencies]
grpc = { version = "1.83.0", features = ["codegen"] }
```

```cpp
// build.mcpp
import mcpp;
import grpcgen;
int main() { return grpcgen::generate_all() ? 0 : 1; }
```

- **Off by default, and deliberately not the edge's `visibility`.** `visibility`
  already defaults to `"public"`, so riding it would let any dependency at any
  depth put entries into the build program's tool namespace without saying so.
  Handing something to consumers is a supply-chain statement; it has to be
  written down.
- **One hop per declaration.** A re-exported provision reaches the consumers of
  the package that declared it. For it to travel further, the next package must
  re-export in turn — each package decides only what *it* hands on.
- **A feature may add a request to an already-declared dependency.** gRPC
  depends on protobuf unconditionally and its `codegen` feature adds
  `tools = ["protoc"], reexport = true` to that same edge. `tools` and
  `features` union, `host-module` and `reexport` OR together; `version` /
  `path` / `git` do not merge, so a feature still cannot silently override the
  unconditional entry's identity.
- **Visibility, not execution.** `dep_bin()` returns a path; whether anything
  runs is still the consumer's `build.mcpp`'s decision. Nothing changes about
  who builds the tool or how the tool store is keyed.
- **Unqualified names are resolved by a ladder, not by luck.** Once two
  libraries can re-export, both may offer the tail `protobuf`. The
  fully-qualified `MCPP_DEP_<NS>_<NAME>_BIN_<TOOL>` is always published; the
  bare spelling is bound to `mcpplibs.<x>`, else `compat.<x>`, else an
  unnamespaced `<x>`, else the single remaining candidate — and when it is
  contested mcpp says so instead of picking silently.

#### Older mcpp reading a manifest that uses this

An unrecognized dependency key is reported as a **degradation** and ignored
(mcpp 2026.8.6.2+), so a package written for a newer mcpp still loads and the
parts this reader understands still apply. Before that release it was a hard
load failure with a misleading message, which is why a published package could
not adopt a new key at all — the same property the index floor establishes:
data must not decide whether the program works.

Consequently a package that *relies* on `reexport` for its ergonomics still
needs a client new enough to implement it; what changed is that everything else
about that package keeps working on an older one.

#### Scoping a provision per platform

A package may declare a `bin` target on some platforms only. Because the
*library* now decides what is requested, an unconditional request turns an
unsupported platform into an error its user cannot edit away. Scope it:

```toml
[target.'cfg(not(windows))'.feature-deps.codegen]
"compat.protobuf" = { version = "35.1", tools = ["protoc"], reexport = true }
```

`[target.<sel>.feature-deps.<feature>]` (2026.8.6.2+) follows the same rules as
the other conditional dependency tables ([22 — The Target Side](22-target-side.md)). The **feature itself is
registered on every platform** — only what it pulls in is conditional — so
requesting it where no predicate matches is not an unknown-feature error.



## Current limitations

- **Runs on the host — including under cross** (mcpp 0.0.95+). Under
  `mcpp build --target <triple>` the program is compiled with a host-resolved
  toolchain, runs on the host, and sees `MCPP_TARGET` = the cross triple.
  For purely declarative target gating, `[target.'cfg(...)']` tables remain
  the first choice — see [04 - mcpp.toml Manifest Guide](04-mcpp-toml.md).
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
