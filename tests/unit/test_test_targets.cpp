#include <gtest/gtest.h>

import std;
import mcpp.build.test_targets;

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        static std::atomic_uint64_t sequence{0};
        auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_test_targets_{}_{}", timestamp,
                           sequence.fetch_add(1, std::memory_order_relaxed));
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_file(const std::filesystem::path& path, std::string_view body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << body;
}

void write_manifest(const std::filesystem::path& root, std::string_view body) {
    write_file(root / "mcpp.toml", body);
}

TEST(TestTargets, NestedNamesUseTestsRelativePath) {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"
)");
    write_file(tmp.path / "tests/tagged/nested/smoke.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->targets.size(), 1u);
    EXPECT_EQ(result->targets[0].name, "tagged/nested/smoke");
    EXPECT_EQ(std::filesystem::path(result->targets[0].main),
              std::filesystem::path("tests") / "tagged" / "nested" / "smoke.cpp");
}

TEST(TestTargets, GlobFlagsBecomePerTargetFlags) {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"

[build]
flags = [{ glob = "tests/**/*.cpp", defines = ["TEST_FEATURE"], cflags = ["-Wall"], cxxflags = ["-Wextra"] }]
)");
    write_file(tmp.path / "tests/main.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->targets.size(), 1u);
    EXPECT_EQ(result->targets[0].defines, std::vector<std::string>{"TEST_FEATURE"});
    EXPECT_EQ(result->targets[0].cflags, std::vector<std::string>{"-Wall"});
    EXPECT_EQ(result->targets[0].cxxflags, std::vector<std::string>{"-Wextra"});
}

TEST(TestTargets, PackageFilterScopesWorkspaceMember) {
    Tmp tmp;
    write_manifest(tmp.path, R"([workspace]
members = ["a", "b"]
)");
    write_manifest(tmp.path / "a", R"([package]
name = "a"
version = "0.1.0"
)");
    write_manifest(tmp.path / "b", R"([package]
name = "b"
version = "0.1.0"
)");
    write_file(tmp.path / "a/tests/main.cpp", "int main() {}\n");
    write_file(tmp.path / "b/tests/main.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, "a");
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->packageRoot, tmp.path / "a");
    ASSERT_EQ(result->targets.size(), 1u);
    EXPECT_EQ(std::filesystem::path(result->targets[0].main),
              std::filesystem::path("tests") / "main.cpp");
}

TEST(TestTargets, DirectorySymlinkKeepsLexicalPackageBoundary) {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"
)");

    Tmp outside;
    write_file(outside.path / "external.cpp", "int main() {}\n");
    std::filesystem::create_directories(tmp.path / "tests");
    std::error_code ec;
    std::filesystem::create_directory_symlink(
        outside.path, tmp.path / "tests/vendor", ec);
    if (ec) GTEST_SKIP() << "directory symlink unavailable: " << ec.message();

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->targets.size(), 1u);
    EXPECT_EQ(result->targets[0].name, "vendor/external");
    EXPECT_EQ(std::filesystem::path(result->targets[0].main),
              std::filesystem::path("tests") / "vendor" / "external.cpp");
}

// `[test] discover` (#634 A5): names are relative to the fixed directory of
// the glob that found the file, `main` stays relative to the package root.
TEST(TestTargets, DiscoverNamesAreRelativeToTheGlobsDirectory) {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"

[test]
discover = ["checks/**/*.cpp"]
)");
    write_file(tmp.path / "checks/deep/a.cpp", "int main() {}\n");
    write_file(tmp.path / "tests/ignored.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(result->discoverDeclared);
    ASSERT_EQ(result->targets.size(), 1u);
    EXPECT_EQ(result->targets[0].name, "deep/a");
    EXPECT_EQ(std::filesystem::path(result->targets[0].main),
              std::filesystem::path("checks") / "deep" / "a.cpp");
}

// An exclusion removes a file whichever positive glob found it, and a glob
// with no fixed directory names files relative to the package root.
TEST(TestTargets, DiscoverExclusionsApplyAcrossGlobs) {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"

[test]
discover = ["tests/**/*.cpp", "*_check.cpp", "!tests/fixtures/**"]
)");
    write_file(tmp.path / "tests/kept.cpp", "int main() {}\n");
    write_file(tmp.path / "tests/fixtures/dropped.cpp", "int main() {}\n");
    write_file(tmp.path / "root_check.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    ASSERT_TRUE(result.has_value()) << result.error();
    std::set<std::string> names;
    for (auto const& t : result->targets) names.insert(t.name);
    EXPECT_EQ(names, (std::set<std::string>{"kept", "root_check"}));
}

TEST(TestTargets, AnEmptyDiscoverFindsNothing) {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"

[test]
discover = []
)");
    write_file(tmp.path / "tests/main.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(result->discoverDeclared);
    EXPECT_TRUE(result->discover.empty());
    EXPECT_TRUE(result->targets.empty());
}

TEST(TestTargets, TwoFilesWithOneNameAreRefusedNamingBoth) {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"

[test]
discover = ["tests/*.cpp", "checks/*.cpp"]
)");
    write_file(tmp.path / "tests/same.cpp", "int main() {}\n");
    write_file(tmp.path / "checks/same.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("duplicate test name 'same'"), std::string::npos) << result.error();
    EXPECT_NE(result.error().find("tests/same.cpp"), std::string::npos) << result.error();
    EXPECT_NE(result.error().find("checks/same.cpp"), std::string::npos) << result.error();
}

// Without the key the set, and every name, is the one earlier releases gave.
TEST(TestTargets, WithoutDiscoverTheDefaultIsTestsGlob) {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"
)");
    write_file(tmp.path / "tests/unit/a.cpp", "int main() {}\n");
    write_file(tmp.path / "checks/b.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_FALSE(result->discoverDeclared);
    EXPECT_EQ(result->discover, std::vector<std::string>{"tests/**/*.cpp"});
    ASSERT_EQ(result->targets.size(), 1u);
    EXPECT_EQ(result->targets[0].name, "unit/a");
}

TEST(TestTargets, BrokenManifestStillReturnsInventory) {
    Tmp tmp;
    write_file(tmp.path / "mcpp.toml", "this is not valid TOML\n");
    write_file(tmp.path / "tests/main.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->packageRoot, tmp.path);
    ASSERT_EQ(result->targets.size(), 1u);
    EXPECT_EQ(result->targets[0].name, "main");
}

} // namespace
