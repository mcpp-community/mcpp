# Remaining Platform Macros Outside src/platform/ — Analysis Report

## Summary

14 files, 39 occurrences of platform-specific macros/conditionals outside `src/platform/`.

## P0: Eliminate `#define popen _popen` (6 files, ~16 occurrences)

Migrate all raw popen/pclose calls to `platform::process::capture()` / `run_streaming()` / `run_silent()`.

Files: cli.cppm, ninja_backend.cppm, msvc.cppm, stdmod.cppm, pack/pack.cppm, pm/publisher.cppm, modgraph/p1689.cppm

## P1: Shell quoting in cli.cppm (5 occurrences)

Replace manual `'...'` vs `"..."` with `platform::shell::quote()`.

Lines: 1954, 1965, 2527, 2620, 3178

## P2: Terminal abstraction — ui.cppm (3 occurrences)

Create `platform/terminal.cppm` for isatty, terminal width detection.

Lines: 8-9, 149, 329

## P3: kXpkgPlatform in resolver.cppm (1 occurrence)

Move to `platform::common` as a constant.

Line: 30

## P4: Link strategy constants in flags.cppm (3 occurrences)

Add `supports_full_static`, `supports_rpath`, `needs_explicit_libcxx` to `platform::common`.

Lines: 162, 175, 183

## P5: xlings.cppm build_command_prefix (3 occurrences)

Abstract Windows env-setting vs POSIX shell-prefix into platform layer.

Lines: 401, 612, 739

## P6: ninja_backend.cppm rule generation (10 occurrences)

Abstract `$toolenv` prefix and shell syntax differences.

Lines: 178, 207, 231, 243, 254, 287, 297, 548, 560

## Priority: P0+P1 = 54% elimination with minimal risk.
