#include <gtest/gtest.h>

import std;
import mcpp.pack.abi_tag;
import mcpp.toolchain.model;

using namespace mcpp::pack;
using mcpp::toolchain::CompilerId;
using mcpp::toolchain::Toolchain;

namespace {

Toolchain tc(CompilerId cc, std::string ver, std::string stdlib, std::string stdlibVer) {
    Toolchain t;
    t.compiler      = cc;
    t.version       = std::move(ver);
    t.stdlibId      = std::move(stdlib);
    t.stdlibVersion = std::move(stdlibVer);
    // Deliberately the COMPILER's spelling, which must not reach the tag.
    t.targetTriple  = "x86_64-w64-mingw32";
    return t;
}

}  // namespace

// ─── the tag is a projection, and of the CANONICAL triple ──────────────────

TEST(AbiTag, CxxSurfaceNamesEveryDimension) {
    auto t = cxx_surface_tag(tc(CompilerId::GCC, "16.1.0", "libstdc++", "16.1.0"),
                             "x86_64-linux-gnu", 23);
    EXPECT_EQ(t.str(), "x86_64-linux-gnu-gcc16-libstdcxx16-c++23");
    EXPECT_FALSE(t.c_surface());
}

TEST(AbiTag, UsesTheCanonicalTripleNotTheCompilerReportedOne) {
    // The toolchain above reports `x86_64-w64-mingw32`; mcpp's target
    // vocabulary — and every `[target.'<triple>']` key a package can be
    // selected by — says `x86_64-windows-gnu`. Publishing the compiler's
    // spelling would give one decision two spellings.
    auto t = cxx_surface_tag(tc(CompilerId::GCC, "16.1.0", "libstdc++", "16.1.0"),
                             "x86_64-windows-gnu", 23);
    EXPECT_EQ(t.triple, "x86_64-windows-gnu");
    EXPECT_EQ(t.str().find("mingw"), std::string::npos);
}

TEST(AbiTag, LibcxxAndMsvcStlTokenize) {
    EXPECT_EQ(stdlib_token("libstdc++"), "libstdcxx");
    EXPECT_EQ(stdlib_token("libc++"),    "libcxx");
    EXPECT_EQ(stdlib_token("msvc-stl"),  "msvcstl");
    EXPECT_EQ(stdlib_token(""),          "unknownstl");
}

TEST(AbiTag, MajorIsLeadingDigitsOnly) {
    EXPECT_EQ(major_of("16.1.0"),      "16");
    EXPECT_EQ(major_of("19.44.35207"), "19");
    EXPECT_EQ(major_of("22"),          "22");
    EXPECT_EQ(major_of(""),            "0");
}

// ─── the SHAPE is the surface: a C library publishes a shorter tag ─────────

TEST(AbiTag, CSurfaceIsTripleOnly) {
    auto t = c_surface_tag("x86_64-linux-gnu");
    EXPECT_EQ(t.str(), "x86_64-linux-gnu");
    EXPECT_TRUE(t.c_surface());
}

TEST(AbiTag, CSurfaceAcceptsAnyCompilerAndStdlib) {
    auto published = c_surface_tag("x86_64-linux-gnu");
    auto current   = cxx_surface_tag(tc(CompilerId::Clang, "22.1.8", "libc++", "22.1.8"),
                                     "x86_64-linux-gnu", 26);
    // An extern "C" library constrains the libc ABI and nothing else, so an
    // unspecified dimension is don't-care — the same rule abi_check uses.
    EXPECT_TRUE(tag_check(published, current).empty());
}

TEST(AbiTag, CSurfaceStillRefusesAForeignTriple) {
    auto published = c_surface_tag("x86_64-linux-gnu");
    auto current   = c_surface_tag("aarch64-linux-gnu");
    auto bad = tag_check(published, current);
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0].dimension, "triple");
}

// ─── round-trip: parsing runs from the END, because triples vary in length ──

TEST(AbiTag, ParsesFullTagBackWithATwoDashTriple) {
    auto t = parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++23");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->triple,   "x86_64-linux-gnu");
    EXPECT_EQ(t->compiler, "gcc16");
    EXPECT_EQ(t->stdlib,   "libstdcxx16");
    EXPECT_EQ(t->standard, "c++23");
}

TEST(AbiTag, ParsesFullTagBackWithAOneDashTriple) {
    // `aarch64-macos` has one dash, `x86_64-linux-gnu` has two. Splitting from
    // the front cannot tell where the triple ends; splitting from the back can,
    // because the C++ half is exactly three segments ending in c++NN.
    auto t = parse_abi_tag("aarch64-macos-llvm22-libcxx22-c++23");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->triple,   "aarch64-macos");
    EXPECT_EQ(t->compiler, "llvm22");
    EXPECT_EQ(t->standard, "c++23");
}

TEST(AbiTag, ParsesATripleOnlyTag) {
    auto t = parse_abi_tag("x86_64-linux-gnu");
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(t->c_surface());
    EXPECT_EQ(t->triple, "x86_64-linux-gnu");
}

TEST(AbiTag, RoundTripsEveryShape) {
    for (auto s : { "x86_64-linux-gnu",
                    "x86_64-linux-musl-gcc16-libstdcxx16-c++23",
                    "aarch64-macos-llvm22-libcxx22-c++26",
                    "x86_64-windows-msvc-msvc19-msvcstl19-c++23" }) {
        auto t = parse_abi_tag(s);
        ASSERT_TRUE(t.has_value()) << s;
        EXPECT_EQ(t->str(), s);
    }
}

TEST(AbiTag, RejectsEmptyAndNonTagInput) {
    EXPECT_FALSE(parse_abi_tag("").has_value());
    // Three trailing segments that do not end in c++NN are part of the triple,
    // not a C++ half — this must not be silently mis-split.
    auto t = parse_abi_tag("a-b-c-d");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->triple, "a-b-c-d");
    EXPECT_TRUE(t->c_surface());
}

// ─── the gate ──────────────────────────────────────────────────────────────

TEST(AbiTag, RefusesADifferentCompilerMajor) {
    auto published = *parse_abi_tag("x86_64-linux-gnu-gcc15-libstdcxx15-c++23");
    auto current   = *parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++23");
    auto bad = tag_check(published, current);
    ASSERT_EQ(bad.size(), 2u);
    EXPECT_EQ(bad[0].dimension, "compiler");
    EXPECT_EQ(bad[0].need, "gcc15");
    EXPECT_EQ(bad[0].got,  "gcc16");
    EXPECT_EQ(bad[1].dimension, "stdlib");
}

TEST(AbiTag, StandardIsAFloorNotAnEquality) {
    auto published = *parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++23");
    // Building at a HIGHER level is fine: the interface being compiled is the
    // artifact's own source and a newer level accepts it.
    auto higher = *parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++26");
    EXPECT_TRUE(tag_check(published, higher).empty());
    // Lower is not: the interface may use syntax this level lacks.
    auto lower = *parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++20");
    auto bad = tag_check(published, lower);
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0].dimension, "standard");
}

TEST(AbiTag, ReportsEveryMismatchAtOnce) {
    // One diagnostic, not one per rebuild.
    auto published = *parse_abi_tag("aarch64-linux-gnu-gcc15-libcxx15-c++26");
    auto current   = *parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++23");
    EXPECT_EQ(tag_check(published, current).size(), 4u);
}
