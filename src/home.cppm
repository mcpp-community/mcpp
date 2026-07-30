// mcpp.home — the single resolver for MCPP_HOME and its well-known subdirs.
//
// Every path under the mcpp home must be derived from here. Before #311 this
// logic existed in three places (config.cppm, toolchain/stdmod.cppm and an
// inline lambda in build/prepare.cppm); the copies drifted, and the oldest one
// (stdmod, unchanged since v0.0.1) never learned about Windows' USERPROFILE or
// about self-contained installs. The observable damage: on Windows PowerShell
// (no $HOME) the std BMI cache landed in the *current working directory* as
// `.mcpp-bmi/`, while dep BMIs went to %USERPROFILE%\.mcpp\bmi — two roots for
// one cache, and a cwd-dependent one at that.
//
// This module is deliberately a leaf: it imports std + mcpp.platform only, so
// anything (config, toolchain, build) can depend on it without cycles.

module;
#include <cstdlib>    // getenv

export module mcpp.home;

import std;
import mcpp.platform;

export namespace mcpp::home {

// $MCPP_HOME. See root() below for the resolution order.
std::filesystem::path root();

// The cross-project BMI cache root ($MCPP_HOME/bmi). Both the std module
// cache (toolchain/stdmod.cppm) and the dep BMI cache (bmi_cache.cppm, via
// GlobalConfig::bmiCacheDir) live here — they MUST agree.
std::filesystem::path bmi_root();

} // namespace mcpp::home

namespace mcpp::home {

namespace {

std::filesystem::path default_home() {
    // Windows: %USERPROFILE%\.mcpp   POSIX: $HOME/.mcpp
    if constexpr (mcpp::platform::is_windows) {
        if (auto* e = std::getenv("USERPROFILE"); e && *e)
            return std::filesystem::path(e) / ".mcpp";
    }
    if (auto* e = std::getenv("HOME"); e && *e)
        return std::filesystem::path(e) / ".mcpp";
    return std::filesystem::current_path() / ".mcpp";
}

} // namespace

// Resolve MCPP_HOME, in priority order:
//   1. $MCPP_HOME env var (explicit override — used by CI / dev / multi-instance)
//   2. <binary-dir>/.. — self-contained mode, when mcpp lives at
//      <root>/bin/mcpp. Release tarballs and `xlings install mcpp`
//      use this layout; the unpacked tree IS the home. Dev builds
//      live under target/<triple>/<fp>/bin/mcpp, which is the same
//      "in a bin/ dir" shape — so we additionally exclude any path
//      with a "target" ancestor as mcpp's own dev convention.
//   3. fallback to $HOME/.mcpp (%USERPROFILE%\.mcpp on Windows).
std::filesystem::path root() {
    // 1. Explicit $MCPP_HOME takes priority (CI, advanced users).
    if (auto* e = std::getenv("MCPP_HOME"); e && *e)
        return std::filesystem::path(e);

    auto exe = mcpp::platform::fs::self_exe_path();
    if (exe.has_parent_path() && exe.parent_path().filename() == "bin") {
        auto candidate = exe.parent_path().parent_path();

        // Disqualify self-contained mode for two cases:
        //   a) Dev builds: .../target/<triple>/<fp>/bin/<exe>
        //   b) xlings packages: .../data/xpkgs/xim-x-mcpp/<ver>/bin/mcpp
        //      Creating a nested xlings sandbox inside the xpkgs directory
        //      breaks toolchain installation (nested XLINGS_HOME) and loses
        //      installed toolchains when the mcpp package version is upgraded.
        bool disqualified = false;
        for (auto p = candidate;
             p.has_parent_path() && p != p.root_path();
             p = p.parent_path()) {
            if (p.filename() == "target") { disqualified = true; break; }
            if (p.filename() == "xpkgs") {
                auto parent = p.parent_path().filename().string();
                if (parent == "data") { disqualified = true; break; }
            }
        }
        if (!disqualified)
            return candidate;
    }

    return default_home();
}

std::filesystem::path bmi_root() {
    return root() / "bmi";
}

} // namespace mcpp::home
