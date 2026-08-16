#include <gtest/gtest.h>

import std;
import mcpp.toolchain.lifecycle;

using namespace mcpp::toolchain;

// WHAT IS AND IS NOT TESTED HERE, because the difference is not a gap.
//
// `remove_payload_tree()` falls back to moving held FILES aside so the tree
// can be deleted around them. That path is reachable only where a file can be
// renamed while something holds it open — which is Windows. On POSIX the two
// permissions are the same one: a file you cannot unlink is a file you cannot
// rename either, because both need write on the parent directory. There is no
// POSIX state that models "held open by another process".
//
// A POSIX fixture cannot stand in for it either, and the reason is worth
// keeping: the only POSIX way to make a file undeletable is to drop write on
// its parent directory — and this function's SECOND pass adds write back
// across the tree, on purpose, because that is exactly the read-only-payload
// case it must fix. The fixture therefore becomes removable the moment the
// code under test touches it. That is the function working, not a hole.
//
// So the held-file path is gated by e2e 239 on the Windows runner, and what
// is pinned here is what must hold on every platform: the ordinary deletion
// takes the ordinary route, and the sweep is precise about what it eats.
// Nothing here asserts something this platform cannot express.

TEST(ToolchainRemove, AnOrdinaryPayloadIsJustDeleted) {
    // The common path must not grow a `.trash-` directory: moving files aside
    // is the fallback, and a fallback that fires always is not a fallback.
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp-rm-ok-{}", std::chrono::steady_clock::now()
                                                .time_since_epoch().count());
    std::filesystem::create_directories(dir / "14.44.35207" / "bin");
    std::ofstream{dir / "14.44.35207" / "bin" / "cl.exe"} << "not a compiler";

    std::error_code ec;
    EXPECT_TRUE(remove_payload_tree(dir / "14.44.35207", ec));
    EXPECT_FALSE(std::filesystem::exists(dir / "14.44.35207"));

    bool anyTrash = false;
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        anyTrash |= e.path().filename().string().starts_with(".trash-");
    EXPECT_FALSE(anyTrash) << "a deletable payload was parked instead of deleted";

    std::filesystem::remove_all(dir, ec);
}

TEST(ToolchainRemove, TheSweepDeletesParkedPayloadsAndNothingElse) {
    // The sweep runs before every install and remove, so it must be precise:
    // `.trash-*` goes, an installed version directory beside it stays.
    auto pkgRoot = std::filesystem::temp_directory_path()
                 / std::format("mcpp-sweep-{}", std::chrono::steady_clock::now()
                                                    .time_since_epoch().count());
    std::filesystem::create_directories(pkgRoot / ".trash-14.44.35207-1" / "sub");
    std::ofstream{pkgRoot / ".trash-14.44.35207-1" / "sub" / "held.dll"} << "x";
    std::filesystem::create_directories(pkgRoot / "14.44.35207" / "bin");
    std::ofstream{pkgRoot / "14.44.35207" / "bin" / "cl.exe"} << "keep me";

    sweep_parked_payloads(pkgRoot);

    EXPECT_FALSE(std::filesystem::exists(pkgRoot / ".trash-14.44.35207-1"))
        << "parked payload was not swept";
    EXPECT_TRUE(std::filesystem::exists(pkgRoot / "14.44.35207" / "bin" / "cl.exe"))
        << "the sweep deleted an installed toolset";

    std::error_code ec;
    std::filesystem::remove_all(pkgRoot, ec);
}
