# 05 — Dependencies and Resolution

**Reader:** an author whose build now contains more than their own code.

**The question this chapter answers:** where does a dependency come from, what
does a version constraint mean, and what happens when two of them disagree.

**Not here:** what makes two packages the same package — that is
[SPEC-001](specs/package-identity.md), which this chapter applies rather than
restates — and how to publish one, which is
[11 — Publishing a Library](11-publishing-a-library.md).

Before: [04 — The mcpp.toml Manifest](04-mcpp-toml.md) is where these tables
live among the others. After: [06 — Features and Capabilities](06-features-and-capabilities.md)
is how a dependency becomes optional.

## `[dependencies]` — Runtime Dependencies

**The recommended form is a dotted selector with an exact version.** The dotted
form names one identity — everything before the final dot is the namespace, the
final segment is the package name — so what resolves does not depend on which
namespaces happen to be configured.

```toml
[dependencies]
compat.gtest               = "1.15.2"
imgui.core                 = "0.0.1"
imgui.backend.glfw_opengl3 = "0.0.1"
mcpplibs.capi.lua          = "0.0.3"
```

A bare name is accepted and resolves through the default namespace
(`mcpplibs`), which is convenient in a project that uses only that namespace and
ambiguous in one that does not:

```toml
[dependencies]
cmdline = "0.0.2"        # resolves to mcpplibs.cmdline
```

<details>
<summary>Equivalent spelling: the namespace sub-table</summary>

Groups entries under one namespace. It resolves to exactly the same identities
as the dotted form and is worth using when many dependencies share a namespace.

```toml
[dependencies.mcpplibs]
cmdline   = "0.0.2"
tinyhttps = "0.2.2"
llmapi    = "0.2.5"

[dependencies.compat]
glfw = "3.4"                    # Explicit namespace; no fallback search
```

</details>

```toml
# Path dependency (local development)
[dependencies]
mylib = { path = "../mylib" }
```

```toml
# Git dependency — pick exactly one of tag / branch / rev
[dependencies]
mylib = { git = "https://github.com/user/mylib.git", tag = "v1.0.0" }
applib = { git = "https://github.com/user/applib.git", branch = "develop" }
```

```toml
# Long-form dep spec: features and backend knobs
[dependencies]
imgui = { version = "0.0.3", features = ["docking"] }   # Request a feature of this dependency
widget = { version = "1.0", backend = "glfw_opengl3" }  # Sugar for: features=["backend-glfw_opengl3"]
```

`backend = "<impl>"` is **general-purpose convention sugar**: it desugars 1:1 into
requesting the dependency's `backend-<impl>` feature (a library that supports this
knob should declare a `backend-*` family in its own `[features]`). If the target
package declares `[features]` but does not include the requested feature (including
the result of backend desugaring), a warning is issued by default, and an error
under `mcpp build --strict`.

**Git dependencies and `mcpp.lock`**: a `tag` or `rev` already names a fixed point
in history, but a `branch` moves. The first build resolves the branch to a commit
and records it in `mcpp.lock`, and every later build rebuilds **that** commit — the
lock is authoritative, not a cache hint, so deleting `~/.mcpp/git` or moving to
another machine cannot silently move the build to a newer tip. A newer tip must be requested
explicitly:

```bash
mcpp update mylib     # drop the recorded commit; the next build re-resolves it
mcpp update           # same, for every dependency
```

Because the recorded commit is enough to decide what to build, a rebuild with the
clone already in `~/.mcpp/git` makes no network request at all and works under
`--offline`. Only two things need the network: resolving a branch that has no
commit in the lock, and cloning a commit that is not cached yet. A `git =` value
that names a local directory (or a `file://` URL) needs neither, so it is never
refused offline.

**SemVer constraints**:

```toml
[dependencies]
foo = "^1.2.3"      # >= 1.2.3, < 2.0.0 (caret, default)
bar = "~1.2.3"      # >= 1.2.3, < 1.3.0 (tilde)
baz = "=1.2.3"      # Exact match
qux = ">=1.0, <2.0" # Range combination
```

### `visibility` — whether a dependency's usage crosses this package's own boundary

```toml
[dependencies]
sdk = { version = "1.0", visibility = "private" }
```

Every dependency edge carries a `visibility`, defaulting to `public`. It
decides whether the dependency's include directories, defines and flags —
what the dependency asks of a consumer, `provides`/`requires` aside — reach
only this package's own translation units, or reach this package's
*consumers* as well.

| Value | This package's own units | This package's consumers |
|---|---|---|
| `public` (default) | yes | yes |
| `private` | yes | **no** |
| `interface` | no | yes |

`private` is the ordinary case for an implementation detail: a vendored
library, a platform SDK a package needs to implement a facility but does not
expose in its own interface. `interface` is the rarer, opposite case — a
header-only dependency this package's own headers `#include` but whose
objects it never compiles against. `public` is what most dependencies want:
a type from the dependency appears in this package's own public headers, so
a consumer needs the same include path to use them.

Getting this wrong in one direction is silent (an unused `public` broadcasts
headers nobody asked for) and in the other is a build failure at the first
consumer that needed what `private` withheld — which is the safer failure
mode, and why `[feature-deps.<feature>]` (§06, "A platform SDK dependency
stays private") states `private` explicitly rather than relying on a default
that happens to work until a consumer is added.

### When two declarations of one dependency disagree

Two edges in the dependency graph can name the same identity — the same
`(namespace, name)` pair — from two different places: the root manifest and a
dependency's own `[dependencies]`, or two unrelated dependencies. A declaration
has a **kind** (`version`, `git`, or `path`) and a **reference** within that
kind (a SemVer constraint, a `git` URL plus `rev`/`tag`/`branch`, or a
filesystem path). mcpp resolves the identity once; every requester's edge is
recorded, and what one requester's declaration decides is what every other
requester of the same identity gets.

| first declaration | second declaration | outcome |
|---|---|---|
| any | same kind, same reference | Unchanged: the second declaration becomes an edge to the identity already resolved. Nothing is reported. |
| `version` | `version`, a different constraint | The two constraints are AND-combined by SemVer, as above; an unsatisfiable pair is refused, naming both constraints and both requesters. |
| `git` | `git`, a different `rev`/`tag`/`branch` | The root's declaration wins when the root is one of the two requesters; otherwise the declaration resolved first wins. A `dependency/source-override` warning names both requesters and both references, states which one is used and why, and how to take the other. This is never silent. |
| `path` | `path`, a different directory | The same rule and the same warning as the row above, compared by canonical absolute directory rather than by git reference. |
| a root `path` or `git` declaration | a dependency's `git` or `version` declaration (a kind clash) | The root's declaration wins, with the same warning. When the losing declaration is a `version` requirement, it is checked against the `[package] version` of the root's resolved checkout; a violated requirement is refused, naming the pin, the requester and the requirement. |
| a dependency's `path`/`git` declaration | another dependency's declaration of a different kind (a kind clash, and neither party is the root) | Refused: "requested as both a … dep … and a … dep …. Pick one." The message adds one sentence: declare the identity in the root to settle it. |

The root's privilege here is bounded the same way `linkage` is bounded to the
root manifest's own edges (see `[dependencies]` above): a whole-graph choice of
*which checkout* an identity resolves to is a decision only the artifact's own
manifest may make silently on a dependency's behalf. A dependency that
disagrees with another dependency, with neither being the root, is never
settled by guessing which one was declared first — that is exactly the
"accident of queue order" this section replaces.

### The identity of a `path` or `git` dependency (mcpp 2026.9.14.2+)

A `path` or `git` dependency is the package its manifest declares, whatever key
reaches it. A key that normalises to another identity than the manifest's
`[package] namespace` and `name` takes the declared identity, and mcpp warns
once for each declaring edge, naming the requester, the key, the identity the
key names and the identity the manifest declares:

```toml
# comp/mcpp.toml; fw/mcpp.toml declares namespace = "huxdemo"
[dependencies]
fw = { path = "../fw" }            # names mcpplibs.fw; huxdemo.fw is used
```

```
warning: 'huxdemo.comp@path' declares the dependency 'fw', which names mcpplibs.fw; the manifest '.../fw/mcpp.toml' declares huxdemo.fw, and that identity is used.
  hint: write 'huxdemo.fw' in 'huxdemo.comp@path' to state the identity the manifest declares.
```

Two edges written `fw` and `huxdemo.fw` over one directory are therefore one
package, compiled once, and `mcpp why deps` lists both keys under it. A
manifest that declares no namespace takes the key's, so two keys with
different namespaces over one such directory are two identities over one
source: the build is refused before scanning, naming both, and the fix is a
`namespace` in that manifest or one key in both places. A `version` dependency
is unaffected; its identity is the key.

### A package of a git repository (mcpp 2026.9.16.1+)

A `git` dependency names a repository, and its key names which package of the
repository is meant. The root manifest's package is one; each entry of that
manifest's `[workspace] members` is another. A key whose identity is not the
root package's selects the member whose manifest declares it, at the same
commit:

```toml
# repo/mcpp.toml declares spike.fw and [workspace] members = ["tool"];
# repo/tool/mcpp.toml declares spike.fw-installer
[dependencies]
spike.fw           = { git = "https://example.org/fw.git", rev = "cc3c74c5" }
spike.fw-installer = { git = "https://example.org/fw.git", rev = "cc3c74c5", tools = ["fw-installer"] }
```

- The member inherits the repository's `[workspace.package]`, as it does when
  the repository is built from its own checkout.
- A member's `path` edge that stays inside the clone, such as
  `spike.fw = { path = ".." }`, names the same git source at the same commit,
  so it is the package the root's key resolved rather than a second,
  path-sourced declaration of it.
- A key that names neither the root package nor a member keeps the rule of the
  section above: the root manifest's identity is used, with the warning.

A key selects by identity, and no `subdir` key exists: an older client would
ignore such a key and build the root package without a word.

### Namespace resolution rules

Every package has a two-part identity: a **namespace** and a **name**. Every
selector normalizes to exactly one identity:

- `cmdline` → `(mcpplibs, cmdline)`; omitting the namespace means the
  `mcpplibs` default, and nothing else.
- `compat.gtest` → `(compat, gtest)`.
- `mcpplibs.capi.lua` → `(mcpplibs.capi, lua)`.

There is no ordered fallback or fuzzy, index-wide search by short name:

```toml
# Correct — dotted selector
[dependencies]
chriskohlhoff.asio = "1.38.1"

# Correct — namespace sub-table (preferred for several packages from one org)
[dependencies.chriskohlhoff]
asio = "1.38.1"

# Wrong — a bare name never reaches the `chriskohlhoff` namespace
[dependencies]
asio = "1.38.1"
```

The third form fails with an error that names the exact `(mcpplibs, asio)`
identity that was tried and, when the short name exists elsewhere, gives a
copyable explicit selector.

#### Migration window for bare names (`2026.8.10.1` → removed in `2026.9`)

Every published `compat.*` package and every manifest written before exact
identity spells its dependency bare — `gtest = "1.15.2"`. Failing those
outright on upgrade would break builds against data that is already published
and cannot be edited retroactively, so for one release a bare name that misses
`mcpplibs` still reaches `compat.<name>`, and a descriptor that declares no
namespace at all still answers to its bare name.

It is not quiet about it:

```
warning: dependency 'gtest' resolved to 'compat.gtest' through the deprecated
bare-name search; namespace omission means `mcpplibs` only. Write the exact
package:
    [dependencies.compat]
    gtest = "1.15.2"
  (or run `mcpp add compat.gtest@1.15.2`). This fallback is removed in 2026.9.
```

What reaches `mcpp.lock`, the install layer and the cache is the canonical
identity, so the ambiguous spelling lives in exactly one place — the manifest —
until it is rewritten. `mcpp add gtest@1.15.2` performs that rewrite.

The window does **not** apply to a selector that states a namespace
(`mcpplibs.gtest` misses and stays missed), and a bare name still never reaches
a third-party namespace.

**Why one identity?** Dependency resolution has to be reproducible. Candidate
search would let two namespaces with the same short name be settled by index
state, and adding an index could silently retarget an existing dependency.

**For xpkg authors:** in an index descriptor, identity is the pair
`(package.namespace, package.name)`. The namespace is the dotted path; **`name` is
a single atomic segment**:

```lua
package = {
    namespace = "chriskohlhoff",
    name      = "asio",                 -- one segment; NOT "chriskohlhoff.asio"
}

package = {
    namespace = "mcpplibs.capi",        -- depth belongs here
    name      = "lua",
}
```

The filename is only a hint — a descriptor is found by its declared identity, so
`pkgs/c/chriskohlhoff.asio.lua` and `pkgs/z/anything.lua` resolve identically.
`<name>.lua` or `<namespace>.<name>.lua` are recommended (they hit mcpp's fast
path) but not required.

The older fully-qualified spelling (`name = "chriskohlhoff.asio"`) is still
accepted, so already-published descriptors keep working. `mcpp xpkg parse`
enforces the descriptor rule; run it in index CI. Descriptor identity
requires mcpp >= 0.0.106; exact selectors require mcpp >= 2026.8.10.1; both use
xlings >= 0.4.69. Full normative text is in `docs/specs/package-identity.md`.

`mcpp new --template` deliberately reuses this identity model instead of
creating another package grammar: `[ns.]name[@version][:tname]`. A bare name
there also means only `mcpplibs`; version and template may be omitted
independently. Omitted `tname` selects the sole explicit default, or the only
template when no `default = true` is present. Multiple unmarked templates are
an error, never a directory-order choice. See the normative template rows in
`docs/specs/package-identity.md` §4.4.

### When mcpp refreshes the package index

`mcpp build` / `run` / `test` refresh the package index **only when a dependency
cannot be resolved from the local copy** — never merely because time has passed.
Concretely, a refresh happens when there is no local index at all, when a
dependency's descriptor is missing from it, or when a SemVer constraint matches
none of the versions it knows. A build whose dependencies all resolve locally
makes no network request, however old the local index is.

The consequence worth knowing: a constraint like `^1.2` resolves against the
**the versions the local index knows**. A `1.3.0` published upstream after the last refresh
is invisible until it is fetched:

```bash
mcpp index update     # sync the index
mcpp update           # sync, then re-resolve dependencies
mcpp index status     # local state: state, age and revision
```

Controls, in order of precedence:

| Control | Effect |
|---|---|
| `--offline` (any command) | Never touch the network — no index refresh, no downloads, no toolchain auto-install, no `git ls-remote`/`clone`. Anything already installed still builds, including git deps whose commit is in `mcpp.lock` and whose clone is cached |
| `MCPP_OFFLINE=1` | Same, for a whole shell session or CI job |
| `[index] auto_refresh = false` in `~/.mcpp/config.toml` | Never refresh an index implicitly: not on a dependency miss, not before installing a package the local index lacks, and not for the first sync of a project's custom index (that build stops and names `mcpp index update`). Downloads still work |

`MCPP_NO_AUTO_INSTALL=1` remains accepted as the older, narrower spelling of
`--offline` (it gates only toolchain auto-install).

A refresh is bounded. `[index] refresh_timeout` (seconds, default 120) is the
longest one refresh may take; a refresh that exceeds it is stopped, a warning
names the setting, and the build continues with the local index, as it does
after any failed refresh. An install through xlings is stopped when xlings
writes nothing, not even its heartbeat, for 300 seconds. Terminating mcpp
terminates the xlings process it started.

Run any command with `-v` to see the decision for each dependency and why.

## `[dev-dependencies]` — Test Dependencies

```toml
[dev-dependencies.compat]
gtest = "1.15.2"
```

`mcpp build` ignores these; `mcpp test` resolves and uses them. `mcpp test` automatically discovers `tests/**/*.cpp` and compiles them into test binaries. The runner is framework-agnostic: each file is an independent binary judged by exit code — a bare `main`, gtest (via `[dev-dependencies]` + `gtest_main`), or any other framework all work identically, and `-- args` are forwarded to every test binary (e.g. `-- --gtest_filter=...`). Note: synthesized test target names may contain `/` (`tests/00-a/0.cpp` → `00-a/0`), unlike `[targets.*]` names — the two namespaces are intentionally separate (test targets never enter the manifest or publishing). Tests are named by their `tests/`-relative path (`tests/00-a/0.cpp` → `00-a/0`), each test compiles in isolation (a broken test fails alone; package/dep breakage is reported as a build error instead), and `mcpp test <pattern>` / `--message-format json` filter and machine-format the run.

## `[build-dependencies]` — Build-Time Dependencies (mcpp 2026.8.29.1+)

```toml
[build-dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

The section and the per-edge request answer **different questions**, and
conflating them is a modelling error rather than a matter of spelling.

- The **section** says whether the package itself reaches the target.
  `[dependencies]` means it does; `[build-dependencies]` means it never does,
  and neither does anything reachable only through it.
- The **request on the edge** says which build-time product is wanted.
  `tools = [...]` asks for a host executable; `host-module = true` asks for a
  module the build program can import.

A package takes a value on both axes at once, and protobuf is the case that
proves the axes must stay separate: a project links `libprotobuf` *and* needs
`protoc` during the build. It is written once, in `[dependencies]`:

```toml
[dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

`[build-dependencies]` is for the combination the first axis cannot otherwise
express — a package whose library must not reach the target while its tool or
its rule is still wanted. Naming one package in both tables is not an error:
the ordinary declaration wins, because a `[build-dependencies]` line must not
quietly drop a library the target needs, and what each declaration requests
(`tools`, `features`, `host-module`, `reexport`) is requested of the one edge
(mcpp 2026.9.16.1+; before that release the second declaration's requests were
dropped).

**A package of programs contributes only its programs (mcpp 2026.9.16.1+).** A
dependency whose declared `[targets]` are all programs (`bin`, `app`, `test`)
has nothing to link. Its tools are built by the tool sub-build, which resolves
the package as its own root, and in the consumer's graph it provides its tools
and its directory and nothing else: its own dependencies are not resolved
there, its sources are not compiled there, and its `ldflags` do not reach the
consumer's link. A program may therefore depend on the package that requests
it, which is how an SDK provides a program built against itself. A package
that declares no `[targets]` table is unaffected, even when a `src/main.cpp`
infers a program for it.

A cycle among packages is refused where the graph is resolved, naming its
edges, under every cache mode; a tool whose own sub-build requests it again is
refused at that first repetition, naming the chain.

Unlike `[dev-dependencies]`, these **are** walked transitively: a build
dependency's own dependencies are what make it work, and they inherit its
build-only nature.

A feature scopes build-time requests without a second declaration site. A
`[feature-deps.<name>]` entry may restate a dependency already declared
unconditionally, with the same source, and add `tools` to it, so "only when
needed" needs no separate table:

```toml
[dependencies]
spike.installer = { path = "../installer" }

[feature-deps.installer]
spike.installer = { path = "../installer", tools = ["installer"] }
```

The restatement names its source because every dependency table does: an entry
without `path`, `git`, `version` or `workspace` is read as a namespace table
and refused, and the refusal says to restate the source. `tools`, `features`,
`host-module` and `reexport` of the restatement are added to the declaration in
effect on the row. A restatement that names another source is refused, naming
both sources (mcpp 2026.9.16.1+); before that release it was ignored.

> The section has been parsed since early versions and, until 2026.8.29.1, read
> by nothing that made a decision: writing it produced a manifest that loaded,
> no diagnostic, and no effect.

## Current limitations

- **Two things need the network, and only two:** resolving a branch that has no
  commit in the lock, and cloning a commit that is not cached yet. A `git =`
  value naming a local directory or a `file://` URL needs neither and is never
  refused offline.
- The index-refresh window **does not apply to a selector that states a
  namespace**. `mcpplibs.gtest` misses and stays missed until the next refresh.
- `mcpp.lock` records and verifies a resolution; it does not constrain one. See
  [51 — Supported Versions and Compatibility](51-supported-versions.md).
