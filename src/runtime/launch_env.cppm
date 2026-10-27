// mcpp.runtime.launch_env — the loader's search path a program mcpp starts is
// given: which variable a system reads, and the list composed for it.
export module mcpp.runtime.launch_env;

import std;

export namespace mcpp::runtime::launch_env {

// The separator of a list of directories on this host.
std::string path_list_separator();

// The PATH this invocation was started with, before anything mcpp stated: a
// value that must not depend on what was stated since -- an environment a
// build program's run is keyed on -- starts from this one.
const std::string& inherited_path();

// The variable the system's loader searches for a program's libraries: PATH on
// Windows, LD_LIBRARY_PATH on Linux, none on macOS (DYLD_LIBRARY_PATH reaches
// every executable ninja starts, ninja included, and can make the system's
// frameworks load an incompatible private libc++; macOS keeps toolchain rpaths).
std::string runtime_library_path_key();
// The same for a host tool, which on macOS is DYLD_LIBRARY_PATH.
std::string host_tool_runtime_library_path_key();

// Drop mcpp's private-glibc payload entries from a loader search path.
//
// An outer `mcpp run`/`mcpp test` points LD_LIBRARY_PATH at the payload so ITS
// child (a sandbox-linked binary) can load. Anything further down the process
// tree that is a HOST binary — /bin/sh, or a payload tool patched against a
// DIFFERENT payload version — then resolves a mismatched libc.so.6 against the
// host ld.so and dies inside the dynamic linker before main (signature: a bare
// `__vdso_time` line, then SIGSEGV). libc and ld.so are version-locked through
// GLIBC_PRIVATE, so this never reproduces when the two happen to match.
//
// Both mcpp.run (which sanitizes the inherited variable for every child) and
// prepend_path_list (which sanitizes the inherited tail it carries into a
// composed override) use it; directories a caller passes are always kept.
std::string strip_private_glibc(std::string_view paths);

// `dirs` before the variable's current value, in the host's spelling: the
// value a started program receives for `key`. "" when there is nothing to put.
std::string prepend_path_list(std::string_view key,
                              std::span<const std::filesystem::path> dirs);

}  // namespace mcpp::runtime::launch_env
