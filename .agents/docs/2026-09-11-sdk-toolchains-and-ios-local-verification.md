---
subject: targets
status: active
---

# SDK toolchains, the payload/engine seam, and iOS local verification

**Status:** design, for review. Nothing here is implemented.

**Scope.** Four items, in dependency order. The first is measured and only needs
writing down; the second removes something rather than adding; the third is the
only structural change; the fourth is the one with a new capability.

| | item | shape |
|---|---|---|
| A | `aarch64-linux-android` becomes `verified`, and the extraction tool changes | evidence exists, two edits |
| B | the `ndk` toolchain alias is withdrawn | deletion |
| C | a payload describes itself, so the engine stops knowing the NDK | new contract at an existing seam |
| D | iOS builds and runs on the simulator, locally and in CI | one parameter, one row column, one runner package |

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

## Task list and dependencies

```
A1 android-system-image: debugfs -> xim:7zip          xim-pkgindex
A2 aarch64-linux-android -> verified + docs            mcpp        (independent)
B1 withdraw the `ndk` alias                            mcpp
C1 sdk_path(sdk) parameter                             mcpp        (D depends)
C2 .mcpp-toolchain.json reader + malformed refusal      mcpp
C3 android-ndk.lua writes the descriptor                xim-pkgindex (after C2)
D1 three iOS rows: pin llvm, located sysroot            mcpp        (after C1)
D2 -m*-version-min per row                              mcpp        (after D1)
D3 xim:apple-simulator-tools with simctl-run            xim-pkgindex
D4 runner wired for the two sim rows                    mcpp        (after D3)
D5 docs: 20-toolchains SDK section + zh, examples/13    mcpp
```

`A2`, `B1`, `C1`, `C2`, `D1`, `D2`, `D4`, `D5` are one mcpp PR with a version
number, per the single-PR rule. `A1`, `C3`, `D3` are one xim-pkgindex PR. `C3`
must land after `C2` ships, because a descriptor no engine reads is a file
nothing checks.

## Criteria

Each claim below fails when its subject is removed, which is the only reason to
write it down.

| claim | criterion |
|---|---|
| A: the device row runs | qemu-user run in CI on a Linux runner, asserting `1-2-3` and exit 0; the extraction is the packaged 7zip, not a host tool |
| B: one spelling | `toolchain = "ndk@…"` is refused AT PARSE with "unknown toolchain", not by the capability gate |
| C: the engine stops knowing | delete `ndk_host_tag()`'s call site and the build still resolves, because the descriptor answered; and a malformed descriptor is refused naming the file |
| C: no flag day | a payload with no descriptor resolves exactly as today -- asserted against the released android-ndk |
| D: iOS builds | macOS runner, `xim:llvm` plus the located SDK, artefact is Mach-O arm64 with the iOS platform in `LC_BUILD_VERSION` |
| D: the simulator runs | macOS runner, `mcpp run --target aarch64-ios-sim` prints `1-2-3` |
| D: the host surface is bounded | on a macOS runner with `xcode-select` pointing nowhere, both iOS rows fail with a message naming the SDK -- and no other row changes |

**One premise needs measuring before D is scheduled**: that GitHub's macOS
runners ship both an iOS SDK and a bootable simulator. If they ship the SDK but
no simulator, D1/D2 are still verifiable in CI and D4 is a local-only claim,
which changes the tier the sim rows can reach and nothing else in this design.

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
