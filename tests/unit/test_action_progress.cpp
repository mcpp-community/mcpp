#include <gtest/gtest.h>
import std;
import mcpp.build.action_progress;
namespace telemetry = mcpp::build::action_progress;

TEST(ActionProgress, AcceptsUnknownTotalsAndExplicitCounts) {
    const auto unknown = telemetry::parse(R"({"v":1,"seq":0,"phase":"configure","detail":"compiler checks"})");
    ASSERT_TRUE(unknown);
    EXPECT_FALSE(unknown->total);
    EXPECT_EQ(telemetry::describe(*unknown), "configure · compiler checks");
    const auto known = telemetry::parse(R"({"v":1,"seq":7,"phase":"compile","done":3,"total":9})");
    ASSERT_TRUE(known);
    EXPECT_EQ(telemetry::describe(*known), "compile 3/9");
}
TEST(ActionProgress, RejectsInvalidSnapshotsWithoutInferringSuccess) {
    for (const auto text : {
        R"({"v":2,"seq":1,"phase":"compile"})",
        R"({"v":1,"seq":-1,"phase":"compile"})",
        R"({"v":1,"seq":1,"phase":""})",
        R"({"v":1,"seq":1,"phase":"compile","done":4,"total":3})",
        R"({"v":1,"seq":1,"phase":"compile","done":1.5})",
        R"({"v":1,"seq":1,"phase":"compile\nforged"})",
        R"({"v":1,"seq":1,"phase":"compile","detail":"\u001b[31m"})",
        R"({"v":1,"seq":1,"phase":"compile")"})
        EXPECT_FALSE(telemetry::parse(text)) << text;
    EXPECT_FALSE(telemetry::parse(std::string(telemetry::kMaxBytes + 1, ' ')));
    // Finishing internal work is data, not a successful command or a stamp.
    EXPECT_TRUE(telemetry::parse(R"({"v":1,"seq":2,"phase":"link","done":1,"total":1})"));
}
TEST(ActionProgress, AllocatesIndependentRunDirectoriesAndBoundsReads) {
    const auto base = std::filesystem::temp_directory_path();
    const auto first = telemetry::make_directory(base);
    const auto second = telemetry::make_directory(base);
    ASSERT_FALSE(first.empty()); ASSERT_FALSE(second.empty());
    EXPECT_NE(first, second);
    const auto a = telemetry::channel(first, "obj/a.o");
    EXPECT_NE(a, telemetry::channel(first, "obj/b.o"));
    EXPECT_NE(a, telemetry::channel(second, "obj/a.o"));
    std::ofstream(a) << std::string(telemetry::kMaxBytes + 1, ' ');
    EXPECT_FALSE(telemetry::read(a));
    std::ofstream(a) << R"({"v":1,"seq":0,"phase":"work"})";
    EXPECT_TRUE(telemetry::read(a));
    std::filesystem::remove_all(first); std::filesystem::remove_all(second);
}
