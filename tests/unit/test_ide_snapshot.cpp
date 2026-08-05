#include <gtest/gtest.h>

import std;
import mcpp.ide.model;

TEST(IdeSnapshotModel, WireNamesAreStable) {
    using namespace mcpp::ide;
    EXPECT_EQ(wire_name(SnapshotState::Partial), "partial");
    EXPECT_EQ(wire_name(SnapshotState::Stale), "stale");
    EXPECT_EQ(wire_name(SnapshotState::Unavailable), "unavailable");
    EXPECT_EQ(wire_name(SnapshotState::Ready), "ready");
    EXPECT_EQ(wire_name(ArtifactState::Missing), "missing");
    EXPECT_EQ(wire_name(ArtifactState::Stale), "stale");
    EXPECT_EQ(wire_name(Severity::Warning), "warning");
    EXPECT_EQ(wire_name(Severity::Error), "error");
}
