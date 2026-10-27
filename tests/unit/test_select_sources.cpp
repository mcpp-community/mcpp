#include <gtest/gtest.h>

import std;
import mcpp.modgraph.scanner;

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

TEST(SelectSources, AnExclusionRemovesWhatThePatternsSelected) {
    Tree t;
    auto got = t.names(mcpp::modgraph::select_sources(t.root, { "src/**/*.cpp", "!src/**/*_test.cpp",
                                                                "!src/native/**" }));
    EXPECT_EQ(got, (std::vector<std::string>{ "src/a.cpp" }));
}

TEST(SelectSources, ASystemsRowLeavesOutTheOtherSystemsDirectory) {
    Tree t;
    // The unconditional list, then the linux row.
    auto got = t.names(mcpp::modgraph::select_sources(t.root, { "src/**/*.cpp", "!src/native/windows/**" }));
    EXPECT_EQ(got, (std::vector<std::string>{ "src/a.cpp", "src/b_test.cpp", "src/native/posix/lock.cpp" }));
}

TEST(SelectSources, AnExclusionRemovesWhicheverPatternSelected) {
    Tree t;
    // An exclusion written before the pattern that selects the file still
    // removes it, as published manifests rely on.
    auto got = t.names(mcpp::modgraph::select_sources(t.root, { "!src/native/**", "src/**/*.cpp",
                                                                "src/native/posix/*.cpp" }));
    EXPECT_EQ(got, (std::vector<std::string>{ "src/a.cpp", "src/b_test.cpp" }));
}
