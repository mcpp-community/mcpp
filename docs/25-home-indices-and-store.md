# 25 — The Home, Its Indices and Its Store

**Reader:** someone who decides where mcpp keeps what it installs: a developer
with an existing `~/.xlings`, an administrator of a shared machine, a Luban
system.

**The question this chapter answers:** what is in an mcpp home, which program
owns each part, and how a home reuses packages another home already has.

**Not here:** choosing a toolchain, which is [20 — Toolchain
Management](20-toolchains.md), and a project's own build environment, which is
[23 — The Project Environment](23-the-project-environment.md).

mcpp installs toolchains and packages through xlings, its package provider. A
payload — an installed package version — is written by the provider alone and
is never modified afterwards: mcpp does not patch, mark or copy it. Whatever a
build needs that a payload does not hold (a loader, a run path, a sysroot) is
stated on the build's own command lines.

## 1. The layout

```
~/.mcpp/                    (MCPP_HOME)
  config.toml
  indices/<name>/           the data of each index mcpp serves (mcpplibs, [index.repos.<name>])
  xpkgs/                    the payloads those indices serve
  registry/                 the provider's own home: its index (xim), its runtime environments, its store
    data/xpkgs/             toolchains and tools; an entry may be a link to an upstream's payload
  build-cache/  cache/  provisioned/ …

<project>/.mcpp/
  indices/<name>/           the data of the project's own [indices]
  xpkgs/                    the payloads they serve
```

Ownership follows the index. An index mcpp serves keeps its data and its
payloads in mcpp's home; the provider's own indexes (`xim` and the ones it adds)
stay under `registry/`. A local path index (`path = "…"`) is read where it is.

A home created by an earlier mcpp keeps working. Its index data moves to
`indices/<name>` once, on the first run; payloads already installed under
`registry/data/xpkgs` stay where they are and are still found, and what was
built against them keeps running. The markers an earlier mcpp wrote into
payloads (`.mcpp_ok`, `.mcpp-fixup.json`) are no longer read.

### Selecting the home (2026.10.19.1+)

Choose a directory before the first initialization:

```powershell
mcpp self init --home D:\mcpp
```

The selection persists across processes without setting an environment variable.
The original home is chosen by `MCPP_HOME`, then a portable installation root,
then the user's `.mcpp`. Its `home.toml` names the effective home; mcpp reads
that file once. By default the original and effective directories are the same.
A locator in the target directory is ignored. `MCPP_HOME` is never rewritten.

An existing initialized home requires an explicit switch:

```powershell
mcpp self init --force --home E:\mcpp
```

This saves the new default and initializes it. Old data and existing target data
remain in place. `mcpp self config` and `mcpp self doctor` report the original,
effective and previous directories when relevant. The previous path is a report,
not a migration or automatic cleanup instruction. Nested original/target homes
are refused. Selecting the original directory removes its locator.

On Windows, the first explicit `self init` may offer `D:\mcpp` and the user's
`.mcpp` when D is a writable local fixed drive and stdin/stderr are terminals.
An existing `D:\mcpp` that is a file or reparse point is not suggested; an explicit
`--home` still selects a custom path. Enter selects D; only `1`, `2` and an empty line are accepted. Other input exits
with a `--home` hint. There is no menu on Linux, macOS or noninteractive runs,
or when an environment, portable installation or saved selection supplies home.

`self init --force` without `--home` retains its registry/cache reset semantics.
For a redirected home it names the effective target before resetting it.
`--system self init --home` is refused. The exact selection and persistence
rules are in [SPEC-010](specs/home-initialization.md).

### Initialization and network diagnosis (2026.10.19.1+)

`self env`, `self config`, `self doctor` and `toolchain list` read local state
without installing tools. `self config --mirror auto|CN|GLOBAL` updates only local
configuration, so it remains usable while the network is broken.

### Retention of build outputs (2026.10.31.3+)

The build cache under `build-cache/v1/` is shared by every project of the home;
a project's `target/` is its own. Each has a retention, in config.toml:

| key | default | removed |
|---|---|---|
| `[cache] unused_days` | 30 | after a build, at most once a day: cache entries (packages, `std` modules, host tools) no build used for that many days |
| `[build] keep_days` | 3 | after `mcpp build` or `mcpp test`, at most once a day per project: build directories no configuration used for that many days |

`0` keeps everything. `mcpp self config --cache-unused-days <N>` and
`--keep-days <N>` write them; `mcpp self config` shows them.

A cache entry's files never change after they are written, and a file system
mounted `noatime` or `relatime` does not record reads, so mcpp states a use: it
sets the time of the entry's record file (`entry.json`, `std-module.json`) to
now when a build uses the entry, at most once in 12 hours, so a use costs one
`stat`. A build that replays its graph without planning does not reach the
cache; each configuration directory names the entries it used in `uses.list`,
and the sweep after a build marks them used.

Before a missing bootstrap tool requires network access, initialization displays
bounded checks of the provider's configured resources. `self doctor` reports the
same checks as warnings; local errors keep their meaning. `--offline` skips the
checks and acquisition. Installed tools and cached packages remain usable.
A failed probe does not disable a source or block local reuse.

The locator is saved after local validation, before acquisition. An incomplete
offline or failed-network init returns 1 while retaining the chosen directory.
Rerunning `self init` continues there.

## 2. Refreshing an index

An index mcpp owns is fetched into a staging directory beside it, checked there
against the floor it declares (`index.toml` `min_mcpp`), and exchanged with the
copy in use only when this mcpp can read it. A refreshed index that needs a
newer mcpp leaves the previous copy in place and is reported once, after the
command's own output.

## 3. Reusing another home: upstreams

```toml
# ~/.mcpp/config.toml
[xlings]
upstream = ["~/.xlings"]
```

An upstream is a home whose payloads this home installs from **by link**: a
payload of the same identity — namespace, name, version and the recipe's
revision — that is complete there is published in this home's store as a
symbolic link (a junction on Windows). Nothing is downloaded and the upstream is
never written to. Removing the package here removes the link only. When the
upstream's payload disappears, the next install puts a payload of its own at the
same path, so what was built against that path keeps working; with `--offline`
that install cannot happen and the command fails.

| upstream | provides |
|---|---|
| an xlings home (`~/.xlings`, `/xlings`) | the provider's own packages (`xim`) |
| an mcpp home (another `~/.mcpp`, the system home) | its registry's `xim` packages, and the packages of mcpp's indices in its `xpkgs` |

mcpp's own packages are taken only from an mcpp home, never from an xlings home.

**Trust.** An upstream belongs to the current user or to the system (root; on
Windows also Administrators and SYSTEM), and when it is not the user's own the
user cannot write it. A home that fails this is refused when the configuration
is read: using another person's home means running programs that person can
replace. An upstream that does not exist, or is neither kind of home, is
reported as a degradation (no reuse this run).

**Where it is read.** `upstream` in the user's `config.toml`; when absent, the
system-level configuration (`/etc/mcpp/config.toml`; Windows
`%ProgramData%\mcpp\config.toml`); when absent too, none — the default, a
self-contained home.

**A home that already has copies.** `mcpp self store dedupe` lists the payloads
this home installed itself that an upstream has the same of;
`mcpp self store dedupe --apply` replaces each with a link, atomically.

## 4. A machine's system home

On a machine shared by several users an administrator maintains one home for
all of them:

```bash
sudo mcpp --system self init
sudo mcpp --system toolchain install gcc 16.1.0
```

| | |
|---|---|
| location | `[system] home` of the system configuration; else `/var/lib/mcpp` (macOS `/Library/Application Support/mcpp`, Windows `%ProgramData%\mcpp`) |
| written by | `mcpp --system` with one of `self init`, `self doctor`, `self store`, `toolchain install`, `toolchain remove`, `toolchain default`, `index update` |
| refused | `--system` with any other command (a build as root would write root's files into a user's project); `--system` without write access to the system home (the message names `sudo`) |
| written as | files readable by every user (umask 022), one writer at a time, the caller's `HOME` and `MCPP_HOME` ignored |
| marked by | `.mcpp-home` with `mode = "system"` |

Users read it without root. The system configuration names it as their
upstream, and may name a default toolchain:

```toml
# /etc/mcpp/config.toml
[xlings]
upstream = ["/var/lib/mcpp"]

[toolchain]
default = "gcc@16.1.0"
default_target = "x86_64-linux-gnu"
```

Each user keeps a home of their own: their index data, their build cache (never
shared between users) and links into the system home's payloads. An index of
the user's that resolves a newer version installs it into the user's home.

On Luban OS the system configuration names `/xlings` as well, so a payload the
machine already has exists on it once.

`config.toml` (the user's, then the system's) may also state a machine's
defaults for one target (2026.10.24.1+): its base and how its artifacts reach
the machine that runs them. A project's `mcpp.toml` outranks both, and
`MCPP_TARGET_<TRIPLE>_SYSROOT` / `_RUN_UNDER` override all three; a relative
`path` is relative to the `config.toml` that writes it.

```toml
# ~/.mcpp/config.toml
[target.x86_64-ios-sim]
run_under = ["remote-run", "macvm", "--"]   # only this machine reaches a Mac this way

[target.x86_64-linux-gnu]
sysroot = { path = "/opt/acme/sysroot" }    # the team's agreed path, on a machine without it in the repo
```

The table takes `sysroot` and `run_under` and nothing else: everything else
about a target belongs to the project. See [20 — Toolchains](20-toolchains.md)
and [04 — mcpp.toml](04-mcpp-toml.md).

## 5. The provider program

`[xlings] binary` chooses the xlings mcpp runs: `"bundled"` (the one mcpp
ships, the default), `"system"` (the one on `PATH`) or a path. Whichever it is,
mcpp asks it what it is (`xlings interface --version`): its version, its
protocol and the features mcpp relies on. One that cannot serve this mcpp gives
way to the bundled one, with a note naming why.

`[xlings] home` moves the registry; it names a home mcpp made. A home mcpp did
not make (an `~/.xlings`) is refused there: reusing its packages is what
`upstream` is for (§3).

## 6. Filesystems and long paths

A home may be on any filesystem. mcpp measures what the one holding its home
can do — an atomic rename that refuses an existing name, an atomic exchange,
hard links, symbolic links — and publishes by what it measured.
`mcpp self doctor` shows the answer, and whether reuse from an upstream is
possible there (it needs symbolic links).

On Windows a path is limited to 260 characters unless the system enables long
paths (`LongPathsEnabled` = 1). `mcpp.exe` declares itself long-path aware. When
long paths are off and `MCPP_HOME` is long enough that a toolchain's deepest
file would pass the limit, mcpp reports a degradation naming the two remedies:
enable long paths, or use a shorter `MCPP_HOME`. mcpp does not change the
setting; `mcpp self doctor` reports it.

A program's own manifest states whether it is long-path aware:
`windows_long_paths` ([04](04-mcpp-toml.md)).

## Current limitations

The home locator follows one hop only. It does not migrate or remove old data, and nested original/target directories are refused. The Windows drive picker is limited to a first explicit terminal initialization; a custom path uses `--home`. Network probes diagnose a resource at that moment and cannot guarantee a later download.

Failure categories depend on the transport details available to the provider. Some TLS handshake failures appear as `connect`; that status does not prove that TCP itself was refused.

Status queries require the provider's `read_only_queries` feature. A custom older provider without it is skipped with a diagnostic warning; local checks continue without installing a replacement. The binary handshake runs in an isolated temporary home.
