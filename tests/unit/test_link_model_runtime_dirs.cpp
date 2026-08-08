// An artifact must find every library it needs inside the sandbox — in BOTH
// link modes.
//
// RUNPATH is assembled by two separate paths, CLibMode::Sysroot and
// CLibMode::PayloadFirst, and any given machine only ever takes one of them.
// The compiler's own runtime (libgcc_s.so.1) lives beside the compiler, not in
// the C library, and a produced binary links it whether or not the build ever
// mentions it. gcc's patched specs used to supply that directory; removing the
// specs rewrite therefore has to supply it explicitly — and at first only the
// Sysroot path did.
//
// The consequence was invisible where it was written: on a machine with a host
// toolchain the artifact simply resolved libgcc_s.so.1 from /lib and ran. It
// failed only in a throw-away home on CI, as `error while loading shared
// libraries` with nothing said about what was missing.
//
// Asserted here rather than in e2e on purpose. Which mode an e2e reaches
// depends on whether the machine happens to have a usable sysroot — this one
// does, so an e2e leg written for PayloadFirst quietly ran Sysroot twice and
// passed with the defect in place. The link model takes its mode from its
// input, so a test can name the mode instead of hoping for it.

#include <gtest/gtest.h>

import std;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.model;

namespace tc = mcpp::toolchain;

namespace {

void touch(const std::filesystem::path& p) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}

// A payload tree with a glibc beside a gcc, as xlings lays them out.
struct Payload {
    std::filesystem::path root, compiler, glibcLib, gccLib, sysroot;
    Payload() {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp_linkmodel_{}", std::random_device{}());
        auto xpkgs = root / "data" / "xpkgs";
        compiler = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
        touch(compiler);
        gccLib = xpkgs / "xim-x-gcc" / "16.1.0" / "lib64";
        touch(gccLib / "libgcc_s.so.1");
        glibcLib = xpkgs / "xim-x-glibc" / "2.39" / "lib64";
        touch(glibcLib / "libc.so.6");
        touch(glibcLib / "ld-linux-x86-64.so.2");
        touch(xpkgs / "xim-x-glibc" / "2.39" / "include" / "features.h");
        sysroot = root / "subos" / "default";
        touch(sysroot / "usr" / "include" / "stdlib.h");
    }
    ~Payload() { std::error_code ec; std::filesystem::remove_all(root, ec); }

    tc::Toolchain toolchain(bool withSysroot) const {
        tc::Toolchain t;
        t.compiler     = tc::CompilerId::GCC;
        t.version      = "16.1.0";
        t.binaryPath   = compiler;
        t.targetTriple = "x86_64-linux-gnu";
        t.runtimeBinding = "glibc@2.39";
        tc::PayloadPaths pp;
        pp.glibcLib     = glibcLib;
        pp.glibcInclude = glibcLib.parent_path() / "include";
        t.payloadPaths  = pp;
        if (withSysroot) t.sysroot = sysroot;
        return t;
    }
};

std::string joined(const tc::ToolchainLinkModel& lm) {
    auto id = [](const std::filesystem::path& p) { return p.string(); };
    std::string s;
    for (auto& tok : lm.link_tokens(id)) { s += tok; s += ' '; }
    return s;
}

TEST(LinkModelRuntimeDirs, PayloadFirstCarriesTheCompilerRuntime) {
    Payload p;
    auto lm = tc::resolve_link_model(p.toolchain(/*withSysroot=*/false));
    ASSERT_EQ(lm.mode, tc::CLibMode::PayloadFirst);
    auto line = joined(lm);
    EXPECT_NE(line.find("-Wl,-rpath," + p.gccLib.string()), std::string::npos)
        << "libgcc_s.so.1 lives here and nothing else will find it:\n" << line;
}

TEST(LinkModelRuntimeDirs, SysrootCarriesTheCompilerRuntime) {
    Payload p;
    auto lm = tc::resolve_link_model(p.toolchain(/*withSysroot=*/true));
    ASSERT_EQ(lm.mode, tc::CLibMode::Sysroot);
    auto line = joined(lm);
    EXPECT_NE(line.find("-Wl,-rpath," + p.gccLib.string()), std::string::npos)
        << line;
}

// The C library's own directory, in both modes, for the same reason.
TEST(LinkModelRuntimeDirs, BothModesCarryTheCLibrary) {
    Payload p;
    for (bool withSysroot : {false, true}) {
        auto line = joined(tc::resolve_link_model(p.toolchain(withSysroot)));
        EXPECT_NE(line.find("-Wl,-rpath," + p.glibcLib.string()),
                  std::string::npos)
            << "withSysroot=" << withSysroot << "\n" << line;
    }
}

// And the interpreter. A sysroot says where headers live; it says nothing
// about which loader runs the result, and gcc's `*link:` no longer answers.
TEST(LinkModelRuntimeDirs, BothModesNameTheInterpreter) {
    Payload p;
    for (bool withSysroot : {false, true}) {
        auto line = joined(tc::resolve_link_model(p.toolchain(withSysroot)));
        EXPECT_NE(line.find("--dynamic-linker=" + p.glibcLib.string()),
                  std::string::npos)
            << "withSysroot=" << withSysroot << "\n" << line;
    }
}

}  // namespace
