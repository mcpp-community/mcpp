// mcpp.toolchain.probe - common compiler probing helpers.
//
// NOTE: This file contains its own run_capture() helper that returns
// std::expected<std::string, DetectError> — a different signature from
// mcpp::process::run_capture() (which returns RunResult).  Do NOT migrate
// existing callers here without care.  For new process invocations that do
// not need DetectError propagation, prefer mcpp::process::run_capture from
// the mcpp.process module.

module;
#include <cstdlib>     // getenv

export module mcpp.toolchain.probe;

import std;
import mcpp.toolchain.model;
import mcpp.xlings;
import mcpp.platform;
import mcpp.log;
import mcpp.fallback.sysroot_complete;
import mcpp.fallback.probe_sysroot;

export namespace mcpp::toolchain {

std::expected<std::string, DetectError> run_capture(const std::string& cmd);

std::string extract_version(std::string_view s);
std::string first_line_of(std::string_view s);
std::string lower_copy(std::string_view s);
std::string trim_line(std::string s);
std::string normalize_driver_output(std::string_view s);

std::vector<std::filesystem::path>
discover_compiler_runtime_dirs(const std::filesystem::path& compilerBin);

std::vector<std::filesystem::path>
discover_link_runtime_dirs(const std::filesystem::path& compilerBin,
                           std::string_view targetTriple);

std::string compiler_env_prefix(const Toolchain& tc);

std::expected<std::filesystem::path, DetectError>
probe_compiler_binary(const std::filesystem::path& explicit_compiler = {});

std::expected<std::string, DetectError>
probe_target_triple(const std::filesystem::path& compilerBin,
                    const std::string& envPrefix);

std::filesystem::path
probe_sysroot(const std::filesystem::path& compilerBin,
              const std::string& envPrefix);

// Probe fine-grained sysroot paths from sibling xpkgs payloads.
// Returns populated PayloadPaths if glibc xpkg found; linux-headers
// may be empty if not available.
std::optional<PayloadPaths>
probe_payload_paths(const std::filesystem::path& compilerBin);

// Ensure sysroot directory has complete headers by symlinking from
// payload xpkgs. Called when GCC's probed sysroot exists but may
// be missing linux kernel headers or glibc headers.
void ensure_sysroot_complete(const std::filesystem::path& sysroot,
                             const PayloadPaths& pp);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

namespace {

void append_existing_unique(std::vector<std::filesystem::path>& out,
                            const std::filesystem::path& p) {
    std::error_code ec;
    if (p.empty() || !std::filesystem::exists(p, ec)) return;
    auto abs = std::filesystem::absolute(p, ec);
    if (ec) abs = p;
    if (std::find(out.begin(), out.end(), abs) == out.end())
        out.push_back(abs);
}

std::string join_colon_paths(const std::vector<std::filesystem::path>& dirs) {
    std::string joined;
    for (auto& d : dirs) {
        if (!joined.empty()) joined += ':';
        joined += d.string();
    }
    return joined;
}

std::string env_prefix_for_dirs(const std::vector<std::filesystem::path>& dirs) {
    return mcpp::platform::linux_::build_clean_ld_library_path_prefix(dirs);
}

} // namespace

std::expected<std::string, DetectError> run_capture(const std::string& cmd) {
    auto r = mcpp::platform::process::capture_host_tool(cmd);
    if (r.exit_code != 0 && r.output.empty()) {
        return std::unexpected(DetectError{std::format("failed to execute: {}", cmd)});
    }
    if (r.exit_code != 0) {
        return std::unexpected(DetectError{
            std::format("'{}' exited with status {}", cmd, r.exit_code)});
    }
    return r.output;
}

std::string extract_version(std::string_view s) {
    std::string out;
    bool seen_digit = false;
    int dots = 0;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            out.push_back(c);
            seen_digit = true;
        } else if (c == '.' && seen_digit && dots < 2) {
            out.push_back('.');
            ++dots;
        } else if (seen_digit) {
            break;
        }
    }
    return out;
}

std::string first_line_of(std::string_view s) {
    auto end = s.find('\n');
    return std::string(s.substr(0, end));
}

std::string lower_copy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string trim_line(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    while (!s.empty() && (s.front() == '\n' || s.front() == '\r' || s.front() == ' '))
        s.erase(s.begin());
    return s;
}

std::string normalize_driver_output(std::string_view s) {
    auto replace_local_paths = [](std::string line) {
        static constexpr std::array<std::string_view, 3> prefixes{
            "/home/", "/tmp/", "/var/"
        };
        for (auto prefix : prefixes) {
            std::size_t pos = 0;
            while ((pos = line.find(prefix, pos)) != std::string::npos) {
                auto end = pos;
                while (end < line.size()) {
                    unsigned char c = static_cast<unsigned char>(line[end]);
                    if (std::isspace(c) || line[end] == '\'' || line[end] == '"')
                        break;
                    ++end;
                }
                line.replace(pos, end - pos, "<PATH>");
                pos += std::string_view("<PATH>").size();
            }
        }
        return line;
    };

    std::string out;
    std::istringstream is(std::string{s});
    std::string line;
    while (std::getline(is, line)) {
        line = trim_line(std::move(line));
        if (line.empty()) continue;
        if (line.starts_with("PWD=")) continue;
        line = replace_local_paths(std::move(line));
        if (!out.empty()) out.push_back('\n');
        out += line;
    }
    return out;
}

std::vector<std::filesystem::path>
discover_compiler_runtime_dirs(const std::filesystem::path& compilerBin) {
    std::vector<std::filesystem::path> dirs;
    auto root = compilerBin.parent_path().parent_path();

    auto rootStr = root.string();
    auto exe = compilerBin.filename().string();
    bool looksLikeLlvm = rootStr.find("xim-x-llvm") != std::string::npos
                      || exe.find("clang") != std::string::npos;
    if (looksLikeLlvm) {
        append_existing_unique(dirs, root / "lib");
        for (auto& d : mcpp::platform::linux_::runtime_lib_dirs(root))
            append_existing_unique(dirs, d);
        for (auto& d : mcpp::platform::macos::runtime_lib_dirs(root))
            append_existing_unique(dirs, d);
    }

    if (auto rt = mcpp::xlings::paths::find_sibling_tool(compilerBin, "gcc-runtime")) {
        append_existing_unique(dirs, *rt / "lib64");
        append_existing_unique(dirs, *rt / "lib");
    }
    return dirs;
}

std::vector<std::filesystem::path>
discover_link_runtime_dirs(const std::filesystem::path& compilerBin,
                           std::string_view targetTriple) {
    std::vector<std::filesystem::path> dirs;
    auto root = compilerBin.parent_path().parent_path();
    if (!targetTriple.empty())
        append_existing_unique(dirs, root / "lib" / std::string(targetTriple));
    for (auto& d : mcpp::platform::linux_::runtime_lib_dirs(root))
        append_existing_unique(dirs, d);
    for (auto& d : mcpp::platform::macos::runtime_lib_dirs(root))
        append_existing_unique(dirs, d);
    append_existing_unique(dirs, root / "lib");

    if constexpr (mcpp::platform::is_linux) {
        if (auto rt = mcpp::xlings::paths::find_sibling_tool(compilerBin, "gcc-runtime")) {
            append_existing_unique(dirs, *rt / "lib64");
            append_existing_unique(dirs, *rt / "lib");
        }
    }
    return dirs;
}

std::string compiler_env_prefix(const Toolchain& tc) {
    return env_prefix_for_dirs(tc.compilerRuntimeDirs);
}

std::expected<std::filesystem::path, DetectError>
probe_compiler_binary(const std::filesystem::path& explicit_compiler) {
    if (!explicit_compiler.empty()) {
        mcpp::log::verbose("probe", std::format("explicit compiler: {}", explicit_compiler.string()));
        if (!std::filesystem::exists(explicit_compiler)) {
            return std::unexpected(DetectError{std::format(
                "explicit compiler path does not exist: {}",
                explicit_compiler.string())});
        }
        return explicit_compiler;
    }

    std::string cxx;
    if (auto* e = std::getenv("CXX"); e && *e) {
        cxx = e;
    } else {
        cxx = "g++";
    }

    auto found = mcpp::platform::fs::which(cxx);
    if (!found) {
        return std::unexpected(DetectError{std::format("compiler '{}' not found in PATH", cxx)});
    }
    mcpp::log::verbose("probe", std::format("resolved compiler: {} → {}", cxx, found->string()));
    return *found;
}

std::expected<std::string, DetectError>
probe_target_triple(const std::filesystem::path& compilerBin,
                    const std::string& envPrefix) {
    auto triple_r = run_capture(std::format("{}{} -dumpmachine {}",
                                            envPrefix,
                                            mcpp::xlings::shq(compilerBin.string()),
                                            mcpp::platform::null_redirect));
    if (!triple_r) return std::unexpected(triple_r.error());
    return trim_line(*triple_r);
}

std::filesystem::path
probe_sysroot(const std::filesystem::path& compilerBin,
              const std::string& envPrefix) {
    // A sysroot is only usable if it actually carries the C library headers.
    // A merely-existing directory (e.g. a partially-bootstrapped sandbox
    // subos) would silently shadow the payload -isystem fallback and produce
    // "stdlib.h: No such file or directory" deep inside the std module build.
    auto usable = [](const std::filesystem::path& root) {
        return std::filesystem::exists(root / "usr" / "include" / "stdlib.h")   // glibc layout
            || std::filesystem::exists(root / "include" / "stdlib.h");          // musl layout
    };

    // 1. Ask the compiler directly (works for GCC; Clang often doesn't support it).
    auto r = run_capture(std::format("{}{} -print-sysroot {}",
                                     envPrefix,
                                     mcpp::xlings::shq(compilerBin.string()),
                                     mcpp::platform::null_redirect));
    if (r) {
        auto s = trim_line(*r);
        if (!s.empty() && std::filesystem::exists(s)) {
            if (usable(s)) return s;
            mcpp::log::debug("probe", std::format(
                "sysroot '{}' exists but lacks usr/include/stdlib.h — ignoring", s));
        }

        // GCC bakes the build-time sysroot into the binary. For xlings-built
        // GCC this is a path like <buildhost>/.xlings/subos/default that
        // doesn't exist on the user's machine. Remap via fallback module.
        if (auto remapped = mcpp::fallback::remap_xlings_baked_sysroot(s, compilerBin)) {
            if (usable(*remapped)) return *remapped;
            mcpp::log::debug("probe", std::format(
                "remapped sysroot '{}' lacks usr/include/stdlib.h — ignoring",
                remapped->string()));
        }
    }

    // 2. macOS fallback: use xcrun to discover the SDK path.
    //
    // NOTE: mcpp used to also mine the Clang driver cfg for --sysroot here.
    // That trust was dead code walking: the cfg is an install-time-generated
    // artifact that mcpp's own fixup pipeline now REGENERATES without a
    // --sysroot line (the C library comes from the payload link model), so
    // the mined value existed only on never-fixed-up installs and pointed at
    // an environment directory the payload doesn't own. The cfg is for
    // humans running clang++ directly; builds derive everything from the
    // link model. Kept as a diagnostic only.
    if (auto cfg = mcpp::fallback::parse_clang_cfg_sysroot(compilerBin)) {
        mcpp::log::debug("probe", std::format(
            "clang cfg declares sysroot '{}' — ignored (payload-first model)",
            cfg->string()));
    }
    if (auto sdk = mcpp::fallback::probe_macos_sdk_sysroot())
        return *sdk;

    mcpp::log::debug("probe", "no sysroot found");
    return {};
}

std::optional<PayloadPaths>
probe_payload_paths(const std::filesystem::path& compilerBin) {
    namespace paths = mcpp::xlings::paths;

    // Find glibc xpkg (required). Compiler siblings first; fall back to the
    // ACTIVE home registry — an inherited/symlinked compiler resolves into
    // its owner home, while the active home may own (or have just installed)
    // the sysroot payloads.
    auto glibc = paths::find_sibling_tool(compilerBin, "glibc");
    if (!glibc) glibc = paths::find_home_tool("glibc", "include/features.h");
    if (!glibc) return std::nullopt;

    // Glibc layout: <root>/include/ + <root>/lib64/ (or lib/).
    auto glibcInclude = *glibc / "include";
    if (!std::filesystem::exists(glibcInclude / "features.h"))
        return std::nullopt;

    auto glibcLib = *glibc / "lib64";
    if (!std::filesystem::exists(glibcLib))
        glibcLib = *glibc / "lib";
    if (!std::filesystem::exists(glibcLib))
        return std::nullopt;

    PayloadPaths pp;
    pp.glibcInclude = glibcInclude;
    pp.glibcLib     = glibcLib;

    // Find linux kernel headers (optional — search across index prefixes,
    // then the active home registry). Require the actual payload: a
    // delegating index package (xim:linux-headers → scode:linux-headers)
    // leaves a metadata-only husk under its own prefix, and the discovery
    // must skip it instead of giving up (issue #120: glibc's local_lim.h
    // needs <linux/limits.h>, so a silent miss breaks every glibc build).
    constexpr std::string_view kLinuxLimits = "include/linux/limits.h";
    auto linuxHeaders =
        paths::find_sibling_package(compilerBin, "linux-headers", kLinuxLimits);
    if (!linuxHeaders)
        linuxHeaders = paths::find_home_tool("linux-headers", kLinuxLimits);
    if (linuxHeaders) {
        pp.linuxInclude = *linuxHeaders / "include";
    } else {
        mcpp::log::verbose("probe",
            "linux-headers payload not found under any index prefix — "
            "glibc builds will fail at <linux/limits.h>");
    }

    mcpp::log::verbose("probe", std::format(
        "payload paths: glibcLib='{}' linuxInclude='{}'",
        pp.glibcLib.string(),
        pp.linuxInclude.empty() ? "(none)" : pp.linuxInclude.string()));
    return pp;
}

void ensure_sysroot_complete(const std::filesystem::path& sysroot,
                             const PayloadPaths& pp) {
    mcpp::fallback::ensure_sysroot_complete(sysroot, pp);
}

} // namespace mcpp::toolchain
