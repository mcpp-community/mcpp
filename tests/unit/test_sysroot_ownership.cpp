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


import std;
import mcpplibs.mtest;
import mcpp.fallback.probe_sysroot;

using namespace mcpplibs::mtest;

namespace fb = mcpp::fallback;

namespace {

const std::filesystem::path kRegistry = "/home/u/.mcpp/registry";
const std::filesystem::path kProject  = "/home/u/work/thisrepo";

UTest payload_sysroot_is_owned{"SysrootOwnership/PayloadSysrootIsOwned", [] {
    expect(!fb::sysroot_is_foreign(
        kRegistry / "subos" / "default", kRegistry, kProject));
}};

UTest project_local_sysroot_is_owned{"SysrootOwnership/ProjectLocalSysrootIsOwned", [] {
    expect(!fb::sysroot_is_foreign(
        kProject / ".xlings" / "subos" / "default", kRegistry, kProject));
}};

// The case that motivated the predicate: a path that exists, is spelled
// exactly like a legitimate one, and belongs to someone else.
UTest another_checkout_is_foreign{"SysrootOwnership/AnotherCheckoutIsForeign", [] {
    expect(fb::sysroot_is_foreign(
        "/home/u/work/otherrepo/.xlings/subos/default", kRegistry, kProject));
}};

UTest another_home_is_foreign{"SysrootOwnership/AnotherHomeIsForeign", [] {
    expect(fb::sysroot_is_foreign(
        "/home/other/.mcpp/registry/subos/default", kRegistry, kProject));
}};

// Both anchors are load-bearing. Registry alone would condemn every
// project-local tree; project alone would condemn every payload sysroot.
UTest either_anchor_suffices{"SysrootOwnership/EitherAnchorSuffices", [] {
    expect(!fb::sysroot_is_foreign(
        kRegistry / "subos" / "default", kRegistry, {}));
    expect(!fb::sysroot_is_foreign(
        kProject / ".xlings" / "subos" / "default", {}, kProject));
}};

// No sysroot is not a foreign sysroot. gcc payloads on this machine report an
// empty `-print-sysroot`, so this is the ordinary case, not an edge one.
UTest empty_is_not_foreign{"SysrootOwnership/EmptyIsNotForeign", [] {
    expect(!fb::sysroot_is_foreign({}, kRegistry, kProject));
}};

// A prefix that merely shares characters is not containment.
UTest sibling_prefix_is_not_containment{"SysrootOwnership/SiblingPrefixIsNotContainment", [] {
    expect(fb::sysroot_is_foreign(
        "/home/u/work/thisrepo-backup/.xlings/subos/default",
        kRegistry, kProject));
}};

// A directory whose NAME begins with two dots is not an escape. The first
// version compared the relative path's text (`rfind("..", 0)`), which called
// `/home/u/.mcpp/registry/..cache` an escape from the registry -- and did not
// compile at all on Windows, where `native()` is a wstring. Containment is a
// question about path components, so it is asked of components.
UTest dot_dot_prefixed_name_is_not_an_escape{"SysrootOwnership/DotDotPrefixedNameIsNotAnEscape", [] {
    expect(!fb::sysroot_is_foreign(
        kRegistry / "..cache" / "subos" / "default", kRegistry, kProject));
}};

UTest path_is_under_is_directly_testable{"SysrootOwnership/PathIsUnderIsDirectlyTestable", [] {
    expect(fb::path_is_under(kRegistry / "a" / "b", kRegistry));
    expect(fb::path_is_under(kRegistry, kRegistry));
    expect(!(fb::path_is_under(kRegistry.parent_path(), kRegistry)));
    expect(!fb::path_is_under(kRegistry, {}));
    expect(!fb::path_is_under({}, kRegistry));
}};

}  // namespace
