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
