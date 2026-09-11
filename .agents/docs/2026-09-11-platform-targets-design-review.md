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
| R7 | signing a Mach-O or a `.app` | package `rcodesign` as `xim:rcodesign` and have `dist-apple` prefer it | MPL-2.0 with prebuilt static binaries for linux-musl (both arches), macOS universal and Windows. It removes the last host dependency from the iOS BUILD path, leaving only a device, the Simulator runtime and a notarization credential -- none of which is a program |
| R8 | the four-field spelling | `parse()` should accept `wasm32-unknown-emscripten` and `aarch64-apple-ios` and canonicalise them | the industry writes four fields; refusing the spelling every other toolchain prints is a UX cost with no design benefit, and `parse()` already normalises several aliases |
| R9 | `mcpp pack --format ipa` | a `dist-ipa` member: zip `Payload/<Name>.app/`, after `dist-apple` and `rcodesign` | it needs NO new tool. Every other link is already in the ecosystem or one member away, so iOS PACKAGING closes entirely -- what does not close is the credential and the runtime |
| R10 | `--format dmg` and `--format pkg` | recorded as gaps with a known shape, not attempted | each needs a *creator* as well as a signer (`libdmg-hfsplus`; `xar`), both open source and neither measured here |
| R11 | the macOS rows' runner | Darling recorded as an unmeasured candidate | GPL-3.0, active, and it REIMPLEMENTS Darwin's libraries rather than redistributing them, so unlike the iOS image it carries no licence blocker. A row does not move on a plausible mechanism, so this is a candidate and not a plan |
| R12 | real-device run for both platforms | `xim:android-platform-tools` (have) and a new `xim:pymobiledevice3`, each named by a `runner` program | neither needs Apple or Google software. It supersedes the simulator route rather than complementing it: a device brings its own OS, so the only thing crossing the boundary is a signature the developer already owns |
| R13 | the iOS image | a LOCATOR package, never a re-host, gated on R4 | an image in a public index is redistribution of Apple's OS whatever it is labelled. The locator is the tier `iphoneos-sdk.lua` already documents, and with R12 in place no image is on the critical path at all |

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

## 8. Is `wasm32-emscripten` a standard name, and a common one?

Two different questions, and the answers differ.

### 8.1 The industry name is the four-field one, and mcpp's is a normalisation

Measured, not recalled -- `em++ -v` on this machine passes to its own clang:

    -target wasm32-unknown-emscripten

and `rustc`'s platform table lists the same spelling at Tier 2 with host
tools. That is the industry name.

mcpp writes `wasm32-emscripten`, and `llvm_triple()` restores the vendor
(`triple.cppm:172` emits `arch + "-unknown-emscripten"`). So the short form is
**mcpp's canonical spelling, not a spelling anyone else uses** -- and it is the
same elision mcpp already performs everywhere: `aarch64-macos` becomes
`aarch64-apple-darwin`, `x86_64-windows-gnu` becomes `x86_64-w64-windows-gnu`.

That is defensible and should be stated as what it is. mcpp's `Triple` has
three fields and no vendor, deliberately, and `unknown` is a placeholder that
carries no information for any target in the table. A user who types the
four-field form should still be understood, which is a `parse()` question
rather than a naming one.

### 8.2 The wasm family is nine targets, and the three-field model holds

`rustc`'s table lists nine:

    wasm32-unknown-emscripten     Tier 2 with host tools
    wasm32-unknown-unknown        Tier 2 with host tools
    wasm32-wasip1                 Tier 2 with host tools
    wasm32-wasip1-threads         Tier 2 with host tools
    wasm32-wasip2                 Tier 2 with host tools
    wasm32v1-none                 Tier 2 without host tools
    wasm64-unknown-unknown        Tier 3
    wasm32-wali-linux-musl        Tier 3
    wasm32-wasip3                 Tier 3

Mapped onto `Triple{arch, os, env}`:

| Rust | mcpp | field use |
|---|---|---|
| `wasm32-unknown-emscripten` | `wasm32-emscripten` | os = emscripten |
| `wasm32-unknown-unknown` | `wasm32-none` | os = none, i.e. `is_freestanding()` |
| `wasm32-wasip1` | `wasm32-wasi` + env | os = wasi |
| `wasm32-wasip1-threads` | env = `p1-threads` | **env absorbs Rust's fourth component** |
| `wasm64-unknown-unknown` | `wasm64-none` | arch = wasm64 |

The last row of that table is the important one, and it settles §4.1 from an
unexpected direction. Rust appends a fourth component for a *variant*:
`-threads` here, `-sim` for the iOS simulator. mcpp has exactly one slot for
it, `env`, and the wasm family shows that slot is adequate and already used
that way by every other row (`gnu`, `musl`, `eabihf`). So **`env = "sim"` is
not a workaround; it is the field doing its job**, and R1 is a use of the model
rather than a stretch of it.

## 9. Is the toolchain bound to the SDK? Three platforms, two answers

This is the question that most changes how a row is written, and the three
platforms do not agree.

| platform | compiler | its system | one archive? | mcpp's columns |
|---|---|---|---|---|
| Emscripten | `em++` (clang) | `<payload>/emscripten/cache/sysroot` | **yes** | `pin = emsdk@6.0.9`, `sysroot` empty |
| Android | NDK's `clang++` | bionic, in `toolchains/llvm/prebuilt/<host>/sysroot` | **yes** | `pin = android-ndk@V`, `sysroot` empty |
| iOS | **any sufficiently new clang** | the iPhoneOS SDK, reached with `-isysroot` | **no** | `pin = llvm@V`, `sysroot = xim:iphoneos-sdk@V` |

So `has_own_sysroot()` is not an arbitrary set of two: it is exactly the
platforms whose compiler and system arrive as one payload, and that is why the
predicate reads the way it does.

**And iOS is structurally the same shape as bare metal.** `riscv64-none-elf`
pins `llvm@22.1.8` and names `xim:picolibc-riscv@1.8.12` in the `sysroot`
column -- a generic clang plus a separately-versioned system. iOS is that
shape with a different sysroot package. The `sysroot` column already exists
for precisely this, which means the iOS row needs **no new table machinery**,
only the two columns filled.

The consequence for the other two is the opposite: a `sysroot` entry for
Emscripten or Android would be wrong, because the driver resolves its own and a
second answer competes with it -- which is the defect the wasm row hit three
times.

### 9.1 "The Android SDK" names two unrelated things, and the packages split on that

Worth stating because the naming misleads:

* the **NDK** is the compiler and bionic -- one archive, bound, used at BUILD
  time. `xim:android-ndk`.
* the **SDK** proper is `platform-tools` (adb, fastboot), the emulator, system
  images and build-tools -- used at RUN and PACKAGE time, and unbound both from
  each other and from the compiler. `xim:android-platform-tools`,
  `xim:android-emulator`, `xim:android-system-image`.

Four packages rather than one is therefore not a decomposition choice; it is
what upstream actually ships, and each is independently versioned by Google.

## 10. What can close inside the ecosystem, and what cannot

The preference is stated: close the loop inside xlings, and reach the host only
where nothing else is possible. Enumerated per platform rather than argued.

### 10.1 Closed today, measured

| need | package | evidence |
|---|---|---|
| `em++`, the wasm sysroot and its module surface | `xim:emsdk` | `mcpp run --target wasm32-emscripten` printed `1-2-3` |
| the interpreter `em++` execs | `xim:python` | needed an aarch64 payload added; declared as a dep |
| the JS engine the artefact needs | `xim:node` | the artefact's own `#!/usr/bin/env node` resolves the xvm shim |
| the Android compiler and bionic | `xim:android-ndk` | `import std` built for both Android arches |
| an aarch64 loader and a real bionic | `xim:android-system-image` | `debugfs` extraction, then `qemu-aarch64 -L` ran the DYNAMIC artefact |
| the user-mode translator | `xim:qemu-user-aarch64` | same measurement |
| the ext4 reader that extraction needs | `xim:e2fsprogs` | declared; was a host probe |
| `adb` / `fastboot` | `xim:android-platform-tools` | installs on all three hosts |
| the emulator and its X11 chain | `xim:android-emulator` + six existing libs | declared; was a host probe |

### 10.2 Closeable, and one of them is a finding

| need | how | status |
|---|---|---|
| a clang that targets iOS | `xim:llvm` plus `-isysroot` | the payload exists; the row is unfilled |
| the iPhoneOS SDK | `xim:iphoneos-sdk`, at whichever of three licence tiers applies | exists |
| **signing a Mach-O, a `.app`, a `.dmg` or a `.pkg`** | **`rcodesign`** (crate `apple-codesign`, MPL-2.0) | **not yet packaged, and it should be** |
| finding the SDK path | nothing -- `xcrun` is a path-finder and `-isysroot <path>` needs none | no dependency |

The third row changes the iOS picture. `apple-codesign` states its goal as
being "a stand-in replacement for Apple's `codesign` ... without a dependency
on an Apple hardware device or operating system", covering Mach-O binaries,
`.app` bundles, `.pkg` installers and `.dmg` images. Release 0.29.0 ships
**prebuilt static binaries for `x86_64-unknown-linux-musl`,
`aarch64-unknown-linux-musl`, macOS universal and Windows**, under MPL-2.0.

So signing -- which `dist/apple.cppm` currently reaches through the host's
`codesign` -- **is not a host dependency at all**. It is an unpackaged one.

Sources: [apple-codesign on crates.io](https://crates.io/crates/apple-codesign)
and its [documentation](https://gregoryszorc.com/docs/apple-codesign/stable/).

### 10.3 Signing belongs in the ecosystem AND in the plugin system, in that order

`rcodesign` is a program, so it is a `xim:` package; what it is invoked BY is a
`dist-*` member; and what the user types is `mcpp pack --format <name>`. All
three layers already exist, which is why this needs no new mechanism -- only
the package and the members.

    xim:rcodesign            the program            (MPL-2.0, prebuilt static)
    dist-apple  --format app   the bundle           exists, uses the host's codesign today
    dist-ipa    --format ipa   the shippable file   does not exist
    dist-dmg    --format dmg   a disk image         does not exist
    dist-pkg    --format pkg   an installer         does not exist

Release 0.29.0 signs **bundles**, not only flat Mach-O binaries -- its
changelog discusses `--shallow` bundle mode and child-bundle signing "compatible
with the behavior of Apple's `codesign`" -- which is exactly what a `.app`
inside an `.ipa` needs.

**`--format ipa` is the one that needs no new tool at all.** An `.ipa` is a zip
containing `Payload/<Name>.app/`, so the chain is: clang plus the iPhoneOS SDK
produce the Mach-O (both `xim:`), `dist-apple` assembles the bundle, `rcodesign`
signs it, and a zip step produces the file. Every link is already in the
ecosystem or is one member away.

`--format dmg` and `--format pkg` each need a *creator* as well as a signer,
and neither creator is packaged: a `.dmg` is an HFS+/APFS image (Apple's
`hdiutil`, or `libdmg-hfsplus` off macOS) and a `.pkg` is an XAR archive
(Apple's `pkgbuild`, or `xar`). Both alternatives are open source and neither
has been measured here, so they are named as gaps with a known shape rather
than claimed.

### 10.4 The iOS runtime: the blocker is a licensed IMAGE, not a missing emulator

This is the question worth getting exactly right, because the obvious answer is
wrong in an instructive way.

QEMU can emulate ARM iOS hardware, and community projects exist that boot iOS
on it. What none of them can supply is the **iOS kernel and root filesystem**:
distributing iOS images is against Apple's terms, so an image must be one the
user already legally owns. The emulator is not the scarce thing.

That is the same shape as the Android question, with the opposite answer, and
the comparison is the point:

| | the emulator | the OS image | can the loop close? |
|---|---|---|---|
| Android | Apache-2.0, and `emulator/LICENSE` says so | AOSP `default` builds, OSS notices throughout | **yes** -- both are packaged, and `qemu-aarch64 -L` needs no emulator at all |
| iOS | QEMU, GPL, packageable | **not redistributable** | **no** -- and no amount of tooling changes it |

So `aarch64-ios` cannot reach `verified` for a reason that is not about mcpp,
xlings, or effort. It is the one row in the table whose execution is blocked by
a licence rather than by work, and saying so precisely is better than leaving
it as "needs a device".

Sources: [iOS emulators, Emulation General Wiki](https://emulation.gametechwiki.com/index.php/IOS_emulators)
and [Emulating iOS on Linux](https://linuxvox.com/blog/emulate-ios-on-linux/).

**On macOS, the Simulator is the host's and that is fine.** It ships with Xcode,
it is a (b)-category proprietary runtime, and `xcrun simctl spawn` is an argv
prefix -- so §5's model covers it with no new mechanism, on a macOS host, once
R1 gives the simulator a row.

### 10.5 Real devices close for BOTH platforms, and that is a better answer than an emulator

Asked directly, and it turns out to be the strongest result in this section:
**running on real hardware needs no Apple or Google software on either
platform.**

| platform | what installs and launches | licence | state |
|---|---|---|---|
| Android | `adb push` + `adb shell` | Apache-2.0 | **already packaged** -- `xim:android-platform-tools`, installs on all three hosts |
| iOS | [`pymobiledevice3`](https://github.com/doronz88/pymobiledevice3) | GPL-3.0 | pure Python 3, no compiled extensions, Linux / Windows / macOS; 2736 stars, last push 2026-09-10 |
| iOS | [`libimobiledevice`](https://github.com/libimobiledevice/libimobiledevice) | LGPL-2.1 | the C library it was modelled on; 8177 stars, last push 2026-06-10 |

`pymobiledevice3` describes itself as requiring no Xcode, working with the
system `usbmuxd`, and covering app management plus iOS 17+ developer tooling
over a tunnel. `libimobiledevice` states it needs no jailbreak. Neither
requires a Mac.

So the iOS row's execution story is not "needs a device on a Mac". It is:

    build        xim:llvm + xim:iphoneos-sdk                    closed
    bundle       dist-apple  (--format app)                     closed
    sign         xim:rcodesign                                   closeable, R7
    package      dist-ipa    (--format ipa)                      closeable, R9
    deploy+run   xim:pymobiledevice3                             closeable, R12
    ------------------------------------------------------------------------
    entitlement  a provisioning profile and a signing identity   NOT closeable

**Only the last line does not close, and it is a credential rather than a
tool.** Installing on a non-jailbroken device requires an Apple Developer
provisioning profile; `pymobiledevice3` can install a signed `.ipa` and cannot
conjure the entitlement. That is category (c), and no package manager closes a
credential -- which is the same boundary a developer already lives with when
using Xcode.

That is a materially better position than the simulator route, and it is worth
stating why: the simulator is blocked by a **licensed image** that cannot be
redistributed, while a real device supplies its own OS and the only thing
crossing the boundary is a signature the developer already owns.

### 10.6 Where a device session lives: the runner program absorbs deployment

§5 concluded that a device session is a runner PROGRAM in a `xim:` package,
named by an argv-prefix `runner`. Deployment does not need a fourth verb, and
the reason is that `adb push && adb shell` is one operation from mcpp's side:

    mcpp run --target aarch64-linux-android
      -> runner = ["mcpp-android-device-run"]     a xim package's program
         which pushes, executes, collects the exit code and stdout, tears down

    mcpp run --target aarch64-ios
      -> runner = ["mcpp-ios-device-run"]         a xim package's program
         which installs the signed .ipa, launches it, streams the log, collects

So both mechanisms are used, each for what it is:

    plugin   produces the artefact        dist-ipa, dist-apple  (build time)
    package  deploys and runs it          the runner program    (run time)
    engine   names the runner             the `runner` key      (already exists)

And the ordering is a real dependency rather than a convention: the iOS device
runner has nothing to install until `dist-ipa` has produced a signed file, so
R9 precedes R12.

### 10.7 The iOS image: a locator, not a re-host

Adding an iOS kernel and root filesystem to a public index would be
redistributing Apple's operating system, and a "temporary, test-only, disabled
later" label does not change that -- anyone resolving the index would install
it. This differs from the Android decision earlier in this document in a way
worth stating precisely: there, Apache-2.0 licence files were verified INSIDE
the archives and clause 3.5 genuinely applies; here there is no
open-source component to invoke.

What serves the same purpose legitimately is the third tier
`pkgs/i/iphoneos-sdk.lua` already documents, and R4's `@system`
generalisation is the engine half of it:

    a LOCATOR package records where an image the user already owns lives.
    Nothing is re-hosted; the index carries a path and a probe, not bytes.

That is the `msvc@system` shape, and it is why R4 matters beyond iOS: the
engine currently has no spelling for "this row's system is host-located",
so the locator tier is unreachable even though the recipe describes it.

And it is worth noting what the locator would be FOR. With R12 in place, a
simulator or an emulated image is not on the critical path at all -- a real
device is the supported route, and it needs no image from anyone.

### 10.8 Darling is a candidate for the macOS rows, and is recorded as unmeasured

[Darling](https://github.com/darlinghq/darling) is a macOS compatibility layer
for Linux -- GPL-3.0, actively developed (last push 2026-09-06). It reimplements
Darwin's system libraries rather than redistributing them, so it carries **no
Apple licence blocker**, which makes it categorically different from the iOS
image problem above.

It runs macOS binaries, not iOS ones, so it is irrelevant to `aarch64-ios` and
potentially relevant to `x86_64-macos` and `aarch64-macos` -- the first of which
is `planned` in this table with no host able to serve it off an Apple machine.

Recorded as a candidate and explicitly **not** as a plan: nothing in this
ecosystem has run it, its coverage is partial by construction, and a row does
not move on a plausible mechanism. What it would be, if it worked, is an
ordinary `runner` argv prefix supplied by a `xim:` package -- the same shape as
`qemu-user-aarch64`.

### 10.9 Genuinely host-bound, and there are exactly three

1. **`/dev/kvm`** -- a kernel facility. No package ships a kernel feature, and
   group membership is a machine's configuration. This is why it is the only
   `log.warn` left in `android-emulator.lua`.
2. **A real device, Apple's Simulator runtime, or a licensed OS image.** The
   simulator is a proprietary runtime that exists only on macOS; a device is
   hardware; and an iOS kernel plus root filesystem cannot be redistributed at
   all. Note which of the three is the actual blocker for emulation: QEMU is
   packageable and the IMAGE is not, which is precisely why the same mechanism
   closes for Android -- where the image is AOSP -- and cannot for iOS.
3. **Notarization.** Apple's servers plus a developer credential. A credential
   is never a package, and `rcodesign` can drive the submission but cannot
   supply the account.

### 10.10 The rule that falls out

    A host dependency is legitimate only when the thing needed is
      (a) a kernel facility,
      (b) a proprietary RUNTIME that exists only on its own OS, or
      (c) a credential.
    Anything that is a PROGRAM can be packaged, and the survey found that
    even Apple's signing tool has a redistributable replacement.

That test is worth having because it is falsifiable, and it immediately
reclassifies two things this ecosystem had treated as host dependencies:
`debugfs` and the libX11 chain were (a)-shaped in the recipes' prose and were
in fact just programs. `codesign` is the third instance of the same mistake,
and this document is the first place it is named.

It also narrows R4. `msvc@system` is a (b): Visual Studio is a proprietary
toolchain that exists only where it is installed. Generalising `@system`
should therefore mean "a row may declare that its system is host-located
because it is (b)", not "any family may be located on the host" -- the
existing refusal is right about the general case and wrong only about
believing MSVC is the sole instance.

## 11. The rule, stated so the next platform does not need this document

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
