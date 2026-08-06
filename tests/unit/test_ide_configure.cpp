#include <gtest/gtest.h>

import std;
import mcpp.build.plan;
import mcpp.ide.configure;
import mcpp.ide.model;
import mcpp.libs.json;
import mcpp.toolchain.model;

namespace {

std::filesystem::path temp_root() {
    return std::filesystem::temp_directory_path()
         / std::format("mcpp_ide_configure_{}",
             std::chrono::steady_clock::now().time_since_epoch().count());
}

}  // namespace

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

TEST(IdeConfigure, StagesCachedModulePrerequisitesForPublishedCdb) {
    const auto root = temp_root();
    const auto cache = root / "cache";
    std::filesystem::create_directories(cache);
    std::ofstream(cache / "std.pcm") << "std";
    std::ofstream(cache / "std.compat.pcm") << "std.compat";
    std::ofstream(cache / "dep.pcm") << "dep";

    mcpp::build::BuildPlan plan;
    plan.outputDir = root / "target";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    plan.stdBmiPath = cache / "std.pcm";
    plan.stdCompatBmiPath = cache / "std.compat.pcm";
    plan.compileUnits.push_back({
        .source = "/deps/dep.cppm",
        .object = "obj/dep.m.o",
        .providesModule = "dep",
        .servedFromCache = true,
        .cachedBmi = cache / "dep.pcm",
    });

    auto staged = mcpp::ide::stage_cached_module_prerequisites(plan);
    ASSERT_TRUE(staged.has_value()) << staged.error();
    EXPECT_TRUE(std::filesystem::is_regular_file(
        plan.outputDir / "pcm.cache" / "std.pcm"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        plan.outputDir / "pcm.cache" / "std.compat.pcm"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        plan.outputDir / "pcm.cache" / "dep.pcm"));
    EXPECT_FALSE(std::filesystem::exists(plan.outputDir / "obj"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(IdeConfigure, UsesToolchainSpecificBmiDirectories) {
    const auto root = temp_root();
    const auto cache = root / "cache";
    std::filesystem::create_directories(cache);
    std::ofstream(cache / "dep.pcm") << "dep";
    std::ofstream(cache / "dep.gcm") << "dep";
    std::ofstream(cache / "dep.ifc") << "dep";

    struct Case {
        mcpp::toolchain::CompilerId compiler;
        std::string_view bmiDir;
        std::string_view extension;
    };
    for (const auto& test : {
             Case{mcpp::toolchain::CompilerId::Clang, "pcm.cache", ".pcm"},
             Case{mcpp::toolchain::CompilerId::GCC, "gcm.cache", ".gcm"},
             Case{mcpp::toolchain::CompilerId::MSVC, "ifc.cache", ".ifc"},
         }) {
        mcpp::build::BuildPlan plan;
        plan.outputDir = root / std::string(test.bmiDir);
        plan.toolchain.compiler = test.compiler;
        plan.compileUnits.push_back({
            .source = "/deps/dep.cppm",
            .object = "obj/dep.m.o",
            .providesModule = "dep",
            .servedFromCache = true,
            .cachedBmi = cache / ("dep" + std::string(test.extension)),
        });

        auto staged = mcpp::ide::stage_cached_module_prerequisites(plan);
        ASSERT_TRUE(staged.has_value()) << staged.error();
        EXPECT_TRUE(std::filesystem::is_regular_file(
            plan.outputDir / test.bmiDir / ("dep" + std::string(test.extension))));
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(IdeConfigure, RefusesToPublishWhenCachedModuleIsMissing) {
    const auto root = temp_root();
    mcpp::build::BuildPlan plan;
    plan.outputDir = root / "target";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    plan.compileUnits.push_back({
        .source = "/deps/dep.cppm",
        .object = "obj/dep.m.o",
        .providesModule = "dep",
        .servedFromCache = true,
        .cachedBmi = root / "missing" / "dep.pcm",
    });

    auto staged = mcpp::ide::stage_cached_module_prerequisites(plan);
    ASSERT_FALSE(staged.has_value());
    EXPECT_FALSE(std::filesystem::exists(plan.outputDir));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
