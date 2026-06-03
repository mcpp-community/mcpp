# mcpp core: runtime closure (rpath) + toolchain defaults

> 2026-06-03 · part of the mcpp ecosystem打通 plan
> Master plan: /home/speak/workspace/github/agentdocs/2026-06-03-mcpp-ecosystem-architecture-plan.md

This change fixes two general, long-term issues that block "native + GUI"
packages (e.g. the imgui module package) from working out of the box. Neither
is a special-case for any one package.

## R2 — dependency `[runtime] library_dirs` were dropped from binary RUNPATH

### Symptom
A fresh consumer that depends (transitively) on `compat.glfw` builds fine but
`mcpp run` fails at window creation: `GLX: Failed to load GLX`.

### Root cause (confirmed in source)
- `compat.glx-runtime` (pulled in by `compat.glfw` on Linux) symlinks the host
  GLVND/GL/GLX libraries into its install dir and declares
  `[runtime] library_dirs = { mcpp_generated/glx_runtime/lib }`.
- `src/build/plan.cppm` (~L220) already collects every dependency package's
  `runtime.library_dirs` into `plan.runtimeLibraryDirs` (resolved to absolute).
- BUT `src/build/flags.cppm` (~L258) built the produced binary's RUNPATH by
  iterating only `plan.toolchain.linkRuntimeDirs` — i.e. the toolchain's own
  runtime dirs. The dependency runtime dirs in `plan.runtimeLibraryDirs` were
  never emitted as `-Wl,-rpath`. So the host-GL passthrough dir was not on the
  binary's RUNPATH, and the dlopen()'d `libGL.so.1` / `libGLX.so.0` were
  unreachable at run time.

The dependency dirs were correctly used for the *build/process* environment but
not baked into the *binary* — so anything reached via dlopen (GL/GLX, and any
plugin-style runtime lib) failed.

### Fix
`src/build/flags.cppm`: iterate `plan.runtimeLibraryDirs` (the union of
dependency runtime dirs + toolchain + payload) instead of
`plan.toolchain.linkRuntimeDirs` when emitting `-L`/`-Wl,-rpath`. This is a
superset, so toolchain dirs are still covered; it additionally bakes each
dependency's declared runtime dir into RUNPATH.

This is the correct general behavior: any package that declares
`[runtime] library_dirs` is promising "binaries that use me need these dirs at
run time"; the producer binary must carry them as RUNPATH.

## R1 — fresh-machine bootstrap default toolchain was musl-static on Linux

### Symptom
On a clean machine, "First run no toolchain configured" auto-installs
`gcc@15.1.0-musl` (musl, static). Building any package that links the glibc
world (X11/GL/system libs) then fails, e.g. `libXdmcp` `arc4random_buf`
implicit-declaration under musl.

### Root cause
`src/cli.cppm` (~L1390) hard-coded the Linux first-run default to
`gcc@15.1.0-musl`.

### Fix
Default Linux first-run toolchain to the platform-native glibc gcc
(`gcc@16.1.0`). musl-static remains fully available but **opt-in** via
`mcpp build --target x86_64-linux-musl` (which the project already supports via
`[target.x86_64-linux-musl]`). This mirrors Cargo/Rust: default triple is
`-gnu` (glibc), `-musl` is an explicit target for portable static binaries.
musl-static is a poor *default* because it cannot link the glibc/native world.

## Why these are long-term/industrial, not workarounds
- R2 makes the existing two-plane design actually work: the *host plane*
  (drivers/GLVND, provided by `compat.glx-runtime`, never vendored) is bound to
  the binary via RUNPATH, which is the standard ELF mechanism. No package code
  changes; no env hacks.
- R1 aligns the default with the platform-native ABI, the same principle Cargo
  uses. Static/musl stays a first-class explicit option.

## Test plan (acceptance, via imgui-m, no special-casing)
1. Self-build mcpp with these changes.
2. Fresh consumer: `mcpp new app && mcpp add imgui` then:
   - `mcpp build` → uses glibc gcc by default (R1), no musl error.
   - `readelf -d <bin>` → RUNPATH contains the `compat.glx-runtime` lib dir (R2).
   - `mcpp run` → window opens, ImGui renders, no `GLX: Failed to load GLX`.
3. `mcpp test` headless still passes on all platforms.

## Follow-up (separate, tracked in master plan)
- Declarative `abi` capability on native packages so the resolver *derives* the
  ABI-correct toolchain instead of relying on a good default (defense in depth).
- Capability→provider resolution for `opengl.glx.driver` (glvnd/cocoa/win32).
