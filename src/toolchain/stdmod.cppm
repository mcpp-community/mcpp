module;
#include <cstdlib>    // getenv

// mcpp.toolchain.stdmod — pre-build the `import std` BMI and cache it.
//
// GCC 15 flow (from docs/11-gcc15-cookbook.md §2):
//   g++ -std=c++23 -fmodules -Og -c <std.cc> -o std.o
//     ⇒ produces gcm.cache/std.gcm + std.o
//
// Clang/libc++ flow:
//   clang++ -std=c++23 --precompile <std.cppm> -o pcm.cache/std.pcm
//   clang++ -std=c++23 pcm.cache/std.pcm -c -o std.o
//
// We invoke the compiler in a dedicated cache directory so the produced
// BMI is owned by mcpp and reused across all builds with the same fingerprint.
//
// Output layout:
//   <cache_root>/<fingerprint>/
//      gcm.cache/std.gcm        ← GCC BMI
//      pcm.cache/std.pcm        ← Clang BMI
//      std.o                    ← linked into final binaries

export module mcpp.toolchain.stdmod;

import std;
import mcpp.libs.json;
import mcpp.platform;
import mcpp.toolchain.clang;
import mcpp.toolchain.detect;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.gcc;

export namespace mcpp::toolchain {

struct StdModule {
    std::filesystem::path           cacheDir;            // <cache_root>/<fp>/
    std::filesystem::path           bmiPath;             // <cacheDir>/gcm.cache/std.gcm
    std::filesystem::path           objectPath;          // <cacheDir>/std.o
    std::filesystem::path           compatBmiPath;       // <cacheDir>/pcm.cache/std.compat.pcm
    std::filesystem::path           compatObjectPath;    // <cacheDir>/std.compat.o
};

struct StdModError { std::string message; };

std::filesystem::path default_cache_root();

// Build std module if not already cached. Returns paths to BMI + object.
// `macos_deployment_target` is the RESOLVED value from
// platform::macos::deployment_target() — it must match what flags.cppm
// emits for normal TUs, or the produced std.pcm targets a different
// arm64-apple-macosxNN triple than the code importing it.
std::expected<StdModule, StdModError> ensure_built(
    const Toolchain&                  tc,
    std::string_view                  fingerprint_hex,
    std::string_view                  cpp_standard,
    std::string_view                  cpp_standard_flag,
    std::string_view                  macos_deployment_target = {},
    const std::filesystem::path&      cache_root = default_cache_root());

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

namespace {

std::expected<std::string, StdModError> run_capture_command(const std::string& cmd) {
    auto r = mcpp::platform::process::capture(cmd);
    if (r.exit_code != 0) {
        // Include the command: its --sysroot/-isystem flags are the first
        // thing needed to diagnose header-resolution failures.
        return std::unexpected(StdModError{
            std::format("std module precompile failed (rc={}):\n{}\ncommand: {}",
                        r.exit_code, r.output, cmd)});
    }
    return r.output;
}

std::filesystem::path metadata_path(const std::filesystem::path& cacheDir) {
    return cacheDir / "std-module.json";
}

nlohmann::json metadata_for(const Toolchain& tc,
                            std::string_view cppStandard,
                            std::string_view cppStandardFlag,
                            const std::vector<std::string>& stdCommands,
                            const std::vector<std::string>& compatCommands) {
    nlohmann::json j;
    j["schema"] = 1;
    j["compiler"] = std::string(tc.compiler_name());
    j["compiler_version"] = tc.version;
    j["driver_identity"] = tc.driverIdent.empty()
        ? (tc.binaryPath.empty() ? "" : hash_file(tc.binaryPath))
        : hash_string(tc.driverIdent);
    j["target_triple"] = tc.targetTriple;
    j["stdlib"] = tc.stdlibId;
    j["stdlib_version"] = tc.stdlibVersion;
    j["cpp_standard"] = std::string(cppStandard);
    j["std_flag"] = std::string(cppStandardFlag);
    j["std_module_source"] = tc.stdModuleSource.generic_string();
    j["std_module_source_hash"] = hash_file(tc.stdModuleSource);
    j["std_compat_source"] = tc.stdCompatSource.generic_string();
    j["std_compat_source_hash"] = tc.stdCompatSource.empty() ? "" : hash_file(tc.stdCompatSource);
    j["std_build_commands"] = stdCommands;
    j["std_compat_build_commands"] = compatCommands;
    return j;
}

bool metadata_matches(const std::filesystem::path& path, const nlohmann::json& expected) {
    std::ifstream is(path);
    if (!is) return false;
    nlohmann::json actual;
    try {
        is >> actual;
    } catch (...) {
        return false;
    }
    static constexpr std::array<std::string_view, 14> keys = {
        "schema",
        "compiler",
        "compiler_version",
        "driver_identity",
        "target_triple",
        "stdlib",
        "stdlib_version",
        "cpp_standard",
        "std_flag",
        "std_module_source",
        "std_module_source_hash",
        "std_compat_source",
        "std_compat_source_hash",
        "std_build_commands",
    };
    for (auto key : keys) {
        auto k = std::string(key);
        if (!actual.contains(k) || actual[k] != expected[k]) return false;
    }
    return actual.value("std_compat_build_commands", nlohmann::json::array())
        == expected["std_compat_build_commands"];
}

std::expected<void, StdModError> write_metadata(const std::filesystem::path& path,
                                                const nlohmann::json& metadata) {
    std::ofstream os(path, std::ios::binary);
    if (!os) {
        return std::unexpected(StdModError{
            std::format("cannot write std module metadata '{}'", path.string())});
    }
    os << metadata.dump(2) << "\n";
    if (!os) {
        return std::unexpected(StdModError{
            std::format("failed while writing std module metadata '{}'", path.string())});
    }
    return {};
}

std::expected<std::string, StdModError> run_commands(const std::vector<std::string>& commands) {
    std::string out;
    for (auto const& cmd : commands) {
        if (auto r = run_capture_command(cmd); !r) return std::unexpected(r.error());
        else out += *r;
    }
    return out;
}

} // namespace

std::filesystem::path default_cache_root() {
    if (auto* e = std::getenv("MCPP_HOME"); e && *e) {
        return std::filesystem::path(e) / "bmi";
    }
    if (auto* e = std::getenv("HOME"); e && *e) {
        return std::filesystem::path(e) / ".mcpp" / "bmi";
    }
    return std::filesystem::current_path() / ".mcpp-bmi";
}

std::expected<StdModule, StdModError> ensure_built(
    const Toolchain&                  tc,
    std::string_view                  fingerprint_hex,
    std::string_view                  cpp_standard,
    std::string_view                  cpp_standard_flag,
    std::string_view                  macos_deployment_target,
    const std::filesystem::path&      cache_root)
{
    if (tc.stdModuleSource.empty()) {
        return std::unexpected(StdModError{
            "toolchain has no std module source (import std unsupported on this compiler)"});
    }

    StdModule sm;
    sm.cacheDir   = cache_root / std::string(fingerprint_hex);
    sm.bmiPath    = is_clang(tc)
                  ? mcpp::toolchain::clang::std_bmi_path(sm.cacheDir)
                  : mcpp::toolchain::gcc::std_bmi_path(sm.cacheDir);
    sm.objectPath = sm.cacheDir / "std.o";

    // Build sysroot + include flags for std module precompilation.
    //
    // Payload-first: use fine-grained -isystem paths from xpkgs payloads
    // when available, falling back to --sysroot.
    //
    // For Clang with a cfg file: use --no-default-config to bypass
    // potentially-stale paths, then provide all flags explicitly.
    // Std module precompilation only needs compile flags (no linker flags),
    // so --no-default-config is safe here on all platforms.
    std::string sysroot_flag;
    if (is_clang(tc)) {
        auto cfgPath = tc.binaryPath.parent_path()
                       / (tc.binaryPath.stem().string() + ".cfg");
        if (std::filesystem::exists(cfgPath)) {
            auto llvmRoot = tc.binaryPath.parent_path().parent_path();
            auto libcxxInclude = llvmRoot / "include" / "c++" / "v1";
            sysroot_flag = " --no-default-config -nostdinc++ -stdlib=libc++";
            sysroot_flag += std::format(" -isystem'{}'", libcxxInclude.string());
            if (!tc.targetTriple.empty()) {
                auto targetInclude = llvmRoot / "include"
                                     / tc.targetTriple / "c++" / "v1";
                if (std::filesystem::exists(targetInclude))
                    sysroot_flag += std::format(" -isystem'{}'", targetInclude.string());
            }
            // C library + kernel headers from payload paths.
            if (tc.payloadPaths) {
                sysroot_flag += std::format(" -isystem'{}'", tc.payloadPaths->glibcInclude.string());
                if (!tc.payloadPaths->linuxInclude.empty())
                    sysroot_flag += std::format(" -isystem'{}'", tc.payloadPaths->linuxInclude.string());
            } else if (auto sdk = mcpp::platform::macos::sdk_path()) {
                sysroot_flag += std::format(" --sysroot='{}'", sdk->string());
            } else if (!tc.sysroot.empty()) {
                sysroot_flag += std::format(" --sysroot='{}'", tc.sysroot.string());
            }
        } else if (!tc.sysroot.empty()) {
            sysroot_flag = std::format(" --sysroot='{}'", tc.sysroot.string());
        }
    } else if (!tc.sysroot.empty()) {
        // GCC: use --sysroot (required for include-fixed headers).
        // Supplement with -isystem for linux kernel headers from payload
        // if the probed sysroot is missing them.
        sysroot_flag = std::format(" --sysroot='{}'", tc.sysroot.string());
        if (tc.payloadPaths && !tc.payloadPaths->linuxInclude.empty()) {
            auto sysrootLinux = tc.sysroot / "usr" / "include" / "linux" / "limits.h";
            if (!std::filesystem::exists(sysrootLinux))
                sysroot_flag += std::format(" -isystem'{}'", tc.payloadPaths->linuxInclude.string());
        }
    } else if (tc.payloadPaths) {
        // No usable sysroot: wire the C library headers from the payload.
        // GCC's libstdc++ wraps libc headers via #include_next, which only
        // searches directories AFTER the one the current header came from —
        // and gcc's built-in dirs are LAST in the search order, so an
        // -isystem payload dir (inserted before the built-ins) is unreachable
        // from #include_next. -idirafter appends the payload to the very end,
        // exactly where #include_next looks.
        const bool clang = is_clang(tc);
        auto add_inc = [&](const std::filesystem::path& p) {
            if (clang) sysroot_flag += std::format(" -isystem'{}'", p.string());
            else       sysroot_flag += std::format(" -idirafter'{}'", p.string());
        };
        add_inc(tc.payloadPaths->glibcInclude);
        if (!tc.payloadPaths->linuxInclude.empty())
            add_inc(tc.payloadPaths->linuxInclude);
    }

    // Deployment target must mirror what flags.cppm emits for normal TUs
    // (single resolver: platform::macos::deployment_target).
    if (!macos_deployment_target.empty()) {
        sysroot_flag += std::format(" -mmacosx-version-min={}",
                                    macos_deployment_target);
    }

    std::vector<std::string> stdCommands;
    if (is_clang(tc)) {
        stdCommands = mcpp::toolchain::clang::std_module_build_commands(
            tc, sm.cacheDir, sm.bmiPath, sysroot_flag, cpp_standard_flag);
    } else {
        stdCommands.push_back(mcpp::toolchain::gcc::std_module_build_command(
            tc, sm.cacheDir, sysroot_flag, cpp_standard_flag));
    }
    std::vector<std::string> compatCommands;
    if (is_clang(tc) && !tc.stdCompatSource.empty()) {
        auto compatBmi = mcpp::toolchain::clang::std_compat_bmi_path(sm.cacheDir);
        compatCommands = mcpp::toolchain::clang::std_compat_build_commands(
            tc, sm.cacheDir, compatBmi, sm.bmiPath, sysroot_flag, cpp_standard_flag);
    }
    auto metadata = metadata_for(tc, cpp_standard, cpp_standard_flag, stdCommands, compatCommands);
    auto metaPath = metadata_path(sm.cacheDir);
    bool std_cached = std::filesystem::exists(sm.bmiPath)
                   && std::filesystem::exists(sm.objectPath)
                   && metadata_matches(metaPath, metadata);
    bool rebuiltStd = false;

    if (!std_cached) {
        std::error_code ec;
        std::filesystem::create_directories(sm.bmiPath.parent_path(), ec);
        if (ec) return std::unexpected(StdModError{
            std::format("cannot create '{}': {}", sm.bmiPath.parent_path().string(), ec.message())});

        auto out = run_commands(stdCommands);
        if (!out) return std::unexpected(out.error());

        if (!std::filesystem::exists(sm.bmiPath)) {
            return std::unexpected(StdModError{
                std::format("expected BMI at '{}' but it wasn't produced; output:\n{}",
                            sm.bmiPath.string(), *out)});
        }
        rebuiltStd = true;
    }

    // Build std.compat after std (std.compat depends on std, Clang only).
    if (is_clang(tc) && !tc.stdCompatSource.empty()) {
        auto compatBmi = mcpp::toolchain::clang::std_compat_bmi_path(sm.cacheDir);
        if (rebuiltStd || !std::filesystem::exists(compatBmi)
            || !metadata_matches(metaPath, metadata)) {
            if (auto out = run_commands(compatCommands); !out) {
                return std::unexpected(out.error());
            }
        }
        sm.compatBmiPath = compatBmi;
        sm.compatObjectPath = sm.cacheDir / "std.compat.o";
    }

    if (auto r = write_metadata(metaPath, metadata); !r) {
        return std::unexpected(r.error());
    }

    return sm;
}

} // namespace mcpp::toolchain
