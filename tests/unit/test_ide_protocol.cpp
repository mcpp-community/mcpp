#include <gtest/gtest.h>

import std;
import mcpp.ide.events;
import mcpp.ide.model;

TEST(IdeProtocol, WireNamesCoverLifecycleStates) {
    using namespace mcpp::ide;
    EXPECT_EQ(wire_name(IdePhase::Declared), "declared");
    EXPECT_EQ(wire_name(IdePhase::Configured), "configured");
    EXPECT_EQ(wire_name(IdePhase::Ready), "ready");
    EXPECT_EQ(wire_name(IdePhase::Stale), "stale");
    EXPECT_EQ(wire_name(IdePhase::Unavailable), "unavailable");
}

TEST(IdeProtocol, ConfigurationIdIsStableAndSelectorSensitive) {
    using namespace mcpp::ide;
    ConfigurationSelectors selectors;
    selectors.profile = "dev";
    selectors.target = "aarch64-apple-darwin";
    selectors.features = {"tls", "metrics"};

    const auto first = configuration_id("/workspace/app", selectors, "llvm-22");
    const auto second = configuration_id("/workspace/app", selectors, "llvm-22");
    EXPECT_EQ(first, second);
    EXPECT_TRUE(first.starts_with("config-fnv1a64:"));

    selectors.features.push_back("gui");
    EXPECT_NE(first, configuration_id("/workspace/app", selectors, "llvm-22"));
}

TEST(IdeProtocol, NdjsonRejectsSequenceRegression) {
    using namespace mcpp::ide;
    NdjsonEventParser parser;
    ASSERT_TRUE(parser.consume(R"({"seq":1,"type":"operation-started"})").has_value());
    ASSERT_TRUE(parser.consume(R"({"seq":2,"type":"progress"})").has_value());
    auto result = parser.consume(R"({"seq":2,"type":"diagnostic"})");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "MCPP_IDE_EVENT_SEQUENCE_INVALID");
}

