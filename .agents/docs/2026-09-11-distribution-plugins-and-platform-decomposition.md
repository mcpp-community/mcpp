---
subject: plugins
status: active
---

# The category the plugin taxonomy does not name, and what a platform actually decomposes into

Date: 2026-09-11. Base: mcpp `2d102817`, `mcpp-plugins` `b4f7590` (0.2.6).

**How to read this document.** Sections 1 to 3 are analysis of the base commits
above and can be checked against them. Sections 4 to 7 are a proposal and are
not implemented. Section 8 is what the proposal does not solve.

The thesis is one sentence: **the engine should own the mechanism by which a
distributable is produced, and no format should live in the engine at all.**

The occasion is a framework outside this project — HuxerUI, a declarative UI
library with a CMake build and a six-platform reach — being ported to mcpp, and
its ecosystem library `Lib-Live2D` being examined as the case that a package
manager either serves or does not. Two things came out of it that are about
mcpp rather than about that framework, and this document is those two.

## 1. What is already true

`mcpp:plugins` 0.2.6 is one package whose members are selected by features:

```toml
[features]
default       = []
rules-ascendc = { sources = ["rules/ascendc.cppm"] }
rules-cuda    = { sources = ["rules/cuda.cppm"] }
rules-hip     = { sources = ["rules/hip.cppm"] }
rules-spirv   = { sources = ["rules/spirv.cppm"] }
rules-sycl    = { sources = ["rules/sycl.cppm"] }
tools-embed   = { sources = ["tools/embed.cppm"] }
```

The taxonomy behind the two prefixes is stated in `docs/30-build-mcpp.md:789`:

> **A tool is not a rule.** A rule states how a translation unit is compiled by
> a compiler mcpp does not drive: it submits an action and the engine schedules
> it. A tool states something the build program needs that no compiler
> performs, and does it while the program runs.

And the reserved prefix, from the same file:

> `warning: build rule 'mcpplibs.plugins' declares the module 'mcpp.rules.spirv';
> the 'mcpp.' prefix is reserved for rules maintained by the mcpp project.`

Five extension points do the extending, and `docs/31-authoring-a-rule-package.md`
says what they buy: **the engine holds no name that comes through any of them**.
Slang is the evidence — "the first language mcpp supports without naming it in
the engine".

## 2. The category the taxonomy does not name

A third kind of work fits neither definition. It does not compile a translation
unit, so it is not a rule; and it does not run while the build program runs, so
it is not a tool. It consumes **link outputs** and produces something a user
installs:

| Work | Compiles a TU? | Runs in the build program? |
|---|---|---|
| SPIR-V from a shader | yes | no |
| A data file into a header | no | yes |
| **An MSI from the linked program** | **no** | **no** |
| **codesign on a `.app`** | **no** | **no** |
| **A `.deb`, an AppImage, an `.apk`** | **no** | **no** |

The engine already has the mechanism: `role = "artifact"`, whose inputs are link
outputs, so ninja sequences it after the link. `docs/30-build-mcpp.md:464` names
the intended uses — "codesign, packaging, size budgets". What is missing is a
name for the packages that ship such actions, and therefore a place for them.

**This is not hypothetical.** The HuxerUI port implements a Windows installer
this way and it is green on Windows CI: one `role = "artifact"` action running
`wix build`, with the program passed as `-d Executable=${mcpp.target_file:<target>}`
and the definition naming it with `<File Source="$(Executable)" />`. The
resulting MSI is 1572 KB and its `File` table carries one row, the 6.9 MB
application. Nothing in that action is specific to a UI framework except a
default icon path.

An earlier revision of the same action bound a directory (`-bindpath
Application=bin`) and harvested it. When the path resolved to nothing on
Windows, WiX produced a **valid, empty, 52 KB installer with no diagnostic**.
That failure is worth recording because it is the argument for a shared package
rather than a per-project action: the mistake is not obvious, it is silent, and
every project that writes its own installer step gets to make it once.

### 2.1 `mcpp pack` keeps the universal format and dispatches the rest

`mcpp pack` is the right home, and the question is which half of it is the
engine's. `docs/10-pack-and-release.md:389` documents `[pack]` as declarative
configuration over a fixed set of modes (`static`, `bundle-project`,
`bundle-all`, `system`), and lists `.deb`, `.rpm` and AppImage under **Planned
Support**.

Those three should not land there, and the split that keeps them out is:

> **`mcpp pack` owns the mechanism and the one universal format. Every other
> format lives in a package, and `mcpp pack` dispatches to it.**

The universal format is what it already produces: an archive that extracts and
runs. It is universal in the only sense that matters here — it needs no
knowledge of anyone else's release. Everything past it does. dpkg's control
fields, AppImage's runtime, WiX's schema, Apple's notarisation, Android's
signing scheme v3: each one bound into the engine couples an mcpp release to a
release mcpp does not control. The project already made this argument for
languages — Slang is supported "without naming it in the engine" — and a
distribution format has less claim to a name in the engine than a language
does.

Dispatch is what keeps this from fragmenting into "packaging is a thing you do
outside `mcpp pack`" — and **the flag for it already exists**:

```
mcpp pack --format    tar (default; .zip for a Windows target) | dir
```

`tar` and `dir` answer the same question `msi` and `appimage` answer — what
shape does the output take — so they belong on one axis, and the proposal is
not a new flag but a wider set of values for this one:

```bash
mcpp pack --format msi
mcpp pack --format appimage
```

exactly as `--target` reaches a triple the engine did not have to know about
individually. Two consequences follow from the values no longer being a fixed
list: `--help` says "plus any format the resolved graph provides", and an
unknown value names what *is* available rather than a constant.

### 2.2 What the engine must expose for that to work

Three additions, and each is **format-neutral** — which is the test for whether
something belongs in the engine at all.

**(a) A staged tree an artifact action can consume.** `mcpp pack` already
computes one: the dependency closure, the strip policy, the debug-symbol split,
`include`/`exclude`. It then compresses it and the directory is gone. A `.deb`,
an AppImage, a `.app` and an `.msi` all want exactly that directory and today
must each rebuild it. Exposing it — a `${mcpp.stage_dir}` placeholder, or a
`--stage-only` mode whose output path an action can name — is one addition that
serves every format, present and future, and encodes no format's knowledge.

**(b) Package metadata in the build program.** A build program is told
`package_name()` and `package_namespace()` and not the version, description,
license or authors. Every installer needs the version; the HuxerUI
implementation therefore asks the project to restate it in the rule's options,
where it can drift from `[package] version` with nothing to detect it. This is
a read of data mcpp already parsed.

**(c) `--format` resolves its value through the graph.** A package declares that
it provides a format name; `--format <name>` finds the provider among the
resolved dependencies and hands it the staged tree. The engine holds the
*dispatch*, not the format — the same shape as `device_extensions`, where the
engine classifies a source it cannot compile and a package supplies the
compiler. This is the smallest of the three additions, because the flag, its
parsing and its position in the command are already there.

With (a) to (c), `[pack]`'s built-in modes become one provider among several
rather than the model, and nothing new needs to join them.

## 3. Where a platform actually decomposes

The second question was whether Android, iOS and Web can be reached from
packages. The answer differs per platform, and the difference is not a matter
of degree.

A target is **not** extensible from a package. `modules/toolchain-model/src/triple.cppm`
declares the identity as three strings —

```cpp
struct Triple {
    std::string arch;   // "x86_64" | "aarch64" | "riscv64" | ...
    std::string os;     // "linux" | "macos" | "windows"
    std::string env;    // "gnu" | "musl" | "msvc" | ""
};
```

— with `os == "none"` added for freestanding, and a **23-row
`kKnownTargets` table compiled into the binary**:

```cpp
{ "x86_64-windows-msvc",  "verified",  "PE",   "",            "",                          false },
{ "aarch64-macos",        "verified",  "",     "",            "",                          false },
{ "aarch64-linux-gnu",    "planned",   "",     "",            "",                          false },
{ "riscv64-none-elf",     "verified",  "bare", "llvm@22.1.8", "xim:picolibc-riscv@1.8.12", true  },
```

Bare-metal targets are **rows in that table**, not something a board-support
package introduced; a BSP supplies the runtime for an already-known target. So
the engine boundary is exact: a package can add a language, a tool, an action,
a payload and a generated module. It cannot add a triple.

There is no separate object-format model to extend either, and that is the
sharper form of the same fact. The binary format is not a field; it is derived
from `os` at each site that needs it — `is_pe()` is `os == "windows"`,
`is_freestanding()` is `os == "none"`, and `family()`, the `cfg()` dimension,
answers `windows` or `unix` and **nothing at all** for anything else. A fourth
answer to "what does this produce" is therefore not one
addition but an addition at every such site, which is why wasm is a different
size of change from a table row. (`nasm_format()` looks like an object-format
switch and is not one: it is NASM's `-f` flag, x86-only by construction, and it
hard-errors off x86 rather than choosing.)

With that boundary fixed, each platform decomposes into layers, and only the
first is engine work:

| Layer | Android | iOS | Web | Owner |
|---|---|---|---|---|
| **Toolchain does modules** | **yes, with a supplied surface + one define** (3.1) | not measured | **yes, with a supplied surface** (3.1) | **prerequisite, and answered for two** |
| Target identity | `env = "android"`, one row | `os = "ios"`, one row | **new arch, new os, new object format** | **engine** |
| Toolchain payload | `xim:android-ndk` | `xim:iphoneos-sdk` | `xim:emsdk` | index |
| Sysroot | table column, or `[target.<triple>].sysroot` | same | same | engine / manifest |
| Compile and link flags | `-target aarch64-linux-android<api>` | `-miphoneos-version-min` | `-sUSE_WEBGL2` etc. | **plugin** |
| Packaging | `.apk` | `.app` | `.html` + `.wasm` + `.js` | **plugin** |
| Signing | `apksigner` | `codesign` | — | **plugin** |
| Running | `adb install` | simulator | a browser | **plugin** (`mcpp::runner`) |
| Non-C++ glue | Gradle | Xcode project | JS bridge | **plugin** |

The distances are unequal, and the ranking is the opposite of the usual demand
ranking:

- **Android is the smallest.** `aarch64-linux-gnu` is already a `planned` row,
  ELF is already an object format, and `aarch64` is already an arch. What is
  missing is an `env` value and a sysroot that points at an NDK.
- **iOS is next.** `aarch64-macos` is `verified`, so Mach-O and the Apple half
  of the toolchain model exist. What is missing is an `os` value and the
  iPhoneOS SDK.
- **Web is the outlier.** A new arch (`wasm32`), a new os, a new object format,
  and a driver that is a wrapper rather than a compiler mcpp drives directly.
  This is [#597](https://github.com/mcpp-community/mcpp/issues/597), and it is
  the only one of the three that changes the target model rather than extending
  a table.

**The consequence for planning:** every layer below the first is the same
mechanism on all three platforms. A `.apk` step, a `.app` step and an
`.html`+`.wasm` step are three members of the category section 2 names, and
none of them waits on the engine to be *written* — only to be *useful*.

## 3.1 The prerequisite nobody lists: does the platform's toolchain do modules?

Section 3 treats the target row as the engine's part of the work. That is true
and incomplete. mcpp is module-first — `import std` availability is one of the
eleven fingerprint inputs, and a package's interface is a BMI — so a target row
for a toolchain that cannot compile a module interface unit would resolve, build
nothing, and be worse than its absence.

This was measured on this machine rather than assumed, and the result is better
than the shipped state of either toolchain suggests: **`import std` works on
both Android and Emscripten today, and neither needs a fork or a compiler
upgrade.** What both need is a directory their vendor chose not to install.

### The gap, stated once — and closed by one vendor since

An LLVM installation that supports `import std` carries a generated module
surface beside its headers.

**The NDK ships one as of r30.** Measured 2026-09-11 against
`android-ndk-r30-linux.zip` (`Pkg.Revision 30.0.16248370`, the current LTS):
`share/libc++/v1/` holds `std.cppm`, `std.compat.cppm`, 110 `std/*.inc` and 21
`std.compat/*.inc` — the same 133 files this section measured as **absent** on
r27, generated by Google from the same AOSP checkout as the compiler. So the
table below is r27's state, and for Android the remedy it describes is no
longer needed: there is nothing to fetch from an `llvmorg-*` tag, nothing to
substitute, and no second asset to publish.

Two things about r30 did **not** change, and both matter more than the file
count:

- **`-D__BIONIC_CTYPE_INLINE=` is still required.** The bionic ctype defect
  reproduces verbatim on r30 / clang 21: 28 errors, confined to `cctype.inc`
  and `locale.inc`, `using declaration referring to 'isalnum' with internal
  linkage cannot be exported`. Google ships the surface without shipping a
  build of it that works out of the box.
- **The version to match is still not derivable from the compiler.** r30's
  `_LIBCPP_VERSION` is `210000` from clang `21.0.0 (based on r574158c)` — an
  AOSP `toolchains/llvm-project` mirror revision, **not** a build of any public
  `llvmorg-21.1.x` tag. So the pairing check below cannot compare against an
  upstream tag for Android; it can only pin the value and refuse a change.

That is the second time a guess in this section was wrong in the same
direction: **a vendor had already done the work and nobody looked.** The first
was Apple (below). The table is kept as the r27 measurement it was, because the
argument it supports — that a target row for a toolchain without the surface
would be worse than its absence — is what motivated looking at all.

| | `xim:llvm` 20.1.7 | NDK r27 | Emscripten 4.0.19 |
|---|---|---|---|
| clang | 20.1.7 | 18.0.1 | 22.0.0git |
| `_LIBCPP_VERSION` | 200100 | 180000 | **200100** |
| `_LIBCPP_ABI_NAMESPACE` | `__1` | `__ndk1` | `__2` |
| `share/libc++/v1/std.cppm` | yes | — | — |
| `std/*.inc` + `std.compat/*.inc` | 110 + 21 | 0 | 0 |
| total | 133 files, 620 KB | **0** | **0** |

The NDK additionally sets `_LIBCPP_HAS_NO_STD_MODULES` in its `__config_site`.
That macro appears **exactly once in the whole NDK — in `__config_site`
itself**; no header consults it. It disabled the *installation* of the module
files at libc++ build time and gates nothing in the library, which is what makes
supplying them legitimate rather than a workaround around a disabled feature.
Emscripten does not set it at all.

### Why "just upgrade libc++ to 22" is not the answer

Tried, and it fails for a reason worth recording. libc++'s headers are **not
portable across configurations**: `__config_site` is generated per build and
records the ABI, threading and locale decisions that build made. Pointing NDK
clang at llvm 22.1.8's headers gives

```
__config:13:10: fatal error: '__config_site' file not found
```

and the three `_LIBCPP_ABI_NAMESPACE` values in the table above are the deeper
form of the same fact: `__1`, `__ndk1` and `__2` are deliberately incompatible,
so replacing a vendor's libc++ renames every `std` symbol's ABI namespace.
"Upgrade to 22" therefore means *building* libc++ from source for that target
and accepting a platform ABI change — not a file swap. It is a real option for
a project that already static-links its C++ runtime, and it is not needed,
because matching the revision works.

### Android: measured, works, one define

Named modules work as shipped:

```
$ clang++ --target=aarch64-linux-android24 -std=c++20 --precompile m.cppm   # OK
$ clang++ --target=aarch64-linux-android24 -std=c++20 -fmodule-file=m=m.pcm -c main.cpp   # OK
```

`import std;` does not — `fatal error: module 'std' not found` — for the reason
the table gives. Supplying the surface is mechanical, because `std.cppm` is
generated:

```
// WARNING, this entire header is generated by utils/generate_libcxx_cppm_in.py
```

from `libcxx/modules/std.cppm.in` and one CMake substitution that fills
`@LIBCXX_MODULE_STD_INCLUDE_SOURCES@` with `#include` lines naming the
`std/*.inc` partitions. Taking `libcxx/modules/` from **llvmorg-18.1.8** — the
release matching `_LIBCPP_VERSION 180000` — and performing that substitution
reproduces the vendor's own 133 files.

Compiling it against the NDK's own headers then failed, and the failure is the
interesting part:

```
std/cctype.inc:11: error: using declaration referring to 'isalnum' with
                          internal linkage cannot be exported
bionic ctype.h:127:  __BIONIC_CTYPE_INLINE int isalnum(int __ch) { … }
```

28 errors, all of that one kind, confined to 2 of the 110 partitions
(`cctype.inc` and `locale.inc`). bionic defines the ctype functions
`static __inline`, and a using-declaration naming an internal-linkage entity
cannot be exported from a module.

bionic anticipated this. The macro is overridable and says why:

```c
/* All the functions in this file are trivial … we inline them by default.
 * This macro is meant for internal use only, so that we can also provide
 * actual symbols for any caller that needs them. */
#if !defined(__BIONIC_CTYPE_INLINE)
#define __BIONIC_CTYPE_INLINE static __inline
#endif
```

With `-D__BIONIC_CTYPE_INLINE=` on the `std.cppm` compile, the surface builds
(27.5 MB BMI) and a program that uses it links:

```cpp
import std;
int main() {
  std::vector<int> v{3,1,2};
  std::ranges::sort(v);
  return std::format("{}-{}-{}", v[0], v[1], v[2]) == "1-2-3" ? 0 : 1;
}
```

```
app: ELF 64-bit LSB pie executable, ARM aarch64, interpreter /system/bin/…
```

### Android execution: measured, and the answer is conditional

The binary above was not executed when this was first written. It has been
since, and the result is split in a way that decides the row's tier rather than
merely informing it. Measured 2026-09-11 with NDK r30 and `qemu-aarch64` 8.2.2:

| | static | dynamic (the row's default) |
|---|---|---|
| `x86_64-linux-android` | **runs**, direct execution, no wrapper | does not run off-device |
| `aarch64-linux-android` | **runs** under plain `qemu-aarch64`, no flags | does not run off-device |

Both confirmed with the `import std` program, printing `1-2-3`.

**Dynamic fails for two independent, compounding reasons**, and neither is a
missing flag:

1. **The loader is device-only.** Every dynamic artifact carries
   `interpreter /system/bin/linker64`. `-L` and `QEMU_LD_PREFIX` affect library
   SEARCH, not the interpreter path, so qemu opens the literal absolute path and
   reports `Could not open '/system/bin/linker64'`. An exhaustive search of the
   2.3 GB installed NDK finds **zero** files named `linker` or `linker64`: the
   Android dynamic linker ships only inside a system image.
2. **Even with a loader, bionic's libc is inert.** `libc.so`'s `.text` is 9176
   bytes for 1147 exported functions — about eight bytes each — and every
   function inspected disassembles to exactly `bti c; ret`. That is the
   documented NDK stub shape: it exists so the linker can resolve names, and
   the real bodies come from the device. For contrast `libc++_shared.so` is
   real code, 724 KB, because it is the one runtime the NDK is responsible for.

**So the honest claim is "runnable only when statically linked", and the tier
follows from the DEFAULT configuration rather than from the best case.** The
rows carry `defaultStatic = false`, and that is correct about the platform: a
real Android application links dynamically against the device's bionic, and
setting the flag true to make a CI check pass would misdescribe the platform to
flatter the measurement. Android is therefore `preview`, with execution
recorded for a configuration that is not the default — which is more than
`preview` usually means and less than `verified` requires.

`x86_64-linux-android` needs no runner at all for the static case; mcpp's
"no runner declared, attempt direct execution" default is already right.
`aarch64-linux-android` needs `runner = ["qemu-aarch64"]` and nothing else.

What dynamic execution would take is an emulator harness rather than an
addition: a system image per (ABI, API level) at multiple gigabytes each,
`adb`/`emulator`/`avdmanager` on top of the NDK, KVM or nested virtualisation
that an ordinary runner does not have, and boot times in tens of seconds
against qemu-user's near-instant start. That is its own package and its own CI
lane.

### The state as of 2026-09-11, all three vendors measured at current versions

The sections above are the 2026-09 measurement of the versions then current
(NDK r27, Emscripten 4.0.19). Every one of them was re-taken while building the
payloads, and the picture changed:

| | NDK **r30** | Emscripten **6.0.9** | iPhoneOS **26.5** |
|---|---|---|---|
| `_LIBCPP_VERSION` | 210000 | 220108 | 210106 |
| a public `llvmorg-*` tag? | **no** — AOSP `r574158c` | yes, 22.1.8 | yes, 21.1.6 |
| ships the module surface | **yes**, 133 files | **yes**, 134 files | **no**, 0 files |
| needs a define to build it | `-D__BIONIC_CTYPE_INLINE=` | none | none |
| execution reachable here | see below | **yes** — node | no |

**Two of the three vendors now ship the surface themselves**, and the third's
is derivable because its version is public. So the generation machinery this
section describes at length is needed for one target rather than three, and
what the recipes actually owe is narrower than the section implied: **pin the
`_LIBCPP_VERSION` you measured and refuse a change.** For Android that is all
it can be — an AOSP mirror revision has no upstream tag to compare against.

The other corrections worth carrying:

- **Emscripten's version was wrong in this document, in the direction the
  document warned about.** It recorded `_LIBCPP_VERSION 200100` and clang
  22.0.0git; 6.0.9 reports `220108` and clang 24.0.0git. The lesson it drew --
  that the surface must match the LIBRARY and not the compiler -- is right, and
  the numbers it drew it from are two releases stale.
- **`emsdk` cannot be a payload at all.** `emscripten-core/emsdk` publishes
  **zero** GitHub releases, and its `emsdk.py` fetches the real toolchain over
  the network at install time. The recipe therefore names what `emsdk.py`
  itself downloads: an `emscripten-releases-builds` bundle on Google Cloud
  Storage, addressed by a 40-hex commit hash and thus immutable. The `latest`
  alias moves several times a week and is resolved once, in the recipe, rather
  than at install time.
- **`em++` needs a Python interpreter before it opens any config file**, being
  a `/bin/sh` wrapper that execs Python. And `NODE_JS` is mandatory for
  *linking*, not only for running the result: measured, a broken `NODE_JS`
  survives `-c` and fails the link on `tools/compiler.mjs`.

### Emscripten: measured, works, no define

Named modules work as shipped. The module surface was absent in 4.0.19 as it
was on Android, and the version to match is **not** the one the compiler
reports: `em++` was clang 22.0.0git while its libc++ was `_LIBCPP_VERSION
200100`, LLVM 20.1. llvm 22.1.8's surface therefore failed on `'flat_set' file
not found`; llvm 20.1.7's built with **no additional flags at all** (31 MB
BMI). Both halves of that have since moved -- see the table above.

End to end, and this one did run:

```cpp
import std;
int main() {
  std::vector<int> v{3,1,2};
  std::ranges::sort(v);
  std::print("{}-{}-{}\n", v[0], v[1], v[2]);
}
```

```
$ em++ -std=c++23 -fmodule-file=std=std.pcm -c app.cpp && em++ app.o std.pcm -o app.js
$ node app.js
1-2-3
```

`app.wasm` is 470 KB. So the Web question splits cleanly in two, and only one
half is open: **the standard library story is answered**, and what remains is
[#597](https://github.com/mcpp-community/mcpp/issues/597)'s target model.

### Apple: measured after all, and every guess in this section was wrong

This section first said iOS could not be measured on a Linux host, that Apple's
libc++ is not a build of a public revision, and that the remedy might therefore
not exist. **All three are false**, measured 2026-09-11 against
`iPhoneOS26.5.sdk` (49 MB, from a public mirror) and `xim:llvm@22.1.8`:

| | |
|---|---|
| `_LIBCPP_VERSION` | **210106** — a public revision, and `llvmorg-21.1.6` exists upstream |
| `_LIBCPP_ABI_NAMESPACE` | `__1` — upstream's own default, not a vendor-private namespace like the NDK's `__ndk1` |
| `_LIBCPP_HAS_NO_STD_MODULES` | `/* #undef */` — Apple does **not** disable std modules, unlike the NDK |
| `std.cppm` in the SDK | **0 files**, exactly as on Android and Emscripten |

So the recipe is the same recipe: take `libcxx/modules/` from the tag matching
`_LIBCPP_VERSION`, perform the `@LIBCXX_MODULE_STD_INCLUDE_SOURCES@`
substitution, and get 133 files and 620 KB — the same count and size the other
two produce, and the same the vendor ships where it ships one at all.

Carried end to end, from a Linux host:

```
$ clang++ --no-default-config --target=arm64-apple-ios18.0 -isysroot <sdk> \
      -nostdinc++ -isystem <sdk>/usr/include/c++/v1 -std=c++23 \
      --precompile surface/std.cppm -o std.pcm        # 34 MB BMI
$ clang++ ... -fmodule-file=std=std.pcm -c app.cpp    # import std;  OK
$ clang++ ... -fuse-ld=lld app.o std.pcm -lc++ -o app
$ file app
app: Mach-O 64-bit arm64 executable, flags:<NOUNDEFS|DYLDLINK|TWOLEVEL|…|PIE>
```

The binary was not executed — no device and no simulator on a Linux host — so
this is "compiles and links", which is what the `preview` tier means.

**`--no-default-config` is load-bearing, and the reason generalises past iOS.**
`xim:llvm`'s payload ships a `bin/clang++.cfg` that injects the host's glibc and
libc++ unconditionally:

```
-isystem <xim:llvm>/include/c++/v1
-isystem <xim:glibc>/include
-Wl,--dynamic-linker=<xim:glibc>/lib64/ld-linux-x86-64.so.2
```

Those apply to **every** target the driver is pointed at, so a cross target that
is not Linux/glibc silently gets the host's C and C++ standard library ahead of
its own. `-nostdinc++` does not displace them — measured: with `-isysroot` and
`-nostdinc++` both given, the search list still began with the host's
`include/c++/v1` and the compile failed inside the host's `stdint.h` on
`gnu/stubs-32.h`. The criterion is `clang -v`'s search list, not whether the
compile succeeds, because a compile that reads the wrong standard library
usually succeeds.

That is a property of the payload rather than of iOS, and it applies to any
non-Linux target driven through it.

### What this means for xim-pkgindex

The index has **253 recipes and none of the three**: no `android-ndk`, no
`emsdk`, no `iphoneos-sdk`. `llvm` is there and is the shape to copy.

| Recipe | Beyond the archive it must carry | Blocked on |
|---|---|---|
| `xim:android-ndk` | `share/libc++/v1/` from llvmorg-18.1.8, a `_LIBCPP_VERSION` check, and `-D__BIONIC_CTYPE_INLINE=` on the std BMI | **nothing — measured working** |
| `xim:emsdk` | `share/libc++/v1/` from the release matching *libc++*'s version, not the compiler's | **nothing — measured working** |
| `xim:iphoneos-sdk` | licensing decides whether it installs or merely locates | a licence reading, and one measurement |

Both writable recipes owe the same two things: **pin both halves to one
revision and refuse on mismatch** — the check is exact, `_LIBCPP_VERSION` in
`__config_site` against the release the surface came from — and **re-derive on
every vendor bump**, since the surface is a function of the vendor's libc++,
not a constant.

The licensing asymmetry is worth stating plainly. The NDK is Apache-2.0 and
Emscripten is MIT, both redistributable; the iPhoneOS SDK is neither, which is
why cross-platform toolchains reach it through a locally installed Xcode.
`xim:iphoneos-sdk` may therefore have to be a *locator* — a recipe that finds
and pins what the machine already has, the way `msvc@system` does — rather than
an installer. mcpp already has that shape.

## 4. The members this proposes

Nothing here is a new repository. `mcpp-plugins` exists and its shape — one
package, one module interface unit per feature — already fits.

| Feature | Module | Category | What it does |
|---|---|---|---|
| `dist-wix` | `mcpp.dist.wix` | dist | An MSI from a linked program and a definition it renders. Windows only. |
| `dist-appimage` | `mcpp.dist.appimage` | dist | An AppImage from a staged tree. Linux only. |
| `dist-apple` | `mcpp.dist.apple` | dist | A `.app` bundle, `Info.plist`, and `codesign`. macOS now; iOS when the row exists. |
| `dist-android` | `mcpp.dist.android` | dist | An `.apk`: Gradle invocation or direct `aapt2`/`d8`/`apksigner`. When the row exists. |
| `dist-web` | `mcpp.dist.web` | dist | The `.html`/`.js`/`.wasm` set and its loader. When the target model admits wasm. |

Two of these can be written **today**, against the engine as it is: `dist-wix`
(a working implementation exists and would be a port, not a design) and
`dist-appimage`. `dist-apple` can be written for macOS today and gains iOS when
the row lands. The other two wait on section 3's first layer.

### Why `dist-` and not `rules-`

Because the taxonomy in section 1 is load-bearing and these are not rules. A
consumer reading `rules-wix` would expect a compiler it does not drive and a
translation unit; there is neither. The prefix should say which of the three
questions the member answers:

```
rules-*   how is this translation unit compiled
tools-*   what does the build program need to do itself
dist-*    what comes out of the link, and in what form a user installs it
```

`mcpp.` stays reserved for members of this repository, unchanged, and
`mcpp.build.*` remains the engine's own module family — `mcpp.dist.*` collides
with neither.

## 5. What `tools-embed` is missing

`mcpp.tools.embed` covers one file at a time. `files()` writes **one header per
input**, deriving an identifier from each, and refuses `options::identifier`
because it "names one symbol and `files()` writes several".

The case it does not cover, taken from Lib-Live2D's `cmake/EmbeddedShaders.cmake`
(34 lines of `file(READ)` and string concatenation, with a second 59-line
variant for Metal): **N inputs, one header, one table**, where each row carries
the file's name alongside its contents and the consumer iterates.

```cpp
inline constexpr EmbeddedShader embedded_shaders[]{
  {"Standard.vert", R"(…)"},
  {"Standard.frag", R"(…)"},
};
```

This is not a new plugin. It is a `table()` entry point beside `file()` and
`files()`, with an option for the row type's name and for how the key is
derived from the path. The `write_if_different` behaviour already there is the
part that makes it cheap to call unconditionally, and it carries over.

I have not measured how many projects want the table shape rather than the
per-file shape. One does, and its 93 lines of CMake are the evidence that the
shape is worth having; that is an argument for adding it, not a measurement of
demand.

## 6. What a dist member owes its consumer

The rules in `docs/30-build-mcpp.md` apply unchanged, and two of them bind
harder here than for a rule:

**Expose a plan/submit pair.** A dist member's output is the last thing before
a user's hands, so it is the most likely to need a project-specific edit — a
different compression level, an extra file, a second signature. `generate_all(opt)`
being `submit(plan_all(opt))` is what keeps that edit from becoming a
reimplementation.

**Failure and advice use different channels.** A packaging step that succeeds
while carrying nothing is the failure mode section 2 measured. Where a dist
member can detect an empty or implausible result, it must say so on a
*successful* build through `mcpp::warning`, because stderr on success is
discarded.

**One `(name, version)` names one payload.** A dist member that wraps a signing
tool inherits that tool's compatibility surface. Versioning in lock-step with
the wrapped tool is legitimate and says something true.

Two more that are specific to this category:

**Name the input, do not harvest a directory.** Section 2's 52 KB installer is
the general case: a path that resolves to nothing is silent, and a named input
that is missing is an error. `${mcpp.target_file:<target>}` is the mechanism —
a build program is told neither the triple nor the fingerprint, and an unknown
target name is refused rather than expanded to an empty path.

**Declare the tool where it will be looked up.** `xpkg_dir` answers from
`MCPP_XPKG_*_DIR`, which mcpp sets for the *building* package. A dependency's
declaration provisions the payload — the log says
`Provisioning [xlings.workspace] entries (...)` — without making it visible to
a consumer's build program. A dist member that runs a payload tool therefore
declares it itself, and says so when the lookup returns empty rather than
pointing at a manifest the reader does not own.

## 7. Staging

The order is not the demand order, and the reason is that the lower layers are
shared:

1. **`dist-wix`, then `dist-appimage`.** No engine change. `dist-wix` is a port
   of a working implementation; `dist-appimage` is the same shape on the
   platform where it is easiest to test. Both restage by hand, which is the
   evidence for step 2 rather than a reason to delay them.
2. **The two engine additions of 2.1: a consumable staged tree, and package
   metadata in the build program.** Both are format-neutral, and step 1 will
   have shown what each costs to do without. After this, no format needs the
   engine again.

3. **`tools-embed`'s `table()`.** Independent of everything else, and the
   smallest of these.
4. **`dist-apple` for macOS.** Establishes the bundle-and-sign shape on a target
   that already exists, so that iOS later adds a row and not a design.
5. **`xim:android-ndk` and `xim:emsdk`, each carrying the module surface (3.1).**
   Both are measured working and neither waits on the engine. Writable before
   the rows, and doing them first turns each row into a small verifiable change
   rather than a change plus an unknown.
6. **The Android row.** One `env` value, one table row, one payload. The
   smallest of the three platform steps, and it makes `dist-android` writable.
7. **The iOS row**, then `dist-apple` extends to it — preceded by the same
   measurement for Apple clang, and by whether `xim:iphoneos-sdk` can be a
   payload or must be a locator.
8. **Web**, as [#597](https://github.com/mcpp-community/mcpp/issues/597)
   describes. It is a target-model change and not a table row — but it is now
   *only* that: 3.1 measured the standard-library half and it works, so #597 is
   one problem rather than two.

Steps 1 to 4 do not wait on a platform. Doing them first means that when a
platform row lands, the layer above it already exists — and that the row is the
only thing that had to land.

## 8. What this does not solve

**A published package still carries no consumer dependencies from the target
axis.** `mcpp emit xpkg` derives `xpm.<platform>.deps` from the top-level
`[xlings.workspace]` only. A dist member declaring its tool on the target axis —
which is where a payload the produced code links against belongs — publishes
cleanly and fails in the consumer's link. The workaround is to declare on both
axes, and the second copy states a target fact on the host axis, which mcpp's
own guidance calls the wrong form.

**`mcpp test` is not configurable.** A library with an existing test tree that
is not `tests/**/*.cpp` cannot use `mcpp test` at all; HuxerUI reached this and
answered it by adding a separate package whose `tests/` selects instead of
discovering. This is unrelated to plugins and is noted because it is the second
thing an ecosystem library hits.

**Every guess along the way was wrong, including the ones this document made
about Apple.** That the gap was one file (it is 133); that a mismatched surface
fails obscurely (it names the missing header); that the blocker was libc++ (it
was bionic's `static inline` ctype); that Emscripten's libc++ version follows
its clang version (it does not); that iOS could not be measured from Linux (it
can); that Apple's libc++ is not a public revision (it is, 210106); and that
Xcode might already ship the surface (it does not, and the SDK carries none).

The pattern is worth naming rather than just recording: **each wrong guess was
about what a vendor had done, and each was cheap to check and was not checked.**
The section that guessed least — Emscripten, where the version was read rather
than inferred — is the only one that needed no correction.

**Neither working recipe was carried to a released payload.** Both were built
and exercised in a scratch directory. Turning each into an xim recipe — the
fetch, the substitution, the version check, the layout — is the next step and
is not done here.

**Nothing here shortens the platform work itself.** Sections 3 and 7 say which
layer is engine and which is package; they do not make the engine layer
smaller. Android is small because the model nearly admits it already, not
because a plugin can stand in for the row.

## 9. Review of sections 4 to 7, and what each risk is measured against

The proposal survives review with one correction, one omission that would have
been a silent defect, and one boundary the sections above draw in the wrong
place. They are stated here before the task list because each one moves a task.

### 9.1 The correction: `--stage-only` already shipped, under another name

Section 2.2(a) asks for "a `${mcpp.stage_dir}` placeholder, or a `--stage-only`
mode whose output path an action can name". The second half exists.
`--format dir` sets `writeArchive = false`, and `mcpp pack` then reports
`plan.stagingRoot` — `target/dist/<archive stem>/` — as the output. That tree
is the closure after the strip policy, the debug split and `include`/`exclude`
have run: the thing section 2.2(a) describes.

So only the placeholder is new. `--stage-only` would be a second spelling of a
mode that ships, which is the shape this project refuses elsewhere
(`mcpp sbom` versus `mcpp emit sbom`).

### 9.2 The omission: the requested format is a graph input, so it is graph state

`mcpp pack --format appimage` changes what the build program submits. Anything
that changes what a build program submits changes the graph, and
`target/<triple>/<fp>/build.ninja` is shared mutable state that two fast paths
replay. `mcpp.build.graph_shape` exists because of exactly this class of
defect, and it already carries two such axes: `graph=` (a test graph replayed
for a plain build) and `accel=` (a device variant a flag chose, replayed for a
build that chose nothing).

A `dist` axis with no entry on that line reproduces the same failure a third
time: `mcpp pack --format appimage`, then `mcpp build`, replays a graph
carrying a dist edge that a plain build must not have. The line therefore gains
a third field, and `is_plain_build_graph` requires it to read `none`.

The format deliberately does **not** enter the build fingerprint. It would put
the packaging pass in its own directory and cost a full recompile to produce a
distributable from an already-built tree. The header line is the cheaper half
of that pair and is the half that answers the question the fast paths ask.

### 9.3 The boundary in the wrong place: a staged tree is not a root filesystem

Section 2.1 lists `.deb`, `.rpm` and AppImage as one group and 2.2(a) offers
one staged tree to all of them. Two shapes are being conflated:

| Format | Wants |
|---|---|
| AppImage, `.app`, `.msi` | a **bundle** tree: `bin/`, `lib/`, relocatable, rooted anywhere |
| `.deb`, `.rpm` | an **FHS** tree: `usr/bin/`, `usr/lib/<pkg>/`, rooted at `/` |

`mcpp pack` stages the first — it is what `--mode vendored` means, and the
`$ORIGIN` rewriting and the `run.sh` wrapper are what make it relocatable. A
`.deb` member consuming that tree must re-lay it out, and the re-layout is
`.deb`'s knowledge rather than the engine's, so this is not an argument for a
second staged tree in the engine. It is an argument about which member goes
first: **a bundle-shaped format exercises `${mcpp.stage_dir}` as it is, and an
FHS-shaped one exercises a re-layout step that would then be the thing under
test.** Section 7's choice of AppImage for step 1 is right, and the reason is
this rather than "the platform where it is easiest to test".

### 9.4 The ordering problem sections 2.2 and 7 do not state

A `role = "artifact"` action is a ninja edge. The staged tree is produced by
`mcpp::pack::run` in C++ **after** ninja has finished. So an artifact action
cannot depend on the staged tree in the pass that builds it, and
`${mcpp.stage_dir}` is not expressible in a single-pass pack.

Two passes are, and every value the second one needs is already answered by the
first:

1. `prepare_build`, with no format set. Build programs run and declare the
   formats they provide; none submits a dist action, because none was asked
   for. An unknown `--format` is refused **here**, before anything is compiled,
   naming the set that was declared.
2. The ordinary build. The link outputs exist.
3. `make_plan` and `pack::run`. The staged tree exists at `plan.stagingRoot`.
4. `prepare_build` again, with `pack_format` and `pack_stage_dir` set to what
   steps 1 and 3 answered — **not re-derived**. The claiming member submits its
   action. Its command is what ninja runs.

The re-derivation is what would have been the defect. `plan.stagingRoot` is a
function of the package name, the version, the resolved triple and the mode,
and the resolved triple is not known until prepare has run. Computing it a
second time before prepare — from `host_triple()`, say — is the shape where two
derivations of one value agree on every machine the author has and disagree on
one they do not.

The second prepare is not free. It is bounded by the build program re-running:
the contract values are part of its re-run key unconditionally, so changing
`MCPP_PACK_FORMAT` invalidates exactly that one entry and nothing else.

### 9.5 The declaration must not be gated on the request

A member that emitted `mcpp:pack-format=appimage` only when
`pack_format() == "appimage"` would make the set unknowable: the engine could
never answer "which formats does this graph provide" and `--format bogus` could
name nothing. So the contract has two halves that must not be merged:

> **Declare unconditionally. Submit conditionally.**

This is the load-bearing rule of the whole dispatch, and it is the rule most
likely to be got wrong by a member author, because a member that gets it wrong
still works for the person who wrote it — they always pass their own format.
It therefore needs a test whose failure mode is the wrong half: a build that
requests **no** format and asserts the set is still non-empty.

### 9.6 Risks, each with the measurement that would catch it

| Risk | Why it would pass unnoticed | Criterion |
|---|---|---|
| The dist graph is replayed for a plain build | Both graphs live in one directory; the fast path predates any plan | `pack --format X`, then `build`, then assert no dist edge ran — the `A then B then A` shape |
| The second prepare replays a cached build program | A cached run re-emits the first pass's output, which submits nothing, so the pass succeeds and produces nothing | Assert the dist output **exists**, never that the command exited 0 |
| `${mcpp.stage_dir}` in a non-packing build | Expands to an empty string; the command then reads the build directory root, which exists | Refuse at expansion, and assert the refusal text |
| A member declares a built-in name (`tar`, `dir`) | The built-in wins and the member is silently unreachable | Refuse the collision, naming both |
| The dist action runs before staging | Only in a single-pass design; recorded so the two-pass ordering is not "simplified" away later | Assert the staged tree is non-empty **from inside the action** |
| A dist member's tool is absent | The tool is a host lookup, and an empty path becomes an argv token | Refuse in the build program, naming the tool and where it was looked for |
| The produced distributable is valid and empty | Section 2's measured 52 KB installer | Each member asserts a floor on its own output, on the **success** path, through `mcpp::warning` |

### 9.7 The engine's half of section 3, and what is left after it

Section 3's boundary is exact: a package can add a language, a tool, an action,
a payload and a generated module, and it **cannot add a triple**. Every layer
below the first — the `.apk` step, the `.app` step, the `.html`+`.wasm` step,
the runner, the signing, the non-C++ glue — therefore waits on a row in
`kKnownTargets` and on nothing else in the engine. So the rows are engine work
and belong in the same release as §2.2:

| Row | Tier | What it still needs |
|---|---|---|
| `aarch64-linux-android`, `x86_64-linux-android` | `planned` | `xim:android-ndk` |
| `aarch64-ios` | `planned` | the iPhoneOS SDK, and a licence reading first (§9.8) |
| `wasm32-emscripten` | `planned` | `xim:emsdk` |

**`planned` is a refusal, not a gap.** The tier gate answers `tier-planned`
naming the row, so `mcpp build --target aarch64-linux-android` says the
vocabulary has this target and nothing is wired yet — rather than `unknown
target`, which was false, or a build that resolves and produces nothing, which
§3.1 argues would be worse than the row's absence. Each cell is declared in
`tests/matrix/expected.tsv`, so the day a row is wired the matrix goes red and
says so.

**Web needed one thing the other two did not, and it was not a table row.** The
binary format was never a field: it was re-derived from `os` at each site that
needed it, which is affordable while the answer has two values. `wasm32` is the
first target whose format is neither, and a third value turns those derivations
into an addition at every such site — where a missed site does not fail, it
silently answers ELF. `ObjectFormat` is that addition made once. This is the
substance of [#597](https://github.com/mcpp-community/mcpp/issues/597)'s
"changes the target model rather than extending a table", and with §3.1 having
answered the standard-library half, #597 is now one problem rather than two.

**Android's placement is the modelling decision.** `env = "android"` on a
`linux` OS, not `os = "android"`: the kernel is Linux, so ELF, the `unix`
family and `nasm -f elf64` are already right, and an OS value would have made
every one of them wrong by default and required a new answer at each site. What
differs from `gnu` is bionic, the loader path and the SDK, which is what an
`env` value is for.

What remains outside this pass, each blocked on something no engine work
supplies:

- **`xim:android-ndk`, `xim:emsdk`** (§7 step 5). Both recipes are measured
  working in a scratch directory (§3.1); turning either into a payload means
  fetching and republishing a multi-gigabyte vendor toolchain with a derived
  133-file module surface. That is its own release, not a side effect of this
  one — and the rows landing first is exactly §7's ordering argument, which
  said doing the payloads first would make each row small. The rows turned out
  to be the cheap half either way.
- **The iPhoneOS SDK.** Not measurable on a Linux host, and §9.8 gives the
  decision procedure rather than the measurement.
- **`dist-android`, `dist-web`**. Each waits on its payload, not on its row.

### 9.8 The iOS SDK: a three-tier policy rather than an open question

Section 3.1 leaves `xim:iphoneos-sdk` as "licensing decides whether it installs
or merely locates", and §8 repeats it. The decision procedure is not a
measurement — it is a preference order, and stating it removes the open
question without taking the measurement:

1. **Redistribute, if the licence permits it.** A published payload with a
   GitCode mirror, like every other `xim` toolchain package. Publicly mirrored
   SDK trees exist — `https://github.com/xybp888/iOS-SDKs` is one — and whether
   this tier is reachable is a licence reading of the SDK itself, not of the
   mirror.
2. **Fetch from upstream, without a CN mirror.** A recipe that downloads at
   install time from the upstream URL and mirrors nothing. This is what a
   licence that permits use but not redistribution allows, and declining the
   mirror is the point: a mirrored copy *is* redistribution, so the tier is
   defined by what it refuses to do.
3. **Locate what the machine already has.** The `msvc@system` shape: find, pin
   and report an installed Xcode, install nothing. Correct under any licence,
   and the only tier that cannot serve a machine without Xcode.

Tier 3 always works and is therefore the floor, not the goal. The recipe should
reach for the lowest tier the licence allows and say in its description which
tier it took, because a consumer reading "locator" needs to know that is a
licence conclusion rather than an unfinished recipe.

## 10. The task list

Four repositories. One pull request each, in this order, because each depends
on the one above it being released rather than merely merged.

### 10.1 `mcpp` — the three format-neutral additions

| # | Task | Depends on |
|---|---|---|
| E1 | `MCPP_PKG_VERSION` / `_DESCRIPTION` / `_LICENSE` / `_AUTHORS` / `_REPO` in the build-program environment, with accessors | — |
| E2 | `MCPP_PACK_FORMAT` in that environment, and `mcpp::pack_format()` | — |
| E3 | The `mcpp:pack-format=<name>` outlet, collected onto the plan | — |
| E4 | `${mcpp.stage_dir}`: expansion for Artifact actions, refusal elsewhere, the stage manifest as an implicit input | E2 |
| E5 | `--format` accepts a provided name; the refusal names the available set | E3 |
| E6 | The two-pass pack pipeline (§9.4) | E4, E5 |
| E7 | `dist=` on the graph header line; `is_plain_build_graph` requires `none` | E6 |
| E8 | `docs/10`, `docs/30`, `docs/31`, and a new `docs/35`; the `zh` mirror of each | E1–E7 |
| E9 | Unit tests for E3–E5, e2e for E6–E7, each with the §9.6 criterion | E1–E7 |

### 10.2 `mcpp-plugins` — one member per verified platform

| # | Task | Depends on |
|---|---|---|
| P1 | `tools-embed`: a `table()` entry point (§5) | — |
| P2 | `dist-appimage` → `mcpp.dist.appimage` | mcpp released, X1 |
| P3 | `dist-wix` → `mcpp.dist.wix` | mcpp released |
| P4 | `dist-apple` → `mcpp.dist.apple`, macOS half only | mcpp released |
| P5 | `MCPP_VERSION` in CI raised to the engine that carries E1–E7 | E1–E7 released |
| P6 | Plan-level tests on all three runners; end-to-end each on its own | P1–P4 |

### 10.3 `xim-pkgindex` — the one payload this needs

| # | Task | Depends on |
|---|---|---|
| X1 | `xim:appimagetool` | — |

### 10.4 `mcpp-index` — publication

| # | Task | Depends on |
|---|---|---|
| I1 | `mcpp:plugins@0.3.0` | P1–P6 released |

The engine tasks are the only ones on the critical path. P1 and X1 do not wait
on anything.

## 11. The plan read from nine angles

Section 9 reviewed the proposal on its own terms. This section reads the
*implemented* result from the angles a reviewer would apply independently of it,
because each angle catches a different class of mistake and several of them
caught one.

### 11.1 Architecture

The load-bearing claim is that **the engine holds the dispatch and no format**.
It survives one test the proposal did not anticipate: a format that consumes
nothing. `dist-wix` packages one named program and never reads the staged tree,
which is what §6's own guidance recommends — and the first implementation of the
dispatch refused exactly that member, because it identified the distributable by
"which action named `${mcpp.stage_dir}`". The criterion was a property of the
*mechanism* rather than of the *request*. It is now "which artifact actions the
request introduced", which needs nothing of the member.

The same mistake occurred one layer down and was found by a real macOS runner:
staging ran before the dispatch and its failure was fatal, so every dispatched
format was unreachable on a target whose built-in bundling is refused. Staging
is a service to the provider, not a precondition.

Both are the same error in different clothes: **the engine deciding something on
the provider's behalf.** That is the failure this architecture is most exposed
to, because the whole point of it is that the provider decides.

### 11.2 Stability

Three axes now ride `build.ninja`'s header line — shape, schedule, device
variant — and `dist=` is the fourth. Each was added after the same defect: a
graph written for one purpose replayed for another, in a directory the two
share. The format deliberately does not enter the fingerprint, because it would
cost a full recompile to package an already-built tree; the header line is the
cheaper half of that pair and is the half the fast paths ask.

The measurement that matters is the one that says the criterion is worth
having: on 2026-09-11 a plain build after a pack pass regenerates the graph
*even with the field ignored*, so an end-to-end assertion would pass whether or
not the field works. The unit test is where the invariant is held.

### 11.3 Elegance

Two additions were withdrawn as duplicates of something that ships.
`--stage-only` is `--format dir`. And `rule_module` on a `dist-*` feature was
refused by the engine, correctly: that key means "the module that reaches a
rule" and implies `device_extensions`, which a member compiling nothing cannot
have. The `dist-*` members take the `tools-*` shape instead, and the consumer
writes one line more than a rule needs — which says something true.

### 11.4 User experience

`--format` gained values rather than a second flag, because `tar`, `dir`, `msi`
and `appimage` answer one question. An unknown value names what *is* available
rather than a fixed list, and the refusal arrives before anything is compiled.

The failure mode this category is most exposed to is a step that succeeds while
carrying nothing — §2's measured 52 KB installer. Every member therefore
asserts a floor on the success path through `mcpp::warning`, because stderr on
a successful build is discarded. The first such floor was a size bound and was
wrong on its first real fixture: a stripped hello-world stages at 14999 bytes,
under a 16 KB bound, so a correct AppImage was reported as empty. A size is a
proxy for a question that can be asked directly.

### 11.5 Compatibility

The engine's rule is unchanged: no per-package floor exists, and the index-level
`min_mcpp` does not move for a package, because raising it makes the whole index
unreadable to clients stopped below it. What an older client gets is legible at
the point of use, and for these members it is the best case of that rule —
`mcpp::provides_pack_format` does not exist in an older engine's bundled module,
so a consumer fails at the `build.mcpp` **compile**, naming the missing
function, rather than at a link or in an artifact.

### 11.6 Cross-platform

Three members, three platforms, and the honest asymmetry is that only one of
them could be measured where it was written. `plan_for()` returning
`applies == false` on the wrong OS says the gate works and says nothing about
whether the tool accepts what the member renders. That gap was closed by adding
CI steps that actually run `wix build` and assemble a real `.app` — and the
first thing they did was fail, twice, for unrelated reasons: WiX 7 refuses to
run without an out-of-band licence acceptance (`WIX7015`), and the Mach-O
staging refusal above. Both are findings the plan-level assertions could not
have produced.

### 11.7 Consistency

`rules-*`, `tools-*` and `dist-*` is one taxonomy with one rule: the prefix
says which of three questions a member answers. The engine's own three-value
`ObjectFormat` is the same discipline applied to a fact rather than to a
package — the binary format was re-derived at roughly 35 sites, which is
affordable at two values and becomes an addition at every site at three, where
a missed site silently answers ELF.

Two sites answered the object format by searching for `"apple"` in a string
that never contains it, so an explicit `--target aarch64-macos` — a *verified*
row — linked and recorded as ELF while a native build on the same machine said
`macho`. One function, two paths, only the exercised one right.

### 11.8 Seamless upgrade

Every new field is absent-tolerant in the direction that matters. An older
graph's missing `dist=` reads as a miss and never as `none`. A build program on
an older engine gets empty strings from the new accessors, which a member reads
as "fall back to what you did before". The `pack-format` directive carries a
non-empty cache tag, so a declaration survives a cache hit — the pass that
reads it is `mcpp pack`, which is never a project's first build, and an
unpersisted declaration would be absent exactly when a user names a format.

`kCacheEpoch` is deliberately not bumped: an entry written before the row
carries no such line and the program that wrote it could not emit one, so
replaying it yields what that program said.

### 11.9 Test coverage

The count is not the measure; what each test excludes is. Two are worth naming.

`638` holds nine properties, each paired with the wrong answer it excludes, and
two of them were verified load-bearing by removing the guard and watching the
test fail — including the one that distinguishes "the request introduced this
action" from "any artifact action".

And one CI assertion was itself the defect: it grepped for `struct
embedded_file`, the *default* `row_type`, while the fixture sets
`row_type = "shader_entry"` precisely because that option exists. The code was
correct and the assertion was wrong, which is what a check tied to a spelling
the fixture chooses will eventually always be. It now reads the struct's name
out of the file and asserts the table's element type *is* that struct, for
every generated header rather than whichever `find` listed first.

### 11.10 The dependency order, and why it is not the demand order

    mcpp engine  ──────────────►  released, because plugins CI pins a release
        │
        ├─► xim payloads  ─────►  independent of the engine; merged first
        │
        └─► mcpp-plugins  ─────►  needs the release, so it cannot precede it
                │
                └─► mcpp-index  ►  needs the plugins tag's sha256

Four repositories, one pull request each. The engine is the only thing on the
critical path, and every measurement that changed the plan came from the layer
*above* it — which is the argument for doing the payloads early even though the
rows land last.
