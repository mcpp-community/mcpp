# Windows Pack Design

**Date:** 2026-05-19
**Status:** Planned (stub guard in place, implementation not yet started)

## Current state

`mcpp pack` is fully functional on Linux and macOS. On Windows it exits early
with a clear error message directing users to the CI workflow:

```
error: `mcpp pack` is not yet supported on Windows.
       Use the CI workflow (ci-windows.yml) to produce Windows zip packages.
       Windows PE packaging (DLL collection + zip) is planned.
```

The guard lives at the top of `mcpp::pack::run()` in `src/pack/pack.cppm`.

## Why the current implementation cannot run on Windows

The POSIX implementation relies on three Linux/macOS-only mechanisms:

| Mechanism | POSIX usage | Windows equivalent |
|---|---|---|
| `LD_TRACE_LOADED_OBJECTS=1` | Tells the ELF dynamic linker to print deps without executing `main()` | No direct equivalent. Would need `dumpbin /dependents` (MSVC) or `ldd` emulation via `LoadLibraryEx` |
| `patchelf` | Rewrites `RUNPATH` / `PT_INTERP` ELF headers in-place | Not applicable to PE/COFF. DLL search order is controlled by the OS loader and manifest, not embedded paths |
| `tar -czf` | GNU tar — not universally present on Windows before Win11 22H2 | `Compress-Archive` (PowerShell), `7z`, or Win32 `CreateFile`/`MiniZip` |

## Planned Windows pack implementation

### Goal

Produce a self-contained `.zip` archive (not `.tar.gz`) that users can
extract and run with no additional setup:

```
<name>-<version>-x86_64-pc-windows-msvc.zip
└── <name>-<version>-x86_64-pc-windows-msvc/
    ├── <name>.exe
    ├── *.dll         (bundled DLLs, if any)
    └── README.md / LICENSE (if present)
```

### DLL discovery

Replace `ldd_parse()` with a Win32 equivalent:

1. **Primary: `dumpbin /dependents <binary>`** — available when MSVC tools are
   on `PATH`. Produces a list of DLL names; resolve each against `PATH` /
   `%SystemRoot%\System32` / side-by-side assemblies.

2. **Fallback: `PE header walk`** — open the PE file, walk the Import Directory,
   extract DLL names.  Can be implemented with `<windows.h>` + `ImageNtHeader`.

3. **Skip-list**: mirror the manylinux skip-list concept for Windows:
   `kernel32.dll`, `user32.dll`, `ntdll.dll`, `vcruntime*.dll` (Redist),
   `api-ms-win-*.dll` (API sets), `ucrtbase.dll`.

### Archive creation

Use `std::filesystem` to copy files into a staging directory, then produce
the zip with one of:

- **PowerShell** `Compress-Archive` — available on all modern Windows.
  Invoke via `run_capture("powershell -Command \"Compress-Archive ..."`)`.
  Slow for large trees; fine for typical release packages.
- **libzip / minizip** — statically linkable; avoid the PowerShell dependency.
  Preferred long-term.

### Format

- Output file: `.zip` (not `.tar.gz`) on Windows.
- `pack::Format` enum needs a new `Zip` variant (or auto-select by platform).
- `make_plan()` should derive the output extension from the target platform.

### Entry point

No shell wrapper needed on Windows — users double-click `<name>.exe` or run
it from `cmd.exe` / PowerShell directly.  If DLLs are bundled, they should be
placed in the **same directory** as the executable (the Win32 loader checks
`%EXE_DIR%` first, before `%PATH%`).

### Implementation checklist (for the future PR)

- [ ] Add `Format::Zip` (or `Format::ZipAuto`) to `pack::Format`
- [ ] Implement `dumpbin_parse()` (or PE header walk fallback) in `pack.cppm`
      under `#if defined(_WIN32)`
- [ ] Implement `make_zip()` (PowerShell or libzip) in `pack.cppm`
- [ ] Remove the `#if defined(_WIN32)` early-return guard from `pack::run()`
      once the above are ready
- [ ] Add a Windows-specific integration test to `ci-windows.yml`

### CI workflow (current workaround)

Until this is implemented, `ci-windows.yml` zips the raw build output with
PowerShell `Compress-Archive`. This is good enough for CI artifacts but does
not collect/bundle DLLs or apply the staging-directory layout.
