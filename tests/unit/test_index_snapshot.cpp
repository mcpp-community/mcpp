// The invariant this file exists for:
//
//   An index-side change must never take mcpp from "works" to "does not work".
//
// A published index can raise its client-version floor (index.toml min_mcpp).
// `xlings update` rewrites the tree in place, so before the guard existed a
// floor bump replaced a readable index with an unreadable one and left no way
// back — the refresh was the thing that broke the machine.
//
// The 2026-07-08 index design specified this behaviour ("staged refresh keeps
// the last compatible snapshot") and it was never implemented; the floor was
// checked in exactly one place, the descriptor reader. There was no test that
// could have noticed, because the only coverage was of the pure predicate.
// These are that missing test.

#include <gtest/gtest.h>

import std;
import mcpp.pm.index_snapshot;
import mcpp.pm.index_contract;
import mcpp.version;

using namespace mcpp::pm::index_snapshot;

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_idx_snap_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

void writeFile(const std::filesystem::path& p, std::string_view body) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream is(p);
    return std::string((std::istreambuf_iterator<char>(is)), {});
}

// A minimal index tree: pkgs/ makes it an index, index.toml carries the floor,
// .xlings-index-version is the snapshot identity.
void makeIndex(const std::filesystem::path& dir,
               std::string_view minMcpp,
               std::string_view version,
               std::string_view marker) {
    writeFile(dir / "pkgs" / "z" / "zlib.lua", std::format("-- {}\n", marker));
    writeFile(dir / ".xlings-index-version", version);
    if (minMcpp.empty()) {
        std::error_code ec;
        std::filesystem::remove(dir / "index.toml", ec);
    } else {
        writeFile(dir / "index.toml",
                  std::format("[index]\nspec = \"1\"\nmin_mcpp = \"{}\"\n", minMcpp));
    }
}

constexpr std::string_view kTooNew = "9999.9.9.9";   // no mcpp satisfies this

} // namespace

TEST(IndexSnapshot, DetectsIndexTreesAndIgnoresTheSnapshotStore) {
    Tmp t;
    auto data = t.path / "data";
    makeIndex(data / "mcpplibs", "", "v1", "a");
    makeIndex(data / "xim-pkgindex", "", "v1", "b");
    std::filesystem::create_directories(data / "not-an-index");   // no pkgs/
    std::filesystem::create_directories(data / ".index-snapshots" / "x" / "pkgs");

    auto dirs = index_dirs(data);
    ASSERT_EQ(dirs.size(), 2u);
    EXPECT_EQ(dirs[0].filename(), "mcpplibs");
    EXPECT_EQ(dirs[1].filename(), "xim-pkgindex");
}

TEST(IndexSnapshot, ArchiveRefusesAnUnusableTree) {
    Tmp t;
    auto data = t.path / "data";
    auto idx  = data / "mcpplibs";
    makeIndex(idx, kTooNew, "v1", "unusable");

    // A snapshot exists to be restored; archiving one nobody can read would
    // only give the recovery path a useless candidate.
    EXPECT_FALSE(archive(data, idx));
    EXPECT_TRUE(list_snapshots(data, idx).empty());
}

TEST(IndexSnapshot, ArchiveAndRestoreRoundTrip) {
    Tmp t;
    auto data = t.path / "data";
    auto idx  = data / "mcpplibs";
    makeIndex(idx, "0.0.85", "v1", "original");

    ASSERT_TRUE(archive(data, idx));
    auto snaps = list_snapshots(data, idx);
    ASSERT_EQ(snaps.size(), 1u);

    makeIndex(idx, "0.0.85", "v2", "replaced");
    EXPECT_NE(readFile(idx / "pkgs" / "z" / "zlib.lua").find("replaced"),
              std::string::npos);

    ASSERT_TRUE(restore(snaps[0], idx));
    EXPECT_NE(readFile(idx / "pkgs" / "z" / "zlib.lua").find("original"),
              std::string::npos);
}

// THE load-bearing test. A refresh that raises the floor beyond this binary
// must leave the machine working.
TEST(IndexSnapshot, RefreshThatBreaksTheIndexIsRolledBack) {
    Tmp t;
    auto data = t.path / "data";
    auto idx  = data / "mcpplibs";
    makeIndex(idx, "0.0.85", "good", "usable-tree");

    GuardOutcome out;
    int rc = guarded_refresh(data, [&] {
        // This is what `xlings update` does: rewrite the tree in place, with a
        // floor this binary cannot satisfy.
        makeIndex(idx, kTooNew, "too-new", "unusable-tree");
        return 0;
    }, out);

    EXPECT_EQ(rc, 0);
    ASSERT_EQ(out.rolledBack.size(), 1u) << "the refresh made the index unusable "
                                            "and the guard did not roll it back";
    EXPECT_TRUE(out.stillUnusable.empty());

    // The machine still works: the tree on disk is readable and is the old one.
    EXPECT_TRUE(mcpp::pm::index_usable(idx));
    EXPECT_NE(readFile(idx / "pkgs" / "z" / "zlib.lua").find("usable-tree"),
              std::string::npos);
}

// The mirror case: a refresh that keeps the index readable must be left alone,
// and must silently become the new known-good snapshot.
TEST(IndexSnapshot, NormalRefreshIsUntouchedAndBecomesTheNewSnapshot) {
    Tmp t;
    auto data = t.path / "data";
    auto idx  = data / "mcpplibs";
    makeIndex(idx, "0.0.85", "v1", "first");

    GuardOutcome out;
    guarded_refresh(data, [&] {
        makeIndex(idx, "0.0.85", "v2", "second");
        return 0;
    }, out);

    EXPECT_FALSE(out.degraded()) << "a healthy refresh must not report anything";
    EXPECT_NE(readFile(idx / "pkgs" / "z" / "zlib.lua").find("second"),
              std::string::npos);
    // Both the pre- and post-refresh trees are now archived, so the next
    // refresh has somewhere to fall back to.
    EXPECT_EQ(list_snapshots(data, idx).size(), 2u);
}

// Already-unusable on entry: there is nothing to "roll back" to, but a local
// snapshot from before the floor moved is still the right answer.
TEST(IndexSnapshot, RecoversFromLocalHistoryWhenAlreadyUnusable) {
    Tmp t;
    auto data = t.path / "data";
    auto idx  = data / "mcpplibs";

    // A good tree was seen at some point in the past...
    makeIndex(idx, "0.0.85", "old-good", "known-good");
    ASSERT_TRUE(archive(data, idx));
    // ...and the current tree is already too new (e.g. mcpp was downgraded, or
    // the guard was introduced after the damage was done).
    makeIndex(idx, kTooNew, "too-new", "unusable");

    GuardOutcome out;
    guarded_refresh(data, [&] { return 0; }, out);   // refresh changes nothing

    ASSERT_EQ(out.recovered.size(), 1u);
    EXPECT_TRUE(out.rolledBack.empty());
    EXPECT_TRUE(mcpp::pm::index_usable(idx));
    EXPECT_NE(readFile(idx / "pkgs" / "z" / "zlib.lua").find("known-good"),
              std::string::npos);
}

// No local history and a too-new tree is the one case nothing can rescue. It
// must be reported honestly rather than silently left looking fine.
TEST(IndexSnapshot, ReportsStillUnusableWhenNoSnapshotCanHelp) {
    Tmp t;
    auto data = t.path / "data";
    auto idx  = data / "mcpplibs";
    makeIndex(idx, kTooNew, "too-new", "unusable");

    GuardOutcome out;
    guarded_refresh(data, [&] { return 0; }, out);

    ASSERT_EQ(out.stillUnusable.size(), 1u);
    EXPECT_TRUE(out.rolledBack.empty());
    EXPECT_TRUE(out.recovered.empty());
}

TEST(IndexSnapshot, PruneKeepsTheNewest) {
    Tmp t;
    auto data = t.path / "data";
    auto idx  = data / "mcpplibs";
    for (int i = 0; i < 4; ++i) {
        makeIndex(idx, "0.0.85", std::format("v{}", i), std::format("gen{}", i));
        ASSERT_TRUE(archive(data, idx));
    }
    ASSERT_EQ(list_snapshots(data, idx).size(), 4u);
    prune(data, idx, /*keep=*/2);
    EXPECT_EQ(list_snapshots(data, idx).size(), 2u);
}

// A tree with no index.toml declares no contract, so it is usable by anyone.
// Third-party indexes rely on this and must never be rolled back.
TEST(IndexSnapshot, TreeWithoutAContractIsAlwaysUsable) {
    Tmp t;
    auto data = t.path / "data";
    auto idx  = data / "third-party";
    makeIndex(idx, "", "v1", "no-contract");
    EXPECT_TRUE(mcpp::pm::index_usable(idx));

    GuardOutcome out;
    guarded_refresh(data, [&] { return 0; }, out);
    EXPECT_FALSE(out.degraded());
}
