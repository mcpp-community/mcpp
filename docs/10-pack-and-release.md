# 10 — Packaging an Application for Release

**Reader:** someone shipping a program to a machine that does not have mcpp.

**The question this chapter answers:** how do I turn a build into something
another machine can run, and what does each packaging mode carry.

**Not here:** shipping a **library** for other packages to build against, which
is [12 — Distributing a Prebuilt Library](12-binary-distribution.md). After:
[11 — Publishing a Library to mcpp-index](11-publishing-a-library.md).

> This page is about bundling a **program**. To ship a *library* as interface +
> prebuilt binaries, see [12 - Distributing a Prebuilt Library](12-binary-distribution.md).
>
> Which one `mcpp pack` does is decided by the target's `kind`, not by a flag:
> `mcpp pack <name>` packs `[targets.<name>]`, and a `bin` becomes a bundle
> while a `lib`/`shared` becomes a library package. With no name, mcpp picks
> the only packable target.

> A default dynamically linked binary produced by `mcpp build` has a loader and
> RUNPATH tied to the build sandbox. It is a development artifact, not a
> deliverable. Three routes turn it into one — and none of them uses the host's
> C library.

## Three distribution routes

Every route below produces an artifact whose C runtime comes from the
ecosystem, never from `/lib64`. That is deliberate: mcpp builds against a
private glibc precisely so a binary's behaviour does not depend on which
distribution happens to be underneath it, and reaching back out to the host's
libc to distribute would give that away at the last step.

| | Route | Command | Source of its C runtime | Applicability |
|---|---|---|---|---|
| **A** | Through the ecosystem | `mcpp emit xpkg` → `xlings install <pkg>` | the target machine's own xlings payloads | the target has xlings |
| **B** | One static file | `mcpp build --target x86_64-linux-musl` | nowhere — it is linked in | a single file with no runtime dependency |
| **C** | Carry the runtime | `mcpp pack --mode self-contained` | shipped inside the bundle | any Linux, including older than the build machine |

**On route A.** The `PT_INTERP` recorded in a freshly built binary points at the
build machine's payload, so copying that file to another machine by hand does not
work: the path does not exist there. This is a property of the copy rather than of
the artifact — installed through `xlings`, the package's ELF files are repointed at
the target machine's own payloads at install time. The recorded path is a
build-machine detail, not a distribution format. Routes B and C are the ones that
survive hand-copying.

**On route B.** `--target …-musl` implies a static link, so there is no loader,
no RUNPATH and nothing to find at run time. It is the smallest and most
portable result, and the one to reach for first when the program does not need
glibc-specific behaviour (NSS lookups, `dlopen` of host plugins).

**On route C.** The bundle carries this toolchain's glibc and its loader, so it
runs on distributions older than the build machine — the case B cannot cover
when glibc is actually required. Read the `/proc/self/exe` note below before
choosing it: launching through a bundled loader changes what the program sees
about itself.

## Three axes and legs

A pack is decided by three independent choices and a set of legs (2026.10.27.1+):

| Axis | Option | Question | Values |
|---|---|---|---|
| linkage | `--linkage` | are the libraries inside the program or beside it | `static`, `shared` |
| closure | `--mode` | what the machine that runs the package provides | `system`, `vendored` (default), `self-contained` |
| container | `--pkg-format` | what is delivered | `tar`, `zip`, `dir`, `single-file`, a format a package provides, or a list of containers of one tree (`tar,okb`); unstated: `zip` for a Windows program, `tar` elsewhere |
| legs | `--target`, repeatable | which targets the pack covers | targets and target sets (`x86_64-openkal`, `openkal`, `linux-musl`) |
| systems | `--systems` | which systems a self-contained package's runners, or single files, are for | `linux`, `linux-aarch64`, `all` |

- **Linkage** is a property of the program. `--linkage static|shared` (or
  `[pack] linkage`) is the build's dependency linkage for the pack, over
  `[build] dependency_linkage` and the profile's; unstated, each library links as
  it declares itself. On Linux `--linkage static` selects the `<arch>-linux-musl`
  row (a `-linux-gnu` target becomes its musl row) and packs the fully static
  program whole, whatever `--mode` says; `--target …-linux-musl` implies it. On
  an ok image it is one `.oke` with no `.okso`.
- **Closure** is a property of the package: how much of the shared-library
  closure travels with the program. `--mode static` is shorthand for
  `--linkage static --mode self-contained`.
- **Container** is the shape of the delivery, described below.
- **Legs** are the targets the command names (2026.10.30.1+): every `--target`,
  else `[pack] targets`, else the build's target. A target set (`openkal`,
  `linux-musl`) names the target table's rows of its system that the selected
  platform serves. A platform never adds a leg. The legs are merged by what the
  target is:

  | Target | Several legs |
  |---|---|
  | `lib`, `shared` | one library package ([12](12-binary-distribution.md)) |
  | a program of a platform that states a runner (openkal) | one tree named after the platform, `<name>-<version>-openkal` |
  | an Android application | one APK tree, `lib/<abi>/` per leg |
  | any other program | one package per leg, each named by its triple |

  A pack that writes more than one file takes a directory for `-o`: a path
  ending in `/`, or an existing directory, below which each file keeps its
  default name. One openkal leg is said after `Packed`: `only x86_64;
  --target openkal or [pack] targets = ["openkal"] packs every architecture`.

| Mode | Host must provide | Size | Use case |
|---|---|---|---|
| `system` | every `.so` (incl. third-party) | smallest | `.deb`/`.rpm`, same-distro fleet (pkg manager declares deps) |
| `vendored` (default) | libc / libstdc++ / loader | +a few MB | Mainstream distros (Ubuntu 22+, Debian 12+, RHEL 9+) |
| `self-contained` | nothing | +30–50 MB | Any Linux incl. older glibc; bundles closure + `run.sh` wrapper |
| `static` (`--linkage static --mode self-contained`) | nothing (single file) | +5–10 MB | musl; matching Linux x86_64 or aarch64 host, Docker scratch, Alpine |

How to choose:

- Distro packages (`.deb`/`.rpm`) or same-distro internal deploy → `system`
- Desktop / server releases for mainstream distros → `vendored` (default)
- Cross-distro / older glibc (legacy CentOS, Kylin) → `self-contained`
- Single portable file, no host deps → `static`

**No mode ships a build-machine path.** A development build addresses this
machine on purpose: its `DT_RPATH` names the toolchain's payload directories and
the SubOS library view (`<subos>/lib`), and its `PT_INTERP` names a private
loader. Every mode rewrites both — `vendored`/`self-contained` to `$ORIGIN`
relative paths, `system` by clearing the search path entirely and restoring the
platform's standard interpreter. `system` is not "keep whatever the build had";
it is "the target provides everything", which is a statement about the target
and cannot be spelled with this machine's absolute paths. e2e 215 sweeps every
ELF in the bundle for anything under `$MCPP_HOME` and fails on a hit.

A combination that cannot hold is refused before anything compiles, naming both
choices:

| Combination | Remedy |
|---|---|
| `--pkg-format single-file` with `--mode system` | `--mode vendored` or `self-contained`: a single file carries what it runs |
| `--compress` with `--pkg-format dir` or a provided format | `--pkg-format tar`, `zip` or `single-file`: only a container is compressed |
| `--compress zstd` or `xz` with `--pkg-format zip` | `--compress gzip` (DEFLATE) or `none`, or `--pkg-format tar` |
| `--compress gzip` or `xz` with `--pkg-format single-file` | `--compress zstd[:<level>]` or `none`: the launcher decompresses zstd |
| `--systems` on anything but a self-contained tree of images or a single file of an image | `--mode self-contained`, or `--pkg-format single-file`; `[pack] systems` that does not fit is noted and left aside |
| an image's single file for a system named whose row this mcpp builds no program for (`x86_64-macos`, `aarch64-windows-gnu`) | `--systems` naming the pairs that are built; `--systems all` takes those and says the rest |
| more than one file (a package per leg, several single files) with `-o` naming one file | `-o <directory>/` |
| several images of a platform that states no `runner`, not as single files | one pack per target, or `--pkg-format single-file` |
| `--target` outside the platform's targets | a target the platform serves |
| a target set that expands to nothing, or whose rows select different platforms | a target, or another set |
| `dir` or `single-file` in a `--pkg-format` list; a list with `-o` naming one file; a list on `mcpp run` | one format, or `-o <directory>/` |
| `--mode self-contained` of an image with `okb` as the only format | `--pkg-format tar,okb`: okb holds the images alone, and the runner would be dropped |
| a tree that runs nowhere (`runs_on` empty) | `--mode self-contained --systems <a system this mcpp builds for>` |
| a dispatched format for a `lib` or `shared` target | the library route, [12](12-binary-distribution.md) |
| `--pkg-format single-file` for a `lib` or `shared` target | `tar`, `zip` or `dir` |

A `[pack] compress` that does not fit the container (a directory, `zstd` for a
zip) is left aside for that pack; only the flag is refused.

### A program that needs the HOST to provide something

"Self-contained" has a floor. Some libraries can only come from the target
machine: a graphics driver's user-space half is version-locked to the running
kernel module, and for the proprietary stacks redistribution is not permitted.
Declare those as run-phase capability requirements (§2.11 of
`docs/04-mcpp-toml.md`), and the mode table gains a column:

| Mode | Program needing a host-provided capability |
|---|---|
| `system` | yes |
| `vendored` (default) | **the right default for these** |
| `self-contained` | **refused at pack time** |
| `static` | **refused at pack time** |

The two refusals are the same fact: **a bundle that carries its own libc cannot
consume a library the host supplies.** That `.so` arrives with its own
requirements on the *target's* libc, and the process does not have that libc —
measured in both directions as mcpp#392 / mcpp#401, where a private glibc
meeting host-loaded objects dies during relocation, before `main`. Previously
both modes linked and then failed at startup, or silently degraded (for
graphics: software rendering, with nothing saying so).

`vendored` packages such a program and writes a **`HOST-REQUIREMENTS`** file at
the bundle root stating what the target must supply:

```
capability=opengl.glx.driver discovery=rpath-of-dispatch
```

`discovery` is the actionable half — the mechanisms are independent, so
satisfying one does not satisfy another. It is written only when there is
something to say: an empty file would be a *claim* that nothing is needed.

A package built against a stated target base that loads the target's own C
library (`vendored`, `system`) also states the lowest one it runs on
(2026.10.24.1+): the highest `GLIBC_x.y` the
program and the libraries packed with it need, printed as
`Requires glibc >= 2.38 …` and written as
`capability=c-abi.glibc>=2.38 discovery=PT_INTERP`. The base the program was
built against becomes that floor; `self-contained` ships the base's own loader
and C library instead. The `release` and `dist` profiles that `mcpp pack`
builds trim the source paths the artifact carries and define `NDEBUG`
([04 — `[profile]`](04-mcpp-toml.md)).

### Mode name compatibility

Canonical names are shown above. The old names remain **permanent aliases**:
`bundle-project` = `vendored`, `bundle-all` = `self-contained`. Tarball-name
suffixes are a frozen wire format (consumed by `install.sh`) and do **not**
follow the rename: `vendored` → no suffix, `self-contained` → `-bundle-all`,
`static` → `-static`, `system` → `-system`.

## Commands

```bash
mcpp pack                                        # vendored by default
mcpp pack --mode system
mcpp pack --mode static
mcpp pack --mode self-contained                  # alias: --mode bundle-all
mcpp pack --target x86_64-linux-musl             # equivalent to --mode static
mcpp pack --target aarch64-linux-musl            # ARM64 equivalent
mcpp pack --pkg-format dir                       # output as a directory, no tarball
mcpp pack --pkg-format appimage                  # a format a package in the graph provides
mcpp pack -o myapp.tar.gz                        # filename only: lands at target/dist/myapp.tar.gz
mcpp pack -o /abs/path/myapp.tar.gz              # includes a directory: output to the literal path
mcpp pack --profile dev                          # build with a different profile (default: release)
mcpp pack --dev                                  # the same, as `build` and `run` spell it; --profile wins over it
mcpp pack --message-format json                  # one mcpp.pack envelope on stdout (mcpp 2026.9.16.1+)
mcpp pack --no-strip                             # ship the artifacts as built
mcpp pack -p app --pkg-format release            # a workspace member, as if run in its directory
mcpp pack --workspace --pkg-format release       # every member with a program, planned and built once
mcpp pack -p cli -p gui -o dist/                 # two members, each archive below dist/
mcpp pack --debug-symbols dbg/                   # write the separated *.debug files under dbg/
mcpp pack --pkg-format msi --features installer  # activate root-package features for the pack
mcpp pack --linkage static                       # the libraries inside the program
mcpp pack --pkg-format single-file               # one executable file
mcpp pack --pkg-format tar --compress zstd       # .tar.zst
mcpp pack --target x86_64-openkal                # one leg: the image's tree for x86_64
mcpp pack --target openkal                       # every architecture of the platform, one tree
mcpp pack --target openkal --mode self-contained --systems all --pkg-format tar,okb -o dist/
mcpp pack --target linux-musl -o dist/           # a static package per architecture
mcpp pack --merge a.zip --merge b.zip -o all.zip # one package from packages built on several machines
mcpp emit xpkg --package all.zip --url https://example.org/v1.0.0   # its xim-pkgindex entry
```

`--format` is accepted on `pack` and `run` as the earlier spelling of
`--pkg-format` and prints a warning (2026.10.27.1+). On every other command
`--format` is the output encoding (`--format json`).

`--features <LIST>` (mcpp 2026.9.15.2+) activates root-package features for every
build pass the pack performs: each `--target` leg, and both passes of a dispatched
format. It is the value `mcpp build --features` takes, so a host tool needed only for
one distribution is declared under `[feature-deps.<f>]` with `tools = [...]` and built
only by the pack that names `<f>`. `mcpp run --pkg-format <name> --features <LIST>` hands
the same features to the pack it performs.

`-p <member>` (mcpp 2026.9.28.3+) packs a workspace member from the workspace
root: the member is resolved as every other `-p` resolves it, and the pack runs in
its directory, so the result is the one `mcpp pack` in that directory produces. A
relative `-o` keeps meaning the directory the command was typed in.

`--release` and `--dev` (mcpp 2026.9.16.1+) are the shorthands `build` and `run`
take, with the same precedence: `--profile` wins over either, on all three
commands.

A pack states the build it performs as `mcpp build` states one (mcpp
2026.10.1.2+): a line for each package that does work, the status row while it
runs, and `Finished` before the first `Packing` line. A pack over several
configurations builds every configuration first and writes one `Finished`. The
second pass of a dispatched `--pkg-format` is stated in the same way, and its
`Distributing` and `Packed` lines follow it. The build runs `[build] jobs`
compiles at once, as every command that compiles does. Before 2026.10.1.2 the
build of a pack wrote none of this, and a pack that recompiled for minutes
showed only `Planning`.

### Packing several members (mcpp 2026.10.1.1+)

`-p` may be repeated, and `--workspace` and `--exclude` select members as they do
for `mcpp build` ([07 — Workspaces](07-workspace.md), §5.3). The members are
planned as one selection and built once, and each is then staged, and packed, in
a tree of its own:

```bash
mcpp pack --workspace --pkg-format release          # every member with a program
mcpp pack --workspace --exclude updater         # every such member but one
mcpp pack -p cli -p gui --pkg-format release
```

Packing each member with its own `mcpp pack -p <member>` plans the graph, runs
the build programs and starts the build again for every member, including the
members they share. A pack over several members plans once for each
configuration (the grouping `mcpp build` uses), builds once, and runs each build
program once per pass: a package that several members reach, such as a shared
library with a build program, is prepared and compiled once, and its program
does not run again for the pack.

What each member receives is what `mcpp pack -p <member>` gives it:

- **The product.** A member's archive or tree is the one the member makes when
  packed alone, below its own `target/dist/`. With `--output <dir>` it is written
  below the directory, which is created when it is missing, under the name it has
  by default.
- **A staged tree of its own.** With a dispatched `--pkg-format`, the pack prepares
  twice, as above, and the second pass tells each build program the staged tree
  of the member it acts for. `mcpp::pack_stage_dir()` and `${mcpp.stage_dir}`
  answer per member. A program acts for the member it belongs to; a package that
  only one packed member reaches (a provider the member depends on) acts for that
  member; a package that several packed members reach acts for none of them, and
  is told neither the format nor a tree, so one run of its program serves them
  all.
- **Its own distributable.** An `artifact` action the request introduced belongs
  to the member whose program, or whose provider, submitted it. Each member's
  outputs are verified to exist and are reported as `Packed` for that member.

`--workspace` packs a member only if it has a program target, and says which it
skipped. A member named with `-p` that has none is refused. Members are packed
one configuration at a time and reported in `[workspace] members` order, whatever
order `-p` names them in. A member that
fails, for instance one whose provider submitted nothing for the requested
format, is reported by name and the others are packed; the exit status is
non-zero if any member failed. One member, whether named with one `-p` or by the
directory the command runs in, is packed exactly as before: `mcpp pack -p X`
plans X's closure alone. `mcpp pack` with no selector at a virtual workspace
root packs the first member that has a program, as it always did; `--workspace`
packs them all.

**Refused before anything is compiled**, naming what was refused:

| Input | Reason |
|---|---|
| a positional target name | it names a target of one package |
| more than one `--target` | a program is built for one target, and the several-target Android pack stages the legs of one member |
| `--output` that exists as a file | each member's archive or tree is written below the directory |
| a member named with `-p` that has no program target | there is nothing to pack for it |
| with a dispatched `--pkg-format`, a member no provider acts for | the format is provided by a package the member does not reach, or by a package several members reach |
| two members that would write one archive or tree | `--output` names one directory, and two packages of one name, version and target share an archive name |

A package with a build program that provides a format for several members cannot
serve them: a provider receives one staged tree. Provide the format from each
member's own build program (a helper both call is enough), or pack the members
one at a time.

`--message-format json` (mcpp 2026.9.16.1+) prints one `mcpp.pack` envelope on
stdout after the command finishes and sends every human line to stderr. Its
`data.artifacts` holds each produced file or directory with its absolute path,
its `type` (`file` or `directory`), the `--pkg-format` value and the triples of its
legs; `data.stage` holds the staged tree, its manifest and whether the closure
was walked ([50 — Machine Output](50-machine-output.md)). A pack of several
members lists every member's artifacts, each naming its `member`, leaves
`data.stage` null and names one tree per member in `data.stages`
(2026.10.1.1+). `--pkg-format` names the
package format on this command, so machine output is asked for the way
`mcpp test` asks for it.

### `--pkg-format` owns one axis, and the engine owns four of its values

`tar`, `dir`, `zip` and `single-file` answer the same question `msi` and
`appimage` answer — what shape the output takes — so they are values of one
flag. The split between what the engine holds and what a package holds is:

> **`mcpp pack` owns the mechanism and the formats that need no other release:
> a tree, an archive and a single file. Every other format lives in a package,
> and `mcpp pack` dispatches to it.**

Those four extract, or start, and run, and need no knowledge of anyone else's
release. Everything past it does. dpkg's control
fields, AppImage's runtime, WiX's schema, Apple's notarisation, Android's
signing scheme: each one bound into the engine would couple an mcpp release to
a release mcpp does not control. The same argument the project already made for
languages, where Slang is supported without being named in the engine.

So the value set is open (mcpp 2026.9.11.1+). `--pkg-format <name>` finds the
package in the resolved graph that declares `<name>` and hands it the staged
tree; an unknown value names what *is* available rather than a fixed list:

```
error: unknown --pkg-format 'bogus'.
  available in this build: tar, dir, appimage
  A format past `tar` and `dir` comes from a package in the resolved graph, which declares
  it with `mcpp::provides_pack_format("<name>")` in its build program. Add the package
  that provides 'bogus' to [build-dependencies] and activate its feature.
```

The refusal arrives before anything is compiled. Writing such a package is
[Producing a distributable](30-build-mcpp.md#producing-a-distributable-pack_format--stage_dir-20269111);
the engine's three additions are a staged tree an `artifact` action can consume,
the rest of `[package]` in the build program, and this dispatch. Each is
format-neutral, which is the test for whether something belongs in the engine
at all.

A dispatched format applies to a **program** target — `kind = "app"` included,
on every row, whatever file that row links it to (see
[04 §2.2](04-mcpp-toml.md)). A library package ships an interface plus
prebuilt binaries per triple and has no single staged tree, so
`mcpp pack <lib> --pkg-format <name>` is refused rather than ignored.

**A `kind = "app"` target whose artifact is a shared object accepts more than
one `--target`** (mcpp 2026.9.13.2+): on every Android row an application
*is* the shared library the platform loads, so `mcpp pack myapp --target
aarch64-linux-android --target x86_64-linux-android` builds and stages both
legs into one tree, exactly as a library package's several triples already
do. Each leg lands at `lib/<abi>/lib<name>.so` (`aarch64` → `arm64-v8a`,
`x86_64` → `x86_64`) with its own closure beside it (see
[Android](#android-the-application-object-and-its-closure-under-lib)), the
declared deploy files are staged once, and one
dispatch runs against the combined tree — which is what lets a member such as
`dist-apk` build one universal APK. A single `--target` keeps today's flat
`lib/lib<name>.so` layout unchanged. A target whose artifact is an executable
on any requested row is still refused for a second `--target`: packing one
executable for several triples would need several executables, which is a
different mechanism (`lipo`'s universal binary) that this does not provide.

When `-o` is given a bare filename, the output is placed under `target/dist/`;
when it includes a directory (relative or absolute), the literal path is used.

For the full set of options, see `mcpp pack --help`.

### `mcpp run --pkg-format <name>` (mcpp 2026.9.12.3+)

```bash
mcpp run --target x86_64-linux-android --pkg-format apk
mcpp run --target aarch64-ios-sim      --pkg-format app
```

`--pkg-format` is the same flag `mcpp pack` takes, and reused here for the case a
plain `mcpp run` cannot reach: an Android application is a `.apk` and an
installed iOS application a `.app`, and neither is the link output `mcpp run`
executes by default. `mcpp run --pkg-format <name>` packs the target for `<name>`
— the same two passes and staged tree as `mcpp pack --pkg-format <name>` — and
then runs the artifact the pack reported, through the runner resolved for a
program: the project's `[target.<triple>] runner`, then a dependency's
`mcpp::runner(...)`, then the payload descriptor's.

The artifact a pack reports is the request's **terminal** one: among the
`artifact` actions the request introduced, the output no other introduced
action consumes. A provider is often a chain (`dist-apk`: link, add the
libraries, align, sign), and every output in it is verified to exist, but only
the last is the distributable and only it is printed as `Packed`. A format
whose chain ends in two files is refused by `mcpp run --pkg-format`, naming both,
because a runner takes one operand.

A format may report many outputs, one per file of a distribution tree. Up to
eight are printed one per line; more are printed by the entry each lies in
below their common directory, with a count (`Packed Release/app (1309
files)`), and `--verbose` prints every one (2026.9.29.5+).
`--message-format json` lists every output in either case.

An unknown `<name>` is refused naming the format set the resolved graph
provides, the same set `mcpp pack --pkg-format bogus` reports. `--pkg-format` together
with `--no-runner` is refused — an `.apk` or an installed `.app` cannot be
executed directly. Running a `kind = "app"` target on a row where its form is
a library, with no `--pkg-format`, is refused naming the flag and that same
format set. `mcpp test` is unaffected: a test binary is a program on every
row.

### The build inputs of a packed artifact, and its contents

Two things differ from `mcpp build`, and both exist because a package leaves
this machine:

**The profile falls back to `release`, not `dev`.** Precedence is unchanged
otherwise — `--profile` beats `[build] default-profile`, which beats the
fallback. Only the last step differs, so a project that states a profile still
gets the one it stated, and `mcpp pack` never produces an artifact built with
flags `mcpp build` would not.

> One consequence to know: a bare `mcpp build` and a bare `mcpp pack` now write
> into **different** `target/<triple>/<fingerprint>/` directories, because the
> fingerprint covers the profile. A file placed beside a built artifact by hand
> — a DLL, a data blob — is therefore only visible to `pack` when both commands
> resolve to the same profile: state it in `[build] default-profile`, or pass
> `--profile` to both. The declarative channels (`[runtime] deploy_files`,
> `runtime_search_dirs`) are unaffected.

**Debug information is stripped, and the publisher's paths go with it.** An
unstripped artifact carries DWARF, and DWARF carries the absolute paths of the
producer's source tree and build directory. What is removed depends on what the
artifact *is* — this is dh_strip's division, and the archive row is the one that
matters:

| artifact | strip flags | the reason for no more |
|---|---|---|
| executable | `--strip-all` | nothing links against it |
| shared library | `--strip-unneeded` | keeps `.dynsym` — that IS the export list |
| static archive | `--strip-debug --enable-deterministic-archives` | `--strip-all` removes the archive **symbol index**, and the consumer's link then fails with `archive has no index; run ranlib to add one` |

All three also drop `.comment` and `.note`. Section removal is by exact name, so
`.note.gnu.build-id` survives and still pairs with `--add-gnu-debuglink`.

`--no-strip` (or `[pack] strip = false`) ships the artifacts exactly as built.
`--debug-symbols <dir>` separates the information instead of discarding it:
`<dir>/<artifact>.debug` is written and the shipped artifact gets a
`.gnu_debuglink` pointing at it, which is what a debugger and `debuginfod`
follow.

> `[pack] strip` is not `[profile.<name>].strip`. The profile key appends `-s`
> to the **link**, which never touches a static archive and cannot separate
> anything; this one governs what the **package** carries. Two different
> decisions, two different names.

**What the graph built is stripped on every row that carries debug information in
the image; a library from the store or the host is not** (mcpp 2026.9.16.1+). The
rule is dh_strip's, a package strips what it built, and it covers:

| staged file | stripped | reason |
|---|---|---|
| the program | yes, as an executable; as a shared library on the Android row, where the program is one | this build compiled it |
| a shared library a `SharedLibrary` link unit of the graph produced | yes, `--strip-unneeded` | this build compiled it from source |
| the staged copy of the toolchain's own runtime (`libc++_shared.so` from the NDK) | yes, `--strip-unneeded` | a copy in the tree is not the shared payload; the Android Gradle plugin strips the same file |
| a library from the store, the host, or a prebuilt a package deployed | no | its bytes are its publisher's, and a vendor library may be signed |

Mach-O and MSVC PE keep their debug information beside the image, so nothing is
stripped there, and the `Packing` line says "stripped" only on a row that strips.
`--no-strip` and `--debug-symbols` reach every stripped file. A build program
that stages libraries of its own reads the same decision through
`mcpp::pack_strip()` and `mcpp::pack_debug_symbols_dir()`
([30 — build.mcpp](30-build-mcpp.md))

## Output Layout

The tarball contents are wrapped in a single top-level directory whose name
matches the tarball filename (minus the `.tar.gz`) —— this way both a GUI
"right-click extract" and a command-line `tar -xzf` yield the same
self-contained directory, instead of scattering the contents across the current
path.

**The closure is recorded, and an incomplete one refuses the archive**
(mcpp 2026.9.14.2+). Beside the staged tree, `<tree>.stage-manifest` states
every library name the closure resolved: the staged path of a library the tree
carries, `platform` for one the target provides, and `unresolved` for one found
nowhere (the line format is in [50](50-machine-output.md#the-stage-manifest)).
When a name is unresolved, `--pkg-format tar` and `--pkg-format dir` fail naming it,
because a tree without that library does not start where it is installed; a
dispatched format still receives the tree, with `closure = not-walked`. A
library the target provides and this machine does not have is named in
`[pack.bundle-project] also_skip`. The modes that bundle nothing (`system`,
`static`) state no closure.

### Mode `static`

```
target/dist/myapp-0.1.0-x86_64-linux-musl-static.tar.gz
└── myapp-0.1.0-x86_64-linux-musl-static/
    ├── bin/myapp                ← fully static ELF (no PT_INTERP / RUNPATH)
    ├── myapp                    ← top-level entry point (thin shell wrapper, run ./myapp directly)
    ├── README.md                ← copied automatically from the project root
    └── LICENSE
```

### Mode `vendored` (default; alias: `bundle-project`)

```
target/dist/myapp-0.1.0-x86_64-linux-gnu.tar.gz
└── myapp-0.1.0-x86_64-linux-gnu/
    ├── bin/myapp                ← dynamically linked, RUNPATH=$ORIGIN/../lib
    │                                PT_INTERP=/lib64/ld-linux-x86-64.so.2
    ├── lib/
    │   ├── libcurl.so.4         ← project third-party dependency
    │   ├── libssl.so.3
    │   └── ...
    ├── myapp                    ← top-level entry point
    ├── README.md
    └── LICENSE
```

The skip list follows
[PEP 600 / manylinux2014](https://peps.python.org/pep-0600/) ——
base libraries such as `libc`, `libm`, `libstdc++`, `libgcc_s`, and
`ld-linux-*` are assumed to already exist on the target system and are not
bundled into the tarball.

### Mode `self-contained` (alias: `bundle-all`)

```
target/dist/myapp-0.1.0-x86_64-linux-gnu-bundle-all.tar.gz
└── myapp-0.1.0-x86_64-linux-gnu-bundle-all/
    ├── bin/myapp
    ├── lib/
    │   ├── ld-linux-x86-64.so.2  ← complete loader and libc
    │   ├── libc.so.6
    │   ├── libstdc++.so.6
    │   ├── libgcc_s.so.1
    │   └── ...project dependencies
    ├── myapp                     ← one of two entry points
    ├── run.sh                    ← the other entry point (identical contents)
    ├── README.md
    └── LICENSE
```

With `-o foo.tar.gz`, the top-level directory name also becomes `foo` (the
package name and directory name always stay in sync).

The ELF specification forbids `PT_INTERP` from using `$ORIGIN`, so in
`self-contained` mode the loader is invoked by absolute path through `run.sh` (and
the top-level wrapper of the same name):

```sh
exec "$here/lib/ld-linux-x86-64.so.2" --library-path "$here/lib" "$here/bin/myapp" "$@"
```

The layout and wrapper above use an x86_64 example. The packer derives the
loader name from the target; for aarch64 it is `ld-linux-aarch64.so.1`.

#### `/proc/self/exe` under the bundled loader

Being started *by* the loader has a consequence the layout above does not
show: the kernel sets `/proc/self/exe` to the **loader**, not to the program,
and `/proc/self/cmdline` carries the `--library-path` argument. Every "find my
resources next to the executable" path therefore resolves against `lib/`
instead of the bundle root — and it does so silently. In practice that means
a GUI toolkit rendering blank text because it cannot find its fonts, an
`assets/` directory that appears to be missing, and helper binaries shipped
alongside the program that cannot be located. Code that parses `argv` from
`/proc/self/cmdline` sees the loader's arguments mixed in.

This affects `self-contained` only. `vendored`, `system` and `static` all
carry a `PT_INTERP` that the kernel can use directly, so `/proc/self/exe` is
correct there.

The wrapper exports **`MCPP_BUNDLE_DIR`** (the bundle root) for this. Resolve
against it first and fall back only when it is unset:

```c
const char *base = getenv("MCPP_BUNDLE_DIR");   /* set by run.sh */
if (!base) {
    /* not launched through the wrapper — /proc/self/exe is trustworthy */
}
```

If the application cannot be changed — a third-party GUI framework doing its
own resolution, say — use `--mode vendored` instead. It repoints `PT_INTERP`
at the host loader, at the cost of requiring the host's glibc to be at least
as new as the one the artifact was built against.

### WebAssembly (`wasm32-emscripten`): the stem family

```
target/dist/myapp-0.1.0-wasm32-emscripten.tar.gz
└── myapp-0.1.0-wasm32-emscripten/
    ├── bin/myapp.js              ← the launcher; the file a runner executes
    ├── bin/myapp.wasm            ← implicit output of the link, staged with it
    ├── bin/myapp.data            ← present only when the link carries --preload-file
    ├── README.md
    └── LICENSE
```

The packer stages the launcher plus every other `myapp.<anything>` the same
link wrote beside it — the stem family. `myapp.wasm` is required: the link
edge declares it as an implicit output, so its absence names a build
directory that does not match the graph, and `mcpp pack` refuses rather than
staging a launcher with no module. `myapp.data` and any further sibling emcc
writes with the same stem (`myapp.worker.js`, `myapp.wasm.map`) are optional
and travel exactly when the link produced them.

### Windows (PE): a `.zip`, with the DLLs beside the `.exe`

A Windows target produces a **`.zip`**, not a `.tar.gz`, and the layout is
flat:

```
target/dist/myapp-0.1.0-x86_64-pc-windows-msvc.zip
└── myapp-0.1.0-x86_64-pc-windows-msvc/
    ├── myapp.exe
    ├── vcruntime140.dll        ← only under cxx_runtime = "toolchain-coupled"
    ├── mydep.dll               ← third-party dependencies
    ├── README.md
    └── LICENSE
```

There is no `bin/` + `lib/` split and no entry-point wrapper, and neither is a
style choice. The Win32 loader resolves a DLL from **the directory of the
executable**; PE has no `RUNPATH` to point anywhere else, so "next to the
`.exe`" *is* the mechanism that `$ORIGIN/../lib` provides on ELF.

**Windows' own DLLs are never bundled** — `kernel32.dll`, `ntdll.dll`,
`ucrtbase.dll`, the `api-ms-win-*` API sets. Shipping a private copy of an OS
component is a broken program rather than a heavier one (the process ends up
with two of something that must be unique), and Microsoft's redistribution
terms say the same thing from the other side. `[pack.bundle-project]
force_bundle` still overrides this, as it does the ELF skip list.

`vcruntime140.dll` and `msvcp140.dll` are **not** Windows' own: they belong to
the MSVC toolset, exactly as `libstdc++.so` belongs to gcc. Whether they
travel is decided by `cxx_runtime` (see `docs/04-mcpp-toml.md`), not by this
list — and `mcpp pack` refuses a combination that cannot deliver what the
contract promised:

```
$ mcpp pack --mode system          # with cxx_runtime = "toolchain-coupled"
error: cxx_runtime = "toolchain-coupled" and --mode system contradict each other.
```

#### Packing a Windows program from Linux or macOS

This works, and it is not a special mode — just build for a Windows target and
pack:

```bash
mcpp pack --target x86_64-windows-gnu     # from a Linux host
```

`mcpp pack` used to refuse Windows outright. The reason was not the archiver:
the ELF dependency closure is obtained by **running the artifact** under
`LD_TRACE_LOADED_OBJECTS`, which cannot cross an OS *or* an architecture. A PE
closure is read out of the file's import table instead, so nothing has to be
executed and the packaging host is free. The archive is written by mcpp itself
for the same reason — there is no zip tool present on every host.

Two consequences worth knowing:

- Entries are **deflated** by zlib when that makes them smaller, and stored
  otherwise; `--compress gzip:<level>` sets the level, `--compress none` stores
  every entry. A format left unstated is a zip for a Windows program;
  `--pkg-format tar` stated writes a tar.
- The archive is **deterministic**: no timestamps are read, so two packs of
  the same tree are byte-identical and a published checksum means something.

The reverse direction — packing a Linux artifact *from* Windows — still does
not work: an ELF program's closure is resolved by the target's own dynamic
linker, which a Windows host has no way to run. A Mach-O program's closure is
read from the file, so a macOS program packs from any host.

### macOS (Mach-O): the dylibs beside the program

```
target/dist/myapp-0.1.0-aarch64-macos.tar.gz
└── myapp-0.1.0-aarch64-macos/
    ├── bin/myapp
    ├── bin/libmydep.dylib      ← a dependency's dylib, beside the program
    ├── myapp                   ← entry-point wrapper
    ├── README.md
    └── LICENSE
```

A Mach-O program's closure is read from its load commands (mcpp 2026.9.14.2+),
so packing it runs nothing. Its dylibs are staged beside it in `bin/`, which is
where the `@loader_path` rpath a consumer of a graph-built dylib is linked with
already looks: no load command is edited, nothing is re-signed, and the program
is not stripped.

A needed name is staged when the loader resolves it to the program's
directory:

| Name | Treatment |
|---|---|
| `/usr/lib/…`, `/System/Library/…` | not staged; the OS provides it |
| `@rpath/<file>` | staged when an rpath of the image that needs it, or of the program, is exactly `@loader_path` or `@executable_path` |
| `@loader_path/<file>`, `@executable_path/<file>` | staged at that path, which must stay inside the program's directory |

Any other name leaves the closure incomplete, and `tar` and `dir` refuse naming
it. An absolute install name outside the OS's directories is the common case:
the loader reads it from that path on every machine, whatever the tree carries.
`--mode system` and `--mode static` bundle nothing on this row, as on PE. A
`kind = "lib"` / `"shared"` target is packaged by the library route, which is
unaffected.

### ok images (`<arch>-openkal`): the image, or a program that carries it

An ok image runs through `okrun` on every system of its architecture (see
[24](24-openkal-cross.md)), and its platform states how it is packed
([35](35-platforms.md)). The mode keeps its meaning — what the machine that
runs the package must provide:

| Mode | The package holds | The machine provides |
|---|---|---|
| `system` | the tree below: `lib/<arch>/<name>.oke` and the launchers | `okrun` and every image it names |
| `vendored` (default) | the tree with the `.okso` images the image names | `okrun` |
| `self-contained` | the tree with the runner of each system of `--systems` and each architecture under `libexec/okrun/<os>-<arch>/` | nothing |
| `static` (`--linkage static --mode self-contained`) | the self-contained tree of one static `.oke` per architecture | nothing |

The layout is the same for one leg and for several (2026.10.30.1+).
`--linkage static` builds the image with no `.okso`. A program of this
machine that carries the image in the platform's carrier section (`.oke` on ELF
and COFF, `__OKE,__carried` on Mach-O) is the single file
(`--pkg-format single-file`, below), and only the single file.

### Android: the application object and its closure under `lib/`

```
target/dist/myapp-0.1.0-x86_64-linux-android/
├── lib/libmyapp.so             ← the application object
├── lib/libmydep.so             ← a dependency's shared library
├── lib/libc++_shared.so        ← the NDK's C++ runtime, when the object needs it
└── bin/<to>/…                  ← files `[runtime] deploy` placed
```

On an Android row the application is a shared object staged under `lib/`, and
its closure is read from the files and staged beside it (mcpp 2026.9.14.2+). A
name the device provides is not staged: one present in the API level's stub
directory, which mcpp takes from the row's own compiler (the directory it finds
`libc.so` in) — `libc.so`, `libm.so`, `libdl.so`, `liblog.so` and the other
platform libraries. Every other name must resolve in a directory the link used:
the build's output directory, the `[runtime] library_dirs` and
`link_library_dirs` of the packages in the graph, and the compiler's library
search path, which holds `libc++_shared.so`. A name that resolves nowhere makes
`tar` and `dir` refuse, naming it and the directories searched.

With several `--target` triples each leg is staged under `lib/<abi>/` with its
own closure. `--mode` does not apply on this row: the device provides exactly
the platform libraries, so there is no host set to leave out and no loader to
carry.

### The tree of a platform's images

An image of a platform that states a runner (openkal-platform does) is packed
as one tree, whatever the number of legs: `--target x86_64-openkal` is the tree
of one leg, `hello-0.1.0-x86_64-openkal/`; `--target openkal` is the tree of
both, named after the platform. One tree serves every system and every
architecture it holds:

```
target/dist/hello-0.1.0-openkal/
├── bin/hello                    ← POSIX launcher
├── bin/hello.exe                ← Windows launcher (a program)
├── lib/x86_64/hello.oke         ← one set of images per leg
├── lib/aarch64/hello.oke
├── libexec/okrun/<os>-<arch>/   ← only with --mode self-contained
├── share/…                      ← resources, once
└── mcpp-package.json
```

The other legs are planned one after another and built at once
(`Packed leg <triple>`). The launchers are generated from the platform's
`runner`: the POSIX script resolves the package as the parent of its own
directory, picks the architecture directory from `uname -m` (`x86_64`, `amd64`
→ `x86_64`; `aarch64`, `arm64` → `aarch64`), hands a Git Bash, MSYS or Cygwin
shell to `bin/<name>.exe`, and starts the runner on the image there with its
own arguments, looking for the runner first in `libexec/<runner>/<os>-<arch>/`
and then on `PATH`. The Windows launcher is the engine's program built for
`x86_64-windows-gnu`: it finds the package from its own path, picks the image
of the machine's architecture, and starts the runner the same way; no `.cmd` is
written. A machine of an architecture the package does not hold gets exit
status 126 and a sentence naming both. Files `[runtime] deploy` places lie
beside each leg's image.

Under `--mode self-contained` the runner travels: for each system of
`--systems` (`linux`, `macos`, `windows`, each for every architecture of the
legs; `linux-aarch64` for one pair; `all`; default: this machine's) mcpp builds
the platform's carrier with nothing carried — a program that runs the image its
first argument names, as `okrun` does — into `libexec/<runner>/<os>-<arch>/`.
Each runner is built on its row (`<arch>-linux-musl`, `<arch>-macos`,
`<arch>-windows-gnu`) over the platform's hosted arrangement, so the same
program results whichever machine packs, with no SDK or C run-time of that
system; a Linux runner is static. A row this mcpp builds no program for
(`x86_64-macos`, planned; `aarch64-windows-gnu`, not in its table) is left out
with a warning. An image built on a Windows host is not the bytes of one built
elsewhere (#850): pack trees on Linux or macOS.

**Where it runs.** The pack writes `runs_on` into `mcpp-package.json` and says
it after `Packed`: `self`, the `<os>-<arch>` pairs the package runs on with
what it carries, and `with.<runner>`, the pairs it runs on where the machine
has the runner (the legs' architectures × the platform's `runner.hosts`).

```
      Packed target/dist/hello-0.1.0-openkal-bundle-all
     Runs on linux-aarch64, linux-x86_64, macos-aarch64, windows-x86_64
 Not covered macos-x86_64 (no runner row: this mcpp builds no program for x86_64-macos)
```

A platform that states no `runner.hosts` is read as every system of each
architecture. A tree that runs nowhere — no runner carried and none of the
platform's runner pairs of its architectures — is refused.

Packages built on several machines are joined with
`mcpp pack --merge a.zip --merge b.zip` (also `--merge a.zip,b.zip`). The inputs
are package directories or archives (`.zip`, `.tar`, `.tar.gz`, `.tgz`,
`.tar.zst`, `.tar.xz`) this command wrote; nothing is built. Legs and systems
and `runs_on` are united, each file is taken once, and the result is described
anew. The launchers are derived: written again for the merged legs, so two
one-leg trees merge into the tree of both, named after the platform when `-o`
is left out (`hello-0.1.0-x86_64-openkal` → `hello-0.1.0-openkal`). Inputs of
another name, version, platform, linkage or mode are refused, and so is one
path with two digests, naming the path, both digests and both inputs. The
container is `--pkg-format`, else the extension of `-o`, else the first
input's.

**A list of containers.** `--pkg-format tar,okb` (or `[pack] format = ["tar",
"okb"]`) builds once, stages one tree and writes each container from it, in the
order named. A list holds the containers of one tree — `tar`, `zip` and a
provider's format — never `dir` or `single-file`; it takes a directory for `-o`;
`--compress` applies to its engine containers. okb holds the images alone, so a
self-contained tree whose only format is okb is refused (`--pkg-format
tar,okb`). `mcpp run --pkg-format` takes one format.

### A single-file package

`--pkg-format single-file` delivers one executable file:

| Target and mode | The file | On start |
|---|---|---|
| native, `vendored` or `self-contained` | the engine's launcher carrying the staged tree as one zstd frame | the first run unpacks the tree into the user's cache, and later runs start it from there |
| native Linux, `--linkage static` | a static launcher and the static program | as above, with no system library |
| ok image | the platform's carrier and the image, or a zip of all its images | the carrier loads the images from memory and writes nothing to disk |

A native program's file is `target/dist/<name>-<version>-<triple>` (`.exe` on
Windows); its tree is staged under `target/single-file/stage/`. The launcher is
a small program whose source mcpp writes with each pack (under
`target/single-file/<name>/`, over mcpplibs.os and zstd) and builds for the
target: static (musl) on Linux, with the system's libraries only on macOS and
Windows. The payload is one zstd frame (`--compress zstd[:<level>]`, the
default, or `none`) of the tree. The first run unpacks it into
`<cache>/mcpp/single-file/<sha256 of the payload>/`, where `<cache>` is
`$XDG_CACHE_HOME` (else `~/.cache`) on Linux, `~/Library/Caches` on macOS and
`%LOCALAPPDATA%` on Windows: into a name of its own beside it, renamed into
place without replacing when every file is written (`.mcpp-complete` marks a
complete tree; the directory is the user's alone on Linux and macOS). The frame
carries its content's checksum, and a payload that does not match it is never
unpacked. The launcher then becomes the program — `bin/<name>`, the wrapper at
the root of a self-contained Linux tree, or `<name>.exe` at the root of a
Windows tree — with its arguments, and its exit status is the program's.

An ok image's file is `target/dist/<name>-<version>-<os>-<arch>` (`.exe` on
Windows), one per leg and system of the leg's architecture (`--systems`;
default: this machine's): `--target openkal --pkg-format single-file -o dist/`
writes one per architecture. `--systems all` makes the file of each row this
mcpp builds a program for and says the others (`Not covered`); more than one
file takes a directory for `-o`. The carrier
carries the image alone when it names no other image, and otherwise a stored
zip of all the images, each aligned to 64 KiB, which the carrier's entry reads
through openkal-run's bundle interface (openkal-platform 0.17.2 and later).

A WebAssembly target has no single file: `--pkg-format single-file` with
`--target wasm32-emscripten` is refused before anything compiles
(2026.10.29.1+), and its page is the `dist-web` provider's format.

### `mcpp-package.json`

Every staged tree carries `mcpp-package.json`: the package, its platform, its
three choices, its legs and systems, how it starts, and each file with its role
and sha256. A provided format, `--merge` and `mcpp emit xpkg` read it
([SPEC-014](specs/packaging.md) §4.5):

```json
{"schema": 1, "name": "hello", "version": "0.1.0", "platform": "openkal-platform@0.18.0",
 "linkage": "shared", "mode": "vendored", "legs": ["x86_64-openkal", "aarch64-openkal"], "systems": [],
 "launch": {"runner": ["okrun", "{}"], "entry": {"x86_64": "lib/x86_64/hello.oke", "aarch64": "lib/aarch64/hello.oke"}},
 "files": [{"path": "lib/x86_64/hello.oke", "role": "image", "leg": "x86_64-openkal", "sha256": "…"}]}
```

The file is written for every pack, a provided format's included. Each file's
`role` is `program`, `image`, `library`, `loader`, `launcher`, `resource` or
`metadata`; in a tree every file of `lib/<arch>/` is an image of that leg.
Symbolic links are not listed. `linkage` is `static`, `shared` or `declared`
(each library as it declares itself); `launch` is `program` for one native leg
and `runner` with an `entry` per architecture for a tree; `runs_on` is where the
package runs (`{"self": [...], "with": {"okrun": [...]}}`), and `systems` the
runners it carries.

`mcpp emit xpkg --package <archive> [--package …] --url <prefix> [-o <file>]`
writes the xim-pkgindex entry (spec 2) of packages this command wrote, from
their `mcpp-package.json`. A tree is one asset; each system's block names the
architectures its `runs_on` covers, with the runner as a dependency
(`deps = { "xim:okrun" }`) unless the package carries it, and a tree whose
`runs_on` is empty is refused; packages of one system
and architecture each make that system's entry, one URL with `${arch}` and a
sha256 per architecture. The install moves the archive's top directory into
place and registers the program with xvm (`bin/<name>.exe` on Windows),
from each system's own directory when the packages place the program apart
(a Windows program at the root, a POSIX one under `bin/`). Each archive is its
own `--package`. A package of one leg of a Linux, macOS or Windows target is
described for the triple's system and architecture. A package whose program is
no program of a system is refused, naming the route that distributes it: a
`wasm32-emscripten` package is published as a page (`dist-web`), an ok image as
its platform's tree (`--target openkal`) or a single file, and an Android, iOS
or bare-metal package has no entry.

### Compression and signing

`--compress zstd|gzip|xz|none[:level]` (or `[pack] compress`) chooses the
compression of `tar` (`.tar.gz`, the default, `.tar.zst`, `.tar.xz`, `.tar`).
`tar` writes the archive and mcpp compresses it with its own codecs (zlib, zstd
and liblzma), so no compressor is looked for on `PATH` and an archive is the
same bytes on every host. A `zip` deflates its entries (`gzip[:level]`) or
stores them (`none`); a single file's payload is `zstd` or `none`; the images
inside an okb are stored so that the loader maps them in place.

`[pack] sign = ["<prog>", …, "{}"]` runs once on each program, image, library
and loader of the staged tree and on the Windows launcher before the container
is written, and on each
single file; `{}` is the file. A failure stops the pack, naming the argv and its
output, and `mcpp-package.json` states the digests of the signed files.

## Configuration

Packaging behavior is configured via the `[pack]` section in `mcpp.toml`. The
common fields are:

```toml
[pack]
default_mode  = "static"            # override the normal vendored default for bare `mcpp pack`
linkage       = "shared"            # the default of --linkage
format        = "tar"               # the default of --pkg-format; a list: ["tar", "okb"]
targets       = ["openkal"]         # the default of --target: targets and target sets
systems       = ["linux", "macos"]  # the default of --systems
compress      = "zstd"              # the default of --compress
sign          = ["signtool", "sign", "{}"]   # run on each program, image, library and loader before the container
strip         = true                # default. false ships the artifacts as built
debug_symbols = "dist/debug"        # separate the debug info here instead of discarding it
include       = ["share/**", "config/*.toml"]   # project files staged at the same path, once
exclude       = ["debug/**"]

# Fine-tune the vendored filtering policy. The configuration key keeps its
# established `bundle-project` spelling.
[pack.bundle-project]
also_skip    = ["libcustom.so"]     # libraries assumed to exist on the target system
force_bundle = ["libfoo.so"]        # bundle even if matched by the PEP 600 list
```

`[pack].default_mode` accepts the canonical names `system`, `vendored`,
`self-contained` and `static` (2026.10.30.1+) and the established spellings
`bundle-project` and `bundle-all`.

`[pack] universal`, `[platform] universal` and `--no-universal` no longer decide
what a pack holds (2026.10.30.1+; [51](51-supported-versions.md)):
`[pack] universal = true` is refused with `[pack] targets = ["openkal"]` as the
remedy; `false` and `[platform] universal` are noted and ignored;
`--no-universal` with one target is noted, and with a target set refused.

The `static` mode additionally requires a musl toolchain configured under
`[target.<triple>]`; for the full setup, see the `mcpp.toml` in
[`examples/03-pack-static`](../examples/03-pack-static/).

## Current limitations

- Distribution formats such as `.deb`, `.rpm`, AppImage and `.msi` come from
  packages through `--pkg-format <name>`; the engine provides `tar`, `dir`, `zip`
  and `single-file`.
- One file that runs on several systems is not produced; a single-file package
  serves one system and one architecture.
- A WebAssembly package is neither a single file nor an xlings entry; its page is
  a provider's format (`dist-web`).
- An ELF program's closure is resolved by running its dynamic linker, so a Linux
  program is not packed from Windows.

