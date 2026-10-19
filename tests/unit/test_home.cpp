#include <gtest/gtest.h>
#include <cstdlib>

import std;
import mcpp.home;
import mcpp.toolchain.stdmod;
import mcpp.config;
import mcpp.libs.toml;

namespace {

// Set/restore an environment variable for the duration of a test.
class ScopedEnv {
public:
    ScopedEnv(std::string name, const char* value) : name_(std::move(name)) {
        if (const char* old = std::getenv(name_.c_str()); old) {
            had_ = true;
            old_ = old;
        }
        apply(value);
    }
    ~ScopedEnv() { apply(had_ ? old_.c_str() : nullptr); }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void apply(const char* value) {
#if defined(_WIN32)
        // _putenv_s with "" removes the variable on Windows.
        ::_putenv_s(name_.c_str(), value ? value : "");
#else
        if (value) ::setenv(name_.c_str(), value, 1);
        else       ::unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    bool        had_ = false;
    std::string old_;
};

}  // namespace

TEST(Home, ExplicitMcppHomeWins) {
    ScopedEnv home("MCPP_HOME", "/tmp/mcpp-home-test");
    const auto expected = std::filesystem::weakly_canonical(std::filesystem::absolute("/tmp/mcpp-home-test"));
    EXPECT_EQ(mcpp::home::root(), expected);
    EXPECT_EQ(mcpp::home::cache_root(),
              expected / "build-cache" / "v1");
}

TEST(Home, CacheRootIsAlwaysRootSlashBuildCacheV1) {
    ScopedEnv home("MCPP_HOME", "/tmp/mcpp-home-test-2");
    EXPECT_EQ(mcpp::home::cache_root(), mcpp::home::root() / "build-cache" / "v1");
}

// The layout version has to be a path segment, not an implicit convention:
// replacing the layout must never require migrating or deleting the old tree.
TEST(Home, CacheRootCarriesTheLayoutVersion) {
    ScopedEnv home("MCPP_HOME", "/tmp/mcpp-home-test-2b");
    EXPECT_EQ(mcpp::home::cache_root().filename(), std::filesystem::path("v1"));
    EXPECT_EQ(mcpp::home::cache_root().parent_path().filename(),
              std::filesystem::path("build-cache"));
    // Must NOT nest inside <home>/cache: GlobalConfig's index metadata cache
    // owns that name and its reset path removes the whole directory.
    EXPECT_NE(mcpp::home::cache_root().parent_path().filename(),
              std::filesystem::path("cache"));
    // The pre-v1 root must stay resolvable and must be a DIFFERENT tree, so
    // `doctor` can report it and `cache clean --legacy` can reclaim it.
    EXPECT_EQ(mcpp::home::legacy_bmi_root(), mcpp::home::root() / "bmi");
    EXPECT_NE(mcpp::home::legacy_bmi_root(), mcpp::home::cache_root());
}

// The user-home fallback must land on `.mcpp`, never on the legacy flat
// `.mcpp-bmi` sibling that the pre-#311 std-module resolver produced whenever
// neither MCPP_HOME nor HOME resolved (the default on Windows PowerShell).
TEST(Home, FallbackUsesDotMcppNotLegacyFlatDir) {
    ScopedEnv mcppHome("MCPP_HOME", nullptr);
    ScopedEnv userHome("HOME", nullptr);
#if defined(_WIN32)
    ScopedEnv profile("USERPROFILE", nullptr);
#endif
    auto root = mcpp::home::root();
    EXPECT_EQ(root.filename(), std::filesystem::path(".mcpp"));
    EXPECT_NE(mcpp::home::cache_root().filename(),
              std::filesystem::path(".mcpp-bmi"));
}

// #311's D2 regression gate: the std module cache and the dep cache must
// resolve to ONE root. `default_cache_root()` used to be a private copy of the
// home resolution that drifted (no USERPROFILE, no self-contained detection),
// so on Windows the std BMI landed in the current working directory.
TEST(Home, StdModuleCacheRootEqualsHomeCacheRoot) {
    {
        ScopedEnv home("MCPP_HOME", "/tmp/mcpp-home-test-3");
        EXPECT_EQ(mcpp::toolchain::default_cache_root(), mcpp::home::cache_root());
    }
    {
        ScopedEnv mcppHome("MCPP_HOME", nullptr);
        EXPECT_EQ(mcpp::toolchain::default_cache_root(), mcpp::home::cache_root());
    }
}

namespace {
class HomeSelection : public ::testing::Test {
protected:
    std::filesystem::path scratch = std::filesystem::temp_directory_path()
        / std::format("mcpp-home-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::path original = scratch / "original";
    std::filesystem::path target = scratch / "target";
    ScopedEnv env{"MCPP_HOME", mcpp::home::spelling(original).c_str()};
    void SetUp() override {
        std::filesystem::create_directories(original);
        std::filesystem::create_directories(target);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
    }
    void locator(const std::filesystem::path& directory, const std::filesystem::path& destination) {
        std::ofstream(directory / "home.toml") << "home = "
            << mcpp::libs::toml::escape_string(mcpp::home::spelling(destination)) << '\n';
    }
};
}

TEST_F(HomeSelection, DefaultOriginalAndEffectiveAreIdentical) {
    const auto selected = mcpp::home::selection();
    EXPECT_EQ(selected.original, std::filesystem::weakly_canonical(original));
    EXPECT_EQ(selected.original, selected.effective);
    EXPECT_FALSE(selected.redirected);
}

TEST_F(HomeSelection, EnvironmentSelectsTheLocatorAndOnlyOneHopIsRead) {
    const auto third = scratch / "third";
    std::filesystem::create_directories(third);
    locator(original, target);
    locator(target, third);
    const auto selected = mcpp::home::selection();
    EXPECT_EQ(selected.original, std::filesystem::weakly_canonical(original));
    EXPECT_EQ(selected.effective, std::filesystem::weakly_canonical(target));
    EXPECT_EQ(mcpp::home::cache_root(), selected.effective / "build-cache" / "v1");
    EXPECT_TRUE(selected.redirected);
}

TEST_F(HomeSelection, UnavailableTargetIsAnErrorInsteadOfAFallback) {
    locator(original, scratch / "missing");
    const auto selected = mcpp::home::inspect(original);
    ASSERT_FALSE(selected);
    EXPECT_NE(selected.error().find("unavailable"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(original / "config.toml"));
}

TEST_F(HomeSelection, ForceCanRepairAMalformedLocator) {
    std::ofstream(original / "home.toml") << "home = [\n";
    EXPECT_FALSE(mcpp::home::choose(target, false));
    const auto selected = mcpp::home::choose(target, true);
    ASSERT_TRUE(selected) << selected.error();
    EXPECT_TRUE(mcpp::home::save(*selected));
    const auto restored = mcpp::home::inspect(original);
    ASSERT_TRUE(restored) << restored.error();
    EXPECT_EQ(restored->effective, std::filesystem::weakly_canonical(target));
}

TEST_F(HomeSelection, ExistingHomeNeedsForceAndChangingItKeepsBothDataTrees) {
    std::ofstream(original / "config.toml") << "# existing\n";
    std::ofstream(target / "sentinel") << "keep\n";
    EXPECT_FALSE(mcpp::home::choose(target, false));
    const auto selected = mcpp::home::choose(target, true);
    ASSERT_TRUE(selected) << selected.error();
    ASSERT_TRUE(mcpp::home::save(*selected));
    EXPECT_TRUE(std::filesystem::exists(original / "config.toml"));
    EXPECT_TRUE(std::filesystem::exists(target / "sentinel"));
    EXPECT_EQ(std::string(std::getenv("MCPP_HOME")), mcpp::home::spelling(original));
    const auto restored = mcpp::home::inspect(original);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->previous, std::filesystem::weakly_canonical(original));
}

TEST_F(HomeSelection, ReturningToOriginalRemovesOnlyTheLocator) {
    locator(original, target);
    std::ofstream(target / "config.toml") << "# existing\n";
    const auto selected = mcpp::home::choose(original, true);
    ASSERT_TRUE(selected) << selected.error();
    ASSERT_TRUE(mcpp::home::save(*selected));
    EXPECT_FALSE(std::filesystem::exists(original / "home.toml"));
    EXPECT_TRUE(std::filesystem::exists(target / "config.toml"));
    EXPECT_EQ(mcpp::home::root(), std::filesystem::weakly_canonical(original));
}

TEST_F(HomeSelection, NestedDirectoriesAreRefused) {
    EXPECT_FALSE(mcpp::home::choose(original / "nested", true));
    EXPECT_FALSE(mcpp::home::choose(scratch, true));
}

TEST_F(HomeSelection, ReadOnlyConfigurationDoesNotInitializeOrAcquireAProvider) {
    const auto cold = scratch / "cold";
    ScopedEnv home("MCPP_HOME", mcpp::home::spelling(cold).c_str());
    const auto config = mcpp::config::load_or_init(true, {}, {}, mcpp::config::Initialization::ReadOnly);
    ASSERT_TRUE(config) << config.error().message;
    EXPECT_EQ(config->mcppHome, std::filesystem::weakly_canonical(cold));
    EXPECT_FALSE(std::filesystem::exists(cold));
}
