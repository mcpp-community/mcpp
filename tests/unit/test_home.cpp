#include <cstdlib>

import std;
import mcpplibs.mtest;
import mcpp.invocation;
import mcpp.home;
import mcpplibs.os.platform;
import mcpplibs.os.windows;
import mcpp.toolchain.stdmod;
import mcpp.config;
import mcpp.libs.toml;

using namespace mcpplibs::mtest;

namespace {

// Set/restore an environment variable for the duration of a test.
class ScopedEnv {
public:
    ScopedEnv(std::string name, const char* value)
        : inner_(std::move(name), value ? std::optional<std::string>(value) : std::nullopt) {}
private:
    mcpp::invocation::ScopedEnv inner_;   // stated for this invocation, as mcpp states its own
};

}  // namespace

UTest explicit_mcpp_home_wins{"Home/ExplicitMcppHomeWins", [] {
    ScopedEnv home("MCPP_HOME", "/tmp/mcpp-home-test");
    const auto expected = std::filesystem::weakly_canonical(std::filesystem::absolute("/tmp/mcpp-home-test"));
    expect(eq(mcpp::home::root(), expected));
    expect(eq(mcpp::home::cache_root(), expected / "build-cache" / "v1"));
}};

UTest cache_root_is_always_root_slash_build_cache_v1{"Home/CacheRootIsAlwaysRootSlashBuildCacheV1", [] {
    ScopedEnv home("MCPP_HOME", "/tmp/mcpp-home-test-2");
    expect(eq(mcpp::home::cache_root(), mcpp::home::root() / "build-cache" / "v1"));
}};

// The layout version has to be a path segment, not an implicit convention:
// replacing the layout must never require migrating or deleting the old tree.
UTest cache_root_carries_the_layout_version{"Home/CacheRootCarriesTheLayoutVersion", [] {
    ScopedEnv home("MCPP_HOME", "/tmp/mcpp-home-test-2b");
    expect(eq(mcpp::home::cache_root().filename(), std::filesystem::path("v1")));
    expect(eq(mcpp::home::cache_root().parent_path().filename(), std::filesystem::path("build-cache")));
    // Must NOT nest inside <home>/cache: GlobalConfig's index metadata cache
    // owns that name and its reset path removes the whole directory.
    expect(ne(mcpp::home::cache_root().parent_path().filename(), std::filesystem::path("cache")));
    // The pre-v1 root must stay resolvable and must be a DIFFERENT tree, so
    // `doctor` can report it and `cache clean --legacy` can reclaim it.
    expect(eq(mcpp::home::legacy_bmi_root(), mcpp::home::root() / "bmi"));
    expect(ne(mcpp::home::legacy_bmi_root(), mcpp::home::cache_root()));
}};

// The user-home fallback must land on `.mcpp`, never on the legacy flat
// `.mcpp-bmi` sibling that the pre-#311 std-module resolver produced whenever
// neither MCPP_HOME nor HOME resolved (the default on Windows PowerShell).
UTest fallback_uses_dot_mcpp_not_legacy_flat_dir{"Home/FallbackUsesDotMcppNotLegacyFlatDir", [] {
    ScopedEnv mcppHome("MCPP_HOME", nullptr);
    ScopedEnv userHome("HOME", nullptr);
#if defined(_WIN32)
    ScopedEnv profile("USERPROFILE", nullptr);
#endif
    auto root = mcpp::home::root();
    expect(eq(root.filename(), std::filesystem::path(".mcpp")));
    expect(ne(mcpp::home::cache_root().filename(), std::filesystem::path(".mcpp-bmi")));
}};

// #311's D2 regression gate: the std module cache and the dep cache must
// resolve to ONE root. `default_cache_root()` used to be a private copy of the
// home resolution that drifted (no USERPROFILE, no self-contained detection),
// so on Windows the std BMI landed in the current working directory.
UTest std_module_cache_root_equals_home_cache_root{"Home/StdModuleCacheRootEqualsHomeCacheRoot", [] {
    {
        ScopedEnv home("MCPP_HOME", "/tmp/mcpp-home-test-3");
        expect(eq(mcpp::toolchain::default_cache_root(), mcpp::home::cache_root()));
    }
    {
        ScopedEnv mcppHome("MCPP_HOME", nullptr);
        expect(eq(mcpp::toolchain::default_cache_root(), mcpp::home::cache_root()));
    }
}};

namespace {
class HomeSelection {
protected:
    std::filesystem::path scratch = std::filesystem::temp_directory_path()
        / std::format("mcpp-home-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::path original = scratch / "original";
    std::filesystem::path target = scratch / "target";
    ScopedEnv env{"MCPP_HOME", mcpp::home::spelling(original).c_str()};
    void SetUp() {
        std::filesystem::create_directories(original);
        std::filesystem::create_directories(target);
    }
    void TearDown() {
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
    }
    void locator(const std::filesystem::path& directory, const std::filesystem::path& destination) {
        std::ofstream(directory / "home.toml") << "home = "
            << mcpp::libs::toml::escape_string(mcpp::home::spelling(destination)) << '\n';
    }
};
}

UTest default_original_and_effective_are_identical{"HomeSelection/DefaultOriginalAndEffectiveAreIdentical", [] {
    struct Case : HomeSelection {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    const auto selected = mcpp::home::selection();
    expect(eq(selected.original, std::filesystem::weakly_canonical(original)));
    expect(eq(selected.original, selected.effective));
    expect(!selected.redirected);
}
    } fixture;
    fixture.body();
}};

UTest environment_selects_the_locator_and_only_one_hop_is_read{"HomeSelection/EnvironmentSelectsTheLocatorAndOnlyOneHopIsRead", [] {
    struct Case : HomeSelection {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    const auto third = scratch / "third";
    std::filesystem::create_directories(third);
    locator(original, target);
    locator(target, third);
    const auto selected = mcpp::home::selection();
    expect(eq(selected.original, std::filesystem::weakly_canonical(original)));
    expect(eq(selected.effective, std::filesystem::weakly_canonical(target)));
    expect(eq(mcpp::home::cache_root(), selected.effective / "build-cache" / "v1"));
    expect(bool(selected.redirected));
}
    } fixture;
    fixture.body();
}};

UTest unavailable_target_is_an_error_instead_of_a_fallback{"HomeSelection/UnavailableTargetIsAnErrorInsteadOfAFallback", [] {
    struct Case : HomeSelection {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    locator(original, scratch / "missing");
    const auto selected = mcpp::home::inspect(original);
    expect(fatal(!selected));
    expect(ne(selected.error().find("unavailable"), std::string::npos));
    expect(!std::filesystem::exists(original / "config.toml"));
}
    } fixture;
    fixture.body();
}};

UTest force_can_repair_a_malformed_locator{"HomeSelection/ForceCanRepairAMalformedLocator", [] {
    struct Case : HomeSelection {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    std::ofstream(original / "home.toml") << "home = [\n";
    expect(!mcpp::home::choose(target, false));
    const auto selected = mcpp::home::choose(target, true);
    if (auto&& failed = expect(fatal(bool(selected))); !failed) failed << selected.error();
    expect(bool(mcpp::home::save(*selected)));
    const auto restored = mcpp::home::inspect(original);
    if (auto&& failed = expect(fatal(bool(restored))); !failed) failed << restored.error();
    expect(eq(restored->effective, std::filesystem::weakly_canonical(target)));
}
    } fixture;
    fixture.body();
}};

UTest existing_home_needs_force_and_changing_it_keeps_both_data_trees{"HomeSelection/ExistingHomeNeedsForceAndChangingItKeepsBothDataTrees", [] {
    struct Case : HomeSelection {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    std::ofstream(original / "config.toml") << "# existing\n";
    std::ofstream(target / "sentinel") << "keep\n";
    expect(!mcpp::home::choose(target, false));
    const auto selected = mcpp::home::choose(target, true);
    if (auto&& failed = expect(fatal(bool(selected))); !failed) failed << selected.error();
    expect(fatal(bool(mcpp::home::save(*selected))));
    expect(std::filesystem::exists(original / "config.toml"));
    expect(std::filesystem::exists(target / "sentinel"));
    expect(eq(mcpp::invocation::get("MCPP_HOME").value_or(""), mcpp::home::spelling(original)));
    const auto restored = mcpp::home::inspect(original);
    expect(fatal(bool(restored)));
    expect(eq(restored->previous, std::filesystem::weakly_canonical(original)));
}
    } fixture;
    fixture.body();
}};

UTest returning_to_original_removes_only_the_locator{"HomeSelection/ReturningToOriginalRemovesOnlyTheLocator", [] {
    struct Case : HomeSelection {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    locator(original, target);
    std::ofstream(target / "config.toml") << "# existing\n";
    const auto selected = mcpp::home::choose(original, true);
    if (auto&& failed = expect(fatal(bool(selected))); !failed) failed << selected.error();
    expect(fatal(bool(mcpp::home::save(*selected))));
    expect(!std::filesystem::exists(original / "home.toml"));
    expect(std::filesystem::exists(target / "config.toml"));
    expect(eq(mcpp::home::root(), std::filesystem::weakly_canonical(original)));
}
    } fixture;
    fixture.body();
}};

UTest nested_directories_are_refused{"HomeSelection/NestedDirectoriesAreRefused", [] {
    struct Case : HomeSelection {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    expect(!mcpp::home::choose(original / "nested", true));
    expect(!mcpp::home::choose(scratch, true));
}
    } fixture;
    fixture.body();
}};

UTest read_only_configuration_does_not_initialize_or_acquire_a_provider{"HomeSelection/ReadOnlyConfigurationDoesNotInitializeOrAcquireAProvider", [] {
    struct Case : HomeSelection {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    const auto cold = scratch / "cold";
    ScopedEnv home("MCPP_HOME", mcpp::home::spelling(cold).c_str());
    const auto config = mcpp::config::load_or_init(true, {}, {}, mcpp::config::Initialization::ReadOnly);
    if (auto&& failed = expect(fatal(bool(config))); !failed) failed << config.error().message;
    expect(eq(config->mcppHome, std::filesystem::weakly_canonical(cold)));
    expect(!std::filesystem::exists(cold));
}
    } fixture;
    fixture.body();
}};

UTest user_source_survives_original_path_normalization{"HomeSelection/UserSourceSurvivesOriginalPathNormalization", [] {
    struct Case : HomeSelection {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    const auto user = mcpp::home::spelling(scratch);
    ScopedEnv env("MCPP_HOME", nullptr);
    ScopedEnv posixHome("HOME", user.c_str());
    ScopedEnv windowsHome("USERPROFILE", user.c_str());
    const auto selected = mcpp::home::inspect(scratch / ".mcpp");
    if (auto&& failed = expect(fatal(bool(selected))); !failed) failed << selected.error();
    expect(eq(selected->source, "user"));
}
    } fixture;
    fixture.body();
}};

UTest missing_user_directory_cannot_publish_a_locator_in_the_working_directory{"HomeSelection/MissingUserDirectoryCannotPublishALocatorInTheWorkingDirectory", [] {
    struct Case : HomeSelection {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    ScopedEnv explicitHome("MCPP_HOME", nullptr);
    ScopedEnv posixHome("HOME", nullptr);
    ScopedEnv windowsHome("USERPROFILE", nullptr);
    const auto selected = mcpp::home::choose(target, true);
    expect(fatal(!selected));
    expect(ne(selected.error().find("user directory"), std::string::npos));
}
    } fixture;
    fixture.body();
}};


UTest unicode_locator_round_trip_does_not_narrow_the_selected_path{"HomeSelection/UnicodeLocatorRoundTripDoesNotNarrowTheSelectedPath", [] {
    struct Case : HomeSelection {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    if constexpr (mcpplibs::os::is_windows) {
        const auto acp = mcpplibs::os::windows::code_page();
        expect(fatal(ne(acp, 0u)));
        if (acp == 65001)
            skip([&] { std::ostringstream text; text << "UTF-8 ACP cannot prove the legacy-code-page path conversion boundary; "
                            "the actual CLI Unicode case still runs"; return text.str(); }());
    }
    // Devanagari cannot be represented by any legacy Windows ANSI code page.
    // Explicit UTF-8 code units keep this independent of compiler source encoding.
    const auto unicode = scratch / std::filesystem::path(
        std::u8string(u8"selected space \u0915\u0916\u0917 \u65e5\u672c\u8a9e"));
    std::filesystem::create_directory(unicode);
    const auto selected = mcpp::home::choose(unicode, true);
    if (auto&& failed = expect(fatal(bool(selected))); !failed) failed << selected.error();
    expect(fatal(bool(mcpp::home::save(*selected))));
    const auto restored = mcpp::home::inspect(original);
    if (auto&& failed = expect(fatal(bool(restored))); !failed) failed << restored.error();
    expect(eq(restored->effective, std::filesystem::weakly_canonical(unicode)));
    expect(eq(mcpp::home::from_utf8(mcpp::home::spelling(unicode)), unicode));
}
    } fixture;
    fixture.body();
}};
