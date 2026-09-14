---
subject: design
status: active
---

# The build database of #636, and two defects on the way to the latest xlings

**Status:** proposed on 2026-09-14; adopted the same day as the plan in §6, with
the decisions of §7 settled by the self-review in §8. Landed on 2026-09-15 in one
pull request per repository, mcpp-community/mcpp#639 for every mcpp change and
openxlings/xlings#596 for item A, and released as mcpp 2026.9.15.1 with xlings
2026.9.14.1; §9 is the execution record. Code was read at mcpp
`9b6a1188` (2026.9.14.3), xlings `59068d6` (2026.9.12.1, the latest release) and
lsp-mcpp `4ea9f81` (S1 profile 0.2.0, S2 0.2.0). Measured: the store of this
host's mcpp registry, and the Windows CI logs of 2026-09-14 on `main`. A
statement marked *(code)* was derived from source and has not been run; §6.3
lists the runs that settle each one.

## 0. Ledger

| | subject | classification | home | vehicle |
|---|---|---|---|---|
| A | xlings moves the whole shared download directory into a package installed without an `install()` hook | defect, general | xlings installer; xpkg spec §6 | xlings 2026.9.14.1 |
| B | mcpp on Windows prints `The system cannot find the path specified.` in every command after the first | defect, general | mcpp's calls of the platform launchers | mcpp#639, with the move of `kXlingsVersion` |
| C | #636, `mcpp emit build-database` | feature | engine for facts and a neutral format; the S1 library for structured options; the consumer for editor policy | mcpp#639 |

"The latest xlings inside mcpp" is the xlings release that carries A. B ships
with that pin change, because B is what lets a pin change reach an existing
Windows home (§3.3).

## 1. The rule behind all three

A behaviour that another component depends on is written in a specification
that component can cite, and it is tested at that boundary. No component
depends on another component's unwritten behaviour or internal layout.

- **A.** xpkg spec §6 defines what `install()` does and says nothing about a
  package without one. The implementation's accident became the contract: 55
  mcpp-index descriptors use pointers of the form `mcpp = "*/…/mcpp.toml"`,
  which match only because the archive's top-level directory is never stripped.
- **B.** A command string written for `/bin/sh` is handed to `cmd.exe`.
- **C.** lsp-mcpp today parses mcpp's `build.ninja`, reads `std-module.json`
  from the std cache, and parses `mcpp.toml` as text (lsp-mcpp
  `src/project/mcpp.cpp:22-40`, `:54`, `:65`). #636 replaces these reads with a
  published document. This design keeps that document a projection of the
  plan, and keeps editor policy out of the engine.

## 2. A — a hookless install sweeps the download directory

### 2.1 Mechanism (xlings `59068d6`)

1. Every download of a data root lands in one directory, `<data>/runtimedir`
   (`src/core/xim/installer.cpp:963-968`, `:2575`), beside its `<file>.lock`
   (held while downloading), `<file>.meta` and `<file>.part.*` staging files
   (`downloader.cpp:111`, `:484-492`).
2. Every archive is extracted into that same directory (`installer.cpp:2831-2835`);
   `extract_archive_detailed` returns the directory itself (`extract.cpp:300`).
3. When `install_dir` is still empty after hooks (always for a package without
   `install()`, and for a hook that does nothing), `stage_extracted_payload_`
   receives `runtimedir` and moves every entry in it (`installer.cpp:1121-1190`,
   called at `:3116-3117`). Its branch that strips a single top-level directory
   (`:1135-1137`) never runs for a downloaded archive, because the archive
   itself is always a second entry.

### 2.2 Measured on this host

- mcpp registry, `~/.mcpp/registry/data/xpkgs`: 221 of 401 version directories
  contain `*.lock` sidecars, and their top-level archives alone total 5.2 GiB.
  `mcpplibs-x-openkal/0.8.0` holds 1.6 GiB (nine archives, among them gcc 13.3.0
  at 329 MiB with its extracted tree); `mcpplibs-x-openkal-emscripten/0.1.0`
  holds 1.5 GiB (59 archives, android-ndk-r30 at 704 MiB, and `perl-5.44.0/`).
  The installing clients were xlings 2026.8.27.1 and 2026.9.5.1
  (`.xpkg-install.json`); the second is mcpp's current pin.
- The registry's `runtimedir` is empty (12 KiB). `~/.xlings/data/runtimedir`
  holds 7.2 GiB, and none of that home's 296 version directories contains a
  sidecar. An empty download directory is the signature of the sweep: mcpp-index
  descriptors carry only `url` and `sha256`, so mcpp's registry is where
  hookless installs happen.

Consequences beyond disk:

- a swept archive has left the download cache and is downloaded again;
- *(code)* a swept `.lock` may be held by another process, and a swept
  `.part.*` file is another process's download in progress;
- *(code)* mcpp's default manifest lookup and every `*/…` pointer require
  exactly one match (`src/build/prepare.cppm:5610`, `:5626-5648`), so a swept
  source tree that carries its own `mcpp.toml` turns a working dependency into
  "matched 2 files".

### 2.3 The rule, added to xpkg-manifest-v1 §6

> A package that defines no `install()` receives in `pkginfo.install_dir()` the
> entries of its own archive, laid out as the archive lays them out, and
> nothing else. The archive stays in the download cache.

The top-level directory is kept. That is the layout every hookless install has
produced (2.1, step 3); the 55 descriptors and mcpp's `*/mcpp.toml` lookup are
written against it, and a stripping layout would need an opt-in field that
nothing asks for.

### 2.4 The change

- A package without `install()` extracts into a private directory,
  `<runtimedir>/.stage/<plan-key>-<pid>/`, removed on every exit path. A package
  with a hook extracts beside its archive as before; that is its contract (spec
  §6, whose example at line 293 derives the extracted directory from
  `install_file()`).
- The staging fallback always reads a private extraction. For a hook that left
  `install_dir` empty (the patchelf case named at `installer.cpp:3154`), the
  archive is extracted a second time, privately.
- `stage_extracted_payload_` moves every entry of the private directory. Its
  stripping branch is deleted: with a private directory it would become
  reachable and change the layout.
- Script and SubOS packages are unaffected; their default installers fill
  `install_dir` before staging.

### 2.5 Stores that are already swept

`xlings self doctor` reports a version directory whose top level contains a
`<name>.lock` file. The downloader creates that file only in `runtimedir`, so its
presence in a payload is the sweep's fingerprint. `--fix` reinstalls that
version, which applies 2.3. mcpp adds no code: its registry is an xlings home
that mcpp drives through `XLINGS_HOME` (`src/xlings/xlings.cppm:1235`), and the
remedy is `XLINGS_HOME=<registry> xlings self doctor --fix`.

### 2.6 Criteria

- **Positive.** In a fresh home, install a package whose hook leaves its archive
  in `runtimedir` (for example `xim:gcc`), then a hookless archive package
  (`mcpp:plugins`). The plugins directory's entry set equals its archive's
  top-level entry set, and `runtimedir` still holds both archives.
- **Negative.** The same sequence on 2026.9.12.1 finds the gcc archive inside
  the plugins directory; the e2e is red there before it is green here.
- **Layout.** A single-directory archive installs as `<version>/<top>/…`, and a
  `*/mcpp.toml` pointer still matches exactly once.
- **Doctor.** A store seeded with a swept directory is reported, and after
  `--fix` it equals a fresh install.
- Linux, macOS and Windows (rename within one volume, and the copy fallback).

## 3. B — a POSIX redirect reaches cmd.exe

### 3.1 Mechanism

`vendored_xlings_version` runs `"<bin>" --version 2>/dev/null`
(`src/fallback/xlings_binary.cppm:159-160`) through
`mcpp::platform::process::capture`, which on Windows is `_popen`, that is
`cmd.exe /c` (`modules/platform/src/process.cppm:460-476`). cmd resolves
`/dev/null` to `\dev\null` on the current drive and cannot open it. It writes
the message to its own stderr, which `_popen` does not capture, and does not
run the program, so the probe returns an empty version *(code for the last two
effects)*.

The probe runs in `acquire_xlings_binary` (`xlings_binary.cppm:53-58`) during
configuration loading (`src/config.cppm:753`) whenever the vendored binary
already exists. That is every command that loads the configuration, except the
one that first copies the binary. `mcpp --version` loads no configuration,
which is what xlings#543 observed. The probe and its redirect arrived together
in `fdad165b` (2026.8.8.2, #378) and are in every release since.

### 3.2 Measured

Windows CI of 2026-09-14. In the windows-2022 fresh install of the released
2026.9.14.3, the command that copies the vendored xlings prints nothing, and
every later `mcpp run` and `mcpp new --template` prints the line. On `main`,
`ci-windows-e2e` printed it 26 and 24 times and `toolchains + regressions` 16
times. Every job was green, because no test asserts that the line is absent.

### 3.3 The silent twin

An empty version returns early (`xlings_binary.cppm:57`). On Windows the vendored
xlings is therefore never compared with `kXlingsPinnedVersion`, a pin change
never reaches an existing home, and `mcpp self doctor` can only report that it
cannot read the version (`src/doctor.cppm:495-499`). xlings#543 is the field
instance: the `subos_info` block was missing, `xlings self update` and a
reinstall of xlings did not help, and deleting `~/.mcpp` did. The check that
exists for exactly that case never ran *(code; §6, run 2)*.

### 3.4 Rule and change

> A program probe is an argument vector. Redirection and the null device belong
> to the platform layer.

Five command strings carry POSIX grammar on a path that Windows reaches. All
five move to the argument-vector launcher `capture_exec` (`process.cppm:671`;
`posix_spawn` on POSIX, and on Windows a quoted command line with `2>&1`,
`:741`).

| site | reached on Windows |
|---|---|
| `src/fallback/xlings_binary.cppm:160`, xlings version | every command after the first |
| `src/toolchain/post_install.cppm:853`, `-dumpspecs` for the clean link specs (`prepare.cppm:11833-11835`) | every prepared build with GCC |
| `src/toolchain/gcc.cppm:126`, libstdc++ fallback probe (single-quoted as well) | GCC whose `bits/std.cc` scan misses |
| `src/build/execute.cppm:814`, freestanding size report | freestanding targets |
| `src/pm/publisher.cppm:363`, `sha256sum` | `mcpp publish`, where the tool is also absent |

`src/build/hermetic.cppm:149` returns early off Linux (`:110`), and the patchelf
walk in `post_install.cppm:105-142` touches ELF files only. `xlings --version`
writes one line to stdout and nothing to stderr, so the merged capture leaves
the version parser unchanged.

### 3.5 Criteria

- **Negative**, windows-2022. In a fresh home, the second command's stderr does
  not contain `The system cannot find the path specified.` It does on
  2026.9.14.3 (3.2).
- **Positive.** `mcpp self doctor` prints `vendored xlings <v> (pinned <p>)` on
  Windows.
- **Twin.** With a vendored `xlings.exe` older than the pin and a newer system
  xlings, the next command prints `Updating vendored xlings <old> -> <new>`.
  Linux is the control leg.

## 4. Moving mcpp to the latest xlings

1. xlings: 2.3 to 2.6 in one PR, released as 2026.9.14.1 (X) and indexed.
2. mcpp: `kXlingsVersion = X` together with 3.4, in mcpp#639. The constant is the
   only edit; `check_version_pins.sh` names the other pin points.
3. Verification on the published mcpp: the CI matrix; a sandbox with a fresh
   home (2.6's positive leg inside mcpp's registry, `mcpp:plugins` after a
   toolchain install); and a home that carries a swept directory (2.5).

X also carries everything since 2026.9.5.1, including 2026.9.12.1's changes to
`remove`, `self doctor`, the index overlay and one-time notices. The matrix and
the sandbox are the gate for those; no mcpp change is expected from them.

## 5. C — the build database (#636)

### 5.1 Where each part belongs

| part | content | home | why not elsewhere |
|---|---|---|---|
| facts | translation units and their argument vectors; `provides`, `requires` and the declaration form from mcpp's scanner, `scan_overrides` included; toolchain identity; the std units mcpp compiles; the inputs whose change changes the plan; planning without writing into the project | engine | Only `prepare_build` computes them. A build program runs before the plan exists and sees its own package; a `dist-*` member runs under `mcpp pack` on link outputs. No plugin reaches the finished plan. |
| format | S1 at level 2, rendered from the facts, inside the docs/50 envelope (which is S2 §3.4) | engine, beside `compile_commands.json` | P2977/S1 is a build-system-neutral database, as the CDB is. A plugin would need the engine to export the same facts first (a second contract), a new dispatch point, and a project manifest that declares the plugin, which an editor cannot add to a user's project. |
| structured options | S1 level 3, `ide.options` | the S1 reference library, beside its schema | mcpp holds flags as strings (`src/build/flags.cppm:37-41`) and renders `arguments` by splitting them (`src/build/compile_commands.cppm:221-264`). Options derived inside the engine are a second parser of the same argv that must follow every flag the engine adds. S1 §9 rule 1 already assigns the step to whoever lacks `options`, and lsp-mcpp already parses the three dialects. |
| editor policy | watching and debouncing, time bounds, stale models, fallback to `--configure-only`, status text | consumer (S2 §5) | Behaviour of an editor session, not a property of the build. |

The answer to the review question: this is not a plugin. The engine keeps the
facts and a neutral format, and nothing in the engine names lsp-mcpp.

### 5.2 The contract is specifications

- The document conforms to S1 profile 0.2.0. The issue links the
  `feat/lsp-mcpp-v1` branch, no `spec-s1-v*` or `spec-s2-v*` tag exists, and the
  schema's `$id` names `github.com/mcpp-community/lsp-mcpp`, which does not
  exist. mcpp therefore cites the specification by commit, which is immutable.
- The schema at that commit is vendored into mcpp's tests, with the commit
  recorded beside it, and validates every document they produce. No mcpp test
  runs lsp-mcpp or reads its fixtures.
- mcpp's own obligations (5.3 to 5.6) are written as SPEC-005 in `docs/specs/`,
  with an implementation status per rule, as SPEC-003 does. docs/50 gains the
  kind and the command's effects.
- Once the command exists, `build.ninja`, the std cache's `std-module.json` and
  the text of `mcpp.toml` are declared not to be interfaces.

### 5.3 What the engine changes

`BuildOverrides::work_dir` already means "where mcpp writes": `target/`,
`mcpp.lock`, `compile_commands.json`, `.mcpp/` and the build programs' artifact
directory move together (`src/build/prepare.cppm:1012-1025`), and the host-tool
sub-build uses it in production (`:9317-9330`). The command runs `prepare_build`
with `work_dir = <MCPP_HOME>/cache/workdirs/<hash of the project root>` and
renders the plan in memory. `run_configure_plan`, which writes `build.ninja` and
the CDB and stages BMIs, is not called.

Five engine changes make this exact. Each removes a leak or a second
derivation, and each lands with its own criterion before the command does.

1. **Two writes ignore `work_dir`** and use the literal root: the mangling stage
   directory (`prepare.cppm:6965`) and the root package's `[build]
   generated_files` (`:4185`). The first moves to `work_dir`. The second is a
   source file by design, so the command compares instead of writing: identical
   content needs no write, and a missing or different file becomes a `warning`
   diagnostic that names it.
2. **`prepare_build` compiles the std module** (`:11383`). Its identity (cache
   paths and commands) becomes computable without compiling, and the command
   uses only the identity. This is the only compilation that planning performs
   for a project without build programs.
3. **The std build commands are shell strings** (`src/toolchain/gcc.cppm:182-204`,
   `cd <cache> && … 2>&1`), and the strings are part of the std cache identity
   (`src/toolchain/stdmod.cppm:317-330`). The builders are not changed: a
   changed string would invalidate every user's std BMIs on upgrade. The std
   units' `work-directory` and `arguments` are recovered from the first command
   that names the module source, by the inverse of mcpp's own rendering (the
   `cd` prefix, environment assignments and the trailing redirect removed, the
   quoting undone). Unit tests drive every builder and assert the round trip,
   so a builder that changes shape fails a test rather than the database.
4. **`emit_compile_commands` builds each entry inline.** The per-unit invocation
   (`directory`, `source`, `arguments`, `output`) becomes one record that both
   the CDB and the database render, so equal `arguments` hold by construction.
5. **The scanner discards the declaration form.** A `module M;` implementation
   unit is stored as `requires M` (`src/modgraph/scanner.cppm:941-952`), which
   cannot be told apart from a non-module unit that imports `M`. One enum on
   `SourceUnit` records the form that was read; a `scan_overrides` unit records
   that it is unknown.

`mcpp.lock`: the command copies the project's lock into `work_dir`, where
`prepare_build` reads it (`:2035-2037`), and compares the copy afterwards. A
difference is a `warning` diagnostic; the project's lock is never written.

Build programs still run with the package root as their working directory
(`src/build/build_program.cppm:1468`), and a dependency's host tool is still
built into the global tool store when a build program needs it. The no-write
guarantee covers mcpp's writes. A build program that writes outside
`MCPP_OUT_DIR` is outside the guarantee, as it is under `mcpp build`.

### 5.4 Mapping

| S1 | from | note |
|---|---|---|
| toolchain id | `<compiler_family>-<version>-<triple>`, the triple as the compiler spells it | opaque to a consumer; `llvm`, as in `llvm@22.1.8` |
| `family` | `Toolchain::compiler_name()` (`modules/toolchain-model/src/model.cppm:261-268`): `gcc`, `clang`, `msvc` | not `compiler_family()`, which answers `llvm` |
| `driver`, `version`, `target`, `sysroot` | `binaryPath`, `version`, `targetTriple`, `sysroot` | |
| `stdlib` | `name` and `version` from `stdlibId` and `stdlibVersion`; no `module-metadata` | std resolves through units (next row) |
| `config-files` | clang: the `<driver>.cfg` of `resolve_clang_driver`, unless the units pass `--no-default-config`; GCC: `lib/gcc/<targetTriple>/<version or major>/specs` beside the driver | read from the layout the driver searches; no driver is run |
| set `mcpp:std` | units for `stdModuleSource` and `stdCompatSource`, carrying the commands of 5.3 (3) | One rule for GCC's `bits/std.cc`, libc++'s `std.cppm`, MSVC's `std.ixx` and a package's own `std.cppm`; S1 §6.1 lets units outrank a manifest. `mcpp:` is the engine's reserved namespace (SPEC-002), so the name cannot collide with a package. |
| sets | one per package; test targets' sources in `<package>:test` | `family-name` is the package; `ide.configuration` is `BuildContext::profile`; `ide.kind` comes from the package's declared targets |
| `visible-sets` | every other set | The engine resolves imports over one flat graph per invocation (`scanner.cppm:1289-1319`). A narrower closure would describe a rule the build does not enforce; if the engine later refuses undeclared imports, the database inherits it. |
| units | every `CompileUnit` except NASM units, as in the CDB (`compile_commands.cppm:225-228`) | rendered from the record of 5.3 (4) |
| `baseline-arguments`, `local-arguments` | a unit's arguments are its driver, the set's baseline, its local arguments and its own `-c <source> -o <object>`; the baseline is the longest prefix every unit of the set shares | a prefix keeps argument order, which decides include search and macro definitions; a common subset would not |
| `private` | `false` | every module is visible to every set, as the `visible-sets` row states |
| `provides` | `providesModule` mapped to `""` | S1-8-6 permits an empty path for a producer that performs no build |
| `requires` | `imports` | partitions are already written in full |
| `ide.role` | the declaration form of 5.3 (5) | `unknown` for `scan_overrides` units |

A target's entry source that no `sources` glob matched (a discovered test, a
`main` outside the globs) had its imports read from line-leading `import` alone,
so an import in a comment or a raw string was planned as one. Validating the
lsp-mcpp repository's own database against S1 found it: its scanner test
required three modules no source provides. The entry is now read by the
scanner (`scan_entry_file`); a file the scanner refuses, which this path never
refused, keeps its line-leading imports with the role `unknown`. The standard
library check before planning reads entries the same way, so the two cannot
disagree about `import std`.

### 5.5 `watch` and `inputs-fingerprint`

- `watch` lists what the resolution read and a user edits: the manifests of the
  root, the members and the path dependencies (absolute when outside the
  workspace); `mcpp.lock`; every `build.mcpp` and the inputs it declared through
  `rerun_if_changed` and `rerun_if_changed_glob`; each package's source globs,
  as LSP patterns; and `$MCPP_HOME/config.toml`, which holds the default
  toolchain. Environment variables that steer resolution, such as
  `MCPP_TOOLCHAIN`, cannot be watched, and SPEC-005 names them.
- `inputs-fingerprint` is a digest of the contents that `watch` matches at the
  time of the run, the mcpp version and the selector. It is not the build
  fingerprint (`modules/toolchain-model/src/fingerprint.cppm`), which answers a
  different question and does not include `mcpp.lock` (`prepare.cppm:11360`).
- The command writes no file that `watch` names; this follows from 5.3.
- Reusing an earlier result while the fingerprint is unchanged is not part of
  the first version. If it is added, its criterion is the fast path's: A, then
  B, then A.

### 5.6 Output, selectors, failures and effects

- Without `--format`, the command prints the bare S1 document, as `mcpp emit
  xpkg` prints a bare descriptor; `-o <file>` writes that document atomically
  instead. `--format json` prints the envelope of docs/50 with the document in
  `data.database`.
- The selectors are those of `mcpp build` (`--target`, `--toolchain`,
  `--profile`, `-p`/`--package`, `--workspace`); the issue's `--member` is
  `--package`.
- `--workspace` yields one document whose sets are named `<member>/<package>`.
  Each member is planned separately (`src/cli/cmd_build.cppm`), so a dependency
  shared by two members appears once per member, with that member's arguments.
- A failure prints one envelope without `data`, whose `diagnostics` carry a
  stable `code`, and exits 1 (docs/50 §3). Logs go to stderr. A workspace in
  which one member fails to plan fails as a whole, and the diagnostic names the
  member.
- `--protocol-version` advertises `"mcpp.build-database": 1` and, for
  `emit build-database`, the effects `read-project`, `network`,
  `write-global-cache` and `exec-build-script`, never `write-project`. The
  envelope's `effects` lists what this run did.
- `--format ndjson` remains an error (exit 2) until the issue's second phase.

### 5.7 Criteria

1. Every document produced for `examples/`, for a project with `tests/` and a
   dev-dependency, and for a workspace validates against the vendored S1
   schema; every set has `ide.toolchain` and every unit has `ide.role`.
2. One plan rendered as CDB and as database gives equal `arguments` for every
   unit. The unit test is shown to fail when one renderer receives a different
   flag string.
3. The project tree's content hash is equal before and after the command. The
   same measurement around `--configure-only` differs, which shows that the
   measurement can see a write.
4. The `mcpp:std` units name `llvm-generated/std.cppm` with
   openkal-llvm-runtime, `bits/std.cc` with `gcc@16.1.0` and `std.ixx` with
   `msvc@system`; on the three CI hosts, each unit's `arguments` compile its
   source when run in its `work-directory`.
5. A project with a syntax error, and a project never built, both produce a
   document.
6. Nothing is compiled and no link input is written, on a cold std cache and on
   a warm one: the planning pass's work directory holds no object, BMI or
   `mcpp-clean-link.specs`, and a home whose build cache is empty gains no object
   or BMI, while `--configure-only` in the same home compiles the std module (the
   control). The driver still answers the queries toolchain resolution makes
   (`--version`, `-dumpmachine`, `-print-sysroot`), as it does for `mcpp build`,
   so a driver wrapper that fails whenever it is invoked cannot be the probe.
   The wall time on a warm cache is of the order of `--configure-only`.
7. `tests/unit/test_wire.cpp` pins the kind's key set; `--format ndjson` exits 2
   with empty stdout.

### 5.8 Not in mcpp

S1 level 3 options; S2 stream mode and the `ndjson` progress stream; editor-side
watching, time bounds and fallback; S3 and S4. The issue's follow-up asks for a
project-level default target, which exists as `[build] target`
(`docs/04-mcpp-toml.md:397`, `prepare.cppm:2535-2536`); whether it serves a
repository that needs `x86_64-windows-gnu` on Windows hosts only is a usage
question for lsp-mcpp.

## 6. Plan

### 6.1 Repositories and pull requests

| repository | pull request | content |
|---|---|---|
| openxlings/xlings | one PR, version 2026.9.14.1 | 2.3 to 2.6: the staging rule in xpkg spec §6, private extraction, the doctor check, tests |
| openxlings/xim-pkgindex | the `xlings` bump; later the bot's `mcpp` bump | index entries for the two releases |
| xlings-res (GitHub and GitCode) | none | release mirrors; the GitCode leg is completed locally with `gtc` where CI leaves it short |
| mcpp-community/mcpp | #639, renamed | B, the `kXlingsVersion` move, C (the five engine changes, the command, SPEC-005, docs/50 and its translation, tests), the release version |
| mcpp-community/mcpp-index | none expected | verification only: packages with `*/…` pointers install and build under the new xlings |
| lsp-mcpp | none | its consumer already reads a level 2 document and completes it (`src/project/mcpp.cpp`, `enrich_database`); S1 is cited by commit and its schema is vendored |

### 6.2 Tasks and dependencies

| id | task | depends on |
|---|---|---|
| X1 | xlings: private extraction and the staging rule; strip branch removed | none |
| X2 | xlings: spec sentence in xpkg-manifest-v1 §6 | none |
| X3 | xlings: doctor reports a swept payload; `--fix` reinstalls it | X1 |
| X4 | xlings: e2e for the rule (positive, negative, layout) and the doctor | X1, X3 |
| X5 | xlings: PR, CI, merge, release 2026.9.14.1, mirrors, index | X1 to X4 |
| M1 | mcpp: the five probes of 3.4 through `capture_exec` | none |
| M2 | mcpp: e2e for B (no message on the second command, the doctor reads the version, an older vendored xlings is replaced) | M1 |
| M3 | mcpp: the five engine changes of 5.3 | none |
| M4 | mcpp: `emit build-database` (renderer, `watch`, fingerprint, lock comparison, effects, kind) | M3 |
| M5 | mcpp: unit, contract and e2e tests for C, with the vendored S1 schema and a validator that refuses keywords it does not implement | M4 |
| M6 | mcpp: SPEC-005, docs/50 and docs/01 with their translations, the three specification indexes | M4 |
| M7 | mcpp: `kXlingsVersion = 2026.9.14.1` | X5 |
| M8 | mcpp: #639 renamed; CI green; self-review; merge | M1 to M7 |
| R1 | mcpp release; mirrors (GitCode completed with local `gtc`); index merge | M8 |
| V1 | sandbox (`xlings subos use <name> --sandbox --cmd`, CN mirror): a fresh home installs the released mcpp, builds, emits a database, and its registry holds no swept payload | R1 |
| V2 | sandbox: mcpp-index packages with `*/…` pointers, a package-provided `std`, and `xlings self doctor` on a swept store | R1, X5 |
| V3 | reply on #636; records | V1, V2 |

M1 to M6 proceed in parallel with X1 to X5. M7 is the only mcpp task that waits
on another repository, and it is small.

### 6.3 Runs that settle the *(code)* statements

1. windows-2022: the message of 3.1 is absent after M1 (M2's negative leg).
2. Every host: an older vendored xlings is replaced after M1 (M2's twin leg;
   the Linux leg is the control).
3. xlings: the concurrent download of 2.2 is not measured; 2.4 removes the
   sweep, which removes the question.

## 7. Decisions

1. **Level 2 from mcpp; level 3 from whoever lacks `options`.** S1 §9 rule 1
   makes `options` authoritative when present, so an incomplete `options` object
   is worse than none, and the engine holds no complete structured form.
2. **`visible-sets` states the flat graph the engine enforces.**
3. **Sets per package**, plus `<package>:test` for the sources of test targets
   and `mcpp:std`; in a workspace every name is prefixed with `<member>/`.
4. **`provides` paths are empty**, because the command performs no build.
5. **`work_dir` lives in the global cache**; a root `generated_files` entry that
   is missing or stale is a warning, never a write.
6. **SPEC-005** is a new specification in `docs/specs/`.
7. **The command is `mcpp emit build-database`**, beside `emit xpkg`.
8. **The std cache identity does not change** (5.3, item 3).
9. **S1 is cited by commit.** The schema is vendored with the commit it was taken
   from; tagging the specification is its owner's step and blocks nothing here.

## 8. Self-review

| angle | finding | effect on the plan |
|---|---|---|
| architecture | The engine gains one command and five general changes; no consumer name appears in mcpp; xlings owns its store and its repair | none |
| stability | Rewriting the std builders as data would change the strings that name the std cache, and every home would rebuild std on upgrade | 5.3 item 3 recovers argv from the strings instead |
| stability | A private extraction directory shared by two installs of one package would collide | the directory name carries the process id; removal on every exit path |
| simplicity | Level 3 would need a second parser of every flag the engine emits | level 2 (7.1) |
| user experience | An editor needs the document without a file; a script may want a file | bare document by default, `-o` for a file, `--format json` for the envelope (5.6) |
| compatibility | The CDB must not change; `--protocol-version` gains keys only; new stores keep the un-stripped layout the 55 pointers depend on | a golden test for the CDB bytes; 2.3 keeps the top-level directory |
| cross-platform | B exists only on Windows, so its criteria run on windows-2022; C's paths use the CDB's spelling on every host | M2 includes a Windows leg; units reuse the CDB record |
| consistency | One envelope builder, one selector parser, one unit record, one std command source | none |
| seamless upgrade | A pin move reaches an existing Windows home only through B's fix; swept stores keep working and are repaired on request | B and M7 ship in one pull request; `self doctor --fix` |
| test coverage | Each criterion has a leg that fails on the released binary; the schema validator refuses keywords it does not implement, so a schema change cannot pass unvalidated | M2 and M5 |

## 9. Execution record

### 9.1 What landed, by repository

| repository | vehicle | content |
|---|---|---|
| openxlings/xlings | #596, squash `3cd8061`, released 2026.9.14.1 | 2.3 to 2.6: the §6 sentence, private extraction, the strip branch removed, `self doctor` finding `SweptPayload` with remove-then-reinstall, e2e for the rule and the doctor |
| openxlings/xim-pkgindex | #841 (release bot, +22/-3), squash `fbef674` | `latest` of xlings is 2026.9.14.1 in the three platform tables |
| mcpp-community/mcpp | #639, squash `87d4ff05`, released 2026.9.15.1 | B, C, the pin of §4, and the findings of 9.2; the merged tree equals the tree CI tested (40 checks passed, one conditional job skipped); the ten workflows on `main` at that commit passed |
| openxlings/xim-pkgindex | #842 (release bot, +22/-3), squash `ffde527` | `latest` of mcpp is 2026.9.15.1 in the three platform tables |

### 9.2 Found during implementation

1. **A digest string that GCC accepted and clang refused.** The selector digest
   joined values with `"\x1f"` followed by a letter (`"\x1ffeatures"`), which
   is one out-of-range hex escape. GCC truncated it; clang on macOS and Windows
   refused the file. The separator is now a newline.
2. **The four S1 SHOULD fields.** lsp-mcpp's validator (`specs/tools/validate.py`,
   `s1_semantics`) requires `config-files`, `baseline-arguments`,
   `local-arguments` and `private`, which the first rendering omitted. Each is
   now derived from the plan (5.4).
3. **An entry source read by a second parser.** Validating lsp-mcpp's own
   database found a scanner test planned as importing `also.fake`, `in.raw` and
   `real`, which are text inside its raw string literal. The entry of a target
   that no `sources` glob matched was read by line-leading `import`, in two
   places. Both now call `scan_entry_file` (5.4). A test source placed in
   `src/` did not reproduce it, which is why the fixtures had never shown it.
4. **A precondition met by one runner's configuration.** e2e 687 inherited
   the developer configuration, and the macOS runner's configuration names its
   `~/.xlings` shim as the xlings binary: nothing was vendored and the test
   stopped before either criterion. It now plans in a home with its own
   configuration.
5. **A link input written by a plan that links nothing.** The independent review
   of #639 found `prepare_build` running `g++ -dumpspecs` to write
   `mcpp-clean-link.specs` under `plan_only`. Criterion 6 as first written
   (a driver wrapper that fails when invoked) could not have caught it: toolchain
   resolution queries the driver on every plan. The call is now skipped under
   `plan_only`, the link specs being read only by the link line, and e2e 688
   criterion L measures the work directory and a cold home instead.
6. **The swept-payload fingerprint.** The first xlings rendering flagged any
   top-level file with a download extension or a `.meta` name, so a package
   shipping `setup.exe` would be reinstalled on every `--fix`. The fingerprint
   is now a zero-length `<name>.lock` whose `<name>` is a sibling, has a download
   extension, or has a `<name>.meta` sibling.

### 9.3 Measurements

| run | reading |
|---|---|
| lsp-mcpp `s1_semantics` and `mcpp_contract`, schema, argument decomposition | 0 failures on a GCC 16 project, an LLVM 22 project, and the lsp-mcpp repository (11 sets, `std` from openkal-llvm-runtime 0.9.6) |
| lsp-mcpp conformance runner with this mcpp in place of `lsp-mcpp-mock-mcpp` | `mcpp-emit` 12/12, `mcpp-emit-package-std` 13/13; a watch scenario on the real manifest 10/10 (an edit reloads; a broken manifest leaves the model stale with `MCPP_BUILD_DATABASE_PLAN_FAILED`; the repair makes it fresh) |
| `mcpp emit build-database` on the lsp-mcpp repository | 0.84 s; `git status` unchanged |
| e2e 687 on windows-2022 (39053b9a) | A and B pass: the second command prints no path error, and an older vendored xlings is replaced |
| e2e subset touching `mcpp test` and entry mains, Linux | 60 pass, 6 skipped for capability, 0 fail |
| e2e 687 and 688 in CI at `2f5b71c4` | 687 passes on Linux, macOS and Windows; 688 prints its J, K, L and L-control lines on the three hosts |
| xlings 2026.9.14.1 publication | GitHub: 8 assets, each archive's sha256 equal to its sidecar. GitCode: the Windows archive exceeded the runner's cross-border upload and was completed with `tools/mirror-latest.sh xlings`, 16 of 16 verified. `tools/verify-release.sh 2026.9.14.1` passes, including a byte comparison of the CN copies |
| `xlings self update` on this host | 2026.9.12.1 to 2026.9.14.1 through the index |
| mcpp 2026.9.15.1 publication | The tag points at `87d4ff05`. Each platform archive was uploaded to GitCode with the local `gtc` as soon as its build job attached it to the GitHub release, so `publish-ecosystem` found all eight GitCode assets present and skipped them. The four GitCode archives match the GitHub sidecars byte for byte |
| `xlings install mcpp@2026.9.15.1` on this host, CN mirror | the store payload reports `mcpp 2026.9.15.1` and carries `xlings 2026.9.14.1` |
| `2026-09-14-636-verify.sh` in `xlings subos use verify-636 --sandbox`, CN mirror, the published mcpp 2026.9.15.1 | 18 checks pass, 0 fail; section E (Windows) is not run in a Linux sandbox and is covered by e2e 687 in CI. A: version, CN mirror, xlings pin and vendored xlings 2026.9.14.1. B: a toolchain install followed by the hookless `mcpplibs.cmdline` leaves no download sidecar in any store payload, the payload's top level is the archive's own directory, and `mcpp run` prints. C and C2: the build databases of a module project and of section B's project are S1 documents with the stated sets, no store path in `watch`, and unchanged trees. D: a seeded archive with its lock is reported by name and `--fix` reinstalls the payload |
| `XLINGS_HOME=~/.mcpp/registry xlings self doctor` (2026.9.14.1, read-only) on this host | 223 swept-payload findings, against 221 version directories with download sidecars measured on 2026-09-14 (2.2) |

The first sandbox run failed two checks and read a third finding, all from the verification script:
it looked for `*/mcpp.toml` in `mcpplibs.cmdline` 0.0.1, whose index entry describes that version inline
with `*/src/**/*.cppm` globs because 0.0.1 ships no manifest; and it called the registry's xlings directly
while the sandbox shell exported `XLINGS_ACTIVE_SUBOS`, which mcpp's own calls drop, so `self doctor` tried
to write a manifest for that subos inside the registry. The second run asks what the entry uses and drops the
variable.

### 9.4 Observed and not addressed

- xlings `xlings-ci-fresh-install` fails its "core" legs on `main`: after a
  multi-version switch of mcpp, `self doctor` reports `shim table 3 missing
  (elfpatch, mcpp, patchelf)`. The run on `59068d6` (2026.9.12.1) fails
  identically, so the failure predates #596.
