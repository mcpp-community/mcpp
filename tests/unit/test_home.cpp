#include <gtest/gtest.h>
#include <cstdlib>

import std;
import mcpp.home;
import mcpp.toolchain.stdmod;

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
    EXPECT_EQ(mcpp::home::root(), std::filesystem::path("/tmp/mcpp-home-test"));
    EXPECT_EQ(mcpp::home::bmi_root(),
              std::filesystem::path("/tmp/mcpp-home-test") / "bmi");
}

TEST(Home, BmiRootIsAlwaysRootSlashBmi) {
    ScopedEnv home("MCPP_HOME", "/tmp/mcpp-home-test-2");
    EXPECT_EQ(mcpp::home::bmi_root(), mcpp::home::root() / "bmi");
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
    EXPECT_EQ(mcpp::home::bmi_root().filename(), std::filesystem::path("bmi"));
    EXPECT_NE(mcpp::home::bmi_root().filename(), std::filesystem::path(".mcpp-bmi"));
}

// #311's D2 regression gate: the std module cache and the dep BMI cache must
// resolve to ONE root. `default_cache_root()` used to be a private copy of the
// home resolution that drifted (no USERPROFILE, no self-contained detection),
// so on Windows the std BMI landed in the current working directory.
TEST(Home, StdModuleCacheRootEqualsHomeBmiRoot) {
    {
        ScopedEnv home("MCPP_HOME", "/tmp/mcpp-home-test-3");
        EXPECT_EQ(mcpp::toolchain::default_cache_root(), mcpp::home::bmi_root());
    }
    {
        ScopedEnv mcppHome("MCPP_HOME", nullptr);
        EXPECT_EQ(mcpp::toolchain::default_cache_root(), mcpp::home::bmi_root());
    }
}
