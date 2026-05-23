# Fix: LLVM shared libraries have stale RUNPATH after install

**Date**: 2026-05-22
**Status**: Proposed
**Severity**: Runtime crash on systems without system-installed gcc runtime

## Problem

When mcpp installs the LLVM toolchain (`mcpp toolchain install llvm 20.1.7`),
the shared libraries (`libc++.so.1`, `libc++abi.so.1`, `libunwind.so.1`) retain
RUNPATH entries from the xlings build environment:

```
libc++.so.1 RUNPATH:
  /home/<user>/.xlings/data/xpkgs/xim-x-llvm/20.1.7/lib
  /home/<user>/.xlings/data/xpkgs/xim-x-glibc/2.39/lib64
  /home/<user>/.xlings/data/xpkgs/xim-x-zlib/1.3.1/lib
  ...
```

These paths are invalid in the mcpp registry (`~/.mcpp/registry/data/xpkgs/...`).

`libc++.so.1` has a NEEDED dependency on `libatomic.so.1`. At runtime, the
dynamic linker searches `libc++.so.1`'s own RUNPATH (not the executable's) to
find `libatomic.so.1`. Since the RUNPATH points to non-existent xlings paths,
loading fails:

```
error while loading shared libraries: libatomic.so.1:
  cannot open shared object file: No such file or directory
```

**Why CI passes**: GitHub Actions ubuntu-24.04 has `libatomic.so.1` in
`/usr/lib/x86_64-linux-gnu/` (from pre-installed gcc). The loader's fallback to
system paths finds it. Clean systems without gcc runtime fail.

## Root Cause

### GCC toolchain: fully fixed

The GCC post-install path (`src/cli.cppm:3764-3803`) runs `patchelf_walk()` on
the entire payload, which rewrites both PT_INTERP and RUNPATH for every ELF
file (binaries and shared libraries):

```cpp
// src/cli.cppm:3764
if (pkg.needsGccPostInstallFixup) {
    // ...
    patchelf_walk(payload->root, loader, rpath, patchelfBin);  // line 3794
    fixup_gcc_specs(payload->root, glibcLibDir, gccLibDir);    // line 3797
}
```

### LLVM toolchain: only cfg fixed, shared libs missed

The LLVM post-install path (`src/cli.cppm:3808-3826`) only calls
`fixup_clang_cfg()`, which rewrites text paths in `clang++.cfg`/`clang.cfg`.
It does **not** call `patchelf_walk()` on the LLVM shared libraries:

```cpp
// src/cli.cppm:3808
if (pkg.ximName == "llvm") {
    // ...
    fixup_clang_cfg(payload->root, glibcLibDir);  // line 3825
    // ← Missing: patchelf_walk() for .so files
}
```

### ELF RUNPATH is non-transitive

This matters because RUNPATH does not propagate to transitive dependencies:

```
hh (RUNPATH → ~/.mcpp/registry/...)
 └→ libc++.so.1         ✅ found via hh's RUNPATH
     └→ libatomic.so.1  ❌ searched via libc++.so.1's RUNPATH (stale xlings paths)
```

The executable's RUNPATH includes the correct mcpp registry paths, but the
loader uses `libc++.so.1`'s own RUNPATH when resolving `libc++.so.1`'s
dependencies. Since `libc++.so.1` was never patchelf'd, its RUNPATH still
points to the old xlings build paths.

## Dependency Chain

```
libatomic.so.1 ← NEEDED by libc++.so.1
libc++.so.1    ← NEEDED by user binary (via -stdlib=libc++)
```

`libatomic.so.1` exists in the LLVM xpkg at:
```
~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/lib/x86_64-unknown-linux-gnu/libatomic.so.1
```

But `libc++.so.1` doesn't know to look there because its RUNPATH was never
updated.

## Fix

### Location

`src/cli.cppm`, lines 3808-3826 (LLVM post-install block)

### Change

Add `patchelf_walk()` for the LLVM payload, mirroring what GCC already does.
The RUNPATH should include:
1. LLVM's `lib/x86_64-unknown-linux-gnu` (where `libatomic.so.1`, `libc++.so.1` etc. live)
2. LLVM's `lib` (generic lib dir)
3. glibc's `lib64` (where `libc.so.6`, `libm.so.6`, `ld-linux-x86-64.so.2` live)

### Proposed code

```cpp
// src/cli.cppm — inside the `if (pkg.ximName == "llvm")` block, BEFORE fixup_clang_cfg:

if (pkg.ximName == "llvm") {
    auto glibcRoot = mcpp::xlings::paths::xim_tool_root(xlEnv, "glibc");
    std::filesystem::path glibcLibDir;
    if (std::filesystem::exists(glibcRoot)) {
        for (auto& v : std::filesystem::directory_iterator(glibcRoot)) {
            auto candidate = v.path() / "lib64";
            if (std::filesystem::exists(candidate / "ld-linux-x86-64.so.2")) {
                glibcLibDir = candidate;
                break;
            }
            candidate = v.path() / "lib";
            if (std::filesystem::exists(candidate / "ld-linux-x86-64.so.2")) {
                glibcLibDir = candidate;
                break;
            }
        }
    }

    // NEW: patchelf walk — rewrite PT_INTERP + RUNPATH for LLVM binaries
    //      and shared libraries so they're self-contained in the sandbox.
    auto patchelfBin = mcpp::xlings::paths::xim_tool(xlEnv, "patchelf",
        mcpp::xlings::pinned::kPatchelfVersion) / "bin" / "patchelf";
    auto llvmTargetLib = payload->root / "lib" / "x86_64-unknown-linux-gnu";
    auto llvmGenericLib = payload->root / "lib";
    if (!glibcLibDir.empty() && std::filesystem::exists(patchelfBin)) {
        auto loader = glibcLibDir / "ld-linux-x86-64.so.2";

        // RUNPATH: target-specific lib (libatomic, libc++, libunwind)
        //        + generic lib + glibc lib
        std::string rpath = llvmTargetLib.string()
            + ":" + llvmGenericLib.string()
            + ":" + glibcLibDir.string();

        patchelf_walk(payload->root, loader, rpath, patchelfBin);
    }

    fixup_clang_cfg(payload->root, glibcLibDir);
}
```

### What `patchelf_walk` does (already exists at `src/cli.cppm:643-686`)

1. Recursively walks all files under `payload->root`
2. Checks ELF magic bytes (skips non-ELF)
3. For files with PT_INTERP (executables): sets interpreter to sandbox glibc loader
4. For all ELF files (including .so): sets RUNPATH to the provided rpath string

### Affected shared libraries

These .so files in the LLVM xpkg will get corrected RUNPATH:

| Library | Has stale RUNPATH | Has libatomic.so.1 NEEDED |
|---------|-------------------|--------------------------|
| libc++.so.1 | Yes | Yes (root cause) |
| libc++abi.so.1 | Yes | No |
| libunwind.so.1 | Yes | No |
| libclang*.so | Yes | Possibly |
| libLLVM*.so | Yes | Possibly |

### Cross-platform notes

- **Linux only**: patchelf and RUNPATH are Linux-specific. macOS uses
  `@rpath`/`install_name_tool` (different mechanism, handled separately).
  The `patchelf_walk` function already has platform guards.
- **Windows**: Not applicable (PE format, no RUNPATH concept).

## Verification

After applying the fix:

```bash
# Reinstall LLVM toolchain
mcpp toolchain install llvm 20.1.7

# Verify libc++.so.1 RUNPATH now points to mcpp registry
readelf -d ~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/lib/x86_64-unknown-linux-gnu/libc++.so.1 | grep RUNPATH

# Expected: ~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/lib/x86_64-unknown-linux-gnu:...

# Verify a simple program runs without system libatomic
mcpp toolchain default llvm@20.1.7
cd $(mktemp -d) && mcpp new test_atomic && cd test_atomic && mcpp run
```

## Risk Assessment

- **Low risk**: `patchelf_walk()` is already battle-tested on GCC toolchains
- **Idempotent**: Running it multiple times produces the same result
- **No behavior change for GCC**: Only affects `pkg.ximName == "llvm"` path
