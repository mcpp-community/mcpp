// A sysroot is trusted because of where it lives, not because it is there.
//
// gcc records `--sysroot=<...>/.xlings/subos/default` as a literal string when
// it is built, and keeps reporting that string in every project it later
// serves. On a machine with several checkouts, that path routinely EXISTS and
// belongs to a different one -- so the original predicate, "does the path
// exist", answered yes and the build silently took another project's headers.
// Measured: a build inside mcpp resolved a sysroot under an unrelated repo.
//
// These assertions pin the axis that replaced it. Nothing here touches the
// filesystem, deliberately: the point is that existence is not consulted.

#include <gtest/gtest.h>

import std;
import mcpp.fallback.probe_sysroot;

namespace fb = mcpp::fallback;

namespace {

const std::filesystem::path kRegistry = "/home/u/.mcpp/registry";
const std::filesystem::path kProject  = "/home/u/work/thisrepo";

TEST(SysrootOwnership, PayloadSysrootIsOwned) {
    EXPECT_FALSE(fb::sysroot_is_foreign(
        kRegistry / "subos" / "default", kRegistry, kProject));
}

TEST(SysrootOwnership, ProjectLocalSysrootIsOwned) {
    EXPECT_FALSE(fb::sysroot_is_foreign(
        kProject / ".xlings" / "subos" / "default", kRegistry, kProject));
}

// The case that motivated the predicate: a path that exists, is spelled
// exactly like a legitimate one, and belongs to someone else.
TEST(SysrootOwnership, AnotherCheckoutIsForeign) {
    EXPECT_TRUE(fb::sysroot_is_foreign(
        "/home/u/work/otherrepo/.xlings/subos/default", kRegistry, kProject));
}

TEST(SysrootOwnership, AnotherHomeIsForeign) {
    EXPECT_TRUE(fb::sysroot_is_foreign(
        "/home/other/.mcpp/registry/subos/default", kRegistry, kProject));
}

// Both anchors are load-bearing. Registry alone would condemn every
// project-local tree; project alone would condemn every payload sysroot.
TEST(SysrootOwnership, EitherAnchorSuffices) {
    EXPECT_FALSE(fb::sysroot_is_foreign(
        kRegistry / "subos" / "default", kRegistry, {}));
    EXPECT_FALSE(fb::sysroot_is_foreign(
        kProject / ".xlings" / "subos" / "default", {}, kProject));
}

// No sysroot is not a foreign sysroot. gcc payloads on this machine report an
// empty `-print-sysroot`, so this is the ordinary case, not an edge one.
TEST(SysrootOwnership, EmptyIsNotForeign) {
    EXPECT_FALSE(fb::sysroot_is_foreign({}, kRegistry, kProject));
}

// A prefix that merely shares characters is not containment.
TEST(SysrootOwnership, SiblingPrefixIsNotContainment) {
    EXPECT_TRUE(fb::sysroot_is_foreign(
        "/home/u/work/thisrepo-backup/.xlings/subos/default",
        kRegistry, kProject));
}

// A directory whose NAME begins with two dots is not an escape. The first
// version compared the relative path's text (`rfind("..", 0)`), which called
// `/home/u/.mcpp/registry/..cache` an escape from the registry -- and did not
// compile at all on Windows, where `native()` is a wstring. Containment is a
// question about path components, so it is asked of components.
TEST(SysrootOwnership, DotDotPrefixedNameIsNotAnEscape) {
    EXPECT_FALSE(fb::sysroot_is_foreign(
        kRegistry / "..cache" / "subos" / "default", kRegistry, kProject));
}

TEST(SysrootOwnership, PathIsUnderIsDirectlyTestable) {
    EXPECT_TRUE(fb::path_is_under(kRegistry / "a" / "b", kRegistry));
    EXPECT_TRUE(fb::path_is_under(kRegistry, kRegistry));
    EXPECT_FALSE(fb::path_is_under(kRegistry.parent_path(), kRegistry));
    EXPECT_FALSE(fb::path_is_under(kRegistry, {}));
    EXPECT_FALSE(fb::path_is_under({}, kRegistry));
}

}  // namespace
