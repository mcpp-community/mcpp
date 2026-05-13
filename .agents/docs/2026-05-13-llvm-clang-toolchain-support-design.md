# MCPP LLVM/Clang Toolchain Support Analysis and Design

Date: 2026-05-13

## Summary

xlings now provides the Linux LLVM toolchain package MCPP needs to start
supporting Clang as a first-class compiler family. I installed
`llvm@20.1.7` into MCPP's isolated registry and verified the package layout.

The main conclusion is that MCPP should not add Clang support as scattered
`if clang` branches. Current build behavior is tightly coupled to GCC:
`bits/std.cc`, `gcm.cache/*.gcm`, `-fmodules`, GCC P1689 flags,
`-static-libstdc++`, and binutils `ar` are all assumed in different modules.
Clang support needs a small toolchain abstraction layer that separates
toolchain resolution, executable launch environment, compiler probing,
standard-library module handling, module BMI dialect, and link/runtime
policy.

## Installed Toolchain

Command used from this repository:

```bash
target/x86_64-linux-gnu/407914674e90df09/bin/mcpp toolchain install llvm 20.1.7
```

Result:

```text
Installed llvm@20.1.7 -> /home/speak/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/bin/clang++
```

MCPP isolated environment:

```text
MCPP_HOME    = /home/speak/.mcpp
xlings home  = /home/speak/.mcpp/registry
llvm root    = /home/speak/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7
```

`mcpp toolchain list` now sees the installed toolchain:

```text
Installed:
     TOOLCHAIN               BINARY
     llvm 20.1.7             @mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/bin/clang++
```

Before this implementation, MCPP CLI did not list LLVM in the `Available`
section because `cmd_toolchain list` explicitly enumerated only `gcc` and
`musl-gcc`. Exact install still worked because the install path could resolve
`xim:llvm@20.1.7`.

## xlings Package Findings

The relevant package file is:

```text
/home/speak/.mcpp/registry/data/xim-pkgindex/pkgs/l/llvm.lua
```

Important package facts:

- Package name: `llvm`
- Version: `20.1.7`
- Linux artifact: `llvm-20.1.7-linux-x86_64.tar.xz`
- Declared Linux dependencies: `xim:glibc@2.39`, `xim:linux-headers@5.11.1`
- Tools: `clang`, `clang++`, `lld`, `ld.lld`, `llvm-ar`, `llvm-ranlib`,
  `llvm-nm`, `llvm-objdump`, `llvm-readobj`, `llvm-strip`, etc.
- Config files: `bin/clang.cfg`, `bin/clang-20.cfg`, `bin/clang++.cfg`
- C++ runtime: bundled libc++/libc++abi/libunwind under
  `lib/x86_64-unknown-linux-gnu`
- Compiler runtime: compiler-rt under `lib/clang/20/lib/x86_64-unknown-linux-gnu`

`clang++.cfg` is the key package integration point:

```text
--sysroot=/home/speak/.mcpp/registry/subos/default
-Wl,--dynamic-linker=/home/speak/.mcpp/registry/subos/default/lib/ld-linux-x86-64.so.2
-Wl,--enable-new-dtags,-rpath,/home/speak/.mcpp/registry/subos/default/lib
-Wl,-rpath-link,/home/speak/.mcpp/registry/subos/default/lib
-fuse-ld=lld
--rtlib=compiler-rt
--unwindlib=libunwind
-nostdinc++
-stdlib=libc++
-isystem /home/speak/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/include/c++/v1
-isystem /home/speak/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/include/x86_64-unknown-linux-gnu/c++/v1
-L/home/speak/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/lib/x86_64-unknown-linux-gnu
-Wl,-rpath,/home/speak/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/lib/x86_64-unknown-linux-gnu
```

This is good for MCPP: the package already encodes the intended libc++,
compiler-rt, lld, sysroot, dynamic linker, and rpath choices. MCPP should not
reconstruct all of these manually if the compiler config file is present.

## Direct Usage Observations

Direct execution currently fails without an additional runtime path:

```text
clang++: error while loading shared libraries: libz.so.1: cannot open shared object file
```

With host zlib in `LD_LIBRARY_PATH`, the compiler itself works:

```bash
LD_LIBRARY_PATH=/lib/x86_64-linux-gnu \
  ~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/bin/clang++ --version
```

Output:

```text
clang version 20.1.7
Target: x86_64-unknown-linux-gnu
Configuration file: .../bin/clang++.cfg
```

Simple C and C++ builds work with the same launch environment:

```bash
LD_LIBRARY_PATH=/lib/x86_64-linux-gnu clang -std=c11 hello.c -o hello
LD_LIBRARY_PATH=/lib/x86_64-linux-gnu clang++ -std=c++23 hello.cpp -o hello
```

The produced C++ binary may also need `libatomic.so.1` at runtime. On this
machine, `libatomic.so.1` exists in the host system and in
`~/.mcpp/registry/data/xpkgs/xim-x-gcc-runtime/15.1.0/lib64`, but not in the
LLVM package rpath or default subos lib path. This means MCPP needs a
toolchain runtime environment concept instead of assuming an absolute compiler
binary is self-sufficient.

## Standard Library Module Findings

The LLVM package includes:

```text
lib/x86_64-unknown-linux-gnu/libc++.modules.json
```

It declares:

```json
{
  "logical-name": "std",
  "source-path": "../../share/libc++/v1/std.cppm"
}
```

and:

```json
{
  "logical-name": "std.compat",
  "source-path": "../../share/libc++/v1/std.compat.cppm"
}
```

However, the referenced files are not present in this installed package:

```text
share/libc++/v1/std.cppm          missing
share/libc++/v1/std.compat.cppm   missing
```

As expected, direct `import std` compilation fails:

```text
fatal error: module 'std' not found
```

This is an important integration decision:

- MCPP can support Clang for non-`import std` C++ and C sources first.
- First-class `import std` support requires either the LLVM package to ship
  `std.cppm`/`std.compat.cppm`, or MCPP must have a separate source provider
  for libc++ standard module sources.
- MCPP should report this as a clear capability failure, not as a generic
  compiler failure.

Official Clang/libc++ documentation treats standard library modules as
compiler/version/configuration-sensitive artifacts. The BMI is not portable
across arbitrary compiler or flag changes. Therefore MCPP's fingerprint model
must include the Clang resource dir, libc++ module source hash, and relevant
module flags.

References:

- https://clang.llvm.org/docs/StandardCPlusPlusModules.html
- https://libcxx.llvm.org/Modules.html

## Current MCPP Failure Modes

### 1. Absolute `clang++` Launch Fails

MCPP resolves the installed binary correctly:

```text
Resolved llvm@20.1.7 -> @mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/bin/clang++
```

But then `detect()` runs the absolute binary directly. Without a launch
environment, `clang++ --version` fails due `libz.so.1`.

Implication: a resolved toolchain must carry an execution environment, not only
a path.

### 2. Clang Can Be Misdetected as GCC

With `LD_LIBRARY_PATH=/lib/x86_64-linux-gnu`, detection gets past the loader,
but current `detect.cppm` can classify Clang as GCC because it searches the
entire `--version` output for the substring `gcc`. Clang's version output
contains:

```text
Configuration file: .../bin/clang++.cfg
```

That suffix includes `gcc` inside `clang++.cfg`. The detected label becomes:

```text
gcc 6082 (x86_64-unknown-linux-gnu)
```

`6082` is extracted from the LLVM git commit hash on the first line.

Implication: detection must check Clang before GCC and parse only reliable
version fields. It should not substring-match the entire output.

### 3. GCC std Module Is Mandatory

Even when `[language].import_std = false`, `prepare_build()` calls
`toolchain::ensure_built()` unconditionally. That function requires
`Toolchain.stdModuleSource`, currently populated only for GCC `bits/std.cc`.

Failure:

```text
toolchain has no std module source (import std unsupported on this compiler)
```

Implication: standard module preparation must be capability-driven and only
required when the graph imports `std` or `std.compat`.

### 4. Ninja Rules Are GCC-Specific

The current Ninja backend assumes:

- `-fmodules`
- `-fdeps-format=p1689r5`
- BMI suffix `.gcm`
- `gcm.cache/<module>.gcm`
- staged `std.gcm` and `std.o`
- GCC restat behavior around `.gcm`

Clang uses a different module dialect:

- BMI suffix `.pcm`
- explicit module output or precompile flows
- `-fmodule-file=<name>=<pcm>` or prebuilt module paths
- `clang-scan-deps` for robust dependency scanning
- libc++ standard module sources, not GCC `bits/std.cc`

## Design Goals

1. Make LLVM/Clang a first-class MCPP toolchain family.
2. Keep GCC and musl-GCC behavior stable.
3. Avoid hardcoding one compiler's module model into the generic build plan.
4. Preserve xlings isolation: builds should not accidentally depend on system
   compiler PATH.
5. Keep toolchain-specific complexity behind explicit interfaces.
6. Support staged delivery: first Clang detection and non-`import std` builds,
   then Clang modules, then Clang `import std`.

## Proposed Architecture

### Toolchain Request

Normalize user-facing specs before xlings resolution.

```cpp
struct ToolchainRequest {
    std::string family;        // "gcc", "llvm", "clang", "musl-gcc", "msvc"
    std::string version;       // "20.1.7"
    std::string libcFlavor;    // "", "musl"
    std::string xpkgName;      // "gcc", "musl-gcc", "llvm"
    std::string displaySpec;   // "llvm@20.1.7"
};
```

Rules:

- `llvm@20.1.7` -> `xim:llvm@20.1.7`
- `clang@20.1.7` -> `xim:llvm@20.1.7`, display as `clang/llvm`
- `gcc@15.1.0-musl` -> `xim:musl-gcc@15.1.0`
- `musl-gcc@15.1.0` remains accepted

This removes duplicated parsing in `prepare_build`, `install`, `default`,
`remove`, and `list`.

### Toolchain Resolver

Return more than a compiler path.

```cpp
struct ToolchainPayload {
    std::string displaySpec;
    std::string xpkgName;
    std::string version;
    std::filesystem::path root;
    std::filesystem::path binDir;
    std::filesystem::path cxx;
    std::filesystem::path cc;
    std::filesystem::path ar;
    std::filesystem::path ranlib;
    std::filesystem::path scanner;
    std::vector<std::filesystem::path> runtimeLibraryDirs;
    std::map<std::string, std::string> env;
};
```

For LLVM:

- `cxx = bin/clang++`
- `cc = bin/clang`
- `ar = bin/llvm-ar`
- `ranlib = bin/llvm-ranlib`
- `scanner = bin/clang-scan-deps` if present
- `runtimeLibraryDirs` must include any dirs needed to launch compiler tools
  and linked binaries in MCPP's sandbox.

The resolver should inspect package layout and config files, not assume every
toolchain has binutils siblings.

### Toolchain Launcher

Every subprocess that invokes compiler-family tools should go through a
launcher helper.

```cpp
struct ToolInvocation {
    std::filesystem::path exe;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::filesystem::path cwd;
};
```

Why this is needed:

- LLVM's absolute `clang++` currently needs a runtime library path for
  `libz.so.1`.
- MCPP currently uses raw shell strings in detect, stdmod, scanner, and ninja.
- Ninja build files also need to embed the required environment.

Ninja can express this with rule command prefixes:

```ninja
rule cxx_object
  command = env LD_LIBRARY_PATH=$tool_ld_library_path $cxx $cxxflags -c $in -o $out
```

The exact env should come from the resolved toolchain profile.

### Compiler Profile

`toolchain::detect()` should produce a richer profile:

```cpp
enum class CompilerFamily { GCC, Clang, MSVC };
enum class StdlibFamily { Libstdcxx, Libcxx, MsvcStl, Unknown };
enum class ModuleDialect { GccGcm, ClangPcm, MsvcIfc, None };

struct CompilerProfile {
    CompilerFamily family;
    std::string version;
    std::string targetTriple;
    StdlibFamily stdlib;
    std::string stdlibVersion;
    ModuleDialect moduleDialect;
    std::filesystem::path cxx;
    std::filesystem::path cc;
    std::filesystem::path ar;
    std::filesystem::path ranlib;
    std::filesystem::path resourceDir;
    std::filesystem::path sysroot;
    ToolLaunchEnv launchEnv;
    StdModuleCapability stdModule;
};
```

Detection rules:

- Detect Clang before GCC.
- Parse `clang version X.Y.Z` from the first line.
- Parse GCC from `g++ (...) X.Y.Z` or `gcc version X.Y.Z`, not arbitrary
  trailing numbers.
- Do not infer `libc++` just because family is Clang; inspect config or query
  include/link search paths when possible.

### Standard Module Provider

Replace the single GCC-specific `ensure_built()` with provider strategies.

```cpp
class StdModuleProvider {
public:
    virtual bool available(const CompilerProfile&) const = 0;
    virtual Expected<StdModuleArtifacts, Error>
        ensure_built(const CompilerProfile&, std::string_view fp) = 0;
};
```

Providers:

- `GccStdModuleProvider`
  - Source: GCC `bits/std.cc`
  - Output: `std.gcm`, `std.o`
  - Flags: `-std=c++23 -fmodules`

- `ClangLibcxxStdModuleProvider`
  - Source: `libc++.modules.json` -> `std.cppm` / `std.compat.cppm`
  - Output: `std.pcm`, optional object if needed by the selected flow
  - Flags: `-std=c++23 -stdlib=libc++ -fexperimental-library` when required
  - Hard error if `libc++.modules.json` references missing files

Standard module building should be conditional:

- If graph imports `std` or `std.compat`, require provider availability.
- If no unit imports standard modules, skip std module prebuild entirely.

### Module Dialect

Move compiler-specific BMI naming and flags out of `ninja_backend.cppm`.

```cpp
class ModuleDialectRules {
public:
    virtual std::string bmi_filename(std::string_view logicalName) const = 0;
    virtual std::string cxx_module_flags(const CompileUnit&) const = 0;
    virtual std::string scan_rule() const = 0;
    virtual std::string compile_rule() const = 0;
};
```

GCC dialect:

- BMI dir: `gcm.cache`
- BMI suffix: `.gcm`
- Compile: `-fmodules`
- Scan: `-fdeps-format=p1689r5 -fdeps-file=...`

Clang dialect:

- BMI dir: `pcm.cache` or compiler-neutral `bmi.cache`
- BMI suffix: `.pcm`
- Compile interface: use `--precompile` or `-fmodule-output=<pcm>`
- Compile users: pass `-fmodule-file=<logical>=<pcm>` for known imports
- Scan: prefer `clang-scan-deps -format=p1689` when available

MVP fallback:

- If `clang-scan-deps` is unavailable, MCPP can use its existing graph scanner
  and static dependency edges for project modules.
- That fallback is acceptable for initial Clang support, but less precise than
  compiler-driven scanning.

### Link Policy

Move stdlib/runtime link flags out of generic `compute_flags()`.

```cpp
struct LinkPolicy {
    std::vector<std::string> cxxFlags;
    std::vector<std::string> cFlags;
    std::vector<std::string> ldFlags;
    std::filesystem::path linker;
    std::filesystem::path ar;
    std::filesystem::path ranlib;
};
```

GCC policy:

- `-static-libstdc++` when requested
- binutils `ar` from sibling package when available
- musl target implies static linkage unless explicitly overridden

LLVM policy:

- Do not add `-static-libstdc++`
- Use package config file for libc++/compiler-rt/lld where possible
- Prefer `llvm-ar` and `llvm-ranlib`
- Preserve or reproduce package rpath behavior for libc++ and subos libs
- Decide separately whether MCPP supports fully static LLVM/libc++ builds

## Proposed Implementation Phases

### Phase 1: Make LLVM Resolvable and Detectable

Scope:

- Add data-driven toolchain families: `gcc`, `musl-gcc`, `llvm`, `clang`.
- Make `clang@<ver>` alias to `llvm@<ver>`.
- Include `llvm` in `toolchain list` Available output.
- Resolve `cc`, `cxx`, `ar`, `ranlib` from package profile.
- Add toolchain launch env and use it in `detect()`.
- Fix compiler detection order and parsing.
- Stop unconditional std module prebuild.

Acceptance:

- `mcpp toolchain install llvm 20.1.7`
- `mcpp toolchain default llvm@20.1.7`
- `mcpp build` for a project with no `import std` should reach Ninja.
- Clear error if LLVM compiler launch env cannot be formed.

### Phase 2: Clang Non-Standard-Module Build

Scope:

- Add Clang flag/link policy.
- Use `clang` for `.c`, `clang++` for `.cpp/.cppm`.
- Use `llvm-ar` for archives.
- Generate Ninja commands with toolchain launch env.
- For projects that do not import `std`, skip `std.pcm` entirely.

Acceptance:

- Pure C project builds with LLVM.
- Header-based C++ project builds with LLVM.
- Mixed C/C++ project builds with LLVM.
- Static libraries use `llvm-ar`.
- `compile_commands.json` reflects Clang commands.

### Phase 3: Clang Project Modules

Scope:

- Add Clang PCM BMI layout.
- Emit module interface build edges producing `.pcm` plus objects.
- Pass explicit `-fmodule-file=<name>=<path>` for imports.
- Use existing MCPP graph scanner as initial dependency source.
- Add `clang-scan-deps` support once the tool is available in xlings.

Acceptance:

- Multi-module project without `import std` builds with LLVM.
- Incremental rebuild works when implementation changes.
- GCC behavior remains unchanged.

### Phase 4: Clang `import std`

Scope:

- Implement `ClangLibcxxStdModuleProvider`.
- Read `libc++.modules.json`.
- Validate source paths.
- Prebuild `std.pcm` and `std.compat.pcm`.
- Add std module artifacts to fingerprint inputs.

Dependency:

- The LLVM xpkg must ship `share/libc++/v1/std.cppm` and
  `share/libc++/v1/std.compat.cppm`, or MCPP must define another trusted source
  for them.

Acceptance:

- `import std; int main(){ std::println("ok"); }` builds with LLVM.
- Missing libc++ module sources produce a targeted diagnostic.

### Phase 5: Packaging and Runtime

Scope:

- Teach `mcpp pack` that libc++/libc++abi/libunwind are not manylinux system
  libraries.
- Bundle libc++ runtime in `bundle-project`, unless explicitly skipped.
- Ensure produced binaries do not depend on undeclared host paths.
- Revisit `libatomic` and `libz` handling.

Acceptance:

- LLVM-built dynamic package runs after extraction on a clean target host.
- `bundle-all` includes the correct loader/runtime libraries.

## Required Code Areas

Likely files to touch:

- `src/cli.cppm`
  - Toolchain spec parsing, install/list/default/remove, prepare_build.
- `src/toolchain/detect.cppm`
  - Robust family detection, profile fields, launch env.
- `src/toolchain/stdmod.cppm`
  - Split into provider strategies; make prebuild conditional.
- `src/build/flags.cppm`
  - Replace GCC-only flag assembly with profile/link policy.
- `src/build/ninja_backend.cppm`
  - Move module-specific rules into dialect emitters.
- `src/build/compile_commands.cppm`
  - Preserve Clang command/env assumptions for IDEs.
- `src/modgraph/p1689.cppm`
  - Rename GCC-specific implementation or add scanner strategy.
- `src/pack/pack.cppm`
  - Runtime bundling policy for libc++ and LLVM-built binaries.
- `tests/e2e`
  - Add LLVM install/detect/build tests; gate large downloads where needed.

## Test Plan

Unit tests:

- Parse `llvm@20.1.7`, `clang@20.1.7`, `gcc@15.1.0-musl`.
- Detect Clang from version output containing `clang++.cfg`.
- Verify Clang does not emit `-static-libstdc++`.
- Verify LLVM archive tool is `llvm-ar`.
- Verify std module prebuild is skipped when no source imports `std`.
- Verify missing libc++ `std.cppm` yields a specific diagnostic.

E2E tests:

- `mcpp toolchain install llvm 20.1.7` in isolated `MCPP_HOME`.
- `mcpp toolchain list` shows installed LLVM and available LLVM versions.
- Pure C project builds with LLVM.
- Header-based C++ project builds with LLVM.
- Mixed C/C++ project builds with LLVM.
- Modular project without `import std` builds with LLVM.
- `import std` test is skipped or expected-fails until std.cppm is available.
- `mcpp pack` bundle-project includes libc++ runtime for LLVM-built binaries.

## Open Package Questions

These should be resolved with the xlings package owner before implementing
Phase 4 and Phase 5:

1. Should `llvm` declare or bundle a provider for `libz.so.1` so direct
   absolute `bin/clang++` execution works inside MCPP without host
   `LD_LIBRARY_PATH`?
2. Should `llvm` include `share/libc++/v1/std.cppm` and
   `share/libc++/v1/std.compat.cppm`, since `libc++.modules.json` references
   them?
3. Should `llvm` or `llvm-tools` include `clang-scan-deps`? It is the natural
   tool for Clang module dependency scanning.
4. Should LLVM-built binaries depend on `libatomic.so.1`, or should compiler-rt
   and package rpaths avoid that dependency?

## Recommended Path

Start with Phase 1 and Phase 2. They establish the architecture and make LLVM
usable for non-`import std` projects without committing to the full Clang module
pipeline immediately.

Do not wire Clang into the current GCC `gcm.cache` path. That would create a
fragile hybrid that fails later around BMI format, std module sources,
fingerprints, and incremental rebuilds.

The central design rule is:

```text
BuildPlan stays compiler-neutral.
CompilerProfile + ModuleDialect + StdModuleProvider + LinkPolicy own the
compiler-specific behavior.
```

## Implementation Status

This PR implements the first practical slice of the design:

- `llvm@20.1.7` and `clang@20.1.7` resolve to the xlings `llvm` package.
- Clang detection checks Clang before GCC and only classifies GCC from the
  version banner, avoiding false positives from `gcc-runtime` paths in
  `clang++.cfg`.
- Toolchain metadata now carries compiler launch library paths and output
  link runtime paths.
- Ninja rules execute compiler, linker, and archive commands through the
  toolchain launch environment when needed.
- Clang builds use `clang`, `clang++`, `llvm-ar`, no GCC P1689 dyndep scan,
  and no `-static-libstdc++`.
- `import std` prebuild is conditional on the source graph actually importing
  `std` or `std.compat`.
- If source imports `std` with LLVM today, MCPP reports that the toolchain has
  no std module source. This remains blocked on the xlings LLVM package
  shipping libc++ `std.cppm` / `std.compat.cppm`.

Validated behavior in this implementation:

- Header-based C++ and mixed C/C++ projects build and run with
  `llvm@20.1.7`.
- `clang@20.1.7` works as an alias for the same package.
- Existing GCC/musl-GCC unit and e2e coverage remains intact, except that the
  pack e2e now pins glibc GCC explicitly so it is independent of the user's
  global default toolchain.
