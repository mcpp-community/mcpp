// mcpp.platform — unified platform abstraction facade.
//
// Import this single module to get access to all platform capabilities.
// Re-exports every sub-module so consumers can write:
//
//   import mcpp.platform;
//   // then use mcpp::platform::fs::self_exe_path(), etc.
//
// Platform-specific modules (macos, linux, windows) are always compiled
// on all platforms but their functions are no-ops on non-matching
// platforms, so consumers can call them without #ifdef guards.

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
