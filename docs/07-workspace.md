# 07 — Workspaces

**Reader:** an author whose repository holds more than one package.

**The question this chapter answers:** how do several packages become one build,
and what does a member share with the others.

**Not here:** publishing those packages, which is
[11 — Publishing a Library](11-publishing-a-library.md). Before:
[06 — Features and Capabilities](06-features-and-capabilities.md). After:
[08 — Testing](08-testing.md).

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
myproject.core = { path = "libs/core" }
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
myproject.core = { path = "../core" }

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

An entry that says `.workspace = true` and that no workspace resolves is
refused wherever the package enters a build (the root, a member selected with
`-p`, a `path`, `git` or index dependency), naming the table and the entry
(mcpp 2026.9.27.1+). It is resolved against the `[workspace.dependencies]` of
the workspace whose `members` list the package; a workspace root that carries
its own `[package]` resolves its own entries the same way.

## 4. Inheriting Toolchain and Build Configuration

A table under `workspace.` speaks to every member; a table outside it speaks
about the package of the manifest that holds it (mcpp 2026.10.10.1+). To share
a table with every member, write it with the `workspace.` prefix:

| A package's own table | Shared with every member |
|---|---|
| `[package]` (metadata, `standard`) | `[workspace.package]` |
| `[build]` | `[workspace.build]` |
| `[dependencies]` entries | `[workspace.dependencies]` + `x.workspace = true` (§3) |
| `[toolchain]` | `[workspace.toolchain]` |
| `[indices]` | `[workspace.indices]` |
| `[profile.<name>]` | `[workspace.profile.<name>]` |
| `[target.<selector>]` scalars, `.build`, `.abi`, `.runtime`, `.xlings.workspace` | `[workspace.target.<selector>]` and the same subtables |
| `[xlings.workspace]` | `[workspace.xlings.workspace]` |

A `[workspace.X]` table has the keys, subtables and selectors of `X`. Keys
that describe one package are not shared and are refused there: `[targets]`,
`[features]`, `[resources]`, `[test]`, a selector's `.targets` and
`.dependencies`, `requires_abi`, `allow_host_libs`, and the `sources` and
per-glob `flags` of a `.build` table. An unknown table under `[workspace]` is
refused.

```toml
# workspace root
[workspace]
members = ["libs/core", "apps/server"]

[workspace.package]
mcpp = ">=2026.10.10.1"

[workspace.toolchain]
default = "gcc@16.1.0"

[workspace.target.x86_64-linux-musl]
toolchain = "gcc@16.1.0"
linkage   = "static"

[workspace.target.'cfg(os = "windows")'.build]
dialect_cxxflags = ["-DARCH_COMPAT=1"]
```

```toml
# a member overrides the toolchain for itself
[toolchain]
default = "llvm@23.1.3"
```

Configuration precedence (highest to lowest):

1. Command-line arguments (`--target`, `--toolchain`, `--profile`, `--static`)
2. Declarations in the member `mcpp.toml`
3. The workspace's `[workspace.X]` tables
4. Global configuration (`~/.mcpp/config.toml`)
5. Built-in defaults

`--toolchain` that differs from a toolchain the manifest declares prints a
warning naming both and the file and key that declared the replaced one; it
does not fail under `--strict`.

`[workspace.toolchain]`, `[workspace.indices]`, `[workspace.profile.<name>]`
and the scalar rows of `[workspace.target.<triple>]` choose the compiler, the
indices, the profile and the target rows for a whole graph, so a member takes
them where it is the root of a build: built from the workspace, with `-p`, or as
a host tool of another package. A member reached as a dependency takes them
from that build's root. The rows of `[workspace.target.<selector>]` --
`.build`, `.abi`, `.runtime`, `.xlings.workspace` -- and `[workspace.xlings]`
reach a member in every position (§4.1).

A conditional row counts as it evaluates for the target being built: a
`dialect_cxxflags` or `.abi` row for another target does not separate a member
from the others (§5.4), and one that holds is applied when the member is built
under the workspace's root.

A build without `--target` targets the host, and `[target.<host-triple>]`
applies to it as `--target <host-triple>` would.

`[feature-xlings.<f>]` entries are not shared, because a feature belongs to
the package that declares it.

**A workspace that uses `[workspace.X]` declares the engine floor.** An mcpp
older than 2026.10.10.1 ignores these tables and builds the members without
them. `[workspace.package] mcpp = ">=2026.10.10.1"` makes an older mcpp refuse
instead; without it the build prints a note.

**The root's own tables, read by position.** On a workspace root, a
`[toolchain]`, `[indices]`, `[profile.<name>]`, a `[target.<triple>]` scalar row
or an `[xlings]` / `[target.<selector>.xlings]` entry that has no `[workspace.X]`
spelling still reaches the members as it did before 2026.10.10.1: a member's own
`[toolchain]` replaces the root's whole, a member's row or profile of the same
name replaces the root's whole, and `[indices]` reaches only a member that
declares none. Each use prints a `manifest/workspace-position` warning with the
`[workspace.X]` spelling; on a root with `[package]` the tables are the root
package's own and the warning appears only for a member that received a value
from them. This reading is removed in mcpp 1.0.0. On a root without
`[package]`, writing a table both ways is refused.

**A root without `[package]` holds no package.** `[build]`, `[targets]`,
`[dependencies]`, `[features]`, `[resources]`, `[test]`, `[hooks]`, `[runtime]`
and a selector's `.build`, `.abi`, `.runtime`, `.targets` and `.dependencies`
written there act on nothing; each prints a warning naming the shared spelling
where there is one, and `--strict` refuses.

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
| vectors (`cxxflags`, `cflags`, `ldflags`, `dialect_cxxflags`, `include_dirs`, …) | append, **workspace first** |
| `defines` | a set keyed by macro name: a member entry for an inherited name replaces it, and `!NAME` removes it (2026.9.25.1+) |
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

**What an appended vector overrides.** The member's words follow the
workspace's on the command line. A flag the compiler resolves last-wins is
therefore overridden by restating it: `-fexceptions` after `-fno-exceptions`,
`-Wno-x` after `-Wx`, `-O2` after `-O0`. Include directories are searched in
order, so a header in a workspace `include_dirs` directory is found before a
header of the same name in the member's. A macro is overridden through
`defines`, which emits one `-DNAME` word per name:

```toml
# workspace root
[workspace.build]
defines = ["LOG_LEVEL=1", "TRACE"]

# member
[build]
defines = ["LOG_LEVEL=3", "!TRACE"]   # compiles with -DLOG_LEVEL=3 and no TRACE
```

A `defines` entry also replaces a `-DNAME` word for the same name written in
`cflags` or `cxxflags` of the same package. `!NAME` requires mcpp 2026.9.25.1 or
later; an older mcpp passes it to the compiler as `-D!NAME`, which is an error.

**Every member receives the inherited values exactly once, in every position**
(2026.9.25.1+): as the package a command builds (`-p <member>`, or a command run
inside the member), as another member's `path` dependency, and as a member of a
git-hosted workspace consumed through `git` (§6). A member reached as a dependency
also resolves its own `x.workspace = true` entries.

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

**Named tables merge key by key.** A member's `[profile.release]` and
`[workspace.profile.release]` combine: a key the member wrote is the member's, a
list has the workspace's entries first. `[target.<triple>]` rows combine the
same way. A profile is one value per graph, so members that share it are
planned and compiled together.

**The root package is a member.** In a workspace whose root carries
`[package]`, the root package receives `[workspace.package]`,
`[workspace.build]` and every `[workspace.X]` table once, as every other member
does, so its commands are the same in every selection. The root manifest holds
two kinds of tables:

| Tables | Owner | Effect on the root package | Effect on other members |
|---|---|---|---|
| `[workspace]` and every `[workspace.X]` | the workspace | received, its own declarations first | received; `[workspace.dependencies]` through `x.workspace = true` |
| every other table (`[package]`, `[build]`, `[toolchain]`, `[target.<selector>]`, `[profile.<name>]`, ...) | the root package | its own | none (the position reading above excepted, until 1.0.0) |

The root package's `[build] ldflags` are its own as well: they reach its own
images, and neither the members' images nor a dependency's shared library.

### 4.2 One standard for the whole module graph

A C++ module graph has exactly one standard: BMIs are not compatible across
levels, so the root package's `standard` is applied to every package in the
graph, including dependencies. A dependency's own `standard` is not applied.

One kind of package is the exception. A package that provides the C++ layer
(the standard library itself) and states `standard` compiles each of its
translation units that neither provides nor imports a module at exactly that
level; its module units stay at the graph's level
([22 — Target Side](22-target-side.md), "The Standard Library's Own Language
Level"). No BMI crosses those units, so the rule above is not broken, and it is
what lets a c++20 project use a standard library whose sources are written for
C++23.

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
is promoted to an error by `--strict`. A C++-layer provider whose statement is
applied as described above is not reported. It is reported only for manifests the
project author controls (the root package, workspace members, and `path`
dependencies): a package resolved from an index carries a `standard` written by
a descriptor generator rather than by the person reading the message.

## 5. Build Commands

### 5.1 Building & testing from the Workspace Root

```bash
mcpp build                  # virtual workspace → builds ALL members; rooted → the root package
mcpp build -p server        # build a specific member and its dependencies
mcpp build -p server -p cli # build several members, in one plan
mcpp build --workspace      # build every member explicitly
mcpp build --workspace --exclude legacy   # every member but legacy
mcpp test                   # virtual workspace → tests ALL members; rooted → the root package
mcpp test  -p core          # test a single member
mcpp test  -p core -p http  # test several members: one plan, one build, one report per member
mcpp test  --workspace      # test every member (one report per member; continues past failures)
```

At a **virtual** workspace root (only `[workspace]`, no `[package]`), bare
`mcpp build` / `mcpp test` act on **all** members. At a **rooted** workspace
(`[package]` + `[workspace]`), they act on the root package; `--workspace`
acts on the root package and every member. `mcpp test` over several members
plans them together and builds once (§5.4), then runs each member's
`tests/**/*.cpp` — discovery is scoped per member, so two members may each have
a `tests/main.cpp` without colliding.

### 5.2 Building from a Member Subdirectory

```bash
cd libs/http
mcpp build                  # auto-detects the workspace and builds the current member
```

mcpp searches upward from the current directory; if it finds an `mcpp.toml` containing `[workspace]` and the current directory is listed in `members`, it automatically enters workspace mode and inherits the workspace configuration. The command then acts as `mcpp build -p <this member>` at the workspace root: it builds in the workspace's build directory (§6).

### 5.3 The `-p, --package` Option

`-p` works with `build`, `test`, `run`, `mcpp emit build-database` and other
commands to select the target member. Its value is resolved in one order,
because the option names a *package*:

1. a member's qualified name, `<namespace>.<name>` (only meaningful for a
   member that declares a namespace);
2. otherwise, a member's bare `package.name` — refused, naming every match, if
   two or more members share it;
3. otherwise, a member's path as written in `[workspace] members`, or its
   directory's last segment (the historical spellings, kept as a fallback).

```bash
mcpp build -p server        # matches apps/server (by directory or package name)
mcpp test -p core           # matches libs/core
mcpp run -p server -- --port 8080
```

A value that is one member's package name and a different member's directory
selects the member named by the package, with a warning naming the other one —
the option promises a package, so an exact package-name match outranks a
directory that merely happens to share the spelling.

#### Several members (mcpp 2026.10.1.1+)

`-p` may be repeated on `build`, `test` and `mcpp emit build-database`. Each
value names one member, resolved by the order above, and the command acts on
all of them. The selection is a **set**, kept in `[workspace] members` order
whatever order `-p` was written in; a member named twice, by two spellings, is
selected once.

```bash
mcpp build -p server -p cli         # the same members as -p cli -p server
mcpp test  -p core -p http
```

A value that names no member is refused before anything is planned, and the
refusal lists the members. `-p` together with `--workspace` is refused as
well: the two state two selections, and neither is taken over the other.
`mcpp run` executes one program, so it acts on one
member: a second `-p` is refused, naming every member asked for, and is never
read as "the last one".

#### Leaving members out: `--exclude`

```bash
mcpp build --workspace --exclude legacy      # every member but legacy
mcpp test  --exclude legacy --exclude bench  # at a virtual root: every member but two
```

`--exclude <name>` may be repeated on `build`, `test` and `mcpp emit
build-database`. Its value is resolved like `-p`, and it removes members from
a selection of every member: `--workspace`, or a virtual root without `-p`.
It is refused, before anything is planned, together with `-p`, where neither
form applies (inside a member, or at a rooted root without `--workspace`), for
a name that matches no member, and when it leaves no member.

`--workspace` (on `build`, `test` and `mcpp emit build-database`) is the fan-out
form: it acts on **every** member. `mcpp test --workspace` reports each member
separately and continues past a failing member, exiting non-zero if any member
failed — ideal as a single, shell-free CI step for a workspace that tests many
libraries. A member fails alone whatever failed in it: a test, its package's
build, or its plan (a member that cannot be planned is planned without, and the
others are planned together again).

#### The fan-out report

```
   Workspace building 97 members: libs/core, libs/http, ...
   Workspace built members libs/core, libs/http, ... in 120.40s; slowest: obj/libs/jsc/tests/jsc.o 88.0s
   Workspace testing member 'libs/core' (3/97)
test_paths ... ok (0.31s)
 test result ok. 7 passed; 0 failed; finished in 121.10s (build 120.40s + run 0.60s)
   Workspace member 'libs/core' (3/97) ok — 7 passed, run 0.60s
...
 workspace result ok. 97 member(s); 412 passed; 0 failed; finished in 355.20s
    slowest: libs/install 32.2s, libs/http 24.1s
```

`M/N` progress and per-test durations. The members of one configuration are
built once, so the build is reported once, in the group's line: its members, its
wall time, and the edges that took the most of it. That is the signal the
per-member split used to give: a member whose link takes 90 seconds, and not its
tests, is named by the group's `slowest:` edges. Each member's own line states
the time of its **run**, and the final `slowest:` line ranks members by it.

`--message-format json` carries the same data as NDJSON. Every test record is
member-qualified (`"member"`), a `group_build` record states each group's build
before its first test record, each member's summary names its group
(`build_group`), and the stream ends with a `workspace_summary` record naming
the failed and not-run members — a bare test name is ambiguous the moment two
members both have a `smoke`. The fields are in
[50 — Machine-Readable Output](50-machine-output.md#mcpp-test---message-format-json--the-test-stream).

#### Bounding the fan-out

```bash
mcpp test --workspace --timeout 60        # per-test RUN deadline (default 300)
mcpp test --workspace --build-timeout 300 # per-ninja-drive deadline (default 0 = no limit)
mcpp test --workspace --workspace-timeout 1800   # whole fan-out (default 0 = no limit)
```

The members' runs are serial, so an unbounded member stalls every member after
it. All three deadlines report rather than abort: a timed-out test fails that
test and the fan-out continues; a timed-out build fails the members that waited
for it; `--workspace-timeout` stops the fan-out and lists what did not run
instead of leaving the CI job to kill the process (which discards everything it
had to say).

`--workspace-timeout` bounds the runs, and it is measured from the start of the
command: it is checked before each member's tests start, and a member not
started by then is listed as not run (2026.10.1.1+). The members of a group are
built in one step, which the deadline cannot interrupt, so a workspace whose
build alone exceeds it runs to the end of that build, bounded by
`--build-timeout`, and then starts no member. Before 2026.10.1.1 each member was
built and run in turn, and the deadline could stop the fan-out between builds.

### 5.4 One graph per configuration (mcpp 2026.9.29.1+)

A command on a workspace plans its members together: the selected members and
everything they depend on are one build graph, with one `build.ninja`, and a
member that several members use is compiled once.

- **Configurations.** Members are built in one graph when they share their
  toolchain request, target, C++ standard, `dialect_cxxflags`, C++ runtime,
  `linkage`, profile, indices and the other `[build]` values that apply to a
  whole graph. Members that differ in one of them are built in separate
  graphs, at the same time, sharing the command's jobs. A conditional row
  (`[target.<selector>.build] dialect_cxxflags`, `[target.<selector>.abi]`,
  a `[target.<selector>]` scalar) counts as it evaluates for the target
  being built: `--target`, else the member's `[build] target`, else the host
  (2026.10.10.1+). A relative path a member writes, such as its own
  `[indices]` path, is read from the member's directory.
- **Selection.** `--workspace`, and a virtual root without `-p`, select every
  member. `-p X`, and a command run in X's directory, plan X and what X
  reaches; `-p X -p Y` plans both together, as one selection (§5.3). The
  selections share the build directory, and a unit is compiled by the same
  command in every selection that holds it: the command depends on the unit's
  package, the packages that package reaches, the features the selection
  activates for it and the declarations a selected member holds as the root,
  and on nothing else in the graph (2026.10.1.2+). So `mcpp build --workspace`
  followed by `mcpp build -p X` compiles nothing, and a package is compiled
  again only when its active features, or the root declarations of the
  selected members, differ between the two commands. Before 2026.10.1.2,
  three facts about the whole graph also reached other members' commands: a
  module name that two members provide, a file that two members list from
  outside their own directories, and a member that builds a shared library.
- **Flags.** A member's `cflags`, `cxxflags`, `ldflags` and defines apply to
  that member's commands. Editing them recompiles that member and what
  imports it; the build directory stays the same.
- **Features.** `--features f` activates `f` in each selected member that
  declares it, and is refused when no selected member declares it. A package
  that several members of one configuration use is compiled once, with the
  union of the features they ask for; a package that members of two
  configurations use is compiled once in each.
- **The root's declarations.** Each selected member declares as the root did
  when it was planned alone: its `path` or `git` override of a dependency wins
  over another package's declaration, `linkage` on its dependency edges is
  honoured, and its registry dependencies are considered for the index
  refresh (2026.9.30.2+). Two selected members that disagree about one
  dependency's checkout (its kind or its reference) or its link form are
  refused, naming both. See
  [05 — When two declarations of one dependency disagree](05-dependencies.md#when-two-declarations-of-one-dependency-disagree).
- **Hooks.** The `[hooks]` of every selected member run around the build, in
  member order.
- **Resources.** A member's `[resources]` and `windows_code_page` are
  compiled against the member's directory and include directories and embedded
  into that member's programs and shared libraries only (2026.9.29.2+).
- **Build programs.** The members' build programs run dependencies first, and
  a program's result is reused by every command whose inputs to it are
  unchanged, whichever members the command selects (2026.9.29.5+). Their
  compiles run at the same time, up to the job count, and only their runs
  follow that order, so the plan is the one a serial build writes; a host
  module that several programs import is compiled once for all of them
  (2026.10.1.1+; see [30 — Build programs](30-build-mcpp.md)).
- **Tests.** `mcpp test` over several members plans as `build` does: once per
  configuration, with each member's tests, so a package the members share is
  compiled once, its build program runs once, and its features are the union
  the selection asks for (2026.10.1.1+). The configuration's packages and test
  binaries are built once; then each member's tests run, in member order, with
  that member's runtime directories and no other member's. A test run after
  `mcpp build --workspace` compiles nothing the build compiled, unless a
  dev-dependency changes a package's features. `mcpp test` of one member is one
  plan of that member, as it always was.
- **Compile database.** `mcpp build --configure-only` and `mcpp emit
  build-database` plan as the build does, one plan per configuration with each
  member's tests, so a package the members share is described once per
  configuration. A command that planned several configurations writes the root
  `compile_commands.json` once, as the union of their databases (2026.9.29.5+).
- **No-op builds.** A command repeated with nothing changed is answered by one
  check per configuration, without planning.
- **Module names.** A module name is unique within one program, not within one
  graph (2026.9.30.2+). Two members that share no program may each provide a
  module of the same name, and one `--workspace` command builds both; a member
  that links both is refused. See
  [05 — One module per name in each program](05-dependencies.md#one-module-per-name-in-each-program-mcpp-20269302).

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

A workspace builds at its root (2026.9.29.1+):

```
myproject/
├── mcpp.lock                               # one lock for the workspace
└── target/<triple>/<configuration>/
    ├── build.ninja, compile_commands.json  # one graph and one database per configuration
    ├── obj/<package>/                      # intermediate objects of every package
    ├── gcm.cache/<package>/                # each package's BMIs (pcm.cache with clang)
    ├── modmap/                             # the module maps of the units that import them
    └── bin/
        ├── server/                         # a member's products: bin/<package name>/
        │   ├── server
        │   └── libfoo.so                   # shared libraries and runtime files it loads
        └── ...
```

- A member's programs and shared libraries are in its **product directory**,
  `bin/<package name>/`, with the shared libraries, DLLs and deployed files
  its programs load placed beside them. Two members with the same package
  name use `bin/<namespace>.<name>/`. A rooted workspace's own package keeps
  `bin/`.
- A shared library placed in several product directories is one file with
  several names where the file system supports hard links; elsewhere it is
  copied.
- `compile_commands.json` at the workspace root covers every member that has
  been built or configured.
- `mcpp.lock` at the workspace root records the resolution of every member.
  `mcpp build --workspace` writes the whole record; `mcpp build -p X` updates
  the entries of X's graph.
- A member's build program writes to `<member>/target/.build-mcpp/`. The host
  modules the programs import are compiled into the workspace's own
  `target/.build-mcpp/host-modules/`, once for all of them.
- Build directories a member held under its own `target/` with an earlier mcpp
  are not read; `mcpp clean --stale` removes them.

A project outside the workspace reaches a member of a git-hosted workspace by
the member's identity: `myproject.http = { git = "...", rev = "..." }` selects
`libs/http` among the root manifest's `members`, at the same commit, and the
member inherits `[workspace.package]` as it does here (mcpp 2026.9.16.1+; see
[05 — Dependencies](05-dependencies.md)). It also inherits its repository's
`[workspace.build]` and resolves its `x.workspace = true` entries against that
repository's `[workspace.dependencies]` (2026.9.25.1+), so the same commit
compiles the same way in its own checkout and in a consumer's graph. A member
that an index descriptor points at inside a tag tarball receives the tarball's
workspace in the same way. Publishing a member with `mcpp publish` writes the
inherited values into the published manifest
([11 — Publishing a Library](11-publishing-a-library.md)).

## 7. Relationship to C++ Modules

Workspaces work in concert with the C++23 module mechanism:

- **Interface visibility is controlled by the language** — `export module` and `import` statements determine a module's public interface; the workspace imposes no additional visibility restrictions.
- **Module names are chosen by the library author** — the workspace does not require module names to match the package name or namespace.
- **Partitions are for internal organization** — a partition imported via `import :internal;` (without `export`) is invisible to consumers, with no build-tool involvement required.

## 8. Complete Example

See [`examples/04-workspace/`](../examples/04-workspace/) for a complete, runnable example of a three-member workspace.

