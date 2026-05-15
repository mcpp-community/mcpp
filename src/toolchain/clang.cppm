// mcpp.toolchain.clang - Clang/libc++ compiler behavior.

export module mcpp.toolchain.clang;

import std;
import mcpp.toolchain.model;
import mcpp.toolchain.probe;
import mcpp.xlings;

export namespace mcpp::toolchain::clang {

bool matches_version_output(std::string_view firstLineLower,
                            std::string_view fullOutputLower);

std::optional<std::filesystem::path> find_libcxx_std_module_source(
    const std::filesystem::path& cxx_binary,
    const std::string& envPrefix);

void enrich_toolchain(Toolchain& tc, const std::string& envPrefix);

std::filesystem::path std_bmi_path(const std::filesystem::path& cacheDir);
std::filesystem::path staged_std_bmi_path(const std::filesystem::path& outputDir);

std::vector<std::string> std_module_build_commands(const Toolchain& tc,
                                                   const std::filesystem::path& cacheDir,
                                                   const std::filesystem::path& bmiPath,
                                                   std::string_view sysrootFlag);

std::filesystem::path archive_tool(const Toolchain& tc);

// Locate clang-scan-deps in the same bin/ directory as clang++.
std::optional<std::filesystem::path> find_scan_deps(const Toolchain& tc);

} // namespace mcpp::toolchain::clang

namespace mcpp::toolchain::clang {

namespace {

std::optional<std::string>
json_string_value_after(std::string_view body, std::size_t start, std::string_view key) {
    auto keyToken = std::string{"\""} + std::string(key) + "\"";
    auto keyPos = body.find(keyToken, start);
    if (keyPos == std::string_view::npos) return std::nullopt;

    auto colon = body.find(':', keyPos + keyToken.size());
    if (colon == std::string_view::npos) return std::nullopt;

    auto quote = body.find('"', colon + 1);
    if (quote == std::string_view::npos) return std::nullopt;

    std::string out;
    for (std::size_t i = quote + 1; i < body.size(); ++i) {
        char c = body[i];
        if (c == '"') return out;
        if (c == '\\' && i + 1 < body.size()) {
            out.push_back(body[++i]);
        } else {
            out.push_back(c);
        }
    }
    return std::nullopt;
}

} // namespace

bool matches_version_output(std::string_view firstLineLower,
                            std::string_view fullOutputLower) {
    return firstLineLower.find("clang version") != std::string::npos
        || firstLineLower.find("apple clang version") != std::string::npos
        || fullOutputLower.find("clang") != std::string::npos;
}

std::optional<std::filesystem::path> find_libcxx_std_module_source(
    const std::filesystem::path& cxx_binary,
    const std::string& envPrefix)
{
    auto manifest_r = mcpp::toolchain::run_capture(std::format(
        "{}{} -print-library-module-manifest-path 2>/dev/null",
        envPrefix,
        mcpp::xlings::shq(cxx_binary.string())));
    if (manifest_r) {
        auto manifestPath = std::filesystem::path(
            mcpp::toolchain::trim_line(*manifest_r));
        if (!manifestPath.empty() && std::filesystem::exists(manifestPath)) {
            std::ifstream is(manifestPath);
            std::stringstream ss;
            ss << is.rdbuf();
            auto body = ss.str();

            std::size_t cursor = 0;
            while (true) {
                auto logical = body.find("\"logical-name\"", cursor);
                if (logical == std::string::npos) break;
                auto name = json_string_value_after(body, logical, "logical-name");
                if (name && *name == "std") {
                    auto src = json_string_value_after(body, logical, "source-path");
                    if (src) {
                        std::filesystem::path p = *src;
                        if (p.is_relative())
                            p = manifestPath.parent_path() / p;
                        std::error_code ec;
                        auto canon = std::filesystem::weakly_canonical(p, ec);
                        if (!ec) p = canon;
                        if (std::filesystem::exists(p)) return p;
                    }
                }
                cursor = logical + 1;
            }
        }
    }

    auto root = cxx_binary.parent_path().parent_path();
    auto fallback = root / "share" / "libc++" / "v1" / "std.cppm";
    if (std::filesystem::exists(fallback)) return fallback;
    return std::nullopt;
}

void enrich_toolchain(Toolchain& tc, const std::string& envPrefix) {
    tc.stdlibId      = "libc++";
    tc.stdlibVersion = tc.version.empty() ? "unknown" : tc.version;
    tc.linkRuntimeDirs = mcpp::toolchain::discover_link_runtime_dirs(
        tc.binaryPath, tc.targetTriple);
    if (auto p = find_libcxx_std_module_source(tc.binaryPath, envPrefix)) {
        tc.stdModuleSource = *p;
        tc.hasImportStd    = true;
    }
}

std::filesystem::path std_bmi_path(const std::filesystem::path& cacheDir) {
    return cacheDir / "pcm.cache" / "std.pcm";
}

std::filesystem::path staged_std_bmi_path(const std::filesystem::path& outputDir) {
    return outputDir / "pcm.cache" / "std.pcm";
}

std::vector<std::string> std_module_build_commands(const Toolchain& tc,
                                                   const std::filesystem::path& cacheDir,
                                                   const std::filesystem::path& bmiPath,
                                                   std::string_view sysrootFlag) {
    auto relBmi = std::filesystem::relative(bmiPath, cacheDir).string();
    return {
        std::format(
            "cd {} && {}{} -std=c++23 -Wno-reserved-module-identifier{} "
            "--precompile {} -o {} 2>&1",
            mcpp::xlings::shq(cacheDir.string()),
            mcpp::toolchain::compiler_env_prefix(tc),
            mcpp::xlings::shq(tc.binaryPath.string()),
            sysrootFlag,
            mcpp::xlings::shq(tc.stdModuleSource.string()),
            mcpp::xlings::shq(relBmi)),
        std::format(
            "cd {} && {}{} -std=c++23 -Wno-reserved-module-identifier{} "
            "{} -c -o std.o 2>&1",
            mcpp::xlings::shq(cacheDir.string()),
            mcpp::toolchain::compiler_env_prefix(tc),
            mcpp::xlings::shq(tc.binaryPath.string()),
            sysrootFlag,
            mcpp::xlings::shq(relBmi))
    };
}

std::filesystem::path archive_tool(const Toolchain& tc) {
    auto llvmAr = tc.binaryPath.parent_path() / "llvm-ar";
    if (std::filesystem::exists(llvmAr)) return llvmAr;
    return {};
}

std::optional<std::filesystem::path> find_scan_deps(const Toolchain& tc) {
    auto p = tc.binaryPath.parent_path() / "clang-scan-deps";
    if (std::filesystem::exists(p)) return p;
    return std::nullopt;
}

} // namespace mcpp::toolchain::clang
