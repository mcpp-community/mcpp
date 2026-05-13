#include <gtest/gtest.h>

import std;
import mcpp.toolchain.detect;

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
