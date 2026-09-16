#include <gtest/gtest.h>

import std;
import mcpp.source_kind;
import mcpp.build.compile_commands;
import mcpp.build.flags;
import mcpp.build.plan;
import mcpp.libs.json;
import mcpp.manifest.flag_words;
import mcpp.platform;

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
//
// Serialized THROUGH nlohmann, never hand-formatted: a Windows `file` value
// carries backslashes, and `\g` (from `...\generated\...`) is not a legal JSON
// escape. A format-string version produced text that no parser accepts, which
// merge_compile_commands answers by returning `fresh` verbatim — every
// assertion below then passes without the code under test ever running.
std::string entry(std::string_view file, std::string_view flag) {
    // Keep the file path out of `arguments` so it appears exactly once (in
    // "file") — lets tests count entries per file unambiguously.
    nlohmann::json e;
    e["directory"] = "/p";
    e["file"]      = std::string(file);
    e["arguments"] = nlohmann::json::array(
        { "g++", std::string(flag), "-c", "src.cpp" });
    e["output"]    = "o";
    return e.dump();
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
    // A fixture that does not PARSE makes every merge test vacuously green
    // (merge_compile_commands short-circuits to `fresh` on a discarded parse),
    // so the fixture itself is checked here rather than trusted. Tests that
    // WANT malformed input pass it directly, not through this builder.
    EXPECT_FALSE(nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false)
                     .is_discarded()) << s;
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

TEST(CompileCommandsWriter, PublishesFreshDatabaseWhenNoneExists) {
    TempDir temp;
    auto path = temp.path / "compile_commands.json";  // deliberately absent
    auto content = cdb({entry("/p/src/main.cpp", "-O2")});

    auto result = publish_compile_commands(
        path, content, [](const std::filesystem::path&) { return true; });

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->changed);
    // The file is re-serialised via nlohmann (sorted + 2-space indent), so
    // assert on content, not on byte-equality with the raw input.
    auto published = read_file(path);
    EXPECT_NE(published.find("/p/src/main.cpp"), std::string::npos) << published;
    EXPECT_NE(published.find("-O2"), std::string::npos) << published;
}

TEST(CompileCommandsWriter, UnchangedContentKeepsMtime) {
    TempDir temp;
    auto path = temp.path / "compile_commands.json";
    auto content = cdb({entry((temp.path / "a.cpp").string(), "-DOK")});
    std::ofstream(path, std::ios::binary) << content;
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

// #390 self-heal: a CDB written BEFORE the mixed-separator fix spells the same
// file differently from the way a fresh plan spells it. Because the merge key
// is the NORMALIZED path, the stale entry is recognized as the same file and
// dropped on the first build after upgrade — the user never has to delete
// compile_commands.json by hand.
//
// The stale spelling deliberately differs from fresh on EVERY platform: the
// `/./` segment needs lexically_normal to collapse (POSIX included), and on
// Windows the `/` separators need make_preferred on top. A stale value of just
// `p.generic_string()` would be byte-identical to fresh on POSIX, and the
// literal-string key this test exists to replace would pass it unchanged.
TEST(CompileCommandsMerge, NormalizedFileKeysHealStaleSeparatorSpellings) {
    auto p = std::filesystem::path("/p") / "generated" / "modules" / "a.cpp";
    auto stale = "/p/generated/./modules/a.cpp";
    ASSERT_NE(p.string(), stale) << "fixture must differ from the fresh spelling";

    auto fresh    = cdb({ entry(p.string(), "-O2-FRESH") });
    auto existing = cdb({ entry(stale, "-O0-STALE") });

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

// The host's own reader: single quotes are quoting for `sh` and literal
// characters for the MSVCRT rules, so this row holds on POSIX hosts only.
TEST(CompileCommandsArgs, StripsPosixQuotingFromAToken) {
    if constexpr (mcpp::platform::is_windows) {
        GTEST_SKIP() << "single quotes are literal under the MSVCRT rules";
    }
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
    // The quotes are the host's: flags.cppm quotes with `'` on POSIX and `"`
    // under the MSVCRT rules.
    const std::string_view line = mcpp::platform::is_windows
        ? "-O2 \"-fmodule-file=std=/tmp/my$ project/std.pcm\" -g"
        : "-O2 '-fmodule-file=std=/tmp/my$ project/std.pcm' -g";
    auto out = mcpp::build::split_flags(line);
    ASSERT_EQ(out.size(), 3u) << "the quoted path was split";
    EXPECT_EQ(out[1], "-fmodule-file=std=/tmp/my project/std.pcm");
    for (auto const& t : out) EXPECT_FALSE(has_edge_quote(t)) << t;
}

// Unquoted input: ninja's escapes are undone first, and then the host splits
// the line. `$ ` is a plain space by the time the host reads it, so an
// unquoted `-Idir$ with$ space` is three words to the compiler, and the
// database says so. The engine quotes every word it writes (include_token,
// ninja_command_word); the quoted spelling is the test above.
TEST(CompileCommandsArgs, UnquotedInputSplitsWhereTheHostSplits) {
    auto out = mcpp::build::split_flags("-IC$:/x -DA=1 -Idir$ with$ space");
    ASSERT_EQ(out.size(), 5u);
    EXPECT_EQ(out[0], "-IC:/x");
    EXPECT_EQ(out[1], "-DA=1");
    EXPECT_EQ(out[2], "-Idir");
    EXPECT_EQ(out[3], "with");
    EXPECT_EQ(out[4], "space");
}

// A quote inside a word is quoting on both hosts' readers: `sh` and the MSVCRT
// rules both hand the compiler `-DGREETING=hi` for this text. The version of
// this test before #655 asserted the opposite, which pinned the compile
// database to a reading no host applies.
TEST(CompileCommandsArgs, QuotesInsideAWordAreQuoting) {
    auto out = mcpp::build::split_flags(R"(-DGREETING="hi")");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "-DGREETING=hi");
}

// ── A unit's flag list: the edge and the databases agree (#655) ─────────────
//
// The edge writes each word of a unit's flag list quoted for the host
// (ninja_flag_list); the databases list the words. Reading the edge's text
// the way ninja and the host read it must give back the listed words. The
// test runs the reader of the host it runs on, so each CI host checks its own.

namespace {

const std::vector<std::string> kIssue655Elements{
    R"(-DPLATFORM_CONFIG_H=\"mcpp_libarchive_config.h\")",
    R"(-DMID="mid")",
    "-DSQ='sq'",
    "-include mcpp_lua_platform_config.h",
    R"(-I/opt/my\ dir/include)",
    R"(-IC:\Users\x\include)",
    "-DDOLLAR=a$b",
    // What fold_build_defines_into_flags inserts for `defines = ["DEF=\"def\""]`
    // and `defines = ["SPACE=a b"]`.
    mcpp::manifest::flag_element(R"(-DDEF="def")"),
    mcpp::manifest::flag_element("-DSPACE=a b"),
};

std::vector<std::string> random_words() {
    std::vector<std::string> words{"", "a b", "\\", "a\\", "C:\\dir\\", "\"", "'",
                                   "it's", "-DV=\"x\\\"y\"", "$x", "a$$b"};
    const std::string alphabet = "ab \t\"'\\$:;&|<>";
    std::uint32_t state = 655;
    for (int n = 0; n < 1000; ++n) {
        std::string w;
        const int length = static_cast<int>(state % 10);
        for (int i = 0; i < length; ++i) {
            state = state * 1664525u + 1013904223u;
            w.push_back(alphabet[(state >> 16) % alphabet.size()]);
        }
        state = state * 1664525u + 1013904223u;
        words.push_back(std::move(w));
    }
    return words;
}

}  // namespace

TEST(CompileCommandsArgs, TheEdgeAndTheDatabaseListTheSameWords) {
    const auto words = mcpp::manifest::flag_words(kIssue655Elements);
    EXPECT_EQ(mcpp::build::split_flags(mcpp::build::ninja_flag_list(kIssue655Elements)),
              words);
    const std::vector<std::string> want{
        R"(-DPLATFORM_CONFIG_H="mcpp_libarchive_config.h")", "-DMID=mid", "-DSQ=sq",
        "-include", "mcpp_lua_platform_config.h", "-I/opt/my dir/include",
        R"(-IC:\Users\x\include)", "-DDOLLAR=a$b", R"(-DDEF="def")", "-DSPACE=a b"};
    EXPECT_EQ(words, want);
}

TEST(CompileCommandsArgs, AWordOnAnEdgeReadsBackAsTheWord) {
    for (auto const& w : random_words()) {
        EXPECT_EQ(mcpp::build::split_flags(mcpp::build::ninja_command_word(w)),
                  std::vector<std::string>{w})
            << "word: [" << w << "]";
    }
}

// The reader above is mcpp's model of the host. On a POSIX host the host itself
// is available, so the model is checked against it: `/bin/sh` receives the
// quoted words and prints each one back.
TEST(CompileCommandsArgs, ShellReceivesTheQuotedWordsAsWritten) {
    if constexpr (mcpp::platform::is_windows) {
        GTEST_SKIP() << "the POSIX shell is the reader measured here";
    }
    TempDir temp;
    const auto out = temp.path / "words.txt";
    auto words = random_words();
    std::erase_if(words, [](const std::string& w) { return w.empty(); });
    words.resize(200);
    std::string command = "printf '%s\\n'";
    for (auto const& w : words) {
        command += ' ';
        command += mcpp::build::shell_quote_arg(w);
    }
    command += " > '" + out.string() + "'";
    ASSERT_EQ(std::system(command.c_str()), 0);
    std::ifstream in(out);
    std::vector<std::string> printed;
    for (std::string line; std::getline(in, line);) printed.push_back(line);
    EXPECT_EQ(printed, words);
}

TEST(CompileCommandsEmit, AUnitListsTheWordsOfItsFlagList) {
    BuildPlan plan;
    plan.projectRoot = "/p";
    plan.outputDir = "/p/target";
    plan.compileUnits.push_back({
        .source = std::filesystem::path("/p/src/archive.c"),
        .kind = mcpp::SourceKind::C,
        .object = std::filesystem::path("obj") / "archive.o",
        .packageName = "demo",
        .packageCflags = kIssue655Elements,
    });
    CompileFlags flags;
    flags.ccBinary = std::filesystem::path("/usr/bin/gcc");
    auto j = nlohmann::json::parse(emit_compile_commands(plan, flags));
    std::vector<std::string> args;
    for (auto const& a : j[0]["arguments"]) args.push_back(a.get<std::string>());
    const auto words = mcpp::manifest::flag_words(kIssue655Elements);
    auto at = std::search(args.begin(), args.end(), words.begin(), words.end());
    EXPECT_NE(at, args.end()) << "the unit's words are not listed in order";
}

// ── Emitted paths use native separators ─────────────────────────────────────
//
// The last line of the #390 defence: every ingestion point is normalized at
// the source, but a path that still slips through with a mixed spelling
// (MSVC keeps the input `/` verbatim) would land in the CDB and break CLion.
//
// Scope, precisely: the emitter owns the CDB schema's path fields (`file`,
// `directory`, `output`) and the argv positions it builds itself (`-c`'s and
// `-o`'s values, plus `-I`/`-idirafter` from localIncludeDirs). The flag
// STRINGS — split_flags(f.cxx) and the package cflags/cxxflags — pass through
// untouched and stay the ingestion points' responsibility; normalizing an
// arbitrary flag payload is not safe (`-DPATH="/etc/x"` holds real slashes).
TEST(CompileCommandsEmit, EmittedPathsUseNativeSeparators) {
    BuildPlan plan;
    plan.projectRoot = "/p";
    plan.outputDir = "/p/target";
    plan.compileUnits.push_back({
        // Paths whose spelling carries `/` segments — what the manifest glob
        // and the include_dirs channels used to produce on MSVC.
        .source = std::filesystem::path("C:/Users/x/src/main.cpp"),
        .object = std::filesystem::path("obj") / "main.o",
        .packageName = "demo",
        .localIncludeDirs      = { std::filesystem::path("C:/proj/generated/inc") },
        .localIncludeDirsAfter = { std::filesystem::path("C:/proj/third_party/inc") },
    });
    CompileFlags flags;
    flags.cxxBinary = std::filesystem::path("/usr/bin/g++");

    auto j = nlohmann::json::parse(emit_compile_commands(plan, flags));
    auto const& e = j[0];

    // Assert on the two include tokens by name. The earlier shape of this
    // test ("no `-`-prefixed argument may contain `/`") looked stronger but
    // was vacuous — with no flags configured, the only `-` tokens are the
    // bare `-c` and `-o`, which have no path in them at all.
    auto arg_with = [&](std::string_view pre) {
        for (auto const& a : e["arguments"]) {
            auto s = a.get<std::string>();
            if (s.starts_with(pre)) return s;
        }
        return std::string{};
    };
    const auto inc      = arg_with("-I");
    const auto incAfter = arg_with("-idirafter");

    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        EXPECT_EQ(e["file"].get<std::string>(), "C:\\Users\\x\\src\\main.cpp");
        EXPECT_EQ(e["directory"].get<std::string>(), "\\p");
        EXPECT_EQ(e["output"].get<std::string>(), "\\p\\target\\obj\\main.o");
        EXPECT_EQ(inc,      "-IC:\\proj\\generated\\inc");
        EXPECT_EQ(incAfter, "-idirafterC:\\proj\\third_party\\inc");
    } else {
        EXPECT_EQ(e["file"].get<std::string>(), "C:/Users/x/src/main.cpp");
        EXPECT_EQ(inc,      "-IC:/proj/generated/inc");
        EXPECT_EQ(incAfter, "-idirafterC:/proj/third_party/inc");
    }
}

TEST(CompileCommandsWriter, ReplacesSymlinkTargetWithoutRemovingLink) {
    TempDir temp;
    auto target = temp.path / "build" / "compile_commands.json";
    auto link = temp.path / "compile_commands.json";
    std::filesystem::create_directories(target.parent_path());
    auto oldContent = cdb({entry((temp.path / "old.cpp").string(), "-DOLD")});
    auto newContent = cdb({entry((temp.path / "new.cpp").string(), "-DNEW")});
    std::ofstream(target) << oldContent;
    std::error_code symlinkEc;
    std::filesystem::create_symlink(target, link, symlinkEc);
    if (symlinkEc) GTEST_SKIP() << "symlink unavailable: " << symlinkEc.message();

    auto result = publish_compile_commands(
        link, newContent, [](const std::filesystem::path&) { return false; });

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(std::filesystem::is_symlink(link));
    auto published = read_file(target);
    EXPECT_NE(published.find("-DNEW"), std::string::npos);
    EXPECT_EQ(published.find("-DOLD"), std::string::npos);
}

TEST(CompileCommandsWriter, ExistingUnreadableDatabaseIsNotOverwritten) {
    TempDir temp;
    auto path = temp.path / "compile_commands.json";
    std::ofstream(path) << "last-known-good";
    std::error_code permissionEc;
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read,
        std::filesystem::perm_options::remove, permissionEc);
    if (permissionEc) GTEST_SKIP() << "cannot change permissions: " << permissionEc.message();
    std::ifstream probe(path);
    if (probe) {
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, permissionEc);
        GTEST_SKIP() << "test user can still read restricted file";
    }

    auto result = publish_compile_commands(
        path, cdb({entry((temp.path / "new.cpp").string(), "-DNEW")}),
        [](const std::filesystem::path&) { return true; });

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("read existing"), std::string::npos);
    std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, permissionEc);
}

// ── One record, two databases ───────────────────────────────────────────────
//
// The compile database and the build database render the same per-unit record,
// so `arguments` cannot differ between them. The second half shows the
// comparison can fail: the record under other flags is not the CDB's.
TEST(CompileCommandsEmit, UnitInvocationsAreTheCompileDatabaseArguments) {
    BuildPlan plan;
    plan.projectRoot = "/p";
    plan.outputDir = "/p/target";
    plan.compileUnits.push_back({
        .source = std::filesystem::path("/p/src/main.cpp"),
        .kind = mcpp::SourceKind::Cxx,
        .object = std::filesystem::path("obj") / "main.o",
        .packageName = "demo",
        .localIncludeDirs = { std::filesystem::path("/p/include") },
        .packageCxxflags = { "-DDEMO=1" },
    });
    plan.compileUnits.push_back({
        .source = std::filesystem::path("/p/src/c.c"),
        .kind = mcpp::SourceKind::C,
        .object = std::filesystem::path("obj") / "c.o",
        .packageName = "demo",
    });
    CompileFlags flags;
    flags.cxxBinary = "/usr/bin/g++";
    flags.ccBinary  = "/usr/bin/gcc";
    flags.cxx = "-std=c++23 '-DQUOTED=a b'";
    flags.cc  = "-std=c11";

    auto j = nlohmann::json::parse(emit_compile_commands(plan, flags));
    auto invs = unit_invocations(plan, flags);
    ASSERT_EQ(j.size(), invs.size());
    for (std::size_t i = 0; i < invs.size(); ++i) {
        EXPECT_EQ(j[i]["arguments"].get<std::vector<std::string>>(), invs[i].arguments);
        EXPECT_EQ(j[i]["file"].get<std::string>(), invs[i].file);
        EXPECT_EQ(j[i]["directory"].get<std::string>(), invs[i].directory);
        EXPECT_EQ(j[i]["output"].get<std::string>(), invs[i].output);
        EXPECT_EQ(invs[i].unit, &plan.compileUnits[i]);
    }

    CompileFlags other = flags;
    other.cxx = "-std=c++26";
    EXPECT_NE(j[0]["arguments"].get<std::vector<std::string>>(),
              unit_invocations(plan, other)[0].arguments);
}
