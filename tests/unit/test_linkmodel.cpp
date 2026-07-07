#include <gtest/gtest.h>

import std;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.model;

// The toolchain link model is the single resolver for "how do we compile and
// link against this toolchain's C library" (issue #195 / the hermetic link
// model design doc). These tests pin its three contracts:
//   1. loader names come from data (exports → triple map → glob), never a
//      hardcoded x86_64 string;
//   2. the payload link flags include -B (CRT discovery — the driver never
//      consults -L for Scrt1.o/crti.o/crtn.o);
//   3. mode selection mirrors the historical flags.cppm precedence.

namespace {

namespace tc = mcpp::toolchain;

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_linkmodel_test_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void touch(const std::filesystem::path& p) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}

std::string ident(const std::filesystem::path& p) { return p.string(); }

TEST(LoaderFilename, TripleMap) {
    EXPECT_EQ(tc::loader_filename("x86_64-unknown-linux-gnu"), "ld-linux-x86-64.so.2");
    EXPECT_EQ(tc::loader_filename("aarch64-linux-gnu"), "ld-linux-aarch64.so.1");
    EXPECT_EQ(tc::loader_filename("x86_64-linux-musl"), "ld-musl-x86_64.so.1");
    EXPECT_EQ(tc::loader_filename("aarch64-linux-musl"), "ld-musl-aarch64.so.1");
    EXPECT_EQ(tc::loader_filename("wasm32-unknown-unknown"), "");
}

TEST(DistroLoaderPath, LsbLayoutPerArch) {
    EXPECT_EQ(tc::distro_loader_path("x86_64-linux-gnu"), "/lib64/ld-linux-x86-64.so.2");
    EXPECT_EQ(tc::distro_loader_path("aarch64-linux-gnu"), "/lib/ld-linux-aarch64.so.1");
    EXPECT_EQ(tc::distro_loader_path("mips64-unknown"), "");
}

TEST(ResolveLoader, TripleMapHit) {
    Tmp dir;
    auto lib = dir.path / "lib64";
    touch(lib / "ld-linux-x86-64.so.2");
    EXPECT_EQ(tc::resolve_loader(lib, "x86_64-unknown-linux-gnu"),
              lib / "ld-linux-x86-64.so.2");
}

TEST(ResolveLoader, DeclaredExportsWinOverTripleMap) {
    Tmp dir;
    auto lib = dir.path / "lib64";
    touch(lib / "ld-linux-x86-64.so.2");
    touch(dir.path / "custom" / "ld-linux-x86-64.so.2");
    std::ofstream(dir.path / ".xpkg-exports.json")
        << R"({"runtime":{"loader":"custom/ld-linux-x86-64.so.2"}})";
    EXPECT_EQ(tc::resolve_loader(lib, "x86_64-unknown-linux-gnu"),
              dir.path / "custom" / "ld-linux-x86-64.so.2");
}

TEST(ResolveLoader, GlobFallbackForUnknownTriple) {
    Tmp dir;
    auto lib = dir.path / "lib";
    touch(lib / "ld-linux-aarch64.so.1");
    auto got = tc::resolve_loader(lib, "");  // no triple info at all
    EXPECT_EQ(got, lib / "ld-linux-aarch64.so.1");
}

TEST(ResolveLoader, EmptyWhenNothingFound) {
    Tmp dir;
    auto lib = dir.path / "lib64";
    std::filesystem::create_directories(lib);
    EXPECT_TRUE(tc::resolve_loader(lib, "x86_64-linux-gnu").empty());
}

TEST(PayloadLibDir, PrefersLib64ThenLib) {
    Tmp dir;
    touch(dir.path / "lib" / "ld-linux-x86-64.so.2");
    EXPECT_EQ(tc::payload_lib_dir_with_loader(dir.path, "x86_64-linux-gnu"),
              dir.path / "lib");
    touch(dir.path / "lib64" / "ld-linux-x86-64.so.2");
    EXPECT_EQ(tc::payload_lib_dir_with_loader(dir.path, "x86_64-linux-gnu"),
              dir.path / "lib64");
}

// Fabricate a bundled-LLVM-style toolchain: bin/clang++ + bin/clang++.cfg,
// libc++ headers, and a glibc payload.
struct FakeClangWithPayload {
    Tmp dir;
    tc::Toolchain t;
    std::filesystem::path glibcLib;
    FakeClangWithPayload() {
        auto llvm = dir.path / "llvm";
        touch(llvm / "bin" / "clang++");
        touch(llvm / "bin" / "clang++.cfg");
        touch(llvm / "include" / "c++" / "v1" / "version");
        auto glibc = dir.path / "glibc";
        glibcLib = glibc / "lib64";
        touch(glibc / "include" / "features.h");
        touch(glibcLib / "Scrt1.o");
        touch(glibcLib / "ld-linux-x86-64.so.2");
        t.compiler = tc::CompilerId::Clang;
        t.binaryPath = llvm / "bin" / "clang++";
        t.targetTriple = "x86_64-unknown-linux-gnu";
        t.payloadPaths = tc::PayloadPaths{
            .glibcInclude = glibc / "include",
            .glibcLib = glibcLib,
            .linuxInclude = {},
        };
    }
};

TEST(LinkModel, ClangCfgPayloadFirstCarriesCrtDiscovery) {
    FakeClangWithPayload fx;
    auto lm = tc::resolve_link_model(fx.t);
    EXPECT_EQ(lm.mode, tc::CLibMode::PayloadFirst);
    EXPECT_TRUE(lm.clangWithCfg);
    EXPECT_EQ(lm.crtDir, fx.glibcLib);
    EXPECT_EQ(lm.loader, fx.glibcLib / "ld-linux-x86-64.so.2");

    auto link = lm.link_flags(ident);
    // -B is the #195 fix: CRT objects resolve via -B, never -L.
    EXPECT_NE(link.find(" -B" + fx.glibcLib.string()), std::string::npos);
    EXPECT_NE(link.find(" -L" + fx.glibcLib.string()), std::string::npos);
    EXPECT_NE(link.find("--dynamic-linker=" + lm.loader.string()), std::string::npos);

    auto compile = lm.compile_flags(ident);
    EXPECT_NE(compile.find("-isystem"), std::string::npos);
    EXPECT_EQ(compile.find("-idirafter"), std::string::npos);
}

TEST(LinkModel, ClangDriverModelExposesCfgAndHeaders) {
    FakeClangWithPayload fx;
    auto dm = tc::resolve_clang_driver(fx.t);
    EXPECT_TRUE(dm.hasCfg);
    ASSERT_FALSE(dm.cxxIncludes.empty());
    EXPECT_NE(dm.compile_flags(ident).find("--no-default-config"), std::string::npos);
}

TEST(LinkModel, GccSysrootWinsOverPayload) {
    Tmp dir;
    auto sysroot = dir.path / "sysroot";
    touch(sysroot / "usr" / "include" / "stdlib.h");
    touch(sysroot / "usr" / "include" / "linux" / "limits.h");
    tc::Toolchain t;
    t.compiler = tc::CompilerId::GCC;
    t.binaryPath = dir.path / "bin" / "g++";
    t.targetTriple = "x86_64-linux-gnu";
    t.sysroot = sysroot;
    t.payloadPaths = tc::PayloadPaths{
        .glibcInclude = dir.path / "glibc" / "include",
        .glibcLib = dir.path / "glibc" / "lib64",
        .linuxInclude = dir.path / "linux" / "include",
    };
    auto lm = tc::resolve_link_model(t);
    EXPECT_EQ(lm.mode, tc::CLibMode::Sysroot);
    EXPECT_NE(lm.compile_flags(ident).find("--sysroot="), std::string::npos);
    EXPECT_NE(lm.link_flags(ident).find("--sysroot="), std::string::npos);
    // Kernel headers exist in the sysroot → no supplement.
    EXPECT_TRUE(lm.systemIncludes.empty());
}

TEST(LinkModel, GccSysrootSupplementsMissingKernelHeaders) {
    Tmp dir;
    auto sysroot = dir.path / "sysroot";
    touch(sysroot / "usr" / "include" / "stdlib.h");  // no linux/limits.h
    tc::Toolchain t;
    t.compiler = tc::CompilerId::GCC;
    t.targetTriple = "x86_64-linux-gnu";
    t.sysroot = sysroot;
    t.payloadPaths = tc::PayloadPaths{
        .glibcInclude = dir.path / "glibc" / "include",
        .glibcLib = dir.path / "glibc" / "lib64",
        .linuxInclude = dir.path / "linux" / "include",
    };
    auto lm = tc::resolve_link_model(t);
    ASSERT_EQ(lm.systemIncludes.size(), 1u);
    // Sysroot-mode supplement renders as -isystem even for GCC.
    EXPECT_NE(lm.compile_flags(ident).find("-isystem"), std::string::npos);
}

TEST(LinkModel, GccPayloadUsesIdirafterAndNoLoader) {
    Tmp dir;
    auto glibcLib = dir.path / "glibc" / "lib64";
    touch(glibcLib / "ld-linux-x86-64.so.2");
    tc::Toolchain t;
    t.compiler = tc::CompilerId::GCC;
    t.targetTriple = "x86_64-linux-gnu";
    t.payloadPaths = tc::PayloadPaths{
        .glibcInclude = dir.path / "glibc" / "include",
        .glibcLib = glibcLib,
        .linuxInclude = {},
    };
    auto lm = tc::resolve_link_model(t);
    EXPECT_EQ(lm.mode, tc::CLibMode::PayloadFirst);
    EXPECT_NE(lm.compile_flags(ident).find("-idirafter"), std::string::npos);
    auto link = lm.link_flags(ident);
    EXPECT_NE(link.find(" -B"), std::string::npos);
    // GCC's loader/rpath is owned by the specs fixup, not the command line.
    EXPECT_EQ(link.find("dynamic-linker"), std::string::npos);
    EXPECT_EQ(link.find("-rpath"), std::string::npos);
}

TEST(LinkModel, NothingUsableYieldsNoneAndEmptyFlags) {
    tc::Toolchain t;
    t.compiler = tc::CompilerId::Clang;
    t.targetTriple = "x86_64-linux-gnu";
    auto lm = tc::resolve_link_model(t);
    EXPECT_EQ(lm.mode, tc::CLibMode::None);
    EXPECT_TRUE(lm.compile_flags(ident).empty());
    EXPECT_TRUE(lm.link_flags(ident).empty());
}

}  // namespace
