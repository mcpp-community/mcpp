# Workspace

A workspace organizes multiple related mcpp packages (libraries or applications) within a single repository. Member packages share a unified set of dependency versions and toolchain settings while each keeping its own `mcpp.toml` project file.

## 1. Overview

Workspaces address the following problems:

- **Unified dependency-version management** — multiple sub-packages use the same versions of third-party dependencies, avoiding duplicate declarations and version drift.
- **Shared toolchain configuration** — declare the toolchain once at the workspace root; members inherit it or override it as needed.
- **Multi-package co-development** — libraries and applications are developed in the same repository and reference one another through `path` dependencies.

A workspace does not change how dependencies are declared. Members reference one another through the existing `path = "..."` mechanism, exactly as in a non-workspace project.

## 2. Project File Structure

### 2.1 The Workspace Root

Declare `[workspace]` in the `mcpp.toml` at the repository root:

```toml
[workspace]
members = [
    "libs/core",
    "libs/http",
    "apps/server",
]
```

`members` lists the relative path of each member package; every such path must contain its own `mcpp.toml`.

The optional `exclude` field excludes specific paths:

```toml
[workspace]
members = ["libs/*"]
exclude = ["libs/experimental"]
```

### 2.2 Virtual Workspaces vs. Root-Package Workspaces

**Virtual workspace**: the root `mcpp.toml` contains only `[workspace]` and no `[package]`. The root produces no build artifacts and serves purely as a management node.

```toml
# Virtual workspace — [workspace] only
[workspace]
members = ["libs/core", "apps/server"]
```

**Root-package workspace**: the root `mcpp.toml` contains both `[package]` and `[workspace]`. The root itself is also a buildable package.

```toml
[workspace]
members = ["libs/core"]

[package]
name    = "myapp"
version = "0.1.0"

[dependencies]
core = { path = "libs/core" }
```

### 2.3 Member Project Files

Each member maintains its own `mcpp.toml`, structured just like a regular project:

```toml
# libs/core/mcpp.toml
[package]
namespace = "myproject"
name      = "core"
version   = "0.1.0"

[targets.core]
kind = "lib"
```

Members reference one another through `path` dependencies:

```toml
# libs/http/mcpp.toml
[package]
namespace = "myproject"
name      = "http"
version   = "0.1.0"

[dependencies]
core = { path = "../core" }

[dependencies.compat]
mbedtls.workspace = true
```

## 3. Inheriting Dependency Versions

Declare dependency versions centrally under `[workspace.dependencies]`; members inherit them with `.workspace = true`:

```toml
# root mcpp.toml
[workspace.dependencies]
cmdline = "0.0.2"
mcpplibs.capi.lua = "0.0.3"  # exact selector: (mcpplibs.capi, lua)

[workspace.dependencies.compat]
mbedtls = "3.6.1"
gtest   = "1.15.2"
```

```toml
# member mcpp.toml
[dependencies.compat]
mbedtls.workspace = true    # inherits version → "3.6.1"

[dev-dependencies.compat]
gtest.workspace = true      # inherits version → "1.15.2"
```

A member can override an inherited version:

```toml
[dependencies.compat]
mbedtls = "4.0.0"          # override; does not use the workspace version
```

## 4. Inheriting Toolchain and Build Configuration

The workspace root's `[toolchain]` and `[target.<triple>]` settings are automatically inherited by all members. A member can override them in its own project file.

Configuration precedence (highest to lowest):

1. Command-line arguments (`--target`, `--static`)
2. Declarations in the member `mcpp.toml`
3. Declarations in the workspace-root `mcpp.toml`
4. Global configuration (`~/.mcpp/config.toml`)
5. Built-in defaults

```toml
# workspace root
[toolchain]
default = "gcc@16.1.0"

[target.x86_64-linux-musl]
toolchain = "gcc@16.1.0"
linkage   = "static"
```

```toml
# a member overrides the toolchain
[toolchain]
default = "llvm@20.1.7"
```

### 4.1 `[workspace.package]` and `[workspace.build]`

Package metadata and build flags shared by every member are declared once at the
workspace root:

```toml
[workspace]
members = ["libs/core", "libs/http", "apps/server"]

[workspace.package]
standard = 26                  # or "c++26"; both spellings are accepted
version  = "0.4.2"
license  = "Apache-2.0"
authors  = ["example"]

[workspace.build]
cxxflags         = ["-Wall", "-Wextra"]
dialect_cxxflags = ["-fno-exceptions"]
```

A member then declares only what is its own:

```toml
[package]
name = "core"
# standard, version, license and authors are inherited;
# [workspace.build] cxxflags are inherited
```

**The merge rule.**

| kind | rule |
|---|---|
| scalars (`standard`, `version`, `license`, `c_standard`, `linkage`, …) | the member wins **when it declared the key**; otherwise the workspace value applies |
| vectors (`cxxflags`, `ldflags`, `defines`, `dialect_cxxflags`, `include_dirs`, …) | append, **workspace first** — so a member's own flag comes later on the command line, where it wins |
| `[workspace.dependencies]` | explicit opt-in per dependency, `x.workspace = true` (§3) |

"Declared" means the key was written, not that its value differs from the
default. A member that deliberately pins `standard = "c++23"` under a
`[workspace.package] standard = 26` keeps c++23; a member that says nothing gets
c++26. Those two are the same value and opposite intents, which is why the
distinction is recorded rather than inferred.

Scalars and vectors are inherited **implicitly**, without a per-key opt-in. The
drift a workspace exists to prevent is a member that forgot to opt in, so
inheritance is the default and overriding is what has to be stated.
Dependencies keep their explicit opt-in because a dependency is an edge in the
resolution graph: inheriting one implicitly would change what a member resolves
without its own manifest naming it.

**`version` may be omitted by a member** when `[workspace.package]` supplies it.
It remains required overall — a member with neither is refused, naming both the
member and the workspace key that would have supplied it.

**Not everything is inheritable.** `[workspace.build] allow_host_libs` is
refused. It disables the hermetic-link check for a specific artifact, and a
workspace root able to set it once would disable that check for members added
later by someone who never read the root manifest. Keys that describe *how to
build* are inheritable; keys that describe *which safety check not to run* stay
with the package whose artifact it is. Any other unknown key in
`[workspace.package]` / `[workspace.build]` is refused too, rather than ignored:
a key that is silently dropped from a table whose whole purpose is propagation
produces a workspace that looks configured and is not.

**There is no `[workspace.target.<triple>]`.** A plain `[target.<triple>]` block
in the workspace root is already inherited by every member, per triple, with the
member winning. A second spelling for the same capability would be surface with
no function.

### 4.2 One standard for the whole module graph

A C++ module graph has exactly one standard: BMIs are not compatible across
levels, so the root package's `standard` is applied to every package in the
graph, including dependencies. A dependency's own `standard` is not applied.

When a dependency **declares** a level higher than the graph is built at, mcpp
reports it before compiling:

```
warning: dependency `render` declares standard = "c++26", and this graph is
         built at c++23
  impact: a C++ module graph has one standard, so the dependency's declaration
          is not applied and its sources are compiled at the graph's level
  hint:   raise the consumer's standard to "c++26", or declare it once for
          every member:

            [workspace.package]
            standard = "c++26"
```

This is a warning rather than an error — such a build usually succeeds, and it
is promoted to an error by `--strict`. It is reported only for manifests the
project author controls (the root package, workspace members, and `path`
dependencies): a package resolved from an index carries a `standard` written by
a descriptor generator rather than by the person reading the message.

## 5. Build Commands

### 5.1 Building & testing from the Workspace Root

```bash
mcpp build                  # virtual workspace → builds ALL members; rooted → the root package
mcpp build -p server        # build a specific member and its dependencies
mcpp build --workspace      # build every member explicitly
mcpp test                   # virtual workspace → tests ALL members; rooted → the root package
mcpp test  -p core          # test a single member
mcpp test  --workspace      # test every member (one report per member; continues past failures)
```

At a **virtual** workspace root (only `[workspace]`, no `[package]`), bare
`mcpp build` / `mcpp test` act on **all** members. At a **rooted** workspace
(`[package]` + `[workspace]`), they act on the root package; use `--workspace` to
include all members. `mcpp test --workspace` builds + runs each member's
`tests/**/*.cpp` independently — discovery is scoped per member, so two members may
each have a `tests/main.cpp` without colliding.

### 5.2 Building from a Member Subdirectory

```bash
cd libs/http
mcpp build                  # auto-detects the workspace and builds the current member
```

mcpp searches upward from the current directory; if it finds an `mcpp.toml` containing `[workspace]` and the current directory is listed in `members`, it automatically enters workspace mode and inherits the workspace configuration.

### 5.3 The `-p, --package` Option

`-p` works with `build`, `test`, `run`, and other commands to select the target member. Its value is either the last path segment of a member's directory name or the full relative path:

```bash
mcpp build -p server        # matches apps/server
mcpp test -p core           # matches libs/core
mcpp run -p server -- --port 8080
```

`--workspace` (on `build` and `test`) is the fan-out form: it acts on **every**
member. `mcpp test --workspace` reports each member separately and continues past a
failing member, exiting non-zero if any member failed — ideal as a single,
shell-free CI step for a workspace that tests many libraries.

#### What the fan-out reports

```
   Workspace testing member 'libs/core' (3/97)
test_paths ... ok (0.31s)
 test result ok. 7 passed; 0 failed; finished in 9.50s (build 8.90s + run 0.60s)
   Workspace member 'libs/core' (3/97) ok — 7 passed in 9.50s
...
 workspace result ok. 97 member(s); 412 passed; 0 failed; finished in 355.20s
    slowest: libs/jsc 93.5s, libs/install 32.2s, libs/http 24.1s
```

`M/N` progress, per-test durations, and a per-member time split into **build** vs
**run**. The split is the useful part: a member whose tests take milliseconds but
whose link takes 90 seconds looks identical to a slow test suite in a single merged
number, and only one of those is worth investigating.

`--message-format json` carries the same data as NDJSON. Every test record is
member-qualified (`"member"`), and the stream ends with a `workspace_summary`
record naming the failed and not-run members — a bare test name is ambiguous the
moment two members both have a `smoke`.

#### Bounding the fan-out

```bash
mcpp test --workspace --timeout 60        # per-test RUN deadline (default 300)
mcpp test --workspace --build-timeout 300 # per-ninja-drive deadline (default 0 = no limit)
mcpp test --workspace --workspace-timeout 1800   # whole fan-out (default 0 = no limit)
```

The fan-out is serial, so an unbounded member stalls every member after it. All
three deadlines report rather than abort: a timed-out test fails that test and the
fan-out continues; a timed-out build fails that member; `--workspace-timeout` stops
the fan-out and lists what did not run instead of leaving the CI job to kill the
process (which discards everything it had to say).

## 6. Directory Layout

The recommended directory layout for a workspace:

```
myproject/
├── mcpp.toml               # [workspace] declaration
├── libs/
│   ├── core/
│   │   ├── mcpp.toml       # [package] namespace="myproject" name="core"
│   │   └── src/
│   │       └── core.cppm   # export module myproject.core;
│   └── http/
│       ├── mcpp.toml
│       └── src/
│           └── http.cppm   # export module myproject.http;
└── apps/
    └── server/
        ├── mcpp.toml
        └── src/
            └── main.cpp    # import myproject.http;
```

Each member's build artifacts live under its own `target/` subdirectory.

## 7. Relationship to C++ Modules

Workspaces work in concert with the C++23 module mechanism:

- **Interface visibility is controlled by the language** — `export module` and `import` statements determine a module's public interface; the workspace imposes no additional visibility restrictions.
- **Module names are chosen by the library author** — the workspace does not require module names to match the package name or namespace.
- **Partitions are for internal organization** — a partition imported via `import :internal;` (without `export`) is invisible to consumers, with no build-tool involvement required.

## 8. Complete Example

See [`examples/04-workspace/`](../examples/04-workspace/) for a complete, runnable example of a three-member workspace.
