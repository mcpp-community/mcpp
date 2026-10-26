// mcpp.platform.common — the platform constants mcpp's code dispatches on.
//
// They are mcpplibs.os's compile-time constants (design 2026-10-26 part 2,
// R3), spelt as mcpp's code has always read them; nothing here is a macro, and
// every branch on them is an `if constexpr' that discards the other systems.

export module mcpp.platform.common;

import std;
import mcpplibs.os.platform;

namespace mcpp::platform {
namespace os = mcpplibs::os;
}

export namespace mcpp::platform {

// ── Binary / library name conventions ─────────────────────────────────────

constexpr bool is_windows = os::is_windows;
constexpr bool is_macos   = os::is_macos;
constexpr bool is_linux   = os::is_linux;

constexpr std::string_view exe_suffix     = os::exe_suffix;
constexpr std::string_view static_lib_ext = is_windows ? ".lib" : ".a";
constexpr std::string_view shared_lib_ext = is_windows ? ".dll" : is_macos ? ".dylib" : ".so";
constexpr std::string_view lib_prefix     = is_windows ? "" : "lib";

// ── Shell redirect to null device ─────────────────────────────────────────

constexpr std::string_view null_redirect = is_windows ? "2>nul" : "2>/dev/null";

// ── Platform identification ───────────────────────────────────────────────

constexpr std::string_view name = os::name(os::PLATFORM);

// The architecture of THIS build, in the GNU spelling `triple::parse' writes.
constexpr std::string_view host_arch = os::name(os::ARCH);

// The platform key xpkg descriptors use: `macosx' where the triple says `macos'.
constexpr std::string_view xpkg_platform = is_windows ? "windows" : is_macos ? "macosx" : "linux";

// ── Feature flags ─────────────────────────────────────────────────────────

constexpr bool supports_full_static = is_linux;     // macOS/Windows cannot
constexpr bool supports_rpath       = !is_windows;  // ELF + Mach-O only
constexpr bool needs_explicit_libcxx = is_macos;    // macOS: -lc++ required

} // namespace mcpp::platform
