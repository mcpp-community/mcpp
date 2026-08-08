// When nothing declares the runtime, how far may mcpp go on its own?
//
// The rule this replaced asked a directory for "the glibc" and took the first
// entry readdir yielded. That is a CHOICE made by something with no bearing on
// what the artifact loads, and it went wrong the moment a dependency's
// `xim:glibc@>=2.38` floor installed a second payload: the compile side took
// 2.44 while the interpreter still named 2.39.
//
// So the axis is not "may mcpp look at the payload directory" -- it is
// "is there anything to choose between". One payload is an answer; two are a
// question only the subos can settle.
//
// This mattered because the compatibility sources (gcc's specs, clang's cfg,
// the compiler's PT_INTERP) are all things some earlier mechanism wrote, and a
// machine can legitimately have none of them.

#include <gtest/gtest.h>

import std;
import mcpp.toolchain.post_install;

namespace tc = mcpp::toolchain;

namespace {

struct Home {
    std::filesystem::path root, compiler;
    explicit Home(std::initializer_list<const char*> glibcVersions) {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp_binding_{}", std::random_device{}());
        auto xpkgs = root / "data" / "xpkgs";
        compiler = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
        std::filesystem::create_directories(compiler.parent_path());
        std::ofstream(compiler) << "not an elf";
        for (auto v : glibcVersions)
            std::filesystem::create_directories(
                xpkgs / "xim-x-glibc" / v / "lib64");
    }
    ~Home() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

TEST(RuntimeBindingFallback, OnePayloadIsAnAnswer) {
    Home h{"2.39"};
    EXPECT_EQ(tc::baked_runtime_binding(h.compiler), "glibc@2.39");
}

// The incident, in one assertion. Two payloads and nothing declaring which:
// silence, not a coin flip.
TEST(RuntimeBindingFallback, TwoPayloadsIsSilence) {
    Home h{"2.39", "2.44"};
    EXPECT_EQ(tc::baked_runtime_binding(h.compiler), "");
}

TEST(RuntimeBindingFallback, NoPayloadIsSilence) {
    Home h{};
    EXPECT_EQ(tc::baked_runtime_binding(h.compiler), "");
}

// A compiler outside any xpkgs tree (a system gcc) has no payload set to read,
// and must not acquire one from somewhere else.
TEST(RuntimeBindingFallback, NonPayloadCompilerGetsNothing) {
    EXPECT_EQ(tc::baked_runtime_binding("/usr/bin/g++"), "");
}

TEST(RuntimeBindingFallback, EmptyPathIsSilence) {
    EXPECT_EQ(tc::baked_runtime_binding({}), "");
}

}  // namespace
