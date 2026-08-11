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

// Built-in default deployment floor (rustc-style: every target has a
// baseline). 14.0 = the floor of the official LLVM static libc++
// archives; with the default-static stdlib this makes `mcpp run`
// binaries portable to any macOS ≥ 14 out of the box (no declaration
// needed — a fresh user's std::println hello on macOS 14 used to die
// at dyld against the system libc++). Lower floors need a custom
// libc++ build (tracked; data-only swap via xlings-res).
inline constexpr std::string_view default_deployment_target = "14.0";

// Resolve the effective macOS deployment target: the
// MACOSX_DEPLOYMENT_TARGET env var (explicit per-invocation override,
// the convention cargo/rustc/cc honor) wins over `manifestValue` (the
// [build] macos_deployment_target project default), which wins over
// the built-in default floor — the result is never empty on macOS.
// THE single source of truth — flags.cppm, the BMI fingerprint rule
// and the std-module prebuild must all consume this same resolution,
// or cached std.pcm modules drift from the TUs (config-mismatch /
// unstaged-module failures observed on macos CI).
std::string deployment_target(std::string_view manifestValue);

// Return macOS-specific runtime library directories for LLVM toolchains.
std::string deployment_target(std::string_view manifestValue) {
#if defined(__APPLE__)
    if (const char* dt = std::getenv("MACOSX_DEPLOYMENT_TARGET"); dt && *dt)
        return dt;
    if (!manifestValue.empty())
        return std::string(manifestValue);
    return std::string(default_deployment_target);
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
    // 1. Explicit override wins (matches clang's own SDKROOT handling).
    if (const char* env = std::getenv("SDKROOT"); env && *env) {
        std::filesystem::path p(env);
        if (std::filesystem::exists(p)) return p;
    }
    // 2. xcrun — the canonical query. Try the generic form, then the
    //    macosx-specific one (works even when the active developer dir's
    //    default SDK isn't macOS, e.g. an iOS-defaulted setup).
    for (const char* cmd : {"xcrun --show-sdk-path 2>/dev/null",
                            "xcrun --sdk macosx --show-sdk-path 2>/dev/null"}) {
        auto result = run_capture_trimmed(cmd);
        if (!result.empty() && std::filesystem::exists(result))
            return std::filesystem::path(result);
    }
    // 3. Derive from the active developer dir (`xcode-select -p`) — covers
    //    machines where xcrun is misconfigured but the SDK is present.
    auto devdir = run_capture_trimmed("xcode-select -p 2>/dev/null");
    if (!devdir.empty()) {
        std::filesystem::path base(devdir);
        for (auto cand : {
                base / "Platforms" / "MacOSX.platform" / "Developer" / "SDKs" / "MacOSX.sdk",
                base / "SDKs" / "MacOSX.sdk" }) {
            if (std::filesystem::exists(cand)) return cand;
        }
    }
    // 4. Well-known fixed locations (Command-Line-Tools-only / standard Xcode).
    for (const char* p : {
            "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
            "/Applications/Xcode.app/Contents/Developer/Platforms/"
            "MacOSX.platform/Developer/SDKs/MacOSX.sdk" }) {
        if (std::filesystem::exists(p)) return std::filesystem::path(p);
    }
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
