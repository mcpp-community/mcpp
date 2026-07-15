#include <gtest/gtest.h>

import std;
import mcpp.toolchain.triple;

using namespace mcpp::toolchain::triple;

// ── parse: canonical spellings round-trip ────────────────────────────────────

TEST(Triple, ParsesCanonicalThreeSegment) {
    auto t = parse("x86_64-linux-musl");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->arch, "x86_64");
    EXPECT_EQ(t->os, "linux");
    EXPECT_EQ(t->env, "musl");
    EXPECT_EQ(t->str(), "x86_64-linux-musl");
    EXPECT_TRUE(t->is_musl());
    EXPECT_TRUE(is_known_target(*t));
}

TEST(Triple, ParsesCanonicalWindowsGnu) {
    auto t = parse("x86_64-windows-gnu");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->str(), "x86_64-windows-gnu");
    EXPECT_TRUE(t->is_windows_gnu());
    EXPECT_TRUE(t->is_pe());
    EXPECT_EQ(t->family(), "windows");
    EXPECT_TRUE(is_known_target(*t));
}

TEST(Triple, ParsesCanonicalMacos) {
    auto t = parse("aarch64-macos");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->arch, "aarch64");
    EXPECT_EQ(t->os, "macos");
    EXPECT_EQ(t->env, "");
    EXPECT_EQ(t->str(), "aarch64-macos");
    EXPECT_EQ(t->family(), "unix");
}

// ── parse: alias spellings normalize to canonical ────────────────────────────

TEST(Triple, NormalizesGnuMingwSpelling) {
    // GNU vendor triple: "w64" = vendor, "mingw32" = os segment (historical —
    // 64-bit targets still say mingw32). Canonicalizes to windows-gnu.
    auto t = parse("x86_64-w64-mingw32");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->str(), "x86_64-windows-gnu");
    EXPECT_TRUE(t->is_windows_gnu());
}

TEST(Triple, NormalizesFourSegmentRustSpellings) {
    EXPECT_EQ(parse("x86_64-unknown-linux-musl")->str(), "x86_64-linux-musl");
    EXPECT_EQ(parse("x86_64-unknown-linux-gnu")->str(), "x86_64-linux-gnu");
    EXPECT_EQ(parse("x86_64-pc-windows-msvc")->str(), "x86_64-windows-msvc");
    EXPECT_EQ(parse("x86_64-pc-windows-gnu")->str(), "x86_64-windows-gnu");
}

TEST(Triple, NormalizesAppleSpellings) {
    // arm64 → aarch64 (GNU arch spelling); darwin/macosx version suffixes drop.
    EXPECT_EQ(parse("arm64-apple-darwin24.1.0")->str(), "aarch64-macos");
    EXPECT_EQ(parse("arm64-apple-macosx15.0")->str(), "aarch64-macos");
    EXPECT_EQ(parse("aarch64-apple-darwin")->str(), "aarch64-macos");
}

TEST(Triple, NormalizesBareLinuxToGnuEnv) {
    EXPECT_EQ(parse("x86_64-linux")->str(), "x86_64-linux-gnu");
}

TEST(Triple, NormalizesDumpmachineSpellings) {
    // What real toolchains report via -dumpmachine.
    EXPECT_EQ(parse("x86_64-pc-linux-gnu")->str(), "x86_64-linux-gnu");
    EXPECT_EQ(parse("x86_64-linux-gnu")->str(), "x86_64-linux-gnu");
    EXPECT_EQ(parse("aarch64-linux-musl")->str(), "aarch64-linux-musl");
}

// ── parse: rejects non-triples ───────────────────────────────────────────────

TEST(Triple, RejectsNonTriples) {
    EXPECT_FALSE(parse("").has_value());
    EXPECT_FALSE(parse("gcc").has_value());
    EXPECT_FALSE(parse("x86_64").has_value());
    EXPECT_FALSE(parse("x86_64-linux-mus").has_value());   // typo'd env segment
    EXPECT_FALSE(parse("wasm32-wasi").has_value());        // outside the language
}

// ── known-target vocabulary ──────────────────────────────────────────────────

TEST(Triple, KnownTargetTableExposesTierAndPins) {
    auto t = parse("x86_64-linux-musl");
    ASSERT_TRUE(t.has_value());
    auto* info = find_known_target(*t);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->tier, "verified");
    EXPECT_EQ(info->pin, "gcc@16.1.0");
    EXPECT_TRUE(info->defaultStatic);

    auto w = parse("x86_64-w64-mingw32");   // alias resolves to the same row
    ASSERT_TRUE(w.has_value());
    auto* winfo = find_known_target(*w);
    ASSERT_NE(winfo, nullptr);
    EXPECT_EQ(winfo->canonical, "x86_64-windows-gnu");
    EXPECT_EQ(winfo->pin, "gcc@16.1.0");
    EXPECT_TRUE(winfo->defaultStatic);
}

TEST(Triple, UnknownButParseableTripleIsNotKnown) {
    auto t = parse("riscv64-linux-gnu");
    ASSERT_TRUE(t.has_value());
    EXPECT_FALSE(is_known_target(*t));
}

// ── did-you-mean ─────────────────────────────────────────────────────────────

TEST(Triple, DidYouMeanCatchesTypos) {
    auto s = did_you_mean("x86_64-linux-mus");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "x86_64-linux-musl");

    auto w = did_you_mean("x86_64-w64-mingw");
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(*w, "x86_64-windows-gnu");
}

TEST(Triple, DidYouMeanStaysQuietOnGarbage) {
    EXPECT_FALSE(did_you_mean("totally-unrelated-string-xyz").has_value());
}

// ── host ─────────────────────────────────────────────────────────────────────

TEST(Triple, HostTripleIsCanonicalAndNonEmpty) {
    auto h = host_triple();
    EXPECT_FALSE(h.empty());
    auto reparsed = parse(h.str());
    ASSERT_TRUE(reparsed.has_value());
    EXPECT_EQ(reparsed->str(), h.str());
}
