---
subject: targets
status: active
---

# A UI framework on Android, iOS and Web: where each item of #622 lands, and the three it does not list

**Status:** implemented in mcpp (engine items A1 to A7, A10, A11; the version
is assigned at release); ecosystem changes follow. Every "measured" statement
below was checked against `main` at `85df514a` (mcpp 2026.9.12.2),
`mcpp-plugins` at `9064107` (0.6.0) and `openxlings/xim-pkgindex` at `571f845`,
not against the issue's own citations, and reflects the state at that commit
rather than the engine that landed afterward.

## 0. Scope, and the ledger it starts from

[#622](https://github.com/mcpp-community/mcpp/issues/622) is HuxerUI's list of
what a C++ program with a native host needs from mcpp on the three rows the
2026.9.11.x releases verified: `wasm32-emscripten`, `*-linux-android`,
`aarch64-ios-sim`. It sorts twenty items into three bins by repository. This
record re-measures each item, decides a shape for it, and adds the items the
issue does not list. It is the design the sub-issues are cut from.

The three records it continues, and does not restate:

- [Where a platform's knowledge belongs](2026-09-11-platform-targets-design-review.md)
  — the three questions (what is the target: engine; which compiler serves it:
  engine; what file does the user ship: plugin) and the corollary that decides
  every item below: **per project is a manifest key, per target is engine, per
  artifact shape is a plugin.** Also the host-dependency rule (a kernel
  facility, a proprietary runtime, or a credential; everything that is a
  program is packaged) and the runner-program pattern (a name is cache-safe, a
  path is not).
- [The category the plugin taxonomy does not name](2026-09-11-distribution-plugins-and-platform-decomposition.md)
  — `dist-*`, the staged tree, "declare unconditionally, submit conditionally",
  and the two-pass pack. All landed in 2026.9.11.1.
- [The engine gaps left open after the SDK batch](2026-09-12-engine-gaps-after-the-sdk-batch.md)
  — the `abi` table, built "so that a later graph-wide ABI switch has a place
  that is not another free-form flag list", and `runtime.deploy`. Both landed
  in 2026.9.12.2 and both are the mechanisms A1 and A4 extend.

| item | the issue says | finding on `main` | verdict | home |
|---|---|---|---|---|
| A1 | no target-conditional dialect flag | `kKnownConditionalBuildKeys` has ten keys and `dialect_cxxflags` is not one (`toml.cppm:2873`); `[target.<sel>.abi]` accepts exactly `threads` (`toml.cppm:2752`) | confirmed; the shape the issue proposes is the one the `abi` table was built for | engine, §2.1 |
| A2 | `frameworks` is top-level only | `kKnownCondRuntimeKeys = {libraries, link_library_dirs}` (`toml.cppm:2787`); rendered Mach-O only (`flags.cppm:394`) | confirmed | engine, §2.2 |
| A3 | no cfg gate on `[targets.<name>]` | `kKnownTargetKeys` is ten keys; `required_features` is the only gate (`prepare.cppm:9848`); `kind` is a closed set of three (`toml.cppm:1110`) | confirmed as a problem; **the gate is not the shape**. The fact is the platform's, so it is a fourth `kind`, `app` (§2.3); and `mcpp pack` must treat it as the program | engine, §2.3 |
| A4 | no directive deploys a generated file | the directive table has 25 rows and no `deploy` (`directives.cppm:248-387`); the manifest key's `from` is package-root-relative and refuses an absolute path | confirmed | engine, §2.4 |
| A5 | the wasm artifact contract is unwritten | sharper than asked: `artifact_naming` has rows for windows, macos and linux and falls back to the **host's** naming for everything else (`triple.cppm:1099-1129`); no engine code checks for a `main` symbol; docs/10 and docs/12 do not mention wasm | confirmed; the industry answer and mcpp's own semantics coincide on `<name>.js` (§2.5) | engine + docs, §2.5 |
| A6 | can `requires_abi` be target-conditional | it is one unconditional boolean per package or feature, checked once against the root's target-resolved value (`prepare.cppm:8909`) | confirmed | engine, §2.6 |
| A7 | `platforms` vocabulary for the three rows | the validator is `linux \| macos \| windows` (`prepare.cppm:2256`); the value is written into no descriptor and read by `doctor` and the pack coverage check only | confirmed, small | engine, §2.7 |
| A8 | a reexported dependency's `[xlings.workspace]` should reach the consumer | **misread**: a host module's declaration already reaches every build program it is compiled into (`tests/e2e/622`, since 2026.9.6.6). HuxerUI declared `xim:wix` on the *library*, not on its rule package | no engine change; one line moves in HuxerUI; docs/31 gains a sentence | framework + docs, §2.8 |
| A9 | one artifact from several targets | `make_plan` takes one binary and one triple (`pack.cppm:188`) | confirmed; recorded with a shape, not attempted | §2.9 |
| A10 | — | an application on Android is a `.apk` and on iOS an `.app`; `mcpp run` hands the runner the link output and nothing else | **not listed**: the run operand of an application | engine, §2.10 |
| A11 | — | `dist/apple.cppm:140` states that mcpp does not expose the deployment target to a build program; the engine computes it as `min_platform_version` | **not listed**: B1 cannot write `MinimumOSVersion` without it | engine, §2.11 |
| B1 | `dist-apple` returns for iOS | `apple.cppm:367`, `os != "macos"` | confirmed | plugins, §3.1 |
| B2, B3 | `dist-apk`, `dist-web` do not exist | correct | new members | plugins, §3.2, §3.3 |
| B5 | `dist-wix` and `xim:wix` disagree | README says WiX is not redistributable; `pkgs/w/wix.lua` ships 5.0.2 under MS-RL; `dist/wix.cppm:66-98` itself argues the payload is legitimate | the README is the wrong one | plugins, §3.5 |
| C1, C2 | no build-tools, no platform jar | `grep` of the index finds neither | confirmed | index, §4.1 |
| C3 | `adb-run` does not exist | `android-platform-tools.lua` registers `adb` and `fastboot` | confirmed | index, §4.2 |
| C4 | `simctl-run` spawns a bare executable only | `apple-simulator-tools.lua:59-71`; the recipe rejected `simctl launch` because a bare executable did not need it | confirmed; extend the program rather than add a second | index, §4.3 |
| C5 | Cubism payload | tier 2 by `iphoneos-sdk.lua`'s ladder | recorded | index, §4.4 |
| A0 | — | HuxerUI's `mcpp.toml` carries `dialect_cxxflags = ["-pthread"]` unconditionally, with a comment that it cannot be conditional | **not listed**: after A1 and A6 the line becomes `[target.'cfg(not(os = "emscripten"))'.abi] threads = true` in the *application* and `requires_abi` in the library | framework, §5 |

Two items the issue lists as "asked" turned out to be answered by reading
rather than by design: the emsdk payload ships no standard-library BMI (its
recipe precompiles `std.cppm` once as a self-check and deletes the result,
`emsdk.lua:650-680`), so the consumer's `abi` table builds the only BMI there
is and A1's worry about agreement with the payload does not arise; and the
per-target tool declaration the issue's §A8 wants already exists in the form
the engine intends.

## 1. The rules this design is held to

Stated once so that each section can cite a number instead of an argument.

1. **Per project, per target, per shape.** A manifest key, an engine change, a
   plugin, respectively (2026-09-11 record §11). A1, A2, A6 are per target and
   stay engine; B1 to B3 are per shape and stay plugins; the API level and the
   deployment target are per project and stay manifest keys.
2. **A typed field over a flag list** where the spelling depends on the
   driver. This is why `threads` is a member and not `-pthread`, and why
   `frameworks` exists at all.
3. **Declare unconditionally, submit conditionally.** Every dist member; and
   the reason A10 can find the artifact to run.
4. **A name is cache-safe, a path is not.** Runners are named; keystores,
   images and credentials are the runner program's or the signer's own
   configuration, read at run time (2026-09-11 record §10.7).
5. **Anything that is a program is packaged.** `aapt2`, `d8`, `apksigner`,
   `zipalign`, `android.jar`, `keytool` are programs and files under
   open-source licences inside Google's archives; they take the tier
   `android-ndk.lua` already argued.
6. **Compatibility is by ignoring, not by refusing.** An older mcpp reports an
   unknown key under `[targets.<name>]`, `[target.<sel>]` and
   `[target.<sel>.runtime]` and ignores it (`toml.cppm:1254, 2731, 2799`), so
   every key below can be published in a package before every client has the
   engine that reads it. A member of the `abi` table is the one exception: an
   older mcpp refuses an unknown member by name, which is correct, because a
   root that asks for a switch the engine cannot render must not build.
7. **Each item has its own criterion**, including the ones that share a
   mechanism with another. A requirement folded into a neighbour's fix
   disappears when the neighbour ships.
8. **Where a check can pass while measuring nothing, the negative direction
   is part of the criterion.** Every table in §2 to §4 names it.
9. **The ecosystem's model is the package, not the host path.** A tool, a
   data file, a keystore, a runtime stub reach a build program through a
   declared `xim:` package and `xpkg_dir`, never through `~/.something`, an
   environment variable naming a path, or `PATH`. A member takes package
   *names* as options where it needs a choice, and the only things left on the
   host are the three classes rule 5 admits. `dist-appimage` is the existing
   instance; `dist-wix`'s `MCPP_WIX`/`PATH` tiers are the exception this record
   removes (§3.5), and the Android debug keystore is the case where the rule
   would have been broken first (§3.2).

## 2. Engine

### 2.1 A1 — `exceptions`, the second member of the `abi` table

**Why it is an `abi` member and not a conditional `dialect_cxxflags`.** The
2026-09-12 record declined to make `dialect_cxxflags` conditional for a reason
that applies unchanged: a conditional raw flag has no spelling a cl-driven
build could write, and no requirement a dependency could state. Exceptions are
the case the table was built for: clang records the exception model in a BMI
and refuses an importer that disagrees, so the switch must reach the standard
library prebuild, the scan, every translation unit of every package, and the
link — exactly `threads`' reach.

**Shape.**

```toml
# root manifest; HuxerUI's Web adapter validates arguments with exceptions
[target.'cfg(os = "emscripten")'.abi]
exceptions = true

# a dependency states the need, package-wide or per feature
[package]
requires_abi = { exceptions = true }
```

| member | type | renders as | reaches |
|---|---|---|---|
| `exceptions` | boolean | `-fexceptions` on `os = "emscripten"`, on the compile line through the dialect flags and on the link line; nothing on every other target, where exceptions are the default | the same set `threads` reaches |

- Rendered only where the target's default is off. On gcc, clang and MSVC the
  member is satisfied by default, so a host build of a manifest that declares it
  is byte-identical to one that does not — the same property `threads` has on
  PE.
- The accepted set becomes `threads | exceptions`; an unknown member is refused
  naming both. `false` is accepted and equal to absence, as for `threads`.
- `requires_abi` gains the member. The refusal names the member, as it does
  now.
- Emscripten's native `-fwasm-exceptions` is a different ABI and is **not** a
  value here. If it is needed, `exceptions = "wasm"` is the forward-compatible
  spelling (a string where today only a boolean is accepted), and an older mcpp
  refuses it by name, which is the right answer for an ABI it cannot render.

**One measurement first.** `em++ -fexceptions` selects a different libc++
variant from Emscripten's system-library cache, built on first use inside the
payload's own tree. `-pthread` took the same path in the 2026.9.12.2 sandbox
run (the `-mt` variants) and the build succeeded, so the store was writable
where it had to be; the criterion below repeats that reading for the exception
variant rather than assuming it.

**Criteria.**

| claim | criterion | negative direction |
|---|---|---|
| the switch reaches the BMI | with the member, a Web program that throws and catches across an `import std` boundary runs under the sandbox's node and prints its marker | without it, the same program fails at link or at the throw, and the failure names `-fexceptions` nowhere — which is why the member exists |
| the requirement is enforced | a dependency declaring `requires_abi = { exceptions = true }` is refused before compilation on a Web build whose root lacks the table, naming the member | the same build with the table resolves |
| nothing changes elsewhere | a Linux build of the same manifest has a byte-identical `build.ninja` with and without the table | — |
| the cache key sees it | the dependency cache key differs between the two Web builds | — |

### 2.2 A2 — `frameworks` under `[target.<sel>.runtime]`

docs/22 states that the other `[runtime]` keys "are not per-target". iOS is the
counter-example: `AppKit` does not exist in the iOS SDK and `UIKit` does not
exist in the macOS one, so the one Mach-O key that names a platform library is
the one that must be per target. The per-target `runtime` table was introduced
as "the same two keys `[runtime]` already has, per target, inventing no
vocabulary"; this adds the third under the same sentence.

**Shape.** `frameworks` joins `kKnownCondRuntimeKeys`. Semantics are those of
`libraries`: appended after the top-level list, rendered `-framework <name>` on
Mach-O and nothing elsewhere, no override form. A manifest expresses the
macOS/iOS divergence by keeping the shared names at the top level and moving
the divergent ones down:

```toml
[runtime]
frameworks = ["Foundation", "CoreGraphics", "CoreText", "Metal", "QuartzCore"]

[target.macos.runtime]
frameworks = ["AppKit", "Carbon", "MetalPerformanceShaders"]

[target.'cfg(os = "ios")'.runtime]
frameworks = ["UIKit", "MobileCoreServices"]
```

Append rather than replace, for the reason docs/22 gives for every conditional
input: two matching predicates concatenate, and a replacing key would need a
precedence rule between them that nothing else in the table has.

`-weak_framework` stays in `ldflags`. It has one spelling because Mach-O has one
linker family, so the argument for a neutral key does not apply; it is recorded
here so that it is not proposed as `weak_frameworks` without a case.

**Criteria.** The link line of an `aarch64-ios-sim` build carries `UIKit` and
not `AppKit`; the macOS link carries the reverse; a Linux build of the same
manifest is byte-identical to one without either table. An older engine reports
`[target.macos.runtime] has unsupported key 'frameworks' (ignored)`, which is
the compatibility rule 6 relies on.

### 2.3 A3 — an application is a `kind`, not a gate

**The problem, precisely.** On Linux, Windows, macOS, iOS and the Web an
application is an executable: a process entry the platform starts (`main`, or
the JavaScript launcher that calls it). On Android an application is a shared
library the runtime loads into a Java process; there is no executable form of
an app at all. One package therefore needs `kind = "bin"` on five rows and
`kind = "shared"` on one, and `kind` cannot vary. Today the manifest can say
this only through `required_features`, which forces `--features android` onto
every Android command for a fact `--target` already states, puts a feature into
the fingerprint and `mcpp why` that is not a feature, and still builds the
target under the wrong name if the flag is forgotten. The alternative of
declaring both targets unconditionally links a useless program on Android and
a useless library everywhere else, and leaves `mcpp run` and `mcpp pack` with
two candidates.

**What the issue proposes, and why it is not the shape.** A `cfg` gate on
`[targets.<name>]` is the smallest key that removes the feature flag, and it is
the shape `required_features` has. It was this record's first answer. It is
wrong by rule 1: the fact being expressed — *an Android application links as a
shared object* — is true of every Android application and false of every
application elsewhere. It is a property of the **row**, and a row's properties
are the engine's. A gate makes every project restate a platform fact, twice
(one entry per form, with complementary predicates), gives the two forms two
names, and so two output files (`myapp`, `libmyapp-android.so`) and a Java host
that must know the second. The manifest in §5 would carry two target entries
whose only difference is what Android is.

**Two more shapes, and why not.** A conditional `kind` under
`[target.<sel>.targets.<name>]` keeps one name but is the first override-shaped
key in the conditional tables, which docs/22 excludes on purpose, and it still
asks the project to state the platform's fact. Making the engine rewrite
`kind = "bin"` to a shared object on Android silently would be the worst of
them: a `bin` that is not a binary, with `main` compiled and never called.

**Decision: `kind = "app"`.** A fourth value of the closed set, meaning *the
thing a user launches*. Its link form is a function of the row and nothing
else:

| row | form of `app` | file |
|---|---|---|
| ELF, PE, Mach-O rows, `wasm32-emscripten` | identical to `bin` | `myapp`, `myapp.exe`, `myapp.js` |
| `*-linux-android` | identical to `shared` | `libmyapp.so`, which is what `System.loadLibrary("myapp")` and `android.app.lib_name = "myapp"` name |

- One entry, one name, no predicate, and the manifest says what the author
  means (`app`) rather than how one platform spells it. This is the
  "changing one flag builds for another platform" property the 2026-09-11
  record measured for the Web row, kept for Android.
- `main` keeps its meaning on every row where the form is an executable. On
  Android the file it names is compiled as a translation unit of the library;
  the entry is `ANativeActivity_onCreate` or the JNI exports, which is the
  platform's contract and is not mcpp's to rename. `exports` applies as it does
  to any shared target.
- `windows_subsystem` and `windows_entry` accept `app` exactly as they accept
  `bin`. Whether an `app` should *default* to the `windows` subsystem is a
  reasonable question this record does not decide; the explicit key works, and
  a changed default is a separate, measured change.
- Every site that asks `kind == Binary` to mean "the program" is asked "is a
  program" instead — `plan`, `prepare`, `route`, `xpkg`, the directive
  validators, the manifest parser (13 occurrences across six files at
  `85df514a`). That count is the denominator of the sweep, and the criterion
  below is written so that a missed site is red rather than quiet.
- An older engine refuses `kind = "app"` by name (`toml.cppm:1110`,
  "must be 'bin', 'lib' or 'shared'"), which is correct: an application
  manifest is a root, and a root that names a form the engine cannot produce
  must not build. Nothing an older client resolves as a dependency uses it.

**The second half, now simpler.** docs/10's "a dispatched format applies to a
program target" stays exactly as written: an `app` is a program target, on
every row, whatever file it links to. `mcpp pack --format apk` stages the
library under `lib/` because that is where the closure puts shared objects,
and `dist-apk` names it with `${mcpp.target_file:myapp}`. `mcpp run` of an
`app` on a row where the form is a library, without `--format`, is refused
with the sentence that names `--format apk` and the member that provides it
(§2.10). Both are one predicate — "is this target's form on this row a
library" — asked in two places, and it lives beside `artifact_naming`, which is
the other function that answers per-row file questions.

**What remains of the gate.** A target that exists on one platform only is a
real case — HuxerUI's WiX custom-action DLL is a Windows-only `shared`, today
excluded by a `!` glob — and a `cfg` key on `[targets.<name>]` is the right
shape for *that*, because there the fact is the project's. It is recorded here
as a candidate with one known case, and is not proposed, because nothing in
the three rows needs it.

**Criteria.** One manifest with `[targets.myapp] kind = "app"`: under the host
it links a program that `mcpp run` executes; under `--target x86_64-linux-android`
it links `lib/libmyapp.so`, `mcpp run` without `--format` is refused naming
`--format apk`, and `mcpp pack --format apk` stages the library; `mcpp test`
is unchanged on both; a `[targets.tool] kind = "bin"` in the same package
still links an executable on Android, so `bin` keeps meaning binary;
`windows_subsystem = "windows"` on an `app` renders on PE as it does on a
`bin`; `kind = "app"` on mcpp 2026.9.12.2 is refused by name.

### 2.4 A4 — `mcpp::deploy(from, to)`

`runtime.deploy` places a file that exists in the package at a path relative
to the executable, on every object format, in `bin/` and in the staged tree.
What HuxerUI's resource compiler produces is the same thing one step later:
a file that exists in `out_dir` after an action ran. The manifest key cannot
name it, because `from` is package-root-relative and refuses an absolute
path, and there is no directive for it.

**Shape.** Directive 26, protocol version 11:

```cpp
// build.mcpp, after the action that produces the file
mcpp::deploy((odir + "/final/huxerui/resources.bin").c_str(), "myapp.resources/huxerui");
```

- `from` is a file, absolute or package-root-relative, and when it is the
  declared output of an action the copy edge depends on it by construction —
  the edge's input is the action's output, and ninja sequences them. `to` obeys
  the rules the manifest key enforces (`/`-separated, no `..`, `"."` for the
  executable's directory).
- **A file, not a directory.** "You must name the output files" is the graph's
  rule (docs/30): a generated directory whose members are unknown at prepare
  time cannot be in the graph, and a copy edge with a directory input is dirty
  on the wrong events. A generated tree of N files is N outputs and N
  directives, in a loop. HuxerUI's tree is one file today.
- Scope is the declaring package, like the manifest key: the entry joins that
  package's `runtime.linkIntent.deploy`, so `resolve_runtime_contract` merges
  it into the consumer's `bin/` and `mcpp pack` stages it, with the same
  collision rule (two sources for one destination refuse; the same file name in
  two directories does not).
- Replayed from the cache record on a hit, as `runner` and `warning` are: a
  declaration that vanishes when the program does not re-run is the defect the
  advisory channel's tag exists to prevent.
- Every format gets it for free, and each maps "relative to the executable"
  into its own layout, which is rule 1:

| format | where `to = "myapp.resources"` lands |
|---|---|
| tar, dir, AppImage, MSI | `bin/myapp.resources/` beside the executable, as today |
| `.app` (macOS, iOS) | the bundle's executable directory; `dist-apple` decides whether that is `Contents/MacOS/` or the flat root, and it is the member's knowledge |
| `.apk` | `assets/myapp.resources/` — an executable-relative directory has no meaning in an APK, and `assets/` is where an Android program reads files by relative path |
| web | the same relative path in the static directory, served beside `<name>.js`; a project that wants the files inside the `.data` preload instead links with `--preload-file <dir>@/<to>` from its build program, which is a link flag and therefore not this directive's business |

**Criteria.** After `mcpp build`, `bin/myapp.resources/huxerui/resources.bin`
exists and is the action's output; touching the action's input rebuilds the
copy; a second `mcpp build` with a cache hit still has the file after `bin/` is
deleted (the replay criterion); `mcpp pack --format dir` stages it at the same
relative path; `to = "../x"` is refused naming the directive and the package.

### 2.5 A5 — the wasm artifact contract

Sharper than the issue asked. `artifact_naming` has rows for `windows`,
`macos` and `linux` and returns the **host's** naming for any other OS
(`triple.cppm:1127`: "fall back to the host answer rather than guessing"). So a
`bin` on `wasm32-emscripten` is named `bin/<name>` on a Linux host and
`bin/<name>.exe` on a Windows one. The verified row was measured on Linux.

**What the industry does, read rather than recalled.** Emscripten's driver
chooses what it emits from the extension of `-o`: `.js` is a JavaScript
launcher plus `<stem>.wasm`; `.html` adds a shell page; `.wasm` is a standalone
module with no JavaScript, which is a different runtime contract (WASI-shaped);
anything else, including no extension, is treated as the `.js` case. Every
build system that targets Emscripten therefore fixes the executable suffix to
`.js`, and the two authoritative ones say so in one line each:

| system | where | statement |
|---|---|---|
| Emscripten's own CMake toolchain | `cmake/Modules/Platform/Emscripten.cmake` | `set(CMAKE_EXECUTABLE_SUFFIX ".js")` |
| Rust | `rustc_target/.../wasm32_unknown_emscripten.rs` | `exe_suffix: ".js"` |
| Meson | the `emscripten` compiler family | executable suffix `js` |
| Bazel `emsdk` rules | `wasm_cc_binary` | outputs `<name>.js` and `<name>.wasm` |

Rust's other wasm targets (`wasm32-wasip1`, `wasm32-unknown-unknown`) use
`.wasm`, because there the executable *is* the module and the host runtime is
the launcher. The extension is not cosmetic: it names which of two runtime
contracts the file carries.

**What mcpp's naming means, and why the two coincide.** `artifact_naming` has
one semantics on every row: the executable is *the file a runner executes*, in
the row's own convention (`.exe` on PE, none on ELF and Mach-O). On the
Emscripten row the file a runner executes is the JavaScript launcher — that is
what the payload's `node` receives today and what a browser loads — and the
row's own convention for that file is `.js`. So the row is not an exception to
mcpp's rule and not a concession to another tool's rule; it is the same
sentence evaluated on a row where the industry and the engine give the same
answer. A future `wasm32-wasi` row would say `.wasm` by the same sentence, for
the same reason Rust does.

**Decision.**

| kind | file | siblings |
|---|---|---|
| `bin`, `app` | `bin/<name>.js` | `bin/<name>.wasm`, an implicit output of the link edge; `bin/<name>.data` when the link line carries `--preload-file`; any further file emcc writes with the same stem (`.worker.js`, `.wasm.map`) |
| `lib` | `lib/lib<name>.a` | — |
| `shared` | refused, naming `-sSIDE_MODULE` as the mechanism mcpp does not render | — |

- `${mcpp.target_file:app}` is `bin/app.js`; the payload's `node` runner takes
  it unchanged; test binaries follow. The bare name the row has today relies on
  emcc's fallback and is a different file name on a Windows host, which is the
  defect.
- The `.wasm` is not a second executable and not a deploy'd file the project
  wrote; it is what the link *also* produced. It is declared as an implicit
  output of the link edge so that `ninja -t clean`, the incremental check and
  `mcpp pack` see it through the graph rather than through a naming convention.
  The rest of the stem family is staged by `mcpp pack` as "every file the link
  wrote with this stem", stated as a rule so that a `.data` the project
  expected and the link did not write is visible in `--format dir`.
- `.html` is not an engine output. It is a page, and which page is `dist-web`'s
  knowledge (§3.3), exactly as an MSI's definition is `dist-wix`'s.
- `shared` is refused rather than named `lib<name>.so` by the fallback: a side
  module needs a flag mcpp does not render, and the fallback's output is a
  file that exists and does not work.
- **`--no-entry` and a `bin` with no `main`.** No engine code checks for the
  symbol. `[targets.<name>] main` names a translation unit, and `--no-entry` is
  an ordinary `[target.'cfg(os = "emscripten")'.build] ldflags` entry, so a
  modularised Web program whose page calls the exported factory is a `bin`
  whose main file defines no `main()`. This is stated in docs/21 and docs/22
  rather than changed.
- Also fixed under the same row: `flags.cppm:756` names `LinkIntentFlavor::Wasm`
  as "the open half of #597" and falls back to ELF. `link_lib` renders
  `-l<name>`, which emcc accepts (`-lidbfs.js` is HuxerUI's own example), so
  the flavour is ELF's spelling with `frameworks` and `link_library_dirs`
  unchanged; the fallback becomes a row so that the switch has no default arm.

**Compatibility.** The row was published as `verified` on 2026-09-11 with the
bare name. The rename is a visible change to one day-old row with one known
consumer, and it is made now because every later consumer of `dist-web` and
every `${mcpp.target_file:}` in a build program would otherwise encode the
bare name. The CHANGELOG entry states the old and the new name.

**Criteria.** On Linux and Windows hosts, `mcpp build --target wasm32-emscripten`
names the same two files; `mcpp pack --format dir` stages both and the `.data`
when `--preload-file` is present; a `bin` whose main file has no `main()` links
with `--no-entry` and is refused without it by the linker, not by mcpp;
`kind = "shared"` is refused by name.

### 2.6 A6 — `requires_abi` on the target axis

`requires_abi` is one boolean per package or feature, and the root's `abi` value
is target-resolved. So a root that writes
`[target.'cfg(not(os = "emscripten"))'.abi] threads = true` for a UI library
that needs threads on every hosted row and must not have them on the Web is
refused on the Web build: the library's requirement is unconditional and the
Web value is off.

**Where the requirement belongs.** The threads are needed by the Linux and
Android adapters and not by the Web one, and those adapters are selected by
`[target.'cfg(linux)'.build] sources`. The requirement comes from the same
sources and sits in the same table:

```toml
[target.'cfg(linux)']
requires_abi = { threads = true }

# the per-feature form follows the existing per-target-per-feature pattern
[target.'cfg(linux)'.feature-requires-abi]
mt = { threads = true }
```

- The set of requirements is the union of `[package] requires_abi`, the active
  features' tables, and every matching selector's, evaluated against the
  resolved target before the existing check at `prepare.cppm:8909`; the refusal
  names the selector that required it.
- The feature form is named after `feature-deps.<f>` and `feature-xlings.<f>`,
  the two existing per-target-per-feature tables. It ships in the same change
  because docs/22's own example of `requires_abi` is a feature.
- Evaluation reads the dependency's loaded manifest, as the check does today,
  so no descriptor form is needed and nothing new crosses the index.
- An older engine warns `[target.cfg(linux)] has unsupported key 'requires_abi'
  (ignored)` and loses the requirement, which is the behaviour it had before
  2026.9.12.2.

  **Correction (2026-09-12, T4).** This does not hold. `mcpp 2026.9.12.2`
  reads `[target.'cfg(linux)'] requires_abi = { threads = true }` with no
  diagnostic at all — measured by running it against that exact binary. The
  key is a direct, inline-table-valued key of the selector table, not a
  sub-section, and the schema sweep that would warn on an unsupported
  `[target.<sel>]` key skips every table-valued key on the assumption that a
  table is the conditional channel; an inline table is the same TOML value
  shape and falls through the same sweep unreported. So an older engine
  **silently ignores** the requirement rather than warning about it — a
  package that relies on the refusal to protect an unconditional switch must
  state its own engine floor.

**Criteria.** A library with the Linux table and a root with
`cfg(not(os = "emscripten"))` threads builds for both Linux and Web; remove the
root table and the Linux build is refused naming `cfg(linux)`; the Web build is
unaffected by the library's table in both directions.

### 2.7 A7 — `[package] platforms` names the rows that exist

Two vocabularies are disconnected: the triple's (`ios`, `emscripten` as `os`,
`android` as `env`) and `platforms`' (`linux | macos | windows`). A package that
serves the Web today is warned at for saying so.

**Rule.** A platform name is the triple's `os`, except where an `env` names a
platform of its own. The vocabulary becomes
`linux | macos | windows | ios | android | emscripten`; `linux` does not cover
the Android rows. `emscripten` rather than `web`, because it is the word the
selector grammar already uses and a WASI row would be another `os`.

- The three readers change together: the validator (`prepare.cppm:2256`),
  `doctor`'s "declared platforms" line, and the pack coverage check, whose
  `servable_here` candidate list is hard-coded to three triples and should ask
  `host_can_serve` for each row instead, so the third row of docs/04 §2.12's
  table stays true for `ios` on a Linux runner.
- `mcpp emit xpkg` is untouched: the value is written into no descriptor
  (`xpkg.cppm` has no `platforms` key), and the descriptor's `xpm.<host>` blocks
  are a **host** axis, which a target claim must not feed.

**Criterion.** `platforms = ["emscripten"]` passes `--strict`; `["web"]` is
refused naming the six; `mcpp pack` of a library on a Linux runner with `ios`
declared is silent.

### 2.8 A8 — the declaration was on the wrong package

The issue reads docs/31 as "a dependency's declaration never reaches the
consumer". The engine's rule is narrower and already what is wanted:
`fillXpkgDirs` answers from the building package's declarations **and from
every host module compiled into that build program** (`prepare.cppm:5511-5528`;
`tests/e2e/622_rule_declared_payload_reaches_the_consumers_build_program.sh`).
An ordinary library edge contributes nothing, and that is where HuxerUI put
`xim:wix`: on `huxerui`, the library, while the code that runs `wix` is
`huxerui.rules`, a host module reexported through it.

**Decision.** No engine change and no `reexport` for `[xlings.workspace]`.
The line moves from `huxerui/mcpp.toml` to `huxerui-build-rules/mcpp.toml`,
under `[target.windows.feature-xlings.<f>]` or its `[xlings.workspace]`, and the
"five lines an application never chose" do not arise for the dist members
either: `dist-appimage` already declares `xim:appimagetool` on itself
(`mcpp-plugins/mcpp.toml:359`) and its fixture declares nothing. docs/31's
sentence gains its second half — a host module's declaration is visible in
every build program it is compiled into, which is why the member declares it
and not the project.

**Criterion.** `tests/e2e/622` is the criterion, and it exists; HuxerUI's
probe is `mcpp::xpkg_dir("xim", "wix")` non-empty from an application that
declares only `huxerui`.

### 2.9 A9 — one artifact from several targets

Recorded, not attempted. The shape that fits when it is needed: `mcpp pack`
accepts more than one `--target`, builds and stages each, and the provider
receives `${mcpp.stage_dir:<triple>}` for each and `${mcpp.stage_dir}` for the
current one. A universal APK and a universal Mach-O (`lipo`) are the same
request, which is the argument for an engine placeholder rather than a member
reading sibling `target/` directories — the second breaks the staged tree's
hermeticity. Nothing in §3.2 precludes it.

### 2.10 A10 — the run operand of an application

Not in the issue. `mcpp run` executes the link output, through a runner when
one is named. A Web program and a bare Android or iOS executable are link
outputs and run today. An Android **application** is an `.apk` and an iOS one is
an installed `.app`; neither is the link output, and the runner sessions C3 and
C4 propose take those files. The issue lists both sessions and never says what
hands them their operand.

**Shape.** One flag, reused verbatim from `mcpp pack`:

```bash
mcpp run --target x86_64-linux-android --format apk
mcpp run --target aarch64-ios-sim      --format app
```

`mcpp run --format <name>` is `mcpp pack --format <name>` — the two passes,
the staged tree, the provider's action — followed by the ordinary run with the
artifact `mcpp pack` reports as the operand. The runner resolves as it does for
a program: the project's `[target.<triple>] runner`, then a dependency's
`mcpp::runner(...)`, then the payload descriptor's. Nothing new is resolved and
no new outlet is declared: rule 3 is what makes the reported artifact
unambiguous.

- `--format` with `--no-runner` is refused: an `.apk` cannot be executed.
- `mcpp run` of an `app` on a row where its form is a library (§2.3), with no
  `--format`, is refused with the sentence that names the flag and the set of
  formats the graph provides — the same set `mcpp pack --format bogus` prints.
- `mcpp test` is unchanged; test binaries are programs.
- Web is not this: its program runs under `node` today, and its browser form is
  served, not run. A dev server is a program and packageable, but it is out of
  this record's scope.

**Criteria.** In the sandbox, `mcpp run --target x86_64-linux-android --format apk`
on a NativeActivity program installs into the running emulator and returns
that program's status; the same command without a runner declared is refused
naming `runner`; `--no-runner` with `--format` is refused.

### 2.11 A11 — the platform floor in the build-program contract

Not in the issue, and the reason B1 cannot be written as described:
`dist/apple.cppm:140-143` states that mcpp does not expose the compiled
deployment target to a build program, so `minimum_system_version` is restated
by the project and drifts. The engine has the value as one function,
`min_platform_version` (`prepare.cppm:1581`), and one fingerprint slot, and
names it in the platform's own words in the manifest.

**Shape.** `MCPP_TARGET_MIN_PLATFORM_VERSION`, typed reader
`mcpp::min_platform_version()`: `14.0` on macOS, `18.0` on iOS, `24` on
Android, empty elsewhere, the value the effective triple carries. It joins the
environment contract table in docs/30 and the re-run key as every contract
value does. `dist-apple` reads it for `LSMinimumSystemVersion` and
`MinimumOSVersion`; `dist-apk` for `minSdkVersion`. `minimum_system_version`
in `dist-apple`'s options becomes an override, and its comment is rewritten.

**Criterion.** The value in the build program equals the `minos` in
`LC_BUILD_VERSION` of the produced Mach-O, and the API level in the effective
triple on Android; empty on Linux.

## 3. `mcpp:plugins`

Every member below follows the shape the three existing dist members share
(`options` struct, `plan_for` that refuses by name rather than skipping,
`submit` of `artifact` actions, `generate()` that declares unconditionally and
submits conditionally, a `tests/<name>-consumer` fixture) and the four duties
docs/31 lists. What is specific to each is stated; what is shared is not
restated.

### 3.1 B1 — `dist-apple` on iOS

The member's own header says iOS "is the same shape plus a target row the
engine does not yet have". The row exists; the return at `apple.cppm:367`
becomes a branch on `target_os()`.

| | macOS (today) | iOS |
|---|---|---|
| layout | `Contents/MacOS`, `Contents/Resources`, `Contents/Info.plist` | flat: the executable, `Info.plist` and resources at the bundle root |
| minimum version | `LSMinimumSystemVersion` from an option | `MinimumOSVersion` from A11 |
| platform keys | — | `CFBundleSupportedPlatforms = ["iPhoneSimulator"]` for the `sim` rows, `["iPhoneOS"]` for the device row; `UIDeviceFamily`; `LSRequiresIPhoneOS` |
| icons | `CFBundleIconFile` | flat PNGs named `AppIcon60x60@2x.png` and so on, `CFBundleIcons` in the plist; no `actool`, which is Xcode's |
| deploy'd files (A4) | beside the executable in `Contents/MacOS/` | at the bundle root, which is the executable's directory in a flat bundle |
| signing | opt-in `codesign` | none for the simulator, which installs an unsigned bundle; the device row keeps the `rcodesign` recommendation of the 2026-09-11 record and is out of scope here |
| copies | `ditto` | `ditto`, the same reason |

The `.app` is the operand `simctl-run` receives from `mcpp run --format app`
(A10, §4.3).

**Criterion.** On the macOS lane, `mcpp pack --target aarch64-ios-sim --format app`
produces a bundle `xcrun simctl install` accepts, whose `Info.plist` carries
the A11 value, and `mcpp run --format app` prints the program's marker through
the extended `simctl-run`; a macOS `--format app` on the same tree is
byte-identical to 0.6.0's.

### 3.2 B2 — `dist-apk`

Two levels, and the first has no Java in it.

**Level 0 — a native application.** Android's `NativeActivity` loads a shared
library named in the manifest and needs no `classes.dex` (`hasCode="false"`).
The member generates `AndroidManifest.xml` from `[package]` and its options
(application id `<namespace>.<name>` with `.` for `-`, label, `minSdkVersion`
from A11, `targetSdkVersion` from the platform payload's level,
`android.app.lib_name` from the `app` target's name, §2.3), then:

```
aapt2 compile   resources, when the project supplies any
aapt2 link      -I android.jar --manifest ... --min-sdk-version ... -o base.apk
                + lib/<abi>/ from the staged tree's shared objects and the program
                + assets/ from the deploy'd files (A4)
zipalign -p 4
apksigner sign  --ks <keystore>
```

`<abi>` is derived from the triple's arch: `aarch64` is `arm64-v8a`, `x86_64`
is `x86_64`. Inputs: `xim:android-build-tools` (C1), `xim:android-platform-<api>`
(C2), a JDK — `apksigner` and `d8` are Java programs, so the JDK is needed at
level 0 too — all declared on the member itself under
`[target.'cfg(env = "android")'.feature-xlings.dist-apk]`, which is the
appimage precedent applied to the target axis.

**Level 1 — a Java host.** `java_sources = "android/java"` adds `javac -cp
android.jar` and `d8` producing `classes.dex`, and the manifest names the
consumer's `Activity` instead of `NativeActivity`. The sources are the
consumer's (HuxerUI ships its host library's); the member compiles what it is
given and never carries Java of its own.

**Signing, through the package model and not the host's.** Android's tooling
signs debug builds with a keystore it generates under `~/.android`, per
machine. That is the host-path model rule 9 excludes: a path outside the
store, different bytes on every machine, and a `keytool` run at pack time. The
ecosystem's answer is a payload, `xim:android-debug-keystore`, carrying one
`debug.keystore` with the alias and password Android's documentation has
published for its debug key since the platform's first release (`androiddebugkey`,
`android`). Publishing them discloses nothing: they are the documented
convention, and a debug key's only property is that it is not a release key.
The member declares it beside its other payloads and finds it with `xpkg_dir`,
so a debug APK is signed identically on every machine and in CI, and nothing is
generated at pack time.

A release key is a credential and stays out of any public index, by the
2026-09-11 rule. It still enters by the same door: the member takes a
**package name**, `keystore = "myorg:release-keystore"`, resolved with
`xpkg_dir` from a package the project declares under
`[target.'cfg(env = "android")'.xlings.workspace]` — from a private xim index,
which xlings supports — with the alias and the password's environment variable
name as options. The member never takes a path. A team that keeps the key
outside every index signs after `mcpp pack`, with the `apksigner` the payload
already provides; that is the CI's choice and not a member option.

**The C++ runtime.** Whether an mcpp Android link carries `libc++_shared.so`
or links libc++ statically is not measured in this record. If shared, the
staged closure must already contain it or the member must take it from the
NDK payload's sysroot; the fixture asserts the installed application starts,
which is the reading that distinguishes the two.

**Criteria.** In the sandbox with the emulator lane: a `1-2-3` NativeActivity
program packs, installs, runs and its stdout reaches the caller through the
runner (§4.2); the APK's `lib/x86_64/` lists the program and every shared
object of the closure and nothing else; `apksigner verify` passes; a project
with `java_sources` produces `classes.dex` and the manifest names its
activity; without C1 declared, the member refuses naming
`xim:android-build-tools` and the table to declare it in.

### 3.3 B3 — `dist-web`

Takes `bin/<name>.js`, `bin/<name>.wasm`, the `.data` when present (A5), and
the deploy'd files (A4), and writes a static directory with an `index.html`
rendered from a template. The default template loads the script with
`<script src>`; a modularised program supplies its own
(`template = "web/index.html"`), because whether the page calls a factory or
relies on `main` is the program's contract, not the member's. No bundler, no
hashing, no server: a front-end tool's job, and a dev server is a program that
can be packaged later if a case appears.

**Criterion.** `mcpp pack --format web` on the verified row's program produces a
directory that a static server serves and whose page prints the marker in the
console; the member's staged file set is exactly the A5 family plus the
deploy'd files, checked by listing, not by "the page loads".

### 3.4 B4 — recorded candidates

`rules-metal` (`xcrun metal` and `metallib`, the `rules-spirv` shape; macOS
only because the compiler is Xcode's, which is a (b)-class host dependency
until Apple ships it elsewhere) and `tools-esbuild` (`esbuild` from `xim:node`'s
ecosystem, or its own payload). Neither blocks a row; each has one motivating
library. Recorded so that they are not proposed as engine features.

### 3.5 B5 — `dist-wix` and `xim:wix`

The README is the wrong reading. WiX is MS-RL, an OSI licence that permits
redistribution; `pkgs/w/wix.lua` ships 5.0.2 as three NuGet payloads under
that licence; and `dist/wix.cppm`'s own header quotes the NuGet package's
licence text as evidence the payload is legitimate and calls the current
lookup "a gap and not the answer".

**Decision.** `dist-wix` declares `"xim:wix" = { windows = "5.0.2" }` under
`[target.windows.feature-xlings.dist-wix]` and locates the tool with
`xpkg_dir("xim", "wix")` — and with nothing else. The `MCPP_WIX` and `PATH`
tiers are removed rather than demoted: they existed because the payload was
believed unshippable, and rule 9 leaves no place for a host lookup once a
payload exists. The `options::tool` override stays, as it does in
`dist-appimage`, for a project that builds the tool itself. The README's
sentence and the comment in `mcpp.toml` are corrected. `rules-spirv`'s
environment tiers are a separate, older instance of the same shape and are
recorded here for the same treatment in their own change.

Whether the payload runs without a .NET runtime on the machine is the one
thing to measure before the README says "installed": the recipe's `wix` is a
.NET tool, and if it needs a runtime, `xim:dotnet` is the recipe's dependency
and not the consumer's — the same shape as `xim:emsdk` depending on `xim:node`.

## 4. `xim-pkgindex`

### 4.1 C1, C2 — `android-build-tools`, `android-platform-<api>`

Both are Google's per-host archives and both fall under the licence reading
`android-ndk.lua:78-108` records: the SDK agreement's §3.4 forbids
redistribution and §3.5 carves out bundled open-source components, so the
question is what is inside. `aapt2`, `zipalign`, `apksigner`, `d8` (r8) and
`android.jar` are AOSP components under Apache-2.0 and BSD licences; the tier
is 1 (redistribute, mirror) **if** the archives' `NOTICE` files say so, which
the recipes read and record as `android-ndk.lua` does, not assume. The
platform package is versioned by API level as its name says, because the
level is the thing a consumer pins; the build-tools package by Google's
release number. Neither is Windows-excluded: unlike the NDK, nothing in them
is a module surface.

### 4.2 C3 — `adb-run`

A session, in `android-platform-tools`, registered beside `adb` as
`simctl-run` is registered beside nothing. Two operand shapes, decided by the
file: an `.apk` is `adb install -r`, `am start -n <id>/<activity>` with the id
read from the package with `aapt2 dump badging` or written into the APK's
`assets/` by `dist-apk`, then `logcat --pid` of the launched process until it
exits, with stdout redirected to the log (`log.redirect-stdio`) so a program's
marker reaches the caller; a bare ELF is `adb push` to a temporary directory,
`adb shell` of it, its exit status, and removal. The exit status of an
activity is what the runner can observe: normal termination is `0`, a tombstone
or `FATAL EXCEPTION` for the pid is non-zero. The device or emulator is
whichever `adb` reports as connected, with `ANDROID_SERIAL` as the program's
own configuration — rule 4. The 2026-09-11 record named this program
`mcpp-android-device-run`; `adb-run` follows the `<tool>-run` naming
`simctl-run` established and is preferred.

### 4.3 C4 — `simctl-run` takes a `.app`

The recipe rejected `simctl launch` because a bare executable did not need an
installed bundle; a UIKit application does. Rather than a second program, the
existing one branches on the operand: a `.app` is `simctl install`, `simctl
launch --console-pty --terminate-running-process <bundle-id>` with the id read
from the bundle's `Info.plist`, wait, and the status; anything else is the
`spawn` it does today. One name in the manifest and the docs, and one place the
device-selection logic lives. Whether `launch --console-pty` returns the
application's status or only its console is the measurement the recipe must
record before the row claims a run; if it returns nothing, the runner reads
the `simctl spawn launchctl` state of the pid, and the recipe says so.

### 4.4 C5, C6

Cubism is tier 2 by the ladder `iphoneos-sdk.lua` states: one anonymous
upstream URL, `sha256` pinned, no CN entry, `licenses` recording both the Open
Software License and the Core's proprietary one. Recorded; it blocks no row.
The rest (`android-ndk`, `emsdk`, `node`, the JDKs, `android-emulator`,
`android-system-image`, `android-platform-tools`, `apple-simulator-tools`,
`appimagetool`, `wix`) is present.

## 5. The framework's side, and the manifest it ends with

What stays with HuxerUI is what the issue says (its code generator, resource
compiler, rule adapters, Java host sources, templates), plus one move the
issue lists as an engine wish: the `xim:wix` declaration goes to its rule
package (§2.8), and its `dialect_cxxflags = ["-pthread"]` line becomes an
`abi` statement in the application and a `requires_abi` in the library. The
test of the whole design is the manifest a HuxerUI application writes after
everything lands — one conditional section per row and no per-platform mode,
which is the property the 2026-09-11 record measured for the Web row and this
record must keep. The one target entry is the visible effect of §2.3: a
project that builds for Linux builds for Android by changing the flag, and the
manifest says `app` once:

```toml
[package]
name = "myapp"
namespace = "example"
version = "0.1.0"
platforms = ["linux", "windows", "macos", "ios", "android", "emscripten"]

[dependencies]
huxerui = "0.4"

[build-dependencies.mcpp]
plugins = { version = "0.7", features = ["dist-apple", "dist-apk", "dist-web"], host-module = true }

[targets.myapp]
kind = "app"

[target.'cfg(not(os = "emscripten"))'.abi]
threads = true

[target.'cfg(os = "emscripten")'.abi]
exceptions = true

[target.'cfg(os = "emscripten")'.build]
ldflags = ["--no-entry", "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sEXPORT_NAME=createMyApp"]

[target.aarch64-linux-android]
min_api_level = 24

[build]
ios_deployment_target = "17.0"
```

and the commands, one verb per intent with the target as a flag:

```
mcpp run  --target wasm32-emscripten                     node, from the payload
mcpp pack --target wasm32-emscripten --format web
mcpp run  --target aarch64-ios-sim --format app          simctl-run, from apple-simulator-tools
mcpp run  --target x86_64-linux-android --format apk     adb-run, from android-platform-tools
```

The library's manifest carries `requires_abi = { threads = true }` under
`cfg(linux)` and `cfg(os = "ios")`, `requires_abi = { exceptions = true }` under
`cfg(os = "emscripten")`, and the iOS framework list under
`[target.'cfg(os = "ios")'.runtime]`. Nothing in the application names a
framework, a flag spelling, a tool, or a platform's spelling of what an
application is.

## 6. Order, and the sub-issues

Four repositories. The engine work is one release, because every item in it is
a key or a row and none depends on another repository; the plugin and index
work follows the release, because each adopts something only the new engine
reads or hands over.

```
repo             id   task                                                        depends on
---------------  ---  ----------------------------------------------------------  ----------------
mcpp             M1   A1 exceptions: parser, render, cache key, requires_abi,     -
                      docs/22, docs/04; wasm e2e on the emsdk lane
mcpp             M2   A2 frameworks per target; unit test; docs/22, docs/04       -
mcpp             M3   A3 kind = "app": parser, per-row form beside               -
                      artifact_naming, the 13-site sweep, pack and run rules;
                      unit and e2e; docs/04, docs/10
mcpp             M4   A4 deploy directive, protocol 11; replay on cache hit;      -
                      pack staging; e2e; docs/30, docs/04
mcpp             M5   A5 emscripten naming row, implicit .wasm, stem family in    -
                      pack, LinkIntentFlavor::Wasm, shared refused; two-host
                      criterion in CI; docs/21, docs/22, docs/10
mcpp             M6   A6 requires_abi under a selector and feature-requires-abi;  M1
                      unit test both directions; docs/22
mcpp             M7   A7 platforms vocabulary; servable_here via host_can_serve;  -
                      docs/04
mcpp             M8   A8 docs/31 second sentence                                  -
mcpp             M9   A10 mcpp run --format; refusals; e2e with a stub runner     M3
mcpp             M10  A11 MCPP_TARGET_MIN_PLATFORM_VERSION; docs/30              -
mcpp             M11  CHANGELOG, version, release, sandbox verification of the    M1-M10
                      published form, bootstrap pin
xim-pkgindex     X1   C1 android-build-tools, licence reading recorded            -
xim-pkgindex     X2   C2 android-platform-<api>, same                             -
xim-pkgindex     X3   C3 adb-run in android-platform-tools                        -
xim-pkgindex     X4   C4 simctl-run takes a .app                                  -
mcpp-plugins     P1   B5 dist-wix payload tier; README and mcpp.toml corrected    -
mcpp-plugins     P2   B1 dist-apple iOS branch                                    M11, X4
mcpp-plugins     P3   B3 dist-web                                                 M11
mcpp-plugins     P4   B2 dist-apk level 0, then level 1                           M11, X1-X3, X5
mcpp-plugins     P5   MCPP_VERSION raised; fixtures; release                      P1-P4
mcpp-index       I1   mcpp:plugins next version                                   P5
HuxerUI          H1   the xim:wix line moves to the rule package (§2.8)           -
HuxerUI          H2   the abi / requires_abi / frameworks / cfg sections; 0.4     M11, I1
```

Cut as sub-issues by bin, as the issue asks: **mcpp** one issue per M1 to M10,
with M9 and M10 stated as the two the parent does not list; **xim-pkgindex**
X1 to X4; **mcpp-plugins** P1 to P4. X1 to X5 and P1 and H1 wait on nothing
and can start today. What unblocks each row:

| row | engine | index | plugins |
|---|---|---|---|
| Web | M1, M4, M5, M6 | — | P3 |
| iOS simulator | M2, M4, M9, M10 | X4 | P2 |
| Android emulator | M3, M4, M7, M9, M10 | X1, X2, X3, X5 | P4 |

## 7. Read from the angles the review asked for

**Semantic consistency.** Every new key is an instance of a class the manifest
already has: a second `abi` member; a third per-target `runtime` key; a fourth
value of `kind`, whose per-row form sits beside the function that already
answers per-row file questions; a directive mirroring a manifest key; a per-target
form of a requirement, named after the two per-target-per-feature tables that
exist; a vocabulary widened by the triple's own words. Nothing introduces a
new class of thing. The one new command form, `run --format`, reuses pack's
flag and pack's report.

**Reuse.** The `abi` channel, the deploy pipeline, the two-pass pack, the runner
resolution order, `min_platform_version`, `host_can_serve`, the dist-member
shape and the `feature-xlings` declaration all exist; each item extends one
and builds none.

**Simplicity.** Two items the issue asked for are declined as engine work: A8,
because the mechanism exists and the declaration was misplaced, and A9,
because no row needs it. One item is replaced by a smaller one: a `cfg` gate
that would have made every application state a platform fact twice becomes
one word, `app`. One member (`dist-apk`) is split so that its first level
carries no Java toolchain, and no member reads a host path.

**Cross-platform.** A5's row is the one item that would have differed by
*host* (a Windows host named the wasm program `.exe`); the criterion runs on two
hosts. Every other item is per target and has a byte-identical-elsewhere
criterion.

**Elegance, measured as the manifest in §5.** One conditional section per
row, no tool named, no flag spelling except the Emscripten link options that
are genuinely the program's own contract with its page.

**Ecosystem.** The boundary of the 2026-09-11 record holds: the engine gains
keys and a row and knows no format; the plugins know formats and no target
identity; the index ships programs and never bytes it cannot ship. Two places
the boundary was blurred are corrected in the direction the rule points: a
distribution tool located on the host because a README said it could not be
shipped, and a signing key that Android's own tooling keeps under a home
directory. Both become packages, found the way every other payload is found.

## 8. What this record does not measure

- `-fexceptions` building the libc++ variant inside a read-only-looking payload
  store (§2.1). The threads case succeeded; the exception case is assumed to
  take the same path and is the first thing M1's e2e reads.
- Whether the Android link uses `libc++_shared.so` (§3.2).
- Whether `simctl launch --console-pty` returns the application's exit status
  (§4.3).
- The `NOTICE` contents of the build-tools and platform archives (§4.1).
- Whether `xim:wix`'s tool runs without a machine .NET runtime (§3.5).
- Emscripten's exact stem family for a `-pthread` link on 6.0.9 (§2.5); the
  staging rule is written as a rule so that the family's membership is not a
  claim this record has to get right.
- Whether an APK signed with the published debug key installs over one signed
  by Android Studio's per-machine key on the same emulator (§3.2). It should
  not, by design, and the criterion says so rather than assuming a clean
  device.
- The `kind == Binary` sweep (§2.3) is counted, not read; a site that asks the
  question through another name is what the Android `bin`-beside-`app`
  criterion exists to catch.

Each is a reading a sub-issue takes before its criterion is written down as
met; none changes a shape above.
