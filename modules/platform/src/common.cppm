// mcpp.platform.common — centralized platform-specific constants.
//
// Consumers import this module (via mcpp.platform) instead of scattering
// #if/_WIN32 / __APPLE__ blocks throughout the codebase.  All compile-time
// branching for platform constants lives here.

module;

export module mcpp.platform.common;

import std;

export namespace mcpp::platform {

// ── Binary / library name conventions ─────────────────────────────────────

#if defined(_WIN32)
constexpr std::string_view exe_suffix    = ".exe";
constexpr std::string_view static_lib_ext = ".lib";
constexpr std::string_view shared_lib_ext = ".dll";
constexpr std::string_view lib_prefix    = "";
#elif defined(__APPLE__)
constexpr std::string_view exe_suffix    = "";
constexpr std::string_view static_lib_ext = ".a";
constexpr std::string_view shared_lib_ext = ".dylib";
constexpr std::string_view lib_prefix    = "lib";
#else
// Linux and other POSIX
constexpr std::string_view exe_suffix    = "";
constexpr std::string_view static_lib_ext = ".a";
constexpr std::string_view shared_lib_ext = ".so";
constexpr std::string_view lib_prefix    = "lib";
#endif

// ── Shell / process helpers ────────────────────────────────────────────────

#if defined(_WIN32)
constexpr std::string_view null_redirect = "2>nul";
#else
constexpr std::string_view null_redirect = "2>/dev/null";
#endif

// ── Platform identification ────────────────────────────────────────────────

#if defined(_WIN32)
constexpr bool is_windows = true;
constexpr bool is_macos   = false;
constexpr bool is_linux   = false;
#elif defined(__APPLE__)
constexpr bool is_windows = false;
constexpr bool is_macos   = true;
constexpr bool is_linux   = false;
#elif defined(__linux__)
constexpr bool is_windows = false;
constexpr bool is_macos   = false;
constexpr bool is_linux   = true;
#else
constexpr bool is_windows = false;
constexpr bool is_macos   = false;
constexpr bool is_linux   = false;
#endif

// ── Platform name string ───────────────────────────────────────────────────

constexpr std::string_view name =
#if defined(_WIN32)
    "windows";
#elif defined(__APPLE__)
    "macos";
#elif defined(__linux__)
    "linux";
#else
    "unknown";
#endif

// Host CPU architecture. Uses the GNU triple spelling (`aarch64`, not the
// `arm64` token xlings uses for its release assets) so it composes directly
// with target triples like `aarch64-linux-musl`. Used to decide native vs.
// cross toolchain selection (target arch == host_arch → native musl-gcc;
// otherwise a `<triple>-gcc` cross toolchain).
constexpr std::string_view host_arch =
#if defined(__aarch64__) || defined(_M_ARM64)
    "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    "x86";
#elif defined(__riscv) && (__riscv_xlen == 64)
    "riscv64";
#else
    "unknown";
#endif

// xpkg platform key (used by resolver for xpkg.lua lookups).
// Note: macOS uses "macosx" (not "macos") for xpkg compatibility.
constexpr std::string_view xpkg_platform =
#if defined(_WIN32)
    "windows";
#elif defined(__APPLE__)
    "macosx";
#elif defined(__linux__)
    "linux";
#else
    "linux";
#endif

// ── Link strategy capabilities ─────────────────────────────────────────
// Used by build/flags.cppm to avoid #ifdef blocks in linker flag logic.

constexpr bool supports_full_static = is_linux;  // macOS/Windows cannot
constexpr bool supports_rpath       = !is_windows;  // ELF + Mach-O only
constexpr bool needs_explicit_libcxx = is_macos;  // macOS: -lc++ required

} // namespace mcpp::platform
