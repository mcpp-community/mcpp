module;
#include <cstdio>     // popen, pclose, fgets, FILE
#include <cstdlib>    // getenv

// mcpp.toolchain.stdmod — pre-build the `import std` BMI and cache it.
//
// GCC 15 flow (from docs/11-gcc15-cookbook.md §2):
//   g++ -std=c++23 -fmodules -Og -c <std.cc> -o std.o
//     ⇒ produces gcm.cache/std.gcm + std.o
//
// Clang/libc++ flow:
//   clang++ -std=c++23 --precompile <std.cppm> -o pcm.cache/std.pcm
//   clang++ -std=c++23 pcm.cache/std.pcm -c -o std.o
//
// We invoke the compiler in a dedicated cache directory so the produced
// BMI is owned by mcpp and reused across all builds with the same fingerprint.
//
// Output layout:
//   <cache_root>/<fingerprint>/
//      gcm.cache/std.gcm        ← GCC BMI
//      pcm.cache/std.pcm        ← Clang BMI
//      std.o                    ← linked into final binaries

export module mcpp.toolchain.stdmod;

import std;
import mcpp.toolchain.clang;
import mcpp.toolchain.detect;
import mcpp.toolchain.gcc;

export namespace mcpp::toolchain {

struct StdModule {
    std::filesystem::path           cacheDir;            // <cache_root>/<fp>/
    std::filesystem::path           bmiPath;             // <cacheDir>/gcm.cache/std.gcm
    std::filesystem::path           objectPath;          // <cacheDir>/std.o
    std::filesystem::path           compatBmiPath;       // <cacheDir>/pcm.cache/std.compat.pcm
    std::filesystem::path           compatObjectPath;    // <cacheDir>/std.compat.o
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

namespace {

std::expected<std::string, StdModError> run_capture_command(const std::string& cmd) {
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
    return out;
}

} // namespace

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
    sm.bmiPath    = is_clang(tc)
                  ? mcpp::toolchain::clang::std_bmi_path(sm.cacheDir)
                  : mcpp::toolchain::gcc::std_bmi_path(sm.cacheDir);
    sm.objectPath = sm.cacheDir / "std.o";

    std::string sysroot_flag;
    if (!tc.sysroot.empty()) {
        sysroot_flag = std::format(" --sysroot='{}'", tc.sysroot.string());
    }

    bool std_cached = std::filesystem::exists(sm.bmiPath) && std::filesystem::exists(sm.objectPath);

    if (!std_cached) {
        std::error_code ec;
        std::filesystem::create_directories(sm.bmiPath.parent_path(), ec);
        if (ec) return std::unexpected(StdModError{
            std::format("cannot create '{}': {}", sm.bmiPath.parent_path().string(), ec.message())});

        std::string out;

        if (is_clang(tc)) {
            for (auto& cmd : mcpp::toolchain::clang::std_module_build_commands(
                     tc, sm.cacheDir, sm.bmiPath, sysroot_flag)) {
                if (auto r = run_capture_command(cmd); !r) return std::unexpected(r.error());
                else out += *r;
            }
        } else {
            auto cmd = mcpp::toolchain::gcc::std_module_build_command(
                tc, sm.cacheDir, sysroot_flag);
            if (auto r = run_capture_command(cmd); !r) return std::unexpected(r.error());
            else out += *r;
        }

        if (!std::filesystem::exists(sm.bmiPath)) {
            return std::unexpected(StdModError{
                std::format("expected BMI at '{}' but it wasn't produced; output:\n{}",
                            sm.bmiPath.string(), out)});
        }
    }

    // Build std.compat after std (std.compat depends on std, Clang only)
    if (is_clang(tc) && !tc.stdCompatSource.empty()) {
        auto compatBmi = mcpp::toolchain::clang::std_compat_bmi_path(sm.cacheDir);
        if (!std::filesystem::exists(compatBmi)) {
            std::string out;
            for (auto& cmd : mcpp::toolchain::clang::std_compat_build_commands(
                     tc, sm.cacheDir, compatBmi, sm.bmiPath, sysroot_flag)) {
                if (auto r = run_capture_command(cmd); !r) return std::unexpected(r.error());
                else out += *r;
            }
        }
        sm.compatBmiPath = compatBmi;
        sm.compatObjectPath = sm.cacheDir / "std.compat.o";
    }

    return sm;
}

} // namespace mcpp::toolchain
