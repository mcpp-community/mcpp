// mcpp.platform.macos — macOS-specific platform capabilities.
//
// Provides:
//   has_xcode_clt()       — detect Xcode Command Line Tools
//   sdk_path(sdk)         — discover an Apple SDK via xcrun ("macosx" default)
//   sdk_layout(sdk)       — the directory names that SDK uses inside Xcode
//   sdk_name_of_root(p)   — which SDK a path IS, for the SDKROOT override
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

// THREE SDKS, ONE MACHINE. Apple ships the macOS, iPhoneOS and
// iPhoneSimulator SDKs from one developer directory, and mcpp locates
// rather than installs all three: they are not redistributable, which
// is why the iOS rows carry a located `sysroot` instead of a package.
// `sdk` is the name `xcrun --sdk` takes.
inline constexpr std::string_view sdk_macos     = "macosx";
inline constexpr std::string_view sdk_iphoneos  = "iphoneos";
inline constexpr std::string_view sdk_iphonesim = "iphonesimulator";

// Where an SDK lives inside a developer directory: the `.platform`
// directory that holds it, and its own `.sdk` directory name. Pure, and
// nullopt for a name no Apple SDK answers to.
struct SdkLayout {
    std::string_view platformDir;   // e.g. "iPhoneOS.platform"
    std::string_view sdkDir;        // e.g. "iPhoneOS.sdk"
    bool             inCommandLineTools;  // the CLT-only install ships it
};
std::optional<SdkLayout> sdk_layout(std::string_view sdk);

// Which SDK a path IS, derived from its own directory name:
// `MacOSX15.4.sdk` -> "macosx", `iPhoneSimulator18.4.sdk` ->
// "iphonesimulator". Empty when the path does not name an SDK.
//
// THIS EXISTS BECAUSE `SDKROOT` NAMES ONE SDK AND THERE ARE THREE
// QUESTIONS. Honouring it for every request would answer an iOS query
// with a macOS SDK whenever a shell had it set -- a wrong sysroot, which
// fails later as missing headers rather than as a bad override.
std::string sdk_name_of_root(const std::filesystem::path& p);

// Whether an `SDKROOT` override answers a request for `sdk`.
//
// A path that names an SDK answers for that SDK and no other. A path that
// names NO SDK -- a hand-rolled sysroot, which is a spelling clang accepts --
// answers the DEFAULT question only: it is an answer to "the SDK", not to
// "the iPhoneOS SDK". That asymmetry is what keeps today's callers unchanged
// while making a specific request require positive identification.
bool sdkroot_answers(const std::filesystem::path& candidate,
                     std::string_view sdk);

// Discover an Apple SDK path via `xcrun --show-sdk-path`.
// Returns the SDK path if found, or nullopt. The default argument is
// today's behaviour for today's callers.
std::optional<std::filesystem::path> sdk_path(std::string_view sdk = sdk_macos);

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

std::optional<SdkLayout> sdk_layout(std::string_view sdk) {
    // ONE TABLE, THREE READERS. Every step of `sdk_path` below that names a
    // directory reads it from here, so a fourth SDK is a row and not four
    // edits. `inCommandLineTools` is false for the iOS SDKs because a
    // CLT-only install genuinely does not ship them -- which is a reason the
    // iOS rows can be refused, not a path to probe.
    static constexpr SdkLayout kLayouts[] = {
        { "MacOSX.platform",          "MacOSX.sdk",          true  },
        { "iPhoneOS.platform",        "iPhoneOS.sdk",        false },
        { "iPhoneSimulator.platform", "iPhoneSimulator.sdk", false },
    };
    if (sdk == sdk_macos)     return kLayouts[0];
    if (sdk == sdk_iphoneos)  return kLayouts[1];
    if (sdk == sdk_iphonesim) return kLayouts[2];
    return std::nullopt;
}

std::string sdk_name_of_root(const std::filesystem::path& p) {
    // The installed name carries a version -- `MacOSX15.4.sdk` -- and the
    // requested name does not. Lowercase, drop the suffix, drop the digits.
    //
    // THE SUFFIX IS REQUIRED, because this is positive identification and
    // `.sdk` is the marker Apple's own naming gives it. A directory merely
    // called `MacOSX` names no SDK; `sdkroot_answers` then routes it to the
    // default question, which is what it was before this took a parameter.
    std::string name = p.filename().string();
    for (auto& c : name) c = static_cast<char>(std::tolower(c));
    if (!name.ends_with(".sdk")) return {};
    name.resize(name.size() - 4);
    while (!name.empty() && (std::isdigit(static_cast<unsigned char>(name.back()))
                             || name.back() == '.'))
        name.pop_back();
    for (auto known : {sdk_macos, sdk_iphoneos, sdk_iphonesim})
        if (name == known) return std::string(known);
    return {};
}

bool sdkroot_answers(const std::filesystem::path& candidate,
                     std::string_view sdk) {
    if (!sdk_layout(sdk)) return false;
    auto named = sdk_name_of_root(candidate);
    if (!named.empty()) return named == sdk;
    return sdk == sdk_macos;
}

std::optional<std::filesystem::path> sdk_path(std::string_view sdk) {
    auto layout = sdk_layout(sdk);
    if (!layout) return std::nullopt;
#if defined(__APPLE__)
    // 1. Explicit override wins (matches clang's own SDKROOT handling) -- but
    //    only for a request it answers. See `sdkroot_answers`.
    if (const char* env = std::getenv("SDKROOT"); env && *env) {
        std::filesystem::path p(env);
        if (std::filesystem::exists(p) && sdkroot_answers(p, sdk)) return p;
    }
    // 2. xcrun — the canonical query. The named form is the only one that can
    //    answer for a specific SDK; the generic form returns the ACTIVE
    //    default, so it is tried only for the default request, where it is
    //    today's first probe and stays first.
    std::vector<std::string> cmds;
    if (sdk == sdk_macos) cmds.emplace_back("xcrun --show-sdk-path 2>/dev/null");
    cmds.push_back(std::format("xcrun --sdk {} --show-sdk-path 2>/dev/null", sdk));
    for (auto const& cmd : cmds) {
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
                base / "Platforms" / layout->platformDir / "Developer" / "SDKs"
                     / layout->sdkDir,
                base / "SDKs" / layout->sdkDir }) {
            if (std::filesystem::exists(cand)) return cand;
        }
    }
    // 4. Well-known fixed locations (Command-Line-Tools-only / standard Xcode).
    std::vector<std::filesystem::path> fixed;
    if (layout->inCommandLineTools)
        fixed.emplace_back(std::filesystem::path(
            "/Library/Developer/CommandLineTools/SDKs") / layout->sdkDir);
    fixed.emplace_back(std::filesystem::path(
        "/Applications/Xcode.app/Contents/Developer/Platforms")
        / layout->platformDir / "Developer" / "SDKs" / layout->sdkDir);
    for (auto const& cand : fixed)
        if (std::filesystem::exists(cand)) return cand;
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
