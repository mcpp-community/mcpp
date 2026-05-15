#include <gtest/gtest.h>

import std;
import mcpp.toolchain.detect;
import mcpp.toolchain.probe;

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

} // namespace

TEST(ToolchainDetect, ClangVersionOutputIsNotMisclassifiedByGccPaths) {
    auto clang = make_fake_clang();
    TempDirGuard cleanup{clang.parent_path()};

    auto tc = detect(clang);
    ASSERT_TRUE(tc.has_value()) << tc.error().message;
    EXPECT_EQ(tc->compiler, CompilerId::Clang);
    EXPECT_EQ(tc->version, "20.1.7");
    EXPECT_EQ(tc->targetTriple, "x86_64-unknown-linux-gnu");
    EXPECT_EQ(tc->stdlibId, "libc++");
    EXPECT_FALSE(tc->hasImportStd);
}

// ─── normalize_driver_output: path-free semantic identity ─────────────
//
// Background: the toolchain fingerprint used to hash the compiler binary
// content (hash_file). When the same xim-x-gcc package is installed under
// two different prefixes (~/.mcpp/... vs ~/.xlings/.../xim-x-mcpp/...),
// the on-disk binaries can have different MD5s (build metadata, strip,
// etc.) yet behave identically. We now use a normalized `--version`
// string as the path-free identity instead.

TEST(NormalizeDriverOutput, TrimsWhitespaceAndCollapsesBlankLines) {
    std::string raw =
        "  g++ (xim-x-gcc 16.1.0) 16.1.0\n"
        "\n"
        "Copyright (C) 2023 Free Software Foundation, Inc.   \n"
        "\n\n"
        "This is free software; ...\n";
    auto out = normalize_driver_output(raw);
    EXPECT_EQ(out,
        "g++ (xim-x-gcc 16.1.0) 16.1.0\n"
        "Copyright (C) 2023 Free Software Foundation, Inc.\n"
        "This is free software; ...");
}

TEST(NormalizeDriverOutput, IsStableAcrossInstallPrefixes) {
    // Same gcc package, two different install locations on disk.
    // --version output is identical regardless of where the binary lives,
    // so normalized identity must be identical too.
    std::string from_a =
        "g++ (xim-x-gcc 16.1.0) 16.1.0\n"
        "Copyright (C) 2023 Free Software Foundation, Inc.\n";
    std::string from_b =
        "g++ (xim-x-gcc 16.1.0) 16.1.0\n"
        "Copyright (C) 2023 Free Software Foundation, Inc.\n";
    EXPECT_EQ(normalize_driver_output(from_a), normalize_driver_output(from_b));
}

TEST(NormalizeDriverOutput, ReplacesLocalInstallPaths) {
    std::string a =
        "clang version 20.1.7\n"
        "Configuration file: /home/speak/.mcpp/registry/data/xpkgs/llvm/bin/clang.cfg\n";
    std::string b =
        "clang version 20.1.7\n"
        "Configuration file: /home/speak/.xlings/data/xpkgs/llvm/bin/clang.cfg\n";

    EXPECT_EQ(normalize_driver_output(a), normalize_driver_output(b));
    EXPECT_EQ(normalize_driver_output(a).find("/home/"), std::string::npos);
}

TEST(NormalizeDriverOutput, DistinguishesDifferentVersions) {
    std::string a = "g++ (xim-x-gcc 16.1.0) 16.1.0\n";
    std::string b = "g++ (xim-x-gcc 15.1.0) 15.1.0\n";
    EXPECT_NE(normalize_driver_output(a), normalize_driver_output(b));
}

TEST(NormalizeDriverOutput, EmptyInputProducesEmpty) {
    EXPECT_EQ(normalize_driver_output(""), "");
    EXPECT_EQ(normalize_driver_output("\n\n\n"), "");
}

// ─── detect() populates driverIdent ─────────────────────────────────
TEST(ToolchainDetect, PopulatesDriverIdentFromVersionOutput) {
    auto clang = make_fake_clang();
    TempDirGuard cleanup{clang.parent_path()};

    auto tc = detect(clang);
    ASSERT_TRUE(tc.has_value()) << tc.error().message;
    EXPECT_FALSE(tc->driverIdent.empty())
        << "detect() should populate Toolchain::driverIdent from --version output";
    EXPECT_NE(tc->driverIdent.find("clang version 20.1.7"), std::string::npos)
        << "driverIdent should contain the --version header: " << tc->driverIdent;
}
