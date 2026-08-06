#include <gtest/gtest.h>

import std;
import mcpp.ide.configure;
import mcpp.ide.model;
import mcpp.libs.json;

TEST(IdeConfigure, EmitsConfiguredEventsWithCdbBeforeBuild) {
    mcpp::ide::ConfigureResult result{
        .projectId = "project-fnv1a64:abcd",
        .configurationId = "config-fnv1a64:1234",
        .snapshotId = "snapshot-fnv1a64:5678",
        .projectRoot = "/workspace/app",
        .compileCommands = "/workspace/app/.mcpp/ide/replies/compile_commands.json",
        .compatibilityCompileCommands = "/workspace/app/compile_commands.json",
        .toolchain = "llvm@22",
        .toolchainFingerprint = "fp",
        .compileCommandCount = 2,
        .stdModule = "/cache/std/pcm.cache/std.pcm",
        .stdModuleState = "pending",
    };

    auto events = mcpp::ide::configure_events(result);
    ASSERT_EQ(events.size(), 3u);
    auto started = nlohmann::json::parse(events[0]);
    auto published = nlohmann::json::parse(events[1]);
    auto finished = nlohmann::json::parse(events[2]);
    EXPECT_EQ(started["type"], "operation-started");
    EXPECT_EQ(published["type"], "snapshot-published");
    EXPECT_EQ(published["phase"], "configured");
    EXPECT_EQ(published["compileCommandCount"], 2);
    EXPECT_EQ(published["stdModule"]["state"], "pending");
    EXPECT_EQ(finished["type"], "operation-finished");
    EXPECT_EQ(finished["status"], "success");
}
