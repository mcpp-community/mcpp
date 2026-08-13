// bench.toolchain — WHICH compiler every engine is handed, and where it lives.
//
// This module exists because the same decision was being made in two places.
// The fixture's generated `mcpp.toml` pinned `gcc@16.1.0`, and the CI workflow
// separately resolved `command -v g++` for cmake/xmake/bazel. Those two are not
// the same compiler, and nothing anywhere said so:
//
//   * mcpp built the fixture with the registry's gcc 16.1.0 and passed;
//   * cmake and xmake were handed the runner's gcc 13.3.0, which cannot build
//     C++23 modules at all — cmake failed to configure, xmake crashed gcc with
//     an internal compiler error, and both were recorded as `failed`.
//
// Forty-eight of the seventy-two cells in a "passing" matrix job failed that
// way. The suite's own fairness rule (see `resolve_cxx`) says every engine that
// can be told which compiler to use MUST be told the same one; this module is
// what makes that rule reachable, by naming ONE payload and handing it to
// everybody including mcpp.
//
// The versions are pinned rather than "whatever is newest" for the reason every
// other pin in this repository exists: a benchmark whose toolchain moves under
// it reports the toolchain's change as the engine's.
export module bench.toolchain;

import std;
import bench.platform;

export namespace bench::toolchain {

// The payload every arm of the benchmark compiles against.
//
// Windows is on llvm 20.1.7 rather than 22.1.8 because that is the version
// mcpp's registry actually ships for the PE target; pinning a version that is
// not there does not produce a slower number, it produces `unavailable`.
inline constexpr std::string_view kGcc          = "16.1.0";
inline constexpr std::string_view kLlvm         = "22.1.8";
inline constexpr std::string_view kLlvmWindows  = "20.1.7";

inline bool on_windows() { return platform::OS_NAME == "windows"; }

// Is this compiler request a clang one? The single spelling of that test, used
// by both the manifest emitter and the payload lookup.
inline bool is_clang_request(std::string_view compiler) {
    return compiler.find("clang") != std::string_view::npos
        || compiler.find("llvm")  != std::string_view::npos;
}

// What the fixture's `mcpp.toml` must say so that mcpp uses the same compiler
// every other engine was handed.
inline std::string mcpp_pin(std::string_view compiler) {
    if (is_clang_request(compiler))
        return std::format("llvm@{}", on_windows() ? kLlvmWindows : kLlvm);
    return std::format("gcc@{}", kGcc);
}

// Where mcpp keeps its packages. MCPP_HOME first, matching mcpp's own
// resolution order and the CMake helper in projects/common/.
inline std::filesystem::path registry_xpkgs() {
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    if (const char* home = std::getenv("MCPP_HOME"))
        return std::filesystem::path(home) / "registry" / "data" / "xpkgs";
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    const char* user = std::getenv(platform::OS_NAME == "windows" ? "USERPROFILE" : "HOME");
    if (!user) return {};
    return std::filesystem::path(user) / ".mcpp" / "registry" / "data" / "xpkgs";
}

// The C++ driver for `compiler` inside that payload, or nullopt with a reason.
//
// Returning the REASON rather than a bare nullopt matters: "the payload is not
// unpacked" and "this machine has no mcpp" lead to different fixes, and a
// benchmark that silently falls back to the host compiler when it cannot find
// the payload is the exact failure this module was written to end.
struct Resolved {
    std::filesystem::path driver;
    std::string           why;      // set when `driver` is empty
};

inline Resolved payload_cxx(std::string_view compiler) {
    const auto xpkgs = registry_xpkgs();
    if (xpkgs.empty())
        return {{}, "neither MCPP_HOME nor HOME/USERPROFILE is set"};

    const bool clang = is_clang_request(compiler);
    const std::string pkg = clang ? "xim-x-llvm" : "xim-x-gcc";
    const std::string ver{clang ? (on_windows() ? kLlvmWindows : kLlvm) : kGcc};
    const std::string exe = std::string(clang ? "clang++" : "g++")
                          + (on_windows() ? ".exe" : "");

    const auto driver = xpkgs / pkg / ver / "bin" / exe;
    std::error_code ec;
    if (std::filesystem::exists(driver, ec)) return {driver, {}};

    return {{}, std::format("{} is not unpacked (run `mcpp toolchain install {}@{}`)",
                            driver.string(), clang ? "llvm" : "gcc", ver)};
}

}  // namespace bench::toolchain
