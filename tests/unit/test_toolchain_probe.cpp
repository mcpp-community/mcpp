// Payload resolution: the version comes from an AUTHORITY, never from a scan.
//
// Before this, probe_payload_paths asked find_sibling_tool for "the glibc"
// and took whatever came back -- which is whatever readdir yielded first. With
// one glibc installed that is always right, so nothing ever forced it to be
// correct. Installing a second one (a dependency with a `>=` floor is enough)
// split the build in half: the compile side took one version while the
// artifact's interpreter, written into gcc's specs at install time, still
// named the other. Binaries then referenced GLIBC_2.42 symbols against a 2.39
// runtime, and the failures landed on packages with nothing to do with the
// dependency that pulled the second glibc in.
//
// These assertions are about determinism, not about preference. "Given
// glibc@2.39, return 2.39" must hold with 2.44 also present and must not
// depend on directory order -- which is why the fixture always installs both.
//
// Design: .agents/docs/2026-08-08-payload-version-and-contract-drift-design.md §3.2

#include <gtest/gtest.h>

import std;
import mcpp.toolchain.probe;

namespace {

namespace tc = mcpp::toolchain;

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_probe_test_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

void touch(const std::filesystem::path& p) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}

// A home with TWO glibc payloads. Both are always present: a fixture with one
// cannot tell a resolver that reads the authority from one that guesses.
struct TwoGlibcHome {
    Tmp dir;
    std::filesystem::path compilerBin;
    TwoGlibcHome() {
        auto xpkgs = dir.path / "data" / "xpkgs";
        compilerBin = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
        touch(compilerBin);
        for (auto v : {"2.39", "2.44"}) {
            auto root = xpkgs / "xim-x-glibc" / v;
            touch(root / "include" / "features.h");
            touch(root / "lib64" / "libc.so.6");
            touch(root / "lib64" / "ld-linux-x86-64.so.2");
        }
    }
    std::filesystem::path libOf(std::string_view v) const {
        return dir.path / "data" / "xpkgs" / "xim-x-glibc" / std::string(v) / "lib64";
    }
};

TEST(PayloadProbe, ResolvesTheBindingExactly) {
    TwoGlibcHome h;
    auto pp = tc::probe_payload_paths(h.compilerBin, "glibc@2.39");
    ASSERT_TRUE(pp.has_value());
    EXPECT_EQ(pp->glibcLib, h.libOf("2.39"))
        << "the authority said 2.39 and 2.44 is also installed; a resolver "
           "that scans would be free to return either";
}

TEST(PayloadProbe, ResolvesTheOtherBindingExactly) {
    TwoGlibcHome h;
    auto pp = tc::probe_payload_paths(h.compilerBin, "glibc@2.44");
    ASSERT_TRUE(pp.has_value());
    EXPECT_EQ(pp->glibcLib, h.libOf("2.44"));
}

// No authority is not "pick something sensible" -- it is a refusal. Guessing
// here is what produced a build whose compile and run halves disagreed, and
// the guess is invisible: every artifact looks normal until one links a
// library built against the other version.
TEST(PayloadProbe, NoBindingIsARefusalNotAGuess) {
    TwoGlibcHome h;
    EXPECT_FALSE(tc::probe_payload_paths(h.compilerBin, "").has_value());
}

// A binding naming a version that is not installed must fail rather than fall
// back to one that is. Falling back would reintroduce the split silently.
TEST(PayloadProbe, MissingVersionDoesNotFallBack) {
    TwoGlibcHome h;
    EXPECT_FALSE(tc::probe_payload_paths(h.compilerBin, "glibc@2.28").has_value());
}

// The binding's NAME matters too: `musl@1.2.5` must not resolve a glibc
// payload just because the version string happens to be parseable.
TEST(PayloadProbe, WrongRuntimeFamilyDoesNotResolveGlibc) {
    TwoGlibcHome h;
    EXPECT_FALSE(tc::probe_payload_paths(h.compilerBin, "musl@1.2.5").has_value());
}

// Layout: lib64 preferred, lib accepted. Kept from the original probe -- a
// payload that ships only `lib` is a real shape, and this is the one piece of
// convention that stays, because it is about WHERE inside a chosen payload,
// not about WHICH payload.
TEST(PayloadProbe, AcceptsLibWhenThereIsNoLib64) {
    Tmp dir;
    auto xpkgs = dir.path / "data" / "xpkgs";
    auto bin = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
    touch(bin);
    auto root = xpkgs / "xim-x-glibc" / "2.39";
    touch(root / "include" / "features.h");
    touch(root / "lib" / "ld-linux-x86-64.so.2");

    auto pp = tc::probe_payload_paths(bin, "glibc@2.39");
    ASSERT_TRUE(pp.has_value());
    EXPECT_EQ(pp->glibcLib, root / "lib");
}

}  // namespace
