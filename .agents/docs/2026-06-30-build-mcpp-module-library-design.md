# The `mcpp` build-module library for `build.mcpp` (Architecture & Design)

How mcpp provides a **typed module API** to `build.mcpp` so it can be written
modules-first (`import mcpp;`, no `#include`, no `import std;`) instead of printing
raw `mcpp:` protocol strings. Evaluated on five axes: **简洁 (simplicity) / 覆盖
(coverage) / 优化 (optimization) / 稳定 (stability) / 适配 (adaptability)**.

## The constraint that drives the whole design

The helper's job is to emit the *exact* `mcpp:` wire protocol **this** mcpp parses.
So it is **not a third-party library — it is part of the engine's ABI.** Any design
that lets the helper drift from the engine's protocol version (a separately
released package, a pinned dependency) introduces skew. This single fact rules out
most of the "obvious" options and points straight at "ship it with the engine."

## Options considered

| Option | What | Verdict |
|---|---|---|
| **A. Ship a prebuilt BMI** (`mcpp.gcm`/`.pcm` in the release) | precompiled module interface | ✗ BMIs are **not portable** across compiler vendor/version/flags (GCC gcm is locked to the exact GCC build). Would need a combinatorial matrix of BMIs. Fragile. |
| **B. Header-only** (`#include <mcpp_build.h>`) | a shipped header | ✗ contradicts modules-first ("no headers"). (Most portable, but off-brand; kept as a mental fallback only.) |
| **C. Cargo model** — helper is a normal `[build-dependencies]` package in the index | `build.mcpp` depends on a published `mcpp` package, resolved + compiled like any dep | △ composable, but adds a **resolution step** for a leaf script and reintroduces **version skew** (the package version vs the engine's protocol). Cargo's `build-rs` crate works this way — but Cargo's protocol is far more stable than a young tool's. |
| **D. Zig model** — helper is part of the tool, always present, version-matched | embed the module **source** in the binary; compile on demand against the host toolchain | ✓ **chosen.** Zig's `std.Build` ships with the compiler; the build API and the engine are one artifact, so they can never disagree. |

### Why "embed the **source**, not the BMI"

Source is the only **toolchain-portable** form. A BMI is compiler-version-locked;
source compiles against *whatever* host toolchain resolved for this build (gcc on
Linux, clang on macOS/Windows), at whatever version, with the same sysroot flags
the build already computes. So one embedded `constexpr std::string_view` adapts to
every toolchain — no matrix, no skew. This is the crux of **适配 + 稳定**.

## Chosen design

```
mcpp binary
└── constexpr std::string_view kMcppModuleSource   // the `mcpp` module, embedded
        │  (module; #include <cstdio>  export module mcpp; … inline emitters)
        ▼  only when build.mcpp contains `import mcpp`
   <proj>/target/.build-mcpp/
        ├── mcpp.cppm           written from the embedded source
        ├── mcpp.gcm / .pcm     compiled BMI (GCC gcm.cache/ | Clang pcm)
        ├── mcpp.o              module object (linked into build.mcpp.bin)
        └── build.mcpp.bin
```

1. **Embedded, version-matched** (`build_program.cppm` `kMcppModuleSource`). The
   functions mirror the directive set 1:1 and `std::printf` the `mcpp:` lines. I/O
   is C-level (global module fragment `#include <cstdio>`), so **the module needs
   no `import std;`** — neither does a `build.mcpp` that only `import mcpp;`.
2. **Compiled on demand, into `target/`** — not in the project tree. GCC:
   `-fmodules` → `gcm.cache/mcpp.gcm` + `mcpp.o`; Clang: `--precompile` → `mcpp.pcm`
   then `-c` → `mcpp.o`. Reuses the build's own `host_base_flags` (sysroot etc.).
3. **Gated on actual use** — mcpp scans `build.mcpp` for `import mcpp`; only then
   is the module built + linked and the compile run from `target/.build-mcpp/`
   (so GCC finds `gcm.cache/` relative to cwd, via the 0.0.79 `capture_exec` cwd).
   A `#include`-based `build.mcpp` compiles **byte-identically to before** — zero
   blast radius.

## Five-axis evaluation

- **简洁** — one embedded string + one compile helper; no packaging, no install, no
  registry entry, no version field. The user writes `import mcpp;` and it's there.
- **覆盖** — GCC (gcm) on Linux + Clang (pcm) on macOS/Windows = mcpp's whole
  toolchain matrix (mcpp uses clang, not MSVC, on Windows). The directive API
  covers every wire directive 1:1.
- **优化** — built only when `build.mcpp` *uses* it AND is being (re)compiled
  (already gated by the declared-input cache), so a stable build.mcpp pays nothing.
  Cost when it does run: one ~0.3 s module compile. *Future*: a **global
  per-toolchain BMI cache** (`~/.mcpp/bmi/build-module/<toolchain-hash>/`,
  symlinked into each project's `gcm.cache/`) would compile once per machine
  instead of once per project — deferred; the per-project compile is cheap and
  keeps the code simple.
- **稳定** — embedded source ⇒ **no version skew** (the headline win); use-gating
  ⇒ existing `#include` programs are untouched; failures surface as a clear "mcpp
  module compile failed" with the compiler output.
- **适配** — source-on-demand adapts to any host toolchain/version automatically;
  adding a directive = adding one `inline` function to the embedded string;
  per-compiler module ABI handled by the GCC/Clang branch.

## Naming

`import mcpp;` (top-level) for brevity — `build.mcpp` context makes the scope
unambiguous. Future non-build helpers can live under `mcpp.<sub>` modules without
colliding. (`import mcpp.build;` was considered for namespace precision; rejected
for the common case's verbosity — revisit only if a second `mcpp` module appears.)

## API (mirrors the wire protocol 1:1)

```cpp
import mcpp;
int main() {
    mcpp::cxxflag("-DHAVE_X=1");
    mcpp::cflag("-DFOR_C");
    mcpp::link_lib("m");                 // -lm
    mcpp::link_search("vendor/lib");     // -L…
    mcpp::define("HAVE_FEATURE");         // cfg= → -DHAVE_FEATURE
    mcpp::generated("src/gen.cpp");
    mcpp::rerun_if_changed("config.h");
    mcpp::rerun_if_env_changed("USE_FAST");
}
```

The raw stdout protocol stays the documented low-level substrate; `import mcpp;` is
the typed layer over it (the Cargo `build-rs`-over-`cargo::` shape, but
engine-bundled à la Zig).

## Implementation gotcha (recorded)

The embedded source contains the line `export module mcpp;`. mcpp's **default
line-based regex module scanner** (used on the Windows self-host build; the P1689
compiler-driven scanner ignores string literals) read that line *inside the raw
string literal* as `build_program.cppm` declaring a second module → "file already
exports module … cannot export 'mcpp'". Fix: write the declaration with a
`@MODULE@` placeholder substituted to `export module` at file-write time, so no
literal `export module <name>` text appears in mcpp's own source. (A broader fix
would be to teach the regex scanner to skip string/raw-string literals.)

## Coverage / stability boundaries (recorded)

- **Windows/macOS Clang path** is exercised by the mcpp-index `build-mcpp`
  workspace member (its `mcpp test --workspace` runs on macOS/Windows with clang);
  the e2e `92_build_mcpp_import.sh` covers the GCC path (it `requires: gcc`).
- Cross `--target` builds still skip `build.mcpp` entirely (host-only), so the
  module is host-only too.
