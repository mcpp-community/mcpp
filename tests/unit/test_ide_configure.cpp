#include <gtest/gtest.h>

import std;
import mcpp.build.plan;
import mcpp.ide.configure;
import mcpp.ide.model;
import mcpp.ide.publish;
import mcpp.libs.json;
import mcpp.platform.fs;
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

    auto events = mcpp::ide::configure_events(result, "operation-fnv1a64:test");
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

TEST(IdeConfigure, ContinuesLifecycleAfterEarlyStartedEvent) {
    mcpp::ide::ConfigureResult result{
        .projectId = "project-fnv1a64:abcd",
        .configurationId = "config-fnv1a64:1234",
        .snapshotId = "snapshot-fnv1a64:5678",
    };

    auto events = mcpp::ide::configure_events(result, "operation-fnv1a64:test",
                                               /*firstSeq=*/2,
                                               /*includeStarted=*/false);
    ASSERT_EQ(events.size(), 2u);
    auto published = nlohmann::json::parse(events[0]);
    auto finished = nlohmann::json::parse(events[1]);
    EXPECT_EQ(published["seq"], 2);
    EXPECT_EQ(published["type"], "snapshot-published");
    EXPECT_EQ(finished["seq"], 3);
    EXPECT_EQ(finished["type"], "operation-finished");
}

TEST(IdeConfigure, CorrelatesEveryLifecycleEventWithOperationId) {
    constexpr std::string_view operationId = "operation-fnv1a64:abcd";
    mcpp::ide::ConfigureResult result{
        .projectId = "project-fnv1a64:abcd",
        .configurationId = "config-fnv1a64:1234",
        .snapshotId = "snapshot-fnv1a64:5678",
    };

    auto started = nlohmann::json::parse(
        mcpp::ide::configure_started_event(operationId));
    auto success = mcpp::ide::configure_events(
        result, operationId, /*firstSeq=*/2, /*includeStarted=*/false);
    auto failure = mcpp::ide::configure_failure_events(
        "cannot resolve toolchain", operationId, /*firstSeq=*/2);

    // 显式提取字符串，避免 Windows Clang 在 json 与 string_view 间选择
    // operator== 时产生二义性。
    EXPECT_EQ(started["operationId"].get<std::string>(), std::string(operationId));
    for (const auto& line : success)
        EXPECT_EQ(nlohmann::json::parse(line)["operationId"].get<std::string>(),
                  std::string(operationId));
    for (const auto& line : failure)
        EXPECT_EQ(nlohmann::json::parse(line)["operationId"].get<std::string>(),
                  std::string(operationId));
}

TEST(IdeConfigure, GeneratesDistinctOperationIds) {
    const auto first = mcpp::ide::new_operation_id();
    const auto second = mcpp::ide::new_operation_id();
    EXPECT_TRUE(first.starts_with("operation-fnv1a64:"));
    EXPECT_NE(first, second);
}

TEST(IdeConfigure, FinishesFailedLifecycleAfterDiagnostic) {
    auto events = mcpp::ide::configure_failure_events("cannot resolve toolchain",
                                                       "operation-fnv1a64:test",
                                                       /*firstSeq=*/2);
    ASSERT_EQ(events.size(), 2u);
    auto diagnostic = nlohmann::json::parse(events[0]);
    auto finished = nlohmann::json::parse(events[1]);
    EXPECT_EQ(diagnostic["seq"], 2);
    EXPECT_EQ(diagnostic["type"], "diagnostic");
    EXPECT_EQ(diagnostic["diagnostic"]["code"], "MCPP_IDE_CONFIGURE_FAILED");
    EXPECT_EQ(finished["seq"], 3);
    EXPECT_EQ(finished["type"], "operation-finished");
    EXPECT_EQ(finished["status"], "failed");
    EXPECT_EQ(finished["diagnosticCodes"],
              nlohmann::json::array({"MCPP_IDE_CONFIGURE_FAILED"}));
}

TEST(IdeConfigure, SnapshotIdentityIncludesResolvedBuildFingerprint) {
    const auto first = mcpp::ide::configured_snapshot_id(
        "config-fnv1a64:1234", "build-fingerprint-a", "cdb-hash");
    EXPECT_TRUE(first.starts_with("snapshot-fnv1a64:"));
    EXPECT_NE(first, mcpp::ide::configured_snapshot_id(
                         "config-fnv1a64:1234", "build-fingerprint-b", "cdb-hash"));
    EXPECT_NE(first, mcpp::ide::configured_snapshot_id(
                         "config-fnv1a64:1234", "build-fingerprint-a", "other-cdb"));
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

TEST(IdeConfigure, KeepsFingerprintScopedStagedFileWhenSizeMatches) {
    const auto root = temp_root();
    const auto source = root / "cache" / "dep.pcm";
    const auto destination = root / "target" / "pcm.cache" / "dep.pcm";
    std::filesystem::create_directories(source.parent_path());
    std::filesystem::create_directories(destination.parent_path());
    std::ofstream(source) << "new!";
    std::ofstream(destination) << "old!";

    mcpp::build::BuildPlan plan;
    plan.outputDir = root / "target";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    plan.compileUnits.push_back({
        .source = "/deps/dep.cppm",
        .object = "obj/dep.m.o",
        .providesModule = "dep",
        .servedFromCache = true,
        .cachedBmi = source,
    });

    auto staged = mcpp::ide::stage_cached_module_prerequisites(plan);
    ASSERT_TRUE(staged.has_value()) << staged.error();
    std::ifstream input(destination);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "old!");

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

TEST(IdeConfigure, CanonicalizesWorkspaceAndMemberIdentity) {
    const auto root = temp_root();
    const auto workspace = root / "workspace";
    const auto member = workspace / "libs" / "server";
    std::filesystem::create_directories(member);

    const auto normalized = mcpp::ide::physical_project_root(workspace / "libs" / "..");
    EXPECT_EQ(normalized, mcpp::ide::physical_project_root(workspace));
    EXPECT_EQ(mcpp::ide::canonical_member_selector(workspace, member),
              std::optional<std::string>("libs/server"));
    EXPECT_EQ(mcpp::ide::canonical_member_selector(workspace, workspace), std::nullopt);
    EXPECT_EQ(mcpp::ide::project_id(workspace / "libs" / ".."),
              mcpp::ide::project_id(workspace));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(IdeConfigure, ConvertsThrownExceptionToExpectedFailure) {
    auto result = mcpp::ide::run_configure_safely([]()
        -> std::expected<mcpp::ide::ConfigureResult, std::string> {
        throw std::runtime_error("synthetic configure failure");
    });

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("synthetic configure failure"), std::string::npos);
}

TEST(IdeConfigure, PublishesRecoverableConfiguredSnapshot) {
    const auto root = temp_root();
    std::filesystem::create_directories(root / ".mcpp" / "ide" / "replies");
    const auto cdb = root / ".mcpp" / "ide" / "replies" / "compile_commands-a.json";
    std::ofstream(cdb) << "[]\n";

    mcpp::ide::PublishedSnapshot snapshot{
        .projectId = "project-1",
        .configurationId = "config-1",
        .snapshotId = "snapshot-1",
        .phase = "configured",
        .projectRoot = root,
        .compileCommands = cdb,
        .compatibilityCompileCommands = root / "compile_commands.json",
        .toolchain = "llvm@22",
        .toolchainFingerprint = "toolchain-1",
    };
    auto published = mcpp::ide::publish_current_snapshot(root, snapshot);
    ASSERT_TRUE(published.has_value()) << published.error();

    auto loaded = mcpp::ide::read_current_snapshot(root);
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    ASSERT_TRUE(loaded->has_value());
    EXPECT_EQ((*loaded)->snapshotId, snapshot.snapshotId);
    EXPECT_EQ((*loaded)->configurationId, snapshot.configurationId);
    EXPECT_EQ((*loaded)->compileCommands, cdb);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(IdeConfigure, FailedPublicationKeepsCurrentSnapshot) {
    const auto root = temp_root();
    std::filesystem::create_directories(root / ".mcpp" / "ide" / "replies");
    const auto oldCdb = root / ".mcpp" / "ide" / "replies" / "compile_commands-old.json";
    std::ofstream(oldCdb) << "[]\n";
    auto first = mcpp::ide::publish_current_snapshot(root, {
        .projectId = "project-1",
        .configurationId = "config-1",
        .snapshotId = "snapshot-old",
        .phase = "configured",
        .projectRoot = root,
        .compileCommands = oldCdb,
        .compatibilityCompileCommands = root / "compile_commands.json",
        .toolchain = "llvm@22",
        .toolchainFingerprint = "toolchain-1",
    });
    ASSERT_TRUE(first.has_value()) << first.error();

    auto failed = mcpp::ide::publish_current_snapshot(root, {
        .projectId = "project-1",
        .configurationId = "config-1",
        .snapshotId = "snapshot-new",
        .phase = "configured",
        .projectRoot = root,
        .compileCommands = root / "missing.json",
        .compatibilityCompileCommands = root / "compile_commands.json",
        .toolchain = "llvm@22",
        .toolchainFingerprint = "toolchain-1",
    });
    ASSERT_FALSE(failed.has_value());

    auto current = mcpp::ide::read_current_snapshot(root);
    ASSERT_TRUE(current.has_value()) << current.error();
    ASSERT_TRUE(current->has_value());
    EXPECT_EQ((*current)->snapshotId, "snapshot-old");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(IdeConfigure, MetadataPublicationFailureRestoresCompatibilityCdb) {
    const auto root = temp_root();
    const auto replies = root / ".mcpp" / "ide" / "replies";
    std::filesystem::create_directories(replies);
    const auto snapshotCdb = replies / "compile_commands-new.json";
    const auto compatibilityCdb = root / "compile_commands.json";
    const std::string oldContent = R"([{"file":"old.cpp"}])";
    const std::string newContent = R"([{"file":"new.cpp"}])";
    std::ofstream(snapshotCdb) << newContent;
    std::ofstream(compatibilityCdb) << oldContent;

    // 用目录占据最终 metadata 路径，跨平台稳定制造第二个切换点失败。
    std::filesystem::create_directories(root / ".mcpp" / "ide" / "current.json");
    auto published = mcpp::ide::publish_configured_snapshot(root, {
        .projectId = "project-1",
        .configurationId = "config-1",
        .snapshotId = "snapshot-new",
        .phase = "configured",
        .projectRoot = root,
        .compileCommands = snapshotCdb,
        .compatibilityCompileCommands = compatibilityCdb,
        .toolchain = "llvm@22",
        .toolchainFingerprint = "toolchain-1",
    }, newContent);

    ASSERT_FALSE(published.has_value());
    std::ifstream restored(compatibilityCdb, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), oldContent);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(IdeConfigure, ConcurrentPublicationCannotChangeCompatibilityCdb) {
    const auto root = temp_root();
    const auto replies = root / ".mcpp" / "ide" / "replies";
    std::filesystem::create_directories(replies);
    const auto snapshotCdb = replies / "compile_commands-new.json";
    const auto compatibilityCdb = root / "compile_commands.json";
    const std::string oldContent = R"([{"file":"old.cpp"}])";
    const std::string newContent = R"([{"file":"new.cpp"}])";
    std::ofstream(snapshotCdb) << newContent;
    std::ofstream(compatibilityCdb) << oldContent;
    auto held = mcpp::platform::fs::FileLock::try_acquire(root / ".mcpp" / "ide");
    ASSERT_TRUE(held.has_value());

    auto published = mcpp::ide::publish_configured_snapshot(root, {
        .projectId = "project-1",
        .configurationId = "config-1",
        .snapshotId = "snapshot-new",
        .phase = "configured",
        .projectRoot = root,
        .compileCommands = snapshotCdb,
        .compatibilityCompileCommands = compatibilityCdb,
        .toolchain = "llvm@22",
        .toolchainFingerprint = "toolchain-1",
    }, newContent);

    ASSERT_FALSE(published.has_value());
    EXPECT_NE(published.error().find("already in progress"), std::string::npos);
    std::ifstream unchanged(compatibilityCdb, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(unchanged), {}), oldContent);
    held.reset();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(IdeConfigure, RejectsSnapshotCdbOutsideProject) {
    const auto root = temp_root();
    const auto outside = root.parent_path() / std::format(
        "mcpp_outside_cdb_{}.json",
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    std::ofstream(outside) << "[]\n";

    auto published = mcpp::ide::publish_current_snapshot(root, {
        .projectId = "project-1",
        .configurationId = "config-1",
        .snapshotId = "snapshot-1",
        .phase = "configured",
        .projectRoot = root,
        .compileCommands = outside,
        .compatibilityCompileCommands = root / "compile_commands.json",
        .toolchain = "llvm@22",
        .toolchainFingerprint = "toolchain-1",
    });

    EXPECT_FALSE(published.has_value());
    EXPECT_FALSE(std::filesystem::exists(root / ".mcpp" / "ide" / "current.json"));
    std::error_code ec;
    std::filesystem::remove(outside, ec);
    std::filesystem::remove_all(root, ec);
}

TEST(IdeConfigure, InvalidCurrentMetadataReturnsError) {
    const auto root = temp_root();
    std::filesystem::create_directories(root / ".mcpp" / "ide");
    std::ofstream(root / ".mcpp" / "ide" / "current.json")
        << R"({"schemaVersion":"wrong","kind":17})";

    auto current = mcpp::ide::read_current_snapshot(root);

    EXPECT_FALSE(current.has_value());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(IdeConfigure, CurrentMetadataCannotReferenceCdbOutsideProject) {
    const auto root = temp_root();
    const auto outside = root.parent_path() / std::format(
        "mcpp_external_current_cdb_{}.json",
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root / ".mcpp" / "ide");
    std::ofstream(outside) << "[]\n";
    std::ofstream(root / ".mcpp" / "ide" / "current.json")
        << nlohmann::ordered_json{
            {"schemaVersion", 1},
            {"kind", "mcpp.ide.configured-snapshot"},
            {"phase", "configured"},
            {"projectId", "project-1"},
            {"configurationId", "config-1"},
            {"snapshotId", "snapshot-1"},
            {"projectRoot", root.generic_string()},
            {"compileCommands", outside.generic_string()},
            {"compatibilityCompileCommands",
             (root / "compile_commands.json").generic_string()},
            {"compileCommandCount", 0},
            {"toolchain", "llvm@22"},
            {"toolchainFingerprint", "toolchain-1"},
        }.dump();

    auto current = mcpp::ide::read_current_snapshot(root);

    EXPECT_FALSE(current.has_value());
    if (!current) EXPECT_NE(current.error().find("escapes"), std::string::npos);
    std::error_code ec;
    std::filesystem::remove(outside, ec);
    std::filesystem::remove_all(root, ec);
}
