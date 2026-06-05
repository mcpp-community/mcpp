// mcpp.platform.macos — macOS-specific platform capabilities.
//
// Provides:
//   has_xcode_clt()       — detect Xcode Command Line Tools
//   sdk_path()            — discover macOS SDK via xcrun
//   runtime_lib_dirs()    — macOS-specific library search paths
//   supports_full_static  — macOS cannot fully static-link (libSystem)

module;
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#define popen  _popen
#define pclose _pclose
#endif

export module mcpp.platform.macos;

import std;

export namespace mcpp::platform::macos {

// Whether macOS supports full static linking (it does not — libSystem
// must be dynamically linked).
constexpr bool supports_full_static = false;

// Check whether Xcode Command Line Tools are installed.
// Returns true if `xcode-select -p` succeeds.
bool has_xcode_clt();

// Discover the macOS SDK path via `xcrun --show-sdk-path`.
// Returns the SDK path if found, or nullopt.
std::optional<std::filesystem::path> sdk_path();

// Resolve the effective macOS deployment target: the
// MACOSX_DEPLOYMENT_TARGET env var (explicit per-invocation override,
// the convention cargo/rustc/cc honor) wins over `manifestValue` (the
// [build] macos_deployment_target project default); empty means
// toolchain/SDK default. THE single source of truth — flags.cppm, the
// BMI fingerprint rule and the std-module prebuild must all consume
// this same resolution, or cached std.pcm modules drift from the TUs
// (config-mismatch / unstaged-module failures observed on macos CI).
std::string deployment_target(std::string_view manifestValue);

// Return macOS-specific runtime library directories for LLVM toolchains.
std::string deployment_target(std::string_view manifestValue) {
#if defined(__APPLE__)
    if (const char* dt = std::getenv("MACOSX_DEPLOYMENT_TARGET"); dt && *dt)
        return dt;
    return std::string(manifestValue);
#else
    (void)manifestValue;
    return {};
#endif
}

std::vector<std::filesystem::path>
runtime_lib_dirs(const std::filesystem::path& toolchain_root);

} // namespace mcpp::platform::macos

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::macos {

namespace {

std::string run_capture_trimmed(const std::string& cmd) {
    std::array<char, 4096> buf{};
    std::string out;
#if defined(__APPLE__)
    std::string full = cmd + " </dev/null";
    std::FILE* fp = ::popen(full.c_str(), "r");
    if (!fp) return {};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr)
        out += buf.data();
    ::pclose(fp);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
#else
    (void)cmd;
    (void)buf;
#endif
    return out;
}

} // namespace

bool has_xcode_clt() {
#if defined(__APPLE__)
    int rc = std::system("xcode-select -p </dev/null >/dev/null 2>&1");
    return rc == 0;
#else
    return false;
#endif
}

std::optional<std::filesystem::path> sdk_path() {
#if defined(__APPLE__)
    auto result = run_capture_trimmed("xcrun --show-sdk-path 2>/dev/null");
    if (!result.empty() && std::filesystem::exists(result))
        return std::filesystem::path(result);
#endif
    return std::nullopt;
}

std::vector<std::filesystem::path>
runtime_lib_dirs(const std::filesystem::path& toolchain_root) {
    std::vector<std::filesystem::path> dirs;
#if defined(__APPLE__)
    auto add = [&](const std::filesystem::path& p) {
        if (std::filesystem::exists(p))
            dirs.push_back(p);
    };
    add(toolchain_root / "lib" / "aarch64-apple-darwin");
    add(toolchain_root / "lib" / "darwin");
#else
    (void)toolchain_root;
#endif
    return dirs;
}

} // namespace mcpp::platform::macos
