# 21 — Commands by Scenario

The command reference is `mcpp --help`, and each subcommand carries its own
`--help`. This chapter answers a different question: which command applies to a
situation that has already arisen — a build directory that keeps growing, a
resolution nobody expected, a descriptor about to be published, an index that
may be stale. The commands collected here are the ones whose name does not
announce the situation they belong to.

Related documents: [00 — Getting Started](00-getting-started.md) for the
everyday build/test loop, [03 — Toolchain Management](03-toolchains.md),
[10 — Publishing a Library](10-publishing-a-library.md),
[11 — Machine-Readable Output](11-machine-output.md).

Every output below was produced by the version of mcpp this chapter ships with.

## Reclaiming disk without forcing a rebuild

Two stores grow, they grow for different reasons, and one command each empties
them. Confusing them costs a full rebuild.

| Store | Scope | Grows when | Emptied by |
|---|---|---|---|
| `target/<triple>/<fingerprint>/` | one project | a configuration fingerprint changes and opens a new directory | `mcpp clean`, `mcpp clean --stale` |
| the build cache (`mcpp cache dir`) | the whole machine | any project compiles a dependency or a `std` module | `mcpp cache gc`, `mcpp cache prune`, `mcpp cache clean` |

`mcpp clean` removes `target/` entirely, and the next build recompiles
everything. `mcpp clean --stale` removes only the fingerprint directories that
no recorded build still uses, so the configurations in use survive:

```
$ mcpp clean --stale --dry-run
would remove target/x86_64-linux-gnu/0123456789abcdef  (0.0 B)
Would remove 1 directory (0.0 B)
```

"In use" means recorded in `target/.build_cache`, which `mcpp build` writes and
the fast paths read. Three consequences follow from that definition:

- A directory no record names is not deleted merely for that. `mcpp test`
  builds through a path that writes no record, and so does a `--no-cache`
  build. An unrecorded directory written within `--older-than` (one day by
  default) is kept; older than that it goes, and the cost of being wrong is one
  rebuild of a configuration nothing has touched since.
- With no record at all the command refuses rather than guess. One
  `mcpp build` establishes what is current.
- Directories under `target/` that are not fingerprint directories — `dist/`
  from `mcpp pack`, among others — are never visited.

`--dry-run` lists and deletes nothing. `--stale`, `--dry-run` and
`--older-than` each select this mode: `mcpp clean --older-than 3d` is a scoped
request and is not read as a full wipe. `--older-than 0` keeps no unrecorded
directory; a negative duration is refused.

The build cache is machine-wide, so a project-local command must not empty it —
`--stale` and `--bmi-cache` are refused together. `mcpp cache list` shows what
occupies it. The rows carry no order, and a `0.0 B  (incomplete)` row is an
entry an interrupted build left behind:

```
$ mcpp cache list
key               kind          size       last used  package
8a150ad49d666f94  std       29.6 MiB          6d ago  std gcc@16.1.0 c++23 libstdc++
9234eed9ef786c13  std          0.0 B          2d ago  std  (incomplete)
```

`mcpp cache gc` requires `--max-size`, `--older-than`, or both, and evicts
package entries only. A `std` BMI is shared by every project on the machine, and
the implementation excludes it from size-driven eviction on the grounds that
rebuilding one trades a lot of time for a little disk. `mcpp cache clean --std`
remains the explicit way to remove it.

## The versions a package publishes

`mcpp search` matches a substring and appends what each hit publishes, merged
across the descriptor's per-OS tables and sorted semver-descending:

```
$ mcpp search imgui
  compat:imgui          Dear ImGui immediate-mode GUI library core sources  (1.92.8, 1.92.8-docking)
  mcpplibs:imgui        C++23 module package for Dear ImGui core and GLFW/OpenGL3 backends  (0.0.6, 0.0.5, 0.0.4, ...)
```

The trailing `, ...` marks truncation: three versions are shown by default, and
its absence means the list is complete. `--all-versions` prints the whole list.
A package whose descriptor cannot be read prints as two columns — the version
list is best-effort display and never fails the search.

`mcpp add` carries the same information when a name does not resolve. The
suggestion names the namespace to write and the versions behind it:

```
  a package with this name exists under another namespace:
    compat.eui-neo (0.5.6, 0.5.5, 0.5.3)
```

This scan runs only after a lookup has already failed, and its result reaches
error text and search output only. A bare name never resolves across namespaces
on the strength of it.

## Explaining a resolution

`mcpp why` reports what a build would resolve, and builds nothing:

```
$ mcpp why toolchain
toolchain: gcc 16.1.0 (x86_64-linux-gnu)
  abi(libc)=glibc  cxxstdlib=libstdc++  arch=x86_64  os=linux  triple=x86_64-linux-gnu
  reason: [toolchain] in mcpp.toml if set, else platform-native default
```

The topic is `toolchain`, `runtime`, `deps` or `runners`, and all four report
when none is named. `--target` and `--toolchain` turn the report into a query
about a pair the current directory does not use, which is how a target matrix
asks one cell at a time.

An error code in a diagnostic expands through `mcpp self explain`:

```
$ mcpp self explain E0006
E0006: index requires a newer mcpp

The package index declares (index.toml [index].min_mcpp) that its
descriptors need a newer mcpp than this binary — parsing them would
silently misbehave, so resolution stops instead. Upgrade mcpp:
```

## Index freshness and offline builds

`mcpp index status` answers whether the local index copies are current without
touching the network:

```
$ mcpp index status
  index      state    refreshed    revision     path
  xim        fresh    28s ago      1f4b39d      /home/speak/.mcpp/registry/data/xim-pkgindex
  mcpplibs   fresh    28s ago      d4b36d7      /home/speak/.mcpp/registry/data/mcpplibs
```

`mcpp index update` refreshes them. A package published minutes ago and still
absent after a refresh is a propagation question, not a naming one — indices
reach clients as artifacts rather than git clones.

`--offline` (or `MCPP_OFFLINE=1`) forbids the network for one invocation and
fails rather than fetch. `--locked` fails when resolution differs from
`mcpp.lock` instead of rewriting it, which is the shape a CI job wants.
`mcpp index pin <name> <rev>` records a commit for a custom index in
`mcpp.toml`; `mcpp index unpin` removes it.

## Validating a descriptor before publishing

`mcpp xpkg parse` reads a descriptor with the resolver's own grammar, so what
it reports is what resolution will see:

```
$ mcpp xpkg parse mcpp.plugins.lua
package    mcpp.plugins (namespace 'mcpp')
versions   linux    0.1.1, 0.1.0, latest
versions   macosx   0.1.1, 0.1.0, latest
versions   windows  0.1.1, 0.1.0, latest
form       A — no mcpp segment (build info from the source's mcpp.toml)
parse OK
```

The per-OS lists are printed separately on purpose: a version added to one
platform table and forgotten in the others reads as "not found" on the
platforms that lack it, against a file that contains the version string.
`--json` emits the same facts for a script:

```
$ mcpp xpkg parse mcpp.plugins.lua --json
{"namespace":"mcpp","name":"plugins","versions":{"linux":["0.1.1","0.1.0","latest"],"macosx":["0.1.1","0.1.0","latest"],"windows":["0.1.1","0.1.0","latest"]},"form":"A"}
```

`mcpp emit xpkg` generates the entry to submit. See
[10 — Publishing a Library](10-publishing-a-library.md) for the full path.

## Environment diagnosis

`mcpp self doctor` checks the toolchain, the `std` module, the registry, cache
health and the last runtime-closure verdict, and reports what it found rather
than only what failed:

```
$ mcpp self doctor
    Checking toolchain
          ok gcc 13.3.0 (x86_64-linux-gnu) at /usr/bin/g++
    Checking cache health
          ok build cache size = 2.5 GiB
warning: pre-v1 cache at '/home/speak/.mcpp/bmi' occupies 167.5 MiB and is no longer used — `mcpp cache clean --legacy` reclaims it
```

`mcpp self env` prints the paths and the resolved toolchain, `--format json`
included. `mcpp self config --mirror CN|GLOBAL` selects the download mirror;
mcpp and xlings hold this setting separately, so selecting it for one does not
select it for the other.

## `[hooks]` — Project Build Lifecycle Commands (experimental)

> **Experimental.** A hook cannot currently decide whether a build succeeded.
> Every hook failure is reported as a **warning** and `mcpp build` keeps the
> result it earned on its own; `side_effect = true` is refused with an error
> rather than honoured. The key stays in the schema so that manifests written
> today do not have to change when the feature is promoted. Two further limits
> are permanent rather than provisional: only the root project's hooks run, and
> only `mcpp build` runs them.

A hook is a command `mcpp build` **owns for an interval**, and the event names
the interval:

```toml
[hooks]
build_start = "echo build started"
build_failed = "notify-send 'build failed'"
build_finished = "notify-send 'build finished'"

# Optional; these are the defaults.
timeout_seconds = 10
enabled = true
side_effect = false           # `true` is refused while this is experimental
```

| Key | Type | Default | The interval it names |
|---|---|---:|---|
| `build_start` | command | — | Opens after project preparation, closes when the command exits |
| `build_finished` | command | — | Opens after a build that succeeded, closes when the command exits |
| `build_failed` | command | — | Opens after a build that failed, closes when the command exits |
| `during_build` | command | — | Opens before the build, closes after it |
| `timeout_seconds` | integer, 1–86400 | `10` | Bounds one run of a command |
| `enabled` | bool | `true` | Enables all commands in this table |
| `side_effect` | bool | `false` | Whether a hook failure makes the build fail. **Reserved** — only `false` is accepted while this is experimental |

The first three intervals are **self-closing** — they end when the command
does. "Synchronous" is not a separate mode here; it is what a self-closing
interval looks like. `during_build` is the one interval closed by something
else, and the two keys that only make sense for one shape follow from that
rather than being exceptions.

A command is a string, or a table when it needs options:

| Table key | Applies to | Meaning |
|---|---|---|
| `cmd` | every event | The command. Required. |
| `timeout_seconds` | self-closing events | Overrides the table default for this event |
| `loop` | `during_build` | Restart the command if it exits before the build ends |

`loop` on a self-closing event and `timeout_seconds` on `during_build` are both
**errors**, not ignored keys: a self-closing interval ends when its command
exits, so there is nothing to restart, and `during_build` is already bounded by
the build. A key that is accepted and does nothing reads as a broken feature.

Commands run through the host shell (`/bin/sh` or `cmd.exe`), with the
**project root** as their working directory — not the directory `mcpp build`
was typed in, so a relative path in a hook means the same thing wherever the
build was started. A self-closing command keeps ordinary terminal
input/output. Missing event commands are skipped.

The lifecycle is:

```text
during_build opens
build_start
    ├─ build succeeds → during_build closes → build_finished
    └─ build fails    → during_build closes → build_failed
```

`during_build` closes **before** the terminal hook, so the two commands never
overlap.

`build_failed` and `build_finished` are mutually exclusive, and both are
reachable only after `build_start` has run. A project that cannot be *prepared*
— an invalid manifest, an unresolvable dependency, no usable toolchain — fires
nothing: it has not started building, and its hook program may be exactly what
preparation would have installed.

A hook command that cannot start, returns non-zero, or exceeds its timeout is a
hook failure. For `during_build` there is one more: a looped command that
**fails to stay up** — five consecutive runs ending unsuccessfully within a
second — stops being restarted and is reported. (A command that finishes
quickly and *successfully* is doing exactly what `loop` was asked to repeat,
and is not a failure.) Every one of those is reported as a **warning**, and the
build keeps the result it earned on its own — while `[hooks]` is experimental
it does not get a vote. A hook's own failure does not trigger another hook.

`side_effect = true` is what will change that, and asking for it today is an
error:

```text
error: mcpp.toml: error: [hooks].side_effect = true is not available yet:
[hooks] is experimental and cannot decide whether a build succeeded. …
```

Refused rather than quietly downgraded, because both silent options are worse:
honouring it would give an experimental feature a veto over every build, and
ignoring it would leave a project believing its build is gated on a notifier
when nothing is. When the feature is promoted, `true` will mean "a hook failure
fails the build" — and a build that failed on its own will still keep its own
exit code, so `mcpp build` never reports a compile error as a notifier problem.

Two things are worth knowing about a `during_build` command specifically:

- **Its output is discarded**, because it writes concurrently with the build
  and would otherwise land in the middle of a compiler diagnostic. Run
  `mcpp build --verbose` to see it.
- **It is stopped as a process tree**, not as a process. `player & wait` makes
  the player a grandchild of the command mcpp started, and stopping only the
  latter would leave the audio device held after the build. mcpp puts the
  command in its own process group (a job object on Windows) and stops that,
  including when the build is interrupted with Ctrl-C.

Scope, precisely:

- Only `mcpp build` runs hooks. `mcpp run`, `mcpp test` and
  `mcpp build --configure-only` build too, and deliberately do not.
- Hooks belong to the **package being built**. In a workspace fan-out that is
  each member in turn — its own `[hooks]`, around its own build, in its own
  root. A *virtual* workspace root (`[workspace]` with no `[package]`) builds
  nothing, so a `[hooks]` table there never fires.
- A dependency's `[hooks]` is **skipped**, always. Only the root project's run.
  Every manifest mcpp parses carries the section, a dependency's included, and
  nothing reads it — which is what keeps `mcpp add` from meaning "run this
  author's shell command on my next build". This is a property of the design,
  not a default awaiting a switch.
- Declaring an active hook opts the project out of the no-op fast path, because
  `build_start` is specified to run after preparation. Expect `mcpp build` on an
  already-current hooked project to cost a preparation pass rather than
  milliseconds.

An unrecognised key in `[hooks]`, or inside one event's table, is a warning (an
error under `--strict`), so a manifest written for a newer mcpp still loads. An
unrecognised *value* — a missing or non-string `cmd`, a `timeout_seconds`
outside 1–86400, a key offered to the wrong interval — is a manifest error.

> **A hook is code, and `mcpp.toml` is part of the repository.** Building a
> freshly cloned project runs whatever its `[hooks]` say, with the privileges
> of whoever invoked `mcpp build`. This is the same trust `build.mcpp` already
> asks for ([07 — build.mcpp](07-build-mcpp.md)); `[hooks]` widens its reach
> rather than introducing it.

Hook programs can be installed as ordinary xlings dependencies. For example,
an audio notifier can keep its sound files inside its own executable rather
than adding media handling to mcpp:

```toml
[hooks]
build_finished = "mcpp-hooks-audioplayer niulai-mm"
build_failed = "mcpp-hooks-audioplayer niulai-niulai"
side_effect = false

[xlings.workspace]
"xim:mcpp-hooks-audioplayer" = "0.0.1"
```

A different sound for a successful or failed build. `side_effect = false` is
written out rather than left to the default: it is the value this manifest
wants on its own terms — a missing audio device should never fail a build — so
it will still say so once the key has more than one accepted value.

## Current limitations

- `mcpp why --format json` is defined for the `toolchain` topic only. The other
  topics report `'<topic>' has no machine-readable shape yet` and exit non-zero.
- `mcpp search` matches a substring; there is no field selector, and no way to
  restrict a search to one namespace.
- `mcpp clean --stale` reads `target/.build_cache`, which holds a bounded number
  of recent entries. A project built across more (target, profile) pairs than it
  holds loses its oldest entries, and a directory whose entry has been evicted
  is then treated as unrecorded — kept while it is newer than `--older-than`,
  removed after that.
- `mcpp cache gc --older-than 0` is rejected with `bad --older-than value '0'
  (expected <N>{s,m,h,d})`, while `mcpp clean --stale --older-than 0` accepts it.
  The two options share a parser but not this case.

`mcpp emit xpkg` produces a descriptor that `mcpp xpkg parse` rejects for a
package that keeps its own `mcpp.toml`. The emitted `mcpp` segment carries
`manifest = "mcpp.toml"` and no `sources` list, and the validator requires one:

```
error: d.lua: xpkg-lua://example.mathkit@0.1.0: error: synthesised manifest
       missing sources (mcpp segment must declare `sources = { ... }`)
```

Adding `sources = { "src/*.cppm" }` to the emitted `mcpp` segment makes it
validate. Measured on 2026.9.8.1.
