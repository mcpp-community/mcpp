// mcpp.platform — unified platform abstraction facade.
//
// Import this single module to get access to all platform capabilities:
//
//   import mcpp.platform;
//   // then use mcpp::platform::fs::self_exe_path(), etc.
//
// ─── LAYOUT ────────────────────────────────────────────────────────────────
//
//   src/*.cppm, src/<system>/*.cppm   module interfaces, compiled on every
//                              system: the same declarations everywhere.
//   src/linux/*.cpp            implementation units compiled for Linux,
//   src/macos/*.cpp            for macOS,
//   src/posix/*.cpp            for both (mcpp.toml selects them per target),
//   src/windows/*.cpp          and for Windows.
//
// The operating system itself is reached through mcpplibs.os (design
// 2026-10-26 part 2, §10.1): its compile-time constants
// (mcpp.platform.common), the executable's own name, PATH lookup, links,
// renames that refuse an existing name, volume identities, terminals and
// standard streams. What remains in implementation units is what that
// library does not carry: the launchers and their signal guard, file locks
// compatible with every earlier mcpp, durable sync, and the system-specific
// queries of the linux, macos and windows modules.
//
// ─── HOW PLATFORM DIFFERENCES ARE EXPRESSED ────────────────────────────────
//
// No preprocessor conditional appears in this package. A difference is one
// of:
//
//   1. `if constexpr (mcpp::platform::is_windows)` on mcpplibs.os's constants,
//      where both branches compile everywhere. Both branches then have to
//      COMPILE everywhere, which is what keeps the unused branch from rotting —
//      the Windows deadline was dead code for months precisely because nothing
//      on a Linux CI ever compiled it.
//
//   2. A function declared in an interface and defined in one implementation
//      unit per system, when the difference is a header or a system call. A
//      discarded `if constexpr` branch may name another system's function;
//      a call that is not discarded is a link error, never a silent stub.
//      `mcpp.platform.process`'s bounded-run dispatch is the reference
//      example: two launcher modules behind one signature, one dispatch.
//
//   3. A module whose non-matching system answers "not here" (`.macos`,
//      `.linux`, `.windows`): consumers call it unconditionally.
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
export import mcpp.platform.fs_caps;
export import mcpp.platform.env;
export import mcpp.platform.macos;
export import mcpp.platform.linux;
export import mcpp.platform.windows;
export import mcpp.platform.terminal;

// The bounded-run implementations are deliberately NOT re-exported: they are
// an implementation detail of `mcpp.platform.process`'s deadline variants, and
// nothing outside it should reach past the dispatch to a specific platform.
