// tests/unit/test_cppfly.cpp — mcpp.toolchain.cppfly: the `standard = "c++fly"`
// capability tables (latest std level + experimental gates per compiler) and
// the c++latest/c++fly-aware std-flag spelling.
// Table facts pinned by on-machine probes (gcc 16.1.0, 2026-07-14) — see
// .agents/docs/2026-07-14-std-features-experimental-gate-design.md §2/§11-Q3.
#include <gtest/gtest.h>

import std;
import mcpp.toolchain.cppfly;
import mcpp.toolchain.model;

using namespace mcpp::toolchain;

namespace {
Toolchain tc_of(CompilerId id, std::string ver, std::string stdlib = "libstdc++") {
    Toolchain tc;
    tc.compiler = id;
    tc.version = std::move(ver);
    tc.stdlibId = std::move(stdlib);
    return tc;
}
bool has_flag(const std::vector<std::string>& v, std::string_view f) {
    return std::find(v.begin(), v.end(), f) != v.end();
}
const cppfly::FeatureState* feature(const cppfly::Resolution& r, std::string_view n) {
    for (auto& f : r.features)
        if (f.name == n) return &f;
    return nullptr;
}
} // namespace

TEST(CppFly, CompilerMajor) {
    EXPECT_EQ(cppfly::compiler_major(tc_of(CompilerId::GCC, "16.1.0")), 16);
    EXPECT_EQ(cppfly::compiler_major(tc_of(CompilerId::Clang, "22.1.8")), 22);
    EXPECT_EQ(cppfly::compiler_major(tc_of(CompilerId::GCC, "")), 0);
}

TEST(CppFly, LatestStdCanonical) {
    int lvl = 0;
    EXPECT_EQ(cppfly::latest_std_canonical(tc_of(CompilerId::GCC, "16.1.0"), &lvl), "c++26");
    EXPECT_EQ(lvl, 26);
    EXPECT_EQ(cppfly::latest_std_canonical(tc_of(CompilerId::GCC, "13.2.0")), "c++23");
    EXPECT_EQ(cppfly::latest_std_canonical(tc_of(CompilerId::Clang, "22.1.8")), "c++2c");
    EXPECT_EQ(cppfly::latest_std_canonical(tc_of(CompilerId::MSVC, "19.44")), "c++26");
}

TEST(CppFly, ResolveGcc16) {
    auto r = cppfly::resolve(tc_of(CompilerId::GCC, "16.1.0"));
    EXPECT_EQ(r.stdCanonical, "c++26");
    EXPECT_TRUE(has_flag(r.flags, "-freflection"));
    auto* refl = feature(r, "reflection");
    ASSERT_NE(refl, nullptr);
    EXPECT_TRUE(refl->enabled);
    EXPECT_EQ(refl->paper, "P2996");
    // Contracts: enabled by -std=c++26 alone on GCC 16 (probe-pinned) — no
    // extra flag, but reported enabled for the summary.
    auto* con = feature(r, "contracts");
    ASSERT_NE(con, nullptr);
    EXPECT_TRUE(con->enabled);
    EXPECT_TRUE(con->flags.empty());
    // libstdc++ ships experimental bits ungated.
    EXPECT_FALSE(has_flag(r.flags, "-fexperimental-library"));
}

TEST(CppFly, ResolveGcc15SkipsByVersion) {
    auto r = cppfly::resolve(tc_of(CompilerId::GCC, "15.1.0"));
    EXPECT_TRUE(r.flags.empty());
    auto* refl = feature(r, "reflection");
    ASSERT_NE(refl, nullptr);
    EXPECT_FALSE(refl->enabled);
    EXPECT_NE(refl->reason.find("16"), std::string::npos);  // names the version floor
}

TEST(CppFly, ResolveClangLibcxx) {
    auto r = cppfly::resolve(tc_of(CompilerId::Clang, "22.1.8", "libc++"));
    EXPECT_EQ(r.stdCanonical, "c++2c");
    EXPECT_TRUE(has_flag(r.flags, "-fexperimental-library"));
    auto* refl = feature(r, "reflection");
    ASSERT_NE(refl, nullptr);
    EXPECT_FALSE(refl->enabled);
    auto* lib = feature(r, "experimental-library");
    ASSERT_NE(lib, nullptr);
    EXPECT_TRUE(lib->enabled);
}

TEST(CppFly, ResolveMsvcAllSkipped) {
    auto r = cppfly::resolve(tc_of(CompilerId::MSVC, "19.44", "msvc-stl"));
    EXPECT_TRUE(r.flags.empty());
    for (auto& f : r.features) EXPECT_FALSE(f.enabled);
}

TEST(CppFly, StdFlagResolvesLatestAndFly) {
    auto gcc16 = tc_of(CompilerId::GCC, "16.1.0");
    EXPECT_EQ(cppfly::std_flag(gcc16, "c++fly", 1000), "-std=c++26");
    // standard = "c++latest" used to reach the GNU driver as the invalid
    // spelling -std=c++latest (design §11-Q5) — now resolves to the family
    // latest.
    EXPECT_EQ(cppfly::std_flag(gcc16, "c++latest", 999), "-std=c++26");
    EXPECT_EQ(cppfly::std_flag(gcc16, "c++23", 23), "-std=c++23");
    EXPECT_EQ(cppfly::std_flag(gcc16, "gnu++23", 23), "-std=gnu++23");
    EXPECT_EQ(cppfly::std_flag(tc_of(CompilerId::Clang, "22.1.8"), "c++fly", 1000), "-std=c++2c");
    EXPECT_EQ(cppfly::std_flag(tc_of(CompilerId::MSVC, "19.44"), "c++fly", 1000), "/std:c++latest");
    EXPECT_EQ(cppfly::std_flag(tc_of(CompilerId::MSVC, "19.44"), "c++latest", 999), "/std:c++latest");
}

TEST(CppFly, EffectiveDialectFlagsDedup) {
    auto gcc16 = tc_of(CompilerId::GCC, "16.1.0");
    // User hand-wrote -freflection too — the fly gate must not duplicate it.
    auto out = cppfly::effective_dialect_flags(gcc16, true, {"-freflection", "-fchar8_t"});
    EXPECT_EQ(std::count(out.begin(), out.end(), std::string("-freflection")), 1);
    EXPECT_TRUE(has_flag(out, "-fchar8_t"));
    // Not experimental → manifest flags pass through untouched.
    auto off = cppfly::effective_dialect_flags(gcc16, false, {"-fchar8_t"});
    EXPECT_EQ(off, (std::vector<std::string>{"-fchar8_t"}));
}
