#include <gtest/gtest.h>

import std;
import mcpp.platform;
import mcpp.toolchain.compat;

using namespace mcpp::toolchain::compat;

static std::string host_musl() {
    return std::string(mcpp::platform::host_arch) + "-linux-musl";
}

// ── canonical spellings pass through unchanged ───────────────────────────────

TEST(Compat, CanonicalFamiliesPassThrough) {
    auto g = normalize_spec("gcc", "16.1.0");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->family, "gcc");
    EXPECT_EQ(g->version, "16.1.0");
    EXPECT_TRUE(g->target.empty());
    EXPECT_FALSE(g->changed);

    auto l = normalize_spec("llvm", "20.1.7");
    ASSERT_TRUE(l.has_value());
    EXPECT_EQ(l->family, "llvm");
    EXPECT_FALSE(l->changed);

    auto m = normalize_spec("msvc", "system");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->family, "msvc");
    EXPECT_EQ(m->version, "system");
    EXPECT_FALSE(m->changed);
}

// ── §6.2 alias table: every legacy row round-trips ───────────────────────────

TEST(Compat, MuslVersionSuffixBecomesTarget) {
    auto s = normalize_spec("gcc", "15.1.0-musl");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->family, "gcc");
    EXPECT_EQ(s->version, "15.1.0");
    EXPECT_EQ(s->target.str(), host_musl());
    EXPECT_TRUE(s->changed);
    EXPECT_FALSE(s->hint.empty());
}

TEST(Compat, MuslGccCompilerNameBecomesTarget) {
    auto s = normalize_spec("musl-gcc", "15.1.0");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->family, "gcc");
    EXPECT_EQ(s->version, "15.1.0");
    EXPECT_EQ(s->target.str(), host_musl());
    EXPECT_TRUE(s->changed);
}

TEST(Compat, TripleNamedGccBecomesTarget) {
    auto s = normalize_spec("aarch64-linux-musl-gcc", "16.1.0");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->family, "gcc");
    EXPECT_EQ(s->version, "16.1.0");
    EXPECT_EQ(s->target.str(), "aarch64-linux-musl");
    EXPECT_TRUE(s->changed);
}

TEST(Compat, MingwAndMingwCrossCollapseToWindowsGnu) {
    // One concept, two host-split legacy names → the SAME normalized identity.
    auto native = normalize_spec("mingw", "16.1.0");
    auto cross  = normalize_spec("mingw-cross", "16.1.0");
    ASSERT_TRUE(native.has_value());
    ASSERT_TRUE(cross.has_value());
    EXPECT_EQ(native->family, "gcc");
    EXPECT_EQ(cross->family, "gcc");
    EXPECT_EQ(native->target.str(), "x86_64-windows-gnu");
    EXPECT_EQ(cross->target.str(), "x86_64-windows-gnu");
    EXPECT_TRUE(native->changed);
    EXPECT_TRUE(cross->changed);
}

TEST(Compat, ClangAliasBecomesLlvm) {
    auto s = normalize_spec("clang", "20.1.7");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->family, "llvm");
    EXPECT_EQ(s->version, "20.1.7");
    EXPECT_TRUE(s->changed);
}

TEST(Compat, PartialMuslVersionSuffix) {
    // "gcc 15-musl" — partial version with the legacy suffix.
    auto s = normalize_spec("gcc", "15-musl");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->version, "15");
    EXPECT_EQ(s->target.str(), host_musl());
}

TEST(Compat, XimNamespacePrefixStripped) {
    auto s = normalize_spec("xim:gcc", "16.1.0");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->family, "gcc");
    EXPECT_FALSE(s->changed);
}

// ── rejects ─────────────────────────────────────────────────────────────────

TEST(Compat, RejectsUnknownFamilies) {
    EXPECT_FALSE(normalize_spec("tcc", "1.0").has_value());
    EXPECT_FALSE(normalize_spec("llvm", "20.1.7-musl").has_value());  // llvm has no musl flavor
    EXPECT_FALSE(normalize_spec("not-a-triple-gcc", "1.0").has_value());
}
