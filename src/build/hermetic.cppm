// mcpp.build.hermetic — turn the sandbox self-containment promise into an
// executable assertion.
//
// A sandbox toolchain that cannot resolve its CRT startup objects
// (Scrt1.o/crti.o/crtn.o) or dynamic linker inside the sandbox does one of
// two things, both silent until now: on hosts WITH a system toolchain the
// driver falls back to the host's /lib (host contamination — the build goes
// green while linking a C runtime the sandbox never promised), and on hosts
// WITHOUT one it passes bare CRT names that the linker cannot open (issue
// #195). This check dry-runs the driver (`-###`) with the exact link flags
// the build will use and asserts every CRT object + the dynamic linker
// resolve under an allowed sandbox prefix — converting both failure modes
// into one actionable diagnostic at build time.
//
// Scope: Linux, sandbox toolchains only (a compiler outside the xpkgs
// registry — `[toolchain] system`, a PATH compiler — is the user's explicit
// choice of the host world and is skipped). Escape hatches for users who
// genuinely want host linking: `[build] allow_host_libs = true` or
// MCPP_ALLOW_HOST_LIBS=1 (downgrade to a verbose note).

module;
#include <cstdlib>  // getenv

export module mcpp.build.hermetic;

import std;
import mcpp.log;
import mcpp.platform;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.model;

export namespace mcpp::build {

// Verify the link is hermetic. `ldflagsNinja` is the ninja-escaped ldflags
// string from flags.cppm (un-escaped internally). Returns an error message
// naming the leaked/bare paths, or empty success. `outputDir` caches the
// verdict per flag-set so unchanged builds don't re-spawn the driver.
std::expected<void, std::string> verify_hermetic_link(
    const mcpp::toolchain::Toolchain& tc,
    const std::string& ldflagsNinja,
    const std::filesystem::path& outputDir,
    bool allowHostLibs);

} // namespace mcpp::build

namespace mcpp::build {

namespace {

// Reverse flags.cppm's escape_path (space/'$'/':' are prefixed with '$').
std::string unescape_ninja(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '$' && i + 1 < s.size()) { out.push_back(s[++i]); continue; }
        out.push_back(s[i]);
    }
    return out;
}

bool is_crt_object(std::string_view base) {
    static constexpr std::array<std::string_view, 7> kCrt = {
        "Scrt1.o", "crt1.o", "gcrt1.o", "rcrt1.o", "Mcrt1.o", "crti.o", "crtn.o",
    };
    for (auto c : kCrt)
        if (base == c) return true;
    return base.starts_with("crtbegin") || base.starts_with("crtend")
        || base.find("clang_rt.crt") != std::string_view::npos;
}

bool under_any(const std::filesystem::path& p,
               const std::vector<std::filesystem::path>& prefixes) {
    auto s = p.lexically_normal().string();
    for (auto& pre : prefixes) {
        if (pre.empty()) continue;
        if (s.starts_with(pre.lexically_normal().string())) return true;
    }
    return false;
}

// Split a `-###` output line into tokens; the driver quotes each argument
// with double quotes.
std::vector<std::string> tokenize(std::string_view out) {
    std::vector<std::string> toks;
    std::string cur;
    bool inQuote = false;
    for (char c : out) {
        if (c == '"') {
            if (inQuote) { toks.push_back(cur); cur.clear(); }
            inQuote = !inQuote;
            continue;
        }
        if (inQuote) cur.push_back(c);
        else if (!std::isspace(static_cast<unsigned char>(c))) cur.push_back(c);
        else if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}

}  // namespace

std::expected<void, std::string> verify_hermetic_link(
    const mcpp::toolchain::Toolchain& tc,
    const std::string& ldflagsNinja,
    const std::filesystem::path& outputDir,
    bool allowHostLibs)
{
    if constexpr (!mcpp::platform::is_linux) return {};

    // Sandbox toolchains only: the compiler must live under an xpkgs
    // registry. A host/system compiler is the user's explicit opt-in to the
    // host world.
    auto binStr = tc.binaryPath.string();
    auto xpkgsPos = binStr.find("xpkgs");
    if (xpkgsPos == std::string::npos) return {};
    std::vector<std::filesystem::path> allowed;
    allowed.emplace_back(binStr.substr(0, xpkgsPos + 5));  // the xpkgs root
    // Payloads may be symlink-inherited from another MCPP_HOME (the e2e
    // isolation pattern): mcpp passes the symlink-view paths on the command
    // line, but the driver reports its own resource dir (clang_rt.crt*)
    // through the CANONICAL path. Allow that registry too.
    {
        std::error_code ec;
        auto canon = std::filesystem::weakly_canonical(tc.binaryPath, ec).string();
        auto pos = ec ? std::string::npos : canon.find("xpkgs");
        if (pos != std::string::npos)
            allowed.emplace_back(canon.substr(0, pos + 5));
    }
    if (!tc.sysroot.empty()) allowed.push_back(tc.sysroot);

    if (const char* e = std::getenv("MCPP_ALLOW_HOST_LIBS"); e && *e && *e != '0')
        allowHostLibs = true;

    auto ldflags = unescape_ninja(ldflagsNinja);

    // Verdict cache: same driver + flags ⇒ same resolution; skip the spawn.
    auto marker = outputDir / ".mcpp-hermetic-ok";
    auto key = mcpp::toolchain::hash_string(
        tc.binaryPath.string() + "\x1f" + ldflags
        + "\x1f" + (allowHostLibs ? "1" : "0"));
    {
        std::ifstream is(marker);
        std::string prev;
        if (is && std::getline(is, prev) && prev == key) return {};
    }

    auto cmd = std::format("{} {} -### -x c++ /dev/null -o /dev/null 2>&1",
                           mcpp::platform::shell::quote(tc.binaryPath.string()),
                           ldflags);
    auto r = mcpp::platform::process::capture(cmd);
    if (r.exit_code != 0) {
        // The dry-run itself failing is a link-configuration problem the real
        // link will also hit; don't duplicate the diagnosis here.
        mcpp::log::verbose("hermetic", std::format(
            "-### dry-run failed (rc={}) — skipping hermeticity check", r.exit_code));
        return {};
    }

    std::vector<std::string> leaks;
    auto check = [&](std::string_view value, bool bareIsLeak = true) {
        std::filesystem::path p{std::string(value)};
        if (!p.is_absolute()) {
            if (bareIsLeak)
                leaks.push_back(std::format(
                    "{} (bare name — the linker cannot resolve it)", std::string(value)));
        } else if (!under_any(p, allowed)) {
            leaks.push_back(std::format("{} (outside the sandbox)", std::string(value)));
        }
    };
    auto toks = tokenize(r.output);
    // The dynamic linker may appear more than once (the driver emits its
    // built-in default, then the -Wl override follows); the LAST occurrence
    // on the linker command line wins, so only that one is checked.
    std::string effectiveLoader;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        std::string_view t = toks[i];
        if (t == "-dynamic-linker" && i + 1 < toks.size()) {
            effectiveLoader = toks[++i];
            continue;
        }
        if (t.starts_with("--dynamic-linker=")) {
            effectiveLoader = std::string(t.substr(17));
            continue;
        }
        auto base = std::filesystem::path(std::string(t)).filename().string();
        if (is_crt_object(base)) check(t);
    }
    if (!effectiveLoader.empty()) check(effectiveLoader);

    if (!leaks.empty()) {
        std::string list;
        for (auto& l : leaks) list += "\n         " + l;
        auto msg = std::format(
            "hermetic link check failed — the sandbox toolchain resolves its C "
            "runtime outside the sandbox:{}\n"
            "       allowed prefixes: {}\n"
            "       This usually means the glibc payload is missing or the "
            "toolchain install is incomplete (try `mcpp toolchain install` again).\n"
            "       To deliberately link against host libraries set "
            "[build] allow_host_libs = true (or MCPP_ALLOW_HOST_LIBS=1).",
            list, allowed.front().string());
        if (!allowHostLibs) return std::unexpected(msg);
        mcpp::log::verbose("hermetic", "allow_host_libs set — " + msg);
    }

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    std::ofstream os(marker);
    os << key << "\n";
    return {};
}

} // namespace mcpp::build
