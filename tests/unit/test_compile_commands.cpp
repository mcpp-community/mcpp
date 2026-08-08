#include <gtest/gtest.h>

import std;
import mcpp.build.compile_commands;

using namespace mcpp::build;

namespace {

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / std::format("mcpp-compile-commands-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());

    TempDir() { std::filesystem::create_directories(path); }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), {}};
}

// Build a single CDB entry as JSON text. `flag` is a marker we can grep for.
std::string entry(std::string_view file, std::string_view flag) {
    // Keep the file path out of `arguments` so it appears exactly once (in
    // "file") — lets tests count entries per file unambiguously.
    return std::format(
        R"({{"directory":"/p","file":"{}","arguments":["g++","{}","-c","src.cpp"],"output":"o"}})",
        file, flag);
}

std::string cdb(std::initializer_list<std::string> entries) {
    std::string s = "[\n";
    bool first = true;
    for (auto const& e : entries) {
        if (!first) s += ",\n";
        s += e;
        first = false;
    }
    s += "\n]\n";
    return s;
}

std::size_t count(std::string_view hay, std::string_view needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string_view::npos) { ++n; pos += needle.size(); }
    return n;
}

}  // namespace

// The core regression guard: a plain `mcpp build` regenerates the CDB from a
// plan that lacks test files, but it must NOT wipe out test entries a prior
// `mcpp test` wrote — clangd would lose all completion in tests/ otherwise.
TEST(CompileCommandsMerge, PreservesPriorEntriesForFilesNotInFreshPlan) {
    auto fresh    = cdb({ entry("/p/src/main.cpp", "-O2-FRESH") });
    auto existing = cdb({ entry("/p/src/main.cpp", "-O0-STALE"),
                          entry("/p/tests/test_smoke.cpp", "-Igtest") });

    auto merged = merge_compile_commands(
        fresh, existing, [](const std::filesystem::path&) { return true; });

    // main.cpp takes the fresh plan's flags, not the stale prior ones.
    EXPECT_NE(merged.find("-O2-FRESH"), std::string::npos) << merged;
    EXPECT_EQ(merged.find("-O0-STALE"), std::string::npos) << merged;
    // The test entry, absent from the fresh plan, is preserved.
    EXPECT_NE(merged.find("tests/test_smoke.cpp"), std::string::npos) << merged;
    EXPECT_NE(merged.find("-Igtest"), std::string::npos) << merged;
}

// Prior entries for files that no longer exist on disk must be pruned, so the
// CDB never accumulates dead references (e.g. a deleted test file).
TEST(CompileCommandsMerge, PrunesPriorEntriesWhoseFileNoLongerExists) {
    auto fresh    = cdb({ entry("/p/src/main.cpp", "-O2") });
    auto existing = cdb({ entry("/p/tests/deleted.cpp", "-Igtest") });

    auto merged = merge_compile_commands(
        fresh, existing,
        [](const std::filesystem::path& p) { return p != "/p/tests/deleted.cpp"; });

    EXPECT_EQ(merged.find("deleted.cpp"), std::string::npos) << merged;
}

// Exactly one entry per file: the fresh plan wins, no duplicate accrues.
TEST(CompileCommandsMerge, FreshEntryWinsAndNoDuplicatePerFile) {
    auto fresh    = cdb({ entry("/p/a.cpp", "-FRESH") });
    auto existing = cdb({ entry("/p/a.cpp", "-STALE") });

    auto merged = merge_compile_commands(
        fresh, existing, [](const std::filesystem::path&) { return true; });

    EXPECT_EQ(count(merged, "/p/a.cpp"), 1u) << merged;
    EXPECT_NE(merged.find("-FRESH"), std::string::npos) << merged;
    EXPECT_EQ(merged.find("-STALE"), std::string::npos) << merged;
}

// A malformed prior CDB must never break generation: fall back to fresh.
TEST(CompileCommandsMerge, MalformedExistingFallsBackToFresh) {
    auto fresh = cdb({ entry("/p/src/main.cpp", "-O2") });

    auto merged = merge_compile_commands(
        fresh, "{ this is not valid json ][",
        [](const std::filesystem::path&) { return true; });

    EXPECT_NE(merged.find("src/main.cpp"), std::string::npos) << merged;
    EXPECT_NE(merged.find("-O2"), std::string::npos) << merged;
}

TEST(CompileCommandsMerge, SortsFinalEntriesByFile) {
    auto fresh = cdb({entry("/p/z.cpp", "-DZ"), entry("/p/a.cpp", "-DA")});
    auto merged = merge_compile_commands(
        fresh, "[]", [](const std::filesystem::path&) { return true; });

    EXPECT_LT(merged.find("/p/a.cpp"), merged.find("/p/z.cpp"));
}

TEST(CompileCommandsWriter, RejectsNonArrayFreshJson) {
    TempDir temp;
    auto result = publish_compile_commands(
        temp.path / "compile_commands.json", R"({"file":"not-an-array"})",
        [](const std::filesystem::path&) { return true; });

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("JSON array"), std::string::npos);
}

TEST(CompileCommandsWriter, UnchangedContentKeepsMtime) {
    TempDir temp;
    auto path = temp.path / "compile_commands.json";
    auto content = cdb({entry((temp.path / "a.cpp").string(), "-DOK")});
    std::ofstream(path) << content;
    auto before = std::filesystem::last_write_time(path);

    auto result = publish_compile_commands(
        path, content, [](const std::filesystem::path&) { return true; });

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->changed);
    EXPECT_EQ(std::filesystem::last_write_time(path), before);
}

TEST(CompileCommandsWriter, ReplacementFailurePreservesOldDatabase) {
    TempDir temp;
    auto path = temp.path / "compile_commands.json";
    auto oldContent = cdb({entry((temp.path / "old.cpp").string(), "-DOLD")});
    auto newContent = cdb({entry((temp.path / "new.cpp").string(), "-DNEW")});
    std::ofstream(path) << oldContent;
    auto failReplace = [](const std::filesystem::path&,
                          const std::filesystem::path&,
                          std::error_code& ec) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    };

    auto result = publish_compile_commands(
        path, newContent, [](const std::filesystem::path&) { return true; },
        failReplace);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(read_file(path), oldContent);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(temp.path),
                            std::filesystem::directory_iterator{}), 1);
}
