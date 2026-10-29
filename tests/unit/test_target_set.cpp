#include <gtest/gtest.h>

import std;
import mcpp.toolchain.triple;

namespace ts = mcpp::toolchain::target_set;
namespace tt = mcpp::toolchain::triple;

namespace {

// The engine's own conventions for every candidate, except where the target
// table's platform column names one.
std::expected<std::string, std::string> column_platform(const tt::Triple& t) {
    if (auto* row = tt::find_known_target(t)) return std::string(row->platform);
    return std::string{};
}

std::vector<std::string> legs_of(std::initializer_list<std::string> requests) {
    std::vector<std::string> req(requests);
    auto r = ts::resolve(req, column_platform);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
    std::vector<std::string> out;
    if (r) for (auto& l : *r) out.push_back(l.triple);
    return out;
}

} // namespace

// ── recognition: the first segment, not a failed parse ──────────────────────

TEST(TargetSet, RecognisesSystemWords) {
    EXPECT_TRUE(ts::is_set_spelling("openkal"));
    EXPECT_TRUE(ts::is_set_spelling("linux-musl"));
    EXPECT_TRUE(ts::is_set_spelling("none-eabihf"));
    EXPECT_TRUE(ts::is_set_spelling("android"));
    EXPECT_FALSE(ts::is_set_spelling("x86_64-openkal"));
    EXPECT_FALSE(ts::is_set_spelling("aarch64-linux-musl"));
}

TEST(TargetSet, VendorFirstSpellingIsATriple) {
    // `unknown-linux-gnu` parses as a triple (architecture `unknown`); it is
    // not a set, because `unknown` is not a system word.
    EXPECT_FALSE(ts::is_set_spelling("unknown-linux-gnu"));
    EXPECT_FALSE(ts::is_set_spelling("apple-macos"));
    auto legs = legs_of({"unknown-linux-gnu"});
    ASSERT_EQ(legs.size(), 1u);
    EXPECT_EQ(legs.front(), "unknown-linux-gnu");
}

// ── expansion ────────────────────────────────────────────────────────────────

TEST(TargetSet, OpenkalNamesBothArchitectures) {
    auto e = ts::expand_set("openkal", column_platform);
    ASSERT_TRUE(e.has_value()) << e.error();
    EXPECT_EQ(e->members, (std::vector<std::string>{"x86_64-openkal", "aarch64-openkal"}));
    EXPECT_TRUE(e->platform.starts_with("mcpplibs.openkal-platform@"));
}

TEST(TargetSet, LinuxMuslLeavesPlannedRowsOut) {
    auto e = ts::expand_set("linux-musl", column_platform);
    ASSERT_TRUE(e.has_value()) << e.error();
    EXPECT_EQ(e->members, (std::vector<std::string>{"x86_64-linux-musl", "aarch64-linux-musl"}));
    for (auto& m : e->members) EXPECT_NE(m, "riscv64-linux-musl");
}

TEST(TargetSet, LinuxFillsGnuAndLeavesAndroidOut) {
    auto e = ts::expand_set("linux", column_platform);
    ASSERT_TRUE(e.has_value()) << e.error();
    EXPECT_EQ(e->members, (std::vector<std::string>{"x86_64-linux-gnu", "aarch64-linux-gnu"}));
}

TEST(TargetSet, WindowsFillsGnu) {
    auto e = ts::expand_set("windows", column_platform);
    ASSERT_TRUE(e.has_value()) << e.error();
    EXPECT_EQ(e->members, (std::vector<std::string>{"x86_64-windows-gnu"}));
}

TEST(TargetSet, AndroidIsTheLinuxAndroidRows) {
    auto e = ts::expand_set("android", column_platform);
    ASSERT_TRUE(e.has_value()) << e.error();
    EXPECT_EQ(e->members,
              (std::vector<std::string>{"aarch64-linux-android", "x86_64-linux-android"}));
}

TEST(TargetSet, IosAndItsSimulator) {
    auto device = ts::expand_set("ios", column_platform);
    ASSERT_TRUE(device.has_value()) << device.error();
    EXPECT_EQ(device->members, (std::vector<std::string>{"aarch64-ios"}));
    auto sim = ts::expand_set("ios-sim", column_platform);
    ASSERT_TRUE(sim.has_value()) << sim.error();
    EXPECT_EQ(sim->members, (std::vector<std::string>{"aarch64-ios-sim", "x86_64-ios-sim"}));
}

TEST(TargetSet, BareMetalNeedsItsEnvironment) {
    auto bare = ts::expand_set("none", column_platform);
    ASSERT_FALSE(bare.has_value());
    EXPECT_NE(bare.error().find("none-elf"), std::string::npos);
    auto elf = ts::expand_set("none-elf", column_platform);
    ASSERT_TRUE(elf.has_value()) << elf.error();
    EXPECT_EQ(elf->members, (std::vector<std::string>{
        "riscv64-none-elf", "riscv32-none-elf", "aarch64-none-elf", "x86_64-none-elf"}));
}

TEST(TargetSet, EmptySetIsRefusedNamingThePlannedRows) {
    auto e = ts::expand_set("macos-gnu", column_platform);
    ASSERT_FALSE(e.has_value());
    auto none = ts::expand_set("linux-msvc", column_platform);
    ASSERT_FALSE(none.has_value());
    EXPECT_NE(none.error().find("names no target"), std::string::npos);
}

TEST(TargetSet, MalformedSetsAreRefused) {
    EXPECT_FALSE(ts::expand_set("linux-musl-extra", column_platform).has_value());
    EXPECT_FALSE(ts::expand_set("linux-", column_platform).has_value());
    EXPECT_FALSE(ts::expand_set("openkal-gnu", column_platform).has_value());
    EXPECT_FALSE(ts::expand_set("android-x", column_platform).has_value());
}

TEST(TargetSet, MembersSelectingDifferentPlatformsAreRefused) {
    auto split = [](const tt::Triple& t) -> std::expected<std::string, std::string> {
        return t.arch == "x86_64" ? std::string("a@1") : std::string("b@1");
    };
    auto e = ts::expand_set("openkal", split);
    ASSERT_FALSE(e.has_value());
    EXPECT_NE(e.error().find("select different platforms"), std::string::npos);
    EXPECT_NE(e.error().find("x86_64-openkal"), std::string::npos);
    EXPECT_NE(e.error().find("aarch64-openkal"), std::string::npos);
}

TEST(TargetSet, ASelectionErrorNamesTheMember) {
    auto broken = [](const tt::Triple&) -> std::expected<std::string, std::string> {
        return std::unexpected(std::string("no such platform"));
    };
    auto e = ts::expand_set("openkal", broken);
    ASSERT_FALSE(e.has_value());
    EXPECT_NE(e.error().find("x86_64-openkal: no such platform"), std::string::npos);
}

// ── several --target ─────────────────────────────────────────────────────────

TEST(TargetSet, UnionInTableOrderWithoutDuplicates) {
    auto legs = legs_of({"aarch64-openkal", "openkal", "x86_64-linux-musl"});
    EXPECT_EQ(legs, (std::vector<std::string>{
        "x86_64-linux-musl", "x86_64-openkal", "aarch64-openkal"}));
}

TEST(TargetSet, ATargetKeepsItsSpelling) {
    // Planned as the command would plan it alone: `x86_64-linux` is completed
    // and reported by the build, not here.
    auto legs = legs_of({"x86_64-linux"});
    ASSERT_EQ(legs.size(), 1u);
    EXPECT_EQ(legs.front(), "x86_64-linux");
    auto twice = legs_of({"x86_64-linux", "x86_64-linux-gnu"});
    EXPECT_EQ(twice.size(), 1u);
}

TEST(TargetSet, UnknownTargetsFollowTheTableInWrittenOrder) {
    auto legs = legs_of({"zz-custom-target", "x86_64-openkal", "yy-other"});
    EXPECT_EQ(legs, (std::vector<std::string>{"x86_64-openkal", "zz-custom-target", "yy-other"}));
}

TEST(TargetSet, LegsRecordTheirSet) {
    std::vector<std::string> req{"openkal", "x86_64-linux-musl"};
    auto r = ts::resolve(req, column_platform);
    ASSERT_TRUE(r.has_value());
    for (auto& l : *r) {
        if (l.triple.ends_with("openkal")) EXPECT_EQ(l.fromSet, "openkal");
        else EXPECT_TRUE(l.fromSet.empty());
    }
}
