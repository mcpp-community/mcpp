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

// Resolve the payload paths for an EXPLICIT runtime binding ("glibc@2.39").
//
// The version is named by the caller's authority, never searched for. An
// empty binding returns nullopt: declining PayloadFirst is correct, guessing
// is what split the compile and run halves apart (see §3.2 of
// 2026-08-08-payload-version-and-contract-drift-design.md).
std::optional<PayloadPaths>
probe_payload_paths(const std::filesystem::path& compilerBin,
                    std::string_view runtimeBinding);

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

        // A usable sysroot that belongs to somebody else is still somebody
        // else's. gcc records this path as a string when it is built and
        // reports it forever after; on a machine with several checkouts the
        // recorded one routinely exists AND carries headers, so accepting it
        // on usability alone hands the build another project's tree. Measured
        // right here: this repo's gcc reported a sysroot under an unrelated
        // one, and every build took its headers.
        //
        // The ownership test lives inside remap_xlings_baked_sysroot, which is
        // exactly why the order matters -- the early return below reached it
        // only when the path was missing, so the case the remap exists for was
        // the one case it never saw.
        const bool ownedByThisHome =
            !s.empty() && mcpp::fallback::sysroot_is_owned(s, compilerBin);

        if (ownedByThisHome && usable(s)) return s;
        if (!s.empty() && std::filesystem::exists(s) && !usable(s))
            mcpp::log::debug("probe", std::format(
                "sysroot '{}' exists but lacks usr/include/stdlib.h — ignoring", s));

        // GCC bakes the build-time sysroot into the binary. For xlings-built
        // GCC this is a path like <buildhost>/.xlings/subos/default that
        // doesn't exist on the user's machine -- or exists and belongs to a
        // different one. Remap via fallback module.
        if (auto remapped = mcpp::fallback::remap_xlings_baked_sysroot(s, compilerBin)) {
            if (usable(*remapped)) return *remapped;
            mcpp::log::debug("probe", std::format(
                "remapped sysroot '{}' lacks usr/include/stdlib.h — ignoring",
                remapped->string()));
        }

        // Last resort: a foreign but usable sysroot beats no sysroot. This is
        // the pre-existing behaviour, kept for machines that have no registry
        // subos to remap to -- it is a worse answer, not a wrong one, and
        // taking nothing here would break them outright.
        if (!s.empty() && std::filesystem::exists(s) && usable(s)) {
            mcpp::log::verbose("probe", std::format(
                "using sysroot '{}', which is outside this toolchain's "
                "registry — no equivalent found under it", s));
            return std::filesystem::path(s);
        }
    }

    // 2. macOS fallback: use xcrun to discover the SDK path.
    //
    // NOTE: mcpp used to also mine the Clang driver cfg for --sysroot here.
    // That trust was dead code walking: the cfg is an install-time-generated
    // artifact that mcpp's own fixup pipeline now REGENERATES without a
    // --sysroot line (the C library comes from the payload link model), so
    // the mined value existed only on never-fixed-up installs and pointed at
    // an environment directory the payload doesn't own. The cfg serves
    // direct driver invocations only; builds derive everything from the
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

// Resolve `<name>@<version>` to the payload ROOT for that exact version.
//
// The version is named, never searched: the caller has an authority (the
// resolved runtime binding) and this turns it into an address. Two payload
// roots are tried in the order the rest of the file uses -- the compiler's own
// siblings first, then the active home -- because an inherited or symlinked
// compiler resolves into its owner home while the active home may be the one
// that just installed the payload.
std::optional<std::filesystem::path>
payload_root_for_binding(const std::filesystem::path& compilerBin,
                         std::string_view binding) {
    const auto at = binding.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= binding.size())
        return std::nullopt;
    const auto name    = binding.substr(0, at);
    const auto version = std::string(binding.substr(at + 1));

    std::error_code ec;
    // Compiler siblings: <...>/xpkgs/xim-x-<name>/<version>
    if (auto xpkgs = mcpp::xlings::paths::xpkgs_from_compiler(compilerBin)) {
        auto root = *xpkgs / std::format("xim-x-{}", name) / version;
        if (std::filesystem::exists(root, ec)) return root;
    }
    // Active home.
    if (auto xpkgs = mcpp::xlings::paths::active_home_xpkgs()) {
        auto root = *xpkgs / std::format("xim-x-{}", name) / version;
        if (std::filesystem::exists(root, ec)) return root;
    }
    return std::nullopt;
}

// `runtimeBinding` is the AUTHORITY -- `--runtime`, else the active subos's
// `subos_info.runtime`. Empty is a refusal, not a licence to guess: a guess
// here is how the compile side and the artifact's interpreter came to name
// different glibc versions, and nothing about the resulting binary looks
// wrong until it loads a library built against the other one.
std::optional<PayloadPaths>
probe_payload_paths(const std::filesystem::path& compilerBin,
                    std::string_view runtimeBinding) {
    if (runtimeBinding.empty()) {
        mcpp::log::verbose("probe",
            "no runtime binding resolved — declining PayloadFirst rather than "
            "picking a libc by directory order");
        return std::nullopt;
    }
    // Only a libc family has a payload of this shape. `macos_sdk`/`ucrt` are
    // resolved elsewhere, and a musl target uses the sysroot mode.
    if (!runtimeBinding.starts_with("glibc@")) {
        mcpp::log::verbose("probe", std::format(
            "runtime '{}' is not a glibc payload — no PayloadFirst",
            std::string(runtimeBinding)));
        return std::nullopt;
    }

    auto glibc = payload_root_for_binding(compilerBin, runtimeBinding);
    if (!glibc) {
        mcpp::log::verbose("probe", std::format(
            "runtime '{}' is not installed in this home",
            std::string(runtimeBinding)));
        return std::nullopt;
    }

    namespace paths = mcpp::xlings::paths;

    // Layout WITHIN the chosen payload. This convention stays: it answers
    // "where inside", not "which one".
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
