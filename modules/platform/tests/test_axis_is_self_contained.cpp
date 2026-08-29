#include <gtest/gtest.h>

import std;
import mcpp.platform.axis;

// SUBSYSTEM-LEVEL. This package imports nothing from mcpp, so these tests can
// only see what the package itself promises — which is the point. The
// engine-side tests in the root `tests/unit/` exercise the same vocabulary
// through resolution, so a failure here means the vocabulary changed, while a
// failure there means a consumer was relying on more than it was told.

TEST(PlatformAxis, TheHostPlatformKeyIsNeverEmpty) {
    // Every descriptor lookup is keyed on this. An empty key would not fail —
    // it would match the section named "", which no descriptor has, and the
    // package would simply appear to be unavailable everywhere.
    auto host = mcpp::platform::HostPlatform::current();
    EXPECT_FALSE(host.key().empty());
}

TEST(PlatformAxis, TwoKeysCompareByValue) {
    // `PlatformKey` is only obtainable through an axis type, so possession of
    // one already says which axis it came from. Equality therefore has to be
    // about the key and not the object.
    auto a = mcpp::platform::HostPlatform::current();
    auto b = mcpp::platform::HostPlatform::current();
    EXPECT_TRUE(a == b);
}
