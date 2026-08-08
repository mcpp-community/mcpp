#include <gtest/gtest.h>

import std;
import mcpp.build.compile_commands;
import mcpp.build.flags;
import mcpp.build.plan;
import mcpp.libs.json;

using namespace mcpp::build;

namespace {

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

// #390 self-heal: a CDB written BEFORE the mixed-separator fix spells the
// same file with the generic `/` spelling (`/p/generated/modules/a.cpp` —
// on Windows this is exactly `root\generated/modules\a.cpp` vs the native
// `root\generated\modules\a.cpp`). The merge key is the NORMALIZED path, so
// the stale entry is recognized as the same file and dropped on the first
// build after upgrade — the user never has to delete compile_commands.json
// by hand. (On POSIX the two spellings are byte-identical; the test then
// still verifies the plain fresh-wins contract.)
TEST(CompileCommandsMerge, NormalizedFileKeysHealStaleSeparatorSpellings) {
    auto p = std::filesystem::path("/p") / "generated" / "modules" / "a.cpp";
    auto fresh    = cdb({ entry(p.string(), "-O2-FRESH") });
    auto existing = cdb({ entry(p.generic_string(), "-O0-STALE") });

    auto merged = merge_compile_commands(
        fresh, existing, [](const std::filesystem::path&) { return true; });

    EXPECT_EQ(count(merged, "generated"), 1u) << merged;
    EXPECT_NE(merged.find("-O2-FRESH"), std::string::npos) << merged;
    EXPECT_EQ(merged.find("-O0-STALE"), std::string::npos) << merged;
}

// ── CDB arguments must be argv, not shell words ─────────────────────────────
//
// A consumer (clangd) execs `arguments` LITERALLY — no shell. So a token that
// still carries the quoting a shell would have removed is not a flag, it is a
// filename that does not exist.
//
// This is reachable on every platform, and was found on Windows first only
// because `shell_quote_arg`'s trigger set contains the backslash: every
// Windows path has one, so every path-bearing flag there gets quoted. On
// POSIX it takes a space in the path — which is why no e2e caught it: none of
// their project paths have one.
//
// Measured before the fix, from a real build under `/tmp/.../my project`:
//
//   '-fmodule-file=std=/tmp/.../my project/.../std.pcm     <- closing quote gone
//   '-fprebuilt-module-path=/tmp/.../my project/.../pcm.cache'
//
// The first is worse than a stray quote: the token was split at the space
// INSIDE the quotes, so one argument became two, one of them opening a quote
// that never closes.
namespace {

bool is_quoted(std::string_view s) {
    if (s.size() < 2) return false;
    return (s.front() == '"' && s.back() == '"')
        || (s.front() == '\'' && s.back() == '\'');
}
bool has_edge_quote(std::string_view s) {
    return !s.empty()
        && (s.front() == '"' || s.front() == '\''
         || s.back()  == '"' || s.back()  == '\'');
}

}  // namespace

TEST(CompileCommandsArgs, StripsPosixQuotingFromAToken) {
    auto out = mcpp::build::split_flags("-O2 '-fprebuilt-module-path=/a/b' -g");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1], "-fprebuilt-module-path=/a/b");
    for (auto const& t : out) EXPECT_FALSE(has_edge_quote(t)) << t;
}

TEST(CompileCommandsArgs, StripsWindowsQuotingFromAToken) {
    auto out = mcpp::build::split_flags(R"(-O2 "-IC:\Users\x\inc" -g)");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1], R"(-IC:\Users\x\inc)");
    for (auto const& t : out) EXPECT_FALSE(has_edge_quote(t)) << t;
}

// The one that bit: a quoted path containing a space is ONE argument. The
// space arrives ninja-escaped (`$ `) because flags.cppm escapes before it
// quotes, so the un-escape has to happen INSIDE the quotes — do it in the
// wrong order and the token splits exactly here.
TEST(CompileCommandsArgs, AQuotedPathWithASpaceStaysOneToken) {
    auto out = mcpp::build::split_flags(
        "-O2 '-fmodule-file=std=/tmp/my$ project/std.pcm' -g");
    ASSERT_EQ(out.size(), 3u) << "the quoted path was split";
    EXPECT_EQ(out[1], "-fmodule-file=std=/tmp/my project/std.pcm");
    for (auto const& t : out) EXPECT_FALSE(has_edge_quote(t)) << t;
}

// Unquoted input must keep behaving exactly as before: ninja escapes undone,
// split on spaces.
TEST(CompileCommandsArgs, UnquotedInputIsUnchanged) {
    auto out = mcpp::build::split_flags("-IC$:/x -DA=1 -Idir$ with$ space");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], "-IC:/x");
    EXPECT_EQ(out[1], "-DA=1");
    EXPECT_EQ(out[2], "-Idir with space");
}

// A quote in the MIDDLE of a token is data, not quoting: `-DA="x"` must keep
// its inner quotes or the define changes meaning.
TEST(CompileCommandsArgs, InnerQuotesAreNotStripped) {
    auto out = mcpp::build::split_flags(R"(-DGREETING="hi")");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], R"(-DGREETING="hi")");
    EXPECT_FALSE(is_quoted(out[0]));
}

// ── Emitted paths use native separators, unconditionally ────────────────────
//
// The last line of the #390 defence: every ingestion point is normalized at
// the source, but a path that still slips through with a mixed spelling
// (MSVC keeps the input `/` verbatim) would land in `file`/`-c`/`-o`/`-I`
// and break CLion. emit_compile_commands therefore makes ALL of them native
// — make_preferred(), a no-op on POSIX.
TEST(CompileCommandsEmit, EmittedPathsUseNativeSeparators) {
    BuildPlan plan;
    plan.projectRoot = "/p";
    plan.outputDir = "/p/target";
    plan.compileUnits.push_back({
        // A path whose spelling carries `/` segments (what the manifest
        // glob ingestion used to produce on MSVC).
        .source = std::filesystem::path("C:/Users/x/src/main.cpp"),
        .object = std::filesystem::path("obj") / "main.o",
        .packageName = "demo",
    });
    CompileFlags flags;
    flags.cxxBinary = std::filesystem::path("/usr/bin/g++");

    auto j = nlohmann::json::parse(emit_compile_commands(plan, flags));
    auto const& e = j[0];
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        EXPECT_EQ(e["file"].get<std::string>(), "C:\\Users\\x\\src\\main.cpp");
        EXPECT_EQ(e["directory"].get<std::string>(), "\\p");
        EXPECT_EQ(e["output"].get<std::string>(), "\\p\\target\\obj\\main.o");
        for (auto const& a : e["arguments"]) {
            if (a.is_string() && a.get<std::string>().starts_with("-"))
                EXPECT_EQ(a.get<std::string>().find('/'), std::string::npos);
        }
    } else {
        EXPECT_EQ(e["file"].get<std::string>(), "C:/Users/x/src/main.cpp");
    }
}
