// mcpp.toolchain.detect — discover the C++ compiler and its capabilities

module;
#include <cstdio>      // popen, pclose, fgets, FILE
#include <cstdlib>     // getenv

export module mcpp.toolchain.detect;

import std;

export namespace mcpp::toolchain {

enum class CompilerId { Unknown, GCC, Clang, MSVC };

struct Toolchain {
    CompilerId                          compiler        = CompilerId::Unknown;
    std::string                         version;            // "15.1.0"
    std::filesystem::path               binaryPath;
    std::string                         targetTriple;      // "x86_64-linux-gnu"
    std::string                         stdlibId;          // "libstdc++"
    std::string                         stdlibVersion;
    std::filesystem::path               stdModuleSource;  // bits/std.cc
    std::filesystem::path               sysroot;           // M5.5: -print-sysroot output (or empty)
    bool                                hasImportStd = false;

    std::string label() const {
        return std::format("{} {} ({})", compiler_name(), version, targetTriple);
    }

    std::string_view compiler_name() const {
        switch (compiler) {
            case CompilerId::GCC:   return "gcc";
            case CompilerId::Clang: return "clang";
            case CompilerId::MSVC:  return "msvc";
            default:                return "unknown";
        }
    }
};

struct DetectError { std::string message; };

// Detect toolchain. If explicit_compiler is given, use that binary path
// directly (skipping auto-discovery). Otherwise fall back to $CXX env
// variable, and finally PATH (with implicit warning expected from caller).
std::expected<Toolchain, DetectError>
detect(const std::filesystem::path& explicit_compiler = {});

// Helper: find std module source for a given GCC binary
std::optional<std::filesystem::path> find_std_module_source(
    const std::filesystem::path& cxx_binary, std::string_view version);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

namespace {

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

// Match the leading version triple "X.Y.Z" out of a string.
std::string extract_version(std::string_view s) {
    std::string out;
    bool seen_digit = false;
    int dots = 0;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            out.push_back(c);
            seen_digit = true;
        } else if (c == '.' && seen_digit && dots < 2) {
            out.push_back('.'); ++dots;
        } else if (seen_digit) {
            break;
        }
    }
    return out;
}

} // namespace

std::optional<std::filesystem::path> find_std_module_source(
    const std::filesystem::path& cxx_binary, std::string_view version) {
    // Strategy: starting from CXX binary, derive GCC root and look for
    //   <root>/include/c++/<version>/bits/std.cc
    // Path layout (xlings GCC):
    //   ~/.xlings/data/xpkgs/xim-x-gcc/15.1.0/bin/g++
    //                                    /include/c++/15.1.0/bits/std.cc
    auto root = cxx_binary.parent_path().parent_path();
    auto p = root / "include" / "c++" / std::string(version) / "bits" / "std.cc";
    if (std::filesystem::exists(p)) return p;

    // Try: ask the compiler.
    auto cmd = std::format("'{}' -print-file-name=libstdc++.so 2>/dev/null", cxx_binary.string());
    auto r = run_capture(cmd);
    if (r) {
        std::string trimmed = *r;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) trimmed.pop_back();
        if (!trimmed.empty()) {
            std::filesystem::path libpath = trimmed;
            auto root2 = libpath.parent_path().parent_path();
            auto p2 = root2 / "include" / "c++" / std::string(version) / "bits" / "std.cc";
            if (std::filesystem::exists(p2)) return p2;
        }
    }
    return std::nullopt;
}

std::expected<Toolchain, DetectError>
detect(const std::filesystem::path& explicit_compiler) {
    Toolchain tc;

    // Determine compiler binary:
    //   1. explicit_compiler argument (caller resolved via [toolchain] / sandbox subos)
    //   2. $CXX env
    //   3. g++ in PATH (last resort; caller should warn)
    std::string bin;
    if (!explicit_compiler.empty()) {
        if (!std::filesystem::exists(explicit_compiler)) {
            return std::unexpected(DetectError{std::format(
                "explicit compiler path does not exist: {}",
                explicit_compiler.string())});
        }
        bin = explicit_compiler.string();
    } else {
        std::string cxx;
        if (auto* e = std::getenv("CXX"); e && *e) {
            cxx = e;
        } else {
            cxx = "g++"; // M1: GCC only
        }
        auto bin_path_r = run_capture(std::format("command -v '{}' 2>/dev/null", cxx));
        if (!bin_path_r) {
            return std::unexpected(DetectError{std::format("compiler '{}' not found in PATH", cxx)});
        }
        bin = *bin_path_r;
        while (!bin.empty() && (bin.back() == '\n' || bin.back() == '\r')) bin.pop_back();
        if (bin.empty()) {
            return std::unexpected(DetectError{std::format("compiler '{}' not found", cxx)});
        }
    }
    tc.binaryPath = bin;

    // Version probe
    auto ver_r = run_capture(std::format("'{}' --version 2>&1", bin));
    if (!ver_r) return std::unexpected(ver_r.error());
    const std::string& vstr = *ver_r;

    if (vstr.find("g++") != std::string::npos || vstr.find("GCC") != std::string::npos
        || vstr.find("gcc") != std::string::npos)
    {
        tc.compiler = CompilerId::GCC;
        // Extract version after the parenthesized package label, if any.
        // E.g. "g++ (XPKG: ...) 15.1.0"
        auto first_line_end = vstr.find('\n');
        std::string head = vstr.substr(0, first_line_end);
        // Find rightmost number sequence on first line.
        auto rpos = head.find_last_of("0123456789");
        if (rpos != std::string::npos) {
            // Walk left to find start of "X.Y.Z"
            std::size_t lpos = rpos;
            while (lpos > 0 && (std::isdigit(static_cast<unsigned char>(head[lpos-1])) || head[lpos-1] == '.')) {
                --lpos;
            }
            tc.version = head.substr(lpos, rpos - lpos + 1);
        }
        if (tc.version.empty()) tc.version = extract_version(head);
    } else if (vstr.find("clang") != std::string::npos) {
        tc.compiler = CompilerId::Clang;
        tc.version  = extract_version(vstr);
    } else {
        return std::unexpected(DetectError{
            std::format("unrecognized compiler output:\n{}", vstr)});
    }

    // Target triple
    auto triple_r = run_capture(std::format("'{}' -dumpmachine 2>/dev/null", bin));
    if (triple_r) {
        std::string t = *triple_r;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        tc.targetTriple = t;
    }

    // Stdlib identification (GCC → libstdc++)
    if (tc.compiler == CompilerId::GCC) {
        tc.stdlibId      = "libstdc++";
        tc.stdlibVersion = tc.version; // libstdc++ ships with GCC; same version
    } else if (tc.compiler == CompilerId::Clang) {
        tc.stdlibId      = "libc++";
        tc.stdlibVersion = "unknown";
    }

    // std module source
    if (tc.compiler == CompilerId::GCC) {
        if (auto p = find_std_module_source(tc.binaryPath, tc.version)) {
            tc.stdModuleSource = *p;
            tc.hasImportStd    = true;
        }
    }

    // M5.5: probe sysroot — needed when bypassing xlings wrapper.
    {
        auto cmd = std::format("'{}' -print-sysroot 2>/dev/null", tc.binaryPath.string());
        auto r = run_capture(cmd);
        if (r) {
            std::string s = *r;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty() && std::filesystem::exists(s)) {
                tc.sysroot = s;
            }
        }
    }

    return tc;
}

} // namespace mcpp::toolchain
