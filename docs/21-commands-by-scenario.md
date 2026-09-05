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
