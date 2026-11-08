
import std;
import mcpplibs.mtest;
import mcpp.build.test_targets;

using namespace mcpplibs::mtest;

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

UTest nested_names_use_tests_relative_path{"TestTargets/NestedNamesUseTestsRelativePath", [] {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"
)");
    write_file(tmp.path / "tests/tagged/nested/smoke.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error();
    expect(fatal(eq(result->targets.size(), 1u)));
    expect(eq(result->targets[0].name, "tagged/nested/smoke"));
    expect(eq(std::filesystem::path(result->targets[0].main), std::filesystem::path("tests") / "tagged" / "nested" / "smoke.cpp"));
}};

UTest glob_flags_become_per_target_flags{"TestTargets/GlobFlagsBecomePerTargetFlags", [] {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"

[build]
flags = [{ glob = "tests/**/*.cpp", defines = ["TEST_FEATURE"], cflags = ["-Wall"], cxxflags = ["-Wextra"] }]
)");
    write_file(tmp.path / "tests/main.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error();
    expect(fatal(eq(result->targets.size(), 1u)));
    expect(eq(result->targets[0].defines, std::vector<std::string>{"TEST_FEATURE"}));
    expect(eq(result->targets[0].cflags, std::vector<std::string>{"-Wall"}));
    expect(eq(result->targets[0].cxxflags, std::vector<std::string>{"-Wextra"}));
}};

UTest package_filter_scopes_workspace_member{"TestTargets/PackageFilterScopesWorkspaceMember", [] {
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
    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error();
    expect(fatal(eq(result->packageRoot, tmp.path / "a")));
    expect(fatal(eq(result->targets.size(), 1u)));
    expect(eq(std::filesystem::path(result->targets[0].main), std::filesystem::path("tests") / "main.cpp"));
}};

UTest directory_symlink_keeps_lexical_package_boundary{"TestTargets/DirectorySymlinkKeepsLexicalPackageBoundary", [] {
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
    if (ec) skip([&] { std::ostringstream text; text << "directory symlink unavailable: " << ec.message(); return text.str(); }());

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error();
    expect(fatal(eq(result->targets.size(), 1u)));
    expect(eq(result->targets[0].name, "vendor/external"));
    expect(eq(std::filesystem::path(result->targets[0].main), std::filesystem::path("tests") / "vendor" / "external.cpp"));
}};

// `[test] discover` (#634 A5): names are relative to the fixed directory of
// the glob that found the file, `main` stays relative to the package root.
UTest discover_names_are_relative_to_the_globs_directory{"TestTargets/DiscoverNamesAreRelativeToTheGlobsDirectory", [] {
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
    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error();
    expect(bool(result->discoverDeclared));
    expect(fatal(eq(result->targets.size(), 1u)));
    expect(eq(result->targets[0].name, "deep/a"));
    expect(eq(std::filesystem::path(result->targets[0].main), std::filesystem::path("checks") / "deep" / "a.cpp"));
}};

// An exclusion removes a file whichever positive glob found it, and a glob
// with no fixed directory names files relative to the package root.
UTest discover_exclusions_apply_across_globs{"TestTargets/DiscoverExclusionsApplyAcrossGlobs", [] {
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
    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error();
    std::set<std::string> names;
    for (auto const& t : result->targets) names.insert(t.name);
    expect(eq(names, (std::set<std::string>{"kept", "root_check"})));
}};

UTest an_empty_discover_finds_nothing{"TestTargets/AnEmptyDiscoverFindsNothing", [] {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"

[test]
discover = []
)");
    write_file(tmp.path / "tests/main.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error();
    expect(bool(result->discoverDeclared));
    expect(result->discover.empty());
    expect(result->targets.empty());
}};

UTest two_files_with_one_name_are_refused_naming_both{"TestTargets/TwoFilesWithOneNameAreRefusedNamingBoth", [] {
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
    expect(fatal(!result.has_value()));
    if (auto&& failed = expect(ne(result.error().find("duplicate test name 'same'"), std::string::npos)); !failed) failed << result.error();
    if (auto&& failed = expect(ne(result.error().find("tests/same.cpp"), std::string::npos)); !failed) failed << result.error();
    if (auto&& failed = expect(ne(result.error().find("checks/same.cpp"), std::string::npos)); !failed) failed << result.error();
}};

// Without the key the set, and every name, is the one earlier releases gave.
UTest without_discover_the_default_is_tests_glob{"TestTargets/WithoutDiscoverTheDefaultIsTestsGlob", [] {
    Tmp tmp;
    write_manifest(tmp.path, R"([package]
name = "demo"
version = "0.1.0"
)");
    write_file(tmp.path / "tests/unit/a.cpp", "int main() {}\n");
    write_file(tmp.path / "checks/b.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error();
    expect(!result->discoverDeclared);
    expect(eq(result->discover, std::vector<std::string>{"tests/**/*.cpp"}));
    expect(fatal(eq(result->targets.size(), 1u)));
    expect(eq(result->targets[0].name, "unit/a"));
}};

UTest broken_manifest_still_returns_inventory{"TestTargets/BrokenManifestStillReturnsInventory", [] {
    Tmp tmp;
    write_file(tmp.path / "mcpp.toml", "this is not valid TOML\n");
    write_file(tmp.path / "tests/main.cpp", "int main() {}\n");

    auto result = mcpp::build::discover_test_targets(tmp.path, {});
    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error();
    expect(fatal(eq(result->packageRoot, tmp.path)));
    expect(fatal(eq(result->targets.size(), 1u)));
    expect(eq(result->targets[0].name, "main"));
}};

} // namespace
