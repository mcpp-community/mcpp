# Platform Abstraction Layer — Architecture & Implementation Plan

## 1. Problem Statement

mcpp has ~45+ `#if defined(...)` blocks scattered across 10+ source files for platform-specific logic. This causes:
- **stdin leakage**: subprocess calls on macOS don't close stdin → first-run hangs
- **Code duplication**: `self_exe_path()` duplicated in `config.cppm` and `ninja_backend.cppm`
- **Inconsistent abstractions**: `process.cppm` wraps some calls, but many files still use raw `std::system()`/`popen()`
- **Maintenance burden**: adding platform support requires editing 10+ files

## 2. Target Architecture

```
src/platform/
├── platform.cppm          # Unified facade (re-exports all platform capabilities)
├── common.cppm            # Cross-platform constants + compile-time detection
├── process.cppm           # Unified process execution (auto-closes stdin on POSIX)
├── fs.cppm                # Platform filesystem ops (exe path, file lock, which)
├── env.cppm               # Environment variable operations
├── shell.cppm             # Shell quoting + command building
├── macos.cppm             # macOS: xcrun, SDK discovery, Xcode CLT detection
├── linux.cppm             # Linux: /proc, LD_LIBRARY_PATH, patchelf
└── windows.cppm           # Windows: vswhere, MSVC, _putenv_s, registry
```

## 3. Module Responsibilities

### common.cppm — Compile-time constants & platform detection
- Platform booleans: `is_windows`, `is_macos`, `is_linux`
- Binary naming: `exe_suffix`, `static_lib_ext`, `shared_lib_ext`, `lib_prefix`
- Platform name string
- Null redirect string

### process.cppm — Unified process execution (CORE FIX)
- `capture()` — run command, capture stdout (auto `</dev/null` on POSIX)
- `run_silent()` — run without caring about output
- `run_streaming()` — stream stdout line-by-line via callback
- All subprocess calls go through here → stdin leak impossible

### fs.cppm — Platform filesystem operations
- `self_exe_path()` — current executable path (Win: GetModuleFileName, macOS: _NSGetExecutablePath, Linux: /proc/self/exe)
- `FileLock` — RAII exclusive file lock (Win: LockFileEx, POSIX: flock)
- `which()` — find executable (Win: `where`, POSIX: `command -v`)

### env.cppm — Environment variable operations
- `set()` / `get()` — platform-aware env var manipulation
- `build_env_prefix()` — construct env prefix for commands

### shell.cppm — Shell quoting
- `quote()` — platform-aware shell argument quoting
- `silent_redirect` — full silent redirect string

### macos.cppm — macOS-specific
- `has_xcode_clt()` — check Xcode Command Line Tools
- `sdk_path()` — xcrun SDK discovery
- `runtime_lib_dirs()` — macOS library search paths

### linux.cppm — Linux-specific
- `build_ld_library_path()` — LD_LIBRARY_PATH construction
- `runtime_lib_dirs()` — Linux library search paths

### windows.cppm — Windows-specific
- `find_visual_studio()` — VS discovery via vswhere/env/paths
- `find_std_module_source()` — MSVC STL module source
- `prepend_path()` — Windows PATH manipulation

### platform.cppm — Unified facade
- Re-exports all common modules
- Conditionally exports platform-specific modules

## 4. Migration Impact

| Source File | Changes | Effort |
|-------------|---------|--------|
| `config.cppm` | Use `fs::self_exe_path()`, `fs::which()`, `process::run_silent()` | Medium |
| `xlings.cppm` | Use `shell::quote()`, `process::run_streaming()`, `env::*` | Large |
| `process.cppm` | DELETE — absorbed by `platform/process.cppm` + `platform/shell.cppm` | Delete |
| `probe.cppm` | Use `fs::which()`, `macos::sdk_path()` | Medium |
| `bmi_cache.cppm` | Use `fs::FileLock` | Small |
| `ninja_backend.cppm` | Use `fs::self_exe_path()` | Small |
| `flags.cppm` | Use `common` constants | Small |
| `clang.cppm` | Use `windows::find_std_module_source()` | Small |
| `msvc.cppm` | Discovery logic moves to `platform/windows.cppm` | Medium |
| `cli.cppm` | Use `common::name` | Small |

## 5. Implementation Phases

### Phase 1: Skeleton (parallel-safe)
Create directory + `common.cppm` (move from existing `platform.cppm`)

### Phase 2: Core modules (parallelizable)
- 2a: `process.cppm` — includes stdin fix
- 2b: `fs.cppm` — self_exe_path + FileLock + which
- 2c: `env.cppm` + `shell.cppm`
- 2d: `macos.cppm` + `linux.cppm` + `windows.cppm`
- 2e: `platform.cppm` facade

### Phase 3: Consumer migration
Migrate all files to use new platform modules, remove old `process.cppm`

### Phase 4: Build config + CI verification
Update `mcpp.toml`, verify on all platforms
