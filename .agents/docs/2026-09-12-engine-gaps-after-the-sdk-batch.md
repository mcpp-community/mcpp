---
subject: triage
status: landed
---

# The engine gaps left open after the SDK batch

**Status:** landed in mcpp 2026.9.12.2. The four questions in the first draft
were answered in review (§9), and three statements were measured and corrected
before implementation (§2, §5.2, §7). The corrections made during
implementation are recorded in §11. The ecosystem changes E1 and E2 follow the
release (§10).

## 0. Scope, and the ledger it starts from

The scope is issues #564 to #618 in mcpp-community/mcpp, the gaps the
2026-09-11 record left open, and one defect found while measuring them. Each
issue was checked against the code on `main` at `c688fcab`, not against the PR
that claims to fix it.

| item | before | finding on `main` | action |
|---|---|---|---|
| #564 `default_jobs` unread | open | `prepare.cppm` reads `defaultJobs` into `globalDefaultJobs`; `default_backend` is removed | closed, citing #607 |
| #597 WebAssembly target | open | `ObjectFormat::Wasm`; the `wasm32-emscripten` row is `verified` | closed, citing #605, #610, #617 |
| #599 bench hub path | open | the hub is the pinned tree's own path; the uninitialised branch fails under CI | closed, citing #607 |
| #603 clang on Windows, level 23 | open | the clang path calls `std_module_min_level_for_stl` | closed, citing #607 |
| #604 MSVC `/reference` pair | open | flags are appended verbatim; `orphaned_reference` refuses early | closed, citing #607 |
| #606 scanner inside comments | open | one three-state pass; `tests/e2e/639` | closed, citing #607 |
| #609 MSVC STL 14.51 `_Find_vectorized` | open | upstream microsoft/STL#6294; nothing in mcpp is wrong | §6, then closed |
| #611 personal notes | open | a to-do list spanning three repositories | left open; its one engine item is #613 |
| #613 install hooks and the standard library | open | the refusal already exists; the hook environment does not | §2 |
| #614 `XLINGS_PROJECT_DIR` asymmetry | open | unfixed | §3 |
| #615 runtime files that are not DLLs | open | unfixed | §4 |
| #618 Windows GUI subsystem | open | unfixed | §1 |
| G1 no per-target tool declaration | recorded 2026-09-11 | **misdiagnosed**: the declaration exists | §5.1 |
| G2 no whole-graph channel for `-pthread` | recorded 2026-09-11 | **misdiagnosed in part**: the channel exists; scoping and a requirement do not | §5.2 |
| T1 `--toolchain` replayed by the fast path | found 2026-09-12 | a defect | §7 |
| openkal-musl never built for Darwin | recorded 2026-09-11 | package-side | not engine; belongs to openkal-musl |
| no device runner for `aarch64-ios` | recorded 2026-09-11 | needs a developer signature | not engine; out of scope |

## 1. #618 — a Windows GUI executable, declared on its target

### 1.1 Are `ldflags` equivalent to a field?

No. Four independent reasons, any one of which would be enough.

1. **The spelling belongs to one linker dialect, and a PE target has two.** The
   flags in #365's report, `-Wl,-subsystem:windows -Wl,-entry:mainCRTStartup`,
   are link.exe and lld-link syntax passed through a clang driver. The same
   program built for `x86_64-windows-gnu` links with GNU ld or lld in MinGW
   mode, which take `--subsystem windows`; GCC's own spelling is `-mwindows`.
   The engine has already recorded a GNU linker rejecting a subsystem option
   (`ld: unrecognized option '--subsystem'`, `prepare.cppm:9412`). One intent
   therefore needs one `cfg` block per ABI, and a project that forgets one gets
   a console on that ABI with no diagnostic.
2. **The pair is a pair on one CRT only.** On the MSVC CRT,
   `/SUBSYSTEM:WINDOWS` changes the default entry to `WinMainCRTStartup`, so a
   portable `int main()` fails with `LNK2019: unresolved external symbol
   WinMain` unless `/ENTRY:mainCRTStartup` accompanies it; `/ENTRY:main` links
   and skips CRT initialisation. mingw-w64's startup code is different, and
   copying the MSVC entry override there is not correct by construction. The
   correct flags are a function of the subsystem and the CRT, which a project
   should not have to compute.
3. **The scope is wrong, and no flag-carrying key has the right scope.**
   `[build] ldflags` and `[target.<selector>.build] ldflags` land in the global
   `$ldflags` of `build.ninja`, so every `mcpp test` binary becomes a GUI
   program whose output no terminal shows, and they propagate to consumers
   (docs/30: "`[build] ldflags` already propagates to consumers").
   `mcpp:link-flag` reaches consumers by design. `[targets.<name>]` has no
   link-side key.
4. **The engine cannot read a flag's meaning.** With a field, mcpp knows the
   artefact is a GUI program: `mcpp run` can state that the program has no
   console, `mcpp test` can refuse the key on a test target, and a packager can
   treat the artefact as an application.

A per-target `ldflags` key would fix reason 3 and none of the others. It is not
part of this change.

### 1.2 The fields

```toml
[targets.myapp]
kind              = "bin"
main              = "src/main.cpp"
windows_subsystem = "windows"   # "console" (default) | "windows"
windows_entry     = "main"      # "main" (default) | "wmain" | "WinMain" | "wWinMain"
```

**Naming, as decided in review.** `windows_subsystem` names the one platform it
affects, so a reader on another platform can tell it is inert there. The value is
`"windows"`, the PE subsystem's own name and the value Rust's
`#![windows_subsystem]` and Meson's `win_subsystem` use, so a developer arriving
from either reads it without translation. One spelling, with no alias.

**The entry point.** `windows_entry` names the function the program defines, not
the CRT symbol that calls it. It is independent of the subsystem, because a
console program may define `wmain`.

| `windows_entry` | MSVC CRT startup | mingw-w64 |
|---|---|---|
| `main` | `mainCRTStartup` | default |
| `wmain` | `wmainCRTStartup` | `-municode` |
| `WinMain` | `WinMainCRTStartup` | default |
| `wWinMain` | `wWinMainCRTStartup` | `-municode` |

### 1.3 Rendering

| target | `windows_subsystem = "windows"` renders | decided by |
|---|---|---|
| PE, MSVC style: cl, clang-cl, clang targeting `*-windows-msvc` | `/SUBSYSTEM:WINDOWS` plus the entry's `/ENTRY:` symbol, spelled with `-Wl,` under a GNU-style driver | `pe_msvc_abi` (§11.1) |
| PE, GNU style: MinGW gcc, clang targeting `*-windows-gnu` | `-mwindows`, plus `-municode` for a wide entry | `pe_msvc_abi` (§11.1) |
| ELF, Mach-O, Wasm | nothing; no diagnostic; byte-identical artefact | `ObjectFormat` |

`"console"` with `"main"` renders nothing on every target, because both are the
linker's defaults. On the MSVC ABI any other combination renders both flags
(§11.2). The ABI is answered by `pe_msvc_abi`, the predicate the import library
flag already uses, and the emitter spells the flag for the linker it invokes
(§11.1).

### 1.4 Scope

- Appended to `LinkUnit::linkFlags` of that target's `Binary` unit only
  (`plan.cppm:103`, rendered per edge as `$unit_ldflags`).
- Refused, naming the target and the key, on library targets and on test
  targets.
- It never reaches consumers or another target of the package.
- `kKnownTargetKeys` gains both keys. The warning that lists per-target keys is
  generated from the same list, because the hand-written copy already omits
  `exports`.
- A build program selects the subsystem for a target it names, through a
  directive, so a framework's rule package can mark the application it knows
  about. The directive names a target of the package being built and therefore
  cannot leak into consumers.

### 1.5 Criteria

1. `windows_subsystem = "windows"` with `int main()` links on
   `x86_64-windows-msvc` and on `x86_64-windows-gnu`. The PE optional header's
   Subsystem field reads 2 (`IMAGE_SUBSYSTEM_WINDOWS_GUI`), read from the bytes.
2. In the same package, the `mcpp test` binaries and a second `bin` target read
   3 (`IMAGE_SUBSYSTEM_WINDOWS_CUI`).
3. A static constructor in the GUI target runs before `main` on both ABIs.
4. The same manifest on Linux produces no diagnostic and an artefact
   byte-identical to one built without the keys.
5. The keys are refused on a library target and on a test target, each refusal
   naming the target and the key; an unknown value is refused naming the
   accepted values.
6. Rendering is unit-tested for every row of the tables in §1.2 and §1.3.

### 1.6 What this does not do

It does not produce an application bundle, embed an application manifest, or
choose DPI awareness. Those belong to packaging formats and to `[resources]`.

## 2. #613 — an install hook cannot see the consumer's standard library

### 2.1 What the code does

Build programs receive the resolved toolchain as environment variables:
`MCPP_COMPILER`, `MCPP_CXX_STDLIB`, `MCPP_TARGET`, the `MCPP_TARGET_*` splits and
the `MCPP_TOOLCHAIN_*` paths (`build_program.cppm`, around line 540). The value
of `MCPP_CXX_STDLIB` is the toolchain's `stdlibId`: `libstdc++` for gcc,
`libc++` for clang, `msvc-stl` for clang targeting MSVC. Install hooks receive
none of these. `install_packages` runs as
`cd <home> && env -u XLINGS_PROJECT_DIR XLINGS_HOME=<home> xlings interface
install_packages …` (`xlings.cppm:1435`).

### 2.2 The refusal already exists

The first draft proposed a new `abi = { cxx_stdlib = … }` declaration. It is not
needed. The layer grammar already states the requirement, and the engine already
refuses it at resolution. Measured 2026-09-12 with a path dependency declaring
`requires = ["mcpp:c++-abi=libstdc++"]` and a project on `llvm@22.1.8`:

```
error: `stdreq@0.1.0` requires the c++-abi to be `libstdc++`.
         c++-abi           libc++         (payload)
         required          libstdc++      (required by stdreq@0.1.0)
```

With the default gcc toolchain the same project resolves
`c++-abi libstdc++ (payload)` and builds.

### 2.3 What remains

1. **Order.** The requirement must be refused before an index package's install
   hook runs, not after a source build has already spent several minutes. This
   is measured first; if the check follows provisioning, it moves ahead of it.
2. **The hook's environment.** The install command carries the subset build
   programs already receive, under the same names and the same rule ("always
   emitted, empty when not applicable"): `MCPP_COMPILER`, `MCPP_CXX_STDLIB`,
   `MCPP_TARGET`, `MCPP_TARGET_OS`, `MCPP_TARGET_ARCH`, `MCPP_TARGET_ENV`. They
   are empty while a toolchain payload itself installs.
3. **The rule for a hook.** A hook may use these values to refuse or to
   diagnose. It must not build a variant into a store directory that does not
   name the variant, because the store is keyed by package and version, and the
   first consumer would otherwise decide the flavour for every later one. A
   store keyed by variant is an xlings change and is out of scope.
4. **Documentation.** docs/22 and docs/06 state the recipe for a source-built
   static package: declare `requires = ["mcpp:c++-abi=<stdlib>"]`.

### 2.4 Criteria

- A requirement mismatch on an index package with an install hook is refused
  before the hook runs; the refusal names the layer, both implementations and
  the package.
- A hook that prints `MCPP_CXX_STDLIB` prints the resolved `stdlibId`, and an
  empty value while a toolchain payload installs. The environment composition is
  unit-tested on both platforms.

## 3. #614 — two meanings of "global mode", and an error that stops at the boundary

### 3.1 The asymmetry

`build_command_prefix` (`xlings.cppm`, from line 1136) and the `self init` call
(around line 1590) express global mode as `env -u XLINGS_PROJECT_DIR` on POSIX
and as `env::set("XLINGS_PROJECT_DIR", "")` on Windows. Absent and empty are
different answers to "which scope is this", and xlings resolves its subos scope
from that variable. The Windows branch also mutates mcpp's own process
environment, so the value outlives the invocation that needed it.

### 3.2 Fix

- One function decides the environment of an xlings invocation: home, project
  directory or its absence, PATH prefix, and the hook variables of §2.3. Each
  platform renders that decision. The three copies of the decision become its
  callers.
- On Windows the decision is applied through the scoped guard
  `modules/platform/src/env.cppm` already provides, so global mode is unset and
  the prior value is restored when the command returns.

### 3.3 The diagnostic half

mcpp prints `xlings reported: <childError>` from the NDJSON error event
(`package_fetcher.cppm:396`). xlings' own `[xim]` error lines never reach the
user. When `install_packages` exits non-zero, mcpp appends xlings' error-level
lines to the diagnostic, bounded to the last 20, each prefixed so it reads as
xlings' words.

### 3.4 Criteria

- A unit test of the environment function: global mode produces "unset" on both
  platforms, project mode produces the path, and the process environment is
  unchanged afterwards.
- A failing install's `[xim]` error line appears in mcpp's error output.

## 4. #615 — deploying runtime files that are not DLLs, into subdirectories

### 4.1 What exists

`runtime.deploy_files` is an explicit, platform-neutral list of strings, readable
from `[runtime]` in a manifest (`toml.cppm:2030`) and from a package's exports
(`xpkg.cppm:2080`). Each entry becomes `DeployFile{source, dest}` with
`dest = bin/<filename>` (`plan.cppm:1182-1197`). Two readers consume it: the
collision check (`flags.cppm:1267`) and the copy edges
(`ninja_backend.cppm:653`).

### 4.2 Why a new key, not a new form of the old one

An older mcpp reading `deploy_files` with a table entry does not report an error.
Its reader calls `read_string()`, which returns an empty string without
advancing when the next token is `{`, and the loop around it never terminates.
A published descriptor that extended `deploy_files` would hang every older
client that resolved it. The `runtime` table, by contrast, skips sub-keys it
does not know. The table form therefore takes a new sub-key, `deploy`.

### 4.3 Contract

```lua
runtime = {
    deploy_files = { "bin/vulkan-1.dll" },                      -- unchanged
    deploy = {
        { from = "lib/libMoltenVK.dylib",                 to = "." },
        { from = "share/vulkan/icd.d/MoltenVK_icd.json",  to = "vulkan/icd.d" },
    },
},
```

- `from` is relative to the package root; `to` is a directory relative to the
  executable's directory. Both obey the string rules the payload descriptor
  applies to `frontend`: `/`-separated, not absolute, no drive, no `.` or `..`
  component, except that `to = "."` names the executable's directory itself.
  Anything else is refused by name.
- `dest` becomes `bin/<to>/<filename>`. Both readers key on the full relative
  destination, so two files with the same name in different directories do not
  collide, while two sources for one destination still do.
- Honoured on every object format. DLL discovery through `runtime_search_dirs`
  stays DLL-only, a PE loader rule.
- The manifest's `[runtime] deploy` accepts the same table.
- `mcpp pack` carries the files at the same relative paths.

### 4.4 Criteria

- A dependency deploying a file to `.` and another to a nested directory: after
  `mcpp build`, both exist at `bin/` and `bin/<to>/`; after `mcpp test`, the test
  binaries see the same layout.
- A plan built from `deploy_files` strings only is byte-identical to today's.
- `to = "../x"`, `to = "/x"`, a backslash, and a missing `from` are each
  refused, naming the package and the entry.
- An older mcpp resolves a descriptor carrying `runtime.deploy` without error,
  which is the reason for the new key.

## 5. The two gaps recorded by the SDK batch

### 5.1 G1 was misdiagnosed: the per-target tool declaration exists

The 2026-09-11 record and `examples/13-platform-targets/mcpp.toml` state that a
tool cannot be declared per target, citing
`error: [target.aarch64-ios-sim.xlings] does not accept 'deps'`. The refusal is
real; the conclusion drawn from it is not. `[target.<selector>.xlings.workspace]`
is accepted, and its entries are folded into the same install list `deps` feeds
(`toml.cppm`, around line 2890). It is covered by
`tests/unit/test_target_xlings_axis.cpp` and `tests/e2e/625`.

Measured 2026-09-12 on linux-x86_64 with mcpp 2026.9.12.1: with
`"xim:apple-simulator-tools" = ""` under `[target.aarch64-ios-sim.xlings.workspace]`,
a host `mcpp build` exits 0 and never mentions the macOS-only package, and
`mcpp build --target aarch64-ios-sim` is refused at the SDK gate.

What is missing is three statements that point to it:

1. The refusal gains a second sentence naming
   `[target.<selector>.xlings.workspace]`.
2. `examples/13-platform-targets` declares `simctl-run`'s package beside the row
   that uses it, and its README drops the manual `xlings install` step. The iOS
   CI fixture does the same.
3. This record states the correction; the 2026-09-11 record is not edited.

### 5.2 G2 was misdiagnosed in part: the channel exists; scoping and a requirement do not

**What was recorded.** openkal-emscripten's README states that mcpp has no
channel for a flag that applies to a whole dependency graph, citing:

```
error: POSIX thread support was disabled in precompiled file
       '.../pcm.cache/openkal.types.pcm' but is currently enabled
```

**What was measured, 2026-09-12.** The channel is `[build] dialect_cxxflags`,
which docs/04 describes as applied "to the std BMI prebuild, the module scan and
every translation unit in the graph, including dependencies", and which enters
each dependency's cache key (`cache_key.cppm`, `dialect_flags`). A program that
references `kal_task_start`, with the `threads` feature on:

| root manifest | result |
|---|---|
| `dialect_cxxflags = ["-pthread"]`, `ldflags = ["-pthread"]` | links; 16 `-pthread` in `build.ninja`, including the global `cxxflags` line; the generated JavaScript mentions `SharedArrayBuffer` and `PThread` |
| `cxxflags = ["-pthread"]`, `ldflags = ["-pthread"]` | the recorded error, verbatim, on `openkal.task.pcm` |

The recorded failure put the flag in the per-package channel.

**What remains.**

1. **Scoping.** `dialect_cxxflags` is not a conditional key:
   `[target.<selector>.build]` accepts only build inputs, so a portable manifest
   cannot limit `-pthread` to the Web target.
2. **Portability.** `-pthread` is a GNU-driver spelling. A manifest that builds
   the same program with cl has no correct value to write.
3. **A requirement.** openkal-emscripten's `threads` feature cannot state that it
   needs the switch, so a consumer who forgets it gets a precompiled-module
   mismatch instead of a sentence.

**Decision: a typed `abi` table, first member `threads`.** Reason 2 is the same
reason §1 prefers a field to `ldflags`, and reason 3 is only possible with a
typed value. The table is built now so that a later graph-wide ABI switch has a
place that is not another free-form flag list.

```toml
[target.'cfg(os = "emscripten")'.abi]
threads = true
```

- A sub-table of `[target.<selector>]`, so it takes a triple or a `cfg`
  predicate and is evaluated against the resolved target, like `.build`.
- Rendered through the existing graph-global dialect channel: `-pthread` joins
  `plan.dialectFlags`, and therefore the std module prebuild, the scan, every
  translation unit and every dependency's cache key, and `-pthread` joins the
  link. That is for GNU-style drivers (gcc, clang, em++). MSVC-style drivers
  render nothing, since the MSVC runtime is always multithreaded.
- Only `threads` is accepted. An unknown member is refused naming the accepted
  set, so the table cannot become a flag list.
- A feature or a package states the requirement with
  `requires_abi = { threads = true }`. When the resolved target's `abi` does not
  satisfy it, resolution refuses, naming the package, the feature, the member and
  the manifest line that would satisfy it. An older mcpp reports the unknown key
  and skips it, so the key can be published.
- `dialect_cxxflags` stays the raw channel for flags that have no typed member.
  It does not become conditional in this change: the typed member covers the
  measured case, and a conditional raw flag would bring back reason 2.

**Criteria.**

- With `threads = true` under the Web target's `abi` table, the `threads` feature
  of openkal-emscripten links, and a program that starts a task runs under node.
- Without it, a feature declaring `requires_abi = { threads = true }` is refused
  at resolution, naming the key.
- A dependency's cache key differs between the two builds, and a host build of
  the same manifest is byte-identical to one without the table.
- An unknown member, and a non-boolean `threads`, are refused by name.

## 6. #609 — a known toolchain hazard, stated where readers look

Nothing in mcpp is wrong: microsoft/STL#6294 is open upstream. mcpp is still the
tool that assembles clang with the MSVC STL, so `docs/20-toolchains.md` and its
Chinese copy gain a second "Known Toolchain Hazard" section beside the existing
one. It states the error text, the affected STL release (14.51), the upstream
issue, and the two workarounds measured downstream: an explicit `operator==` on
the element type, or an older runner image. There is no engine change and no
detection heuristic. #609 is closed as documented upstream tracking.

## 7. T1 — the fast path replays a build the command line asked to replace

**Measured 2026-09-12.** In a project with no dependencies:

| step | command | result |
|---|---|---|
| 1 | `mcpp build` | `Resolved gcc@16.1.0`, builds into `target/x86_64-linux-gnu/9dde3d1f4b99cc99` |
| 2 | `mcpp build --toolchain llvm@22.1.8` | `Finished dev in 0.00s`; nothing resolved; the same directory; the gcc artefact is left in place |
| 3 | the same command in a fresh copy | `Resolved llvm@22.1.8`, builds into `1e0091d91feafd6c`, and the artefact's `.comment` names clang 22.1.8 |

Step 2 is a build that reports success with the wrong compiler. It also skips
every resolution-time check, including the §2.2 refusal. That is how this defect
was found: the §2.2 measurement first read "exit 0" under llvm, because the llvm
build never happened.

**Fix.** The fast path is taken only when the inputs that choose the toolchain
are the ones the recorded build used. The command-line override is such an
input, beside the manifest's `[toolchain]` and the global default, so a
different override declines the fast path and resolution runs. Every other
command-line input that changes resolution is checked in the same pass, and the
rule is stated once, as a named set.

**Criteria.** An end-to-end test of A-B-A: build with the default, build with
`--toolchain llvm@22.1.8`, build with the default again. After each step the
artefact's own `.comment` section names the expected compiler, and step 2
resolves rather than replays. The test is run once with the fix removed, and
must fail there.

## 8. Self-review

Each angle states what the design does, and what would be wrong with the
alternative.

- **Architecture.** Every change lands on a mechanism that already exists:
  `LinkUnit` and the PE link-flag predicate (§1), the layer requirement check (§2), the
  `runtime` table's skipping of unknown sub-keys (§4), the xlings workspace
  selector (§5.1), the graph-global dialect channel and the dependency cache key
  (§5.2), and the fast path's recorded inputs (§7). No new channel carries raw
  flags.
- **Stability.** T1 removes a silent wrong-compiler build. Every new key refuses
  malformed input by name rather than ignoring it.
- **Simplicity.** Two gaps recorded as missing engine features are closed with
  statements and examples (§5.1) or with an existing requirement grammar (§2.2).
  New keys exist only where a raw flag cannot express the intent: the subsystem
  and entry (§1), and the thread ABI (§5.2).
- **User experience.** A failure that surfaced as a link error, a
  precompiled-module mismatch or a silently wrong artefact becomes a sentence
  naming the key that fixes it. Names follow the convention of the ecosystems
  users arrive from (§1.2).
- **Compatibility.** No existing key changes meaning. New descriptor keys are
  placed where older parsers skip rather than hang: `runtime.deploy` (§4.2),
  `requires_abi` as an unknown feature key (§5.2). `console`/`main` render
  nothing, so no existing Windows command line changes.
- **Cross-platform.** Rendering is decided by the object format and the PE link-flag predicate; a key
  that means nothing for a format is inert and byte-identical there. The Windows
  criteria run on Windows CI, not on Wine.
- **Consistency.** One vocabulary per concept: `mcpp:c++-abi` is the standard
  library requirement, `windows_*` keys are Windows-only, `abi` holds graph-wide
  ABI switches, and every path rule is the descriptor's `frontend` rule.
- **Seamless upgrade.** A manifest that builds today builds identically after the
  change. The one behavioural change a user can observe is T1: a
  `--toolchain` build that used to replay now builds with the toolchain it
  names.
- **Test coverage.** Each section lists criteria, including the inert case on
  other platforms and a refusal for each malformed shape. Each new unit test is
  run once with its fix removed. The sandbox verification gains checks for §4,
  §5.1, §5.2 and §7 on published artefacts.

**Rejected in review.** A per-target `ldflags` key (§1.1); a new
`abi.cxx_stdlib` declaration, superseded by the existing layer requirement
(§2.2); extending `deploy_files` with tables, which would hang older clients
(§4.2); a general whole-graph flag list (§5.2); making `dialect_cxxflags`
conditional (§5.2).

## 9. Decisions recorded from review

1. The subsystem value is `"windows"`, as in Rust and Meson.
2. `windows_entry` ships in the same change.
3. For #613, a resolve-time refusal is sufficient; a variant-keyed store is left
   to xlings. The refusal turned out to exist already (§2.2).
4. The graph-wide switch is an `abi` table rather than a single key, built now.

## 10. Task list and dependencies

```
repo                id   task                                                     depends on
------------------  ---  -------------------------------------------------------  ------------
mcpp                M1   §5.1 refusal text, examples/13, iOS CI fixture            -
mcpp                M2   §6 docs/20 + zh hazard section                            -
mcpp                M3   §7 fast-path inputs; A-B-A e2e                            -
mcpp                M4   §3 xlings environment function; error surfacing           -
mcpp                M5   §2.3 check order; hook environment; docs                  M4
mcpp                M6   §4 runtime.deploy: parsers, plan, readers, pack, tests    -
mcpp                M7   §1 windows_subsystem/windows_entry: parse, render,        -
                         scope, directive, unit tests, Windows e2e, docs/04
mcpp                M8   §5.2 abi table: parse, render, cache key, requires_abi,   -
                         unit tests, wasm e2e, docs/20, docs/06
mcpp                M9   CHANGELOG (the unreleased 2026.9.12.1 entry folds into    M1-M8
                         the new version), version, record status, index
mcpp                M10  CI green, self-review, release, mirrors, index bump,      M9
                         sandbox verification, bootstrap pin
openkal-emscripten  E1   README correction (§5.2); `threads` feature declares      M10
                         requires_abi; CI engine pin; a task program runs
mcpp-index          E2   compat.mysql-connector-cpp declares                        M10 (and the
                         `requires = ["mcpp:c++-abi=libstdc++"]`                    index floor)
```

M1 to M4, M6, M7 and M8 are independent and are implemented in parallel. The
three ecosystem changes follow the release, because each adopts a key only the
new engine reads, and E2's descriptor must be checked against the index's
minimum engine version before it is published.

## 11. Corrections made during implementation

Each item states what the sections above said, what was measured or read in the
code, and what was built instead.

1. **§1.3, the discriminator.** The ABI is not read from `plan.rcStyle`.
   `LinkUnit` carries the declared words, and the emitter renders them, because
   only the emitter knows whether the link is a separate linker invocation, which
   decides between `/SUBSYSTEM:` and `-Wl,/SUBSYSTEM:`. The ABI is answered by
   `pe_msvc_abi`, extracted from `pe_link_flag`, so the import library and the
   subsystem cannot address two different linkers.
2. **§1.3, the MSVC row.** Both `/SUBSYSTEM:` and `/ENTRY:<entry>CRTStartup` are
   written whenever either key differs from its default. Without `/SUBSYSTEM:`,
   link.exe infers the subsystem from the entry function the objects define, so
   `WinMain` with the console subsystem would link as a GUI program; without
   `/ENTRY:`, the GUI subsystem selects `WinMainCRTStartup`, which a portable
   `int main()` does not satisfy.
3. **§2.3 item 1, the order.** The layer requirement check runs in target-side
   resolution, after the dependency graph is installed, and it is not moved ahead
   of provisioning. A package's manifest may live inside its payload, so the
   complete set of requirements is known only after installation. The hook
   environment is what lets an install hook refuse before it compiles.
4. **§2.3 item 2, the values.** `MCPP_TARGET` in a hook follows the build-program
   rule: the requested triple, or the host triple for a native build. The six
   values are computed by `install_hook_env`, from which the build-program
   environment also takes them, in their existing order, so no build program's
   re-run key changes. The toolchain values are empty while a dependency
   installs, which §2.3 assumed otherwise: `tc` is resolved after the dependency
   graph, because a package in the graph may supply a target-side layer, so no
   compiler or standard library has been decided when a dependency's hook runs.
   Measured with tests/e2e/648, whose hook compiled in an empty `compiler=` and
   `stdlib=` beside the host's `os=linux`. Resolving the toolchain before
   installation is the reorder the engine deliberately does not make, and a
   guessed value would let a hook build the wrong variant, so the variables are
   emitted empty, which also keeps a value inherited from a parent process out
   of the hook. Neither §2.4 criterion holds as written: the refusal follows the
   hook (item 3), and a hook cannot print a resolved `stdlibId`. What holds is
   that the `c++-abi` refusal names both implementations before compilation, and
   that the hook sees the build's target and never an inherited toolchain value.
5. **§3.1, absent and empty on Windows.** The CRT defines `_putenv_s(key, "")` as
   removal, so the Windows branch already produced an absent
   `XLINGS_PROJECT_DIR`, and the two platforms did not disagree about global
   mode. What was wrong on Windows was the lifetime: the value stayed in mcpp's
   environment after the invocation. The asymmetry that did exist was on POSIX,
   where the `install_packages` fallback spelled global mode by hand whatever the
   project directory was.
6. **§3.2, the scope of the guard.** Only `XLINGS_PROJECT_DIR` is scoped.
   `XLINGS_HOME` and the PATH prefix are left process-wide on Windows, as before;
   scoping them has not been measured on Windows and is not part of this change.
   The hook variables of §2.3 are applied by the dependency installer's own
   scope rather than by the xlings environment function.
7. **§4.3, the TOML form.** mcpp's TOML layer refuses an array of tables in any
   section not on an allowlist, and `runtime.deploy` had to join it. The unit
   test written for the key reported this before an end-to-end test or a user
   could.
8. **§5.2, rendering.** Rendering is decided by the object format rather than by
   the driver: `-pthread` on every target that is neither PE nor freestanding,
   nothing on PE and nothing on a freestanding target. The switch reaches the
   root's `dialect_cxxflags`, `cflags` and `ldflags` and every dependency's
   `cflags`. The MinGW driver would accept `-pthread`; it is not rendered there,
   and threads on that ABI are outside this change's criteria.
9. **§7, the named set.** A recorded build is replayed only for the same target
   triple, profile, cache mode, requested features and toolchain request. The
   toolchain request is the command-line override (`--toolchain`,
   `MCPP_TOOLCHAIN`) together with the machine default (`[toolchain] default`).
   `--offline`, `--locked` and `--jobs` change how a resolution is fetched,
   checked or executed, not what it chooses, and are not compared. An entry
   written before the `toolchain=` line declines once.
10. **§7, the criterion.** A CI runner has one toolchain family installed, so the
    A-B-A test requests the platform's own toolchain through `--toolchain` and
    asserts that resolution runs, which the fast path skips. The machine-default
    leg switches to a second installed version of the same family and asserts
    the version string in the artefact; it reports itself as not measured where
    no second version is installed.
