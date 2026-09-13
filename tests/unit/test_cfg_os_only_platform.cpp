#include <gtest/gtest.h>

import std;
import mcpp.build.prepare_inputs;

// #630 item 7 — "a selector that names only an operating system is a
// platform" (design record 2026-09-13-630 §8.2). `os_only_platforms` is the
// function `mcpp emit xpkg` (`mcpp::pm::emit_xpkg`) asks to decide whether a
// `[target.<selector>]` conditional tool declaration can be folded into one
// of the descriptor's three platform blocks (`linux`, `macosx`, `windows`)
// instead of only producing the `publish/target-axis-tools` advisory.
//
// Both directions are asserted: the positive claim (the seven forms §8.2
// lists each map onto the block(s) they name) and the negative claim (a
// predicate mentioning anything else — an architecture, an environment, a
// combinator, `not(...)`, even one built entirely from OS terms — is refused,
// which is what keeps the descriptor from claiming an install-time edge on a
// platform the predicate does not unconditionally name).

namespace cfgpred = mcpp::build::cfgpred;

namespace {
bool has(const std::vector<std::string>& v, std::string_view a) {
    return std::ranges::find(v, a) != v.end();
}
}  // namespace

// ── Positive: the seven OS-only forms ───────────────────────────────────────

TEST(OsOnlyPlatforms, BareAliasWindows) {
    auto p = cfgpred::os_only_platforms("windows");
    EXPECT_EQ(p, std::vector<std::string>({"windows"}));
}

TEST(OsOnlyPlatforms, CfgWindows) {
    auto p = cfgpred::os_only_platforms("cfg(windows)");
    EXPECT_EQ(p, std::vector<std::string>({"windows"}));
}

TEST(OsOnlyPlatforms, CfgOsEqualsLinux) {
    auto p = cfgpred::os_only_platforms(R"(cfg(os = "linux"))");
    EXPECT_EQ(p, std::vector<std::string>({"linux"}));
}

TEST(OsOnlyPlatforms, CfgLinux) {
    auto p = cfgpred::os_only_platforms("cfg(linux)");
    EXPECT_EQ(p, std::vector<std::string>({"linux"}));
}

TEST(OsOnlyPlatforms, CfgMacos) {
    auto p = cfgpred::os_only_platforms("cfg(macos)");
    // The descriptor's spelling is `macosx`, not `macos` — see
    // `mcpp::pm::emit_xpkg`'s three blocks.
    EXPECT_EQ(p, std::vector<std::string>({"macosx"}));
}

TEST(OsOnlyPlatforms, CfgOsEqualsWindows) {
    auto p = cfgpred::os_only_platforms(R"(cfg(os = "windows"))");
    EXPECT_EQ(p, std::vector<std::string>({"windows"}));
}

TEST(OsOnlyPlatforms, CfgOsEqualsMacos) {
    auto p = cfgpred::os_only_platforms(R"(cfg(os = "macos"))");
    EXPECT_EQ(p, std::vector<std::string>({"macosx"}));
}

TEST(OsOnlyPlatforms, CfgUnixNamesBothMacAndLinux) {
    // `unix` means `family == "unix"`, which macOS and Linux both satisfy —
    // the only one of the seven forms that names two blocks at once.
    auto p = cfgpred::os_only_platforms("cfg(unix)");
    EXPECT_EQ(p.size(), 2u);
    EXPECT_TRUE(has(p, "linux"));
    EXPECT_TRUE(has(p, "macosx"));
    EXPECT_FALSE(has(p, "windows"));
}

TEST(OsOnlyPlatforms, BareAliasUnix) {
    auto p = cfgpred::os_only_platforms("unix");
    EXPECT_EQ(p.size(), 2u);
    EXPECT_TRUE(has(p, "linux"));
    EXPECT_TRUE(has(p, "macosx"));
}

// ── Negative: everything the record says keeps the warning ─────────────────

TEST(OsOnlyPlatforms, ArchitectureIsNotOsOnly) {
    auto p = cfgpred::os_only_platforms(R"(cfg(target_arch = "aarch64"))");
    EXPECT_TRUE(p.empty());
}

TEST(OsOnlyPlatforms, CombinatorOfOsAndArchIsNotOsOnly) {
    auto p = cfgpred::os_only_platforms(
        R"(cfg(all(linux, target_arch = "x86_64")))");
    EXPECT_TRUE(p.empty());
}

TEST(OsOnlyPlatforms, NotWindowsIsNotOsOnly) {
    // Syntactically this names only an OS, but `not(...)` answers an
    // unbounded set of platforms (everything this vocabulary does not yet
    // call "windows"), not the two blocks "linux and macosx" would be if this
    // were treated as their union. Kept conservative: any combinator
    // disqualifies, `not` included.
    auto p = cfgpred::os_only_platforms("cfg(not(windows))");
    EXPECT_TRUE(p.empty());
}

TEST(OsOnlyPlatforms, EnvIsNotOsOnly) {
    auto p = cfgpred::os_only_platforms(R"(cfg(env = "android"))");
    EXPECT_TRUE(p.empty());
}

TEST(OsOnlyPlatforms, LayerKeyIsNotOsOnly) {
    auto p = cfgpred::os_only_platforms(R"(cfg(compiler = "llvm"))");
    EXPECT_TRUE(p.empty());
}

TEST(OsOnlyPlatforms, AnAllOsCombinatorIsStillNotOsOnly) {
    // Even a combinator built ENTIRELY from OS terms is refused — see the
    // comment on `os_only_platforms` for why `any(linux, macos)` is not the
    // same claim as either block alone.
    auto p = cfgpred::os_only_platforms("cfg(any(linux, macos))");
    EXPECT_TRUE(p.empty());
}

TEST(OsOnlyPlatforms, ExactTripleIsNotOsOnly) {
    // An exact triple shares no grammar with cfg()/the bare aliases; it names
    // one target, not a platform.
    auto p = cfgpred::os_only_platforms("x86_64-linux-musl");
    EXPECT_TRUE(p.empty());
}

TEST(OsOnlyPlatforms, UnrecognisedOsValueIsNotOsOnly) {
    auto p = cfgpred::os_only_platforms(R"(cfg(os = "freebsd"))");
    EXPECT_TRUE(p.empty());
}
