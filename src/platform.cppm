// mcpp.platform — centralized platform-specific constants.
//
// Consumers import this module instead of scattering #if/_WIN32 / __APPLE__
// blocks throughout the codebase.  All compile-time branching lives here.

module;

// Nothing to #include for compile-time constants; the module fragment is kept
// for future OS headers if needed.

export module mcpp.platform;

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

} // namespace mcpp::platform
