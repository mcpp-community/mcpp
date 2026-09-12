#include <gtest/gtest.h>

import std;
import mcpp.toolchain.abi;
import mcpp.toolchain.model;
import mcpp.manifest;
import mcpp.build.prepare;

using namespace mcpp::toolchain;

namespace {

Toolchain make_tc(std::string triple, std::string stdlib, CompilerId cc) {
    Toolchain tc;
    tc.targetTriple = std::move(triple);
    tc.stdlibId     = std::move(stdlib);
    tc.compiler     = cc;
    return tc;
}

}  // namespace

// ─── abi_profile: each dimension from its single canonical source ───────────

TEST(AbiProfile, ClangLibcxxOnLinuxGnuIsGlibcAtLibcLevel) {
    // The bug: a clang+libc++ toolchain targeting *-linux-gnu is glibc at the
    // libc level; only its C++ stdlib is libc++. These are independent dims.
    auto p = abi_profile(make_tc("x86_64-unknown-linux-gnu", "libc++", CompilerId::Clang));
    EXPECT_EQ(p.libc, "glibc");
    EXPECT_EQ(p.cxxStdlib, "libc++");
    EXPECT_EQ(p.arch, "x86_64");
    EXPECT_EQ(p.os, "linux");
    EXPECT_EQ(p.cxxAbi, "itanium");
}

TEST(AbiProfile, GccGlibc) {
    auto p = abi_profile(make_tc("x86_64-linux-gnu", "libstdc++", CompilerId::GCC));
    EXPECT_EQ(p.libc, "glibc");
    EXPECT_EQ(p.cxxStdlib, "libstdc++");
}

TEST(AbiProfile, MuslTripleIsMusl) {
    auto p = abi_profile(make_tc("x86_64-linux-musl", "libstdc++", CompilerId::GCC));
    EXPECT_EQ(p.libc, "musl");
    EXPECT_EQ(p.os, "linux");
}

// ─── abi_check: the regression + the genuine incompatibility ────────────────

TEST(AbiCheck, GlibcCLibraryUnderLibcxxIsCompatible) {
    // Regression lock for the glfw-under-clang false positive.
    auto p = abi_profile(make_tc("x86_64-unknown-linux-gnu", "libc++", CompilerId::Clang));
    auto c = parse_abi_capability("abi:glibc", "compat.glfw");
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE(abi_check(p, {*c}).empty());
}

TEST(AbiCheck, GlibcDepUnderMuslMismatchesOnLibcDim) {
    auto p = abi_profile(make_tc("x86_64-linux-musl", "libstdc++", CompilerId::GCC));
    auto c = parse_abi_capability("abi:glibc", "compat.glfw");
    ASSERT_TRUE(c.has_value());
    auto mm = abi_check(p, {*c});
    ASSERT_EQ(mm.size(), 1u);
    EXPECT_EQ(mm[0].dim, AbiDim::Libc);
    EXPECT_EQ(mm[0].need, "glibc");
    EXPECT_EQ(mm[0].got, "musl");
}

TEST(AbiCheck, UnspecifiedDimensionIsDontCare) {
    // A cxxstdlib constraint must NOT be implied by a bare libc capability.
    auto p = abi_profile(make_tc("x86_64-unknown-linux-gnu", "libc++", CompilerId::Clang));
    EXPECT_TRUE(abi_check(p, {{AbiDim::Arch, "x86_64", "x"}}).empty());      // matches
    EXPECT_FALSE(abi_check(p, {{AbiDim::Arch, "aarch64", "x"}}).empty());    // mismatches
}

// ─── parse_abi_capability: legacy + dimensional forms ───────────────────────

TEST(AbiCapability, LegacyBareFormIsLibc) {
    auto c = parse_abi_capability("abi:glibc", "pkg");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->dim, AbiDim::Libc);
    EXPECT_EQ(c->value, "glibc");
    EXPECT_EQ(c->source, "pkg");
}

TEST(AbiCapability, DimensionalForm) {
    auto c = parse_abi_capability("abi:cxxstdlib=libc++");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->dim, AbiDim::CxxStdlib);
    EXPECT_EQ(c->value, "libc++");
}

TEST(AbiCapability, NonAbiCapabilityIgnored) {
    EXPECT_FALSE(parse_abi_capability("x11.display").has_value());
    EXPECT_FALSE(parse_abi_capability("opengl.glx.driver").has_value());
}

TEST(AbiCapability, UnknownDimensionIgnored) {
    EXPECT_FALSE(parse_abi_capability("abi:nonsense=1").has_value());
}

// ─── `[target.<sel>.abi] exceptions` and `requires_abi` on the target axis ──
//
// Design 2026-09-12 (a UI framework on Android, iOS and Web), section 2.1
// (A1, the `abi` table's second member) and section 2.6 (A6, `requires_abi`
// moved onto the target axis). Unrelated to the `abi_profile`/`abi_check`
// coverage above -- that is the toolchain's ABI-COMPATIBILITY vocabulary
// (glibc/musl/libc++); this is the MANIFEST's `[target.<selector>.abi]`
// table, a different "abi" that happens to share the word.

namespace manifest_abi {

namespace cfgpred = mcpp::build::cfgpred;

// One matching selector's `ConditionalConfig`, found by predicate text.
// `parse_string` keeps every `[target.<sel>]` section as one entry in
// `conditionalConfigs` regardless of whether any target will ever match it
// (matching happens later, at merge_conditional_config), so a parse-level
// test asks for the entry by name instead of assuming index 0.
const mcpp::manifest::ConditionalConfig* find_cc(
        const mcpp::manifest::Manifest& m, std::string_view predicate) {
    for (auto const& cc : m.conditionalConfigs)
        if (cc.predicate == predicate) return &cc;
    return nullptr;
}

}  // namespace manifest_abi

TEST(AbiTableExceptions, ParsesAndReachesTheConditionalConfig) {
    constexpr auto src = R"(
[package]
name    = "webrt"
version = "0.1.0"
[target.'cfg(os = "emscripten")'.abi]
exceptions = true
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    auto* cc = manifest_abi::find_cc(*m, "cfg(os = \"emscripten\")");
    ASSERT_NE(cc, nullptr);
    EXPECT_TRUE(cc->abiExceptionsDeclared);
    EXPECT_TRUE(cc->abiExceptions);
    // `threads` is untouched by a table that only names `exceptions` --
    // the two members are independent switches, not a package deal.
    EXPECT_FALSE(cc->abiThreadsDeclared);
}

TEST(AbiTableExceptions, BothMembersInOneTableParseIndependently) {
    constexpr auto src = R"(
[package]
name    = "webrt"
version = "0.1.0"
[target.'cfg(os = "emscripten")'.abi]
threads    = true
exceptions = false
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    auto* cc = manifest_abi::find_cc(*m, "cfg(os = \"emscripten\")");
    ASSERT_NE(cc, nullptr);
    EXPECT_TRUE(cc->abiThreadsDeclared);
    EXPECT_TRUE(cc->abiThreads);
    EXPECT_TRUE(cc->abiExceptionsDeclared);
    EXPECT_FALSE(cc->abiExceptions);
}

TEST(AbiTableExceptions, UnknownMemberNamesBothThreadsAndExceptions) {
    constexpr auto src = R"(
[package]
name    = "a"
version = "0.1.0"
[target.x.abi]
frobnicate = true
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find(
        "has no member 'frobnicate'; the members are: threads, exceptions"),
        std::string::npos) << m.error().message;
}

TEST(AbiTableExceptions, ANonBooleanExceptionsIsRefused) {
    constexpr auto src = R"(
[package]
name    = "a"
version = "0.1.0"
[target.x.abi]
exceptions = 1
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find(".exceptions must be true or false"),
              std::string::npos) << m.error().message;
}

// ─── `requires_abi` as a direct key of `[target.<sel>]` ─────────────────────

TEST(RequiresAbiOnTargetAxis, SelectorScopedRequirementParsesIntoTheConditionalConfig) {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)']
requires_abi = { threads = true }
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    auto* cc = manifest_abi::find_cc(*m, "cfg(linux)");
    ASSERT_NE(cc, nullptr);
    EXPECT_TRUE(cc->requiresAbiThreads);
    EXPECT_FALSE(cc->requiresAbiExceptions);
}

TEST(RequiresAbiOnTargetAxis, FeatureFormParsesUnderTheSelector) {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)'.feature-requires-abi]
mt = { threads = true }
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    auto* cc = manifest_abi::find_cc(*m, "cfg(linux)");
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->featureRequiresAbiThreads.contains("mt"));
    EXPECT_TRUE(cc->featureRequiresAbiThreads.at("mt"));
    // The feature-requires-abi table registers the feature UNCONDITIONALLY
    // (the same rule feature-deps follows, #359), so it does not trip the
    // unknown-feature diagnostic on a build that never selects this target.
    EXPECT_TRUE(m->featuresMap.contains("mt"));
}

TEST(RequiresAbiOnTargetAxis, UnknownMemberInsideRequiresAbiNamesBoth) {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)']
requires_abi = { thread = true }
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find(
        "requires_abi.thread: the members are `threads`, `exceptions`, booleans"),
        std::string::npos) << m.error().message;
}

TEST(RequiresAbiOnTargetAxis, UnknownMemberInsideFeatureFormNamesBoth) {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)'.feature-requires-abi]
mt = { thread = true }
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find(
        "feature-requires-abi.mt].thread: the members are `threads`, "
        "`exceptions`, booleans"),
        std::string::npos) << m.error().message;
}

// ─── The union: a matching selector's requirement joins the package's ──────
//
// No cross toolchain needed -- merge_conditional_config only evaluates the
// predicate against a resolved TRIPLE, which `cfgpred::context_for` builds
// without touching a compiler. Same technique as
// tests/unit/test_target_xlings_axis.cpp's `merged_for`.

namespace manifest_abi {

mcpp::manifest::Manifest merged_for(std::string_view src, std::string_view triple) {
    auto m = mcpp::manifest::parse_string(src);
    EXPECT_TRUE(m.has_value()) << (m.has_value() ? "" : m.error().format());
    if (!m) return {};
    mcpp::build::merge_conditional_config(*m, cfgpred::context_for(triple));
    return *m;
}

}  // namespace manifest_abi

TEST(RequiresAbiOnTargetAxis, MatchingSelectorUnionsIntoThePackagesRequirement) {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)']
requires_abi = { threads = true }
)";
    auto lin = manifest_abi::merged_for(src, "x86_64-unknown-linux-gnu");
    ASSERT_EQ(lin.targetRequiresAbiThreads.size(), 1u);
    EXPECT_EQ(lin.targetRequiresAbiThreads.front(), "cfg(linux)");
    EXPECT_TRUE(lin.targetRequiresAbiExceptions.empty());
}

TEST(RequiresAbiOnTargetAxis, ASelectorThatDoesNotMatchTheHostImposesNothing) {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(windows)']
requires_abi = { threads = true }
)";
    // A Windows-only selector must not reach a Linux-resolved build -- the
    // negative direction of the union above, and the case the design record
    // calls out explicitly (A6: "the Web build is unaffected ... in both
    // directions").
    auto lin = manifest_abi::merged_for(src, "x86_64-unknown-linux-gnu");
    EXPECT_TRUE(lin.targetRequiresAbiThreads.empty());
}

TEST(RequiresAbiOnTargetAxis, FeatureFormUnionsOnlyForTheMatchingSelector) {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)'.feature-requires-abi]
mt = { threads = true }
[target.'cfg(windows)'.feature-requires-abi]
mt = { exceptions = true }
)";
    auto lin = manifest_abi::merged_for(src, "x86_64-unknown-linux-gnu");
    ASSERT_TRUE(lin.targetFeatureRequiresAbiThreads.contains("mt"));
    EXPECT_EQ(lin.targetFeatureRequiresAbiThreads.at("mt").front(), "cfg(linux)");
    EXPECT_FALSE(lin.targetFeatureRequiresAbiExceptions.contains("mt"));
}
