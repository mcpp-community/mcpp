
import std;
import mcpplibs.mtest;
import mcpp.modgraph.scanner;

using namespace mcpplibs::mtest;

// A `sources` list selects every pattern's files less the files any `!`
// pattern matches, whichever pattern selected them. The rows of
// `[target.<cfg>.build] sources` are appended to the list, so a system's row
// leaves out the other systems' directories (mcpp's own `src/native`).

namespace fs = std::filesystem;

namespace {
struct Tree {
    fs::path root;
    Tree() {
        root = fs::temp_directory_path() / std::format("mcpp-select-{:08x}", std::random_device{}());
        for (auto rel : { "src/a.cpp", "src/b_test.cpp", "src/native/posix/lock.cpp",
                          "src/native/windows/lock.cpp" }) {
            fs::create_directories((root / rel).parent_path());
            std::ofstream(root / rel) << "\n";
        }
    }
    ~Tree() { std::error_code ec; fs::remove_all(root, ec); }
    std::vector<std::string> names(const std::set<fs::path>& files) const {
        std::vector<std::string> out;
        for (auto const& f : files) out.push_back(f.lexically_relative(root).generic_string());
        return out;
    }
};
}  // namespace

UTest an_exclusion_removes_what_the_patterns_selected{"SelectSources/AnExclusionRemovesWhatThePatternsSelected", [] {
    Tree t;
    auto got = t.names(mcpp::modgraph::select_sources(t.root, { "src/**/*.cpp", "!src/**/*_test.cpp",
                                                                "!src/native/**" }));
    expect(eq(got, (std::vector<std::string>{ "src/a.cpp" })));
}};

UTest a_systems_row_leaves_out_the_other_systems_directory{"SelectSources/ASystemsRowLeavesOutTheOtherSystemsDirectory", [] {
    Tree t;
    // The unconditional list, then the linux row.
    auto got = t.names(mcpp::modgraph::select_sources(t.root, { "src/**/*.cpp", "!src/native/windows/**" }));
    expect(eq(got, (std::vector<std::string>{ "src/a.cpp", "src/b_test.cpp", "src/native/posix/lock.cpp" })));
}};

UTest an_exclusion_removes_whichever_pattern_selected{"SelectSources/AnExclusionRemovesWhicheverPatternSelected", [] {
    Tree t;
    // An exclusion written before the pattern that selects the file still
    // removes it, as published manifests rely on.
    auto got = t.names(mcpp::modgraph::select_sources(t.root, { "!src/native/**", "src/**/*.cpp",
                                                                "src/native/posix/*.cpp" }));
    expect(eq(got, (std::vector<std::string>{ "src/a.cpp", "src/b_test.cpp" })));
}};
