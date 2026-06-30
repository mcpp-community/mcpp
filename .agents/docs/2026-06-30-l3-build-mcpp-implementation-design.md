# L3 `build.mcpp` — native imperative build program (implementation design)

Companion to `2026-06-29-manifest-environment-and-platform-design.md` (§L3). This
doc nails down the concrete MVP shipped in mcpp 0.0.78.

## What it is

A project-local `build.mcpp` (a C++ source file, Zig's `build.zig` / Cargo's
`build.rs` model — but in the project's own language, so no second language and it
dogfoods mcpp). mcpp compiles it with the **host** toolchain and runs it **before**
the main build; the program emits stdout directives that augment the main build.

```cpp
// build.mcpp
#include <cstdio>
int main() {
    std::puts("mcpp:cxxflag=-DHAVE_FEATURE=1");
    std::puts("mcpp:link-lib=m");
    std::puts("mcpp:rerun-if-env-changed=USE_FAST");
}
```

## Directive protocol (Discipline 1 — structured output, not global mutation)

The program communicates **only** via stdout lines; everything else is ignored
(so the program may freely log to stderr/stdout). Recognized directives:

| Directive | Effect |
|---|---|
| `mcpp:cxxflag=<flag>`         | append `<flag>` to `buildConfig.cxxflags` |
| `mcpp:cflag=<flag>`           | append `<flag>` to `buildConfig.cflags` |
| `mcpp:link-lib=<name>`        | append `-l<name>` to `buildConfig.ldflags` |
| `mcpp:link-search=<dir>`      | append `-L<abs dir>` to `buildConfig.ldflags` (dir resolved against the project root) |
| `mcpp:cfg=<name>`             | append `-D<name>` to **both** cflags and cxxflags |
| `mcpp:generated=<path>`       | add `<path>` (relative to project root) to `buildConfig.sources` so the modgraph scanner picks it up |
| `mcpp:rerun-if-changed=<path>`| declare a file input (re-run gate, see Discipline 2) |
| `mcpp:rerun-if-env-changed=<VAR>` | declare an env input (re-run gate) |

It *requests* graph edges (flags/libs/sources); it never silently mutates build state.
Unknown `mcpp:` directives are ignored with a one-line warning (forward-compat).

## Declared-I/O re-run contract (Discipline 2 — fixes the `.mcpp_ok` blind spot)

The program is **not** re-run every build. Its parsed directives + declared inputs
are cached at `<proj>/.mcpp/build.mcpp.cache`. On each build we re-run iff:

- the cache is missing, **or**
- the `build.mcpp` source content hash changed, **or**
- the host compiler identity changed, **or**
- any declared `rerun-if-changed` file's content hash changed (or the file vanished), **or**
- any declared `rerun-if-env-changed` variable's current value changed, **or**
- any `generated=` output path no longer exists.

Otherwise the cached directives are reused without recompiling/running. This is the
documented replacement for the bare `.mcpp_ok` success marker ("process exited 0 ≠
outputs correct"): a **declared-input / declared-output contract**. Hashing reuses
the existing FNV-1a helpers (`mcpp::toolchain::hash_file` / `hash_string`).

Because the applied directives land in `buildConfig.{cflags,cxxflags,ldflags}` —
which already feed `canonical_compile_flags` → the fingerprint — and generated
sources feed the modgraph, the **main** build is automatically sensitive to a
changed `build.mcpp` output. The cache only avoids needless re-execution / file
regeneration (which would otherwise bump mtimes and force spurious rebuilds).

## Constraints (à la carte + supply-chain)

- **Leaf only.** `build.mcpp` chooses flags/sources/codegen and emits link
  requirements; it must **not** gate the top-level dependency graph (that stays in
  the applicative L1 `[target.'cfg(...)']` tables). The directive set deliberately
  excludes "add a registry dependency".
- **Host build, target cfg.** It compiles+runs on the **host**. The MVP therefore
  runs it only for **native** builds; under an explicit cross `--target` it is
  **skipped with a warning** (compiling it with the cross frontend would yield a
  binary that can't run on the host). Host-toolchain-for-cross is a follow-up.
- **Isolation.** Executed as a build action: child-only env (no calling-process
  mutation, via `capture_exec`), declared inputs/outputs. Extending the same
  declared-I/O contract to recipe `install()` is future work.

## Integration (src/build/prepare.cppm)

New module `src/build/build_program.cppm` exports
`run_build_program(Manifest&, root, hostCompiler, cppStandard)`. Called from
`prepare.cppm` right after toolchain detection (`tc`), i.e. **after** target
resolution + the L1 cfg-flag merge (buildConfig flags final) and **before** the
modgraph scanner (so `generated=` sources are scanned). Compile line:

```
<hostCompiler> -std=<cppStandard> -O0 -o <proj>/.mcpp/build.mcpp.bin <proj>/build.mcpp
```

Compile/run failures are hard errors surfaced with captured output.

**Host toolchain flags (sysroot).** A bare `g++ build.mcpp -o bin` works on a warm
dev box but fails on a fresh sandbox: the sandbox compiler can't find crt/libc
without the sysroot wiring the main build adds. So the compile reuses the host
subset of that wiring from the resolved `Toolchain` (`host_base_flags`): GCC gets
`--sysroot=<tc.sysroot>` (or, with no sysroot, the glibc-payload `-idirafter` /
`-B` / `-L`) plus binutils `-B` and the link-runtime `-L`/`-rpath` dirs; Clang
trusts its sibling `.cfg`. This mirrors `flags.cppm`'s GCC branch (kept a small
parallel copy rather than refactoring the platform-sensitive `compute_flags`
pre-release — a future unification should share one helper).

**Artifacts under `target/`.** The compiled program + the declared-input cache live
at `target/.build-mcpp/{build.mcpp.bin, build.mcpp.cache}` (a stable, non-
fingerprint-keyed subdir, since build.mcpp runs before the fingerprint exists), so
they persist across builds and aren't rebuilt needlessly.

## Tests

- `tests/e2e/89_build_mcpp.sh` — a `build.mcpp` emitting a `cxxflag` define + a
  `generated` source; assert the define reaches the TU (a `#ifdef` gate) and the
  generated source links. Second build asserts the cache short-circuits re-run;
  touching a declared `rerun-if-changed` input forces re-run.

## Forward note — `.mcpp` as a first-class C++ extension

The compiler doesn't know the `.mcpp` extension, so we compile build.mcpp with an
explicit `-x c++` (otherwise the driver hands it to the linker as a "linker
script"). This is a special case of a broader convention worth adopting: **inside
an mcpp project, `.mcpp` is just C++.** A natural next step is to add `.mcpp` to the
main build's source glob (`src/**/*.{cppm,cpp,cc,c}` → `+ .mcpp`) with the same
`-x c++` treatment, so a project may use `.mcpp` for ordinary sources/modules — the
extension becomes a marker of "an mcpp-native C++ file" rather than a separate
language. `build.mcpp` is the first instance; the `-x c++` handling here is the
seed. Deferred (out of MVP scope) but the direction is intentional.

## Forward note — typed `import mcpp;` library (Zig-style code API over the wire protocol)

The stdout `mcpp:` text protocol is the **substrate**: it decouples `build.mcpp`
from mcpp's ABI/version, is language-agnostic, and ignores unknown directives
(forward-compatible). This is the Cargo `build.rs` model. Zig sits at the other
end — `build.zig` constructs the graph through a typed `std.Build` **library**.

The chosen direction is the hybrid both ecosystems converge on (cf. Rust's
`build-rs` crate): **keep the text protocol as the wire format, and ship a thin
typed `import mcpp;` module on top** that just emits those strings. So instead of

```cpp
import std;
int main() { std::puts("mcpp:link-lib=m"); }
```

a user writes the modules-first, no-headers form:

```cpp
import mcpp;                      // bundled in the mcpp binary
int main() { mcpp::link_lib("m"); mcpp::cxxflag("-DX"); }
```

Design constraints for that iteration (per project direction):
- **Bundled in the mcpp binary.** mcpp embeds the `mcpp` module source, writes +
  compiles it (cached BMI + object under `target/`, not rebuilt unless the
  toolchain changes), and makes it importable when compiling `build.mcpp`.
- **No `import std;` requirement.** The `mcpp` module implements its I/O with
  minimal C-level primitives (no `import std;` in its interface), so neither it nor
  `build.mcpp` forces the std-module staging cost on a tiny build script.
  (Empirically, a standalone `import std;` needs `gcm.cache/std.gcm` staged at the
  compile CWD + `std.o` linked — GCC ignores `-fmodule-file=std=` for C++ — so the
  module is found via the same `gcm.cache/` staging the ninja backend uses.)
- **Typed API mirrors the directive set** 1:1 (`cxxflag`/`cflag`/`link_lib`/
  `link_search`/`cfg`/`generated`/`rerun_if_changed`/`rerun_if_env_changed`).
- The string protocol stays as the documented low-level escape hatch.

This is the next iteration (post-0.0.78); the 0.0.78 core ships the wire-protocol
substrate so everything above layers on a stable foundation.

## mcpp-index dual perspective

A new workspace member `tests/examples/build-mcpp` whose `build.mcpp` emits a
define consumed by `main.cpp`, exercising the feature through the real pipeline.
