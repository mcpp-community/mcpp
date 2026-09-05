// mcpp.platform — unified platform abstraction facade.
//
// Import this single module to get access to all platform capabilities:
//
//   import mcpp.platform;
//   // then use mcpp::platform::fs::self_exe_path(), etc.
//
// ─── LAYOUT ────────────────────────────────────────────────────────────────
//
//   src/platform/*.cppm        the FACADE and the portable modules. A module
//                              here answers a question the same way everywhere,
//                              or dispatches once (see "dispatch" below).
//   src/platform/unix/         POSIX-only implementations.
//   src/platform/windows/      Win32-only implementations.
//   src/platform/linux/        Linux-only.
//   src/platform/macos/        macOS-only.
//
// A directory does not change a module's NAME — `src/platform/linux/linux.cppm`
// still declares `mcpp.platform.linux`, so moving a file here costs no importer
// a single line. The directory is what tells a reader, before opening anything,
// whether a file can contain platform-specific code at all.
//
// ─── HOW PLATFORM DIFFERENCES ARE EXPRESSED ────────────────────────────────
//
// Preferred, in order:
//
//   1. `if constexpr (mcpp::platform::is_windows)` in the facade layer, calling
//      into one implementation module per platform. Both branches then have to
//      COMPILE everywhere, which is what keeps the unused branch from rotting —
//      the Windows deadline was dead code for months precisely because nothing
//      on a Linux CI ever compiled it.
//
//      `mcpp.platform.process`'s bounded-run dispatch is the reference example:
//      two implementation modules behind one signature, one dispatch, and the
//      only `#if` left is inside each implementation, guarding its own headers.
//
//   2. A per-platform module whose non-matching build is a no-op stub
//      (`mcpp.platform.macos`, `.linux`, `.windows`). Consumers call them
//      unconditionally, with no `#ifdef` at the call site.
//
//   3. `#if` inside ONE module, when the difference is a header or a syscall
//      rather than a behaviour. Still the common case for the older portable
//      modules (`fs`, `env`, `scaffold_fs`); migrating those is incremental
//      work, not a precondition for adding new code the right way.
//
// A module under a platform directory MUST NOT name a `std` type in its
// EXPORTED interface if a widely-imported module will import it: under GCC
// 16.1 that corrupts every BMI downstream, and the error points at an
// unrelated module. Both bounded_process modules document the measurements.
// Builtin types plus a callback is the shape that works.

export module mcpp.platform;

export import mcpp.platform.common;
export import mcpp.platform.shell;
export import mcpp.platform.process;
export import mcpp.platform.fs;
export import mcpp.platform.env;
export import mcpp.platform.macos;
export import mcpp.platform.linux;
export import mcpp.platform.windows;
export import mcpp.platform.terminal;

// The bounded-run implementations are deliberately NOT re-exported: they are
// an implementation detail of `mcpp.platform.process`'s deadline variants, and
// nothing outside it should reach past the dispatch to a specific platform.
