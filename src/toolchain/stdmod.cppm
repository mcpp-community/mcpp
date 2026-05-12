module;
#include <cstdio>     // popen, pclose, fgets, FILE
#include <cstdlib>    // getenv

// mcpp.toolchain.stdmod — pre-build the `import std` BMI and cache it.
//
// GCC 15 flow (from docs/11-gcc15-cookbook.md §2):
//   g++ -std=c++23 -fmodules -Og -c <std.cc> -o std.o
//     ⇒ produces gcm.cache/std.gcm + std.o
//
// We invoke the compiler in a dedicated cache directory so the produced
// gcm.cache/std.gcm is owned by mcpp and reused across all builds with the
// same fingerprint.
//
// Output layout:
//   <cache_root>/<fingerprint>/
//      gcm.cache/std.gcm        ← BMI consumed via -fmodule-file=std=...
//                                 (or simply -fprebuilt-module-path=...)
//      std.o                    ← linked into final binaries

export module mcpp.toolchain.stdmod;

import std;
import mcpp.toolchain.detect;
import mcpp.xlings;

export namespace mcpp::toolchain {

struct StdModule {
    std::filesystem::path           cacheDir;            // <cache_root>/<fp>/
    std::filesystem::path           bmiPath;             // <cacheDir>/gcm.cache/std.gcm
    std::filesystem::path           objectPath;          // <cacheDir>/std.o
};

struct StdModError { std::string message; };

std::filesystem::path default_cache_root();

// Build std module if not already cached. Returns paths to BMI + object.
std::expected<StdModule, StdModError> ensure_built(
    const Toolchain&                  tc,
    std::string_view                  fingerprint_hex,
    const std::filesystem::path&      cache_root = default_cache_root());

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

std::filesystem::path default_cache_root() {
    if (auto* e = std::getenv("MCPP_HOME"); e && *e) {
        return std::filesystem::path(e) / "bmi";
    }
    if (auto* e = std::getenv("HOME"); e && *e) {
        return std::filesystem::path(e) / ".mcpp" / "bmi";
    }
    return std::filesystem::current_path() / ".mcpp-bmi";
}

std::expected<StdModule, StdModError> ensure_built(
    const Toolchain&                  tc,
    std::string_view                  fingerprint_hex,
    const std::filesystem::path&      cache_root)
{
    if (tc.stdModuleSource.empty()) {
        return std::unexpected(StdModError{
            "toolchain has no std module source (import std unsupported on this compiler)"});
    }

    StdModule sm;
    sm.cacheDir   = cache_root / std::string(fingerprint_hex);
    sm.bmiPath    = sm.cacheDir / "gcm.cache" / "std.gcm";
    sm.objectPath = sm.cacheDir / "std.o";

    if (std::filesystem::exists(sm.bmiPath) && std::filesystem::exists(sm.objectPath)) {
        return sm;
    }

    std::error_code ec;
    std::filesystem::create_directories(sm.cacheDir, ec);
    if (ec) return std::unexpected(StdModError{
        std::format("cannot create '{}': {}", sm.cacheDir.string(), ec.message())});

    // Run: cd <cacheDir> && g++ -std=c++23 -fmodules -Og [--sysroot=...] -c <std.cc> -o std.o
    std::string sysroot_flag;
    if (!tc.sysroot.empty()) {
        sysroot_flag = std::format(" --sysroot='{}'", tc.sysroot.string());
    }

    // When the compiler comes from mcpp's private sandbox, pass
    // -B<binutils-bin> so gcc finds `as`/`ld` directly via its internal
    // exec_prefix lookup — no PATH dependency, no xlings shim involvement.
    std::string b_flag;
    if (auto as = mcpp::xlings::paths::find_sibling_binary(
            tc.binaryPath, "binutils", "bin/as")) {
        b_flag = std::format(" -B'{}'", as->parent_path().string());
    }

    auto cmd = std::format(
        "cd '{}' && '{}' -std=c++23 -fmodules -O2{}{} -c '{}' -o std.o 2>&1",
        sm.cacheDir.string(),
        tc.binaryPath.string(),
        sysroot_flag,
        b_flag,
        tc.stdModuleSource.string());

    std::array<char, 4096> buf{};
    std::string out;
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) {
        return std::unexpected(StdModError{
            std::format("failed to spawn compiler: {}", cmd)});
    }
    while (std::fgets(buf.data(), buf.size(), fp) != nullptr) out += buf.data();
    int rc = ::pclose(fp);
    if (rc != 0) {
        return std::unexpected(StdModError{
            std::format("std module precompile failed (rc={}):\n{}", rc, out)});
    }

    if (!std::filesystem::exists(sm.bmiPath)) {
        return std::unexpected(StdModError{
            std::format("expected BMI at '{}' but it wasn't produced; output:\n{}",
                        sm.bmiPath.string(), out)});
    }

    return sm;
}

} // namespace mcpp::toolchain
