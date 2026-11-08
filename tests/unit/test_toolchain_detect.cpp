
import std;
import mcpplibs.mtest;
import mcpp.invocation;
import mcpp.toolchain.detect;
import mcpp.toolchain.probe;

using namespace mcpplibs::mtest;

using namespace mcpp::toolchain;

namespace {

struct TempDirGuard {
    std::filesystem::path root;

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

std::filesystem::path make_fake_clang() {
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp_fake_clang_{}", std::random_device{}());
    std::filesystem::create_directories(dir);
    auto clang = dir / "clang++";

    std::ofstream os(clang);
    os << R"(#!/usr/bin/env bash
case "$1" in
  --version)
    cat <<'OUT'
clang version 20.1.7 (https://github.com/llvm/llvm-project 6146a88f60492b520a36f8f8f3231e15f3cc6082)
Target: x86_64-unknown-linux-gnu
Configuration file: /home/user/.mcpp/registry/data/xpkgs/xim-x-gcc-runtime/15.1.0/lib64
OUT
    ;;
  -dumpmachine)
    echo x86_64-unknown-linux-gnu
    ;;
  -print-sysroot)
    ;;
  *)
    exit 0
    ;;
esac
)";
    os.close();
    std::filesystem::permissions(
        clang,
        std::filesystem::perms::owner_exec
        | std::filesystem::perms::owner_read
        | std::filesystem::perms::owner_write);
    return clang;
}

std::filesystem::path make_hostile_ld_dir() {
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp_hostile_ld_{}", std::random_device{}());
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "libc.so.6").close();
    return dir;
}

} // namespace

#if !defined(_WIN32)
// Uses a fake shell script as a compiler — POSIX only.
UTest clang_version_output_is_not_misclassified_by_gcc_paths{"ToolchainDetect/ClangVersionOutputIsNotMisclassifiedByGccPaths", [] {
    auto clang = make_fake_clang();
    TempDirGuard cleanup{clang.parent_path()};

    auto tc = detect(clang);
    if (auto&& failed = expect(fatal(tc.has_value())); !failed) failed << tc.error().message;
    expect(eq(tc->compiler, CompilerId::Clang));
    expect(eq(tc->version, "20.1.7"));
    expect(eq(tc->targetTriple, "x86_64-unknown-linux-gnu"));
    expect(eq(tc->stdlibId, "libc++"));
    expect(!tc->hasImportStd);
}};
#endif // !defined(_WIN32)

#if defined(__linux__)
UTest ignores_target_runtime_library_path_during_probe{"ToolchainDetect/IgnoresTargetRuntimeLibraryPathDuringProbe", [] {
    auto clang = make_fake_clang();
    TempDirGuard cleanup_clang{clang.parent_path()};
    auto hostile = make_hostile_ld_dir();
    TempDirGuard cleanup_ld{hostile};

    mcpp::invocation::ScopedEnv ld("LD_LIBRARY_PATH", hostile.string());

    auto tc = detect(clang);
    if (auto&& failed = expect(fatal(tc.has_value())); !failed) failed << tc.error().message;
    expect(eq(tc->compiler, CompilerId::Clang));
    expect(eq(tc->targetTriple, "x86_64-unknown-linux-gnu"));
}};
#endif // defined(__linux__)

// ─── normalize_driver_output: path-free semantic identity ─────────────
//
// Background: the toolchain fingerprint used to hash the compiler binary
// content (hash_file). When the same xim-x-gcc package is installed under
// two different prefixes (~/.mcpp/... vs ~/.xlings/.../xim-x-mcpp/...),
// the on-disk binaries can have different MD5s (build metadata, strip,
// etc.) yet behave identically. We now use a normalized `--version`
// string as the path-free identity instead.

UTest trims_whitespace_and_collapses_blank_lines{"NormalizeDriverOutput/TrimsWhitespaceAndCollapsesBlankLines", [] {
    std::string raw =
        "  g++ (xim-x-gcc 16.1.0) 16.1.0\n"
        "\n"
        "Copyright (C) 2023 Free Software Foundation, Inc.   \n"
        "\n\n"
        "This is free software; ...\n";
    auto out = normalize_driver_output(raw);
    expect(eq(out, "g++ (xim-x-gcc 16.1.0) 16.1.0\n"
        "Copyright (C) 2023 Free Software Foundation, Inc.\n"
        "This is free software; ..."));
}};

UTest is_stable_across_install_prefixes{"NormalizeDriverOutput/IsStableAcrossInstallPrefixes", [] {
    // Same gcc package, two different install locations on disk.
    // --version output is identical regardless of where the binary lives,
    // so normalized identity must be identical too.
    std::string from_a =
        "g++ (xim-x-gcc 16.1.0) 16.1.0\n"
        "Copyright (C) 2023 Free Software Foundation, Inc.\n";
    std::string from_b =
        "g++ (xim-x-gcc 16.1.0) 16.1.0\n"
        "Copyright (C) 2023 Free Software Foundation, Inc.\n";
    expect(eq(normalize_driver_output(from_a), normalize_driver_output(from_b)));
}};

UTest replaces_local_install_paths{"NormalizeDriverOutput/ReplacesLocalInstallPaths", [] {
    std::string a =
        "clang version 20.1.7\n"
        "Configuration file: /home/speak/.mcpp/registry/data/xpkgs/llvm/bin/clang.cfg\n";
    std::string b =
        "clang version 20.1.7\n"
        "Configuration file: /home/speak/.xlings/data/xpkgs/llvm/bin/clang.cfg\n";

    expect(eq(normalize_driver_output(a), normalize_driver_output(b)));
    expect(eq(normalize_driver_output(a).find("/home/"), std::string::npos));
}};

UTest distinguishes_different_versions{"NormalizeDriverOutput/DistinguishesDifferentVersions", [] {
    std::string a = "g++ (xim-x-gcc 16.1.0) 16.1.0\n";
    std::string b = "g++ (xim-x-gcc 15.1.0) 15.1.0\n";
    expect(ne(normalize_driver_output(a), normalize_driver_output(b)));
}};

UTest empty_input_produces_empty{"NormalizeDriverOutput/EmptyInputProducesEmpty", [] {
    expect(eq(normalize_driver_output(""), ""));
    expect(eq(normalize_driver_output("\n\n\n"), ""));
}};

#if !defined(_WIN32)
// ─── detect() populates driverIdent ─────────────────────────────────
UTest populates_driver_ident_from_version_output{"ToolchainDetect/PopulatesDriverIdentFromVersionOutput", [] {
    auto clang = make_fake_clang();
    TempDirGuard cleanup{clang.parent_path()};

    auto tc = detect(clang);
    if (auto&& failed = expect(fatal(tc.has_value())); !failed) failed << tc.error().message;
    expect(!(tc->driverIdent.empty()))
        << "detect() should populate Toolchain::driverIdent from --version output";
    if (auto&& failed = expect(ne(tc->driverIdent.find("clang version 20.1.7"), std::string::npos)); !failed) failed << "driverIdent should contain the --version header: " << tc->driverIdent;
}};
#endif // !defined(_WIN32)
