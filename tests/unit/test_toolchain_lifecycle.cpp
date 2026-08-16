#include <gtest/gtest.h>

import std;
import mcpp.toolchain.lifecycle;

using namespace mcpp::toolchain;

namespace {

// A payload tree with one subdirectory that cannot be enumerated, which is
// the portable way to make `remove_all` fail. On Windows the same failure
// arrives via an open handle (mspdbsrv.exe holding mspdbcore.dll); the CAUSE
// differs per platform, the contract this fixture pins does not.
struct UnremovableTree {
    std::filesystem::path parent;
    std::filesystem::path root;
    std::filesystem::path blocked;

    UnremovableTree() {
        parent = std::filesystem::temp_directory_path()
               / std::format("mcpp-rm-{}",
                             std::chrono::steady_clock::now()
                                 .time_since_epoch().count());
        root = parent / "14.44.35207";
        blocked = root / "bin" / "locked";
        std::filesystem::create_directories(blocked);
        std::ofstream{blocked / "mspdbcore.dll"} << "held";
        std::filesystem::permissions(blocked, std::filesystem::perms::none);
    }
    ~UnremovableTree() {
        std::error_code ec;
        std::filesystem::permissions(blocked, std::filesystem::perms::all, ec);
        for (auto& e : std::filesystem::directory_iterator(parent, ec)) {
            std::filesystem::permissions(e.path() / "bin" / "locked",
                                         std::filesystem::perms::all, ec);
        }
        std::filesystem::remove_all(parent, ec);
    }
    UnremovableTree(const UnremovableTree&) = delete;
    UnremovableTree& operator=(const UnremovableTree&) = delete;

    std::vector<std::filesystem::path> parked() const {
        std::vector<std::filesystem::path> out;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(parent, ec))
            if (e.path().filename().string().starts_with(".trash-"))
                out.push_back(e.path());
        return out;
    }
};

} // namespace

TEST(ToolchainRemove, AHeldPayloadStillLeavesItsLocation) {
    // What `toolchain remove` promises is that the toolchain stops being
    // installed — not that every byte is already gone. A payload whose files
    // are still open cannot be deleted, but it CAN be moved: Windows refuses
    // to delete a directory containing an open file and allows renaming one.
    //
    // Before this, remove reported "Access is denied" and left the toolchain
    // exactly where it was, seconds after a build with that same toolset —
    // which is the normal case, not a corner one, because /Zi leaves
    // mspdbsrv.exe running inside the payload being removed.
    UnremovableTree t;
    std::error_code ec;

    ASSERT_FALSE(std::filesystem::remove_all(t.root, ec) && !ec)
        << "fixture is not actually unremovable; the test would prove nothing";

    ec.clear();
    EXPECT_TRUE(remove_payload_tree(t.root, ec))
        << "a held payload was reported as un-removable";
    EXPECT_FALSE(std::filesystem::exists(t.root))
        << "the toolchain is still at the path it was removed from";
}

TEST(ToolchainRemove, ParkedBytesAreSweptOnceNothingHoldsThem) {
    // The other half of the promise: parking is a deferral, not a leak. The
    // next lifecycle command sweeps, and by then the process that held the
    // files has exited — modelled here by dropping the permission block.
    UnremovableTree t;
    std::error_code ec;
    remove_payload_tree(t.root, ec);

    auto parked = t.parked();
    ASSERT_EQ(parked.size(), 1u) << "expected exactly one parked payload";

    std::filesystem::permissions(parked[0] / "bin" / "locked",
                                 std::filesystem::perms::all, ec);
    sweep_parked_payloads(t.parent);
    EXPECT_TRUE(t.parked().empty()) << "parked payload was never swept";
}

TEST(ToolchainRemove, AnOrdinaryPayloadIsJustDeleted) {
    // The common path must not grow a `.trash-` directory: parking is the
    // fallback, and a fallback that fires always is not a fallback.
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp-rm-ok-{}",
                           std::chrono::steady_clock::now()
                               .time_since_epoch().count());
    std::filesystem::create_directories(dir / "14.44.35207" / "bin");
    std::ofstream{dir / "14.44.35207" / "bin" / "cl.exe"} << "not a compiler";

    std::error_code ec;
    EXPECT_TRUE(remove_payload_tree(dir / "14.44.35207", ec));
    EXPECT_FALSE(std::filesystem::exists(dir / "14.44.35207"));

    bool anyParked = false;
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        anyParked |= e.path().filename().string().starts_with(".trash-");
    EXPECT_FALSE(anyParked) << "a deletable payload was parked instead of deleted";

    std::filesystem::remove_all(dir, ec);
}
