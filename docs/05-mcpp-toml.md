# The mcpp.toml Manifest Guide

`mcpp.toml` is the project configuration file for the mcpp build tool, analogous to Cargo's `Cargo.toml` or Node's `package.json`. Place it in your project root and `mcpp build` will discover and read it automatically.

## 1. Minimal Examples

mcpp is designed around **convention over configuration** — most fields have sensible defaults, so the simplest `mcpp.toml` is just a few lines:

### 1.1 Executable (minimal)

```toml
[package]
name    = "hello"
version = "0.1.0"
```

mcpp infers automatically:
- Source files: `src/**/*.{cppm,cpp,cc,c}`
- Entry point: `src/main.cpp` → produces the `hello` binary
- Standard: C++23
- Modules: scans `export module ...` declarations and builds the dependency graph automatically

### 1.2 Library project (minimal)

```toml
[package]
name    = "mylib"
version = "0.1.0"

[targets.mylib]
kind = "lib"
```

lib-root convention: the primary module interface defaults to `src/mylib.cppm` (the last segment of the package name).

## 2. Full Field Reference

### 2.1 `[package]` — Package Metadata

```toml
[package]
name        = "myapp"              # Package name (required)
version     = "0.1.0"              # Semantic version (required)
standard    = "c++23"              # C++ standard (default c++23; can be set to c++26)
description = "My awesome app"     # Description (optional)
license     = "MIT"                # License (optional)
authors     = ["Alice", "Bob"]     # Author list (optional)
repo        = "https://github.com/user/myapp"  # Repository URL (optional)
```

`standard` is the first-class setting for the C++ language standard. Recommended values:

- `c++23`: the default, suited to the current module-based default templates.
- `c++26`: use when you need C++26 language features.
- `c++2c`: a compatibility alias, normalized to `c++26` after parsing.
- `gnu++23` / `gnu++26`: use when you need a GNU dialect; this enters the fingerprint and the std BMI cache key.
- `c++latest`: follows the newest standard mcpp currently supports. Good for local experimentation, but not recommended for release packages that require reproducibility.

### 2.2 `[targets.<name>]` — Build Targets

```toml
# Executable (default; inferred automatically when src/main.cpp exists)
[targets.myapp]
kind = "bin"
main = "src/main.cpp"       # Optional, defaults to src/main.cpp

# Static library
[targets.mylib]
kind = "lib"

# Shared library
[targets.mylib]
kind = "shared"
soname = "libmylib.so.1"  # Optional: ELF/Mach-O ABI name; an alias of the same name is generated at runtime
```

`soname` is the ABI name for a shared library, analogous to `SOVERSION`/`SONAME` in
Autotools/CMake. On Linux, mcpp passes `-Wl,-soname,<name>` to the linker and
generates a `<name> -> lib<target>.so` alias in the output directory, so that
downstream programs can load the library via its standard ABI name through
`DT_NEEDED` or `dlopen()`. This field only applies to `kind = "shared"`, and the
value must be a filename basename.

### 2.3 `[build]` — Build Configuration

```toml
[build]
sources      = ["src/**/*.cppm", "src/**/*.cpp"]  # Source globs (default: src/**/*.{cppm,cpp,cc,c})
include_dirs = ["include", "third_party/include"]  # Header search paths
c_standard   = "c11"              # Standard for C source files (default c11)
cflags       = ["-DFOO=1"]        # Extra C compile flags
cxxflags     = ["-DBAR=2"]        # Extra C++ compile flags (do not put -std=... here)
ldflags      = ["-lfoo"]          # Extra link flags
static_stdlib = true               # Statically link libstdc++ (default true)
macos_deployment_target = "14.0"   # Minimum supported OS version for macOS artifacts (macOS only)
```

`macos_deployment_target` sets the minimum system version in the artifact's
Mach-O header (`LC_BUILD_VERSION minos`), i.e. the oldest macOS the binary can
run on. The precedence follows ecosystem convention: the `MACOSX_DEPLOYMENT_TARGET`
environment variable (an explicit per-invocation override, honored the same way by
cargo/rustc, cc, etc.) > this field (the project default, similar to SwiftPM's
`platforms:`) > the **built-in default `14.0`** (rustc-style — every target has a
baseline, and 14.0 is the floor of LLVM's official static libraries themselves).
This value enters the BMI fingerprint, so switching targets automatically rebuilds
the module cache.

**Static runtime by default (portable by default)**: when `static_stdlib = true`
(the default), macOS linking statically links in LLVM's bundled libc++/libc++abi —
the system libc++ would otherwise pin the actual runnable version to the build
machine's OS (older systems lack newer symbols, e.g. the support symbols behind
`std::print`), and only static linking can truly deliver the floor. As a result,
the default build's artifacts work out of the box on any macOS ≥ 14. Set
`static_stdlib = false` to fall back to the dynamic system libc++ (the artifact is
then only guaranteed to run on the build machine's version and above). A lower
floor (11–13) requires a self-built libc++ archive (already verified to work, a
data-level switch, available on request).

Do not configure the C++ standard via `build.cxxflags = ["-std=..."]`. Instead use:

```toml
[package]
standard = "c++26"
```

mcpp applies the same standard to ordinary C++ compilation, module scanning,
`compile_commands.json`, and the standard library BMI build for `import std`.

**glob exclusion** (`!` prefix, mcpp 0.0.4+):

```toml
[build]
sources = [
    "src/**/*.cpp",
    "!src/**/*_test.cpp",       # Exclude test files
    "!src/**/*_fuzzer.cpp",     # Exclude fuzzers
]
```

### 2.4 `[lib]` — Library Root Module Convention

```toml
[lib]
path = "src/capi/lua.cppm"    # Override the default lib-root location
```

Default convention: `src/<last segment of package name>.cppm` (e.g. package name `mcpplibs.cmdline` → `src/cmdline.cppm`).

### 2.5 `[dependencies]` — Runtime Dependencies

```toml
# Packages under the default package namespace (mcpplibs)
[dependencies]
gtest   = "1.15.2"              # Exact version
mbedtls = "3.6.1"
ftxui   = "6.1.9"

# Dotted selector: try mcpplibs.<path> first, then fall back to the sibling peer root.
# For example, imgui.core is tried in order as mcpplibs.imgui/core, then imgui/core.
[dependencies]
capi.lua = "0.0.3"
compat.gtest = "1.15.2"
imgui.core = "0.0.1"
imgui.backend.glfw_opengl3 = "0.0.1"

# Namespace sub-table form
[dependencies.mcpplibs]
cmdline   = "0.0.2"
tinyhttps = "0.2.2"
llmapi    = "0.2.5"

[dependencies.compat]
glfw = "3.4"                    # Explicit namespace, skips the mcpplibs-first candidate

# Path dependency (local development)
[dependencies]
mylib = { path = "../mylib" }

# Git dependency
[dependencies]
mylib = { git = "https://github.com/user/mylib.git", tag = "v1.0.0" }

# Long-form dep spec: features and backend knobs
[dependencies]
imgui = { version = "0.0.3", features = ["docking"] }   # Request a feature of this dependency
widget = { version = "1.0", backend = "glfw_opengl3" }  # Sugar for: features=["backend-glfw_opengl3"]
```

`backend = "<impl>"` is **general-purpose convention sugar**: it desugars 1:1 into
requesting the dependency's `backend-<impl>` feature (a library that supports this
knob should declare a `backend-*` family in its own `[features]`). If the target
package declares `[features]` but does not include the requested feature (including
the result of backend desugaring), a warning is issued by default, and an error
under `mcpp build --strict`.

**SemVer constraints**:

```toml
[dependencies]
foo = "^1.2.3"      # >= 1.2.3, < 2.0.0 (caret, default)
bar = "~1.2.3"      # >= 1.2.3, < 1.3.0 (tilde)
baz = "=1.2.3"      # Exact match
qux = ">=1.0, <2.0" # Range combination
```

### 2.6 `[dev-dependencies]` — Test Dependencies

```toml
[dev-dependencies]
gtest = "1.15.2"
```

`mcpp build` ignores these; `mcpp test` resolves and uses them. `mcpp test` automatically discovers `tests/**/*.cpp` and compiles them into test binaries.

### 2.7 `[toolchain]` — Toolchain Configuration

```toml
[toolchain]
default = "gcc@16.1.0"

# Cross-compilation target override
[target.x86_64-linux-musl]
toolchain = "gcc@15.1.0-musl"
linkage   = "static"
```

### 2.8 `[features]` — Features (Cargo-style, additive)

```toml
[features]
default = ["base"]        # Default activation set
base    = []
docking = ["extra"]       # Activating docking implies activating extra (transitive closure)
extra   = []
```

- Activation sources: the package's own `default` set ∪ explicit requests (the root
  package via `mcpp build --features a,b`; dependencies via the long-form dep spec's
  `features = [...]` / `backend = "..."` sugar).
- Each activated feature gets the macro `-DMCPP_FEATURE_<NAME>` during that package's
  compilation (the name is uppercased and non-alphanumerics become `_`, e.g.
  `backend-a` → `MCPP_FEATURE_BACKEND_A`).
- **strict validation**: when the target package declares a `[features]` table,
  requesting an undeclared feature produces a warning; an error under `--strict`. A
  package that does not declare `[features]` accepts any request (pure macro usage).

### 2.9 `[profile.<name>]` — Build Profiles

```toml
[profile.dist]
opt      = 3              # -O level (a number, or the string "s"/"z")
debug    = false          # -g
lto      = true           # -flto (note: some packaged gcc builds ship without the LTO plugin)
strip    = true           # -s at link time
# passthrough escape hatch (fixed keys, open values):
cflags   = ["-fno-plt"]
cxxflags = ["-fno-plt"]
ldflags  = []
```

- Selection: `mcpp build --profile <name>`, defaulting to `release`.
- Built-in profiles: `release` (-O2) / `dev`, `debug` (-O0 -g) / `dist` (-O3 + strip;
  **LTO is not enabled by default**). `[profile.<built-in name>]` can override a
  built-in definition wholesale.

### 2.10 `[runtime]` — Host Runtime Capabilities

```toml
[runtime]
library_dirs = ["vendor/lib"]            # Directories baked into the artifact's RUNPATH (relative to the package root)
dlopen_libs  = ["libGL.so.1"]            # sonames dlopen'd at runtime (validated by doctor)
capabilities = ["opengl.glx.driver"]     # Host capabilities required (open namespace)
provides     = ["opengl.glx.driver"]     # Explicitly declares capabilities this package fulfills (strong provider)

# Explicit provider override (the "explicit" notch of the three-way knob)
[runtime."opengl.glx.driver"]
provider = "compat.glx-runtime"
```

- **Provider selection**: a package that declares `provides` (strong) takes precedence
  over one that merely lists a capability under `capabilities` (weak, backward
  compatible); `[runtime.<cap>] provider=` is an explicit override with the highest
  precedence, and pointing at a provider not present in the dependency graph produces
  a warning.
- The resolved result can be inspected via `mcpp why runtime`, `mcpp self doctor`, and
  the build artifact `target/<triple>/<fp>/resolution.json` (it is not magic by
  default).
- Capability naming convention: layered lowercase `domain.sub.role` (e.g.
  `opengl.glx.driver`, `x11.display`) and prefix-style `abi:<name>` (e.g. `abi:glibc`,
  which participates in toolchain ABI enforcement).

### 2.11 `[package] platforms` — Platform Declaration

```toml
[package]
platforms = ["linux", "macos", "windows"]
```

Declares the platforms the package supports (a CI matrix hint, shown via `mcpp why`).
The vocabulary is fixed by mcpp (which owns the target/triple system):
`linux | macos | windows`; unknown values produce a warning, and an error under
`--strict`.

## Appendix A. Schema Ownership Principle (admission criteria for new fields)

> **Closed syntax, open vocabulary**: whoever owns the parsing semantics defines the keys; whoever owns the domain knowledge defines the values.

- mcpp only defines **mechanisms** (feature union/closure, capability
  require/provide/override, profile→compiler flags, platform→triple); the keys and
  shapes are fixed. Domain vocabulary such as feature names, capability names, and
  backend names **appears only in values**, never in mcpp's code.
- **Package-custom toml keys are not supported**: key legitimacy must not depend on
  "first parsing the target package," otherwise the manifest loses static
  parseability (a prerequisite for lockfiles/LSP/auditing). A package's extension
  point = open value domains within fixed mechanisms.
- Package-level knobs all converge into features; for sugar keys (such as `backend=`)
  to enter the core syntax, they must satisfy: ① domain-neutral (a cross-ecosystem
  general pattern) ② 1:1 desugaring with zero new parsing semantics.
- See `.agents/docs/2026-06-04-manifest-schema-ownership.md` for the full field-ownership
  table and the finalized decisions.

## 3. Worked Examples

### 3.1 Simple Hello World

```toml
[package]
name    = "hello"
version = "0.1.0"
```

```cpp
// src/main.cpp
import std;
int main() { std::println("Hello, mcpp!"); }
```

```bash
mcpp build && mcpp run
```

### 3.2 Module-based Library + Tests

```toml
[package]
name    = "mymath"
version = "1.0.0"

[targets.mymath]
kind = "lib"

[dev-dependencies]
gtest = "1.15.2"
```

```cpp
// src/mymath.cppm
export module mymath;
export int add(int a, int b) { return a + b; }
```

```cpp
// tests/test_add.cpp
#include <gtest/gtest.h>
import mymath;
TEST(Math, Add) { EXPECT_EQ(add(1, 2), 3); }
```

```bash
mcpp build   # Compile the library
mcpp test    # Compile + run tests
```

### 3.3 An Application Depending on Other Packages

```toml
[package]
name    = "myapp"
version = "0.1.0"

[dependencies]
ftxui = "6.1.9"

[dependencies.mcpplibs]
cmdline = "0.0.2"
llmapi  = "0.2.5"
```

mcpp automatically:
1. Downloads source tarballs from mcpp-index
2. Propagates header search paths per `[build].include_dirs`
3. Pulls transitive dependencies into the graph (llmapi → tinyhttps → mbedtls, fully automatic)

### 3.4 Pure C Library

```toml
[package]
name    = "myc"
version = "0.1.0"

[build]
c_standard   = "c99"
include_dirs = ["include"]
sources      = ["src/**/*.c"]

[targets.myc]
kind = "lib"
```

### 3.5 Mixed C / C++23 Module Project

```toml
[package]
name    = "hybrid"
version = "0.1.0"

[build]
include_dirs = ["include"]
c_standard   = "c11"

[dependencies]
lua = "5.4.7"     # Pure C library; mcpp compiles .c files with the C compiler automatically

[targets.hybrid]
kind = "bin"
```

### 3.6 Cross-Compiled Static Release

```toml
[package]
name    = "mytool"
version = "1.0.0"

[toolchain]
default = "gcc@16.1.0"

[target.x86_64-linux-musl]
toolchain = "gcc@15.1.0-musl"
linkage   = "static"
```

```bash
mcpp build --target x86_64-linux-musl
# → Produces a fully statically linked binary that can be scp'd directly to any Linux x86_64 machine and run
```

## 4. Conventions and Defaults Cheat Sheet

| Item | Default | Notes |
|---|---|---|
| Source files | `src/**/*.{cppm,cpp,cc,c}` | Scanned recursively and automatically |
| Entry point | `src/main.cpp` | If this file exists, a `bin` target is inferred |
| Library root | `src/<pkg-tail>.cppm` | Override with `[lib].path` |
| C++ standard | `c++23` | Configure with `[package].standard`; supports `c++26` / `c++2c` |
| C standard | `c11` | `.c` files go through the C compiler automatically |
| Static stdlib | `true` | Portable binary |
| Headers | `include/` (if present) | Added to `-I` automatically |
| Tests | `tests/**/*.cpp` | Discovered automatically by `mcpp test` |
| Dependency namespace | `mcpp` (default) | The flat form uses the default ns |

### 4.1 Legacy `[language]` Compatibility Layer

The old configuration is still readable:

```toml
[language]
standard = "c++26"
```

New projects should use `[package].standard`. If both locations are present, `[package].standard` is authoritative.
