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

    selectors.features.pop_back();
    const auto cxx23 = configuration_id("/workspace/app", selectors, "llvm-22");
    selectors.cppStandard = "c++20";
    EXPECT_NE(cxx23, configuration_id("/workspace/app", selectors, "llvm-22"));
}

TEST(IdeProtocol, NdjsonRejectsSequenceRegression) {
    using namespace mcpp::ide;
    NdjsonEventParser parser;
    ASSERT_TRUE(parser.consume(
        R"({"schemaVersion":1,"seq":1,"type":"operation-started","operationId":"operation:1","operation":"configure"})")
                    .has_value());
    ASSERT_TRUE(parser.consume(
        R"({"schemaVersion":1,"seq":2,"type":"progress","operationId":"operation:1","phase":"resolve","completed":1,"total":2})")
                    .has_value());
    auto result = parser.consume(
        R"({"schemaVersion":1,"seq":2,"type":"diagnostic","operationId":"operation:1"})");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "MCPP_IDE_EVENT_SEQUENCE_INVALID");
}

TEST(IdeProtocol, NdjsonRequiresAndPreservesOperationIdentity) {
    using namespace mcpp::ide;
    NdjsonEventParser parser;

    auto missingSchema = parser.consume(
        R"({"seq":1,"type":"operation-started","operationId":"operation:1"})");
    ASSERT_FALSE(missingSchema.has_value());
    EXPECT_EQ(missingSchema.error(), "MCPP_IDE_EVENT_INVALID");

    auto missingOperation = parser.consume(
        R"({"schemaVersion":1,"seq":1,"type":"operation-started"})");
    ASSERT_FALSE(missingOperation.has_value());
    EXPECT_EQ(missingOperation.error(), "MCPP_IDE_EVENT_INVALID");

    auto valid = parser.consume(
        R"({"schemaVersion":1,"seq":1,"type":"operation-started","operationId":"operation:1","operation":"configure"})");
    ASSERT_TRUE(valid.has_value());
    ASSERT_TRUE(valid->has_value());
    EXPECT_EQ(valid->value().operationId, "operation:1");
}

TEST(IdeProtocol, NdjsonValidatesPayloadRequiredByEventType) {
    using namespace mcpp::ide;
    const auto expect_invalid = [](std::string_view line) {
        NdjsonEventParser parser;
        auto result = parser.consume(line);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), "MCPP_IDE_EVENT_INVALID");
    };

    expect_invalid(
        R"({"schemaVersion":1,"seq":1,"type":"operation-started","operationId":"operation:1"})");
    expect_invalid(
        R"({"schemaVersion":1,"seq":1,"type":"progress","operationId":"operation:1"})");
    expect_invalid(
        R"({"schemaVersion":1,"seq":1,"type":"diagnostic","operationId":"operation:1"})");
    expect_invalid(
        R"({"schemaVersion":1,"seq":1,"type":"snapshot-published","operationId":"operation:1"})");
    expect_invalid(
        R"({"schemaVersion":1,"seq":1,"type":"operation-finished","operationId":"operation:1"})");
}

TEST(IdeProtocol, NdjsonAcceptsCompletePayloadForEveryEventType) {
    using namespace mcpp::ide;
    NdjsonEventParser parser;
    const std::array<std::string_view, 5> lines = {
        R"({"schemaVersion":1,"seq":1,"type":"operation-started","operationId":"operation:1","operation":"configure"})",
        R"({"schemaVersion":1,"seq":2,"type":"progress","operationId":"operation:1","phase":"resolve","completed":2,"total":5})",
        R"({"schemaVersion":1,"seq":3,"type":"diagnostic","operationId":"operation:1","diagnostic":{}})",
        R"({"schemaVersion":1,"seq":4,"type":"snapshot-published","operationId":"operation:1","phase":"configured","snapshotId":"snapshot:1","compileCommands":"/tmp/compile_commands.json"})",
        R"({"schemaVersion":1,"seq":5,"type":"operation-finished","operationId":"operation:1","operation":"configure","status":"success"})",
    };

    for (std::uint64_t seq = 1; seq <= lines.size(); ++seq) {
        auto result = parser.consume(lines[seq - 1]);
        ASSERT_TRUE(result.has_value()) << result.error();
        ASSERT_TRUE(result->has_value());
        EXPECT_EQ(result->value().seq, seq);
        EXPECT_EQ(result->value().operationId, "operation:1");
    }
}
