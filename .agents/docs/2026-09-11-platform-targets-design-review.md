---
subject: targets
status: active
---

# Where a platform's knowledge belongs: iOS, Android and Web across the engine, the index and the plugins

Date: 2026-09-11. Written after `wasm32-emscripten` reached `verified`, because
that row is the first of the three to be wired end to end and what it needed is
the evidence this review is about.

The question: for iOS, Android and Web, is mcpp's division of target
identity, toolchain resolution and artifact shape the right one, and are the
names right. Answered against what four other build systems do, and against
the seven engine changes the wasm row actually required.

## 1. What mcpp does today

Three separable things, currently in three places.

| | where | what decides it |
|---|---|---|
| target identity | engine, `kKnownTargets` | a closed table; a package cannot add a row |
| toolchain resolution | engine, `to_xim_package` + the gates | a property of the target |
| the payload itself | index, `xim:<name>` | a licence question before a packaging one |
| artifact shape | plugins, `dist-*` | the format's own tool |

The engine's stated rule is one sentence: **the engine owns the mechanism by
which a distributable is produced, and no format lives in the engine.** That
rule is about the fourth row. This document is mostly about the first two,
which the wasm work exercised for the first time.

## 2. The comparison, and it is more reassuring than not

### 2.1 Rust is the closest analogue, and the tiers line up almost exactly

`rustc`'s target list is **closed** — `rustc --print target-list` — and carried
in the compiler, which is what `kKnownTargets` is. Its tier definitions:

| Rust | guarantee | mcpp |
|---|---|---|
| Tier 1 | "guaranteed to work": official builds **and automated tests** | `verified` -- "built AND RUN" |
| Tier 2 | "guaranteed to build"; automated tests **are not always run** | `preview` |
| Tier 3 | code exists, no automated building or testing | `planned` |

The mapping is close enough to be worth adopting as calibration rather than
coincidence. And it produces an uncomfortable measurement:

**All three of these platforms are Tier 2 in Rust** —
`wasm32-unknown-emscripten`, `aarch64-linux-android` and `aarch64-apple-ios`
are each "guaranteed to build", with tests not always run.

So mcpp's `verified` for `wasm32-emscripten` is a **stronger** claim than Rust
makes for its own wasm target, and mcpp's `planned` for Android and iOS is
**weaker** than Rust's. The honest end state for Android and iOS is
`preview` — buildable, not run in CI — and that is not a compromise, it is the
same claim the most comparable toolchain in the industry makes.

`cargo` does **not** manage SDKs. `cargo-ndk`, `cargo-apk` and `xcodebuild`
are outside it. So Rust puts identity in the core and the SDK outside it,
which is mcpp's split with `xim:` in the "outside" position.

### 2.2 Zig is the strongest counter-model, and it draws the same line

Zig ships libc and headers for a closed list of 97 targets, and calls
cross-compilation "a first-class use case". It is the system most willing to
bundle a target's system — and it ships **neither the Apple SDK nor the
Android NDK**. Those remain the user's to provide.

Two conclusions. First, `Triple::has_own_sysroot()` is not an mcpp
peculiarity: "the toolchain arrives with its own system" is a real axis that
the most aggressive bundler in the field also recognises. Second, the line Zig
draws — bundle what is freely redistributable, decline the vendor SDKs — is
the line this ecosystem reached independently from the licence text, and
`xim:iphoneos-sdk`'s three-tier posture is the same conclusion.

### 2.3 CMake puts the target knowledge in the SDK, and it is why CMake cross-compilation is per-SDK folklore

CMake has `CMAKE_SYSTEM_NAME` and toolchain files, and for these three
platforms the knowledge lives in a file the **SDK** ships: Emscripten's
`Emscripten.cmake`, the NDK's `android.toolchain.cmake`, Apple's
`CMAKE_OSX_SYSROOT`. There is no list of targets in CMake to be complete
about.

The cost is that every SDK invents its own variables and every project learns
three unrelated dialects. mcpp's closed table is the opposite trade: adding a
platform is an engine change, and in exchange `--target <triple>` means one
thing everywhere. The wasm row is the evidence that the trade is payable — the
row cost seven engine changes and zero project-side vocabulary.

### 2.4 Bazel and the platform tools agree on the last row

Bazel has platforms and toolchains in the core and registers them from
**external rulesets**: `rules_android`, `rules_apple`, `emsdk`. And the
packaging step is always the platform's own tool — Gradle produces the `.aab`,
Xcode the `.ipa`. Nobody reimplements those.

That is the `dist-*` family's justification, and it is unanimous across every
system surveyed: **artifact shape is never core.**

## 3. Where the current design is right

* **A closed target table with a tier column.** Matches Rust. The alternative
  (CMake) moves the cost onto every project.
* **`dist-*` in the plugins.** Matches everyone.
* **The payload in the index, with the licence deciding its tier.** Matches
  Zig's line and is the only one of the four that states the reason.
* **`has_own_sysroot()` as a target property read by the shared producer.**
  The wasm work found three independent readers of this question, and putting
  the answer in one place is what stopped the fourth from being missed.

## 4. Four things the design does not yet answer, and each is engine-side

These are not defects in what shipped. They are decisions the next two rows
force, and every one of them is in the engine rather than in a plugin — which
is itself the finding: **the plugin boundary is already in the right place;
what is unfinished is upstream of it.**

### 4.1 The iOS simulator cannot be spelled, and `Triple` has no field for it

mcpp's model is `Triple{arch, os, env}` and the canonical form drops the
vendor: `aarch64-ios`, against Rust's and LLVM's `aarch64-apple-ios`. Dropping
the vendor is defensible and consistent (`aarch64-macos` →
`aarch64-apple-darwin` in `llvm_triple()`), and the table's own comment
defends it.

But Rust spells the simulator `aarch64-apple-ios-sim`, and that trailing
`-sim` is a **fourth** component. mcpp's row comment says the simulator "is
deliberately not a row... folding it in would make two targets share an
identity" — which is right, and leaves the question open rather than answered:
with three fields and no vendor, there is no place for `sim` except `env`.

`env = "sim"` would work and reads oddly, because every other `env` value
names a C library or an ABI. The alternative is a fourth field, which touches
every triple in the system.

**iOS development without the simulator is not iOS development**, so this is
not deferrable to after the row lands. It should be decided before
`aarch64-ios` moves off `planned`, and the decision belongs in `triple.cppm`.

### 4.2 Android's API level has nowhere to live

The NDK's own target is `aarch64-linux-android21` — the API level is part of
the **LLVM triple**, not a flag. mcpp's row is `aarch64-linux-android` with no
version, and the level has to come from somewhere because it changes the ABI:
it selects which bionic symbols exist.

Three places it could go, and they are not equivalent:

* `env = "android21"` — puts it in the identity, so the output directory,
  `cfg(env = ...)` and the packed ABI tag all carry it. Correct, and it
  multiplies the table by every level anyone wants.
* a `[target.aarch64-linux-android] api = 24` manifest key — keeps the table
  small, and the level must then enter the **fingerprint** explicitly or two
  different ABIs share a build directory. That is the defect class this
  codebase has recorded most often.
* the row pins one level — simple, honest, and wrong for anyone shipping to a
  different minimum.

Rust has the same problem and answers it outside the triple. Whatever mcpp
picks, **the level must be in the fingerprint**, and that is an engine change
no plugin can make.

### 4.3 The family axis and the payload axis are conflated, and I hit it

`emsdk` normalises to `Family::Llvm`, because `em++` *is* clang and inventing a
fourth family value would be a claim about the compiler that is false. That is
the right conclusion about the compiler and it has a consequence I ran into
directly: `mcpp toolchain list` reports the payload as `llvm@6.0.9`, which is
indistinguishable from the real `xim:llvm` in any listing keyed on
`family@version` — and the target matrix's scan takes **one toolchain per
family**, so two llvm-family payloads on one host cannot both be enumerated.

The two axes are genuinely different questions:

    family   what flag vocabulary does this compiler speak?   -> llvm
    payload  which archive provides it?                       -> emsdk

`to_xim_package` already answers the second from the target. What is missing is
that nothing *displays* the second, so a user with emsdk installed sees `llvm
6.0.9` and a matrix cell cannot name it. The fix is a display/identity field on
the resolved toolchain, not a new family — and it is engine-side.

### 4.4 A non-MSVC toolchain cannot be located on the machine, which is the iOS tier-3 shape

`parse_toolchain_spec` refuses `@system` for every family but MSVC, by name,
with a written reason: Visual Studio is often already installed and cannot
always be redistributed, and that is a concession to one platform rather than
a general capability.

The iOS SDK is the second instance of exactly that situation, and
`xim:iphoneos-sdk`'s own header names "locate what the machine already has" as
the third of its three tiers. If the licence ever forces that tier — or if a
developer with Xcode installed simply prefers it — **the engine currently has
no spelling for it.** `aarch64-ios` at tier 3 is unreachable, and the refusal
that blocks it is one that argues from MSVC's uniqueness.

That argument is now weaker by one instance. Whether to generalise `@system`
or to add a second named exception is a decision; having neither is not.

## 5. The runner, the emulator and the simulator: where "how do I run this" belongs

This is the part the first draft of this document only flagged. It needs
designing, because two of the three platforms cannot be exercised by executing
a file.

### 5.1 Running an artefact has three shapes, not one

Measured across the three rows:

| shape | example | what it needs |
|---|---|---|
| **the artefact runs itself** | `wasm32-emscripten` | nothing. `em++` writes `#!/usr/bin/env node` and marks the file executable, and `node` on PATH is the xvm shim `xim:emsdk` already depends on |
| **a translator wraps it** | `aarch64-linux-android`, static or with `-L` | one argv prefix: `qemu-aarch64 -L <root>` plus an env var |
| **a session exercises it** | the Android emulator, the iOS simulator, a device | boot, transfer, execute remotely, collect the exit code and stdout, tear down |

mcpp's `runner` key is an **argv prefix**. It covers the first two shapes
exactly and **cannot express the third**, because the third is a stateful
lifecycle rather than a command.

### 5.2 The industry answer is unanimous, and it is cheaper than a new mechanism

Every system surveyed provides an argv-prefix hook in the core and puts the
device lifecycle outside it:

| system | the hook | shape |
|---|---|---|
| cargo | `target.<triple>.runner` | program + args |
| CTest | `CMAKE_CROSSCOMPILING_EMULATOR` | program + args |
| Bazel | `--run_under` | program + args |
| Gradle / Xcode | `connectedAndroidTest`, `xcodebuild -destination` | a session, owned by the platform's own tool |

Cargo's reference is explicit about the boundary: the runner is invoked "with
the actual executable passed as an argument", and managing devices, emulators
or simulators is **out of scope** — "that responsibility falls to the runner
program itself."

That sentence is the design. mcpp does not need a fourth member family, a
session protocol, or any engine change:

* `runner` stays an argv prefix. It already is, and it already matches three
  other build systems.
* **The program the prefix names is an ecosystem package.** A program that
  boots an AVD, pushes a binary, runs it under `adb shell`, collects the exit
  code and tears down is an ordinary executable — so it is a `xim:` package
  shipping a binary, exactly like `xim:qemu-user-aarch64` is today.

So the split is:

    engine    `runner` = argv prefix, and the row's default value
    ecosystem the program that prefix names, including any device lifecycle
    plugin    nothing -- a plugin is a build-time module, and a runner is a
              run-time program; putting it there would be a category error

### 5.3 Why a `dist-*`-style plugin is the wrong home, stated so it is not tried

A member in the `dist-*` family is a C++ module compiled into the build
program. It runs during the build and its output is a build-graph action. A
runner runs **after** the build, once per `mcpp run` or `mcpp test`, and has to
survive the build system exiting. The two have different lifetimes, and the
only thing they share is the word "platform".

The taxonomy already says this: `rules-*` is what goes into the compile,
`tools-*` what runs beside it, `dist-*` what comes out of the link. None of
those is "how the output is exercised", and the reason is that the answer is
not a module.

### 5.4 What each row's runner is, concretely

| row | `runner` default | provided by |
|---|---|---|
| `wasm32-emscripten` | **none** | nothing needed -- the shebang and `xim:node` |
| `x86_64-linux-android` | none for a static artefact on an x86_64 host; a session program for the dynamic default | `xim:android-platform-tools` + `xim:android-emulator` + `xim:android-system-image` |
| `aarch64-linux-android` | `qemu-aarch64 -L <qemu-user-root>` | `xim:qemu-user-aarch64` + the extracted root `xim:android-system-image` already produces |
| `aarch64-ios` | **none possible** | an artefact cannot run off an iOS device; the simulator needs `xcrun simctl` on a macOS host |

The aarch64 Android row is the interesting one: its runner is an argv prefix
plus one environment variable, which is exactly what the key expresses, and
every piece is already published. **It needs no new mechanism at all** —
which is why §4.2's identity decision is the only thing in front of it.

And the x86_64 Android row is the one that wants a session program, because
its DEFAULT configuration is dynamic and the emulator is what supplies a real
`linker64`. That program does not exist yet and is a package, not a feature.

### 5.5 The simulator is a target, not a runner, and that is the whole confusion

An iOS simulator build is **a different target**: its own SDK, its own object,
`x86_64`/`aarch64` host-native code rather than device code. Calling it "a way
to run the iOS target" is the category error the row's own comment already
warns about -- "folding it in would make two targets share an identity".

So the simulator needs a ROW, and §4.1's missing spelling is what blocks it.
With `env = "sim"` the two rows are `aarch64-ios` and `aarch64-ios-sim`, which
is Rust's `aarch64-apple-ios` / `aarch64-apple-ios-sim` pair modulo the vendor
elision mcpp already performs everywhere. Once the row exists, its runner is an
ordinary argv prefix over `xcrun simctl spawn`, on a macOS host, provided by a
package -- no new mechanism, again.

## 6. Recommendations, as decisions rather than options

| # | question | recommendation | why |
|---|---|---|---|
| R1 | the simulator's spelling | `env = "sim"`, giving `aarch64-ios-sim` and `x86_64-ios-sim` as their own rows | matches Rust's pair modulo a vendor elision mcpp already does; satisfies the row comment's own objection, which was to NOT having a separate row |
| R2 | Android's API level | a `[target.<triple>] api = <n>` manifest key, and **it must enter the build fingerprint** | every system surveyed keeps it out of the triple (Rust: outside; CMake: `ANDROID_PLATFORM`; Gradle: `minSdk`), because it is a per-PROJECT minimum. The fingerprint is non-negotiable: it selects which bionic symbols exist, so two levels are two ABIs |
| R3 | the payload identity shown for `emsdk` | a display identity on the resolved toolchain, not a fourth `Family` | `em++` is clang and a fourth family would be a false claim about the compiler; what is missing is only that nothing prints which archive answered |
| R4 | `@system` for a non-MSVC family | generalise it, with the row deciding whether it is permitted | the refusal argues from MSVC's uniqueness, and iOS is the second instance of exactly that situation. `xim:iphoneos-sdk` already names "locate what the machine has" as its third tier, and the engine has no spelling for it |
| R5 | device and simulator sessions | a `xim:` package shipping a runner program, named by `runner` | cargo states this boundary explicitly; no engine change, no new member family, and it puts platform knowledge in the ecosystem |
| R6 | the tier each row can reach | `verified` for wasm (reached); `preview` for both Android rows and for iOS | Rust rates all three Tier 2. `verified` for Android is reachable and needs a CI lane, not a design |

## 7. User-facing experience, which is the test of all of the above

The whole point of keeping identity in the engine is that the user types one
thing:

    mcpp build --target wasm32-emscripten          works today
    mcpp run   --target wasm32-emscripten          works today
    mcpp build --target aarch64-linux-android      after R2
    mcpp run   --target aarch64-linux-android      after R2 + R5's package
    mcpp build --target aarch64-ios-sim            after R1
    mcpp pack  --target aarch64-ios --format app   after R1, R4 and dist-apple

One verb per intent, the target as a flag, and no per-platform mode. Compare
what the alternatives ask of a user:

* **CMake** — a different toolchain file per platform, each with its own
  variable vocabulary (`EMSCRIPTEN`, `ANDROID_ABI`, `CMAKE_OSX_SYSROOT`).
* **Gradle / Xcode** — a separate task graph and project model per platform.
* **Bazel** — a ruleset per platform, loaded in `WORKSPACE`.

mcpp's cost for that uniformity is that a platform is an engine change. The
wasm row is the first real measurement of that cost: **seven engine changes,
and zero new vocabulary for the project.** A project that builds for Linux
builds for the web by changing one flag. That is the argument for the design,
and it is now measured rather than asserted.

## 8. The rule, stated so the next platform does not need this document

Three questions, and the answer to each is the same for every platform:

1. **What is this target?** — engine. A package cannot add a row, because
   every layer above attaches to one: the output directory, `cfg()`, the ABI
   tag, the fingerprint, the runner. Rust and Zig both keep this closed and in
   the core.

2. **Which compiler serves it, where does it live, and what must it be told?**
   — engine, because these are properties of the **target**, not of any
   package. The wasm row needed seven such answers and every one of them was a
   predicate that already existed and was right about the rows its author had
   in mind. A plugin cannot fix `resolve_link_model`.

3. **What file does the user ship?** — plugin. Unanimous across CMake, Bazel,
   Gradle, Xcode and Emscripten: the format's own tool owns the format.

And one corollary, which is the boundary's real test: **if a question's answer
differs per project, it is a manifest key; if it differs per target, it is
engine; if it differs per artifact shape, it is a plugin.** The Android API
level is the interesting case, because it looks like the first and behaves like
the second.

## 6. What this implies for the three rows

| row | engine work remaining | plugin work | tier it can honestly reach |
|---|---|---|---|
| `wasm32-emscripten` | the `.wasm` sibling as an implicit link output | an `.html` shell, `--preload-file` data staging, a dev server | **`verified`** -- reached |
| `x86_64-linux-android` | the API-level decision (§4.2) | `.apk` / `.aab` assembly, which is Gradle's tool | `preview`, `verified` if a CI lane runs the emulator |
| `aarch64-linux-android` | the same, plus nothing else -- route A executes it | the same | `preview`; `verified` needs the qemu-user runner wired |
| `aarch64-ios` | the simulator spelling (§4.1) and possibly `@system` (§4.4) | `.app` (exists) and `.ipa`, which is Xcode's tool | `preview` -- there is no way to run an iOS artefact off a device |

The two Android rows are the nearest, and the thing blocking them is not
payload work — both payloads are published and both execution routes are
measured. It is one identity decision.
