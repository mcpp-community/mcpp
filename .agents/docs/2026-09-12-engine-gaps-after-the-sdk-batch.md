---
subject: triage
status: active
---

# The engine gaps left open after the SDK batch

**Status:** design, for review before implementation. Nothing described here is
implemented. The implementation is one mcpp PR carrying the next date version.
2026.9.12.1 (#617) is merged and deliberately unreleased, so its change ships
with that PR.

## 0. Scope, and the ledger it starts from

The scope is issues #564 to #618 in mcpp-community/mcpp, plus the gaps the
2026-09-11 record left open. Each issue was checked against the code on `main`
at `c688fcab`, not against the PR that claims to fix it.

| item | before | finding on `main` | action |
|---|---|---|---|
| #564 `default_jobs` unread | open | `prepare.cppm` reads `defaultJobs` into `globalDefaultJobs`; `default_backend` is removed | closed, citing #607 |
| #597 WebAssembly target | open | `ObjectFormat::Wasm`; the `wasm32-emscripten` row is `verified` | closed, citing #605, #610, #617 |
| #599 bench hub path | open | the hub is the pinned tree's own path; the uninitialised branch fails under CI | closed, citing #607 |
| #603 clang on Windows, level 23 | open | the clang path calls `std_module_min_level_for_stl` | closed, citing #607 |
| #604 MSVC `/reference` pair | open | flags are appended verbatim; `orphaned_reference` refuses early | closed, citing #607 |
| #606 scanner inside comments | open | one three-state pass; `tests/e2e/639` | closed, citing #607 |
| #609 MSVC STL 14.51 `_Find_vectorized` | open | upstream microsoft/STL#6294; nothing in mcpp is wrong | left open; §6 |
| #611 personal notes | open | a to-do list spanning three repositories | left open; its one engine item is #613 |
| #613 install hooks and the standard library | open | unfixed | §2 |
| #614 `XLINGS_PROJECT_DIR` asymmetry | open | unfixed | §3 |
| #615 runtime files that are not DLLs | open | unfixed | §4 |
| #618 Windows GUI subsystem | open | unfixed | §1 |
| G1 no per-target tool declaration | recorded 2026-09-11 | **misdiagnosed**: the axis exists | §5.1 |
| G2 no whole-graph channel for `-pthread` | recorded 2026-09-11 | confirmed | §5.2 |
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
   (`ld: unrecognized option '--subsystem'`, `prepare.cppm:9412`). So one
   intent needs one `cfg` block per ABI, and a project that forgets one gets a
   console on that ABI with no diagnostic.
2. **The pair is a pair on one CRT only.** On the MSVC CRT,
   `/SUBSYSTEM:WINDOWS` changes the default entry to `WinMainCRTStartup`, so a
   portable `int main()` fails with `LNK2019: unresolved external symbol
   WinMain` unless `/ENTRY:mainCRTStartup` accompanies it; `/ENTRY:main` links
   and skips CRT initialisation. mingw-w64's startup code is different, and
   copying the MSVC entry override there is not correct by construction.
   Whether `-mwindows` alone reaches `main` on mingw-w64 is a criterion to
   measure (§1.6), not an assumption this design rests on. Either way the
   correct flags are a function of the subsystem and the CRT, which is what a
   project should not have to compute.
3. **The scope is wrong, and no flag-carrying key has the right scope.**
   `[build] ldflags` and `[target.<selector>.build] ldflags` land in the global
   `$ldflags` of `build.ninja`, so every `mcpp test` binary becomes a GUI
   program whose output no terminal shows, and they propagate to consumers
   (docs/30: "`[build] ldflags` already propagates to consumers").
   `mcpp:link-flag` reaches consumers by design. `[targets.<name>]` has no
   link-side key. The subsystem is a property of one executable, and nothing
   that carries raw flags today is scoped to one executable.
4. **The engine cannot read a flag's meaning.** With a field, mcpp knows the
   artefact is a GUI program: `mcpp run` can state that the program has no
   console instead of appearing to print nothing, `mcpp test` can refuse the key
   on a test target, and a packager can treat the artefact as an application. A
   string in `ldflags` is opaque to all three.

A per-target `ldflags` key would fix reason 3 and none of the others. It may be
worth adding later as an escape hatch for options that are genuinely
linker-specific; it does not replace the field.

### 1.2 The field, and its name

```toml
[targets.myapp]
kind              = "bin"
main              = "src/main.cpp"
windows_subsystem = "gui"      # "console" (default) | "gui"
```

The candidates were judged by one test: the name should say what it does,
including where it does nothing.

| spelling | reads as | cost |
|---|---|---|
| `subsystem = "windows"` (#618's proposal) | "the subsystem is windows" | in a cross-platform manifest it reads as "target Windows", and `subsystem` names no platform, so a reader cannot tell it is inert on Linux |
| `windows_subsystem = "windows"` (Rust's attribute; Meson's `win_subsystem`) | "the Windows subsystem is windows" | the platform is named; the value repeats a PE header constant, clear to someone who knows the header and opaque otherwise |
| `windows_subsystem = "gui"` | "the Windows subsystem is GUI" | the key names the one platform it affects, and the value names the observable behaviour: whether the program gets a console |

Recommended: `windows_subsystem = "console" | "gui"`. One spelling, with no
alias for `"windows"`, for the reason the `ndk` toolchain alias was withdrawn in
2026.9.11.4: a second spelling that parses is a second thing to keep correct.

**The entry point, as a separate and deferrable key.** The default keeps a
portable `int main()`. A program that writes another entry names the function it
wrote, not the CRT symbol that calls it:

```toml
windows_entry = "wWinMain"     # "main" (default) | "wmain" | "WinMain" | "wWinMain"
```

The engine maps the function to the startup symbol per CRT
(`mainCRTStartup`, `wmainCRTStartup`, `WinMainCRTStartup`, `wWinMainCRTStartup`
on the MSVC CRT; the wide forms on mingw-w64 need `-municode`). It is
independent of `windows_subsystem`, because a console program may use `wmain`.
Nothing in #618 needs it, so it may be deferred.

### 1.3 Rendering

| target | `"gui"` renders | decided by |
|---|---|---|
| PE, MSVC style: cl, clang-cl, clang targeting `*-windows-msvc` | `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup`, spelled with `-Wl,` under a GNU-style driver | `plan.rcStyle == "msvc"` |
| PE, GNU style: MinGW gcc, clang targeting `*-windows-gnu` | `-mwindows`, which both drivers accept for MinGW targets (measured in §1.6) | `plan.rcStyle == "gnu"` |
| ELF, Mach-O, Wasm | nothing; no diagnostic; byte-identical artefact | `ObjectFormat` |

`"console"` renders nothing on every target. It is the linker's default, and
writing it explicitly would change only the command line of every existing
Windows build.

The dialect is read from `rcStyle`, the field the plan already uses to choose
between rc/llvm-rc and windres (`plan.cppm:262`), rather than derived a second
time. Resources and the subsystem are the two halves of the "Windows
application" story the 2026-08-07 record set side by side (§A6), and they must
agree about which linker they are addressing.

### 1.4 Scope

- Appended to `LinkUnit::linkFlags` of that target's `Binary` unit only
  (`plan.cppm:103`, rendered per edge as `$unit_ldflags`). No new plumbing.
- Refused, naming the target and the key, on `lib` and shared-library targets
  and on test targets. A GUI subsystem on a test binary is the defect #618
  describes.
- It never reaches consumers or another target of the package, because nothing
  it writes is in `[build]`.
- It changes the link command of one unit, which ninja already treats as a
  reason to relink; no other unit's output depends on it.
- `kKnownTargetKeys` gains the key. The warning that lists per-target keys is
  corrected in the same change: it already omits `exports`, a second copy of the
  list that has drifted from the first.

### 1.5 The build-program form

`mcpp::target_windows_subsystem("myapp", "gui")` is a directive that names a
target of the package being built. It lets a framework's rule package select the
subsystem for the application it knows about (HuxerUI's installer rule already
receives the bin target), and it cannot leak, because it names a target in this
package. This is phase two, after the manifest key exists.

### 1.6 Criteria

1. `windows_subsystem = "gui"` with `int main()` links on `x86_64-windows-msvc`
   (clang) and on `x86_64-windows-gnu` (gcc and clang). The PE optional
   header's Subsystem field reads 2, `IMAGE_SUBSYSTEM_WINDOWS_GUI`, read from
   the bytes.
2. In the same package, `mcpp test` binaries and a second `bin` target read 3,
   `IMAGE_SUBSYSTEM_WINDOWS_CUI`. A consumer of a library in the graph is
   byte-identical to a build without the key.
3. A static constructor in the GUI target runs before `main` on both ABIs. This
   is the CRT-initialisation criterion.
4. The same manifest on Linux and macOS produces no diagnostic and a
   byte-identical artefact.
5. On mingw-w64, whether `-mwindows` alone reaches `main` is measured under
   criterion 1. If it does not, the GNU row gains the entry it needs.
6. The key is refused on a `lib` target and on a test target, each refusal
   naming the target and the key.

### 1.7 What this does not do

It does not produce an application bundle, embed an application manifest, or
choose DPI awareness. Those belong to packaging formats and to `[resources]`.

## 2. #613 — an install hook cannot see the consumer's standard library

### 2.1 What the code does

Build programs receive the resolved toolchain as environment variables:
`MCPP_COMPILER`, `MCPP_CXX_STDLIB`, `MCPP_TARGET`, the `MCPP_TARGET_*` splits and
the `MCPP_TOOLCHAIN_*` paths (`build_program.cppm`, around line 540). Install
hooks receive none of them. `install_packages` runs as
`cd <home> && env -u XLINGS_PROJECT_DIR XLINGS_HOME=<home> xlings interface
install_packages …` (`xlings.cppm:1435`). The probe in mcpplibs/mcpp-index#392
logged `MCPP_CXX_STDLIB=nil` from inside a hook.

The ordering does not block a fix. In `prepare_build`, the provisioning calls
(around lines 4239 and 5022) come after toolchain resolution (lines 2929 to
3835), so the values exist when a hook runs.

### 2.2 The trap in the obvious fix

Exporting `MCPP_CXX_STDLIB` and letting a hook *adapt* would be wrong. The
payload store is keyed by package and version (`xim-x-<pkg>/<version>`), not by
ABI. The first consumer to install a source-built static package would decide
its standard library for every later consumer on the machine. A libc++ project
would then receive a libstdc++ archive built earlier for another project, and the
link error would return, dependent on install order.

### 2.3 Recommendation, in two parts

1. **Expose the values, for refusal and diagnosis.** The install command carries
   the subset build programs already get, under the same names and the same rule
   ("always emitted, empty when not applicable"): `MCPP_COMPILER`,
   `MCPP_CXX_STDLIB`, `MCPP_TARGET`, `MCPP_TARGET_OS`, `MCPP_TARGET_ARCH`,
   `MCPP_TARGET_ENV`. They are empty while a toolchain payload itself installs,
   because no compiler exists yet. A hook may refuse with a message naming the
   standard library; it must not build a variant into a store directory that
   does not name the variant.
2. **Declare the requirement, and refuse at resolution.** A package's
   mcpp-facing exports gain `abi = { cxx_stdlib = "libstdc++" }`. mcpp compares
   it with the resolved toolchain before installing, and refuses naming the
   package, its requirement and the toolchain. The failure becomes a diagnostic
   at resolve time instead of `undefined symbol: std::__cxx11::…` at link. This
   is #613's option 3, and it needs no hook at all.

A store keyed by variant (`xim-x-<pkg>/<version>+libc++`) would let one machine
hold both flavours. It is an xlings store change rather than an engine change,
and is recorded as the direction rather than designed here.

### 2.4 Criteria

- A hook that prints `MCPP_CXX_STDLIB` prints `libc++` on a clang/libc++
  toolchain and `libstdc++` on gcc, and prints an empty value while the
  toolchain payload itself installs.
- A package declaring `abi.cxx_stdlib = "libstdc++"` is refused on the libc++
  leg before any download, and the refusal names both sides.
- The same package installs and links on the gcc leg as it does today, and the
  install command's other arguments are unchanged.

## 3. #614 — two meanings of "global mode", and an error that stops at the boundary

### 3.1 The asymmetry

`build_command_prefix` (`xlings.cppm`, from line 1136) and the `self init` call
(around line 1590) express global mode as `env -u XLINGS_PROJECT_DIR` on POSIX
and as `env::set("XLINGS_PROJECT_DIR", "")` on Windows. Absent and empty are
different answers to "which scope is this", and xlings resolves its subos scope
from that variable. The Windows branch also mutates mcpp's own process
environment, so the value outlives the invocation that needed it.

### 3.2 Fix

- Use the scoped guard `modules/platform/src/env.cppm` already provides
  ("Temporarily set or unset an env var, restoring the prior value on scope
  exit"). Global mode is then *unset* on every host, and the prior value is
  restored when the command returns.
- One function decides the environment of an xlings invocation (home, project
  directory or its absence, PATH prefix), and each platform renders that
  decision. The three copies of the decision, at lines 1141, 1435 and 1592,
  become its callers.

### 3.3 The diagnostic half

mcpp prints `xlings reported: <childError>` from the NDJSON error event
(`package_fetcher.cppm:396`). xlings' own `[xim]` error lines, which carried the
exact rejection in the vulkan-loader case, never reach the user. When
`install_packages` exits non-zero, mcpp appends xlings' error-level stderr lines
to the diagnostic, bounded to the last 20, each prefixed so it reads as xlings'
words. No new flag is added; the existing `MCPP_VERBOSE=1` advice stays for
everything else.

### 3.4 Criteria

- A unit test of the environment function: global mode produces "unset" on both
  platforms, project mode produces the path, and the parent process's
  environment is unchanged afterwards.
- A failing hook's `[xim]` error line appears in mcpp's error output. The fixture
  is a local package whose config hook raises.
- Windows CI runs the fixture, because Windows is where the asymmetry lived.

## 4. #615 — deploying runtime files that are not DLLs, into subdirectories

### 4.1 What exists

`runtime.deploy_files` is already an explicit, platform-neutral list. It is
readable from `[runtime]` in a manifest (`toml.cppm:2030`) and from a package's
exports (`xpkg.cppm:2080`). Each entry becomes `DeployFile{source, dest}` with
`dest = bin/<filename>` (`plan.cppm:1182-1197`). Two readers consume it: the
collision check (`flags.cppm:1267`) and the copy edges
(`ninja_backend.cppm:653`). Separately, `runtime_search_dirs` discovers `*.dll`
files and nothing else.

So #615 does not need a new mechanism. What is missing is a destination that can
contain a directory.

### 4.2 Contract

An entry is either the existing string, meaning `bin/<filename>`, or a table:

```lua
runtime = {
    deploy_files = {
        "bin/vulkan-1.dll",                                                -- unchanged
        { from = "lib/libMoltenVK.dylib",                 to = "." },
        { from = "share/vulkan/icd.d/MoltenVK_icd.json",  to = "vulkan/icd.d" },
    },
},
```

- `to` is a directory relative to the executable's directory. It obeys the
  string rules the payload descriptor applies to `frontend`: `/`-separated, not
  absolute, no drive, no `.` or `..` component. Anything else is refused by name.
- `dest` becomes `bin/<to>/<filename>`. Both readers key on the full relative
  destination, so two files with the same name in different directories no
  longer collide, while two sources for one destination still do.
- Explicit entries are honoured on every object format. DLL discovery through
  `runtime_search_dirs` stays DLL-only: that is a PE loader rule, and extending
  it to `*.dylib` would copy libraries a Mach-O executable already reaches
  through its RPATH.
- The manifest's `[runtime] deploy_files` accepts the same table.
- `mcpp pack` carries the files at the same relative paths
  (`prepare.cppm:12069`). Packing a Mach-O program stays refused for its
  existing, unrelated reason.

### 4.3 Criteria

- #615's measured layout: a package deploys `libMoltenVK.dylib` to `.` and the
  ICD manifest to `vulkan/icd.d`. On macos-15, `vkCreateInstance` returns 0 and
  enumerates one device, both under `mcpp run` and from a copy of `bin/` in
  another directory (#615's case B3).
- A plan built from string entries only is byte-identical to today's.
- `to = "../x"`, `to = "/x"` and a backslash are each refused, naming the package
  and the entry.

## 5. The two gaps recorded by the SDK batch

### 5.1 G1 was misdiagnosed: the per-target tool declaration exists

The 2026-09-11 record and `examples/13-platform-targets/mcpp.toml` state that a
tool cannot be declared per target, citing
`error: [target.aarch64-ios-sim.xlings] does not accept 'deps'`. The refusal is
real; the conclusion drawn from it is not. `[target.<selector>.xlings.workspace]`
is accepted, and its entries are folded into the same install list `deps` feeds
(`toml.cppm`, around line 2890). It is covered by
`tests/unit/test_target_xlings_axis.cpp` and `tests/e2e/625`.

Measured 2026-09-12 on linux-x86_64 with mcpp 2026.9.12.1:

```toml
[target.aarch64-ios-sim]
runner = ["simctl-run"]

[target.aarch64-ios-sim.xlings.workspace]
"xim:apple-simulator-tools" = ""
```

`mcpp build` for the host exits 0 and never mentions the macOS-only package.
`mcpp build --target aarch64-ios-sim` is refused at the SDK gate, as it should be.

What is missing is therefore not an axis but three statements that point to it:

1. The refusal gains a second sentence: a tool for this target is declared under
   `[target.<selector>.xlings.workspace]`. The error that sent the last batch the
   wrong way should have sent it the right way.
2. `examples/13-platform-targets` declares `simctl-run`'s package beside the row
   that uses it, and its README drops the manual `xlings install` step. The iOS
   CI fixture does the same.
3. The 2026-09-11 record is not edited. This record states the correction.

### 5.2 G2: a switch that must reach every translation unit in the link

**What fails.** `openkal.task` on Emscripten needs `-pthread`, which selects a
different C library build, memory model and loader contract. Measured
2026-09-11 with the feature on and `-pthread` on the consumer:

```
error: POSIX thread support was disabled in precompiled file
       '.../pcm.cache/openkal.types.pcm' but is currently enabled
```

The specification package's module was compiled without the switch, and nothing
a consumer writes can reach that compile. The root's `[build]` flags do not reach
dependency translation units, by design (`cache_key.cppm`, line 17); a feature
contributes sources, defines and per-glob flags to its own package; and
`[build] ldflags` reaches the link only.

**Why not a general channel.** A key for "flags for the whole graph" would let
one package change how every other package compiles, which is the property every
per-package scope in the engine exists to prevent. It would also put arbitrary
strings into every dependency's cache key. This flag is not arbitrary: it is an
ABI switch with a closed set of values.

**Proposal: a typed per-target key.**

```toml
[target.wasm32-emscripten]
threads = true
```

- It lives in `TargetEntry`, beside `linkage` and `cxx_runtime`, the keys that
  already decide something for the whole artefact.
- The engine renders it into every compile unit of every package, into the std
  module's own precompile and codegen commands, and into the link. That is the
  distribution `target_implied_flags` already has (`cache_key.cppm:85`, "the
  flags the TRIPLE implies"), so it enters each dependency's cache key through
  the group that already carries target-shaped flags.
- Its meaning is per object format. On Wasm it renders `-pthread`. On ELF it is
  already the default and renders nothing. On PE and Mach-O it renders nothing.
  A value that means nothing for a format is accepted without a diagnostic, as
  `windows_subsystem` is on Linux.
- A package can require it. A feature may declare
  `requires_target = { threads = true }`, so `openkal-emscripten`'s `threads`
  feature is refused at resolution with a message naming the key, instead of
  failing inside a precompiled module.

**Criteria.** With `threads = true`, the `threads` feature of openkal-emscripten
links, and a program calling `kal_task_start` runs under node. Without the key,
the feature is refused at resolution, naming it. A dependency's cache key differs
between the two builds. A host build is byte-identical.

## 6. #609 — a known toolchain hazard, stated where readers look

Nothing in mcpp is wrong: microsoft/STL#6294 is open upstream. mcpp is still the
tool that assembles clang with the MSVC STL, so `docs/20-toolchains.md` and its
Chinese copy gain a second "Known Toolchain Hazard" section beside the existing
one. It states the error text, the affected STL release (14.51), the upstream
issue, and the two workarounds measured downstream: an explicit `operator==` on
the element type, or an older runner image. There is no engine change and no
detection heuristic; matching compile output by substring is a shape this
repository has recorded as unreliable. #609 is then closed as documented
upstream tracking.

## 7. Delivery

One mcpp PR with the next date version, which also publishes #617's change.
Commits follow risk, lowest first: §5.1 (text and an example), §6 (docs), §3,
§4, §1, §2, §5.2.

| repository | change | depends on |
|---|---|---|
| mcpp | §1 to §6 | nothing |
| mcpp-index | `compat.mysql-connector-cpp` declares `abi.cxx_stdlib`; its llvm-leg exclusion comes out | the engine release |
| openkal-emscripten | the `threads` feature requires `threads = true` | the engine release |
| xim-pkgindex | a package adopting §4's table form, such as a MoltenVK payload | the engine release |

After the release, the sandbox verification runs its 27 checks, expected to all
hold, plus one new check each for §4 and §5.2. §1 is verified on the Windows CI
runner, because the sandbox is Linux.

## 8. Questions for review

1. §1.2: `windows_subsystem = "console" | "gui"`, or the value `"windows"` that
   Rust and Meson use?
2. §1.2: ship `windows_entry` in the same PR, or defer it?
3. §2.3: is a resolve-time refusal (`abi.cxx_stdlib`) enough for the ecosystem
   today, with a variant-keyed store left to xlings?
4. §5.2: `threads` as a single typed key, or a typed `[target.<selector>.abi]`
   table that can grow other members?
