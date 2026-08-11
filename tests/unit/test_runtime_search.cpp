// The run-time search-path contract: rank, provenance, machine-locality.
//
// This module is pure policy — it never touches the filesystem and knows
// nothing about ELF — so it is the one part of the closure that can be pinned
// exactly, on every platform, including the ones where none of it applies.
// Everything downstream (the DT_RPATH mcpp emits, what `mcpp pack` strips,
// what the closure resolver walks) is a consequence of these four answers.

#include <gtest/gtest.h>

import std;
import mcpp.platform.runtime_search;

namespace search = mcpp::platform::search;
using search::Origin;

namespace {

// THE invariant. Payload before farm is not a preference: `<subos>/lib` is a
// symlink view rewritten by every `xlings install`, a payload directory is
// written once, and an already-linked artifact must not have its libc changed
// out from under it by a later install.
TEST(RuntimeSearch, PayloadOutranksFarm) {
    EXPECT_LT(search::rank(Origin::Payload), search::rank(Origin::SubosFarm));
    EXPECT_LT(search::rank(Origin::Package), search::rank(Origin::SubosFarm));
    EXPECT_LT(search::rank(Origin::SubosFarm), search::rank(Origin::HostDefault));
}

// `mcpp pack` asks this to decide what may not be baked into a distributable.
// The host's own /usr/lib is the only one that is NOT this machine's private
// state — relying on it is an ordinary host requirement, not a dependency on
// the build box.
TEST(RuntimeSearch, MachineLocalIsEverythingButTheHostDefaults) {
    EXPECT_TRUE(search::is_machine_local(Origin::Payload));
    EXPECT_TRUE(search::is_machine_local(Origin::Package));
    EXPECT_TRUE(search::is_machine_local(Origin::SubosFarm));
    EXPECT_FALSE(search::is_machine_local(Origin::HostDefault));
}

// These strings are PUBLISHED — they are the `origin` field of every entry in
// `resolution.json`'s `runtime.search.closure`, which CI, `mcpp why runtime` and
// e2e 219 all read. Renaming one is a wire-format change, so it is pinned here
// rather than left to whatever `to_string` happens to say.
TEST(RuntimeSearch, OriginNamesArePublishedAndStable) {
    EXPECT_EQ(search::to_string(Origin::Payload),     "payload");
    EXPECT_EQ(search::to_string(Origin::Package),     "package");
    EXPECT_EQ(search::to_string(Origin::SubosFarm),   "subos_farm");
    EXPECT_EQ(search::to_string(Origin::HostDefault), "host_default");
}

TEST(RuntimeSearch, OrderedSortsByRankNotByInsertion) {
    std::vector<search::Dir> input{
        {"/farm", Origin::SubosFarm},
        {"/pkg",  Origin::Package},
        {"/pay",  Origin::Payload},
    };
    auto out = search::ordered(input);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].path, std::filesystem::path("/pay"));
    EXPECT_EQ(out[1].path, std::filesystem::path("/pkg"));
    EXPECT_EQ(out[2].path, std::filesystem::path("/farm"));
}

// Stable within a rank, because insertion order still decides among peers —
// libglvnd resolves GL vendors by exactly that order, so reordering equals
// would change which driver a program gets.
TEST(RuntimeSearch, OrderedIsStableWithinARank) {
    std::vector<search::Dir> input{
        {"/pay/b", Origin::Payload},
        {"/pay/a", Origin::Payload},
        {"/pay/c", Origin::Payload},
    };
    auto out = search::ordered(input);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].path, std::filesystem::path("/pay/b"));
    EXPECT_EQ(out[1].path, std::filesystem::path("/pay/a"));
    EXPECT_EQ(out[2].path, std::filesystem::path("/pay/c"));
}

// The same directory reachable two ways is ONE entry, and it takes the
// stronger origin. A payload that is also visible through the farm view is a
// payload; ranking it as farm would push the real libc behind everything else
// the farm holds — the exact failure the ordering exists to prevent.
TEST(RuntimeSearch, DuplicatePathKeepsTheStrongestOrigin) {
    std::vector<search::Dir> input{
        {"/shared", Origin::SubosFarm},
        {"/shared", Origin::Payload},
        {"/other",  Origin::SubosFarm},
    };
    auto out = search::ordered(input);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].path, std::filesystem::path("/shared"));
    EXPECT_EQ(out[0].origin, Origin::Payload);
    EXPECT_EQ(out[1].origin, Origin::SubosFarm);
}

TEST(RuntimeSearch, OrderedNormalizesAndDropsEmpty) {
    std::vector<search::Dir> input{
        {"", Origin::Payload},
        {"/a/./b/", Origin::Payload},
        {"/a/b", Origin::Payload},
    };
    auto out = search::ordered(input);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].path, std::filesystem::path("/a/b"));
}

// The opt-out is a CROSS-REPO name (openxlings/xlings#540). Pinning the
// spelling here means a rename shows up as a failed test rather than as a
// silently ineffective declaration — the failure mode of an env var nobody
// reads is that everything looks fine.
TEST(RuntimeSearch, LinkerOptOutIsSpelledOnce) {
    EXPECT_EQ(search::kLinkerPathInjectionOptOut, "XLINGS_SUBOS_LD_PATHS");
    EXPECT_EQ(search::kLinkerPathInjectionOptOutValue, "0");
}

} // namespace
