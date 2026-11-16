
import std;
import mcpplibs.mtest;
import mcpp.build.directives;
import mcpp.toolchain.fingerprint;

using namespace mcpplibs::mtest;

namespace dirs = mcpp::build::directives;

namespace {

struct GlobInputs {
    std::filesystem::path tree;
    std::filesystem::path root;

    void SetUp() {
        tree = std::filesystem::temp_directory_path() / "mcpp_parent_glob_inputs";
        std::filesystem::remove_all(tree);
        root = tree / "workspace" / "app";
        std::filesystem::create_directories(root);
    }

    void TearDown() {
        std::error_code ec;
        std::filesystem::remove_all(tree, ec);
    }

    void write(const std::filesystem::path& relative, std::string_view content = "x") {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path);
        out << content;
    }

    std::string fingerprint(std::string_view pattern) {
        return dirs::glob_fingerprint(root, pattern, "target");
    }
};

} // namespace

UTest parent_directory_membership_invalidates_the_fingerprint{"GlobInputs/ParentDirectoryMembershipInvalidatesTheFingerprint", [] {
    struct Case : GlobInputs {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    write("../inputs/a.in");
    const auto one = fingerprint("../inputs/**/*.in");
    expect(ne(one, mcpp::toolchain::hash_string("")));

    write("../inputs/nested/b.in");
    expect(ne(fingerprint("../inputs/**/*.in"), one));
    std::filesystem::remove(root / "../inputs/nested/b.in");
    expect(eq(fingerprint("../inputs/**/*.in"), one));

    write("../inputs/a.in", "different contents");
    write("../inputs/ignored.txt");
    write("../unrelated/c.in");
    expect(eq(fingerprint("../inputs/**/*.in"), one));
}
    } fixture;
    fixture.body();
}};

UTest multiple_parent_levels_and_exact_paths_are_watched{"GlobInputs/MultipleParentLevelsAndExactPathsAreWatched", [] {
    struct Case : GlobInputs {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    const auto before = fingerprint("../../inputs/*.in");
    write("../../inputs/a.in");
    expect(ne(fingerprint("../../inputs/*.in"), before));
    expect(ne(fingerprint("../../inputs/a.in"), before));
    std::filesystem::remove(root / "../../inputs/a.in");
    expect(eq(fingerprint("../../inputs/*.in"), before));
}
    } fixture;
    fixture.body();
}};

UTest missing_literal_directory_does_not_hide_later_files{"GlobInputs/MissingLiteralDirectoryDoesNotHideLaterFiles", [] {
    struct Case : GlobInputs {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    const auto before = fingerprint("../missing/**/*.in");
    write("../missing/ignored.txt");
    expect(eq(fingerprint("../missing/**/*.in"), before));
    write("../missing/nested/a.in");
    expect(ne(fingerprint("../missing/**/*.in"), before));
}
    } fixture;
    fixture.body();
}};

UTest literal_prefix_does_not_bypass_excluded_directories{"GlobInputs/LiteralPrefixDoesNotBypassExcludedDirectories", [] {
    struct Case : GlobInputs {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    const auto before = fingerprint("**");
    write("target/generated.in");
    write(".git/index.in");
    write("../inputs/target/generated.in");
    write("../inputs/.git/index.in");
    expect(eq(fingerprint("**"), before));
    for (const auto pattern : {"target/**", ".git/**", "../inputs/target/**",
                               "../inputs/.git/**", "../inputs/**"})
        expect(eq(fingerprint(pattern), mcpp::toolchain::hash_string(""))) << pattern;
}
    } fixture;
    fixture.body();
}};

UTest parent_patterns_retain_path_set_semantics{"GlobInputs/ParentPatternsRetainPathSetSemantics", [] {
    struct Case : GlobInputs {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    write("../inputs/a.in");
    const auto before = fingerprint("../inputs/**");
    write("../inputs/a.in", "longer content");
    std::filesystem::last_write_time(root / "../inputs/a.in",
        std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
    expect(eq(fingerprint("../inputs/**"), before));
    write("../inputs/nested/b.in");
    expect(ne(fingerprint("../inputs/**"), before));
}
    } fixture;
    fixture.body();
}};

UTest directory_symlinks_are_followed_as_the_source_scan_follows_them{"GlobInputs/DirectorySymlinksAreFollowedAsTheSourceScanFollowsThem", [] {
    struct Case : GlobInputs {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    write("../real/nested/a.in");
    std::error_code ec;
    std::filesystem::create_directory_symlink(tree / "workspace" / "real",
        root / "../link", ec);
    if (ec) skip([&] { std::ostringstream text; text << "Directory symlinks are unavailable: " << ec.message(); return text.str(); }());
    const auto before = fingerprint("../link/**/*.in");
    expect(ne(before, mcpp::toolchain::hash_string("")));
    expect(ne(fingerprint("../link/nested/*.in"), mcpp::toolchain::hash_string("")));
    write("../real/nested/b.in");
    expect(ne(fingerprint("../link/**/*.in"), before));
}
    } fixture;
    fixture.body();
}};

UTest a_link_cycle_ends_the_walk{"GlobInputs/ALinkCycleEndsTheWalk", [] {
    struct Case : GlobInputs {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    write("../loop/a.in");
    std::error_code ec;
    std::filesystem::create_directory_symlink(tree / "workspace" / "loop",
        root / "../loop/again", ec);
    if (ec) skip([&] { std::ostringstream text; text << "Directory symlinks are unavailable: " << ec.message(); return text.str(); }());
    expect(ne(fingerprint("../loop/**/*.in"), mcpp::toolchain::hash_string("")));
}
    } fixture;
    fixture.body();
}};

UTest absolute_patterns_match_absolute_paths{"GlobInputs/AbsolutePatternsMatchAbsolutePaths", [] {
    struct Case : GlobInputs {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    write("../inputs/a.in");
    const auto dir = (tree / "workspace" / "inputs").lexically_normal().generic_string();
    const auto before = fingerprint(dir + "/**/*.in");
    expect(ne(before, mcpp::toolchain::hash_string("")));
    write("../inputs/nested/b.in");
    expect(ne(fingerprint(dir + "/**/*.in"), before));
}
    } fixture;
    fixture.body();
}};

UTest patterns_that_leave_the_package_are_recognised{"GlobInputBoundary/PatternsThatLeaveThePackageAreRecognised", [] {
    expect(!dirs::glob_leaves_package("proto/**/*.proto"));
    expect(!dirs::glob_leaves_package("a/../b/*.in"));
    expect(bool(dirs::glob_leaves_package("../inputs/**/*.in")));
    expect(bool(dirs::glob_leaves_package("a/../../inputs/*.in")));
    expect(bool(dirs::glob_leaves_package("/usr/share/data/*.in")));
    expect(bool(dirs::glob_leaves_package("C:/data/*.in")));
}};

UTest an_unwatchable_pattern_says_why{"GlobInputs/AnUnwatchablePatternSaysWhy", [] {
    struct Case : GlobInputs {
        Case() { SetUp(); }
        ~Case() { TearDown(); }
        void body() {
    expect(!(dirs::glob_unwatchable(root, "proto/**", "target").has_value()));
    expect(!(dirs::glob_unwatchable(root, "../inputs/**", "target").has_value()));
    for (const auto pattern : {"target/**", ".git/HEAD", "../inputs/.mcpp/*", "/*.in"})
        expect(dirs::glob_unwatchable(root, pattern, "target").has_value()) << pattern;
}
    } fixture;
    fixture.body();
}};
