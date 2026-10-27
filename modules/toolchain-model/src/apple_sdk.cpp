// mcpp.toolchain.apple_sdk — the implementation: the queries of Apple's own
// tools.
module mcpp.toolchain.apple_sdk;

import std;
import mcpp.invocation;
import mcpplibs.os.platform;
import mcpplibs.os.process;

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::toolchain::apple {

namespace {

// What an Apple tool prints, trimmed; empty when it cannot be run or fails.
// Its error output is not read: a probe parses what it captured.
std::string run_capture_trimmed(std::vector<std::string> argv) {
    mcpplibs::os::process::spawn_options o;
    o.program = argv.front();
    o.args.assign(argv.begin() + 1, argv.end());
    o.in = mcpplibs::os::process::stdio::null;
    auto r = mcpplibs::os::process::run(o);
    if (!r || !r->status.success()) return {};
    std::string out = std::move(r->out);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}
} // namespace

bool has_xcode_clt() {
    if constexpr (mcpplibs::os::is_macos) {
        return !run_capture_trimmed({"xcode-select", "-p"}).empty();
    } else {
        return false;
    }
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

std::optional<std::string> sdk_version(std::string_view sdk) {
    if constexpr (mcpplibs::os::is_macos) {
        auto v = run_capture_trimmed({"xcrun", "--sdk", std::string(sdk), "--show-sdk-version"});
        if (v.empty()) return std::nullopt;
        return v;
    } else {
        (void)sdk;
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> sdk_path(std::string_view sdk) {
    auto layout = sdk_layout(sdk);
    if (!layout) return std::nullopt;
    // 1. Explicit override wins (matches clang's own SDKROOT handling) -- but
    //    only for a request it answers. See `sdkroot_answers`.
    //
    //    ON EVERY HOST. `SDKROOT` is Apple's own convention for naming an SDK,
    //    and an SDK a developer produced from their own Xcode on Linux is
    //    named the same way (design 2026-10-23 §4.4, D18). The steps below
    //    ask Apple's tools, which exist only on an Apple host.
    if (auto env = mcpp::invocation::get("SDKROOT")) {
        std::filesystem::path p(*env);
        if (std::filesystem::exists(p) && sdkroot_answers(p, sdk)) return p;
    }
    if constexpr (mcpplibs::os::is_macos) {
        // 2. xcrun — the canonical query. The named form is the only one that can
        //    answer for a specific SDK; the generic form returns the ACTIVE
        //    default, so it is tried only for the default request, where it is
        //    today's first probe and stays first.
        std::vector<std::vector<std::string>> cmds;
        if (sdk == sdk_macos) cmds.push_back({"xcrun", "--show-sdk-path"});
        cmds.push_back({"xcrun", "--sdk", std::string(sdk), "--show-sdk-path"});
        for (auto const& cmd : cmds) {
            auto result = run_capture_trimmed(cmd);
            if (!result.empty() && std::filesystem::exists(result))
                return std::filesystem::path(result);
        }
        // 3. Derive from the active developer dir (`xcode-select -p`) — covers
        //    machines where xcrun is misconfigured but the SDK is present.
        auto devdir = run_capture_trimmed({"xcode-select", "-p"});
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
    }
    return std::nullopt;
}

std::string sdk_settings_version(const std::filesystem::path& sdkRoot) {
    std::ifstream is(sdkRoot / "SDKSettings.json", std::ios::binary);
    if (!is) return {};
    std::string text((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    auto key = text.find("\"Version\"");
    if (key == std::string::npos) return {};
    auto q1 = text.find('"', text.find(':', key));
    if (q1 == std::string::npos) return {};
    auto q2 = text.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return text.substr(q1 + 1, q2 - q1 - 1);
}

Slice sdk_has_target(const std::filesystem::path& sdkRoot, std::string_view target) {
    std::ifstream is(sdkRoot / "usr" / "lib" / "libSystem.tbd", std::ios::binary);
    if (!is) return Slice::unknown;
    std::string line;
    while (std::getline(is, line)) {
        if (!line.starts_with("targets:")) continue;
        // `targets: [ x86_64-ios-simulator, arm64-ios-simulator ]`, possibly
        // wrapped onto following lines until the closing bracket.
        std::string list = line;
        while (list.find(']') == std::string::npos && std::getline(is, line)) list += line;
        std::size_t pos = 0;
        while ((pos = list.find(target, pos)) != std::string::npos) {
            const auto end = pos + target.size();
            const bool startOk = pos == 0 || !(std::isalnum(static_cast<unsigned char>(list[pos - 1]))
                                               || list[pos - 1] == '-' || list[pos - 1] == '_');
            const bool endOk = end >= list.size() || !(std::isalnum(static_cast<unsigned char>(list[end]))
                                                       || list[end] == '-' || list[end] == '_');
            if (startOk && endOk) return Slice::carried;
            pos = end;
        }
        return Slice::absent;
    }
    return Slice::unknown;
}


std::string deployment_target(bool targetIsMacos, std::string_view manifestValue) {
    if (!targetIsMacos) return {};
    if (auto dt = mcpp::invocation::get("MACOSX_DEPLOYMENT_TARGET"))
        return *dt;
    if (!manifestValue.empty())
        return std::string(manifestValue);
    return std::string(default_deployment_target);
}

} // namespace mcpp::toolchain::apple
