---
subject: targets
status: landed
---

# SDK toolchains, the payload/engine seam, and openkal across iOS, Android and Web

**Status:** implemented, in mcpp #612 (2026.9.11.4), xim-pkgindex #813 and
#820, openkal #29 and #30, mcpp-index #393, and the new repository
`mcpplibs/openkal-emscripten`. The design sections are kept as they were
reviewed; where the implementation departed from them, the sections from "What
the implementation found" onward record why.

**Scope.** Five items, in dependency order. The first is measured and only needs
writing down; the second removes something rather than adding; the third is the
only structural change in the engine; the fourth adds a platform; the fifth
closes the same three platforms for openkal, where one is already done, one is
a reuse and one is new software.

| | item | shape |
|---|---|---|
| A | `aarch64-linux-android` becomes `verified`, and the extraction tool changes | evidence exists, two edits |
| B | the `ndk` toolchain alias is withdrawn | deletion |
| C | a payload describes itself, so the engine stops knowing the NDK | new contract at an existing seam |
| D | iOS builds and runs on the simulator, locally and in CI | one parameter, one row column, one runner package |
| E | openkal's cross-build closes on all three platforms, with examples and docs | one shared, one located, one new implementation |

## A. Android's device row has an execution path after all

`aarch64-linux-android` was recorded `preview` on the grounds that no execution
path exists from an x86_64 host. That was wrong, and the reason it looked right
is worth stating: the route the earlier design named was correct, and only its
EXTRACTION TOOL was broken.

Measured 2026-09-11:

```
7zz l  <system.img>          Type = Ext ; bin/linker64  1120256 bytes
7zz x  <system.img> …        5 files extracted
qemu-aarch64-static -L <root>  <the mcpp-built aarch64 artifact>
                             ->  1-2-3      exit 0
```

`xim:e2fsprogs@1.47.3`'s `debugfs` SIGFPEs on every filesystem-opening command,
including against a control image made by that same payload's `mke2fs`, while
`dumpe2fs`, `e2fsck` and `tune2fs` from the same archive work. `xim:7zip` reads
ext4 directly and is already in the index.

**Two consequences.** The row becomes `verified`, with the vehicle recorded:
the platform's own emulator for `x86_64-linux-android`, qemu-user plus the
system image's bionic for `aarch64-linux-android`. And
`pkgs/a/android-system-image.lua` takes its four files with `xim:7zip` instead
of `xim:e2fsprogs`, which makes the arm64 key installable for anyone rather
than only where a working host `debugfs` happens to exist.

The e2fsprogs defect stays recorded where it is. It is not this change's to
fix, and nothing else in the index depends on that program.

## B. The toolchain axis already has the semantics asked of it

The question was whether an SDK toolchain behaves like any other: a default
that needs no declaration, and a per-target override in `mcpp.toml`. Measured,
it does:

```
no declaration                        -> Resolved emsdk@6.0.9        (auto-installed)
toolchain = "emsdk@6.0.9"             -> Resolved emsdk@6.0.9
toolchain = "emsdk@5.0.0"             -> resolves, install fails: not in the index
toolchain = "android-ndk@30.0.16248370" -> Resolved android-ndk@…
toolchain = "llvm@22.1.8"             -> refused: capability pin
```

So the default is the row's pin, the override is the ordinary
`[target.<triple>] toolchain` key, the **version** is free within what the
index publishes, and the **payload name** is fixed for a capability row. That
is the same shape every other toolchain has, and nothing needs to change.

**What does change is a half-supported spelling.** `compat.cppm` accepts `ndk`
as an alias for `android-ndk`, and the capability gate compares the declared
spelling against the pin's own name -- so `toolchain = "ndk@30.0.16248370"`
parses and is then refused. A spelling the parser accepts and the gate rejects
is worse than one spelling: it reads as a defect at the point of use.

**Withdraw the alias** rather than teach the gate to normalise. One payload,
one name. The gate's comparison is then correct by construction instead of
correct by a second mechanism, and there is one string in the ecosystem for
this payload -- the one the index uses.

## C. The payload describes itself

### The coupling, named

Three NDK-specific facts live in the general engine today:

| fact | site | what it encodes |
|---|---|---|
| `toolchains/llvm/prebuilt/<host>/bin` | `registry.cppm` `frontendSubdir` + `ndk_host_tag()` | the NDK's internal directory layout, per host |
| the API floor | `registry.cppm` `ndk_min_api_level()` | that the floor lives in `meta/platforms.json`, and that file's schema |
| `-D__BIONIC_CTYPE_INLINE=` | `prepare.cppm` | that this libc++ module surface needs it against this bionic |

Each is a fact the **installing recipe already knows**: `pkgs/a/android-ndk.lua`
computes `host_tag()` for its own probes, reads the payload to check its libc++
version, and applies the same define in its own self-test. The engine
re-derives all three, which is why adding a second such SDK means editing the
engine rather than publishing a package.

### The contract

The recipe writes one file beside the payload at install time; the engine reads
it if present.

```
<payload root>/.mcpp-toolchain.json
{
  "schema": 1,
  "frontend": "toolchains/llvm/prebuilt/linux-x86_64/bin/clang++",
  "platform_floor": "21",
  "std_module_defines": ["__BIONIC_CTYPE_INLINE="]
}
```

- **`frontend`** is a path relative to the payload root, already host-resolved
  by the recipe. The engine stops computing a host tag, and `frontendSubdir`
  becomes the fallback for a payload that ships no descriptor.
- **`platform_floor`** is the string `llvm_triple(param)` already takes. It is
  the payload's answer, not a constant compiled in -- which is the property
  `ndk_min_api_level()` was written to get and this generalises.
- **`std_module_defines`** reach the std module's own command assembly, which
  is a separate channel from the compile flags. They enter the build
  fingerprint, because they change what the module compiles to.

### Three properties this has to have

**Absence is compatibility, not silence.** No descriptor means today's
behaviour exactly, so a released payload keeps working and there is no flag
day. A descriptor that is PRESENT AND MALFORMED is refused, naming the file --
otherwise a typo reads as "an older payload" and the engine silently uses the
hardcoded path for a layout that has moved.

**It is not a general flag channel.** Three keys, each answering a question the
engine already asks. A payload cannot inject arbitrary compile flags: that is
`[build]` in a manifest, which is the project's to write, and a payload that
could would be a package changing a build it does not own.

**The descriptor is the recipe's output, not a file in the archive.** Upstream
does not ship it and should not have to. The recipe writes it, which also means
the recipe's tests can assert its content -- the same place the layout facts
are already asserted.

### What this does not do

It does not make the NDK a fourth compiler family. `android-ndk` and `emsdk`
normalise to `Family::Llvm` because their compiler IS clang; the family answers
"which flag vocabulary does this compiler speak" and the payload name answers
"which archive provides it". Two axes, and this change touches neither.

## D. iOS: an ecosystem compiler and a located SDK

### The compiler is ours; only the SDK is Apple's

This is the question that shrinks the whole item. iOS does NOT need Xcode's
clang. It needs:

| | comes from | why |
|---|---|---|
| the compiler | `xim:llvm` | any sufficiently new clang emits arm64 Mach-O for an iOS deployment target |
| the C++ runtime | the payload's libc++ | as on every other Apple row |
| the **SDK** | the machine's Xcode | headers and stub libraries, not redistributable |
| running on a simulator | the machine's `simctl` | a proprietary runtime that exists only on its own OS |

`aarch64-macos` is `verified` today on exactly this split, which is the
precedent: `xim:llvm` compiles, and the macOS SDK is located.

### The mechanism exists

`modules/platform/src/macos/macos.cppm` already has

```cpp
std::optional<std::filesystem::path> sdk_path();   // xcrun --show-sdk-path
```

and it already tries `xcrun --sdk macosx --show-sdk-path` as its second form.
So the change is a **parameter**, not a concept:

```cpp
std::optional<std::filesystem::path> sdk_path(std::string_view sdk = "macosx");
//                                                  "iphoneos" | "iphonesimulator"
```

Three existing callers are unchanged by the default argument.

### What `@system` means, and why it is not needed here

`@system` marks a toolchain mcpp LOCATES rather than installs -- `msvc@system`
is the one instance, because MSVC is a compiler that cannot be redistributed.
`parse_toolchain_spec` refuses `@system` for every other family by name, and
that refusal is right: admitting `gcc@system` would cost hermeticity for no
reason, since gcc is packaged.

The earlier design record proposed generalising it (R4) and then withdrew it.
**It stays withdrawn, and this item does not revive it.** The thing being
located for iOS is not a compiler, it is a **sysroot** -- and
`TargetInfo::sysroot` plus `[target.<triple>].sysroot` are an axis that already
exists. So:

```
row  aarch64-ios       pin = llvm@22.1.8   sysroot = <located iphoneos SDK>
row  aarch64-ios-sim   pin = llvm@22.1.8   sysroot = <located iphonesimulator SDK>
row  x86_64-ios-sim    pin = llvm@22.1.8   sysroot = <located iphonesimulator SDK>
```

A locator on the sysroot axis cannot admit `gcc@system`, because it says
nothing about compilers. That is why this is the narrow change and R4 was not.

### Deployment target

The `-m…-version-min` flag differs between the two, which is the second reason
the simulator is its own row rather than a flag on the device row:

```
aarch64-ios       -miphoneos-version-min=<floor>
*-ios-sim         -mios-simulator-version-min=<floor>
```

The floor is the project's, on the axis that already carries
`macos_deployment_target` and `min_api_level` -- one slot in the fingerprint,
because a target is Apple or Android and never both.

### Running it

The engine learns nothing about simulators. `runner` is an argv prefix and the
session belongs to a package:

```toml
[target.aarch64-ios-sim]
runner = ["simctl-run"]
```

`simctl-run` is a program from a new `xim:apple-simulator-tools`, and it owns
everything simulator-shaped: a bare Mach-O executable cannot be launched by
`simctl`, so the program wraps it in a minimal bundle, boots or reuses a
device, installs, launches, collects stdout and the exit code, and tears down.
That is the R5 boundary -- platform knowledge in the ecosystem -- and it is
also why the runner cannot be `xcrun simctl spawn` written into a manifest: a
manifest cannot express the bundle.

The device row keeps `runner` unset. An artefact cannot be run off an iOS
device without a signature the developer owns, which is R12's separate subject.

### The host surface, bounded and named

This item adds exactly two host dependencies, both on macOS only:

| what | why it is permitted |
|---|---|
| the iPhoneOS / iPhoneSimulator SDK, via `xcrun` | a proprietary runtime that exists only on its own OS -- category (b) of the recorded policy |
| `simctl`, via the runner program | the same category |

Everything else is ecosystem: compiler, C++ runtime, linker, packaging. The
recorded rule is that a host surface must be minimal, named and written down,
and two named items on one host is the whole of it. Neither is a fallthrough:
each is reached deliberately, and its absence is an error that names it.

## E. openkal across the three platforms

openkal is the other half of "a platform is supported": the target rows say a
COMPILER can emit for a platform, and openkal says a program written against
one interface can RUN there. The three platforms are three different answers,
and the spec's own capability model is what makes the third one legitimate
rather than a compromise.

### E1. Android shares the Linux implementation — done

`openkal-linux` is written on the Linux kernel's system-call interface and
borrows nothing from any C library, which is what lets it sit beneath one.
Android's kernel IS Linux, the per-architecture syscall ABI is identical, and
`src/sys.h` branches on `__x86_64__` / `__aarch64__` — the architecture, not
the operating system.

So a portable program needs no new line: `cfg(os = "linux")` is true for an
Android triple, because Android is an `env` value on a `linux` OS.

Measured and merged (`openkal-linux#26`), against the RELEASED
`mcpp 2026.9.11.3` rather than a working tree:

```
== aarch64-linux-android ==   17 objects, ARM aarch64
== x86_64-linux-android ==    17 objects, x86-64
both Android ABIs build from this implementation unchanged, and name no C library symbol
```

and a program written against openkal alone — no C library, no `import std` —
printed `openkal: 1-2-3` with exit 0 on an API 24 emulator image.

The symbol check is the half worth having. Compiling is the weaker statement;
the property this package exists for is that its objects name no C library
symbol, and on Android the C library is a DIFFERENT one, so a reference that
resolved to glibc by habit would appear as a bionic name. Two compiler-runtime
names are permitted with their reason recorded, both measured to be defined in
the NDK's `libclang_rt.builtins-<arch>-android.a` and in neither bionic
`libc.so`.

### E2. iOS reuses the macOS implementation

The same argument as Android, on Apple's side: iOS and macOS share the Darwin
kernel, and `openkal-macos` is arch-dispatched the same way. What blocked it
was never the implementation — it was that there was nothing to build against,
and item D is what supplies that.

So E2 is a consequence of D rather than new work: with `sdk_path("iphoneos")`
and `sdk_path("iphonesimulator")` resolving, `openkal-macos` compiles for the
three iOS rows, and a `cfg` selection picks it up. The selection needs one
decision: `cfg(os = "ios")` is its own OS value, so a portable program's
`cfg(os = "macos")` line does NOT cover iOS the way `cfg(os = "linux")` covers
Android. The two are not symmetric, and the reason is the modelling decision
itself — Android is an `env` on `linux`, iOS is its own `os`.

That asymmetry is correct and should not be papered over. A program targeting
both Apple platforms writes:

```toml
[target.'cfg(os = "macos")'.dependencies]
openkal-macos = "0.7.0"

[target.'cfg(os = "ios")'.dependencies]
openkal-macos = "0.7.0"
```

An alternative was considered and rejected: making `cfg(os = "macos")` true for
iOS, or adding a `cfg(apple)` predicate. `Triple::is_apple()` exists in the
engine for exactly this question, so a `cfg(apple)` dimension is expressible —
but it would be a NEW cfg dimension introduced for one consumer's convenience,
and the honest reading is that two OS values are two dependency lines. If a
third Apple platform arrives the case can be revisited with three consumers
rather than one.

### E3. Web needs a new implementation, above a C library

Emscripten is the one platform that changes the model. There is no kernel and
there are no system calls to issue: Emscripten supplies its own C library over
a JavaScript host. So an implementation cannot be written the way
`openkal-linux` is — BENEATH a C library — and must sit ABOVE one, which the
specification explicitly permits: "an implementation may be built upon a C
library, beneath one, or without one."

`openkal-emscripten` is therefore new software rather than a sharing decision.
It is also the SMALLEST of the three implementations, because forwarding to a
POSIX-shaped libc is thinner than issuing syscalls: the Linux implementation is
3918 lines and the macOS one 3512, most of which is calling-convention and
kernel-structure detail that an above-libc implementation does not have.

**The partial surface is legitimate, and the spec says how.** Clause 6.2 gives
three times, each the earliest at which the information exists:

| time | mechanism | question |
|---|---|---|
| dependency resolution | the package declares what it provides | may this program be built against this implementation |
| link | an undefined symbol | was an interface used that the implementation does not provide |
| run | a capability word | how does it behave within an interface it provides |

So the three groups of the 137-symbol surface get three different treatments,
and none of them is "present and always fails" — which the spec names as a
defect *because the caller cannot tell*:

| group | treatment | why |
|---|---|---|
| `stream`, `fs`, `time`, `env`, `memory`, `random`, `abort`, `terminal` | provided, forwarding to Emscripten's libc | MEMFS and the JS host serve all of these |
| `net`, `datagram`, `task`, `timeout` | provided, with the capability word reporting what is exercisable | sockets exist but are WebSocket-shaped; threads exist but need a link flag. `KAL_EXEC_PROP_AVAILABLE` is the precedent: the interface is provided either way and whether it can be exercised is read |
| `process`, `exec`, `space` | NOT PROVIDED | there is no fork, no exec and no second address space. A program that uses one fails at LINK naming the symbol, which is clause 6.2's second time and is the mechanism rather than a defect |

The third row is the design decision worth stating plainly: **an absent symbol
is the report.** Providing `kal_process_spawn` on wasm so that it returns an
error would be the shape the spec forbids, and it would also be undetectable
until run time on a platform where the answer is known at link time.

### E4. The examples and the documentation

**The `portable` example is stale on every target, not just the new ones.**
Measured: `openkal/examples/portable` fails to compile for the host with
`unknown type name 'kal_spawn'` — it pins `openkal = "0.10.0"` while using an
API from a different version, and it fails IDENTICALLY on `x86_64-linux-android`.
So the example that exists to demonstrate portability does not build anywhere,
and that has to be fixed before it can demonstrate three more platforms.

Once it builds, it gains the three platform legs as `cfg` dependency lines and
a README recording what was measured on each — which is the same discipline
`examples/13-platform-targets` follows in mcpp: every claim in the README is
measured against the artefact the README describes, not carried over from
another one.

`docs/24-openkal-cross.md` (and its Chinese copy) already carries an
"Android, Web and iOS under this model" section written when Web had no
implementation. It needs the one change that makes it current: the Web row
stops reading "needs an implementation written above a C library" and starts
naming `openkal-emscripten`, with the three-group table above as the reason a
partial surface is a conformant one.

## Self-review across the angles

Recorded because the plan is large enough that an angle left unexamined is an
angle that fails late.

**Architecture.** Item C is the only structural change, and it moves knowledge
in the direction the rest of the ecosystem already flows: the recipe that
installs a payload is the thing that knows its layout. Items A, B, D and E2
remove or reuse rather than add. E3 adds a package, not a mechanism.

**Stability.** C's descriptor is additive and absence is today's behaviour, so
no released payload changes meaning. D touches a function with three existing
callers behind a default argument. The risk concentrates in E3, which is new
code — and new code in a new package cannot regress anything that exists.

**Elegance and simplicity.** B is a deletion. D turns out to be a parameter
rather than a concept, because the SDK locator already exists. The one place
simplicity was deliberately refused is E2's two `cfg` lines: a `cfg(apple)`
dimension would be shorter to write and would introduce a dimension for one
consumer.

**User experience.** Nothing has to be declared for any of the five platforms:
the row's pin is the default and installs on demand. What a user writes is the
deployment floor and, for a program over openkal, one `cfg` line per platform.
The refusals carry their own row's reason, which is what B and the capability
gate are for.

**Compatibility and seamless upgrade.** No floor moves. `index.toml`'s
`min_mcpp` is untouched, so a client stopped at an older engine keeps reading
the whole index. The engine's CI pin moves in the repositories whose CI needs
the new rows, which is the distinction between a pin and a floor.

**Cross-platform.** The host surface this plan adds is two named items on one
host (the iOS SDK via `xcrun`, `simctl` via the runner), both category (b) of
the recorded policy. Everything else resolves through the ecosystem. Three of
the five items are verified on hosts other than the one I am working from,
which is where the last several defects in this area came from.

**Consistency.** One name per payload after B. One slot in the fingerprint for
the deployment floor, because a target is Apple or Android and never both. One
tier vocabulary across four documents, now enforced by
`check_target_tiers.py`.

**Test coverage.** Every claim has a criterion that fails when its subject is
removed, listed below. The two that cannot be measured from a Linux machine are
named as such, with the runner that measures them instead.

## Task list and dependencies

```
repo            id  task                                        depends on
--------------  --  ------------------------------------------  ----------
xim-pkgindex    A1  android-system-image: debugfs -> xim:7zip    -
mcpp            A2  aarch64-linux-android -> verified + 4 docs   -
mcpp            B1  withdraw the `ndk` alias                     -
mcpp            C1  sdk_path(sdk) parameter                      -
mcpp            C2  .mcpp-toolchain.json reader + refusal        -
xim-pkgindex    C3  android-ndk.lua writes the descriptor        C2 shipped
mcpp            D1  three iOS rows: llvm pin, located sysroot    C1
mcpp            D2  -m*-version-min per row                      D1
xim-pkgindex    D3  xim:apple-simulator-tools (simctl-run)       -
mcpp            D4  runner wired for the two simulator rows      D3
mcpp            D5  docs: 20-toolchains + zh, examples/13        A2 D1
openkal         E3a openkal-emscripten: the new implementation   -
openkal         E3b its conformance + independence CI            E3a
openkal         E4a fix examples/portable (stale on every host)  -
openkal         E4b examples/portable: three platform legs       E4a E3a D1
mcpp            E4c docs/24-openkal-cross + zh: name the Web impl E3a
```

**Per-repo single PRs.** `A2 B1 C1 C2 D1 D2 D4 D5 E4c` are one mcpp PR with a
version number -- #612, retitled. `A1 C3 D3` are one xim-pkgindex PR.
`E3a E3b E4a E4b` are one openkal-side PR, except that `openkal-emscripten` is
a NEW PACKAGE and therefore its own repository by this ecosystem's convention
(one package, one repository) -- which is a split by construction rather than
by choice.

**Three edges are not reorderable.** `C3` after `C2` ships, because a
descriptor no engine reads is a file nothing checks. `D4` after `D3`, because a
`runner` naming a program no package provides is a manifest that cannot
resolve. `E4b` after `D1`, because the example cannot have an iOS leg before
the rows can build.

**E2 has no task.** It is a consequence of `D1`: once the SDK resolves,
`openkal-macos` compiles for the iOS rows unchanged, and the only artefact is
two `cfg` lines in `E4b`'s example.

## Criteria

Each claim below fails when its subject is removed, which is the only reason to
write it down.

| claim | criterion | measured |
|---|---|---|
| A: the device row runs | qemu-user run in CI on a Linux runner, asserting `1-2-3` and exit 0; the extraction is the packaged 7zip, not a host tool | yes -- and the packaged tool is `xim:7zip`, not `xim:e2fsprogs`, whose `debugfs` SIGFPEs on every filesystem-opening command |
| B: one spelling | `toolchain = "ndk@…"` is refused AT PARSE with "unknown toolchain", not by the capability gate | yes, with an A/B: re-adding the alias turns the assertion red |
| C: the engine stops knowing | delete `ndk_host_tag()`'s call site and the build still resolves, because the descriptor answered; and a malformed descriptor is refused naming the file | yes -- a descriptor naming `oddly/named/clang++` resolved there, a path no engine derivation produces; a non-string `platform_floor` was refused naming the file and the key |
| C: no flag day | a payload with no descriptor resolves exactly as today -- asserted against the released android-ndk | yes, against the installed r30 payload: with the file removed the resolution line and the effective triple are byte-identical |
| C: the floor and the defines travel | not in the original list, and each needs its own reading or it is carried by the frontend's | yes -- `platform_floor = "26"` gave `…-android26` while `meta/platforms.json` says 21; an added define appeared in the std module's command AND in its cache identity |
| D: iOS builds | macOS runner, `xim:llvm` plus the located SDK, artefact is Mach-O arm64 with the iOS platform in `LC_BUILD_VERSION` | yes -- `platform 2` (IOS) for the device and `platform 7` (IOSSIMULATOR) for both simulator rows, `minos 18.0`, `sdk 18.5`. Asserted by `ios-engine`, which fails when any of the three readings differs, an empty reading included |
| D: the simulator runs | macOS runner, `mcpp run --target aarch64-ios-sim` prints `1-2-3` | yes, through the `runner` and `xim:apple-simulator-tools`; and separately under a bare `simctl spawn`, which is what proved a bundle is not needed. Asserted by `ios-engine`, which fails when the exit status is not 0 or the line is absent |
| D: the host surface is bounded | on a macOS runner with `xcode-select` pointing nowhere, both iOS rows fail with a message naming the SDK -- and no other row changes | THE CRITERION WAS WRONG AND WAS REPLACED. `DEVELOPER_DIR=/nonexistent` did not make the SDK unlocatable -- `xcrun` ignores an invalid developer directory and falls back -- so the iOS build SUCCEEDED and the step asserted nothing. The claim now lives where the SDK is genuinely absent: e2e 641 on every non-Apple host, with the refusal required to name the SDK, the `xcrun` command, the Command-Line-Tools note and the compiler, and to arrive before any payload is resolved |
| E1: Android shares the implementation | merged and green against the RELEASED engine: both ABIs build, objects name no C library symbol, and a program over openkal alone ran on an emulator | yes (openkal-linux 0.12.0) |
| E2: iOS reuses it | `openkal-macos` compiles for the three iOS rows on a macOS runner, and its objects name no C library symbol -- the same check the Android leg applies, against a third libc | the `cfg` line is in `examples/portable`; the compile leg belongs to openkal-macos's own CI and is not in this batch |
| E3: the Web implementation conforms | the conformance suite passes for the groups it provides; and a program using `kal_process_spawn` fails at LINK naming the symbol, which is the criterion that the absent groups are absent rather than present-and-failing | yes -- 86 held, 0 did not hold, 13 not observed; and `wasm-ld: error: obj/main.o: undefined symbol: kal_process_spawn` |
| E4: the examples build | `examples/portable` builds for the host and for all five platforms it names -- it currently builds for NONE, which is why this is a criterion and not an assumption | THE CRITERION WAS NOT SATISFIABLE AS WRITTEN, and the measurement is what said so. Host and both Android ABIs: yes. The Web leg RESOLVES and then fails at link naming seven symbols -- `kal_process_{spawn,wait,close}` and `kal_task_{start,join,wait,wake}` -- because this program uses `openkal.process` and `openkal.task` and this platform has neither. That is the mechanism working, not a gap: the line stays and the example now demonstrates the BOUNDARY, with the linker's own output as the evidence |

**The premise held.** A macos-15 runner ships Xcode 16.4 with both located SDKs
at 18.5 and five bootable iOS simulator runtimes (18.5, 18.6, 26.0, 26.1,
26.2). So D1, D2 and D4 are all CI claims and none of them is local-only.

**Three things the measurement changed about this design.**

The Web leg of `examples/portable` cannot be a build. This record asked for
the example to build "for all five platforms it names", and the fifth is a
platform with twelve of the fifteen interfaces while the program uses two of
the three absent ones. Nothing about that is fixable without either
`#ifdef`-ing the one file in this ecosystem that exists to contain none, or
having `openkal-emscripten` provide operations it cannot perform -- the shape
clause 6.2 forbids. So the criterion was wrong and the example is better for
it: a program about portability that also shows where portability stops, with
seven undefined symbols as the reading.

`simctl-run` is a boot-and-spawn wrapper and not a bundle builder. This record
said a bare Mach-O "cannot be launched by `simctl`, so the program wraps it in
a minimal bundle" -- true of `simctl launch`, which needs an installed `.app`,
and false of `simctl spawn`, which takes an executable. Measured against an
artefact with no bundle, no signature and no `Info.plist`: `1-2-3`, exit 0.

`openkal.task` on Emscripten is behind a feature and cannot yet be exercised
end to end. `-pthread` selects a different C library build, memory model and
loader contract, so it is a property of the WHOLE LINK -- and mcpp has no
channel for a flag that applies to a whole dependency graph, so the
specification package's own modules compile without it and the module cache
refuses the mix. The interface is therefore absent at link rather than present
and failing, which is what clause 6.1 asks for, and the gap is recorded in
openkal-emscripten's README as the engine change it is.

## What the implementation found, and what each finding is about

The plan's self-review is above and was written before any of this was built.
This section is the review of the implementation, and its unit is a measured
defect rather than an angle: each row is something that was wrong, the reading
that made it visible, and the class it belongs to. Nineteen, of which nine are
in mcpp itself.

### A decision written in several places, all but one repaired

| finding | reading |
|---|---|
| `mcpp toolchain install emsdk` and `… android-ndk` had never worked | `error: installed package has no known C++ frontend in '.../xim-x-emsdk/6.0.9/bin'` |

`payload_frontend`'s own comment records that five callers composed
`<root>/bin` themselves and so could not see where the package says its
compiler is. Four were repaired when that function was written; the install
path was not, so both SDK payloads fetched correctly and then looked for
`clang++` in a directory neither keeps it in. The install and the build now ask
one function, which is what makes them unable to disagree.

### One question with two inputs, of which one was read

| finding | reading |
|---|---|
| the same install failure, second cause | `emsdk@6.0.9` resolved the generic llvm shape |

`to_xim_package` decided the payload from the TARGET. That is right for the
spelling a build uses and there is no target at all in an install, so a spec
that NAMED the payload was answered by a field nobody had filled. `payloadName`
now decides first and the target remains the answer for a spec that names no
payload -- which is the escape-hatch spelling the capability gate has to be
able to refuse, so both inputs are still read and neither is guessed.

### A special case that was a table

| finding | reading |
|---|---|
| `em++`'s C compiler became `em` | `/bin/sh: 1: .../emscripten/em: not found` |

Dropping `++` is clang's rule and was being applied as the rule; `g` was
already a special case for exactly this reason. The property is "this driver
names its C compiler with a different word", and a table can be read as the
list of drivers for which that is true. Found by the conformance suite's one C
translation unit -- the only C in this ecosystem's openkal work.

### A value that acquires the receiving layer's semantics

| finding | reading |
|---|---|
| a descriptor naming `/usr/bin/g++` passed the guard on Windows | `path("/usr/bin/g++").is_absolute()` is FALSE there |

Windows calls that path root-relative: it has a root directory and no root
name. `payloadRoot / "/usr/bin/g++"` then resolves to `C:/usr/bin/g++` -- a
host compiler chosen by a package, on the one host where the guard did not
look. The field is one shape on every host, so it is validated as a STRING and
positively. Asking a path type whether it is absolute is asking a question
whose meaning the host supplies.

### A branch selected by the host, serving a target it gets wrong

Three findings, one class, and the class is the sharpest thing in this batch.
`flags.cppm` and `hostflags.cppm` both branch on `needs_explicit_libcxx` --
which asks where mcpp was built. That was the same question as "which platform
is this for" while macOS was the only Apple target mcpp could serve.

| finding | what it would have produced |
|---|---|
| the macOS link branch carried no `--target` at all | a macOS binary from objects compiled as iOS |
| the Mach-O distribution cell chose the self-contained contract | a link of the payload's macOS `libc++.a` into an iOS artefact, which ld64 refuses |
| `hostflags` emitted `-mmacosx-version-min` whenever the host was macOS | `error: invalid argument '-mmacosx-version-min=14.0' not allowed with 'arm64-apple-ios18.0'` |

The note two hundred lines below the first one already said "ONLY THE THIRD
BRANCH EVER CONSUMED `link_toolchain_flags`, WHICH IS WHERE `--target=`
LIVES" -- correct, and read as a statement about openkal rather than as a
statement about every target that branch would ever serve.

### A per-machine artefact read before the command line

| finding | reading |
|---|---|
| an iOS-simulator link with the right `-isysroot` used the macOS SDK | `ld64.lld: error: .../MacOSX.sdk/usr/lib/libc++.tbd(...) is incompatible with arm64 (iOS Simulator18.0.0)` |

`post_install` writes the located macOS SDK into the payload's `clang++.cfg` so
that a native build is deterministic, and that file is read for search purposes
before the command line. An Apple cross therefore has to suppress it, which
mcpp's cross path already did and a hand-written probe did not. The probe was
wrong and the engine was right, and finding out which took one CI run.

### A criterion whose object was wrong

| finding | reading |
|---|---|
| the host-surface claim measured nothing | with `DEVELOPER_DIR=/nonexistent`, the iOS build SUCCEEDED |

`xcrun` ignores an invalid developer directory and falls back to the recorded
one, so the environment change did not make the SDK unlocatable. The predicate
was right and the object was wrong -- the class this repository records most
often. The claim now lives where the SDK is genuinely absent, which is every
non-Apple host, and it asserts four things about the message plus that the
refusal arrives before any payload is resolved.

### A predicate that was the same question until a second platform arrived

| finding | reading |
|---|---|
| the three iOS rows were absent from `toolchain list` on macOS, and present on linux-x86_64 | a target-matrix scan that reached three fewer cells than the table declares |

Two defects, one on each side of the same question, and the tier move is what
exposed them -- while the rows were `planned` the listing kept them
unconditionally, so neither could be seen.

`host_can_serve` asked `os == "macos"`, which was the same question as "is this
an Apple target" while macOS was the only one. An iOS target fell through to
the function's final `return false`, on every host including the one that
serves it. It now asks `is_apple()` -- a predicate the table had carried with
NO READER AT ALL until this line.

And `toolchain list`'s `graphCouldServe` claimed them. Its own comment names
`aarch64-macos` as correctly absent on a Linux host, and that row was absent by
ACCIDENT rather than by rule: its pin is empty, so `!info.pin.empty()`
excluded it. The iOS rows have a pin now, so they entered the branch, found
llvm in the index, and were listed on a host that cannot produce them. The
discriminator the comment appeals to is real and is not the pin -- it is
whether a PACKAGE can supply the target's system, and no package supplies an
Apple SDK.

### The same criterion wrong in three places within one hour

`host_can_serve` now answers for the host, which is correct and which changed
what `toolchain list` reports. Three criteria asserted the old answer, and each
was written by me:

| where | the claim it made | how it surfaced |
|---|---|---|
| e2e 641 case 7 | the three iOS rows appear in the listing with their pin | `FAIL: aarch64-ios does not name llvm in toolchain list`, on CI |
| the sandbox verification's section 2 | the same | a local dry run of the script, before the run that mattered |
| `tests/matrix/expected.tsv` | 30 rows for hosts that cannot serve them | `scan (linux-aarch64)` reaching three fewer cells than declared |

The first is the one worth recording, because I had already written the fix
down and not applied it: while diagnosing the matrix failure I noted that "case
7's claim must become host-conditional -- on macOS the rows are listed; on
other hosts they are correctly absent". Then I fixed cases 3 and 8, added the
unit test, ran the unit suite, and pushed. **I changed the listing and did not
re-run the end-to-end test that asserts the listing.** The unit suite passing
is not evidence about a behaviour no unit test observes.

All three now name the host and carry both arms, and the Linux arm carries two
companions so that an absence cannot be read as a gap: `aarch64-macos` is
absent for the same reason, and `x86_64-macos` is present because a `planned`
row stays discoverable everywhere.

### A skip I wrote from an assumption, refuted by a row in the same table

The macOS scan also reported `graph × iOS` as `mismatch / build-failed`, and I
first wrote it off as out of the fixture's domain -- "an Apple row's system
comes from a located SDK and cannot be replaced by a graph-supplied musl". A
row in the same table says otherwise: `graph macos-arm64 aarch64-macos` is
measured `ok / none`, with `musl(graph)` in its own c-abi column. The predicate
I had written would have skipped that working cell too.

The real reason is one line of a dependency and is in the gap table above: the
fixture's pinned `openkal-musl 0.3.5` does not compile for a Darwin target at
all. So the skip is scoped to the rows the diagnostic covers, and its comment
says why macOS is NOT skipped -- because the alternative was a rule that the
table next to it disproves.

### A fifth copy of a table that a checker covered four of

| finding | reading |
|---|---|
| `test_toolchain_triple.cpp` asserted `preview` after the row and four documents said `verified` | the suite went red; `check_target_tiers.py` said "OK: 29 target tiers agree across 4 documents" |

A literal in a test is in neither the engine's table nor the documents, so a
checker over documents cannot see it. The test's tier claims are now one-line
pairs -- a shape the checker can read -- and it reads the test file as a fifth
document. Removing the fix makes the checker fail, which is the only reason to
add one.

### And the platform refused two mechanisms outright

`openkal-emscripten` is where the specification met a platform that says no
rather than differently.

| finding | reading |
|---|---|
| `.init_array` with `(argc, argv, envp)` | `wasm-ld: error: constructor functions cannot take arguments` |
| `EM_ASM` using `stringToUTF8` into a `_malloc` buffer | `Aborted(malloc() called but not included in the build)` |
| `handle.h` copied from openkal-linux | `handle.h:22: warning: shift count >= width of type` |
| `kal_node_info::self_size` ignored | `DID NOT HOLD an enquiry writes no more of the structure than the caller stated` |

The first is not a calling-convention difference to be careful about -- wasm's
start section takes no arguments, so the mechanism does not exist. The second
is the rule that a library cannot require an export list from every program
that uses it. The third is a constant that was a property of the machine the
file was written on, travelling as if it were a property of the scheme. The
fourth is a field the caller sets and the implementation must honour, and the
suite passes a deliberately short structure to find out.

### A verified tier whose only check could not fail

The macOS job that measured the iOS rows was written as a probe. Every step
continued on error, because its first version stopped at the first unmet
premise and skipped the four measurements after it, and for a probe that was
correct. It stayed that way after those measurements moved `aarch64-ios-sim` to
`verified`, and a probe is not a gate: a regression in the Apple cross path
would have printed `RUN-THROUGH-RUNNER-FAILED` inside a job reported green, and
the tier would have gone on claiming a run that no longer happened.

The final review found it by reading the workflow rather than its result, which
is the only way it could have been found. Every reading of that job was green.

The job is now split along that distinction. `ios-host-surface` keeps the
premise measurements, continues on error, and runs only on request, because a
check that cannot fail, shown beside a gate, reads as a second gate.
`ios-engine` fails on four claims. Each of the three artefacts must be a Mach-O
of the row's architecture whose `LC_BUILD_VERSION` names the row's platform
(`2` for the device, `7` for both simulator rows) and the project's `minos`,
compared as whole values so that an empty reading fails. And
`mcpp run --target aarch64-ios-sim` must exit 0 with `1-2-3` as a whole line of
its output.

The same review removed the fixture's three `toolchain = "llvm@22.1.8"`
overrides. They were needed while the rows were `planned`, and they meant the
job measured an override and never the rows' own pin, which is the path a
project that declares nothing takes.

Two smaller findings from the same pass have the shapes already recorded
above. The located SDK path reaches three command lines and was quoted on two:
the std module's own command spliced it into a shell string unquoted, while
every other path in that string goes through `shq`, so an Xcode installed as
`Xcode 16.app` would have broken only the module precompile. And the payload
root joined `bin/` in the runner's search with a measurement behind it and no
test. The rule is now `runner_lookup::payload_search_dirs`, and a unit test
fails when it is reverted to `bin/` alone, which was checked by reverting it.

## Gaps this batch recorded and did not close

Each is an engine or package change with a measurement behind it, and each was
left rather than worked around.

| gap | measurement | why it was left |
|---|---|---|
| no per-target tool axis | `error: [target.aarch64-ios-sim.xlings] does not accept 'deps'` | a runner's program belongs beside the row that names it; declaring it at the top level makes `examples/13`'s Linux build depend on a macOS-only package, so the example asks the reader to install it |
| no whole-graph flag channel | `error: POSIX thread support was disabled in precompiled file '.../openkal.types.pcm' but is currently enabled` | `-pthread` is an ABI switch for every unit in the link including a dependency's; `openkal.task` is gated behind a feature so the absence is a link error rather than a present-and-failing operation |
| `xim:e2fsprogs`'s `debugfs` | SIGFPE on every filesystem-opening command, while dumpe2fs/e2fsck/tune2fs from the same build work | recorded in that recipe; nothing else in the index depends on it, and `android-system-image` now reads ext4 with `xim:7zip` |
| a device runner for `aarch64-ios` | none -- it needs a signature the developer owns | R12's subject, and a package cannot supply a signature |
| `openkal-musl` has never been built for an Apple target | `okm_syscall.c:444: error: incompatible pointer types passing 'uint64_t *' (aka 'unsigned long *') to parameter of type 'kal_u64 *' (aka 'unsigned long long *')` | one width, two type identities: musl's own `<stdint.h>` spells `uint64_t` as `unsigned long` on LP64, and `kal_u64` is `__UINT64_TYPE__`, which clang defines as `unsigned long long` for a DARWIN target. On every Linux and Windows musl target the two coincide, so that package's CI has never seen it. The current 0.13.1 has the same shape. It belongs to openkal-musl, and the iOS rows do not depend on it: their system is the located SDK, which is the `payload` mode the tier rests on |

## Deliberately not done

- **`xim:iphoneos-sdk` as a package.** The earlier record listed it at
  "whichever of three licence tiers applies". The SDK is not redistributable,
  so the tier is the locator, and a package that only locates is a package that
  ships nothing -- the row's `sysroot` column says it more directly.
- **Xcode's clang as the toolchain.** It would work and it is the wrong default:
  it makes the compiler a host dependency where the ecosystem already has one,
  and it would make the iOS rows the only Apple rows that do not use
  `xim:llvm`.
- **Fixing `xim:e2fsprogs`.** Recorded, not owned here. Nothing else in the
  index depends on its `debugfs`.
- **A device runner for `aarch64-ios`.** R12's subject, and it needs a
  signature rather than a package.
