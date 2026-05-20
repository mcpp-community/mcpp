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
    return mcpp::platform::linux_::build_ld_library_path_prefix(dirs);
}

} // namespace

std::expected<std::string, DetectError> run_capture(const std::string& cmd) {
    auto r = mcpp::platform::process::capture(cmd);
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
    auto r = run_capture(std::format("{}{} -print-sysroot {}",
                                     envPrefix,
                                     mcpp::xlings::shq(compilerBin.string()),
                                     mcpp::platform::null_redirect));
    if (r) {
        auto s = trim_line(*r);
        if (!s.empty() && std::filesystem::exists(s)) return s;
    }
    // macOS fallback: use xcrun to discover the SDK path.
    // The sysroot is used for regular compilation flags (flags.cppm) but
    // skipped for std module precompilation on macOS (stdmod.cppm) to
    // avoid breaking SDK internal header dependencies.
    if (auto sdk = mcpp::platform::macos::sdk_path())
        return *sdk;
    return {};
}

} // namespace mcpp::toolchain
