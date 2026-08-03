// mcpp.toolchain.ohos — HarmonyOS / OpenHarmony platform SDK discovery.
//
// This module answers ONE question: given a `*-linux-ohos` target, where are
// the platform's C library, C++ standard library and compiler runtime? It
// never answers "which compiler" — that stays mcpp's own LLVM payload.
//
// ── Why the SDK is a sysroot and not a toolchain ────────────────────────────
//
// The obvious design is "use the NDK's clang, like everyone else does". It
// does not work, and not marginally: **OpenHarmony SDK 6.1 (API 23, released
// 2026-03) still ships clang 15.0.4** — measured, not assumed. C++20 modules
// need `-fmodule-output` (clang 16) and mcpp's whole build model is modules;
// `import std` needs libc++ ≥ 19. The SDK's compiler is four major versions
// short of mcpp's floor and has been for years, so "wait for the vendor" is
// not a plan.
//
// So mcpp takes the shape .agents/docs/2026-07-24-embedded-platform-support-design.md
// calls config②: **mcpp brings the compiler, the platform brings the sysroot.**
// HarmonyOS is the first target where that is not a preference but the only
// possibility — GCC has no `ohos` target at all, so the "bring a matching-libc
// GCC" half of that design cannot be spelled here and the archived clang route
// (decision #8/D) becomes the required one.
//
// What mcpp consumes from the SDK:
//   <native>/sysroot                              → --sysroot (libc, crt1/crti/crtn)
//   <native>/llvm/include/libcxx-ohos/include/c++/v1 → target libc++ headers
//   <native>/llvm/lib/<triple>/                   → target libc++/libc++abi/libunwind
//   <native>/llvm/lib/clang/<ver>/                → target compiler-rt + clang_rt.crt*
//
// The last one is why `CrossTarget::linkResourceDir` exists: upstream clang's
// OHOS driver looks for `libclang_rt.builtins.a` and `clang_rt.crt{begin,end}.o`
// under ITS OWN resource dir, which of course has no aarch64-linux-ohos
// subdirectory. Pointing `-resource-dir` at the SDK's on the LINK line only
// supplies them; doing it on the compile line too would feed clang-22 the
// clang-15 intrinsic headers, which is a different and much quieter bug.

module;
#include <cstdlib>     // getenv

export module mcpp.toolchain.ohos;

import std;
import mcpp.toolchain.triple;
import mcpp.platform;
import mcpp.log;

export namespace mcpp::toolchain::ohos {

// A located OpenHarmony native SDK ("<sdk>/native", the NDK proper).
struct SdkInstallation {
    std::filesystem::path root;          // .../native
    std::filesystem::path sysroot;       // <root>/sysroot
    std::filesystem::path llvmRoot;      // <root>/llvm
    std::filesystem::path libcxxInclude; // OHOS-patched libc++ headers
    std::filesystem::path resourceDir;   // <llvmRoot>/lib/clang/<version>
    std::string sdkVersion;              // "6.1.0.31"     (oh-uni-package.json)
    std::string apiVersion;              // "23"           (oh-uni-package.json)
    std::string source;                  // which knob found it (diagnostics)

    std::string display() const {
        std::string s = "OpenHarmony SDK";
        if (!sdkVersion.empty()) { s += ' '; s += sdkVersion; }
        if (!apiVersion.empty()) { s += " (API "; s += apiVersion; s += ')'; }
        return s;
    }
};

// Locate a usable SDK, or nullopt. Never throws, never installs.
std::optional<SdkInstallation> detect_installation();

// Actionable text for "no SDK found" — the msvc@system precedent: mcpp cannot
// install this SDK (it is a ~2.5 GB vendor archive behind a EULA), so the only
// honest response is to say exactly what to set.
std::string install_guidance();

// The SDK's per-target library dir, e.g. <llvm>/lib/aarch64-linux-ohos.
// Empty when the SDK does not carry that target.
std::filesystem::path target_lib_dir(const SdkInstallation& sdk,
                                     const triple::Triple& t);

// The SDK's per-target compiler-rt dir (builtins + clang_rt.crt*).
std::filesystem::path target_runtime_dir(const SdkInstallation& sdk,
                                         const triple::Triple& t);

// An externally-supplied libc++ built FOR the ohos target, which is what
// upgrades a HarmonyOS build from "named modules only" to "import std".
//
// Deliberately env-first and index-last: an index-side package must never be
// what decides whether mcpp works (see
// .agents/docs/2026-08-03-index-availability-must-not-decide-mcpp-availability.md).
// The SDK's own libc++ is always there and always usable; this is the upgrade.
struct LibcxxOverlay {
    std::filesystem::path include;   // <root>/include/c++/v1
    std::filesystem::path lib;       // <root>/lib
    std::filesystem::path stdModule; // <root>/share/libc++/v1/std.cppm ("" = none)
};
std::optional<LibcxxOverlay> detect_libcxx_overlay(const triple::Triple& t);

// Assemble the CrossTarget-shaped answer for `t`. Returns the pieces; the
// caller (detect.cppm) owns stitching them onto a Toolchain, so this module
// stays free of the toolchain model.
struct CrossPaths {
    std::filesystem::path sysroot;
    std::vector<std::filesystem::path> cxxIncludes;
    std::vector<std::filesystem::path> libDirs;
    std::filesystem::path linkResourceDir;
    std::filesystem::path stdModuleSource;   // "" when the target has no std module
    std::string provider;
};
std::expected<CrossPaths, std::string>
cross_paths(const SdkInstallation& sdk, const triple::Triple& t);

} // namespace mcpp::toolchain::ohos

namespace mcpp::toolchain::ohos {

namespace {

std::string env_or_empty(const char* key) {
    if (const char* v = std::getenv(key); v && *v) return v;
    return {};
}

bool path_exists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

// Extract `"key": "value"` from the SDK's tiny oh-uni-package.json. A real
// JSON parser would be overkill for a two-field read of a vendor-generated
// file, and mcpp's own TOML reader does not apply.
std::string json_field(const std::string& body, std::string_view key) {
    auto k = std::string("\"") + std::string(key) + "\"";
    auto pos = body.find(k);
    if (pos == std::string::npos) return {};
    auto colon = body.find(':', pos + k.size());
    if (colon == std::string::npos) return {};
    auto q1 = body.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    auto q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return body.substr(q1 + 1, q2 - q1 - 1);
}

// Is `p` the `native` directory of an OpenHarmony SDK? Judged by the two
// things mcpp actually needs, not by the directory's name: a path that merely
// looks right but carries no sysroot must be rejected here rather than fail
// later inside a compile command.
bool looks_like_native_sdk(const std::filesystem::path& p) {
    return path_exists(p / "sysroot" / "usr" / "include" / "stdlib.h")
        && path_exists(p / "llvm" / "lib");
}

// Candidate `native` roots, in priority order. Each entry is (path, provenance).
std::vector<std::pair<std::filesystem::path, std::string>> native_candidates() {
    std::vector<std::pair<std::filesystem::path, std::string>> out;
    auto add = [&](std::filesystem::path p, std::string why) {
        if (!p.empty()) out.emplace_back(std::move(p), std::move(why));
    };

    // 1. Explicit, unambiguous: the NDK root itself.
    if (auto v = env_or_empty("OHOS_NDK_HOME"); !v.empty()) {
        add(v, "$OHOS_NDK_HOME");
        add(std::filesystem::path(v) / "native", "$OHOS_NDK_HOME/native");
    }
    // openharmony-rs/setup-ohos-sdk exports this one, and it already points
    // AT the native dir — which is why it is checked as-is.
    if (auto v = env_or_empty("OHOS_SDK_NATIVE"); !v.empty())
        add(v, "$OHOS_SDK_NATIVE");

    // 2. SDK roots, which nest the API level in some layouts and not others.
    for (auto key : {"OHOS_SDK_HOME", "HOS_SDK_HOME", "DEVECO_SDK_HOME"}) {
        auto v = env_or_empty(key);
        if (v.empty()) continue;
        std::string why = std::string("$") + key;
        add(std::filesystem::path(v) / "native", why + "/native");
        // <sdk>/<api>/native — DevEco's layout. Pick the highest API level
        // present rather than the first directory the filesystem hands back.
        std::error_code ec;
        std::vector<std::pair<int, std::filesystem::path>> levels;
        for (auto& e : std::filesystem::directory_iterator(v, ec)) {
            std::error_code de;
            if (!e.is_directory(de)) continue;
            auto name = e.path().filename().string();
            // from_chars, not stoi: a 20-digit directory name is not an API
            // level but it IS a valid unsigned literal, and stoi would throw
            // out_of_range out of a discovery routine that promises not to.
            int level = 0;
            auto* first = name.data();
            auto* last  = first + name.size();
            auto [ptr, ec2] = std::from_chars(first, last, level);
            if (ec2 != std::errc{} || ptr != last) continue;
            levels.emplace_back(level, e.path() / "native");
        }
        std::ranges::sort(levels, std::ranges::greater{},
                          &std::pair<int, std::filesystem::path>::first);
        for (auto& [lvl, p] : levels)
            add(p, std::format("{}/{}/native", why, lvl));
    }

    // 3. Conventional unpack locations, so a developer who followed the
    //    upstream instructions needs no environment at all.
    if constexpr (!mcpp::platform::is_windows) {
        if (auto home = env_or_empty("HOME"); !home.empty()) {
            add(std::filesystem::path(home) / "ohos-sdk" / "native",
                "~/ohos-sdk/native");
            add(std::filesystem::path(home) / "command-line-tools" / "sdk"
                    / "default" / "openharmony" / "native",
                "~/command-line-tools (DevEco CLI)");
        }
        add("/opt/ohos-sdk/native", "/opt/ohos-sdk/native");
        add("/usr/local/ohos-sdk/native", "/usr/local/ohos-sdk/native");
    }
    return out;
}

} // namespace

std::optional<SdkInstallation> detect_installation() {
    for (auto& [cand, why] : native_candidates()) {
        if (!looks_like_native_sdk(cand)) {
            mcpp::log::debug("ohos", std::format(
                "candidate '{}' ({}) is not an OpenHarmony native SDK",
                cand.string(), why));
            continue;
        }
        SdkInstallation sdk;
        std::error_code ec;
        sdk.root = std::filesystem::weakly_canonical(cand, ec);
        if (ec) sdk.root = cand;
        sdk.sysroot  = sdk.root / "sysroot";
        sdk.llvmRoot = sdk.root / "llvm";
        sdk.source   = why;

        // The OHOS-patched libc++ header tree. The plain <llvm>/include/c++/v1
        // in the same payload is the HOST's (its __config_site is generated
        // for x86_64-unknown-linux-gnu) — picking it would compile the target
        // against host libc++ configuration and fail far from here.
        auto ohosCxx = sdk.llvmRoot / "include" / "libcxx-ohos" / "include"
                     / "c++" / "v1";
        if (path_exists(ohosCxx / "__config_site")) sdk.libcxxInclude = ohosCxx;

        // <llvm>/lib/clang/<version>/ — exactly one version dir in practice,
        // but sorted so the answer never depends on iteration order.
        std::vector<std::filesystem::path> resDirs;
        for (auto& e : std::filesystem::directory_iterator(
                 sdk.llvmRoot / "lib" / "clang", ec)) {
            std::error_code de;
            if (e.is_directory(de)) resDirs.push_back(e.path());
        }
        std::ranges::sort(resDirs);
        if (!resDirs.empty()) sdk.resourceDir = resDirs.back();

        if (auto pkg = sdk.root / "oh-uni-package.json"; path_exists(pkg)) {
            std::ifstream is(pkg);
            std::stringstream ss;
            ss << is.rdbuf();
            auto body = ss.str();
            sdk.sdkVersion = json_field(body, "version");
            sdk.apiVersion = json_field(body, "apiVersion");
        }

        mcpp::log::verbose("ohos", std::format(
            "{} at {} (via {})", sdk.display(), sdk.root.string(), sdk.source));
        return sdk;
    }
    return std::nullopt;
}

std::string install_guidance() {
    return
        "no OpenHarmony SDK found — a `*-linux-ohos` target needs the platform\n"
        "sysroot, which mcpp does not ship (it is a vendor archive under its own\n"
        "licence).\n"
        "\n"
        "  1. Download the SDK ('ohos-sdk' / 'Native' component) from\n"
        "     https://gitee.com/openharmony/docs (release notes -> SDK), or in CI\n"
        "     use openharmony-rs/setup-ohos-sdk.\n"
        "  2. Point mcpp at the unpacked `native` directory:\n"
        "\n"
        "       export OHOS_NDK_HOME=/path/to/ohos-sdk/linux/native\n"
        "\n"
        "mcpp uses the SDK for its sysroot and runtime libraries only — the\n"
        "compiler stays mcpp's own LLVM, because the SDK's bundled clang is\n"
        "15.0.4 even in SDK 6.1 and cannot build C++20 modules.";
}

std::filesystem::path target_lib_dir(const SdkInstallation& sdk,
                                     const triple::Triple& t) {
    auto p = sdk.llvmRoot / "lib" / t.str();
    return path_exists(p) ? p : std::filesystem::path{};
}

std::filesystem::path target_runtime_dir(const SdkInstallation& sdk,
                                         const triple::Triple& t) {
    if (sdk.resourceDir.empty()) return {};
    auto p = sdk.resourceDir / "lib" / t.str();
    return path_exists(p) ? p : std::filesystem::path{};
}

std::optional<LibcxxOverlay> detect_libcxx_overlay(const triple::Triple& t) {
    std::vector<std::pair<std::filesystem::path, std::string>> roots;
    // Per-target first, so one machine can hold overlays for several targets.
    auto perTarget = std::string("MCPP_OHOS_LIBCXX_")
                   + [&] { std::string s = t.str();
                           for (auto& c : s)
                               c = (c == '-') ? '_'
                                              : static_cast<char>(std::toupper(
                                                    static_cast<unsigned char>(c)));
                           return s; }();
    if (auto v = env_or_empty(perTarget.c_str()); !v.empty())
        roots.emplace_back(v, perTarget);
    if (auto v = env_or_empty("MCPP_OHOS_LIBCXX"); !v.empty())
        roots.emplace_back(v, "$MCPP_OHOS_LIBCXX");

    for (auto& [root, why] : roots) {
        LibcxxOverlay ov;
        ov.include = root / "include" / "c++" / "v1";
        ov.lib     = root / "lib";
        if (!path_exists(ov.include / "__config_site") || !path_exists(ov.lib)) {
            mcpp::log::debug("ohos", std::format(
                "libc++ overlay '{}' ({}) has no include/c++/v1/__config_site "
                "+ lib/ — ignoring", root.string(), why));
            continue;
        }
        auto stdm = root / "share" / "libc++" / "v1" / "std.cppm";
        if (path_exists(stdm)) ov.stdModule = stdm;
        mcpp::log::verbose("ohos", std::format(
            "libc++ overlay for {} at {} (via {}){}",
            t.str(), root.string(), why,
            ov.stdModule.empty() ? " — no std module" : " — with std module"));
        return ov;
    }
    return std::nullopt;
}

std::expected<CrossPaths, std::string>
cross_paths(const SdkInstallation& sdk, const triple::Triple& t) {
    CrossPaths cp;
    cp.sysroot  = sdk.sysroot;
    cp.provider = sdk.display();

    auto sdkLib = target_lib_dir(sdk, t);
    if (sdkLib.empty())
        return std::unexpected(std::format(
            "{} carries no libraries for '{}' (looked in {}); the SDK's "
            "supported targets are the directories under that path",
            sdk.display(), t.str(), (sdk.llvmRoot / "lib").string()));

    auto rtDir = target_runtime_dir(sdk, t);
    if (rtDir.empty())
        return std::unexpected(std::format(
            "{} carries no compiler runtime for '{}' (expected "
            "libclang_rt.builtins.a under {})",
            sdk.display(), t.str(),
            (sdk.resourceDir.empty() ? sdk.llvmRoot / "lib" / "clang"
                                     : sdk.resourceDir / "lib").string()));
    // The resource dir, not its per-target subdir: `-resource-dir` names the
    // dir clang appends `lib/<triple>/` to itself.
    cp.linkResourceDir = sdk.resourceDir;

    // Header + library search order is the whole upgrade mechanism: a
    // purpose-built target libc++ (with a std module) goes FIRST, the SDK's
    // patched libc++ 15 is the always-available fallback. Both lib dirs stay
    // on the line — the overlay does not ship a usable libunwind for this
    // target, so the platform's is what actually resolves.
    if (auto ov = detect_libcxx_overlay(t)) {
        cp.cxxIncludes.push_back(ov->include);
        cp.libDirs.push_back(ov->lib);
        cp.stdModuleSource = ov->stdModule;
        cp.provider += " + external libc++";
    } else if (!sdk.libcxxInclude.empty()) {
        cp.cxxIncludes.push_back(sdk.libcxxInclude);
    } else {
        return std::unexpected(std::format(
            "{} has no OHOS libc++ headers (expected "
            "llvm/include/libcxx-ohos/include/c++/v1)", sdk.display()));
    }
    cp.libDirs.push_back(sdkLib);
    return cp;
}

} // namespace mcpp::toolchain::ohos
