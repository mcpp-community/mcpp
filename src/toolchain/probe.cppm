// mcpp.toolchain.probe - common compiler probing helpers.

module;
#include <cstdio>      // popen, pclose, fgets, FILE
#include <cstdlib>     // getenv

export module mcpp.toolchain.probe;

import std;
import mcpp.toolchain.model;
import mcpp.xlings;

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
    if (dirs.empty()) return "";
    auto joined = join_colon_paths(dirs);
    return std::format("env LD_LIBRARY_PATH={}${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}} ",
                       mcpp::xlings::shq(joined));
}

} // namespace

std::expected<std::string, DetectError> run_capture(const std::string& cmd) {
    std::array<char, 4096> buf{};
    std::string out;
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) {
        return std::unexpected(DetectError{std::format("failed to execute: {}", cmd)});
    }
    while (std::fgets(buf.data(), buf.size(), fp) != nullptr) out += buf.data();
    int rc = ::pclose(fp);
    if (rc != 0) {
        return std::unexpected(DetectError{
            std::format("'{}' exited with status {}", cmd, rc)});
    }
    return out;
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
#if defined(__linux__)
        append_existing_unique(dirs, root / "lib" / "x86_64-unknown-linux-gnu");
        append_existing_unique(dirs, "/lib/x86_64-linux-gnu");
        append_existing_unique(dirs, "/usr/lib/x86_64-linux-gnu");
        append_existing_unique(dirs, "/usr/lib64");
#elif defined(__APPLE__)
        append_existing_unique(dirs, root / "lib" / "aarch64-apple-darwin");
        append_existing_unique(dirs, root / "lib" / "darwin");
#endif
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
#if defined(__linux__)
    append_existing_unique(dirs, root / "lib" / "x86_64-unknown-linux-gnu");
#elif defined(__APPLE__)
    append_existing_unique(dirs, root / "lib" / "aarch64-apple-darwin");
    append_existing_unique(dirs, root / "lib" / "darwin");
#endif
    append_existing_unique(dirs, root / "lib");

#if defined(__linux__)
    if (auto rt = mcpp::xlings::paths::find_sibling_tool(compilerBin, "gcc-runtime")) {
        append_existing_unique(dirs, *rt / "lib64");
        append_existing_unique(dirs, *rt / "lib");
    }
#endif
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

    auto bin_path_r = run_capture(std::format("command -v '{}' 2>/dev/null", cxx));
    if (!bin_path_r) {
        return std::unexpected(DetectError{std::format("compiler '{}' not found in PATH", cxx)});
    }
    auto bin = trim_line(*bin_path_r);
    if (bin.empty()) {
        return std::unexpected(DetectError{std::format("compiler '{}' not found", cxx)});
    }
    return std::filesystem::path(bin);
}

std::expected<std::string, DetectError>
probe_target_triple(const std::filesystem::path& compilerBin,
                    const std::string& envPrefix) {
    auto triple_r = run_capture(std::format("{}{} -dumpmachine 2>/dev/null",
                                            envPrefix,
                                            mcpp::xlings::shq(compilerBin.string())));
    if (!triple_r) return std::unexpected(triple_r.error());
    return trim_line(*triple_r);
}

std::filesystem::path
probe_sysroot(const std::filesystem::path& compilerBin,
              const std::string& envPrefix) {
    auto r = run_capture(std::format("{}{} -print-sysroot 2>/dev/null",
                                     envPrefix,
                                     mcpp::xlings::shq(compilerBin.string())));
    if (r) {
        auto s = trim_line(*r);
        if (!s.empty() && std::filesystem::exists(s)) return s;
    }
#if defined(__APPLE__)
    // macOS fallback: use xcrun to discover the SDK path
    auto xcrun_r = run_capture("xcrun --show-sdk-path 2>/dev/null");
    if (xcrun_r) {
        auto sdk = trim_line(*xcrun_r);
        if (!sdk.empty() && std::filesystem::exists(sdk)) return sdk;
    }
#endif
    return {};
}

} // namespace mcpp::toolchain
